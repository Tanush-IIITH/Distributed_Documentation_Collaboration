#include "name_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <strings.h>

#define NS_DB_PATH "ns_metadata.db"

static void ns_save_metadata(NameServer *ns);
static void ns_load_metadata(NameServer *ns);

// Initialize the Name Server
int ns_init(NameServer *ns, int port) {
    if (!ns) {
        return -1;
    }
    
    memset(ns, 0, sizeof(NameServer));
    ns->port = port;
    ns->sockfd = -1;
    ns->ss_count = 0;
    ns->client_count = 0;
    ns->file_count = 0;
    ns->request_count = 0;
    ns->next_request_id = 1;
    memset(ns->file_index, 0, sizeof(ns->file_index));
    memset(ns->file_cache, 0, sizeof(ns->file_cache));
    ns->cache_tick = 0;
    ns->ss_round_robin_index = 0;
    if (pthread_mutex_init(&ns->state_lock, NULL) != 0) {
        log_message(LOG_ERROR, "NS", "Failed to initialize mutex: %s", strerror(errno));
        return -1;
    }
    
    ns_load_metadata(ns);
    log_message(LOG_INFO, "NS", "Name Server initialized on port %d", port);
    return 0;
}

// Connection context structure - to hold per-connection state
typedef struct {
    NameServer *ns;
    int conn_fd; //connection file descriptor
    char peer_ip[INET_ADDRSTRLEN]; //peer ip address
    int peer_port; //peer port number
    int is_client;
    int is_storage_server;
    char username[MAX_USERNAME_LENGTH];
    StorageServerInfo *ss_info;
} ConnectionContext;

//close connection and free context
static void close_connection(ConnectionContext *ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->conn_fd >= 0) {
        close(ctx->conn_fd);
    }
    free(ctx);
}

//send error message and log it
static void send_error_and_log(int fd, int code, const char *message, const char *peer_ip, int peer_port) {
    char *err_resp = protocol_build_error(code, message);
    if (err_resp) {
        protocol_send_message(fd, err_resp);
        free(err_resp);
    }
    log_message(LOG_WARNING, "NS", "Sent error %d to %s:%d -> %s", code, peer_ip, peer_port, message);
}

static void send_simple_ok(int fd, const char *detail) {
    const char *text = (detail && detail[0]) ? detail : "OK";
    char *resp = protocol_build_ok(text);
    if (!resp) {
        return;
    }
    protocol_send_message(fd, resp);
    free(resp);
}

static int send_info_line(int fd, const char *line_text) {
    const char *payload = (line_text && line_text[0]) ? line_text : "N/A";
    const char *fields[] = {RESP_INFO_LINE, payload};
    char *resp = protocol_build_message(fields, 2);
    if (!resp) {
        return -1;
    }
    int rc = protocol_send_message(fd, resp);
    free(resp);
    return rc < 0 ? -1 : 0;
}

static void acl_append_token(char *buffer, size_t buffer_size, const char *token, int *is_first) {
    if (!buffer || buffer_size == 0 || !token || !token[0] || !is_first) {
        return;
    }

    if (*is_first) {
        buffer[0] = '\0';
        safe_strcpy(buffer, token, buffer_size);
        *is_first = 0;
    } else {
        safe_strcat(buffer, ", ", buffer_size);
        safe_strcat(buffer, token, buffer_size);
    }
}

// initialize per-storage-server communication primitives
static void storage_server_comm_init(StorageServerInfo *ss) {
    if (!ss) {
        return;
    }

    if (pthread_mutex_init(&ss->comm_lock, NULL) != 0) {
        log_message(LOG_ERROR, "NS", "Failed to init SS comm mutex: %s", strerror(errno));
    }

    if (pthread_cond_init(&ss->response_cond, NULL) != 0) {
        log_message(LOG_ERROR, "NS", "Failed to init SS response condition: %s", strerror(errno));
    }

    ss->awaiting_response = 0;
    ss->response_ready = 0;
    ss->response_status = 0;
    ss->response_raw = NULL;
    ss->refcount = 0;
}

// clear and destroy per-storage-server communication primitives
static void storage_server_comm_destroy(StorageServerInfo *ss) {
    if (!ss) {
        return;
    }

    pthread_mutex_lock(&ss->comm_lock);
    if (ss->response_raw) {
        free(ss->response_raw);
        ss->response_raw = NULL;
    }
    ss->awaiting_response = 0;
    ss->response_ready = 0;
    ss->response_status = 0;
    pthread_mutex_unlock(&ss->comm_lock);

    pthread_mutex_destroy(&ss->comm_lock);
    pthread_cond_destroy(&ss->response_cond);
}

// increment the reference count while the NS thread is working with the SS entry
static void storage_server_acquire(StorageServerInfo *ss) {
    if (!ss) {
        return;
    }

    pthread_mutex_lock(&ss->comm_lock);
    ss->refcount++;
    pthread_mutex_unlock(&ss->comm_lock);
}

// release a previously acquired reference to the SS entry
static void storage_server_release(StorageServerInfo *ss) {
    if (!ss) {
        return;
    }

    pthread_mutex_lock(&ss->comm_lock);
    if (ss->refcount > 0) {
        ss->refcount--;
        if (ss->refcount == 0) {
            pthread_cond_broadcast(&ss->response_cond);
        }
    }
    pthread_mutex_unlock(&ss->comm_lock);
}

// signal any waiter that the storage server connection has failed
static void storage_server_signal_failure(StorageServerInfo *ss) {
    if (!ss) {
        return;
    }

    pthread_mutex_lock(&ss->comm_lock);
    ss->is_alive = 0;
    ss->sockfd = -1;
    if (ss->awaiting_response && !ss->response_ready) {
        if (ss->response_raw) {
            free(ss->response_raw);
            ss->response_raw = NULL;
        }
        ss->response_status = -1;
        ss->response_ready = 1;
        ss->awaiting_response = 0;
        pthread_cond_broadcast(&ss->response_cond);
    }
    pthread_mutex_unlock(&ss->comm_lock);
}

// send a request to the storage server and block until the paired response arrives
static int storage_server_send_and_wait(StorageServerInfo *ss, const char **fields, int field_count, char **out_raw) {
    if (!ss || !fields || field_count <= 0 || !out_raw) {
        return -1;
    }

    if (ss->sockfd < 0 || !ss->is_alive) {
        return -2;
    }

    pthread_mutex_lock(&ss->comm_lock);

    while (ss->awaiting_response) {
        pthread_cond_wait(&ss->response_cond, &ss->comm_lock);
    }

    if (ss->response_raw) {
        free(ss->response_raw);
        ss->response_raw = NULL;
    }
    ss->response_ready = 0;
    ss->response_status = 0;

    char *msg = protocol_build_message(fields, field_count);
    if (!msg) {
        pthread_mutex_unlock(&ss->comm_lock);
        return -3;
    }

    if (protocol_send_message(ss->sockfd, msg) < 0) {
        free(msg);
        pthread_mutex_unlock(&ss->comm_lock);
        return -4;
    }
    free(msg);

    ss->awaiting_response = 1;
    ss->response_ready = 0;
    ss->response_status = 0;

    while (!ss->response_ready) {
        pthread_cond_wait(&ss->response_cond, &ss->comm_lock);
    }

    if (ss->response_status != 0) {
        if (ss->response_raw) {
            free(ss->response_raw);
            ss->response_raw = NULL;
        }
        ss->awaiting_response = 0;
        ss->response_ready = 0;
        pthread_cond_broadcast(&ss->response_cond);
        pthread_mutex_unlock(&ss->comm_lock);
        return -5;
    }

    char *raw = ss->response_raw;
    ss->response_raw = NULL;
    ss->awaiting_response = 0;
    ss->response_ready = 0;
    pthread_cond_broadcast(&ss->response_cond);
    pthread_mutex_unlock(&ss->comm_lock);

    if (!raw) {
        return -6;
    }

    *out_raw = raw;
    return 0;
}

/*
    Explanation of Hash implementation
    The hash function takes a string (filename) as input and produces a fixed-size integer (hash value) as output. 
    This hash value is used to determine the index in the hash table (file index) where the corresponding file metadata will be stored. 
    The goal of the hash function is to distribute the file entries uniformly across the hash table to minimize collisions and ensure efficient lookups.
    In case of collisions (two filenames producing the same hash value), a linked list is used to store multiple entries in the same bucket.
*/

//hash function for filenames (djb2 variant keeps distribution stable across runs)
static unsigned long hash_filename(const char *str) {
    unsigned long hash = 5381;
    int c;
    while (str && (c = *str++)) {
        hash = ((hash << 5) + hash) + (unsigned long)c;
    }
    return hash;
}

//find file metadata index from hash buckets
static FileIndexNode *file_index_find(NameServer *ns, const char *filename) {
    if (!ns || !filename) {
        return NULL;
    }

    unsigned long bucket = hash_filename(filename) % FILE_INDEX_SIZE;
    FileIndexNode *node = ns->file_index[bucket];
    while (node) {
        if (strcmp(node->filename, filename) == 0) {
            return node;
        }
        node = node->next;
    }
    return NULL;
}

//insert or update hash bucket entry for a filename
static int file_index_insert(NameServer *ns, const char *filename, int array_index) {
    if (!ns || !filename || array_index < 0) {
        return -1;
    }

    unsigned long bucket = hash_filename(filename) % FILE_INDEX_SIZE;
    FileIndexNode *node = ns->file_index[bucket];
    while (node) {
        if (strcmp(node->filename, filename) == 0) {
            node->file_array_index = array_index;
            return 0;
        }
        node = node->next;
    }

    FileIndexNode *new_node = (FileIndexNode *)malloc(sizeof(FileIndexNode));
    if (!new_node) {
        return -1;
    }

    memset(new_node, 0, sizeof(FileIndexNode));
    if (safe_strcpy(new_node->filename, filename, sizeof(new_node->filename)) != 0) {
        free(new_node);
        return -1;
    }
    new_node->file_array_index = array_index;
    new_node->next = ns->file_index[bucket];
    ns->file_index[bucket] = new_node;

    return 0;
}

//find storage server by socket fd
static StorageServerInfo *find_storage_server_by_sockfd(NameServer *ns, int sockfd) {
    if (!ns) {
        return NULL;
    }
    for (int i = 0; i < ns->ss_count; i++) {
        StorageServerInfo *info = ns->storage_servers[i];
        if (!info) {
            continue;
        }
        if (info->sockfd == sockfd) {
            return info;
        }
    }
    return NULL;
}

//LRU file cache lookup
static int file_cache_lookup(NameServer *ns, const char *filename, FileMetadata *out_metadata, int *out_index) {
    if (!ns || !filename) {
        return 0;
    }

    for (int i = 0; i < FILE_CACHE_SIZE; i++) {
        FileCacheEntry *entry = &ns->file_cache[i];
        if (!entry->valid) {
            continue;
        }

        if (strcmp(entry->filename, filename) == 0) {
            if (entry->file_array_index < 0 || entry->file_array_index >= ns->file_count) {
                entry->valid = 0;
                continue;
            }

            if (out_metadata) {
                *out_metadata = ns->files[entry->file_array_index];
            }
            if (out_index) {
                *out_index = entry->file_array_index;
            }
            entry->last_used = ++ns->cache_tick;
            return 1;
        }
    }

    return 0;
}

//LRU file cache store to remember recent filename -> metadata mappings
static void file_cache_store(NameServer *ns, const char *filename, int array_index) {
    if (!ns || !filename || array_index < 0 || array_index >= ns->file_count) {
        return;
    }

    FileCacheEntry *target = NULL;

    for (int i = 0; i < FILE_CACHE_SIZE; i++) {
        FileCacheEntry *entry = &ns->file_cache[i];
        if (entry->valid && strcmp(entry->filename, filename) == 0) {
            target = entry;
            break;
        }
    }

    if (!target) {
        for (int i = 0; i < FILE_CACHE_SIZE; i++) {
            FileCacheEntry *entry = &ns->file_cache[i];
            if (!entry->valid) {
                target = entry;
                break;
            }
        }
    }

    if (!target) {
        unsigned long oldest = ULONG_MAX;
        FileCacheEntry *oldest_entry = NULL;
        for (int i = 0; i < FILE_CACHE_SIZE; i++) {
            FileCacheEntry *entry = &ns->file_cache[i];
            if (!entry->valid) {
                oldest_entry = entry;
                break;
            }
            if (entry->last_used < oldest) {
                oldest = entry->last_used;
                oldest_entry = entry;
            }
        }
        target = oldest_entry;
    }

    if (!target) {
        return;
    }

    if (safe_strcpy(target->filename, filename, sizeof(target->filename)) != 0) {
        target->valid = 0;
        return;
    }

    target->file_array_index = array_index;
    target->last_used = ++ns->cache_tick;
    target->valid = 1;
}

// Resolve metadata for a filename using the cache first, falling back to the index.
// Caller must hold ns->state_lock because we access shared arrays directly.
static FileMetadata *file_lookup_locked(NameServer *ns, const char *filename, int *out_index) {
    if (!ns || !filename) {
        return NULL;
    }

    FileMetadata cached_copy;
    int cache_index = -1;
    if (file_cache_lookup(ns, filename, &cached_copy, &cache_index)) {
        if (cache_index >= 0 && cache_index < ns->file_count) {
            if (out_index) {
                *out_index = cache_index;
            }
            return &ns->files[cache_index];
        }
    }

    FileIndexNode *node = file_index_find(ns, filename);
    if (!node || node->file_array_index < 0 || node->file_array_index >= ns->file_count) {
        return NULL;
    }

    if (out_index) {
        *out_index = node->file_array_index;
    }
    file_cache_store(ns, filename, node->file_array_index);
    return &ns->files[node->file_array_index];
}

static void ns_clear_file_index(NameServer *ns) {
    if (!ns) {
        return;
    }

    for (int i = 0; i < FILE_INDEX_SIZE; i++) {
        FileIndexNode *node = ns->file_index[i];
        while (node) {
            FileIndexNode *next = node->next;
            free(node);
            node = next;
        }
        ns->file_index[i] = NULL;
    }
}

