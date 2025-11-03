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
    memset(ns->file_index, 0, sizeof(ns->file_index));
    memset(ns->file_cache, 0, sizeof(ns->file_cache));
    ns->cache_tick = 0;
    if (pthread_mutex_init(&ns->state_lock, NULL) != 0) {
        log_message(LOG_ERROR, "NS", "Failed to initialize mutex: %s", strerror(errno));
        return -1;
    }
    
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
        if (ns->storage_servers[i].sockfd == sockfd) {
            return &ns->storage_servers[i];
        }
    }
    return NULL;
}

//LRU file cache lookup
static int file_cache_lookup(NameServer *ns, const char *filename, FileMetadata *out_metadata, int *out_index) {
    if (!ns || !filename || !out_metadata) {
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

            *out_metadata = ns->files[entry->file_array_index];
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

static int file_acl_contains(char entries[][MAX_USERNAME_LENGTH], int count, const char *username) {
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
            // TODO: Handle LIST_USERS
        } else if (strcmp(command, MSG_CREATE) == 0) {
            // Create flow: validate request, register metadata locally, defer physical creation to storage server layer
            if (cmd_msg.field_count < 2) {
                send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Filename missing in CREATE.", ctx->peer_ip, ctx->peer_port);
            } else {
                const char *filename = cmd_msg.fields[1];

                if (!validate_filename(filename)) {
                    send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid filename requested for CREATE.", ctx->peer_ip, ctx->peer_port);
                } else if (ctx->username[0] == '\0') {
                    send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Client identity unknown.", ctx->peer_ip, ctx->peer_port);
                } else {
                    int result = -1;
                    pthread_mutex_lock(&ns->state_lock);

                    if (ns->file_count >= NS_MAX_FILES) {
                        pthread_mutex_unlock(&ns->state_lock);
                        send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Maximum file limit reached.", ctx->peer_ip, ctx->peer_port);
                    } else if (file_index_find(ns, filename) != NULL) {
                        pthread_mutex_unlock(&ns->state_lock);
                        send_error_and_log(ctx->conn_fd, ERR_FILE_EXISTS, "File already exists.", ctx->peer_ip, ctx->peer_port);
                    } else if (ns->ss_count == 0) {
                        pthread_mutex_unlock(&ns->state_lock);
                        send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "No storage servers available.", ctx->peer_ip, ctx->peer_port);
                    } else {
                        // TODO: load-balance across storage servers; currently pick the first registered node
                        StorageServerInfo *target_ss = &ns->storage_servers[0];
                        int new_index = ns->file_count;
                        FileMetadata *file = &ns->files[new_index];
                        memset(file, 0, sizeof(FileMetadata));

                        if (safe_strcpy(file->filename, filename, sizeof(file->filename)) != 0 ||
                            safe_strcpy(file->owner, ctx->username, sizeof(file->owner)) != 0 ||
                            safe_strcpy(file->ss_ip, target_ss->client_ip, sizeof(file->ss_ip)) != 0) {
                            memset(file, 0, sizeof(FileMetadata));
                            pthread_mutex_unlock(&ns->state_lock);
                            send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to record file metadata.", ctx->peer_ip, ctx->peer_port);
                        } else {
                            file->ss_port = target_ss->client_port;
                            time_t now = get_current_time();
                            file->created = now;
                            file->modified = now;
                            file->read_access_count = 0;
                            file->write_access_count = 0;

                            if (file_index_insert(ns, filename, new_index) != 0) {
                                memset(file, 0, sizeof(FileMetadata));
                                pthread_mutex_unlock(&ns->state_lock);
                                send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to index new file.", ctx->peer_ip, ctx->peer_port);
                            } else {
                                ns->file_count++;
                                file_cache_store(ns, filename, new_index);
                                result = 0;
                                pthread_mutex_unlock(&ns->state_lock);

                                if (result == 0) {
                                    char detail[MAX_FIELD_SIZE];
                                    snprintf(detail, sizeof(detail), "File '%s' registered to %s:%d.", filename, target_ss->client_ip, target_ss->client_port);
                                    char *resp = protocol_build_ok(detail);
                                    if (resp) {
                                        protocol_send_message(ctx->conn_fd, resp);
                                        free(resp);
                                    } else {
                                        send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to send OK response.", ctx->peer_ip, ctx->peer_port);
                                    }
                                    log_message(LOG_INFO, "NS", "File '%s' created by %s.", filename, ctx->username);
                                }
                            }
                        }
                    }
                }
            }
        } else if (strcmp(command, MSG_REQ_LOC) == 0) {
            if (cmd_msg.field_count < 2) {
                send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Filename missing in location request.", ctx->peer_ip, ctx->peer_port);
            } else {
                const char *filename = cmd_msg.fields[1];
                if (!validate_filename(filename)) {
                    send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid filename requested.", ctx->peer_ip, ctx->peer_port);
                } else {
                    FileMetadata file_copy;
                    int found = 0;

                    pthread_mutex_lock(&ns->state_lock);
                    int file_index = -1;
                    if (file_cache_lookup(ns, filename, &file_copy, &file_index)) {
                        found = 1;
                    } else {
                        FileIndexNode *node = file_index_find(ns, filename);
                        if (node && node->file_array_index >= 0 && node->file_array_index < ns->file_count) {
                            file_index = node->file_array_index;
                            file_copy = ns->files[file_index];
                            file_cache_store(ns, filename, file_index);
                            found = 1;
                        }
                    }
                    pthread_mutex_unlock(&ns->state_lock);

                    if (found) {
                        char port_buf[16];
                        snprintf(port_buf, sizeof(port_buf), "%d", file_copy.ss_port);
                        const char *resp_fields[] = {RESP_OK_LOC, filename, file_copy.ss_ip, port_buf};
                        char *resp = protocol_build_message(resp_fields, 4);
                        if (resp) {
                            protocol_send_message(ctx->conn_fd, resp);
                            free(resp);
                        } else {
                            send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to build OK_LOC response.", ctx->peer_ip, ctx->peer_port);
                        }
                    } else {
                        send_error_and_log(ctx->conn_fd, ERR_FILE_NOT_FOUND, "Requested file not found.", ctx->peer_ip, ctx->peer_port);
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
                    int result = -1;
                    pthread_mutex_lock(&ns->state_lock);
                    FileIndexNode *node = file_index_find(ns, filename);
                    if (!node || node->file_array_index < 0 || node->file_array_index >= ns->file_count) {
                        pthread_mutex_unlock(&ns->state_lock);
                        send_error_and_log(ctx->conn_fd, ERR_FILE_NOT_FOUND, "File not found for ADDACCESS.", ctx->peer_ip, ctx->peer_port);
                    } else {
                        FileMetadata *file = &ns->files[node->file_array_index];
                        if (strncmp(file->owner, ctx->username, MAX_USERNAME_LENGTH) != 0) {
                            pthread_mutex_unlock(&ns->state_lock);
                            send_error_and_log(ctx->conn_fd, ERR_PERMISSION_DENIED, "Only the owner may modify access.", ctx->peer_ip, ctx->peer_port);
                        } else {
                            result = file_add_access(file, target_user, grant_read, grant_write);
                            pthread_mutex_unlock(&ns->state_lock);

                            if (result != 0) {
                                send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to update ACL.", ctx->peer_ip, ctx->peer_port);
                            } else {
                                char detail[MAX_FIELD_SIZE];
                                snprintf(detail, sizeof(detail), "Access granted to %s (%s).", target_user, permission);
                                char *resp = protocol_build_ok(detail);
                                if (resp) {
                                    protocol_send_message(ctx->conn_fd, resp);
                                    free(resp);
                                } else {
                                    send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to send OK response.", ctx->peer_ip, ctx->peer_port);
                                }
                            }
                        }
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
                    int removed = 0;
                    pthread_mutex_lock(&ns->state_lock);
                    FileIndexNode *node = file_index_find(ns, filename);
                    if (!node || node->file_array_index < 0 || node->file_array_index >= ns->file_count) {
                        pthread_mutex_unlock(&ns->state_lock);
                        send_error_and_log(ctx->conn_fd, ERR_FILE_NOT_FOUND, "File not found for REMACCESS.", ctx->peer_ip, ctx->peer_port);
                    } else {
                        FileMetadata *file = &ns->files[node->file_array_index];
                        if (strncmp(file->owner, ctx->username, MAX_USERNAME_LENGTH) != 0) {
                            pthread_mutex_unlock(&ns->state_lock);
                            send_error_and_log(ctx->conn_fd, ERR_PERMISSION_DENIED, "Only the owner may modify access.", ctx->peer_ip, ctx->peer_port);
                        } else {
                            removed = file_remove_access(file, target_user);
                            pthread_mutex_unlock(&ns->state_lock);

                            if (!removed) {
                                send_error_and_log(ctx->conn_fd, ERR_USER_NOT_FOUND, "User had no explicit access.", ctx->peer_ip, ctx->peer_port);
                            } else {
                                char detail[MAX_FIELD_SIZE];
                                snprintf(detail, sizeof(detail), "Access revoked for %s.", target_user);
                                char *resp = protocol_build_ok(detail);
                                if (resp) {
                                    protocol_send_message(ctx->conn_fd, resp);
                                    free(resp);
                                } else {
                                    send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to send OK response.", ctx->peer_ip, ctx->peer_port);
                                }
                            }
                        }
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

    // Get storage server information
    while (1) {
        char *file_msg_raw = protocol_receive_message(ctx->conn_fd);
        if (!file_msg_raw) {
            log_message(LOG_WARNING, "NS", "SS %s:%d disconnected during file sync.", ctx->peer_ip, ctx->peer_port);
            return;
        }

        ProtocolMessage file_msg;
        if (protocol_parse_message(file_msg_raw, &file_msg) != 0 || file_msg.field_count == 0) {
            send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid file sync message.", ctx->peer_ip, ctx->peer_port);
            free(file_msg_raw);
            continue;
        }

        if (strcmp(file_msg.fields[0], MSG_SS_HAS_FILE) == 0) {
            if (file_msg.field_count < 2) {
                send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Filename missing in SS_HAS_FILE.", ctx->peer_ip, ctx->peer_port);
                protocol_free_message(&file_msg);
                free(file_msg_raw);
                continue;
            }

            const char *filename = file_msg.fields[1];
            if (!validate_filename(filename)) {
                send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid filename received from storage server.", ctx->peer_ip, ctx->peer_port);
                protocol_free_message(&file_msg);
                free(file_msg_raw);
                continue;
            }

            pthread_mutex_lock(&ns->state_lock);
            StorageServerInfo *ss = find_storage_server_by_sockfd(ns, ctx->conn_fd);
            if (!ss) {
                pthread_mutex_unlock(&ns->state_lock);
                send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Unknown storage server during file sync.", ctx->peer_ip, ctx->peer_port);
                protocol_free_message(&file_msg);
                free(file_msg_raw);
                continue;
            }

            time_t now = get_current_time();
            FileIndexNode *existing = file_index_find(ns, filename);
            if (existing && existing->file_array_index >= 0 && existing->file_array_index < ns->file_count) {
                FileMetadata *file = &ns->files[existing->file_array_index];
                safe_strcpy(file->ss_ip, ss->client_ip, sizeof(file->ss_ip));
                file->ss_port = ss->client_port;
                file->modified = now;
                log_message(LOG_INFO, "NS", "Updated file '%s' location to %s:%d", filename, ss->client_ip, ss->client_port);
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
                safe_strcpy(file->filename, filename, sizeof(file->filename));
                file->owner[0] = '\0';
                safe_strcpy(file->ss_ip, ss->client_ip, sizeof(file->ss_ip));
                file->ss_port = ss->client_port;
                file->created = now;
                file->modified = now;

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
            break;
        }

        ProtocolMessage cmd_msg;
        if (protocol_parse_message(raw_command, &cmd_msg) != 0 || cmd_msg.field_count == 0) {
            send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid storage server command.", ctx->peer_ip, ctx->peer_port);
            free(raw_command);
            continue;
        }

        const char *command = cmd_msg.fields[0];
        log_request("NS", ctx->peer_ip, ctx->peer_port, "0.0.0.0", ns->port, NULL, command);

        // TODO: Handle storage server commands (e.g., heartbeats, write notifications)

        protocol_free_message(&cmd_msg);
        free(raw_command);
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
            if (client->is_connected && strcmp(client->username, ctx->username) == 0) {
                int last_index = ns->client_count - 1;
                if (i != last_index) {
                    ns->clients[i] = ns->clients[last_index];
                }
                memset(&ns->clients[last_index], 0, sizeof(ClientInfo));
                ns->client_count--;
                log_message(LOG_INFO, "NS", "Client %s deregistered", ctx->username);
                break;
            }
        }
    } else if (ctx->is_storage_server) {
        for (int i = 0; i < ns->ss_count; i++) {
            StorageServerInfo *ss = &ns->storage_servers[i];
            if (ss->sockfd == ctx->conn_fd) {
                int last_index = ns->ss_count - 1;
                if (i != last_index) {
                    ns->storage_servers[i] = ns->storage_servers[last_index];
                }
                memset(&ns->storage_servers[last_index], 0, sizeof(StorageServerInfo));
                ns->ss_count--;
                log_message(LOG_INFO, "NS", "Storage server %s:%d deregistered", ctx->peer_ip, ctx->peer_port);
                break;
            }
        }
    }
    pthread_mutex_unlock(&ns->state_lock);

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

        pthread_t thread_id;
        //call the connection_thread function to handle the connection and detach subsequently
        if (pthread_create(&thread_id, NULL, connection_thread, ctx) != 0) {
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
        StorageServerInfo *info = &ns->storage_servers[i];
        if (info->is_alive && strcmp(info->ns_ip, ns_ip) == 0 && info->ns_port == ns_port) {
            log_message(LOG_WARNING, "NS", "Storage server %s:%d already registered", ns_ip, ns_port);
            return -1;
        }
    }

    StorageServerInfo *ss = &ns->storage_servers[ns->ss_count];
    memset(ss, 0, sizeof(StorageServerInfo));
    strncpy(ss->ns_ip, ns_ip, sizeof(ss->ns_ip) - 1);
    ss->ns_ip[sizeof(ss->ns_ip) - 1] = '\0';
    ss->ns_port = ns_port;
    strncpy(ss->client_ip, client_ip, sizeof(ss->client_ip) - 1);
    ss->client_ip[sizeof(ss->client_ip) - 1] = '\0';
    ss->client_port = client_port;
    ss->sockfd = sockfd;
    ss->is_alive = 1;
    ss->last_heartbeat = get_current_time();

    ns->ss_count++;

    log_message(LOG_INFO, "NS", "Registered storage server: %s:%d", client_ip, client_port);
    return 0;
}

int ns_register_client(NameServer *ns, const char *username, int sockfd) {
    if (!ns || !username) {
        return -1;
    }
    
    if (ns->client_count >= NS_MAX_CLIENTS) {
        log_message(LOG_ERROR, "NS", "Maximum clients reached");
        return -1;
    }

    for (int i = 0; i < ns->client_count; i++) {
        if (ns->clients[i].is_connected && strcmp(ns->clients[i].username, username) == 0) {
            log_message(LOG_WARNING, "NS", "Client %s already connected", username);
            return -1;
        }
    }

    ClientInfo *client = &ns->clients[ns->client_count];
    memset(client, 0, sizeof(ClientInfo));
    strncpy(client->username, username, sizeof(client->username) - 1);
    client->username[sizeof(client->username) - 1] = '\0';
    client->sockfd = sockfd;
    client->is_connected = 1;

    ns->client_count++;

    log_message(LOG_INFO, "NS", "Registered client: %s", username);
    return 0;
}

int ns_find_storage_server(NameServer *ns, const char *filename) {
    if (!ns || !filename) {
        return -1;
    }
    
    // TODO: Implement file location lookup
    // 1. Search file metadata
    // 2. Return SS index
    
    return -1;
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
        if (ns->storage_servers[i].sockfd >= 0) {
            close(ns->storage_servers[i].sockfd);
        }
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
    
    // Create Name Server
    NameServer ns;
    if (ns_init(&ns, port) != 0) {
        fprintf(stderr, "Failed to initialize Name Server\n");
        return 1;
    }
    
    // Start server
    printf("Name Server starting on port %d...\n", port);
    if (ns_start(&ns) != 0) {
        fprintf(stderr, "Server error\n");
        ns_cleanup(&ns);
        return 1;
    }
    
    // Cleanup
    ns_cleanup(&ns);
    log_cleanup();
    
    return 0;
}