static int remove_file_metadata_locked(NameServer *ns, const char *filename) {
    if (!ns || !filename) {
        return 0;
    }

    FileIndexNode *node = file_index_find(ns, filename);
    if (!node) {
        return 0;
    }

    int removed_index = (node->file_array_index >= 0 && node->file_array_index < ns->file_count)
                            ? node->file_array_index
                            : -1;
    int original_count = ns->file_count;
    int last_index = original_count - 1;
    int moved = 0;
    FileMetadata moved_meta;
    memset(&moved_meta, 0, sizeof(moved_meta));

    unsigned long bucket = hash_filename(filename) % FILE_INDEX_SIZE;
    FileIndexNode *prev = NULL;
    FileIndexNode *iter = ns->file_index[bucket];
    while (iter) {
        if (strcmp(iter->filename, filename) == 0) {
            if (prev) {
                prev->next = iter->next;
            } else {
                ns->file_index[bucket] = iter->next;
            }
            free(iter);
            break;
        }
        prev = iter;
        iter = iter->next;
    }

    if (original_count > 0) {
        if (removed_index < 0 || removed_index >= original_count) {
            removed_index = last_index;
        }

        if (removed_index != last_index) {
            moved_meta = ns->files[last_index];
            ns->files[removed_index] = moved_meta;
            moved = (moved_meta.filename[0] != '\0');
        }

        memset(&ns->files[last_index], 0, sizeof(FileMetadata));
        ns->file_count = original_count - 1;

        if (moved) {
            FileIndexNode *moved_node = file_index_find(ns, moved_meta.filename);
            if (moved_node) {
                moved_node->file_array_index = removed_index;
            }
        }
    }

    for (int i = 0; i < FILE_CACHE_SIZE; i++) {
        FileCacheEntry *entry = &ns->file_cache[i];
        if (!entry->valid) {
            continue;
        }
        if (strcmp(entry->filename, filename) == 0) {
            entry->valid = 0;
            continue;
        }
        if (moved && strcmp(entry->filename, moved_meta.filename) == 0) {
            entry->file_array_index = removed_index;
        }
        if (entry->valid && entry->file_array_index >= ns->file_count) {
            entry->valid = 0;
        }
    }

    return 1;
}

static int file_acl_contains(const char entries[][MAX_USERNAME_LENGTH], int count, const char *username) {
    if (!username) {
        return 0;
    }

    for (int i = 0; i < count; i++) {
        if (strncmp(entries[i], username, MAX_USERNAME_LENGTH) == 0) {
            return 1;
        }
    }

    return 0;
}

static int file_acl_add(char entries[][MAX_USERNAME_LENGTH], int *count, const char *username) {
    if (!entries || !count || !username) {
        return -1;
    }

    if (file_acl_contains(entries, *count, username)) {
        return 0;
    }

    if (*count >= NS_MAX_CLIENTS) {
        return -1;
    }

    if (safe_strcpy(entries[*count], username, MAX_USERNAME_LENGTH) != 0) {
        return -1;
    }

    (*count)++;
    return 0;
}

static int file_acl_remove(char entries[][MAX_USERNAME_LENGTH], int *count, const char *username) {
    if (!entries || !count || !username) {
        return 0;
    }

    for (int i = 0; i < *count; i++) {
        if (strncmp(entries[i], username, MAX_USERNAME_LENGTH) == 0) {
            int last_index = *count - 1;
            if (i != last_index) {
                memcpy(entries[i], entries[last_index], MAX_USERNAME_LENGTH);
            }
            entries[last_index][0] = '\0';
            (*count)--;
            return 1;
        }
    }

    return 0;
}

static void file_acl_parse_csv(char entries[][MAX_USERNAME_LENGTH], int *count, const char *csv) {
    if (!entries || !count) {
        return;
    }

    *count = 0;
    if (!csv || csv[0] == '\0') {
        return;
    }

    char buffer[MAX_FIELD_SIZE];
    if (safe_strcpy(buffer, csv, sizeof(buffer)) != 0) {
        buffer[sizeof(buffer) - 1] = '\0';
    }

    char *token = strtok(buffer, ",");
    while (token) {
        trim_string(token);
        if (token[0] != '\0' && validate_username(token)) {
            if (file_acl_add(entries, count, token) != 0) {
                break;
            }
        }
        token = strtok(NULL, ",");
    }
}

static void ns_save_metadata(NameServer *ns) {
    if (!ns) {
        return;
    }

    pthread_mutex_lock(&ns->state_lock);
    FILE *fp = fopen(NS_DB_PATH, "w");
    if (!fp) {
        pthread_mutex_unlock(&ns->state_lock);
        return;
    }

    fprintf(fp, "%d\n", ns->file_count);
    for (int i = 0; i < ns->file_count; i++) {
        FileMetadata *f = &ns->files[i];
        const char *last_access_user = f->last_access_user[0] ? f->last_access_user : "N/A";
        const char *last_modified_user = f->last_modified_user[0] ? f->last_modified_user : "N/A";
        fprintf(fp, "FILE|%s|%s|%s|%d|%lld|%lld|%lld|%lld|%lld|%lld|%s|%s\n",
                f->filename,
                f->owner,
                f->ss_ip,
                f->ss_port,
                f->size,
                f->word_count,
                f->char_count,
                (long long)f->created,
                (long long)f->modified,
                (long long)f->last_access,
                last_access_user,
                last_modified_user);

        fprintf(fp, "READ|");
        for (int j = 0; j < f->read_access_count; j++) {
            fprintf(fp, "%s%s", f->read_access_users[j], (j + 1 < f->read_access_count) ? "," : "");
        }
        fprintf(fp, "\n");

        fprintf(fp, "WRITE|");
        for (int j = 0; j < f->write_access_count; j++) {
            fprintf(fp, "%s%s", f->write_access_users[j], (j + 1 < f->write_access_count) ? "," : "");
        }
        fprintf(fp, "\n");
    }

    fclose(fp);
    pthread_mutex_unlock(&ns->state_lock);
}

static void ns_load_metadata(NameServer *ns) {
    if (!ns) {
        return;
    }

    pthread_mutex_lock(&ns->state_lock);
    FILE *fp = fopen(NS_DB_PATH, "r");
    if (!fp) {
        pthread_mutex_unlock(&ns->state_lock);
        return;
    }

    ns_clear_file_index(ns);
    memset(ns->files, 0, sizeof(ns->files));
    memset(ns->file_cache, 0, sizeof(ns->file_cache));
    ns->cache_tick = 0;
    ns->file_count = 0;

    char line[MAX_FIELD_SIZE * 2];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        pthread_mutex_unlock(&ns->state_lock);
        return;
    }

    FileMetadata *current = NULL;
    while (fgets(line, sizeof(line), fp) && ns->file_count < NS_MAX_FILES) {
        trim_string(line);
        if (line[0] == '\0') {
            continue;
        }

        if (strncmp(line, "FILE|", 5) == 0) {
            char *payload = line + 5;
            char *saveptr = NULL;
            char *parts[12] = {0};
            int token_count = 0;
            for (int idx = 0; idx < 12; idx++) {
                parts[idx] = (idx == 0) ? strtok_r(payload, "|", &saveptr)
                                        : strtok_r(NULL, "|", &saveptr);
                if (!parts[idx]) {
                    break;
                }
                token_count++;
            }

            if (!parts[0] || !validate_filename(parts[0])) {
                current = NULL;
                continue;
            }

            FileMetadata *file = &ns->files[ns->file_count];
            memset(file, 0, sizeof(FileMetadata));
            safe_strcpy(file->filename, parts[0], sizeof(file->filename));
            if (parts[1]) {
                safe_strcpy(file->owner, parts[1], sizeof(file->owner));
            }
            if (parts[2]) {
                safe_strcpy(file->ss_ip, parts[2], sizeof(file->ss_ip));
            }
            if (parts[3]) {
                file->ss_port = atoi(parts[3]);
            }
            file->word_count = 0;
            file->char_count = 0;
            if (parts[4]) {
                file->size = atoll(parts[4]);
            }

            if (token_count >= 10) {
                if (parts[5]) {
                    file->word_count = atoll(parts[5]);
                }
                if (parts[6]) {
                    file->char_count = atoll(parts[6]);
                }
                if (parts[7]) {
                    file->created = (time_t)atoll(parts[7]);
                }
                if (parts[8]) {
                    file->modified = (time_t)atoll(parts[8]);
                }
                if (parts[9]) {
                    file->last_access = (time_t)atoll(parts[9]);
                }
            } else {
                // Legacy metadata (without counts) -> set counters to zero
                if (parts[5]) {
                    file->created = (time_t)atoll(parts[5]);
                }
                if (parts[6]) {
                    file->modified = (time_t)atoll(parts[6]);
                }
                if (parts[7]) {
                    file->last_access = (time_t)atoll(parts[7]);
                }
            }

            // Default to owner for older metadata entries
            if (file->owner[0]) {
                safe_strcpy(file->last_access_user, file->owner, sizeof(file->last_access_user));
                safe_strcpy(file->last_modified_user, file->owner, sizeof(file->last_modified_user));
            }

            if (parts[10] && parts[10][0]) {
                safe_strcpy(file->last_access_user, parts[10], sizeof(file->last_access_user));
            }

            if (parts[11] && parts[11][0]) {
                safe_strcpy(file->last_modified_user, parts[11], sizeof(file->last_modified_user));
            }

            file_index_insert(ns, file->filename, ns->file_count);
            current = file;
            ns->file_count++;
        } else if (current && strncmp(line, "READ|", 5) == 0) {
            file_acl_parse_csv(current->read_access_users, &current->read_access_count, line + 5);
        } else if (current && strncmp(line, "WRITE|", 6) == 0) {
            file_acl_parse_csv(current->write_access_users, &current->write_access_count, line + 6);
        }
    }

    fclose(fp);
    pthread_mutex_unlock(&ns->state_lock);
    log_message(LOG_INFO, "NS", "Loaded %d files from metadata store", ns->file_count);
}

static int file_add_access(FileMetadata *file, const char *username, int grant_read, int grant_write) {
    if (!file || !username) {
        return -1;
    }

    if (grant_read) {
        if (file_acl_add(file->read_access_users, &file->read_access_count, username) != 0) {
            return -1;
        }
    }

    if (grant_write) {
        if (file_acl_add(file->write_access_users, &file->write_access_count, username) != 0) {
            return -1;
        }
    }

    return 0;
}

static int file_remove_access(FileMetadata *file, const char *username) {
    if (!file || !username) {
        return 0;
    }

    int removed = 0;
    removed |= file_acl_remove(file->read_access_users, &file->read_access_count, username);
    removed |= file_acl_remove(file->write_access_users, &file->write_access_count, username);
    return removed;
}

static int file_has_read_access(const FileMetadata *file, const char *username) {
    if (!file || !username) {
        return 0;
    }

    if (strncmp(file->owner, username, MAX_USERNAME_LENGTH) == 0) {
        return 1;
    }

    if (file_acl_contains(file->read_access_users, file->read_access_count, username)) {
        return 1;
    }

    if (file_acl_contains(file->write_access_users, file->write_access_count, username)) {
        return 1;
    }

    return 0;
}

static int file_has_write_access(const FileMetadata *file, const char *username) {
    if (!file || !username) {
        return 0;
    }

    if (strncmp(file->owner, username, MAX_USERNAME_LENGTH) == 0) {
        return 1;
    }

    if (file_acl_contains(file->write_access_users, file->write_access_count, username)) {
        return 1;
    }

    return 0;
}

static int parse_permission_flags(const char *perm, int *grant_read, int *grant_write) {
    if (!perm || !grant_read || !grant_write) {
        return -1;
    }

    *grant_read = 0;
    *grant_write = 0;

    if (strcasecmp(perm, "R") == 0 || strcasecmp(perm, "READ") == 0) {
        *grant_read = 1;
    } else if (strcasecmp(perm, "W") == 0 || strcasecmp(perm, "WRITE") == 0) {
        *grant_write = 1;
    } else if (strcasecmp(perm, "RW") == 0 || strcasecmp(perm, "WR") == 0 ||
               strcasecmp(perm, "READWRITE") == 0 || strcasecmp(perm, "WRITEREAD") == 0) {
        *grant_read = 1;
        *grant_write = 1;
    } else {
        return -1;
    }

    return 0;
}

//client command processing loop
static void run_client_loop(ConnectionContext *ctx) {
    if (!ctx || !ctx->ns) {
        return;
    }

    NameServer *ns = ctx->ns;

    while (1) {
        char *raw_command = protocol_receive_message(ctx->conn_fd);
        if (!raw_command) {
            log_message(LOG_INFO, "NS", "Client %s:%d disconnected.", ctx->peer_ip, ctx->peer_port);
            break;
        }

        ProtocolMessage cmd_msg;
        if (protocol_parse_message(raw_command, &cmd_msg) != 0 || cmd_msg.field_count == 0) {
            send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid command format.", ctx->peer_ip, ctx->peer_port);
            free(raw_command);
            continue;
        }

        const char *command = cmd_msg.fields[0];
        log_request("NS", ctx->peer_ip, ctx->peer_port, "0.0.0.0", ns->port,
                    ctx->username[0] ? ctx->username : NULL, command);

        if (strcmp(command, MSG_LIST_USERS) == 0) {
            // Enumerate every connected user and stream back their usernames to the requester
            pthread_mutex_lock(&ns->state_lock);
            for (int i = 0; i < ns->client_count; i++) {
                char display_name[MAX_USERNAME_LENGTH + 32];
                const char *status = ns->clients[i].is_connected ? "Online" : "Offline";
                snprintf(display_name, sizeof(display_name), "%s [%s]", ns->clients[i].username, status);

                const char *fields[] = {RESP_OK_LIST, display_name};
                char *resp = protocol_build_message(fields, 2);
                if (!resp) {
                    log_message(LOG_WARNING, "NS", "Failed to build LIST_USERS entry for %s.", ns->clients[i].username);
                    continue;
                }

                protocol_send_message(ctx->conn_fd, resp);
                free(resp);
            }
            pthread_mutex_unlock(&ns->state_lock);

            // Signal to the client that the stream has finished
            const char *end_fields[] = {RESP_OK_LIST_END};
            char *end_msg = protocol_build_message(end_fields, 1);
            if (end_msg) {
                protocol_send_message(ctx->conn_fd, end_msg);
                free(end_msg);
            } else {
                send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to build LIST_USERS terminator.", ctx->peer_ip, ctx->peer_port);
            }
        } else if (strcmp(command, MSG_VIEW) == 0) {
            const char *flags = (cmd_msg.field_count >= 2) ? cmd_msg.fields[1] : "";
            int request_all = 0;
            int request_details = 0;
            int invalid_flag = 0;

            if (flags) {
                for (const char *p = flags; *p; p++) {
                    if (*p == '-' || *p == ' ' || *p == '\t') {
                        continue;
                    }
                    if (*p == 'a' || *p == 'A') {
                        request_all = 1;
                    } else if (*p == 'l' || *p == 'L') {
                        request_details = 1;
                    } else {
                        invalid_flag = 1;
                        break;
                    }
                }
            }

            if (invalid_flag) {
                send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Unsupported VIEW flag.", ctx->peer_ip, ctx->peer_port);
            } else if (ctx->username[0] == '\0') {
                send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Client identity unknown.", ctx->peer_ip, ctx->peer_port);
            } else {
                pthread_mutex_lock(&ns->state_lock);
                for (int i = 0; i < ns->file_count; i++) {
                    FileMetadata *file = &ns->files[i];
                    int has_access = request_all;

                    if (!has_access) {
                        if (strncmp(file->owner, ctx->username, MAX_USERNAME_LENGTH) == 0) {
                            has_access = 1;
                        } else if (file_acl_contains(file->read_access_users, file->read_access_count, ctx->username)) {
                            has_access = 1;
                        } else if (file_acl_contains(file->write_access_users, file->write_access_count, ctx->username)) {
                            has_access = 1;
                        }
                    }

                    if (!has_access) {
                        continue;
                    }

                    if (request_details) {
                        char word_buf[32];
                        char char_buf[32];
                        char accessed_buf[64];
                        char modified_buf[64];

                        snprintf(word_buf, sizeof(word_buf), "%lld", file->word_count);
                        snprintf(char_buf, sizeof(char_buf), "%lld", file->char_count);

                        const char *last_access = (file->created > 0) ? format_time(file->created) : "N/A";
                        const char *last_modified = (file->modified > 0) ? format_time(file->modified) : "N/A";
                        snprintf(accessed_buf, sizeof(accessed_buf), "%s", last_access ? last_access : "N/A");
                        snprintf(modified_buf, sizeof(modified_buf), "%s", last_modified ? last_modified : "N/A");

                        const char *resp_fields[] = {
                            RESP_OK_VIEW_L,
                            file->filename,
                            file->owner,
                            word_buf,
                            char_buf,
                            accessed_buf,
                            modified_buf
                        };

                        char *resp = protocol_build_message(resp_fields, 7);
                        if (!resp) {
                            log_message(LOG_WARNING, "NS", "Failed to build VIEW entry for %s.", file->filename);
                            continue;
                        }

                        protocol_send_message(ctx->conn_fd, resp);
                        free(resp);
                    } else {
                        const char *resp_fields[] = {RESP_OK_VIEW_L, file->filename};
                        char *resp = protocol_build_message(resp_fields, 2);
                        if (!resp) {
                            log_message(LOG_WARNING, "NS", "Failed to build VIEW entry for %s.", file->filename);
                            continue;
                        }

                        protocol_send_message(ctx->conn_fd, resp);
                        free(resp);
                    }
                }
                pthread_mutex_unlock(&ns->state_lock);

                const char *end_fields[] = {RESP_OK_VIEW_END};
                char *end_msg = protocol_build_message(end_fields, 1);
                if (end_msg) {
                    protocol_send_message(ctx->conn_fd, end_msg);
                    free(end_msg);
                } else {
                    send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to build VIEW terminator.", ctx->peer_ip, ctx->peer_port);
                }
            }
        } else if (strcmp(command, MSG_INFO) == 0) {
            if (cmd_msg.field_count < 2) {
                send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Filename missing in INFO.", ctx->peer_ip, ctx->peer_port);
            } else {
                const char *filename = cmd_msg.fields[1];

                if (!validate_filename(filename)) {
                    send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid filename requested for INFO.", ctx->peer_ip, ctx->peer_port);
                } else if (ctx->username[0] == '\0') {
                    send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Client identity unknown.", ctx->peer_ip, ctx->peer_port);
                } else {
                    int error_sent = 0;
                    int metadata_updated = 0;
                    FileMetadata file_snapshot;
                    memset(&file_snapshot, 0, sizeof(file_snapshot));

                    pthread_mutex_lock(&ns->state_lock);
                    int file_index = -1;
                    FileMetadata *file = file_lookup_locked(ns, filename, &file_index);
                    if (!file) {
                        send_error_and_log(ctx->conn_fd, ERR_FILE_NOT_FOUND, "File not found for INFO.", ctx->peer_ip, ctx->peer_port);
                        error_sent = 1;
                    } else {
                        if (!file_has_read_access(file, ctx->username)) {
                            send_error_and_log(ctx->conn_fd, ERR_PERMISSION_DENIED, "User lacks read access for INFO.", ctx->peer_ip, ctx->peer_port);
                            error_sent = 1;
                        } else {
                            // time_t now = get_current_time();
                            // file->last_access = now;
                            // safe_strcpy(file->last_access_user, ctx->username, sizeof(file->last_access_user));
                            file_snapshot = *file;
                            // metadata_updated = 1;
                        }
                    }
                    pthread_mutex_unlock(&ns->state_lock);

                    if (error_sent) {
                        continue;
                    }

                    const char *start_fields[] = {RESP_OK_INFO_START, file_snapshot.filename};
                    char *start_msg = protocol_build_message(start_fields, 2);
                    if (!start_msg) {
                        send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to build INFO start message.", ctx->peer_ip, ctx->peer_port);
                        continue;
                    }

                    int send_failed = 0;
                    if (protocol_send_message(ctx->conn_fd, start_msg) < 0) {
                        send_failed = 1;
                    }
                    free(start_msg);

                    if (!send_failed) {
                        char line[MAX_FIELD_SIZE];

                        snprintf(line, sizeof(line), "Owner: %s", file_snapshot.owner[0] ? file_snapshot.owner : "N/A");
                        if (send_info_line(ctx->conn_fd, line) != 0) {
                            send_failed = 1;
                        }

                        if (!send_failed) {
                            long long size_value = file_snapshot.size >= 0 ? file_snapshot.size : 0;
                            snprintf(line, sizeof(line), "Size: %lld bytes", size_value);
                            if (send_info_line(ctx->conn_fd, line) != 0) {
                                send_failed = 1;
                            }
                        }

                        if (!send_failed) {
                            snprintf(line, sizeof(line), "Words: %s", "N/A");
                            if (send_info_line(ctx->conn_fd, line) != 0) {
                                send_failed = 1;
                            }
                        }

                        if (!send_failed) {
                            snprintf(line, sizeof(line), "Chars: %s", "N/A");
                            if (send_info_line(ctx->conn_fd, line) != 0) {
                                send_failed = 1;
                            }
                        }

                        if (!send_failed) {
                            long long created_epoch = (long long)file_snapshot.created;
                            char created_buf[64];
                            if (created_epoch > 0) {
                                const char *tmp = format_time((time_t)created_epoch);
                                safe_strcpy(created_buf, tmp ? tmp : "N/A", sizeof(created_buf));
                            } else {
                                safe_strcpy(created_buf, "N/A", sizeof(created_buf));
                            }
                            snprintf(line, sizeof(line), "Created: %s", created_buf);
                            if (send_info_line(ctx->conn_fd, line) != 0) {
                                send_failed = 1;
                            }
                        }

                        if (!send_failed) {
                            long long modified_epoch = (long long)file_snapshot.modified;
                            char modified_buf[64];
                            if (modified_epoch > 0) {
                                const char *tmp = format_time((time_t)modified_epoch);
                                safe_strcpy(modified_buf, tmp ? tmp : "N/A", sizeof(modified_buf));
                            } else {
                                safe_strcpy(modified_buf, "N/A", sizeof(modified_buf));
                            }
                            const char *mod_user = file_snapshot.last_modified_user[0] ? file_snapshot.last_modified_user : "N/A";
                            snprintf(line, sizeof(line), "Modified: %s by %s", modified_buf, mod_user);
                            if (send_info_line(ctx->conn_fd, line) != 0) {
                                send_failed = 1;
                            }
                        }

                        if (!send_failed) {
                            long long access_epoch = (long long)file_snapshot.last_access;
                            char access_buf[64];
                            if (access_epoch > 0) {
                                const char *tmp = format_time((time_t)access_epoch);
                                safe_strcpy(access_buf, tmp ? tmp : "N/A", sizeof(access_buf));
                            } else {
                                safe_strcpy(access_buf, "N/A", sizeof(access_buf));
                            }
                            snprintf(line, sizeof(line), "Last Access: %s", access_buf);
                            if (send_info_line(ctx->conn_fd, line) != 0) {
                                send_failed = 1;
                            }
                        }

                        if (!send_failed) {
                            const char *last_user = file_snapshot.last_access_user[0] ? file_snapshot.last_access_user : "N/A";
                            snprintf(line, sizeof(line), "Last Access User: %s", last_user);
                            if (send_info_line(ctx->conn_fd, line) != 0) {
                                send_failed = 1;
                            }
                        }

                        if (!send_failed) {
                            char read_list[MAX_FIELD_SIZE];
                            char write_list[MAX_FIELD_SIZE];
                            read_list[0] = '\0';
                            write_list[0] = '\0';

                            int read_first = 1;
                            int write_first = 1;

                            if (file_snapshot.owner[0]) {
                                acl_append_token(read_list, sizeof(read_list), file_snapshot.owner, &read_first);
                                acl_append_token(write_list, sizeof(write_list), file_snapshot.owner, &write_first);
                            }

                            for (int i = 0; i < file_snapshot.read_access_count; i++) {
                                if (strncmp(file_snapshot.read_access_users[i], file_snapshot.owner, MAX_USERNAME_LENGTH) == 0) {
                                    continue;
                                }
                                acl_append_token(read_list, sizeof(read_list), file_snapshot.read_access_users[i], &read_first);
                            }

                            for (int i = 0; i < file_snapshot.write_access_count; i++) {
                                if (strncmp(file_snapshot.write_access_users[i], file_snapshot.owner, MAX_USERNAME_LENGTH) == 0) {
                                    continue;
                                }
                                acl_append_token(write_list, sizeof(write_list), file_snapshot.write_access_users[i], &write_first);
                            }

                            if (read_first) {
                                safe_strcpy(read_list, "None", sizeof(read_list));
                            }
                            if (write_first) {
                                safe_strcpy(write_list, "None", sizeof(write_list));
                            }

                            snprintf(line, sizeof(line), "Access Rights: Read=[%s]; Write=[%s]", read_list, write_list);
                            if (send_info_line(ctx->conn_fd, line) != 0) {
                                send_failed = 1;
                            }
                        }
                    }

                    const char *end_fields[] = {RESP_OK_INFO_END};
                    char *end_msg = protocol_build_message(end_fields, 1);
                    if (end_msg) {
                        if (!send_failed) {
                            protocol_send_message(ctx->conn_fd, end_msg);
                        }
                        free(end_msg);
                    }

                    if (send_failed) {
                        log_message(LOG_WARNING, "NS", "Failed to stream INFO response to %s:%d", ctx->peer_ip, ctx->peer_port);
                    } else {
                        log_message(LOG_INFO, "NS", "Served INFO for '%s' to %s", file_snapshot.filename, ctx->username);
                    }

                    if (metadata_updated) {
                        ns_save_metadata(ns);
                    }
                }
            }
        } else if (strcmp(command, MSG_EXEC) == 0) {
            if (cmd_msg.field_count < 2) {
                send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Filename missing in EXEC.", ctx->peer_ip, ctx->peer_port);
            } else {
                const char *filename = cmd_msg.fields[1];
                if (!validate_filename(filename)) {
                    send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid filename.", ctx->peer_ip, ctx->peer_port);
                } else if (ctx->username[0] == '\0') {
                    send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Client identity unknown.", ctx->peer_ip, ctx->peer_port);
                } else {
                    StorageServerInfo *target_ss = NULL;
                    int target_ss_acquired = 0;
                    char resolved_filename[MAX_FILENAME_LENGTH] = {0};
                    char *ss_resp_raw = NULL;
                    ProtocolMessage content_msg;
                    int content_msg_valid = 0;
                    int metadata_dirty = 0;
                    FILE *pipe = NULL;

                    do {
                        pthread_mutex_lock(&ns->state_lock);
                        int file_index = -1;
                        FileMetadata *file = file_lookup_locked(ns, filename, &file_index);
                        if (!file) {
                            pthread_mutex_unlock(&ns->state_lock);
                            if (metadata_dirty) {
                                ns_save_metadata(ns);
                                metadata_dirty = 0;
                            }
                            send_error_and_log(ctx->conn_fd, ERR_FILE_NOT_FOUND, "File not found.", ctx->peer_ip, ctx->peer_port);
                            break;
                        }

                        FileMetadata file_snapshot = *file;
                        if (!file_has_read_access(&file_snapshot, ctx->username)) {
                            pthread_mutex_unlock(&ns->state_lock);
                            if (metadata_dirty) {
                                ns_save_metadata(ns);
                                metadata_dirty = 0;
                            }
                            send_error_and_log(ctx->conn_fd, ERR_PERMISSION_DENIED, "User lacks read access for EXEC.", ctx->peer_ip, ctx->peer_port);
                            break;
                        }

                        time_t now = get_current_time();
                        file->last_access = now;
                        safe_strcpy(file->last_access_user, ctx->username, sizeof(file->last_access_user));
                        metadata_dirty = 1;
                        file_snapshot = *file;

                        if (file_index >= 0) {
                            file_cache_store(ns, file_snapshot.filename, file_index);
                        }

                        if (safe_strcpy(resolved_filename, file_snapshot.filename, sizeof(resolved_filename)) != 0) {
                            pthread_mutex_unlock(&ns->state_lock);
                            if (metadata_dirty) {
                                ns_save_metadata(ns);
                                metadata_dirty = 0;
                            }
                            send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to resolve filename for EXEC.", ctx->peer_ip, ctx->peer_port);
                            break;
                        }

                        for (int i = 0; i < ns->ss_count; i++) {
                            StorageServerInfo *candidate = ns->storage_servers[i];
                            if (!candidate || !candidate->is_alive || candidate->sockfd < 0) {
                                continue;
                            }
                            if (strncmp(candidate->client_ip, file_snapshot.ss_ip, MAX_IP_LENGTH) == 0 &&
                                candidate->client_port == file_snapshot.ss_port) {
                                target_ss = candidate;
                                storage_server_acquire(target_ss);
                                target_ss_acquired = 1;
                                break;
                            }
                        }

                        pthread_mutex_unlock(&ns->state_lock);
                        if (metadata_dirty) {
                            ns_save_metadata(ns);
                            metadata_dirty = 0;
                        }

                        if (!target_ss) {
                            send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Storage server for file is offline.", ctx->peer_ip, ctx->peer_port);
                            break;
                        }

                        const char *content_fields[] = {MSG_GET_CONTENT, resolved_filename};
                        int ss_status = storage_server_send_and_wait(target_ss, content_fields, 2, &ss_resp_raw);
                        storage_server_release(target_ss);
                        target_ss_acquired = 0;

                        if (ss_status != 0) {
                            const char *detail = "Failed to get file content from storage server.";
                            send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, detail, ctx->peer_ip, ctx->peer_port);
                            break;
                        }

                        if (!ss_resp_raw || protocol_parse_message(ss_resp_raw, &content_msg) != 0 || content_msg.field_count == 0) {
                            send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Malformed storage server response.", ctx->peer_ip, ctx->peer_port);
                            break;
                        }
                        content_msg_valid = 1;

                        if (protocol_is_error(&content_msg)) {
                            int err_code = protocol_get_error_code(&content_msg);
                            const char *detail = (content_msg.field_count >= 3) ? content_msg.fields[2] : "Storage server error.";
                            send_error_and_log(ctx->conn_fd, err_code, detail, ctx->peer_ip, ctx->peer_port);
                            break;
                        }

                        if (strcmp(content_msg.fields[0], RESP_OK_CONTENT) != 0 || content_msg.field_count < 3) {
                            send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Unexpected storage server payload for EXEC.", ctx->peer_ip, ctx->peer_port);
                            break;
                        }

                        const char *file_content = content_msg.fields[2] ? content_msg.fields[2] : "";
                        pipe = popen(file_content, "r");
                        if (!pipe) {
                            send_error_and_log(ctx->conn_fd, ERR_EXEC_FAILED, "Failed to execute command on server.", ctx->peer_ip, ctx->peer_port);
                            break;
                        }

                        int send_failed = 0;
                        const char *start_fields[] = {RESP_OK_EXEC_START};
                        char *resp = protocol_build_message(start_fields, 1);
                        if (!resp || protocol_send_message(ctx->conn_fd, resp) < 0) {
                            send_failed = 1;
                        }
                        if (resp) {
                            free(resp);
                        }

                        char line_buf[MAX_FIELD_SIZE];
                        while (!send_failed && fgets(line_buf, sizeof(line_buf), pipe) != NULL) {
                            trim_string(line_buf);
                            const char *line_fields[] = {RESP_EXEC_OUT, line_buf};
                            resp = protocol_build_message(line_fields, 2);
                            if (!resp) {
                                send_failed = 1;
                                break;
                            }
                            if (protocol_send_message(ctx->conn_fd, resp) < 0) {
                                send_failed = 1;
                            }
                            free(resp);
                        }

                        if (pipe) {
                            pclose(pipe);
                            pipe = NULL;
                        }

                        if (!send_failed) {
                            const char *end_fields[] = {RESP_OK_EXEC_END};
                            resp = protocol_build_message(end_fields, 1);
                            if (!resp || protocol_send_message(ctx->conn_fd, resp) < 0) {
                                send_failed = 1;
                            }
                            if (resp) {
                                free(resp);
                            }
                        }

                        if (send_failed) {
                            log_message(LOG_WARNING, "NS", "EXEC streaming interrupted for %s:%d", ctx->peer_ip, ctx->peer_port);
                        } else {
                            log_message(LOG_INFO, "NS", "Executed '%s' for %s", resolved_filename, ctx->username);
                        }
                    } while (0);

                    if (pipe) {
                        pclose(pipe);
                    }
                    if (content_msg_valid) {
                        protocol_free_message(&content_msg);
                    }
                    if (ss_resp_raw) {
                        free(ss_resp_raw);
                    }
                    if (target_ss_acquired && target_ss) {
                        storage_server_release(target_ss);
                    }
                }
            }
        } else if (strcmp(command, MSG_REQUEST_ACCESS) == 0) {
            if (cmd_msg.field_count < 3) {
                send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Usage: REQUESTACCESS <filename> <R|W>", ctx->peer_ip, ctx->peer_port);
            } else {
                const char *filename = cmd_msg.fields[1];
                const char *perm = cmd_msg.fields[2];
                int grant_read = 0;
                int grant_write = 0;

                if (!validate_filename(filename)) {
                    send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid filename.", ctx->peer_ip, ctx->peer_port);
                    
                } else if (parse_permission_flags(perm, &grant_read, &grant_write) != 0 || (!grant_read && !grant_write)) {
                    send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Permission must be R or W.", ctx->peer_ip, ctx->peer_port);
                } else {
                    pthread_mutex_lock(&ns->state_lock);
                    int file_index = -1;
                    FileMetadata *file = file_lookup_locked(ns, filename, &file_index);
                    if (!file) {
                        pthread_mutex_unlock(&ns->state_lock);
                        send_error_and_log(ctx->conn_fd, ERR_FILE_NOT_FOUND, "File not found.", ctx->peer_ip, ctx->peer_port);
                    } else if (ns->request_count >= NS_MAX_ACCESS_REQUESTS) {
                        pthread_mutex_unlock(&ns->state_lock);
                        send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Request queue is full.", ctx->peer_ip, ctx->peer_port);
                    } else {
                        AccessRequest *req = &ns->access_requests[ns->request_count++];
                        req->id = ns->next_request_id++;
                        safe_strcpy(req->filename, filename, sizeof(req->filename));
                        safe_strcpy(req->from_user, ctx->username, sizeof(req->from_user));
                        safe_strcpy(req->to_user, file->owner, sizeof(req->to_user));
                        req->grant_read = grant_read;
                        req->grant_write = grant_write;
                        req->is_pending = 1;
                        pthread_mutex_unlock(&ns->state_lock);
                        send_simple_ok(ctx->conn_fd, "Access request submitted.");
                    }
                }
            }
        } else if (strcmp(command, MSG_LIST_REQUESTS) == 0) {
            pthread_mutex_lock(&ns->state_lock);
            for (int i = 0; i < ns->request_count; i++) {
                AccessRequest *req = &ns->access_requests[i];
                if (!req->is_pending || strcmp(req->to_user, ctx->username) != 0) {
                    continue;
                }

                char id_buf[16];
                snprintf(id_buf, sizeof(id_buf), "%d", req->id);

                const char *perm;
                if (req->grant_write && req->grant_read) {
                    perm = "Read & Write";
                } else if (req->grant_write) {
                    perm = "Write";
                } else {
                    perm = "Read";
                }
                const char *fields[] = {RESP_OK_REQUEST_LIST, id_buf, req->filename, req->from_user, perm};
                char *resp = protocol_build_message(fields, 5);
                if (resp) {
                    protocol_send_message(ctx->conn_fd, resp);
                    free(resp);
                }
            }
            pthread_mutex_unlock(&ns->state_lock);

            const char *end_fields[] = {RESP_OK_REQUEST_LIST_END};
            char *end_msg = protocol_build_message(end_fields, 1);
            if (end_msg) {
                protocol_send_message(ctx->conn_fd, end_msg);
                free(end_msg);
            }
        } else if (strcmp(command, MSG_APPROVE_ACCESS) == 0) {
            if (cmd_msg.field_count < 2) {
                send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Usage: APPROVEACCESS <request_id>", ctx->peer_ip, ctx->peer_port);
            } else {
                int req_id = atoi(cmd_msg.fields[1]);
                AccessRequest *target_req = NULL;
                int file_index = -1;
                StorageServerInfo *target_ss = NULL;
                int target_ss_acquired = 0;
                char *ss_resp_raw = NULL;
                int rollback_needed = 0;

                pthread_mutex_lock(&ns->state_lock);
                for (int i = 0; i < ns->request_count; i++) {
                    if (ns->access_requests[i].id == req_id) {
                        target_req = &ns->access_requests[i];
                        break;
                    }
                }

                if (!target_req || !target_req->is_pending || strcmp(target_req->to_user, ctx->username) != 0) {
                    pthread_mutex_unlock(&ns->state_lock);
                    send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid request ID or not file owner.", ctx->peer_ip, ctx->peer_port);
                } else {
                    FileMetadata *file = file_lookup_locked(ns, target_req->filename, &file_index);
                    if (!file) {
                        pthread_mutex_unlock(&ns->state_lock);
                        send_error_and_log(ctx->conn_fd, ERR_FILE_NOT_FOUND, "File for this request no longer exists.", ctx->peer_ip, ctx->peer_port);
                    } else {
                        file_add_access(file, target_req->from_user, target_req->grant_read, target_req->grant_write);
                        target_req->is_pending = 0;

                        for (int i = 0; i < ns->ss_count; i++) {
                            StorageServerInfo *candidate = ns->storage_servers[i];
                            if (!candidate || !candidate->is_alive || candidate->sockfd < 0) {
                                continue;
                            }
                            if (strcmp(candidate->client_ip, file->ss_ip) == 0 && candidate->client_port == file->ss_port) {
                                target_ss = candidate;
                                storage_server_acquire(target_ss);
                                target_ss_acquired = 1;
                                break;
                            }
                        }

                        pthread_mutex_unlock(&ns->state_lock);

                        if (!target_ss) {
                            rollback_needed = 1;
                            send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Storage server is offline. Access not granted.", ctx->peer_ip, ctx->peer_port);
                        } else {
                            char perm_token[3] = "";
                            if (target_req->grant_read) {
                                safe_strcat(perm_token, "R", sizeof(perm_token));
                            }
                            if (target_req->grant_write) {
                                safe_strcat(perm_token, "W", sizeof(perm_token));
                            }
                            const char *ss_fields[] = {MSG_SS_ADDACCESS, target_req->filename, target_req->from_user, perm_token[0] ? perm_token : "R"};
                            int ss_status = storage_server_send_and_wait(target_ss, ss_fields, 4, &ss_resp_raw);

                            if (ss_status != 0) {
                                rollback_needed = 1;
                                send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Storage server failed to update ACL.", ctx->peer_ip, ctx->peer_port);
                            } else if (ss_resp_raw) {
                                ProtocolMessage ss_msg;
                                if (protocol_parse_message(ss_resp_raw, &ss_msg) != 0 || protocol_is_error(&ss_msg)) {
                                    rollback_needed = 1;
                                    send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Storage server rejected ACL change.", ctx->peer_ip, ctx->peer_port);
                                    protocol_free_message(&ss_msg);
                                } else {
                                    protocol_free_message(&ss_msg);
                                    send_simple_ok(ctx->conn_fd, "Access approved and saved.");
                                }
                                free(ss_resp_raw);
                                ss_resp_raw = NULL;
                            } else {
                                send_simple_ok(ctx->conn_fd, "Access approved and saved.");
                            }
                        }

                        if (rollback_needed) {
                            pthread_mutex_lock(&ns->state_lock);
                            if (file_index >= 0 && file_index < ns->file_count) {
                                file_remove_access(&ns->files[file_index], target_req->from_user);
                            }
                            target_req->is_pending = 1;
                            pthread_mutex_unlock(&ns->state_lock);
                        }

                        if (target_ss_acquired) {
                            storage_server_release(target_ss);
                        }
                    }
                }
            }
        } else if (strcmp(command, MSG_REJECT_ACCESS) == 0) {
            if (cmd_msg.field_count < 2) {
                send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Usage: REJECTACCESS <request_id>", ctx->peer_ip, ctx->peer_port);
            } else {
                int req_id = atoi(cmd_msg.fields[1]);
                int found = 0;

                pthread_mutex_lock(&ns->state_lock);
                for (int i = 0; i < ns->request_count; i++) {
                    AccessRequest *req = &ns->access_requests[i];
                    if (req->id == req_id && req->is_pending && strcmp(req->to_user, ctx->username) == 0) {
                        req->is_pending = 0;
                        found = 1;
                        break;
                    }
                }
                pthread_mutex_unlock(&ns->state_lock);

                if (found) {
                    send_simple_ok(ctx->conn_fd, "Request rejected.");
                } else {
                    send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid or non-pending request ID.", ctx->peer_ip, ctx->peer_port);
                }
            }
        } else if (strcmp(command, MSG_CREATE) == 0) {
            // Handle the CREATE command: validate input, update metadata, and coordinate with storage servers

            if (cmd_msg.field_count < 2) {
                // Check if the filename is provided in the command
                send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Filename missing in CREATE.", ctx->peer_ip, ctx->peer_port);
            } else {
                const char *filename = cmd_msg.fields[1];

                if (!validate_filename(filename)) {
                    // Validate the filename format
                    send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid filename requested for CREATE.", ctx->peer_ip, ctx->peer_port);
                } else if (ctx->username[0] == '\0') {
                    // Ensure the client identity is known
                    send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Client identity unknown.", ctx->peer_ip, ctx->peer_port);
                } else {
                    int result = -1;
                    int persist_metadata = 0;
                    StorageServerInfo *target_ss = NULL;
                    int target_ss_acquired = 0;
                    pthread_mutex_lock(&ns->state_lock);

                    int preexisting_index = -1;

                    if (ns->file_count >= NS_MAX_FILES) {
                        // Check if the maximum file limit is reached
                        pthread_mutex_unlock(&ns->state_lock);
                        send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Maximum file limit reached.", ctx->peer_ip, ctx->peer_port);
                    } else if (file_lookup_locked(ns, filename, &preexisting_index) != NULL) {
                        // Check if the file already exists
                        pthread_mutex_unlock(&ns->state_lock);
                        send_error_and_log(ctx->conn_fd, ERR_FILE_EXISTS, "File already exists.", ctx->peer_ip, ctx->peer_port);
                    } else if (ns->ss_count == 0) {
                        // Ensure there are available storage servers
                        pthread_mutex_unlock(&ns->state_lock);
                        send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "No storage servers available.", ctx->peer_ip, ctx->peer_port);
                    } else {
                        // Select a storage server using round-robin load balancing
                        int start_index = ns->ss_round_robin_index;

                        for (int i = 0; i < ns->ss_count; i++) {
                            int current_index = (start_index + i) % ns->ss_count;
                            StorageServerInfo *candidate = ns->storage_servers[current_index];
                            if (!candidate || !candidate->is_alive) {
                                continue;
                            }
                            target_ss = candidate;
                            ns->ss_round_robin_index = (current_index + 1) % ns->ss_count;
                            break;
                        }

                        if (!target_ss) {
                            // Handle the case where no alive storage servers are available
                            pthread_mutex_unlock(&ns->state_lock);
                            send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "No alive storage servers available.", ctx->peer_ip, ctx->peer_port);
                            continue;
                        }

                        storage_server_acquire(target_ss);
                        target_ss_acquired = 1;

                        int new_index = ns->file_count;
                        FileMetadata *file = &ns->files[new_index];
                        memset(file, 0, sizeof(FileMetadata));

                        if (safe_strcpy(file->filename, filename, sizeof(file->filename)) != 0 ||
                            safe_strcpy(file->owner, ctx->username, sizeof(file->owner)) != 0 ||
                            safe_strcpy(file->ss_ip, target_ss->client_ip, sizeof(file->ss_ip)) != 0) {
                            // Handle metadata initialization errors
                            memset(file, 0, sizeof(FileMetadata));
                            if (target_ss_acquired) {
                                storage_server_release(target_ss);
                                target_ss_acquired = 0;
                            }
                            pthread_mutex_unlock(&ns->state_lock);
                            send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to record file metadata.", ctx->peer_ip, ctx->peer_port);
                        } else {
                            // Populate metadata fields
                            file->ss_port = target_ss->client_port;
                            time_t now = get_current_time();
                            file->created = now;
                            file->modified = now;
                            file->last_access = now;
                            safe_strcpy(file->last_access_user, ctx->username, sizeof(file->last_access_user));
                            safe_strcpy(file->last_modified_user, ctx->username, sizeof(file->last_modified_user));
                            file->size = 0;
                            file->read_access_count = 0;
                            file->write_access_count = 0;

                            if (file_index_insert(ns, filename, new_index) != 0) {
                                // Handle file indexing errors
                                memset(file, 0, sizeof(FileMetadata));
                                if (target_ss_acquired) {
                                    storage_server_release(target_ss);
                                    target_ss_acquired = 0;
                                }
                                pthread_mutex_unlock(&ns->state_lock);
                                send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to index new file.", ctx->peer_ip, ctx->peer_port);
                            } else {
                                // Successfully updated metadata
                                ns->file_count++;
                                file_cache_store(ns, filename, new_index);
                                result = 0;
                                pthread_mutex_unlock(&ns->state_lock);

                                // --- NEW STORAGE SERVER COORDINATION ---
                                if (result == 0) {
                                    int metadata_valid = 1;

                                    if (!target_ss || target_ss->sockfd < 0) {
                                        // Ensure the storage server socket is valid
                                        send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Storage server socket unavailable.", ctx->peer_ip, ctx->peer_port);
                                        metadata_valid = 0;
                                    } else {
                                        const char *fields[] = {MSG_CREATE_FILE, filename, ctx->username};
                                        char *ss_resp_raw = NULL;
                                        int ss_status = storage_server_send_and_wait(target_ss, fields, 3, &ss_resp_raw);
                                        if (ss_status != 0) {
                                            // Map status codes to descriptive messages for the client
                                            const char *detail = "Storage server communication failure.";
                                            if (ss_status == -2) {
                                                detail = "Storage server unavailable.";
                                            } else if (ss_status == -3) {
                                                detail = "Failed to encode storage server command.";
                                            } else if (ss_status == -4) {
                                                detail = "Failed to send command to storage server.";
                                            } else if (ss_status == -5) {
                                                detail = "Storage server disconnected.";
                                            } else if (ss_status == -6) {
                                                detail = "Incomplete response from storage server.";
                                            }
                                            send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, detail, ctx->peer_ip, ctx->peer_port);
                                            if (ss_resp_raw) {
                                                free(ss_resp_raw);
                                            }
                                            metadata_valid = 0;
                                        } else {
                                            ProtocolMessage ss_resp_msg;
                                            if (protocol_parse_message(ss_resp_raw, &ss_resp_msg) != 0 || ss_resp_msg.field_count == 0) {
                                                // Handle response parsing errors
                                                send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to parse SS response.", ctx->peer_ip, ctx->peer_port);
                                                metadata_valid = 0;
                                            } else if (protocol_is_error(&ss_resp_msg)) {
                                                // Handle error responses from storage server
                                                int err_code = protocol_get_error_code(&ss_resp_msg);
                                                const char *detail = (ss_resp_msg.field_count >= 3) ? ss_resp_msg.fields[2] : "Storage server error.";
                                                send_error_and_log(ctx->conn_fd, err_code, detail, ctx->peer_ip, ctx->peer_port);
                                                metadata_valid = 0;
                                            } else if (strcmp(ss_resp_msg.fields[0], RESP_OK_CREATE) == 0) {
                                                // Handle successful file creation response
                                                char detail[MAX_FIELD_SIZE];
                                                snprintf(detail, sizeof(detail), "File '%s' created successfully.", filename);
                                                char *resp = protocol_build_ok(detail);
                                                if (resp) {
                                                    protocol_send_message(ctx->conn_fd, resp);
                                                    free(resp);
                                                }
                                                log_message(LOG_INFO, "NS", "File '%s' created by %s on SS %s:%d.", filename, ctx->username, target_ss->client_ip, target_ss->client_port);
                                                persist_metadata = 1;
                                            } else {
                                                // Handle unexpected responses from storage server
                                                send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Unexpected SS response.", ctx->peer_ip, ctx->peer_port);
                                                metadata_valid = 0;
                                            }

                                            protocol_free_message(&ss_resp_msg);
                                            free(ss_resp_raw);
                                        }
                                    }

                                    if (!metadata_valid) {
                                        // Rollback metadata changes in case of errors
                                        pthread_mutex_lock(&ns->state_lock);
                                        if (!remove_file_metadata_locked(ns, filename)) {
                                            log_message(LOG_WARNING, "NS", "CREATE rollback: metadata for '%s' already absent.", filename);
                                        }
                                        pthread_mutex_unlock(&ns->state_lock);
                                    }
                                }

                                if (target_ss_acquired) {
                                    storage_server_release(target_ss);
                                    target_ss_acquired = 0;
                                }

                                if (persist_metadata) {
                                    ns_save_metadata(ns);
                                }
                            }
                        }
                    }
                }
            }
        } else if (strcmp(command, MSG_DELETE) == 0) {
            if (cmd_msg.field_count < 2) {
                send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Filename missing in DELETE.", ctx->peer_ip, ctx->peer_port);
            } else {
                const char *filename = cmd_msg.fields[1];

                if (!validate_filename(filename)) {
                    send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid filename requested for DELETE.", ctx->peer_ip, ctx->peer_port);
                } else if (ctx->username[0] == '\0') {
                    send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Client identity unknown.", ctx->peer_ip, ctx->peer_port);
                } else {
                    StorageServerInfo *target_ss = NULL;
                    int target_ss_acquired = 0;
                    char *ss_resp_raw = NULL;
                    ProtocolMessage ss_resp_msg;
                    int ss_resp_parsed = 0;
                    int persist_metadata = 0;

                    pthread_mutex_lock(&ns->state_lock);
                    int file_slot = -1;
                    FileMetadata *file = file_lookup_locked(ns, filename, &file_slot);
                    if (!file) {
                        pthread_mutex_unlock(&ns->state_lock);
                        send_error_and_log(ctx->conn_fd, ERR_FILE_NOT_FOUND, "File not found for DELETE.", ctx->peer_ip, ctx->peer_port);
                    } else {
                        if (strncmp(file->owner, ctx->username, MAX_USERNAME_LENGTH) != 0) {
                            pthread_mutex_unlock(&ns->state_lock);
                            send_error_and_log(ctx->conn_fd, ERR_PERMISSION_DENIED, "Only the owner may delete the file.", ctx->peer_ip, ctx->peer_port);
                        } else {
                            for (int i = 0; i < ns->ss_count; i++) {
                                StorageServerInfo *candidate = ns->storage_servers[i];
                                if (!candidate || !candidate->is_alive) {
                                    continue;
                                }
                                if (strncmp(candidate->client_ip, file->ss_ip, MAX_IP_LENGTH) == 0 && candidate->client_port == file->ss_port) {
                                    target_ss = candidate;
                                    break;
                                }
                            }

                            if (!target_ss || target_ss->sockfd < 0 || !target_ss->is_alive) {
                                pthread_mutex_unlock(&ns->state_lock);
                                send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Storage server unavailable for DELETE.", ctx->peer_ip, ctx->peer_port);
                            } else {
                                storage_server_acquire(target_ss);
                                target_ss_acquired = 1;
                                pthread_mutex_unlock(&ns->state_lock);

                                const char *fields[] = {MSG_DELETE_FILE, filename};
                                int ss_status = storage_server_send_and_wait(target_ss, fields, 2, &ss_resp_raw);
                                if (ss_status != 0) {
                                    const char *detail = "Storage server communication failure.";
                                    if (ss_status == -2) {
                                        detail = "Storage server unavailable.";
                                    } else if (ss_status == -3) {
                                        detail = "Failed to encode storage server command.";
                                    } else if (ss_status == -4) {
                                        detail = "Failed to send command to storage server.";
                                    } else if (ss_status == -5) {
                                        detail = "Storage server disconnected.";
                                    } else if (ss_status == -6) {
                                        detail = "Incomplete response from storage server.";
                                    }
                                    send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, detail, ctx->peer_ip, ctx->peer_port);
                                } else if (protocol_parse_message(ss_resp_raw, &ss_resp_msg) != 0 || ss_resp_msg.field_count == 0) {
                                    send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to parse SS response.", ctx->peer_ip, ctx->peer_port);
                                } else {
                                    ss_resp_parsed = 1;
                                    if (protocol_is_error(&ss_resp_msg)) {
                                        int err_code = protocol_get_error_code(&ss_resp_msg);
                                        const char *detail = (ss_resp_msg.field_count >= 3) ? ss_resp_msg.fields[2] : "Storage server error.";
                                        send_error_and_log(ctx->conn_fd, err_code, detail, ctx->peer_ip, ctx->peer_port);
                                    } else if (strcmp(ss_resp_msg.fields[0], RESP_OK_DELETE) != 0) {
                                        send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Unexpected SS response.", ctx->peer_ip, ctx->peer_port);
                                    } else {
                                        pthread_mutex_lock(&ns->state_lock);
                                        if (!remove_file_metadata_locked(ns, filename)) {
                                            pthread_mutex_unlock(&ns->state_lock);
                                            send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to retire file metadata.", ctx->peer_ip, ctx->peer_port);
                                        } else {
                                            for (int i = 0; i < ns->request_count; i++) {
                                                AccessRequest *req = &ns->access_requests[i];
                                                if (req->is_pending && strcmp(req->filename, filename) == 0) {
                                                    req->is_pending = 0;
                                                    log_message(LOG_INFO, "NS", "Auto-cancelled request #%d for deleted file '%s'", req->id, filename);
                                                }
                                            }

                                            pthread_mutex_unlock(&ns->state_lock);
                                            char detail_buf[MAX_FIELD_SIZE];
                                            snprintf(detail_buf, sizeof(detail_buf), "File '%s' deleted successfully.", filename);
                                            send_simple_ok(ctx->conn_fd, detail_buf);
                                            log_message(LOG_INFO, "NS", "File '%s' deleted by %s via SS %s:%d.", filename, ctx->username, target_ss->client_ip, target_ss->client_port);
                                            persist_metadata = 1;
                                        }
                                    }
                                }

                                if (ss_resp_parsed) {
                                    protocol_free_message(&ss_resp_msg);
                                    ss_resp_parsed = 0;
                                }
                                if (ss_resp_raw) {
                                    free(ss_resp_raw);
                                    ss_resp_raw = NULL;
                                }
                                if (target_ss_acquired) {
                                    storage_server_release(target_ss);
                                    target_ss_acquired = 0;
                                }

                                if (persist_metadata) {
                                    ns_save_metadata(ns);
                                }
                            }
                        }
                    }
                }
            }
        } else if (strcmp(command, MSG_REQ_LOC) == 0) {
            const char *operation = NULL;
            const char *filename = NULL;
            if (cmd_msg.field_count >= 3) {
                operation = cmd_msg.fields[1];
                filename = cmd_msg.fields[2];
            } else if (cmd_msg.field_count >= 2) {
                operation = "READ";
                filename = cmd_msg.fields[1];
            } else {
                send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Filename missing in location request.", ctx->peer_ip, ctx->peer_port);
                protocol_free_message(&cmd_msg);
                free(raw_command);
                continue;
            }

            if (!validate_filename(filename)) {
                send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid filename requested.", ctx->peer_ip, ctx->peer_port);
            } else if (ctx->username[0] == '\0') {
                send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Client identity unknown.", ctx->peer_ip, ctx->peer_port);
            } else {
                FileMetadata file_snapshot;
                int have_access = 0;
                int file_exists = 0;
                int operation_known = 1;
                int operation_is_destructive = 0;
                int metadata_dirty = 0;

                pthread_mutex_lock(&ns->state_lock);
                int file_index = -1;
                FileMetadata *file = file_lookup_locked(ns, filename, &file_index);
                if (file) {
                    file_exists = 1;

                    if (!operation || operation[0] == '\0' || strcasecmp(operation, "READ") == 0) {
                        have_access = file_has_read_access(file, ctx->username);
                    } else if (strcasecmp(operation, "WRITE") == 0) {
                        have_access = file_has_write_access(file, ctx->username);
                        operation_is_destructive = 1;
                    } else if (strcasecmp(operation, "STREAM") == 0) {
                        have_access = file_has_read_access(file, ctx->username);
                    } else if (strcasecmp(operation, "CHECKPOINT") == 0) {
                        have_access = file_has_write_access(file, ctx->username);
                        operation_is_destructive = 1;
                    } else if (strcasecmp(operation, "REVERT") == 0) {
                        have_access = file_has_write_access(file, ctx->username);
                        operation_is_destructive = 1;
                    } else if (strcasecmp(operation, "UNDO") == 0) {
                        have_access = file_has_write_access(file, ctx->username);
                        operation_is_destructive = 1;
                    } else if (strcasecmp(operation, "VIEWCHECKPOINT") == 0) {
                        have_access = file_has_read_access(file, ctx->username);
                    } else if (strcasecmp(operation, "LISTCHECKPOINTS") == 0) {
                        have_access = file_has_read_access(file, ctx->username);
                    } else if (strcasecmp(operation, "EXEC") == 0) {
                        have_access = file_has_read_access(file, ctx->username);
                    } else {
                        operation_known = 0;
                    }

                    if (operation_known && have_access) {
                        time_t now = get_current_time();
                        file->last_access = now;
                        safe_strcpy(file->last_access_user, ctx->username, sizeof(file->last_access_user));
                        if (operation_is_destructive) {
                            file->modified = now;
                            safe_strcpy(file->last_modified_user, ctx->username, sizeof(file->last_modified_user));
                        }
                        metadata_dirty = 1;
                        file_snapshot = *file;
                        if (file_index >= 0) {
                            file_cache_store(ns, filename, file_index);
                        }
                    }
                }
                pthread_mutex_unlock(&ns->state_lock);

                if (metadata_dirty) {
                    ns_save_metadata(ns);
                }

                if (!operation_known) {
                    send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Unsupported location request operation.", ctx->peer_ip, ctx->peer_port);
                } else if (!file_exists) {
                    send_error_and_log(ctx->conn_fd, ERR_FILE_NOT_FOUND, "Requested file not found.", ctx->peer_ip, ctx->peer_port);
                } else if (!have_access) {
                    send_error_and_log(ctx->conn_fd, ERR_PERMISSION_DENIED, "User lacks required access.", ctx->peer_ip, ctx->peer_port);
                } else {
                    char port_buf[16];
                    snprintf(port_buf, sizeof(port_buf), "%d", file_snapshot.ss_port);
                    const char *resp_fields[] = {RESP_OK_LOC, file_snapshot.filename, file_snapshot.ss_ip, port_buf};
                    char *resp = protocol_build_message(resp_fields, 4);
                    if (resp) {
                        protocol_send_message(ctx->conn_fd, resp);
                        free(resp);
                    } else {
                        send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to build OK_LOC response.", ctx->peer_ip, ctx->peer_port);
                    }
                }
            }
        } else if (strcmp(command, MSG_ADDACCESS) == 0) {
            if (cmd_msg.field_count < 4) {
                send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Missing parameters for ADDACCESS.", ctx->peer_ip, ctx->peer_port);
            } else {
                const char *filename = cmd_msg.fields[1];
                const char *target_user = cmd_msg.fields[2];
                const char *permission = cmd_msg.fields[3];

                int grant_read = 0;
                int grant_write = 0;

                if (!validate_filename(filename) || !validate_username(target_user)) {
                    send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid filename or username for ADDACCESS.", ctx->peer_ip, ctx->peer_port);
                } else if (parse_permission_flags(permission, &grant_read, &grant_write) != 0 || (!grant_read && !grant_write)) {
                    send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid permission token for ADDACCESS.", ctx->peer_ip, ctx->peer_port);
                } else if (ctx->username[0] == '\0') {
                    send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Client identity unknown.", ctx->peer_ip, ctx->peer_port);
                } else {
                    StorageServerInfo *target_ss = NULL;
                    int target_ss_acquired = 0;
                    char *ss_resp_raw = NULL;
                    int file_index = -1;
                    int metadata_applied = 0;
                    int rollback_needed = 0;
                    int success = 0;
                    int error_sent = 0;
                    int state_locked = 0;
                    int persist_metadata = 0;

                    pthread_mutex_lock(&ns->state_lock);
                    state_locked = 1;

                    do {
                        FileMetadata *file = file_lookup_locked(ns, filename, &file_index);
                        if (!file) {
                            send_error_and_log(ctx->conn_fd, ERR_FILE_NOT_FOUND, "File not found for ADDACCESS.", ctx->peer_ip, ctx->peer_port);
                            error_sent = 1;
                            break;
                        }

                        if (strncmp(file->owner, ctx->username, MAX_USERNAME_LENGTH) != 0) {
                            send_error_and_log(ctx->conn_fd, ERR_PERMISSION_DENIED, "Only the owner may modify access.", ctx->peer_ip, ctx->peer_port);
                            error_sent = 1;
                            break;
                        }

                        if (strncmp(file->owner, target_user, MAX_USERNAME_LENGTH) == 0) {
                            char detail[MAX_FIELD_SIZE];
                            snprintf(detail, sizeof(detail), "Owner '%s' already has full access.", target_user);
                            char *resp = protocol_build_ok(detail);
                            if (resp) {
                                protocol_send_message(ctx->conn_fd, resp);
                                free(resp);
                            }
                            error_sent = 1;
                            break;
                        }

                        if (file_add_access(file, target_user, grant_read, grant_write) != 0) {
                            send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to update ACL.", ctx->peer_ip, ctx->peer_port);
                            error_sent = 1;
                            break;
                        }

                        metadata_applied = 1;

                        for (int i = 0; i < ns->ss_count; i++) {
                            StorageServerInfo *candidate = ns->storage_servers[i];
                            if (!candidate || !candidate->is_alive || candidate->sockfd < 0) {
                                continue;
                            }
                            if (strncmp(candidate->client_ip, file->ss_ip, MAX_IP_LENGTH) == 0 &&
                                candidate->client_port == file->ss_port) {
                                target_ss = candidate;
                                storage_server_acquire(target_ss);
                                target_ss_acquired = 1;
                                break;
                            }
                        }

                        pthread_mutex_unlock(&ns->state_lock);
                        state_locked = 0;

                        if (!target_ss) {
                            rollback_needed = 1;
                            send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Storage server is offline.", ctx->peer_ip, ctx->peer_port);
                            error_sent = 1;
                            break;
                        }

                        char perm_token[3];
                        int perm_len = 0;
                        if (grant_read) {
                            perm_token[perm_len++] = 'R';
                        }
                        if (grant_write) {
                            perm_token[perm_len++] = 'W';
                        }
                        perm_token[perm_len] = '\0';

                        const char *ss_fields[] = {MSG_SS_ADDACCESS, filename, target_user, perm_token};
                        int ss_status = storage_server_send_and_wait(target_ss, ss_fields, 4, &ss_resp_raw);
                        if (ss_status != 0) {
                            rollback_needed = 1;
                            const char *detail = "Storage server communication failure.";
                            if (ss_status == -2) {
                                detail = "Storage server unavailable.";
                            } else if (ss_status == -3) {
                                detail = "Failed to encode storage server command.";
                            } else if (ss_status == -4) {
                                detail = "Failed to send command to storage server.";
                            } else if (ss_status == -5) {
                                detail = "Storage server disconnected.";
                            } else if (ss_status == -6) {
                                detail = "Incomplete response from storage server.";
                            }
                            send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, detail, ctx->peer_ip, ctx->peer_port);
                            error_sent = 1;
                            break;
                        }

                        ProtocolMessage ss_resp_msg;
                        if (protocol_parse_message(ss_resp_raw, &ss_resp_msg) != 0 || ss_resp_msg.field_count == 0) {
                            rollback_needed = 1;
                            send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to parse SS response.", ctx->peer_ip, ctx->peer_port);
                            error_sent = 1;
                            break;
                        }

                        if (protocol_is_error(&ss_resp_msg)) {
                            rollback_needed = 1;
                            int err_code = protocol_get_error_code(&ss_resp_msg);
                            const char *detail = (ss_resp_msg.field_count >= 3) ? ss_resp_msg.fields[2] : "Storage server error.";
                            send_error_and_log(ctx->conn_fd, err_code, detail, ctx->peer_ip, ctx->peer_port);
                            protocol_free_message(&ss_resp_msg);
                            error_sent = 1;
                            break;
                        }

                        if (strcmp(ss_resp_msg.fields[0], RESP_OK_ACCESS_CHANGED) != 0) {
                            rollback_needed = 1;
                            send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Unexpected SS response.", ctx->peer_ip, ctx->peer_port);
                            protocol_free_message(&ss_resp_msg);
                            error_sent = 1;
                            break;
                        }

                        protocol_free_message(&ss_resp_msg);
                        success = 1;
                    } while (0);

                    if (state_locked) {
                        pthread_mutex_unlock(&ns->state_lock);
                        state_locked = 0;
                    }

                    if (ss_resp_raw) {
                        free(ss_resp_raw);
                        ss_resp_raw = NULL;
                    }

                    if (target_ss_acquired) {
                        storage_server_release(target_ss);
                    }

                    if (!success && metadata_applied && rollback_needed) {
                        pthread_mutex_lock(&ns->state_lock);
                        if (file_index >= 0 && file_index < ns->file_count) {
                            FileMetadata *file = &ns->files[file_index];
                            file_remove_access(file, target_user);
                        }
                        pthread_mutex_unlock(&ns->state_lock);
                    }

                    if (success) {
                        char detail[MAX_FIELD_SIZE];
                        snprintf(detail, sizeof(detail), "Access granted to %s (%s).", target_user, permission);
                        char *resp = protocol_build_ok(detail);
                        if (resp) {
                            protocol_send_message(ctx->conn_fd, resp);
                            free(resp);
                        } else {
                            send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to send OK response.", ctx->peer_ip, ctx->peer_port);
                        }
                        log_message(LOG_INFO, "NS", "%s granted %s %s access on '%s'", ctx->username, target_user, permission, filename);
                        persist_metadata = 1;
                    } else if (!error_sent) {
                        send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to update ACL.", ctx->peer_ip, ctx->peer_port);
                    }

                    if (persist_metadata) {
                        ns_save_metadata(ns);
                    }
                }
            }
        } else if (strcmp(command, MSG_REMACCESS) == 0) {
            if (cmd_msg.field_count < 3) {
                send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Missing parameters for REMACCESS.", ctx->peer_ip, ctx->peer_port);
            } else {
                const char *filename = cmd_msg.fields[1];
                const char *target_user = cmd_msg.fields[2];

                if (!validate_filename(filename) || !validate_username(target_user)) {
                    send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid filename or username for REMACCESS.", ctx->peer_ip, ctx->peer_port);
                } else if (ctx->username[0] == '\0') {
                    send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Client identity unknown.", ctx->peer_ip, ctx->peer_port);
                } else {
                    StorageServerInfo *target_ss = NULL;
                    int target_ss_acquired = 0;
                    char *ss_resp_raw = NULL;
                    int file_index = -1;
                    int metadata_applied = 0;
                    int rollback_needed = 0;
                    int success = 0;
                    int error_sent = 0;
                    int state_locked = 0;
                    int had_read = 0;
                    int had_write = 0;
                    int persist_metadata = 0;

                    pthread_mutex_lock(&ns->state_lock);
                    state_locked = 1;

                    do {
                        FileMetadata *file = file_lookup_locked(ns, filename, &file_index);
                        if (!file) {
                            send_error_and_log(ctx->conn_fd, ERR_FILE_NOT_FOUND, "File not found for REMACCESS.", ctx->peer_ip, ctx->peer_port);
                            error_sent = 1;
                            break;
                        }

                        if (strncmp(file->owner, ctx->username, MAX_USERNAME_LENGTH) != 0) {
                            send_error_and_log(ctx->conn_fd, ERR_PERMISSION_DENIED, "Only the owner may modify access.", ctx->peer_ip, ctx->peer_port);
                            error_sent = 1;
                            break;
                        }

                        had_read = file_acl_contains(file->read_access_users, file->read_access_count, target_user);
                        had_write = file_acl_contains(file->write_access_users, file->write_access_count, target_user);

                        if (!file_remove_access(file, target_user)) {
                            send_error_and_log(ctx->conn_fd, ERR_USER_NOT_FOUND, "User had no explicit access.", ctx->peer_ip, ctx->peer_port);
                            error_sent = 1;
                            break;
                        }

                        metadata_applied = 1;

                        for (int i = 0; i < ns->ss_count; i++) {
                            StorageServerInfo *candidate = ns->storage_servers[i];
                            if (!candidate || !candidate->is_alive || candidate->sockfd < 0) {
                                continue;
                            }
                            if (strncmp(candidate->client_ip, file->ss_ip, MAX_IP_LENGTH) == 0 &&
                                candidate->client_port == file->ss_port) {
                                target_ss = candidate;
                                storage_server_acquire(target_ss);
                                target_ss_acquired = 1;
                                break;
                            }
                        }

                        pthread_mutex_unlock(&ns->state_lock);
                        state_locked = 0;

                        if (!target_ss) {
                            rollback_needed = 1;
                            send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Storage server is offline.", ctx->peer_ip, ctx->peer_port);
                            error_sent = 1;
                            break;
                        }

                        const char *ss_fields[] = {MSG_SS_REMACCESS, filename, target_user};
                        int ss_status = storage_server_send_and_wait(target_ss, ss_fields, 3, &ss_resp_raw);
                        if (ss_status != 0) {
                            rollback_needed = 1;
                            const char *detail = "Storage server communication failure.";
                            if (ss_status == -2) {
                                detail = "Storage server unavailable.";
                            } else if (ss_status == -3) {
                                detail = "Failed to encode storage server command.";
                            } else if (ss_status == -4) {
                                detail = "Failed to send command to storage server.";
                            } else if (ss_status == -5) {
                                detail = "Storage server disconnected.";
                            } else if (ss_status == -6) {
                                detail = "Incomplete response from storage server.";
                            }
                            send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, detail, ctx->peer_ip, ctx->peer_port);
                            error_sent = 1;
                            break;
                        }

                        ProtocolMessage ss_resp_msg;
                        if (protocol_parse_message(ss_resp_raw, &ss_resp_msg) != 0 || ss_resp_msg.field_count == 0) {
                            rollback_needed = 1;
                            send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to parse SS response.", ctx->peer_ip, ctx->peer_port);
                            error_sent = 1;
                            break;
                        }

                        if (protocol_is_error(&ss_resp_msg)) {
                            rollback_needed = 1;
                            int err_code = protocol_get_error_code(&ss_resp_msg);
                            const char *detail = (ss_resp_msg.field_count >= 3) ? ss_resp_msg.fields[2] : "Storage server error.";
                            send_error_and_log(ctx->conn_fd, err_code, detail, ctx->peer_ip, ctx->peer_port);
                            protocol_free_message(&ss_resp_msg);
                            error_sent = 1;
                            break;
                        }

                        if (strcmp(ss_resp_msg.fields[0], RESP_OK_ACCESS_CHANGED) != 0) {
                            rollback_needed = 1;
                            send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Unexpected SS response.", ctx->peer_ip, ctx->peer_port);
                            protocol_free_message(&ss_resp_msg);
                            error_sent = 1;
                            break;
                        }

                        protocol_free_message(&ss_resp_msg);
                        success = 1;
                    } while (0);

                    if (state_locked) {
                        pthread_mutex_unlock(&ns->state_lock);
                        state_locked = 0;
                    }

                    if (ss_resp_raw) {
                        free(ss_resp_raw);
                        ss_resp_raw = NULL;
                    }

                    if (target_ss_acquired) {
                        storage_server_release(target_ss);
                    }

                    if (!success && metadata_applied && rollback_needed) {
                        pthread_mutex_lock(&ns->state_lock);
                        if (file_index >= 0 && file_index < ns->file_count) {
                            FileMetadata *file = &ns->files[file_index];
                            file_add_access(file, target_user, had_read, had_write);
                        }
                        pthread_mutex_unlock(&ns->state_lock);
                    }

                    if (success) {
                        char detail[MAX_FIELD_SIZE];
                        snprintf(detail, sizeof(detail), "Access revoked for %s.", target_user);
                        char *resp = protocol_build_ok(detail);
                        if (resp) {
                            protocol_send_message(ctx->conn_fd, resp);
                            free(resp);
                        } else {
                            send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to send OK response.", ctx->peer_ip, ctx->peer_port);
                        }
                        log_message(LOG_INFO, "NS", "%s revoked %s access on '%s'", ctx->username, target_user, filename);
                        persist_metadata = 1;
                    } else if (!error_sent) {
                        send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to update ACL.", ctx->peer_ip, ctx->peer_port);
                    }

                    if (persist_metadata) {
                        ns_save_metadata(ns);
                    }
                }
            }
        } else {
            send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Unknown client command.", ctx->peer_ip, ctx->peer_port);
        }

        protocol_free_message(&cmd_msg);
        free(raw_command);
    }
}

static void run_ss_loop(ConnectionContext *ctx) {
    if (!ctx || !ctx->ns) {
        return;
    }

    NameServer *ns = ctx->ns;
    StorageServerInfo *ss = ctx->ss_info;

    // Get storage server information
    while (1) {
        char *file_msg_raw = protocol_receive_message(ctx->conn_fd);
        if (!file_msg_raw) {
            log_message(LOG_WARNING, "NS", "SS %s:%d disconnected during file sync.", ctx->peer_ip, ctx->peer_port);
            storage_server_signal_failure(ss);
            return;
        }

        ProtocolMessage file_msg;
        if (protocol_parse_message(file_msg_raw, &file_msg) != 0 || file_msg.field_count == 0) {
            send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid file sync message.", ctx->peer_ip, ctx->peer_port);
            free(file_msg_raw);
            continue;
        }

        if (strcmp(file_msg.fields[0], MSG_SS_HAS_FILE) == 0) {
            if (file_msg.field_count < 8) {
                send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Incomplete SS_HAS_FILE message.", ctx->peer_ip, ctx->peer_port);
                protocol_free_message(&file_msg);
                free(file_msg_raw);
                continue;
            }

            const char *filename = file_msg.fields[1];
            const char *owner = (file_msg.field_count >= 3 && file_msg.fields[2]) ? file_msg.fields[2] : "";
            const char *read_acl_csv = (file_msg.field_count >= 4 && file_msg.fields[3]) ? file_msg.fields[3] : "";
            const char *write_acl_csv = (file_msg.field_count >= 5 && file_msg.fields[4]) ? file_msg.fields[4] : "";
            long long size = (file_msg.field_count >= 6 && file_msg.fields[5]) ? atoll(file_msg.fields[5]) : 0;
            long long words = (file_msg.field_count >= 7 && file_msg.fields[6]) ? atoll(file_msg.fields[6]) : 0;
            long long chars = (file_msg.field_count >= 8 && file_msg.fields[7]) ? atoll(file_msg.fields[7]) : 0;
            int has_extended_metadata = file_msg.field_count >= 12;
            time_t created_ts = 0;
            time_t modified_ts = 0;
            time_t last_access_ts = 0;
            const char *last_user_field = "";
            if (has_extended_metadata) {
                if (file_msg.fields[8]) {
                    created_ts = (time_t)atoll(file_msg.fields[8]);
                }
                if (file_msg.fields[9]) {
                    modified_ts = (time_t)atoll(file_msg.fields[9]);
                }
                if (file_msg.fields[10]) {
                    last_access_ts = (time_t)atoll(file_msg.fields[10]);
                }
                if (file_msg.fields[11]) {
                    last_user_field = file_msg.fields[11];
                }
            } else {
                log_message(LOG_DEBUG, "NS", "SS %s:%d sent legacy metadata for '%s'; defaulting timestamps.",
                            ctx->peer_ip, ctx->peer_port, filename);
            }
            if (!validate_filename(filename)) {
                send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid filename received from storage server.", ctx->peer_ip, ctx->peer_port);
                protocol_free_message(&file_msg);
                free(file_msg_raw);
                continue;
            }

            if (owner[0] != '\0' && !validate_username(owner)) {
                send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid owner received from storage server.", ctx->peer_ip, ctx->peer_port);
                protocol_free_message(&file_msg);
                free(file_msg_raw);
                continue;
            }

            pthread_mutex_lock(&ns->state_lock);
            StorageServerInfo *ss_entry = ss;
            if (!ss_entry) {
                ss_entry = find_storage_server_by_sockfd(ns, ctx->conn_fd);
                if (ss_entry) {
                    ctx->ss_info = ss_entry;
                    ss = ss_entry;
                }
            }
            if (!ss_entry) {
                pthread_mutex_unlock(&ns->state_lock);
                send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Unknown storage server during file sync.", ctx->peer_ip, ctx->peer_port);
                protocol_free_message(&file_msg);
                free(file_msg_raw);
                continue;
            }

            time_t now = get_current_time();
            time_t created_value = (has_extended_metadata && created_ts > 0) ? created_ts : now;
            time_t modified_value = (has_extended_metadata && modified_ts > 0) ? modified_ts : now;
            time_t last_access_value = (has_extended_metadata && last_access_ts > 0) ? last_access_ts : now;
            const char *last_user_value = (has_extended_metadata && last_user_field && last_user_field[0])
                                             ? last_user_field
                                             : (owner[0] ? owner : "N/A");
            FileIndexNode *existing = file_index_find(ns, filename);
            if (existing && existing->file_array_index >= 0 && existing->file_array_index < ns->file_count) {
                FileMetadata *file = &ns->files[existing->file_array_index];
                if (owner[0] != '\0') {
                    if (safe_strcpy(file->owner, owner, sizeof(file->owner)) != 0) {
                        pthread_mutex_unlock(&ns->state_lock);
                        send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to record owner metadata.", ctx->peer_ip, ctx->peer_port);
                        protocol_free_message(&file_msg);
                        free(file_msg_raw);
                        continue;
                    }
                }
                safe_strcpy(file->ss_ip, ss_entry->client_ip, sizeof(file->ss_ip));
                file->ss_port = ss_entry->client_port;
                file->created = created_value;
                file->modified = modified_value;
                file->last_access = last_access_value;
                file->size = size;
                file->word_count = words;
                file->char_count = chars;
                file_acl_parse_csv(file->read_access_users, &file->read_access_count, read_acl_csv);
                file_acl_parse_csv(file->write_access_users, &file->write_access_count, write_acl_csv);
                safe_strcpy(file->last_access_user, last_user_value, sizeof(file->last_access_user));
                log_message(LOG_INFO, "NS", "Updated file '%s' location to %s:%d", filename, ss_entry->client_ip, ss_entry->client_port);
                file_cache_store(ns, filename, existing->file_array_index);
            } else {
                if (ns->file_count >= NS_MAX_FILES) {
                    pthread_mutex_unlock(&ns->state_lock);
                    send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Maximum file limit reached.", ctx->peer_ip, ctx->peer_port);
                    protocol_free_message(&file_msg);
                    free(file_msg_raw);
                    continue;
                }

                int new_index = ns->file_count;
                FileMetadata *file = &ns->files[new_index];
                memset(file, 0, sizeof(FileMetadata));
                if (safe_strcpy(file->filename, filename, sizeof(file->filename)) != 0 ||
                    safe_strcpy(file->ss_ip, ss_entry->client_ip, sizeof(file->ss_ip)) != 0) {
                    memset(file, 0, sizeof(FileMetadata));
                    pthread_mutex_unlock(&ns->state_lock);
                    send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to record file metadata.", ctx->peer_ip, ctx->peer_port);
                    protocol_free_message(&file_msg);
                    free(file_msg_raw);
                    continue;
                }

                if (owner[0] != '\0' && safe_strcpy(file->owner, owner, sizeof(file->owner)) != 0) {
                    memset(file, 0, sizeof(FileMetadata));
                    pthread_mutex_unlock(&ns->state_lock);
                    send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to record owner metadata.", ctx->peer_ip, ctx->peer_port);
                    protocol_free_message(&file_msg);
                    free(file_msg_raw);
                    continue;
                }

                file->ss_port = ss_entry->client_port;
                file->created = created_value;
                file->modified = modified_value;
                file->last_access = last_access_value;
                file->size = size;
                file->word_count = words;
                file->char_count = chars;
                file_acl_parse_csv(file->read_access_users, &file->read_access_count, read_acl_csv);
                file_acl_parse_csv(file->write_access_users, &file->write_access_count, write_acl_csv);

                const char *default_access_user = (last_user_value && last_user_value[0]) ? last_user_value
                                                                                         : (file->owner[0] ? file->owner : "N/A");
                safe_strcpy(file->last_access_user, default_access_user, sizeof(file->last_access_user));
                const char *default_modified_user = file->owner[0] ? file->owner : "N/A";
                safe_strcpy(file->last_modified_user, default_modified_user, sizeof(file->last_modified_user));

                if (file_index_insert(ns, filename, new_index) != 0) {
                    memset(file, 0, sizeof(FileMetadata));
                    pthread_mutex_unlock(&ns->state_lock);
                    send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to index file metadata.", ctx->peer_ip, ctx->peer_port);
                    protocol_free_message(&file_msg);
                    free(file_msg_raw);
                    continue;
                }

                ns->file_count++;
                file_cache_store(ns, filename, new_index);
                log_message(LOG_INFO, "NS", "Registered file '%s' at index %d", filename, new_index);
            }

            pthread_mutex_unlock(&ns->state_lock);
        } else if (strcmp(file_msg.fields[0], MSG_SS_FILES_DONE) == 0) {
            log_message(LOG_INFO, "NS", "SS %s:%d file sync complete.", ctx->peer_ip, ctx->peer_port);
            protocol_free_message(&file_msg);
            free(file_msg_raw);
            break;
        } else {
            send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Unknown storage server sync command.", ctx->peer_ip, ctx->peer_port);
        }

        protocol_free_message(&file_msg);
        free(file_msg_raw);
    }

    // Main loop to handle storage server commands
    while (1) {
        char *raw_command = protocol_receive_message(ctx->conn_fd);
        if (!raw_command) {
            log_message(LOG_INFO, "NS", "SS %s:%d disconnected.", ctx->peer_ip, ctx->peer_port);
            storage_server_signal_failure(ss);
            break;
        }

        StorageServerInfo *ss_entry = ss;
        if (!ss_entry) {
            pthread_mutex_lock(&ns->state_lock);
            ss_entry = find_storage_server_by_sockfd(ns, ctx->conn_fd);
            if (ss_entry) {
                ctx->ss_info = ss_entry;
                ss = ss_entry;
            }
            pthread_mutex_unlock(&ns->state_lock);
        }

        int dispatched_to_waiter = 0;
        if (ss_entry) {
            pthread_mutex_lock(&ss_entry->comm_lock);
            if (ss_entry->awaiting_response && !ss_entry->response_ready) {
                if (ss_entry->response_raw) {
                    free(ss_entry->response_raw);
                }
                ss_entry->response_raw = raw_command;
                ss_entry->response_status = 0;
                ss_entry->response_ready = 1;
                ss_entry->awaiting_response = 0;
                pthread_cond_broadcast(&ss_entry->response_cond);
                dispatched_to_waiter = 1;
            }
            pthread_mutex_unlock(&ss_entry->comm_lock);
        }

        if (dispatched_to_waiter) {
            continue;
        }

        ProtocolMessage cmd_msg;
        if (protocol_parse_message(raw_command, &cmd_msg) != 0 || cmd_msg.field_count == 0) {
            send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid storage server command.", ctx->peer_ip, ctx->peer_port);
            free(raw_command);
            continue;
        }

        const char *command = cmd_msg.fields[0];
        log_request("NS", ctx->peer_ip, ctx->peer_port, "0.0.0.0", ns->port, NULL, command);

        if (strcmp(command, MSG_WRITE_COMPLETE) == 0) {
            if (cmd_msg.field_count < 5) {
                log_message(LOG_WARNING, "NS", "WRITE_COMPLETE missing fields from %s:%d", ctx->peer_ip, ctx->peer_port);
            } else {
                const char *filename = cmd_msg.fields[1];
                long long size = atoll(cmd_msg.fields[2]);
                long long words = atoll(cmd_msg.fields[3]);
                long long chars = atoll(cmd_msg.fields[4]);

                int updated = 0;
                pthread_mutex_lock(&ns->state_lock);
                FileIndexNode *node = file_index_find(ns, filename);
                if (node && node->file_array_index >= 0 && node->file_array_index < ns->file_count) {
                    FileMetadata *file = &ns->files[node->file_array_index];
                    file->size = size;
                    file->word_count = words;
                    file->char_count = chars;
                    file->modified = get_current_time();
                    file_cache_store(ns, filename, node->file_array_index);
                    updated = 1;
                } else {
                    log_message(LOG_WARNING, "NS", "WRITE_COMPLETE for unknown file '%s'", filename);
                }
                pthread_mutex_unlock(&ns->state_lock);

                if (updated) {
                    ns_save_metadata(ns);
                    log_message(LOG_INFO, "NS", "Updated stats for '%s' (size=%lld, words=%lld, chars=%lld)",
                                filename, size, words, chars);
                }
            }
        } else {
            log_message(LOG_WARNING, "NS", "Unhandled SS command '%s' from %s:%d", command, ctx->peer_ip, ctx->peer_port);
        }

        protocol_free_message(&cmd_msg);
        free(raw_command);
    }

    if (ss) {
        ss->is_alive = 0;
        ss->sockfd = -1;
    }
}

//handle each connection in a separate thread
static void *connection_thread(void *arg) {
    ConnectionContext *ctx = (ConnectionContext *)arg;
    if (!ctx || !ctx->ns) {
        close_connection(ctx);
        return NULL;
    }

    NameServer *ns = ctx->ns;

    log_message(LOG_INFO, "NS", "Connection accepted from %s:%d", ctx->peer_ip, ctx->peer_port);

    // 1. Read handshake message
    char *raw_message = protocol_receive_message(ctx->conn_fd);
    if (!raw_message) {
        log_message(LOG_WARNING, "NS", "Failed to read handshake from %s:%d", ctx->peer_ip, ctx->peer_port);
        close_connection(ctx);
        return NULL;
    }

    // 2. Parse message
    ProtocolMessage msg;
    if (protocol_parse_message(raw_message, &msg) != 0 || msg.field_count == 0) {
        send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid message format.", ctx->peer_ip, ctx->peer_port);
        free(raw_message);
        close_connection(ctx);
        return NULL;
    }

    log_request("NS", ctx->peer_ip, ctx->peer_port, "0.0.0.0", ns->port, NULL, msg.fields[0]);

    const char *command = msg.fields[0];
    int handled = 0;
    int handshake_success = 0;

    //handle the client
    if (strcmp(command, MSG_HELLO_CLIENT) == 0) {
        handled = 1;
        if (msg.field_count < 2) {
            send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Missing username.", ctx->peer_ip, ctx->peer_port);
        } else if (!validate_username(msg.fields[1])) {
            send_error_and_log(ctx->conn_fd, ERR_USERNAME_INVALID, "Username invalid or already connected.", ctx->peer_ip, ctx->peer_port);
        } else {
            pthread_mutex_lock(&ns->state_lock);
            int reg_result = ns_register_client(ns, msg.fields[1], ctx->conn_fd);
            pthread_mutex_unlock(&ns->state_lock);

            if (reg_result != 0) {
                send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to register client.", ctx->peer_ip, ctx->peer_port);
            } else {
                char welcome[MAX_FIELD_SIZE];
                snprintf(welcome, sizeof(welcome), "Welcome, %s.", msg.fields[1]);
                const char *fields[] = {RESP_OK, welcome};
                char *resp = protocol_build_message(fields, 2);
                if (resp) {
                    protocol_send_message(ctx->conn_fd, resp);
                    free(resp);
                }
                handshake_success = 1;
                ctx->is_client = 1;
                ctx->is_storage_server = 0;
                ctx->username[0] = '\0';
                strncpy(ctx->username, msg.fields[1], sizeof(ctx->username) - 1);
                ctx->username[sizeof(ctx->username) - 1] = '\0';
                log_message(LOG_INFO, "NS", "Client %s registered from %s:%d", ctx->username, ctx->peer_ip, ctx->peer_port);
            }
        }
    } 
    //handle the storage server
    else if (strcmp(command, MSG_HELLO_SS) == 0) {
        handled = 1;
        if (msg.field_count < 5) {
            send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Missing storage server parameters.", ctx->peer_ip, ctx->peer_port);
        } else if (!is_valid_ip(msg.fields[1]) || !is_valid_port(atoi(msg.fields[2])) ||
                   !is_valid_ip(msg.fields[3]) || !is_valid_port(atoi(msg.fields[4]))) {
            send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid storage server address info.", ctx->peer_ip, ctx->peer_port);
        } else {
            int ns_port = atoi(msg.fields[2]);
            int client_port = atoi(msg.fields[4]);
            pthread_mutex_lock(&ns->state_lock);
            int reg_result = ns_register_storage_server(ns, msg.fields[1], ns_port, msg.fields[3], client_port, ctx->conn_fd);
            StorageServerInfo *registered_ss = NULL;
            if (reg_result == 0) {
                registered_ss = find_storage_server_by_sockfd(ns, ctx->conn_fd);
            }
            pthread_mutex_unlock(&ns->state_lock);

            if (reg_result != 0) {
                send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to register storage server.", ctx->peer_ip, ctx->peer_port);
            } else {
                const char *fields[] = {RESP_OK, "SS registered. Awaiting file list."};
                char *resp = protocol_build_message(fields, 2);
                if (resp) {
                    protocol_send_message(ctx->conn_fd, resp);
                    free(resp);
                }
                handshake_success = 1;
                ctx->is_client = 0;
                ctx->is_storage_server = 1;
                ctx->username[0] = '\0';
                ctx->ss_info = registered_ss;
                log_message(LOG_INFO, "NS", "Storage server %s:%d registered (client %s:%d)", msg.fields[1], ns_port, msg.fields[3], client_port);
            }
        }
    }

    if (!handled) {
        send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Unknown command.", ctx->peer_ip, ctx->peer_port);
    }

    protocol_free_message(&msg);
    free(raw_message);

    if (handshake_success && ctx->is_client) {
        log_message(LOG_INFO, "NS", "Client %s handshake complete. Entering command loop.", ctx->username);
        run_client_loop(ctx);
    } else if (handshake_success && ctx->is_storage_server) {
        log_message(LOG_INFO, "NS", "Storage server %s:%d handshake complete. Entering sync loop.", ctx->peer_ip, ctx->peer_port);
        run_ss_loop(ctx);
    } else {
        log_message(LOG_INFO, "NS", "Connection with %s:%d finished without persistent session.", ctx->peer_ip, ctx->peer_port);
    }

    pthread_mutex_lock(&ns->state_lock);
    if (ctx->is_client && ctx->username[0]) {
        for (int i = 0; i < ns->client_count; i++) {
            ClientInfo *client = &ns->clients[i];
            if (strcmp(client->username, ctx->username) == 0) {
                client->is_connected = 0;
                client->sockfd = -1;
                log_message(LOG_INFO, "NS", "Client %s disconnected (session kept)", ctx->username);
                break;
            }
        }
    }

    StorageServerInfo *ss_to_free = NULL;
    if (ctx->is_storage_server) {
        for (int i = 0; i < ns->ss_count; i++) {
            StorageServerInfo *ss = ns->storage_servers[i];
            if (!ss) {
                continue;
            }
            if (ss->sockfd == ctx->conn_fd) {
                int last_index = ns->ss_count - 1;
                ss_to_free = ss;
                if (i != last_index) {
                    ns->storage_servers[i] = ns->storage_servers[last_index];
                }
                ns->storage_servers[last_index] = NULL;
                ns->ss_count--;
                log_message(LOG_INFO, "NS", "Storage server %s:%d deregistered", ctx->peer_ip, ctx->peer_port);
                break;
            }
        }
    }
    pthread_mutex_unlock(&ns->state_lock);

    if (ss_to_free) {
        storage_server_signal_failure(ss_to_free);
        pthread_mutex_lock(&ss_to_free->comm_lock);
        while (ss_to_free->refcount > 0) {
            pthread_cond_wait(&ss_to_free->response_cond, &ss_to_free->comm_lock);
        }
        pthread_mutex_unlock(&ss_to_free->comm_lock);
        storage_server_comm_destroy(ss_to_free);
        free(ss_to_free);
    }

    log_message(LOG_INFO, "NS", "Connection closed for %s:%d", ctx->peer_ip, ctx->peer_port);
    close_connection(ctx);
    return NULL;
}

int ns_start(NameServer *ns) {
    if (!ns) {
        return -1;
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0); //ipv4 and tcp
    if (listen_fd < 0) {
        log_message(LOG_ERROR, "NS", "Failed to create socket: %s", strerror(errno));
        return -1;
    }

    // Enable the socket to reuse the address (port) immediately after the server restarts
    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        log_message(LOG_WARNING, "NS", "setsockopt failed: %s", strerror(errno));
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(ns->port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { //binding the socket 
        log_message(LOG_ERROR, "NS", "Bind failed on port %d: %s", ns->port, strerror(errno));
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, SOMAXCONN) < 0) { //one time setup to keep adding connections in the connection queue
        log_message(LOG_ERROR, "NS", "Listen failed: %s", strerror(errno));
        close(listen_fd);
        return -1;
    }


    ns->sockfd = listen_fd;
    log_message(LOG_INFO, "NS", "Name Server listening on port %d", ns->port);

    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        log_message(LOG_ERROR, "NS", "Failed to init thread attributes: %s", strerror(errno));
    }

    size_t stack_size = 4 * 1024 * 1024; 
    if (pthread_attr_setstacksize(&attr, stack_size) != 0) {
        log_message(LOG_ERROR, "NS", "Failed to set stack size: %s", strerror(errno));
    }
    while (1) {
        struct sockaddr_in peer_addr;
        socklen_t peer_len = sizeof(peer_addr);
        int conn_fd = accept(listen_fd, (struct sockaddr *)&peer_addr, &peer_len); //accepting connection
        if (conn_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_message(LOG_WARNING, "NS", "Accept failed: %s", strerror(errno));
            continue;
        }

        char peer_ip[INET_ADDRSTRLEN] = "unknown";
        if (!inet_ntop(AF_INET, &peer_addr.sin_addr, peer_ip, sizeof(peer_ip))) {
            strncpy(peer_ip, "unknown", sizeof(peer_ip) - 1);
            peer_ip[sizeof(peer_ip) - 1] = '\0';
        }
        int peer_port = ntohs(peer_addr.sin_port);

        log_message(LOG_INFO, "NS", "Connection accepted from %s:%d", peer_ip, peer_port);
        ConnectionContext *ctx = (ConnectionContext *)malloc(sizeof(ConnectionContext));
        if (!ctx) {
            log_message(LOG_ERROR, "NS", "Failed to allocate thread context for %s:%d", peer_ip, peer_port);
            close(conn_fd);
            continue;
        }

        //Initialize the connection context
        ctx->ns = ns;
        ctx->conn_fd = conn_fd;
        strncpy(ctx->peer_ip, peer_ip, sizeof(ctx->peer_ip) - 1);
        ctx->peer_ip[sizeof(ctx->peer_ip) - 1] = '\0';
        ctx->peer_port = peer_port;
        ctx->is_client = 0;
        ctx->is_storage_server = 0;
        ctx->username[0] = '\0';
    ctx->ss_info = NULL;

        pthread_t thread_id;
        //call the connection_thread function to handle the connection and detach subsequently
        if (pthread_create(&thread_id, &attr, connection_thread, ctx) != 0) {
            log_message(LOG_ERROR, "NS", "Failed to spawn thread for %s:%d: %s", peer_ip, peer_port, strerror(errno));
            close(conn_fd);
            free(ctx);
            continue;
        }

        pthread_detach(thread_id); //the main thread does not wait for this thread to finish
    }

    return 0;
}

int ns_register_storage_server(NameServer *ns, const char *ns_ip, int ns_port,
                                const char *client_ip, int client_port, int sockfd) {
    if (!ns || !ns_ip || !client_ip) {
        return -1;
    }
    
    if (ns->ss_count >= NS_MAX_STORAGE_SERVERS) {
        log_message(LOG_ERROR, "NS", "Maximum storage servers reached");
        return -1;
    }

    for (int i = 0; i < ns->ss_count; i++) {
        StorageServerInfo *info = ns->storage_servers[i];
        if (!info) {
            continue;
        }
        if (info->is_alive && strcmp(info->client_ip, client_ip) == 0 && info->client_port == client_port) {
            log_message(LOG_WARNING, "NS", "Storage server %s:%d already registered", client_ip, client_port);
            return -1;
        }
    }

    StorageServerInfo *ss = (StorageServerInfo *)calloc(1, sizeof(StorageServerInfo));
    if (!ss) {
        log_message(LOG_ERROR, "NS", "Failed to allocate storage server entry");
        return -1;
    }
    strncpy(ss->ns_ip, ns_ip, sizeof(ss->ns_ip) - 1);
    ss->ns_ip[sizeof(ss->ns_ip) - 1] = '\0';
    ss->ns_port = ns_port;
    strncpy(ss->client_ip, client_ip, sizeof(ss->client_ip) - 1);
    ss->client_ip[sizeof(ss->client_ip) - 1] = '\0';
    ss->client_port = client_port;
    ss->sockfd = sockfd;
    ss->is_alive = 1;
    ss->last_heartbeat = get_current_time();
    storage_server_comm_init(ss);

    ns->storage_servers[ns->ss_count++] = ss;

    log_message(LOG_INFO, "NS", "Registered storage server: %s:%d", client_ip, client_port);
    return 0;
}

int ns_register_client(NameServer *ns, const char *username, int sockfd) {
    if (!ns || !username) {
        return -1;
    }
    
    int existing_index = -1;
    for (int i = 0; i < ns->client_count; i++) {
        if (strcmp(ns->clients[i].username, username) == 0) {
            if (ns->clients[i].is_connected) {
                log_message(LOG_WARNING, "NS", "Client %s already connected", username);
                return -1;
            }
            existing_index = i;
            break;
        }
    }

    ClientInfo *client = NULL;
    if (existing_index >= 0) {
        client = &ns->clients[existing_index];
        log_message(LOG_INFO, "NS", "Reactivating existing client: %s", username);
    } else {
        if (ns->client_count >= NS_MAX_CLIENTS) {
            log_message(LOG_ERROR, "NS", "Maximum clients reached");
            return -1;
        }
        client = &ns->clients[ns->client_count++];
        memset(client, 0, sizeof(ClientInfo));
        strncpy(client->username, username, sizeof(client->username) - 1);
        client->username[sizeof(client->username) - 1] = '\0';
        log_message(LOG_INFO, "NS", "Registered client: %s", username);
    }

    client->sockfd = sockfd;
    client->is_connected = 1;
    return 0;
}

int ns_find_storage_server(NameServer *ns, const char *filename) {
    if (!ns || !filename) {
        return -1;
    }

    int ss_index = -1;

    // Protect shared metadata structures while resolving the filename → storage server mapping
    pthread_mutex_lock(&ns->state_lock);

    // Step 1: Resolve the filename to its metadata entry using the cache-aware helper
    int file_index = -1;
    FileMetadata *file = file_lookup_locked(ns, filename, &file_index);
    if (!file) {
        pthread_mutex_unlock(&ns->state_lock);
        return -1;
    }

    // Step 2: Find the live storage server whose client-facing address matches the file location
    for (int i = 0; i < ns->ss_count; i++) {
        StorageServerInfo *ss = ns->storage_servers[i];
        if (!ss || !ss->is_alive) {
            continue;
        }
        if (strncmp(ss->client_ip, file->ss_ip, MAX_IP_LENGTH) == 0 && ss->client_port == file->ss_port) {
            ss_index = i;
            break;
        }
    }

    pthread_mutex_unlock(&ns->state_lock);
    return ss_index;
}

void ns_cleanup(NameServer *ns) {
    if (!ns) {
        return;
    }
    
    if (ns->sockfd >= 0) {
        close(ns->sockfd);
    }
    
    // Close all client and SS connections
    for (int i = 0; i < ns->client_count; i++) {
        if (ns->clients[i].sockfd >= 0) {
            close(ns->clients[i].sockfd);
        }
    }
    
    for (int i = 0; i < ns->ss_count; i++) {
        StorageServerInfo *ss = ns->storage_servers[i];
        if (!ss) {
            continue;
        }
        if (ss->sockfd >= 0) {
            close(ss->sockfd);
        }
        storage_server_comm_destroy(ss);
        free(ss);
        ns->storage_servers[i] = NULL;
    }

    for (int i = 0; i < FILE_INDEX_SIZE; i++) {
        FileIndexNode *node = ns->file_index[i];
        while (node) {
            FileIndexNode *next = node->next;
            free(node);
            node = next;
        }
        ns->file_index[i] = NULL;
    }
    
    pthread_mutex_destroy(&ns->state_lock);
    
    log_message(LOG_INFO, "NS", "Name Server cleanup complete");
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }
    
    int port = atoi(argv[1]);
    if (!is_valid_port(port)) {
        fprintf(stderr, "Invalid port number\n");
        return 1;
    }
    
    // Initialize logging
    log_init("name_server.log", LOG_INFO);
    
    // Create Name Server structure on the heap to avoid stack overflows
    NameServer *ns = (NameServer *)calloc(1, sizeof(NameServer));
    if (!ns) {
        fprintf(stderr, "Failed to allocate Name Server\n");
        return 1;
    }

    if (ns_init(ns, port) != 0) {
        fprintf(stderr, "Failed to initialize Name Server\n");
        free(ns);
        return 1;
    }
    
    // Start server
    printf("Name Server starting on port %d...\n", port);
    if (ns_start(ns) != 0) {
        fprintf(stderr, "Server error\n");
        ns_cleanup(ns);
        free(ns);
        return 1;
    }
    
    // Cleanup
    ns_cleanup(ns);
    free(ns);
    log_cleanup();
    
    return 0;
}
