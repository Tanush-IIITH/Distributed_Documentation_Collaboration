#include "storage_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

static void ss_free_file_records(FileRecord *head);
static void ss_load_existing_files(StorageServer *ss);
static int ss_open_ns_socket(StorageServer *ss);
static int ss_send_hello(StorageServer *ss);
static int ss_emit_file_list(StorageServer *ss);
static int ss_start_ns_thread(StorageServer *ss);
static int ss_open_client_listener(StorageServer *ss);
static void ss_send_ns_error(StorageServer *ss, int code, const char *message);
static int ss_create_file(StorageServer *ss, const char *filename, const char *owner);
static int ss_delete_file(StorageServer *ss, const char *filename);
static int ss_handle_rename(StorageServer *ss, const char *old_logical, const char *new_logical,
                            const char *old_flat, const char *new_flat);
static void ss_set_record_paths(StorageServer *ss, FileRecord *rec, const char *logical_name);
static int ss_load_metadata(StorageServer *ss, FileRecord *rec);
static int ss_write_metadata(FileRecord *rec);
static char *ss_read_file_content(const char *path);
static int ss_write_file_content(const char *path, const char *data);
static int ss_check_read_access(FileRecord *rec, const char *user);
static int ss_check_write_access(FileRecord *rec, const char *user);
static int ss_acl_contains(const char entries[][MAX_USERNAME_LENGTH], int count, const char *username);
static int ss_acl_add(char entries[][MAX_USERNAME_LENGTH], int *count, const char *username);
static int ss_acl_remove(char entries[][MAX_USERNAME_LENGTH], int *count, const char *username);
static int ss_join_acl(const char entries[][MAX_USERNAME_LENGTH], int count, char *buffer, size_t buffer_size);
static int ss_get_sentence_bounds(const char *text, int sentence_index,
                                  size_t *start_out, size_t *end_out);
static int ss_apply_word_edit(WriteSession *session, int word_index, const char *content);
static WriteSession *ss_find_session_by_sentence(StorageServer *ss, FileRecord *rec, int sentence_index);
static int ss_file_has_active_writers(StorageServer *ss, FileRecord *rec);
static int ss_file_exists(const char *path);
static int ss_copy_file(const char *src_path, const char *dest_path);
static FILE *ss_open_truncating_binary(const char *path);
static void ss_sanitize_tag(const char *tag, char *buffer, size_t size);
static CheckpointRecord *ss_find_checkpoint(FileRecord *rec, const char *tag);
static void ss_prune_oldest_checkpoint(FileRecord *rec);
static void handle_client_checkpoint(StorageServer *ss, int fd, ProtocolMessage *msg);
static void handle_client_viewcheckpoint(StorageServer *ss, int fd, ProtocolMessage *msg);
static void handle_client_revert(StorageServer *ss, int fd, ProtocolMessage *msg);
static void handle_client_listcheckpoints(StorageServer *ss, int fd, ProtocolMessage *msg);
static void handle_ss_copy(StorageServer *ss, ProtocolMessage *msg);

typedef struct {
    StorageServer *ss;
} NsThreadArgs;

typedef struct {
    StorageServer *ss;
    int client_fd;
    char client_ip[INET_ADDRSTRLEN];
    int client_port;
} ClientThreadArgs;

static void ss_free_file_records(FileRecord *head) {
    FileRecord *node = head;
    while (node) {
        FileRecord *next = node->next;
        pthread_mutex_destroy(&node->file_lock);
        free(node);
        node = next;
    }
}

static int build_path(char *buffer, size_t size, const char *base, const char *file) {
    if (!buffer || !base || !file) {
        return -1;
    }

    if (snprintf(buffer, size, "%s/%s", base, file) >= (int)size) {
        return -1;
    }

    return 0;
}

static int ss_build_storage_path(char *buffer, size_t size, const char *client_ip, int client_port) {
    if (!buffer || size == 0) {
        return -1;
    }

    const char *ip_source = (client_ip && client_ip[0]) ? client_ip : "local";
    char sanitized_ip[MAX_IP_LENGTH];
    if (safe_strcpy(sanitized_ip, ip_source, sizeof(sanitized_ip)) != 0) {
        return -1;
    }

    for (size_t i = 0; sanitized_ip[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)sanitized_ip[i];
        if (!(isalnum(ch) || ch == '_')) {
            sanitized_ip[i] = '_';
        }
    }

    if (sanitized_ip[0] == '\0') {
        safe_strcpy(sanitized_ip, "local", sizeof(sanitized_ip));
    }

    if (client_port < 0) {
        client_port = 0;
    }

    int written = snprintf(buffer, size, "%sserver_%s_%d/", SS_STORAGE_PATH, sanitized_ip, client_port);
    if (written < 0 || (size_t)written >= size) {
        return -1;
    }

    return 0;
}

static int dirent_is_directory(const char *base_path, const struct dirent *entry) {
    if (!base_path || !entry) {
        return 0;
    }

#if defined(_DIRENT_HAVE_D_TYPE) && defined(DT_DIR)
    if (entry->d_type == DT_DIR) {
        return 1;
    }
#endif

#if defined(_DIRENT_HAVE_D_TYPE) && defined(DT_UNKNOWN)
    if (entry->d_type != DT_UNKNOWN) {
        return 0;
    }
#endif

    char full_path[MAX_PATH_LENGTH];
    if (build_path(full_path, sizeof(full_path), base_path, entry->d_name) != 0) {
        return 0;
    }

    struct stat st;
    if (stat(full_path, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    return 0;
}

static int ss_file_exists(const char *path) {
    if (!path || !path[0]) {
        return 0;
    }
    struct stat st;
    return stat(path, &st) == 0;
}

static FILE *ss_open_truncating_binary(const char *path) {
    if (!path || !path[0]) {
        errno = EINVAL;
        return NULL;
    }

    return fopen(path, "wb");
}

static int ss_copy_file(const char *src_path, const char *dest_path) {
    if (!src_path || !dest_path) {
        return -1;
    }

    FILE *src = fopen(src_path, "rb");
    if (!src) {
        return -1;
    }

    FILE *dest = ss_open_truncating_binary(dest_path);
    if (!dest) {
        fclose(src);
        return -1;
    }

    char buffer[4096];
    size_t bytes_read;
    int rc = 0;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        if (fwrite(buffer, 1, bytes_read, dest) != bytes_read) {
            rc = -1;
            break;
        }
    }

    if (ferror(src)) {
        rc = -1;
    }

    fclose(src);
    fclose(dest);

    if (rc != 0) {
        unlink(dest_path);
    }

    return rc;
}

static void ss_sanitize_tag(const char *tag, char *buffer, size_t size) {
    if (!buffer || size == 0) {
        return;
    }

    buffer[0] = '\0';
    if (!tag) {
        return;
    }

    size_t out_idx = 0;
    for (const char *ptr = tag; *ptr && out_idx + 1 < size; ptr++) {
        unsigned char ch = (unsigned char)*ptr;
        if (isalnum(ch) || ch == '_' || ch == '-') {
            buffer[out_idx++] = (char)ch;
        } else if (!isspace(ch)) {
            buffer[out_idx++] = '_';
        } else {
            buffer[out_idx++] = '-';
        }
    }
    buffer[out_idx] = '\0';
}

static CheckpointRecord *ss_find_checkpoint(FileRecord *rec, const char *tag) {
    if (!rec || !tag || !tag[0]) {
        return NULL;
    }

    for (int i = 0; i < rec->checkpoint_count; i++) {
        if (strncmp(rec->checkpoints[i].tag, tag, sizeof(rec->checkpoints[i].tag)) == 0) {
            return &rec->checkpoints[i];
        }
    }

    return NULL;
}

static void ss_prune_oldest_checkpoint(FileRecord *rec) {
    if (!rec || rec->checkpoint_count <= 0) {
        return;
    }

    CheckpointRecord *oldest = &rec->checkpoints[0];
    if (oldest->filepath[0]) {
        unlink(oldest->filepath);
    }

    if (rec->checkpoint_count > 1) {
        memmove(&rec->checkpoints[0], &rec->checkpoints[1],
                (size_t)(rec->checkpoint_count - 1) * sizeof(CheckpointRecord));
    }

    rec->checkpoint_count--;
}

static void ss_set_record_paths(StorageServer *ss, FileRecord *rec, const char *logical_name) {
    if (!ss || !rec || !logical_name) {
        return;
    }

    safe_strcpy(rec->filename, logical_name, sizeof(rec->filename));
    if (flatten_logical_path(logical_name, rec->physical_name, sizeof(rec->physical_name)) != 0) {
        safe_strcpy(rec->physical_name, logical_name, sizeof(rec->physical_name));
    }

    if (build_path(rec->filepath, sizeof(rec->filepath), ss->storage_path, rec->physical_name) != 0) {
        rec->filepath[0] = '\0';
    }

    char meta_name[MAX_FILENAME_LENGTH + 8];
    snprintf(meta_name, sizeof(meta_name), "%s%s", rec->physical_name, SS_META_SUFFIX);
    if (build_path(rec->metapath, sizeof(rec->metapath), ss->storage_path, meta_name) != 0) {
        rec->metapath[0] = '\0';
    }

    char undo_name[MAX_FILENAME_LENGTH + 8];
    snprintf(undo_name, sizeof(undo_name), "%s%s", rec->physical_name, SS_UNDO_SUFFIX);
    if (build_path(rec->undopath, sizeof(rec->undopath), ss->storage_path, undo_name) != 0) {
        rec->undopath[0] = '\0';
    }
}

static void ss_init_file_record(StorageServer *ss, FileRecord *rec, const char *filename) {
    memset(rec, 0, sizeof(FileRecord));
    ss_set_record_paths(ss, rec, filename);
    pthread_mutex_init(&rec->file_lock, NULL);
}

static int ss_load_metadata(StorageServer *ss, FileRecord *rec) {
    FILE *fp = fopen(rec->metapath, "r");
    if (!fp) {
        return -1;
    }

    rec->read_count = 0;
    rec->write_count = 0;
    rec->undo_available = 0;
    rec->checkpoint_count = 0;

    char logical_override[MAX_FILENAME_LENGTH];
    logical_override[0] = '\0';

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        trim_string(line);
        if (strncmp(line, "LOGICAL=", 8) == 0) {
            safe_strcpy(logical_override, line + 8, sizeof(logical_override));
        } else if (strncmp(line, "OWNER=", 6) == 0) {
            safe_strcpy(rec->owner, line + 6, sizeof(rec->owner));
        } else if (strncmp(line, "READ=", 5) == 0) {
            char *token = strtok(line + 5, ",");
            rec->read_count = 0;
            while (token && rec->read_count < SS_MAX_USERS_PER_FILE) {
                trim_string(token);
                if (strlen(token) > 0) {
                    safe_strcpy(rec->read_users[rec->read_count], token, MAX_USERNAME_LENGTH);
                    rec->read_count++;
                }
                token = strtok(NULL, ",");
            }
        } else if (strncmp(line, "WRITE=", 6) == 0) {
            char *token = strtok(line + 6, ",");
            rec->write_count = 0;
            while (token && rec->write_count < SS_MAX_USERS_PER_FILE) {
                trim_string(token);
                if (strlen(token) > 0) {
                    safe_strcpy(rec->write_users[rec->write_count], token, MAX_USERNAME_LENGTH);
                    rec->write_count++;
                }
                token = strtok(NULL, ",");
            }
        } else if (strncmp(line, "CREATED=", 8) == 0) {
            rec->created = (time_t)atoll(line + 8);
        } else if (strncmp(line, "MODIFIED=", 9) == 0) {
            rec->modified = (time_t)atoll(line + 9);
        } else if (strncmp(line, "LAST_ACCESS=", 12) == 0) {
            rec->last_access = (time_t)atoll(line + 12);
        } else if (strncmp(line, "LAST_USER=", 10) == 0) {
            safe_strcpy(rec->last_access_user, line + 10, sizeof(rec->last_access_user));
        } else if (strncmp(line, "UNDO=", 5) == 0) {
            rec->undo_available = atoi(line + 5);
        } else if (strncmp(line, "CHECKPOINT=", 11) == 0) {
            if (rec->checkpoint_count < SS_MAX_CHECKPOINTS) {
                char *tag = line + 11;
                char *filepath = strchr(tag, ':');
                if (filepath) {
                    *filepath = '\0';
                    filepath++;
                    trim_string(tag);
                    trim_string(filepath);
                    CheckpointRecord *ckpt = &rec->checkpoints[rec->checkpoint_count];
                    safe_strcpy(ckpt->tag, tag, sizeof(ckpt->tag));
                    safe_strcpy(ckpt->filepath, filepath, sizeof(ckpt->filepath));
                    rec->checkpoint_count++;
                }
            }
        }
    }

    fclose(fp);

    if (logical_override[0] != '\0') {
        ss_set_record_paths(ss, rec, logical_override);
    }

    return 0;
}

static int ss_write_metadata(FileRecord *rec) {
    FILE *fp = fopen(rec->metapath, "w");
    if (!fp) {
        return -1;
    }

    fprintf(fp, "LOGICAL=%s\n", rec->filename);
    fprintf(fp, "OWNER=%s\n", rec->owner);
    fprintf(fp, "CREATED=%lld\n", (long long)rec->created);
    fprintf(fp, "MODIFIED=%lld\n", (long long)rec->modified);
    fprintf(fp, "LAST_ACCESS=%lld\n", (long long)rec->last_access);
    fprintf(fp, "LAST_USER=%s\n", rec->last_access_user);
    fprintf(fp, "UNDO=%d\n", rec->undo_available);

    fprintf(fp, "READ=");
    for (int i = 0; i < rec->read_count; i++) {
        fprintf(fp, "%s%s", rec->read_users[i], (i + 1 < rec->read_count) ? "," : "");
    }
    fprintf(fp, "\n");

    fprintf(fp, "WRITE=");
    for (int i = 0; i < rec->write_count; i++) {
        fprintf(fp, "%s%s", rec->write_users[i], (i + 1 < rec->write_count) ? "," : "");
    }
    fprintf(fp, "\n");

    for (int i = 0; i < rec->checkpoint_count && i < SS_MAX_CHECKPOINTS; i++) {
        fprintf(fp, "CHECKPOINT=%s:%s\n", rec->checkpoints[i].tag, rec->checkpoints[i].filepath);
    }

    fclose(fp);
    return 0;
}

static FileRecord *ss_find_file(StorageServer *ss, const char *filename) {
    FileRecord *node = ss->files;
    while (node) {
        if (strncmp(node->filename, filename, sizeof(node->filename)) == 0) {
            return node;
        }
        node = node->next;
    }
    return NULL;
}

static int ss_add_file_record(StorageServer *ss, const char *filename) {
    FileRecord *rec = (FileRecord *)safe_malloc(sizeof(FileRecord));
    if (!rec) {
        return -1;
    }

    ss_init_file_record(ss, rec, filename);
    if (ss_load_metadata(ss, rec) != 0) {
        rec->created = get_current_time();
        rec->modified = rec->created;
        rec->last_access = rec->created;
        rec->undo_available = 0;
        rec->read_count = 0;
        rec->write_count = 0;
        ss_write_metadata(rec);
    }

    rec->next = ss->files;
    ss->files = rec;
    return 0;
}

static void ss_load_existing_files(StorageServer *ss) {
    DIR *dir = opendir(ss->storage_path);
    if (!dir) {
        log_message(LOG_ERROR, "SS", "Failed to open storage path %s: %s", ss->storage_path, strerror(errno));
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (dirent_is_directory(ss->storage_path, entry)) {
            continue;
        }

        const char *name = entry->d_name;
        size_t len = strlen(name);
        if (len == 0) {
            continue;
        }

        if (len > strlen(SS_META_SUFFIX) &&
            strcmp(name + len - strlen(SS_META_SUFFIX), SS_META_SUFFIX) == 0) {
            continue;
        }

        if (len > strlen(SS_UNDO_SUFFIX) &&
            strcmp(name + len - strlen(SS_UNDO_SUFFIX), SS_UNDO_SUFFIX) == 0) {
            continue;
        }

        if (strstr(name, ".ckpt_") != NULL) {
            continue;
        }

        ss_add_file_record(ss, name);
    }

    closedir(dir);
}

static void ss_touch_file_metadata(StorageServer *ss, FileRecord *rec, const char *username, int update_modified) {
    time_t now = get_current_time();
    rec->last_access = now;
    if (username) {
        safe_strcpy(rec->last_access_user, username, sizeof(rec->last_access_user));
    }
    if (update_modified) {
        rec->modified = now;
    }
    ss_write_metadata(rec);
}

static void ss_sync_metadata_to_ns(StorageServer *ss, FileRecord *rec) {
    if (!ss || !rec || ss->ns_sockfd < 0) {
        return;
    }

    long size = get_file_size(rec->filepath);
    int words = 0;
    int chars = 0;
    char *content = ss_read_file_content(rec->filepath);
    if (content) {
        words = count_words(content);
        chars = count_chars(content);
        free(content);
    }

    char size_buf[32];
    char word_buf[32];
    char char_buf[32];
    snprintf(size_buf, sizeof(size_buf), "%ld", size >= 0 ? size : 0);
    snprintf(word_buf, sizeof(word_buf), "%d", words);
    snprintf(char_buf, sizeof(char_buf), "%d", chars);

    const char *fields[] = {MSG_WRITE_COMPLETE, rec->filename, size_buf, word_buf, char_buf};
    char *msg = protocol_build_message(fields, 5);
    if (msg) {
        protocol_send_message(ss->ns_sockfd, msg);
        free(msg);
    }
}

static WriteSession *ss_find_session(StorageServer *ss, int client_fd) {
    WriteSession *node = ss->sessions;
    while (node) {
        if (node->client_fd == client_fd) {
            return node;
        }
        node = node->next;
    }
    return NULL;
}

static WriteSession *ss_find_session_by_sentence(StorageServer *ss, FileRecord *rec, int sentence_index) {
    WriteSession *node = ss->sessions;
    while (node) {
        if (node->file == rec && node->sentence_index == sentence_index) {
            return node;
        }
        node = node->next;
    }
    return NULL;
}

static int ss_file_has_active_writers(StorageServer *ss, FileRecord *rec) {
    WriteSession *node = ss->sessions;
    while (node) {
        if (node->file == rec) {
            return 1;
        }
        node = node->next;
    }
    return 0;
}

static void ss_free_session(WriteSession *session) {
    if (!session) {
        return;
    }
    if (session->file_snapshot) {
        free(session->file_snapshot);
    }
    if (session->sentence_working) {
        free(session->sentence_working);
    }
    free(session);
}

static WriteSession *ss_detach_session(StorageServer *ss, int client_fd) {
    WriteSession *prev = NULL;
    WriteSession *node = ss->sessions;
    while (node) {
        if (node->client_fd == client_fd) {
            if (prev) {
                prev->next = node->next;
            } else {
                ss->sessions = node->next;
            }
            node->next = NULL;
            return node;
        }
        prev = node;
        node = node->next;
    }
    return NULL;
}

static void ss_abort_session(StorageServer *ss, int client_fd) {
    pthread_mutex_lock(&ss->sessions_lock);
    WriteSession *session = ss_detach_session(ss, client_fd);
    pthread_mutex_unlock(&ss->sessions_lock);

    if (!session) {
        return;
    }

    ss_free_session(session);
}

static WriteSession *ss_create_session(StorageServer *ss, int client_fd, FileRecord *rec,
                                       const char *username, int sentence_index,
                                       size_t start_offset, size_t end_offset,
                                       const char *file_snapshot, const char *sentence_text) {
    WriteSession *session = (WriteSession *)safe_malloc(sizeof(WriteSession));
    if (!session) {
        return NULL;
    }
    memset(session, 0, sizeof(WriteSession));
    session->client_fd = client_fd;
    session->file = rec;
    session->sentence_index = sentence_index;
    safe_strcpy(session->username, username, sizeof(session->username));
    session->sentence_start = start_offset;
    session->sentence_end = end_offset;
    if (file_snapshot) {
        session->file_snapshot = strdup(file_snapshot);
        if (!session->file_snapshot) {
            free(session);
            return NULL;
        }
    }
    if (sentence_text) {
        session->sentence_working = strdup(sentence_text);
        if (!session->sentence_working) {
            free(session->file_snapshot);
            free(session);
            return NULL;
        }
    }
    session->next = ss->sessions;
    ss->sessions = session;
    return session;
}

static int ss_open_ns_socket(StorageServer *ss) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        log_message(LOG_ERROR, "SS", "Failed to create NS socket: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ss->ns_port);
    if (inet_pton(AF_INET, ss->ns_ip, &addr.sin_addr) != 1) {
        log_message(LOG_ERROR, "SS", "Invalid NS IP: %s", ss->ns_ip);
        close(sockfd);
        return -1;
    }

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_message(LOG_ERROR, "SS", "Failed to connect to NS %s:%d: %s", ss->ns_ip, ss->ns_port, strerror(errno));
        close(sockfd);
        return -1;
    }

    ss->ns_sockfd = sockfd;
    return 0;
}

static int ss_send_hello(StorageServer *ss) {
    char ns_port_buf[16];
    snprintf(ns_port_buf, sizeof(ns_port_buf), "%d", ss->ns_port);
    char client_port_buf[16];
    snprintf(client_port_buf, sizeof(client_port_buf), "%d", ss->client_port);

    const char *fields[] = {MSG_HELLO_SS, ss->ns_ip, ns_port_buf, ss->client_ip, client_port_buf};
    char *msg = protocol_build_message(fields, 5);
    if (!msg) {
        return -1;
    }

    if (protocol_send_message(ss->ns_sockfd, msg) < 0) {
        free(msg);
        log_message(LOG_ERROR, "SS", "Failed to send HELLO_SS");
        return -1;
    }
    free(msg);

    char *resp_raw = protocol_receive_message(ss->ns_sockfd);
    if (!resp_raw) {
        log_message(LOG_ERROR, "SS", "No response from NS during registration");
        return -1;
    }

    ProtocolMessage resp;
    if (protocol_parse_message(resp_raw, &resp) != 0 || resp.field_count == 0) {
        free(resp_raw);
        return -1;
    }

    int ok = (strcmp(resp.fields[0], RESP_OK) == 0);
    protocol_free_message(&resp);
    free(resp_raw);
    return ok ? 0 : -1;
}

static int ss_emit_file_list(StorageServer *ss) {
    pthread_mutex_lock(&ss->files_lock);
    FileRecord *node = ss->files;
    while (node) {
        const char *owner = node->owner[0] ? node->owner : "";
        char read_acl_buf[MAX_FIELD_SIZE] = {0};
        char write_acl_buf[MAX_FIELD_SIZE] = {0};

        long size = get_file_size(node->filepath);
        int words = 0;
        int chars = 0;
        char *content = ss_read_file_content(node->filepath);
        if (content) {
            words = count_words(content);
            chars = count_chars(content);
            free(content);
        }

        char size_buf[32];
        char word_buf[32];
        char char_buf[32];
        snprintf(size_buf, sizeof(size_buf), "%ld", size);
        snprintf(word_buf, sizeof(word_buf), "%d", words);
        snprintf(char_buf, sizeof(char_buf), "%d", chars);

        if (ss_join_acl((const char (*)[MAX_USERNAME_LENGTH])node->read_users, node->read_count, read_acl_buf, sizeof(read_acl_buf)) != 0) {
            log_message(LOG_ERROR, "SS", "Failed to serialize read ACL for %s", node->filename);
            read_acl_buf[0] = '\0';
        }
        if (ss_join_acl((const char (*)[MAX_USERNAME_LENGTH])node->write_users, node->write_count, write_acl_buf, sizeof(write_acl_buf)) != 0) {
            log_message(LOG_ERROR, "SS", "Failed to serialize write ACL for %s", node->filename);
            write_acl_buf[0] = '\0';
        }

        const char *fields[] = {
            MSG_SS_HAS_FILE,
            node->filename,
            owner,
            read_acl_buf,
            write_acl_buf,
            size_buf,
            word_buf,
            char_buf};
        char *msg = protocol_build_message(fields, 8);
        if (!msg) {
            pthread_mutex_unlock(&ss->files_lock);
            return -1;
        }
        if (protocol_send_message(ss->ns_sockfd, msg) < 0) {
            free(msg);
            pthread_mutex_unlock(&ss->files_lock);
            return -1;
        }
        free(msg);
        node = node->next;
    }
    pthread_mutex_unlock(&ss->files_lock);

    const char *done_fields[] = {MSG_SS_FILES_DONE};
    char *done_msg = protocol_build_message(done_fields, 1);
    if (!done_msg) {
        return -1;
    }
    if (protocol_send_message(ss->ns_sockfd, done_msg) < 0) {
        free(done_msg);
        return -1;
    }
    free(done_msg);
    return 0;
}

static void *ns_control_loop(void *arg) {
    NsThreadArgs *params = (NsThreadArgs *)arg;
    StorageServer *ss = params->ss;
    free(params);

    while (ss->running) {
        char *raw = protocol_receive_message(ss->ns_sockfd);
        if (!raw) {
            log_message(LOG_WARNING, "SS", "Lost connection to Name Server");
            ss->running = 0;
            if (ss->client_listen_fd >= 0) {
                shutdown(ss->client_listen_fd, SHUT_RDWR);
            }
            break;
        }

        ProtocolMessage msg;
        if (protocol_parse_message(raw, &msg) != 0 || msg.field_count == 0) {
            free(raw);
            continue;
        }

        const char *command = msg.fields[0];

        if (strcmp(command, MSG_CREATE_FILE) == 0) {
            if (msg.field_count < 3) {
                ss_send_ns_error(ss, ERR_INVALID_REQUEST, "CREATE_FILE missing fields");
            } else {
                const char *filename = msg.fields[1];
                const char *owner = msg.fields[2];
                if (!validate_filename(filename) || !validate_username(owner)) {
                    ss_send_ns_error(ss, ERR_INVALID_REQUEST, "Invalid filename or owner");
                } else if (ss_create_file(ss, filename, owner) != 0) {
                    ss_send_ns_error(ss, ERR_FILE_EXISTS, "File already exists");
                } else {
                    const char *fields[] = {RESP_OK_CREATE, filename};
                    char *resp = protocol_build_message(fields, 2);
                    if (resp) {
                        protocol_send_message(ss->ns_sockfd, resp);
                        free(resp);
                    }
                    log_message(LOG_INFO, "SS", "Created file %s for owner %s", filename, owner);
                }
            }
        } else if (strcmp(command, MSG_DELETE_FILE) == 0) {
            if (msg.field_count < 2) {
                ss_send_ns_error(ss, ERR_INVALID_REQUEST, "DELETE_FILE missing filename");
            } else {
                const char *filename = msg.fields[1];
                if (!validate_filename(filename)) {
                    ss_send_ns_error(ss, ERR_INVALID_REQUEST, "Invalid filename");
                } else {
                    int delete_status = ss_delete_file(ss, filename);

                    if (delete_status == -1) {
                        ss_send_ns_error(ss, ERR_FILE_NOT_FOUND, "File not found");
                    } else if (delete_status == -2) {
                        ss_send_ns_error(ss, ERR_SENTENCE_LOCKED, "File is locked for writing");
                    } else {
                        const char *fields[] = {RESP_OK_DELETE, filename};
                        char *resp = protocol_build_message(fields, 2);
                        if (resp) {
                            protocol_send_message(ss->ns_sockfd, resp);
                            free(resp);
                        }
                        log_message(LOG_INFO, "SS", "Deleted file %s", filename);
                    }
                }
            }
        } else if (strcmp(command, MSG_GET_STATS) == 0) {
            if (msg.field_count < 2) {
                ss_send_ns_error(ss, ERR_INVALID_REQUEST, "GET_STATS missing filename");
            } else {
                const char *filename = msg.fields[1];
                pthread_mutex_lock(&ss->files_lock);
                FileRecord *rec = ss_find_file(ss, filename);
                if (!rec) {
                    pthread_mutex_unlock(&ss->files_lock);
                    ss_send_ns_error(ss, ERR_FILE_NOT_FOUND, "Stats target missing");
                } else {
                    pthread_mutex_lock(&rec->file_lock);
                    pthread_mutex_unlock(&ss->files_lock);

                    long size = get_file_size(rec->filepath);
                    char *content = ss_read_file_content(rec->filepath);
                    int words = content ? count_words(content) : 0;
                    int chars = content ? count_chars(content) : 0;

                    char size_buf[32];
                    snprintf(size_buf, sizeof(size_buf), "%ld", size >= 0 ? size : 0);
                    char words_buf[32];
                    snprintf(words_buf, sizeof(words_buf), "%d", words);
                    char chars_buf[32];
                    snprintf(chars_buf, sizeof(chars_buf), "%d", chars);

                    char created_buf[32], modified_buf[32], access_buf[32];
                    snprintf(created_buf, sizeof(created_buf), "%lld", (long long)rec->created);
                    snprintf(modified_buf, sizeof(modified_buf), "%lld", (long long)rec->modified);
                    snprintf(access_buf, sizeof(access_buf), "%lld", (long long)rec->last_access);

                    const char *fields[] = {
                        RESP_OK_STATS,
                        rec->filename,
                        rec->owner,
                        size_buf,
                        words_buf,
                        chars_buf,
                        created_buf,
                        modified_buf,
                        access_buf,
                        rec->last_access_user
                    };
                    char *resp = protocol_build_message(fields, 10);
                    if (resp) {
                        protocol_send_message(ss->ns_sockfd, resp);
                        free(resp);
                    }

                    if (content) {
                        free(content);
                    }

                    pthread_mutex_unlock(&rec->file_lock);
                }
            }
        } else if (strcmp(command, MSG_GET_CONTENT) == 0) {
            if (msg.field_count < 2) {
                ss_send_ns_error(ss, ERR_INVALID_REQUEST, "GET_CONTENT missing filename");
            } else {
                const char *filename = msg.fields[1];
                pthread_mutex_lock(&ss->files_lock);
                FileRecord *rec = ss_find_file(ss, filename);
                if (!rec) {
                    pthread_mutex_unlock(&ss->files_lock);
                    ss_send_ns_error(ss, ERR_FILE_NOT_FOUND, "Content file missing");
                } else {
                    pthread_mutex_lock(&rec->file_lock);
                    pthread_mutex_unlock(&ss->files_lock);

                    char *content = ss_read_file_content(rec->filepath);
                    if (!content) {
                        pthread_mutex_unlock(&rec->file_lock);
                        ss_send_ns_error(ss, ERR_INTERNAL_ERROR, "Failed to read file");
                    } else {
                        const char *fields[] = {RESP_OK_CONTENT, rec->filename, content};
                        char *resp = protocol_build_message(fields, 3);
                        if (resp) {
                            protocol_send_message(ss->ns_sockfd, resp);
                            free(resp);
                        }
                        free(content);
                        pthread_mutex_unlock(&rec->file_lock);
                    }
                }
            }
        } else if (strcmp(command, MSG_SS_RENAME) == 0) {
            if (msg.field_count < 5) {
                ss_send_ns_error(ss, ERR_INVALID_REQUEST, "SS_RENAME missing fields");
            } else {
                const char *old_logical = msg.fields[1];
                const char *new_logical = msg.fields[2];
                const char *old_flat = msg.fields[3];
                const char *new_flat = msg.fields[4];
                if (!validate_filename(old_logical) || !validate_filename(new_logical)) {
                    ss_send_ns_error(ss, ERR_INVALID_REQUEST, "Invalid logical path");
                } else if (ss_handle_rename(ss, old_logical, new_logical, old_flat, new_flat) != 0) {
                    ss_send_ns_error(ss, ERR_INTERNAL_ERROR, "Rename failed");
                } else {
                    const char *ok_fields[] = {RESP_OK};
                    char *resp = protocol_build_message(ok_fields, 1);
                    if (resp) {
                        protocol_send_message(ss->ns_sockfd, resp);
                        free(resp);
                    }
                }
            }
        } else if (strcmp(command, MSG_SS_ADDACCESS) == 0) {
            if (msg.field_count < 4) {
                ss_send_ns_error(ss, ERR_INVALID_REQUEST, "ADDACCESS missing fields");
            } else {
                const char *filename = msg.fields[1];
                const char *username = msg.fields[2];
                const char *perm_token = msg.fields[3];

                pthread_mutex_lock(&ss->files_lock);
                FileRecord *rec = ss_find_file(ss, filename);
                if (!rec) {
                    pthread_mutex_unlock(&ss->files_lock);
                    ss_send_ns_error(ss, ERR_FILE_NOT_FOUND, "File not found");
                } else {
                    pthread_mutex_lock(&rec->file_lock);
                    pthread_mutex_unlock(&ss->files_lock);

                    int want_read = 0;
                    int want_write = 0;
                    for (const char *p = perm_token; p && *p; ++p) {
                        char c = (char)toupper((unsigned char)*p);
                        if (c == 'R') {
                            want_read = 1;
                        } else if (c == 'W') {
                            want_write = 1;
                        }
                    }

                    if (!want_read && !want_write) {
                        pthread_mutex_unlock(&rec->file_lock);
                        ss_send_ns_error(ss, ERR_INVALID_REQUEST, "Invalid permission token");
                    } else {
                        int had_read = ss_acl_contains(rec->read_users, rec->read_count, username);
                        int had_write = ss_acl_contains(rec->write_users, rec->write_count, username);
                        int added_read = 0;
                        int added_write = 0;
                        int status = 0;

                        if (want_read && !had_read) {
                            int add_res = ss_acl_add(rec->read_users, &rec->read_count, username);
                            if (add_res < 0) {
                                status = -1;
                            } else {
                                added_read = add_res > 0;
                            }
                        }

                        if (status == 0 && want_write && !had_write) {
                            int add_res = ss_acl_add(rec->write_users, &rec->write_count, username);
                            if (add_res < 0) {
                                status = -1;
                            } else {
                                added_write = add_res > 0;
                            }
                        }

                        int metadata_error = 0;
                        if (status == 0 && (added_read || added_write)) {
                            if (ss_write_metadata(rec) != 0) {
                                metadata_error = 1;
                            }
                        }

                        if (status != 0 || metadata_error) {
                            if (added_read) {
                                ss_acl_remove(rec->read_users, &rec->read_count, username);
                            }
                            if (added_write) {
                                ss_acl_remove(rec->write_users, &rec->write_count, username);
                            }
                            pthread_mutex_unlock(&rec->file_lock);
                            ss_send_ns_error(ss, ERR_INTERNAL_ERROR, "Failed to update ACL metadata");
                        } else {
                            pthread_mutex_unlock(&rec->file_lock);
                            const char *fields[] = {RESP_OK_ACCESS_CHANGED, filename};
                            char *resp = protocol_build_message(fields, 2);
                            if (resp) {
                                protocol_send_message(ss->ns_sockfd, resp);
                                free(resp);
                            }
                        }
                    }
                }
            }
        } else if (strcmp(command, MSG_SS_REMACCESS) == 0) {
            if (msg.field_count < 3) {
                ss_send_ns_error(ss, ERR_INVALID_REQUEST, "REMACCESS missing fields");
            } else {
                const char *filename = msg.fields[1];
                const char *username = msg.fields[2];

                pthread_mutex_lock(&ss->files_lock);
                FileRecord *rec = ss_find_file(ss, filename);
                if (!rec) {
                    pthread_mutex_unlock(&ss->files_lock);
                    ss_send_ns_error(ss, ERR_FILE_NOT_FOUND, "File not found");
                } else {
                    pthread_mutex_lock(&rec->file_lock);
                    pthread_mutex_unlock(&ss->files_lock);

                    int removed_read = ss_acl_remove(rec->read_users, &rec->read_count, username);
                    int removed_write = ss_acl_remove(rec->write_users, &rec->write_count, username);

                    if (!removed_read && !removed_write) {
                        pthread_mutex_unlock(&rec->file_lock);
                        ss_send_ns_error(ss, ERR_USER_NOT_FOUND, "User not present in ACL");
                    } else {
                        if (ss_write_metadata(rec) != 0) {
                            if (removed_read) {
                                ss_acl_add(rec->read_users, &rec->read_count, username);
                            }
                            if (removed_write) {
                                ss_acl_add(rec->write_users, &rec->write_count, username);
                            }
                            pthread_mutex_unlock(&rec->file_lock);
                            ss_send_ns_error(ss, ERR_INTERNAL_ERROR, "Failed to persist ACL metadata");
                        } else {
                            pthread_mutex_unlock(&rec->file_lock);
                            const char *fields[] = {RESP_OK_ACCESS_CHANGED, filename};
                            char *resp = protocol_build_message(fields, 2);
                            if (resp) {
                                protocol_send_message(ss->ns_sockfd, resp);
                                free(resp);
                            }
                        }
                    }
                }
            }
        } else if (strcmp(command, MSG_SS_COPY) == 0) {
            handle_ss_copy(ss, &msg);
        } else {
            log_message(LOG_WARNING, "SS", "Unknown NS command: %s", command);
        }

        protocol_free_message(&msg);
        free(raw);
    }

    return NULL;
}

static int ss_start_ns_thread(StorageServer *ss) {
    NsThreadArgs *args = (NsThreadArgs *)safe_malloc(sizeof(NsThreadArgs));
    if (!args) {
        return -1;
    }
    args->ss = ss;
    if (pthread_create(&ss->ns_thread, NULL, ns_control_loop, args) != 0) {
        log_message(LOG_ERROR, "SS", "Failed to start NS control thread");
        free(args);
        return -1;
    }
    ss->ns_thread_active = 1;
    ss->ns_thread_started = 1;
    return 0;
}

static int ss_open_client_listener(StorageServer *ss) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_message(LOG_ERROR, "SS", "Failed to create client listener: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ss->client_ip);
    addr.sin_port = htons(ss->client_port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_message(LOG_ERROR, "SS", "Bind failed on %s:%d: %s", ss->client_ip, ss->client_port, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, SOMAXCONN) < 0) {
        log_message(LOG_ERROR, "SS", "Listen failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    ss->client_listen_fd = fd;
    return 0;
}

static int parse_int(const char *text, int *out_value) {
    if (!text || !out_value) {
        return -1;
    }
    char *endptr = NULL;
    long val = strtol(text, &endptr, 10);
    if (!endptr || *endptr != '\0') {
        return -1;
    }
    if (val < INT_MIN || val > INT_MAX) {
        return -1;
    }
    *out_value = (int)val;
    return 0;
}

static int ss_check_read_access(FileRecord *rec, const char *user) {
    if (!rec || !user) {
        return 0;
    }
    if (rec->owner[0] != '\0' && strncmp(rec->owner, user, MAX_USERNAME_LENGTH) == 0) {
        return 1;
    }
    for (int i = 0; i < rec->read_count; i++) {
        if (strncmp(rec->read_users[i], user, MAX_USERNAME_LENGTH) == 0) {
            return 1;
        }
    }
    for (int i = 0; i < rec->write_count; i++) {
        if (strncmp(rec->write_users[i], user, MAX_USERNAME_LENGTH) == 0) {
            return 1;
        }
    }
    return 0;
}

static int ss_check_write_access(FileRecord *rec, const char *user) {
    if (!rec || !user) {
        return 0;
    }
    if (rec->owner[0] != '\0' && strncmp(rec->owner, user, MAX_USERNAME_LENGTH) == 0) {
        return 1;
    }
    for (int i = 0; i < rec->write_count; i++) {
        if (strncmp(rec->write_users[i], user, MAX_USERNAME_LENGTH) == 0) {
            return 1;
        }
    }
    return 0;
}

static int ss_acl_contains(const char entries[][MAX_USERNAME_LENGTH], int count, const char *username) {
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

static int ss_acl_add(char entries[][MAX_USERNAME_LENGTH], int *count, const char *username) {
    if (!entries || !count || !username) {
        return -1;
    }

    if (ss_acl_contains(entries, *count, username)) {
        return 0;
    }

    if (*count >= SS_MAX_USERS_PER_FILE) {
        return -1;
    }

    if (safe_strcpy(entries[*count], username, MAX_USERNAME_LENGTH) != 0) {
        return -1;
    }

    (*count)++;
    return 1;
}

static int ss_acl_remove(char entries[][MAX_USERNAME_LENGTH], int *count, const char *username) {
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

static int ss_join_acl(const char entries[][MAX_USERNAME_LENGTH], int count, char *buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) {
        return -1;
    }

    buffer[0] = '\0';
    if (!entries || count <= 0) {
        return 0;
    }

    size_t offset = 0;
    for (int i = 0; i < count; i++) {
        size_t len = strnlen(entries[i], MAX_USERNAME_LENGTH);
        if (len == 0) {
            continue;
        }
        if (offset + len + (offset > 0 ? 1 : 0) >= buffer_size) {
            return -1;
        }
        if (offset > 0) {
            buffer[offset++] = ',';
        }
        memcpy(buffer + offset, entries[i], len);
        offset += len;
    }
    buffer[offset] = '\0';
    return 0;
}

static char *ss_read_file_content(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long len = ftell(fp);
    if (len < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);

    char *buffer = (char *)safe_malloc((size_t)len + 1);
    if (!buffer) {
        fclose(fp);
        return NULL;
    }

    size_t read = fread(buffer, 1, (size_t)len, fp);
    buffer[read] = '\0';
    fclose(fp);
    return buffer;
}

static int ss_connect_to_peer(const char *ip, int port) {
    if (!ip || port <= 0) {
        return -1;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        close(sockfd);
        return -1;
    }

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sockfd);
        return -1;
    }

    return sockfd;
}

static int ss_write_file_content(const char *path, const char *data) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return -1;
    }
    size_t len = strlen(data);
    size_t written = fwrite(data, 1, len, fp);
    fclose(fp);
    return (written == len) ? 0 : -1;
}

static void ss_send_ns_error(StorageServer *ss, int code, const char *message) {
    char *err = protocol_build_error(code, message);
    if (!err) {
        return;
    }
    protocol_send_message(ss->ns_sockfd, err);
    free(err);
}

static int ss_create_file(StorageServer *ss, const char *filename, const char *owner) {
    pthread_mutex_lock(&ss->files_lock);
    if (ss_find_file(ss, filename)) {
        pthread_mutex_unlock(&ss->files_lock);
        return -1;
    }

    FileRecord *rec = (FileRecord *)safe_malloc(sizeof(FileRecord));
    if (!rec) {
        pthread_mutex_unlock(&ss->files_lock);
        return -1;
    }

    ss_init_file_record(ss, rec, filename);
    if (owner) {
        safe_strcpy(rec->owner, owner, sizeof(rec->owner));
        safe_strcpy(rec->last_access_user, owner, sizeof(rec->last_access_user));
    }
    rec->created = get_current_time();
    rec->modified = rec->created;
    rec->last_access = rec->created;
    rec->undo_available = 0;
    rec->read_count = 0;
    rec->write_count = 0;

    FILE *fp = fopen(rec->filepath, "wb");
    if (!fp) {
        pthread_mutex_destroy(&rec->file_lock);
        free(rec);
        pthread_mutex_unlock(&ss->files_lock);
        return -1;
    }
    fclose(fp);

    ss_write_metadata(rec);

    rec->next = ss->files;
    ss->files = rec;
    pthread_mutex_unlock(&ss->files_lock);
    return 0;
}

static int ss_delete_file(StorageServer *ss, const char *filename) {
    pthread_mutex_lock(&ss->files_lock);
    FileRecord *prev = NULL;
    FileRecord *node = ss->files;
    while (node) {
        if (strncmp(node->filename, filename, sizeof(node->filename)) == 0) {
            break;
        }
        prev = node;
        node = node->next;
    }

    if (!node) {
        pthread_mutex_unlock(&ss->files_lock);
        return -1;
    }

    pthread_mutex_lock(&ss->sessions_lock);
    int writers_active = ss_file_has_active_writers(ss, node);
    pthread_mutex_unlock(&ss->sessions_lock);

    if (writers_active) {
        pthread_mutex_unlock(&ss->files_lock);
        return -2;
    }

    unlink(node->filepath);
    unlink(node->metapath);
    unlink(node->undopath);

    for (int i = 0; i < node->checkpoint_count; i++) {
        if (node->checkpoints[i].filepath[0]) {
            unlink(node->checkpoints[i].filepath);
        }
    }

    if (prev) {
        prev->next = node->next;
    } else {
        ss->files = node->next;
    }

    pthread_mutex_unlock(&ss->files_lock);
    pthread_mutex_destroy(&node->file_lock);
    free(node);
    return 0;
}

static int ss_handle_rename(StorageServer *ss, const char *old_logical, const char *new_logical,
                            const char *old_flat, const char *new_flat) {
    if (!ss || !old_logical || !new_logical || !old_flat || !new_flat) {
        return -1;
    }

    pthread_mutex_lock(&ss->files_lock);
    FileRecord *rec = ss_find_file(ss, old_logical);
    if (!rec) {
        pthread_mutex_unlock(&ss->files_lock);
        return -1;
    }

    pthread_mutex_lock(&rec->file_lock);
    pthread_mutex_unlock(&ss->files_lock);

    char expected_old_flat[MAX_FILENAME_LENGTH];
    if (flatten_logical_path(rec->filename, expected_old_flat, sizeof(expected_old_flat)) != 0) {
        pthread_mutex_unlock(&rec->file_lock);
        return -1;
    }

    if (strncmp(expected_old_flat, old_flat, sizeof(expected_old_flat)) != 0) {
        pthread_mutex_unlock(&rec->file_lock);
        return -1;
    }

    char current_path[MAX_PATH_LENGTH];
    char new_path[MAX_PATH_LENGTH];
    if (build_path(current_path, sizeof(current_path), ss->storage_path, old_flat) != 0 ||
        build_path(new_path, sizeof(new_path), ss->storage_path, new_flat) != 0) {
        pthread_mutex_unlock(&rec->file_lock);
        return -1;
    }

    char current_meta[MAX_PATH_LENGTH];
    char new_meta[MAX_PATH_LENGTH];
    char current_undo[MAX_PATH_LENGTH];
    char new_undo[MAX_PATH_LENGTH];

    snprintf(current_meta, sizeof(current_meta), "%s%s", current_path, SS_META_SUFFIX);
    snprintf(new_meta, sizeof(new_meta), "%s%s", new_path, SS_META_SUFFIX);
    snprintf(current_undo, sizeof(current_undo), "%s%s", current_path, SS_UNDO_SUFFIX);
    snprintf(new_undo, sizeof(new_undo), "%s%s", new_path, SS_UNDO_SUFFIX);

    if (rename(current_path, new_path) != 0) {
        pthread_mutex_unlock(&rec->file_lock);
        return -1;
    }

    rename(current_meta, new_meta);
    rename(current_undo, new_undo);

    safe_strcpy(rec->filename, new_logical, sizeof(rec->filename));
    safe_strcpy(rec->physical_name, new_flat, sizeof(rec->physical_name));
    safe_strcpy(rec->filepath, new_path, sizeof(rec->filepath));
    safe_strcpy(rec->metapath, new_meta, sizeof(rec->metapath));
    safe_strcpy(rec->undopath, new_undo, sizeof(rec->undopath));

    ss_touch_file_metadata(ss, rec, NULL, 1);

    pthread_mutex_unlock(&rec->file_lock);
    return 0;
}

static void send_error_response(int fd, int code, const char *message) {
    char *err = protocol_build_error(code, message);
    if (!err) {
        return;
    }
    protocol_send_message(fd, err);
    free(err);
}

static void send_simple_ok(int fd, const char *detail) {
    if (detail) {
        const char *fields[] = {RESP_OK, detail};
        char *msg = protocol_build_message(fields, 2);
        if (!msg) {
            return;
        }
        protocol_send_message(fd, msg);
        free(msg);
    } else {
        const char *fields[] = {RESP_OK};
        char *msg = protocol_build_message(fields, 1);
        if (!msg) {
            return;
        }
        protocol_send_message(fd, msg);
        free(msg);
    }
}

static int split_words(const char *text, char ***words_out, int *count_out) {
    if (!words_out || !count_out) {
        return -1;
    }
    *words_out = NULL;
    *count_out = 0;

    if (!text) {
        return 0;
    }

    const char *ptr = text;
    while (*ptr) {
        while (*ptr && isspace((unsigned char)*ptr)) {
            ptr++;
        }
        if (!*ptr) {
            break;
        }
        const char *start = ptr;
        while (*ptr && !isspace((unsigned char)*ptr)) {
            ptr++;
        }
        size_t len = (size_t)(ptr - start);
        if (len == 0) {
            continue;
        }

        char *word = (char *)safe_malloc(len + 1);
        if (!word) {
            for (int i = 0; i < *count_out; i++) {
                free((*words_out)[i]);
            }
            free(*words_out);
            *words_out = NULL;
            *count_out = 0;
            return -1;
        }
        memcpy(word, start, len);
        word[len] = '\0';

        char **new_array = (char **)realloc(*words_out, (size_t)(*count_out + 1) * sizeof(char *));
        if (!new_array) {
            free(word);
            for (int i = 0; i < *count_out; i++) {
                free((*words_out)[i]);
            }
            free(*words_out);
            *words_out = NULL;
            *count_out = 0;
            return -1;
        }
        *words_out = new_array;
        (*words_out)[*count_out] = word;
        (*count_out)++;
    }

    return 0;
}

static void free_words(char **words, int count) {
    if (!words) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(words[i]);
    }
    free(words);
}

static char *join_words(char **words, int count, char delimiter_char) {
    int ends_with_delimiter = 0;
    if (count > 0) {
        const char *last_word = words[count - 1];
        if (last_word) {
            size_t last_len = strlen(last_word);
            if (last_len > 0 && is_sentence_delimiter(last_word[last_len - 1])) {
                ends_with_delimiter = 1;
            }
        }
    }

    size_t total = 0;
    for (int i = 0; i < count; i++) {
        if (words[i]) {
            total += strlen(words[i]);
        }
        if (i + 1 < count) {
            total++; // space
        }
    }
    if (delimiter_char && !ends_with_delimiter) {
        total++;
    }
    char *result = (char *)safe_malloc(total + 1);
    if (!result) {
        return NULL;
    }
    result[0] = '\0';
    for (int i = 0; i < count; i++) {
        if (i > 0) {
            safe_strcat(result, " ", total + 1);
        }
        if (words[i]) {
            safe_strcat(result, words[i], total + 1);
        }
    }
    if (delimiter_char && !ends_with_delimiter) {
        size_t len = strlen(result);
        result[len] = delimiter_char;
        result[len + 1] = '\0';
    }
    return result;
}

static int ss_get_sentence_bounds(const char *text, int sentence_index,
                                  size_t *start_out, size_t *end_out) {
    if (!text || sentence_index < 0 || !start_out || !end_out) {
        return -1;
    }

    const char *cursor = text;
    int current = 0;

    while (*cursor && isspace((unsigned char)*cursor)) {
        cursor++;
    }

    const char *sentence_start = cursor;

    if (*sentence_start == '\0') {
        if (sentence_index == 0) {
            *start_out = (size_t)(sentence_start - text);
            *end_out = *start_out;
            return 0;
        }
        return -1;
    }

    while (*cursor) {
        if (is_sentence_delimiter(*cursor)) {
            const char *sentence_end = cursor + 1;
            if (current == sentence_index) {
                *start_out = (size_t)(sentence_start - text);
                *end_out = (size_t)(sentence_end - text);
                return 0;
            }

            current++;
            cursor = sentence_end;
            while (*cursor && isspace((unsigned char)*cursor)) {
                cursor++;
            }
            sentence_start = cursor;
            continue;
        }
        cursor++;
    }

    if (current == sentence_index && sentence_start <= cursor) {
        *start_out = (size_t)(sentence_start - text);
        *end_out = (size_t)(cursor - text);
        return 0;
    }

    return -1;
}

static int ss_apply_word_edit(WriteSession *session, int word_index, const char *content) {
    if (!session || word_index < 0 || !content) {
        return -1;
    }

    const char *current_sentence = session->sentence_working ? session->sentence_working : "";
    char *buffer = strdup(current_sentence);
    if (!buffer) {
        return -1;
    }

    size_t len = strlen(buffer);
    while (len > 0 && isspace((unsigned char)buffer[len - 1])) {
        buffer[len - 1] = '\0';
        len--;
    }

    char delimiter = '\0';
    if (len > 0 && is_sentence_delimiter(buffer[len - 1])) {
        delimiter = buffer[len - 1];
        buffer[len - 1] = '\0';
    }

    char **words = NULL;
    int word_count = 0;
    if (split_words(buffer, &words, &word_count) != 0) {
        free(buffer);
        return -1;
    }

    free(buffer);

    if (word_index > word_count + 1) {
        free_words(words, word_count);
        return -1;
    }

    char **new_words = NULL;
    int new_count = 0;
    if (split_words(content, &new_words, &new_count) != 0) {
        free_words(words, word_count);
        return -1;
    }

    if (new_count > 0) {
        int original_count = word_count;
        int insert_pos = word_index;
        if (word_index > original_count) {
            insert_pos = original_count;
        }

        char **expanded = (char **)realloc(words, (size_t)(original_count + new_count) * sizeof(char *));
        if (!expanded) {
            free_words(words, word_count);
            free_words(new_words, new_count);
            return -1;
        }
        words = expanded;
        if (insert_pos < original_count) {
            memmove(&words[insert_pos + new_count],
                    &words[insert_pos],
                    (size_t)(original_count - insert_pos) * sizeof(char *));
        }
        for (int i = 0; i < new_count; i++) {
            words[insert_pos + i] = new_words[i];
        }
        word_count = original_count + new_count;
        free(new_words);
    } else {
        free_words(new_words, new_count);
    }

    char *joined = join_words(words, word_count, delimiter);
    free_words(words, word_count);
    if (!joined) {
        return -1;
    }

    if (session->sentence_working) {
        free(session->sentence_working);
    }
    session->sentence_working = joined;
    return 0;
}

static void handle_client_read(StorageServer *ss, int fd, ProtocolMessage *msg) {
    if (msg->field_count < 3) {
        send_error_response(fd, ERR_INVALID_REQUEST, "Missing fields in REQ_READ");
        return;
    }

    const char *username = msg->fields[1];
    const char *filename = msg->fields[2];

    pthread_mutex_lock(&ss->files_lock);
    FileRecord *rec = ss_find_file(ss, filename);
    if (!rec) {
        pthread_mutex_unlock(&ss->files_lock);
        send_error_response(fd, ERR_FILE_NOT_FOUND, "File not found");
        return;
    }

    if (!ss_check_read_access(rec, username)) {
        pthread_mutex_unlock(&ss->files_lock);
        send_error_response(fd, ERR_PERMISSION_DENIED, "No read access");
        return;
    }

    pthread_mutex_lock(&rec->file_lock);
    pthread_mutex_unlock(&ss->files_lock);

    char *content = ss_read_file_content(rec->filepath);
    if (!content) {
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_INTERNAL_ERROR, "Failed to read file");
        return;
    }

    const char *start_fields[] = {RESP_OK_READ_START, rec->filename};
    char *start_msg = protocol_build_message(start_fields, 2);
    if (start_msg) {
        protocol_send_message(fd, start_msg);
        free(start_msg);
    }

    const char *content_fields[] = {RESP_OK_CONTENT, content};
    char *content_msg = protocol_build_message(content_fields, 2);
    if (content_msg) {
        protocol_send_message(fd, content_msg);
        free(content_msg);
    }

    const char *end_fields[] = {RESP_OK_READ_END};
    char *end_msg = protocol_build_message(end_fields, 1);
    if (end_msg) {
        protocol_send_message(fd, end_msg);
        free(end_msg);
    }

    ss_touch_file_metadata(ss, rec, username, 0);
    pthread_mutex_unlock(&rec->file_lock);
    free(content);
}

static void handle_client_stream(StorageServer *ss, int fd, ProtocolMessage *msg) {
    if (msg->field_count < 3) {
        send_error_response(fd, ERR_INVALID_REQUEST, "Missing fields in REQ_STREAM");
        return;
    }

    const char *username = msg->fields[1];
    const char *filename = msg->fields[2];

    pthread_mutex_lock(&ss->files_lock);
    FileRecord *rec = ss_find_file(ss, filename);
    if (!rec) {
        pthread_mutex_unlock(&ss->files_lock);
        send_error_response(fd, ERR_FILE_NOT_FOUND, "File not found");
        return;
    }

    if (!ss_check_read_access(rec, username)) {
        pthread_mutex_unlock(&ss->files_lock);
        send_error_response(fd, ERR_PERMISSION_DENIED, "No read access");
        return;
    }

    pthread_mutex_lock(&rec->file_lock);
    pthread_mutex_unlock(&ss->files_lock);

    char *content = ss_read_file_content(rec->filepath);
    if (!content) {
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_INTERNAL_ERROR, "Failed to read file");
        return;
    }

    const char *start_fields[] = {RESP_OK_STREAM, rec->filename};
    char *start_msg = protocol_build_message(start_fields, 2);
    if (start_msg) {
        protocol_send_message(fd, start_msg);
        free(start_msg);
    }

    const char *cursor = content;
    while (*cursor) {
        while (*cursor && isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (!*cursor) {
            break;
        }

        const char *word_start = cursor;
        while (*cursor && !isspace((unsigned char)*cursor)) {
            cursor++;
        }
        size_t word_len = (size_t)(cursor - word_start);
        if (word_len == 0) {
            continue;
        }

        char *word = (char *)safe_malloc(word_len + 1);
        if (!word) {
            break;
        }
        memcpy(word, word_start, word_len);
        word[word_len] = '\0';

        const char *word_fields[] = {RESP_OK_CONTENT, word};
        char *word_msg = protocol_build_message(word_fields, 2);
        if (word_msg) {
            protocol_send_message(fd, word_msg);
            free(word_msg);
        }
        free(word);
        usleep(100000);
    }

    const char *end_fields[] = {RESP_OK_STREAM_END};
    char *end_msg = protocol_build_message(end_fields, 1);
    if (end_msg) {
        protocol_send_message(fd, end_msg);
        free(end_msg);
    }

    ss_touch_file_metadata(ss, rec, username, 0);
    pthread_mutex_unlock(&rec->file_lock);
    free(content);
}

static void handle_ss_copy(StorageServer *ss, ProtocolMessage *msg) {
    if (!ss || !msg) {
        return;
    }

    if (msg->field_count < 6) {
        ss_send_ns_error(ss, ERR_INVALID_REQUEST, "SS_COPY missing fields");
        return;
    }

    const char *dest_filename = msg->fields[1];
    const char *src_ip = msg->fields[2];
    const char *src_port_str = msg->fields[3];
    const char *src_filename = msg->fields[4];
    const char *username = msg->fields[5];

    if (!validate_filename(dest_filename) ||
        !validate_filename(src_filename) ||
        !validate_username(username)) {
        ss_send_ns_error(ss, ERR_INVALID_REQUEST, "Invalid COPY request payload");
        return;
    }

    int source_is_local = (strcmp(src_ip, "LOCAL") == 0);
    int src_port = 0;
    if (!source_is_local) {
        if (parse_int(src_port_str, &src_port) != 0 || src_port <= 0) {
            ss_send_ns_error(ss, ERR_INVALID_REQUEST, "Invalid source endpoint");
            return;
        }
    }

    pthread_mutex_lock(&ss->files_lock);
    FileRecord *dest_rec = ss_find_file(ss, dest_filename);
    FileRecord *src_rec = NULL;
    if (source_is_local) {
        src_rec = ss_find_file(ss, src_filename);
    }

    if (!dest_rec) {
        pthread_mutex_unlock(&ss->files_lock);
        ss_send_ns_error(ss, ERR_FILE_NOT_FOUND, "Destination file not found");
        return;
    }
    if (source_is_local && !src_rec) {
        pthread_mutex_unlock(&ss->files_lock);
        ss_send_ns_error(ss, ERR_FILE_NOT_FOUND, "Source file not found");
        return;
    }

    FileRecord *first_lock = dest_rec;
    FileRecord *second_lock = NULL;
    if (source_is_local && src_rec && src_rec != dest_rec) {
        second_lock = src_rec;
        if (first_lock > second_lock) {
            FileRecord *tmp = first_lock;
            first_lock = second_lock;
            second_lock = tmp;
        }
    }

    pthread_mutex_lock(&first_lock->file_lock);
    if (second_lock) {
        pthread_mutex_lock(&second_lock->file_lock);
    }
    pthread_mutex_unlock(&ss->files_lock);

    int src_locked = (source_is_local && src_rec && src_rec != dest_rec);

    pthread_mutex_lock(&ss->sessions_lock);
    int dest_busy = ss_file_has_active_writers(ss, dest_rec);
    int src_busy = (source_is_local && src_rec) ? ss_file_has_active_writers(ss, src_rec) : 0;
    pthread_mutex_unlock(&ss->sessions_lock);

    if (dest_busy || src_busy) {
        if (src_locked) {
            pthread_mutex_unlock(&src_rec->file_lock);
        }
        pthread_mutex_unlock(&dest_rec->file_lock);
        if (dest_busy) {
            ss_send_ns_error(ss, ERR_SENTENCE_LOCKED, "Destination locked for writing");
        } else {
            ss_send_ns_error(ss, ERR_SENTENCE_LOCKED, "Source locked for writing");
        }
        return;
    }

    char temp_path[MAX_PATH_LENGTH];
    snprintf(temp_path, sizeof(temp_path), "%s.copytmp", dest_rec->filepath);
    unlink(temp_path);

    int success = 0;
    int failure_code = ERR_INTERNAL_ERROR;
    const char *failure_msg = "Failed to copy file content";

    if (source_is_local) {
        if (ss_copy_file(src_rec->filepath, temp_path) == 0) {
            success = 1;
        } else {
            failure_msg = "Failed to clone local source file";
        }
        if (src_locked) {
            pthread_mutex_unlock(&src_rec->file_lock);
            src_locked = 0;
        }
    } else {
        int peer_fd = ss_connect_to_peer(src_ip, src_port);
        if (peer_fd < 0) {
            failure_msg = "Failed to reach source server";
        } else {
            const char *req_fields[] = {MSG_REQ_READ, username, src_filename};
            char *req = protocol_build_message(req_fields, 3);
            int request_sent = (req && protocol_send_message(peer_fd, req) >= 0);
            if (req) {
                free(req);
            }

            if (!request_sent) {
                failure_msg = "Failed to request source content";
                close(peer_fd);
            } else {
                FILE *fp = ss_open_truncating_binary(temp_path);
                if (!fp) {
                    failure_msg = "Failed to create temporary file";
                    close(peer_fd);
                } else {
                    int stream_error = 0;
                    while (1) {
                        char *raw = protocol_receive_message(peer_fd);
                        if (!raw) {
                            stream_error = 1;
                            failure_msg = "Connection dropped during COPY";
                            break;
                        }

                        ProtocolMessage peer_msg;
                        if (protocol_parse_message(raw, &peer_msg) != 0 || peer_msg.field_count == 0) {
                            free(raw);
                            stream_error = 1;
                            failure_msg = "Invalid data from source server";
                            break;
                        }

                        if (protocol_is_error(&peer_msg)) {
                            int peer_code = protocol_get_error_code(&peer_msg);
                            if (peer_code == ERR_FILE_NOT_FOUND) {
                                failure_code = ERR_FILE_NOT_FOUND;
                                failure_msg = "Source file not found";
                            } else if (peer_code == ERR_PERMISSION_DENIED) {
                                failure_code = ERR_PERMISSION_DENIED;
                                failure_msg = "Source access denied";
                            } else {
                                failure_msg = "Source server rejected COPY";
                            }
                            protocol_free_message(&peer_msg);
                            free(raw);
                            stream_error = 1;
                            break;
                        }

                        const char *type = peer_msg.fields[0];
                        if (strcmp(type, RESP_OK_CONTENT) == 0 && peer_msg.field_count >= 2) {
                            const char *chunk = peer_msg.fields[1] ? peer_msg.fields[1] : "";
                            size_t len = strlen(chunk);
                            if (len > 0 && fwrite(chunk, 1, len, fp) != len) {
                                failure_msg = "Failed to persist stream chunk";
                                protocol_free_message(&peer_msg);
                                free(raw);
                                stream_error = 1;
                                break;
                            }
                        } else if (strcmp(type, RESP_OK_READ_END) == 0) {
                            protocol_free_message(&peer_msg);
                            free(raw);
                            success = 1;
                            break;
                        }

                        protocol_free_message(&peer_msg);
                        free(raw);
                    }

                    fclose(fp);
                    close(peer_fd);
                    if (stream_error || !success) {
                        unlink(temp_path);
                    }
                }
            }
        }
    }

    if (success) {
        if (rename(temp_path, dest_rec->filepath) != 0) {
            failure_msg = "Failed to finalize destination file";
            success = 0;
        }
    }

    if (!success) {
        unlink(temp_path);
        pthread_mutex_unlock(&dest_rec->file_lock);
        ss_send_ns_error(ss, failure_code, failure_msg);
        return;
    }

    ss_touch_file_metadata(ss, dest_rec, username, 1);
    ss_sync_metadata_to_ns(ss, dest_rec);
    pthread_mutex_unlock(&dest_rec->file_lock);
    send_simple_ok(ss->ns_sockfd, "Copy successful");
}

static void handle_client_write_lock(StorageServer *ss, int fd, ProtocolMessage *msg) {
    if (msg->field_count < 4) {
        send_error_response(fd, ERR_INVALID_REQUEST, "Missing fields in REQ_WRITE_LOCK");
        return;
    }

    const char *username = msg->fields[1];
    const char *filename = msg->fields[2];
    int sentence_index = 0;
    if (parse_int(msg->fields[3], &sentence_index) != 0 || sentence_index < 0) {
        send_error_response(fd, ERR_INVALID_REQUEST, "Invalid sentence index");
        return;
    }

    ss_abort_session(ss, fd);

    pthread_mutex_lock(&ss->files_lock);
    FileRecord *rec = ss_find_file(ss, filename);
    if (!rec) {
        pthread_mutex_unlock(&ss->files_lock);
        send_error_response(fd, ERR_FILE_NOT_FOUND, "File not found");
        return;
    }

    if (!ss_check_write_access(rec, username)) {
        pthread_mutex_unlock(&ss->files_lock);
        send_error_response(fd, ERR_PERMISSION_DENIED, "No write access");
        return;
    }

    pthread_mutex_lock(&rec->file_lock);
    pthread_mutex_unlock(&ss->files_lock);

    pthread_mutex_lock(&ss->sessions_lock);
    WriteSession *existing = ss_find_session_by_sentence(ss, rec, sentence_index);
    pthread_mutex_unlock(&ss->sessions_lock);

    if (existing) {
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_SENTENCE_LOCKED, "Sentence already locked");
        return;
    }

    char *file_data = ss_read_file_content(rec->filepath);
    if (!file_data) {
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_INTERNAL_ERROR, "Failed to read file");
        return;
    }

    size_t start = 0, end = 0;
    int bound_status = ss_get_sentence_bounds(file_data, sentence_index, &start, &end);
    if (bound_status < 0) {
        free(file_data);
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_INVALID_REQUEST, "Sentence index out of range");
        return;
    }

    char *sentence_text = NULL;
    if (end > start) {
        size_t len = end - start;
        sentence_text = (char *)safe_malloc(len + 1);
        if (!sentence_text) {
            free(file_data);
            pthread_mutex_unlock(&rec->file_lock);
            send_error_response(fd, ERR_INTERNAL_ERROR, "Memory allocation failed");
            return;
        }
        memcpy(sentence_text, file_data + start, len);
        sentence_text[len] = '\0';
    } else {
        sentence_text = strdup("");
    }

    pthread_mutex_lock(&ss->sessions_lock);
    WriteSession *session = ss_create_session(ss, fd, rec, username, sentence_index, start, end, file_data, sentence_text);
    pthread_mutex_unlock(&ss->sessions_lock);

    free(file_data);
    if (sentence_text) {
        free(sentence_text);
    }

    if (!session) {
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_INTERNAL_ERROR, "Failed to create write session");
        return;
    }

    const char *lock_fields[] = {RESP_OK_LOCKED, rec->filename, msg->fields[3]};
    char *lock_msg = protocol_build_message(lock_fields, 3);
    if (lock_msg) {
        protocol_send_message(fd, lock_msg);
        free(lock_msg);
    }

    if (session->sentence_working) {
        const char *sent_fields[] = {RESP_OK_CONTENT, session->sentence_working};
        char *sent_msg = protocol_build_message(sent_fields, 2);
        if (sent_msg) {
            protocol_send_message(fd, sent_msg);
            free(sent_msg);
        }
    }

    pthread_mutex_unlock(&rec->file_lock);
}

static void handle_client_write_data(StorageServer *ss, int fd, ProtocolMessage *msg) {
    if (msg->field_count < 3) {
        send_error_response(fd, ERR_INVALID_REQUEST, "Missing fields in WRITE_DATA");
        return;
    }

    int word_index = 0;
    if (parse_int(msg->fields[1], &word_index) != 0 || word_index < 0) {
        send_error_response(fd, ERR_INVALID_REQUEST, "Invalid word index");
        return;
    }

    pthread_mutex_lock(&ss->sessions_lock);
    WriteSession *session = ss_find_session(ss, fd);
    pthread_mutex_unlock(&ss->sessions_lock);

    if (!session) {
        send_error_response(fd, ERR_INVALID_REQUEST, "No active WRITE session");
        return;
    }

    if (ss_apply_word_edit(session, word_index, msg->fields[2]) != 0) {
        send_error_response(fd, ERR_INVALID_REQUEST, "Failed to apply edit");
        return;
    }

    send_simple_ok(fd, "WRITE_DATA applied");
}

static int count_sentences_in_span(const char *text, size_t start, size_t end) {
    if (!text || start >= end) {
        return 0;
    }

    int count = 0;
    for (size_t i = start; i < end; i++) {
        if (is_sentence_delimiter((unsigned char)text[i])) {
            count++;
        }
    }

    if (count == 0) {
        count = 1;
    }

    return count;
}

static void ss_update_active_sessions(StorageServer *ss, FileRecord *rec,
                                      size_t edit_start, size_t old_len, size_t new_len,
                                      int sentence_delta, WriteSession *exclude_session) {
    ssize_t char_delta = (ssize_t)new_len - (ssize_t)old_len;
    size_t edit_end = edit_start + old_len;

    pthread_mutex_lock(&ss->sessions_lock);
    for (WriteSession *s = ss->sessions; s; s = s->next) {
        if (s == exclude_session || s->file != rec) {
            continue;
        }

        if (s->sentence_start >= edit_end) {
            s->sentence_start = (size_t)((ssize_t)s->sentence_start + char_delta);
            s->sentence_end = (size_t)((ssize_t)s->sentence_end + char_delta);
            if (sentence_delta != 0) {
                s->sentence_index += sentence_delta;
                if (s->sentence_index < 0) {
                    s->sentence_index = 0;
                }
            }
        }
    }
    pthread_mutex_unlock(&ss->sessions_lock);
}

static void handle_client_etirw(StorageServer *ss, int fd) {
    pthread_mutex_lock(&ss->sessions_lock);
    WriteSession *session = ss_find_session(ss, fd);
    if (!session) {
        pthread_mutex_unlock(&ss->sessions_lock);
        send_error_response(fd, ERR_INVALID_REQUEST, "No active WRITE session");
        return;
    }

    FileRecord *rec = session->file;
    size_t locked_start = session->sentence_start;
    size_t locked_end = session->sentence_end;
    pthread_mutex_unlock(&ss->sessions_lock);

    pthread_mutex_lock(&rec->file_lock);

    const char *replacement = session->sentence_working ? session->sentence_working : "";
    char username_copy[MAX_USERNAME_LENGTH];
    safe_strcpy(username_copy, session->username, sizeof(username_copy));

    char *current_snapshot = ss_read_file_content(rec->filepath);
    if (!current_snapshot) {
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_INTERNAL_ERROR, "Failed to read file");
        return;
    }

    size_t base_len = strlen(current_snapshot);
    if (locked_start > base_len || locked_end > base_len || locked_start > locked_end) {
        free(current_snapshot);
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_INTERNAL_ERROR, "Invalid sentence bounds");
        return;
    }

    size_t old_len = locked_end - locked_start;
    size_t replacement_len = strlen(replacement);

    int old_sentences = count_sentences_in_span(current_snapshot, locked_start, locked_end);
    int new_sentences = count_sentences_in_span(replacement, 0, replacement_len);
    int sentence_delta = new_sentences - old_sentences;

    size_t new_len = locked_start + replacement_len + (base_len - locked_end);
    char *new_content = (char *)safe_malloc(new_len + 1);
    if (!new_content) {
        free(current_snapshot);
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_INTERNAL_ERROR, "Memory allocation failed");
        return;
    }

    memcpy(new_content, current_snapshot, locked_start);
    memcpy(new_content + locked_start, replacement, replacement_len);
    memcpy(new_content + locked_start + replacement_len,
           current_snapshot + locked_end,
           base_len - locked_end);
    new_content[new_len] = '\0';

    if (ss_write_file_content(rec->undopath, current_snapshot) != 0) {
        free(new_content);
        free(current_snapshot);
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_INTERNAL_ERROR, "Failed to persist undo");
        return;
    }

    if (ss_write_file_content(rec->filepath, new_content) != 0) {
        free(new_content);
        free(current_snapshot);
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_INTERNAL_ERROR, "Failed to write file");
        return;
    }

    free(new_content);

    rec->undo_available = 1;
    ss_touch_file_metadata(ss, rec, session->username, 1);

    ss_update_active_sessions(ss, rec, locked_start, old_len, replacement_len, sentence_delta, session);

    pthread_mutex_unlock(&rec->file_lock);

    pthread_mutex_lock(&ss->sessions_lock);
    WriteSession *detached = ss_detach_session(ss, fd);
    ss_free_session(detached);
    pthread_mutex_unlock(&ss->sessions_lock);

    free(current_snapshot);

    const char *fields[] = {RESP_OK_WRITE_DONE};
    char *resp = protocol_build_message(fields, 1);
    if (resp) {
        protocol_send_message(fd, resp);
        free(resp);
    }

    log_message(LOG_INFO, "SS", "Write committed on %s by %s", rec->filename, username_copy);

    ss_sync_metadata_to_ns(ss, rec);
}

static void handle_client_checkpoint(StorageServer *ss, int fd, ProtocolMessage *msg) {
    if (msg->field_count < 4) {
        send_error_response(fd, ERR_INVALID_REQUEST, "Missing fields in REQ_CHECKPOINT");
        return;
    }

    const char *username = msg->fields[1];
    const char *filename = msg->fields[2];
    const char *tag = msg->fields[3];

    char tag_copy[MAX_TAG_LENGTH];
    if (safe_strcpy(tag_copy, tag, sizeof(tag_copy)) != 0) {
        send_error_response(fd, ERR_INVALID_REQUEST, "Checkpoint tag too long");
        return;
    }
    trim_string(tag_copy);
    if (tag_copy[0] == '\0') {
        send_error_response(fd, ERR_INVALID_REQUEST, "Checkpoint tag cannot be empty");
        return;
    }

    pthread_mutex_lock(&ss->files_lock);
    FileRecord *rec = ss_find_file(ss, filename);
    if (!rec) {
        pthread_mutex_unlock(&ss->files_lock);
        send_error_response(fd, ERR_FILE_NOT_FOUND, "File not found");
        return;
    }

    if (!ss_check_write_access(rec, username)) {
        pthread_mutex_unlock(&ss->files_lock);
        send_error_response(fd, ERR_PERMISSION_DENIED, "No write access");
        return;
    }

    pthread_mutex_lock(&rec->file_lock);
    pthread_mutex_unlock(&ss->files_lock);

    pthread_mutex_lock(&ss->sessions_lock);
    int writers_active = ss_file_has_active_writers(ss, rec);
    pthread_mutex_unlock(&ss->sessions_lock);
    if (writers_active) {
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_SENTENCE_LOCKED, "File locked for writing");
        return;
    }

    if (ss_find_checkpoint(rec, tag_copy)) {
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_INVALID_REQUEST, "Checkpoint already exists");
        return;
    }

    char sanitized[MAX_TAG_LENGTH];
    ss_sanitize_tag(tag_copy, sanitized, sizeof(sanitized));
    if (sanitized[0] == '\0') {
        safe_strcpy(sanitized, "checkpoint", sizeof(sanitized));
    }

    char ckpt_filename[MAX_FILENAME_LENGTH + MAX_TAG_LENGTH + 32];
    snprintf(ckpt_filename, sizeof(ckpt_filename), "%s.ckpt_%lld_%s",
             rec->filename, (long long)get_current_time(), sanitized);

    char ckpt_path[MAX_PATH_LENGTH];
    if (build_path(ckpt_path, sizeof(ckpt_path), ss->storage_path, ckpt_filename) != 0) {
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_INTERNAL_ERROR, "Failed to build checkpoint path");
        return;
    }

    if (ss_copy_file(rec->filepath, ckpt_path) != 0) {
        unlink(ckpt_path);
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_INTERNAL_ERROR, "Failed to persist checkpoint");
        return;
    }

    if (rec->checkpoint_count >= SS_MAX_CHECKPOINTS) {
        ss_prune_oldest_checkpoint(rec);
    }

    CheckpointRecord *slot = &rec->checkpoints[rec->checkpoint_count++];
    safe_strcpy(slot->tag, tag_copy, sizeof(slot->tag));
    safe_strcpy(slot->filepath, ckpt_path, sizeof(slot->filepath));

    ss_touch_file_metadata(ss, rec, username, 0);
    pthread_mutex_unlock(&rec->file_lock);

    char message[256];
    snprintf(message, sizeof(message), "Checkpoint '%s' created.", tag_copy);
    const char *fields[] = {RESP_OK_CHECKPOINT, message};
    char *resp = protocol_build_message(fields, 2);
    if (resp) {
        protocol_send_message(fd, resp);
        free(resp);
    }

    log_message(LOG_INFO, "SS", "Checkpoint %s created for %s by %s", tag_copy, rec->filename, username);
}

static void handle_client_viewcheckpoint(StorageServer *ss, int fd, ProtocolMessage *msg) {
    if (msg->field_count < 4) {
        send_error_response(fd, ERR_INVALID_REQUEST, "Missing fields in REQ_VIEWCHECKPOINT");
        return;
    }

    const char *username = msg->fields[1];
    const char *filename = msg->fields[2];
    const char *tag = msg->fields[3];

    char tag_copy[MAX_TAG_LENGTH];
    if (safe_strcpy(tag_copy, tag, sizeof(tag_copy)) != 0) {
        send_error_response(fd, ERR_INVALID_REQUEST, "Checkpoint tag too long");
        return;
    }
    trim_string(tag_copy);
    if (!tag_copy[0]) {
        send_error_response(fd, ERR_INVALID_REQUEST, "Checkpoint tag cannot be empty");
        return;
    }

    pthread_mutex_lock(&ss->files_lock);
    FileRecord *rec = ss_find_file(ss, filename);
    if (!rec) {
        pthread_mutex_unlock(&ss->files_lock);
        send_error_response(fd, ERR_FILE_NOT_FOUND, "File not found");
        return;
    }

    if (!ss_check_read_access(rec, username)) {
        pthread_mutex_unlock(&ss->files_lock);
        send_error_response(fd, ERR_PERMISSION_DENIED, "No read access");
        return;
    }

    pthread_mutex_lock(&rec->file_lock);
    pthread_mutex_unlock(&ss->files_lock);

    CheckpointRecord *ckpt = ss_find_checkpoint(rec, tag_copy);
    if (!ckpt) {
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_INVALID_REQUEST, "Checkpoint not found");
        return;
    }

    if (!ss_file_exists(ckpt->filepath)) {
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_INTERNAL_ERROR, "Checkpoint data missing");
        return;
    }

    char *content = ss_read_file_content(ckpt->filepath);
    if (!content) {
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_INTERNAL_ERROR, "Failed to read checkpoint");
        return;
    }

    char display_name[MAX_FILENAME_LENGTH + MAX_TAG_LENGTH + 4];
    snprintf(display_name, sizeof(display_name), "%s [%s]", rec->filename, tag_copy);

    const char *start_fields[] = {RESP_OK_READ_START, display_name};
    char *start_msg = protocol_build_message(start_fields, 2);
    if (start_msg) {
        protocol_send_message(fd, start_msg);
        free(start_msg);
    }

    const char *content_fields[] = {RESP_OK_CONTENT, content};
    char *content_msg = protocol_build_message(content_fields, 2);
    if (content_msg) {
        protocol_send_message(fd, content_msg);
        free(content_msg);
    }

    const char *end_fields[] = {RESP_OK_READ_END};
    char *end_msg = protocol_build_message(end_fields, 1);
    if (end_msg) {
        protocol_send_message(fd, end_msg);
        free(end_msg);
    }

    ss_touch_file_metadata(ss, rec, username, 0);
    pthread_mutex_unlock(&rec->file_lock);
    free(content);
}

static void handle_client_revert(StorageServer *ss, int fd, ProtocolMessage *msg) {
    if (msg->field_count < 4) {
        send_error_response(fd, ERR_INVALID_REQUEST, "Missing fields in REQ_REVERT");
        return;
    }

    const char *username = msg->fields[1];
    const char *filename = msg->fields[2];
    const char *tag = msg->fields[3];

    char tag_copy[MAX_TAG_LENGTH];
    if (safe_strcpy(tag_copy, tag, sizeof(tag_copy)) != 0) {
        send_error_response(fd, ERR_INVALID_REQUEST, "Checkpoint tag too long");
        return;
    }
    trim_string(tag_copy);
    if (!tag_copy[0]) {
        send_error_response(fd, ERR_INVALID_REQUEST, "Checkpoint tag cannot be empty");
        return;
    }

    pthread_mutex_lock(&ss->files_lock);
    FileRecord *rec = ss_find_file(ss, filename);
    if (!rec) {
        pthread_mutex_unlock(&ss->files_lock);
        send_error_response(fd, ERR_FILE_NOT_FOUND, "File not found");
        return;
    }

    if (!ss_check_write_access(rec, username)) {
        pthread_mutex_unlock(&ss->files_lock);
        send_error_response(fd, ERR_PERMISSION_DENIED, "No write access");
        return;
    }

    pthread_mutex_lock(&rec->file_lock);
    pthread_mutex_unlock(&ss->files_lock);

    pthread_mutex_lock(&ss->sessions_lock);
    int writers_active = ss_file_has_active_writers(ss, rec);
    pthread_mutex_unlock(&ss->sessions_lock);
    if (writers_active) {
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_SENTENCE_LOCKED, "File locked for writing");
        return;
    }

    CheckpointRecord *ckpt = ss_find_checkpoint(rec, tag_copy);
    if (!ckpt) {
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_INVALID_REQUEST, "Checkpoint not found");
        return;
    }

    if (!ss_file_exists(ckpt->filepath)) {
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_INTERNAL_ERROR, "Checkpoint data missing");
        return;
    }

    if (ss_copy_file(rec->filepath, rec->undopath) != 0) {
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_INTERNAL_ERROR, "Failed to create undo snapshot");
        return;
    }

    if (ss_copy_file(ckpt->filepath, rec->filepath) != 0) {
        ss_copy_file(rec->undopath, rec->filepath);
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_INTERNAL_ERROR, "Failed to revert file");
        return;
    }

    rec->undo_available = 1;
    ss_touch_file_metadata(ss, rec, username, 1);
    pthread_mutex_unlock(&rec->file_lock);

    char message[256];
    snprintf(message, sizeof(message), "File reverted to '%s'.", tag_copy);
    const char *fields[] = {RESP_OK_REVERT, message};
    char *resp = protocol_build_message(fields, 2);
    if (resp) {
        protocol_send_message(fd, resp);
        free(resp);
    }

    log_message(LOG_INFO, "SS", "File %s reverted to %s by %s", rec->filename, tag_copy, username);
}

static void handle_client_listcheckpoints(StorageServer *ss, int fd, ProtocolMessage *msg) {
    if (msg->field_count < 3) {
        send_error_response(fd, ERR_INVALID_REQUEST, "Missing fields in REQ_LIST_CHECKPOINTS");
        return;
    }

    const char *username = msg->fields[1];
    const char *filename = msg->fields[2];

    pthread_mutex_lock(&ss->files_lock);
    FileRecord *rec = ss_find_file(ss, filename);
    if (!rec) {
        pthread_mutex_unlock(&ss->files_lock);
        send_error_response(fd, ERR_FILE_NOT_FOUND, "File not found");
        return;
    }

    if (!ss_check_read_access(rec, username)) {
        pthread_mutex_unlock(&ss->files_lock);
        send_error_response(fd, ERR_PERMISSION_DENIED, "No read access");
        return;
    }

    pthread_mutex_lock(&rec->file_lock);
    pthread_mutex_unlock(&ss->files_lock);

    for (int i = 0; i < rec->checkpoint_count; i++) {
        const char *fields[] = {RESP_OK_LIST_CHECKPOINT, rec->checkpoints[i].tag};
        char *msg_line = protocol_build_message(fields, 2);
        if (msg_line) {
            protocol_send_message(fd, msg_line);
            free(msg_line);
        }
    }

    const char *end_fields[] = {RESP_OK_LIST_CHECKPOINT_END};
    char *end_msg = protocol_build_message(end_fields, 1);
    if (end_msg) {
        protocol_send_message(fd, end_msg);
        free(end_msg);
    }

    ss_touch_file_metadata(ss, rec, username, 0);
    pthread_mutex_unlock(&rec->file_lock);
}

static void handle_client_undo(StorageServer *ss, int fd, ProtocolMessage *msg) {
    if (msg->field_count < 3) {
        send_error_response(fd, ERR_INVALID_REQUEST, "Missing fields in REQ_UNDO");
        return;
    }

    const char *username = msg->fields[1];
    const char *filename = msg->fields[2];

    pthread_mutex_lock(&ss->files_lock);
    FileRecord *rec = ss_find_file(ss, filename);
    if (!rec) {
        pthread_mutex_unlock(&ss->files_lock);
        send_error_response(fd, ERR_FILE_NOT_FOUND, "File not found");
        return;
    }

    if (!ss_check_write_access(rec, username)) {
        pthread_mutex_unlock(&ss->files_lock);
        send_error_response(fd, ERR_PERMISSION_DENIED, "No write access");
        return;
    }

    pthread_mutex_lock(&rec->file_lock);
    pthread_mutex_unlock(&ss->files_lock);

    pthread_mutex_lock(&ss->sessions_lock);
    int writers_active = ss_file_has_active_writers(ss, rec);
    pthread_mutex_unlock(&ss->sessions_lock);

    if (writers_active) {
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_SENTENCE_LOCKED, "File locked for writing");
        return;
    }

    if (!rec->undo_available || !file_exists(rec->undopath)) {
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_NO_UNDO_HISTORY, "No undo available");
        return;
    }

    char *undo_content = ss_read_file_content(rec->undopath);
    if (!undo_content) {
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_INTERNAL_ERROR, "Failed to read undo data");
        return;
    }

    if (ss_write_file_content(rec->filepath, undo_content) != 0) {
        free(undo_content);
        pthread_mutex_unlock(&rec->file_lock);
        send_error_response(fd, ERR_INTERNAL_ERROR, "Failed to restore file");
        return;
    }

    free(undo_content);
    rec->undo_available = 0;
    ss_touch_file_metadata(ss, rec, username, 1);

    pthread_mutex_unlock(&rec->file_lock);

    const char *fields[] = {RESP_OK_UNDO};
    char *resp = protocol_build_message(fields, 1);
    if (resp) {
        protocol_send_message(fd, resp);
        free(resp);
    }
}

static void *client_thread(void *arg) {
    ClientThreadArgs *params = (ClientThreadArgs *)arg;
    StorageServer *ss = params->ss;
    int fd = params->client_fd;
    char client_ip[INET_ADDRSTRLEN];
    int client_port = params->client_port;
    strncpy(client_ip, params->client_ip, sizeof(client_ip));
    free(params);

    while (ss->running) {
        char *raw = protocol_receive_message(fd);
        if (!raw) {
            break;
        }

        ProtocolMessage msg;
        if (protocol_parse_message(raw, &msg) != 0 || msg.field_count == 0) {
            free(raw);
            continue;
        }

        const char *command = msg.fields[0];

        if (strcmp(command, MSG_REQ_READ) == 0) {
            handle_client_read(ss, fd, &msg);
        } else if (strcmp(command, MSG_REQ_STREAM) == 0) {
            handle_client_stream(ss, fd, &msg);
        } else if (strcmp(command, MSG_REQ_WRITE_LOCK) == 0) {
            handle_client_write_lock(ss, fd, &msg);
        } else if (strcmp(command, MSG_WRITE_DATA) == 0) {
            handle_client_write_data(ss, fd, &msg);
        } else if (strcmp(command, MSG_ETIRW) == 0) {
            handle_client_etirw(ss, fd);
        } else if (strcmp(command, MSG_REQ_UNDO) == 0) {
            handle_client_undo(ss, fd, &msg);
        } else if (strcmp(command, MSG_REQ_CHECKPOINT) == 0) {
            handle_client_checkpoint(ss, fd, &msg);
        } else if (strcmp(command, MSG_REQ_VIEWCHECKPOINT) == 0) {
            handle_client_viewcheckpoint(ss, fd, &msg);
        } else if (strcmp(command, MSG_REQ_REVERT) == 0) {
            handle_client_revert(ss, fd, &msg);
        } else if (strcmp(command, MSG_REQ_LIST_CHECKPOINTS) == 0) {
            handle_client_listcheckpoints(ss, fd, &msg);
        } else {
            send_error_response(fd, ERR_INVALID_REQUEST, "Unsupported command");
        }

        protocol_free_message(&msg);
        free(raw);
    }

    ss_abort_session(ss, fd);
    close(fd);
    log_message(LOG_INFO, "SS", "Client %s:%d disconnected", client_ip, client_port);
    return NULL;
}

int ss_init(StorageServer *ss, const char *ns_ip, int ns_port,
            const char *client_ip, int client_port) {
    if (!ss || !ns_ip || !client_ip) {
        return -1;
    }
    
    // Copy configuration
    strncpy(ss->ns_ip, ns_ip, MAX_IP_LENGTH - 1);
    ss->ns_port = ns_port;
    strncpy(ss->client_ip, client_ip, MAX_IP_LENGTH - 1);
    ss->client_port = client_port;
    
    // Set storage path (unique per storage server instance)
    if (ss_build_storage_path(ss->storage_path, sizeof(ss->storage_path), ss->client_ip, ss->client_port) != 0) {
        log_message(LOG_ERROR, "SS", "Failed to derive storage path for %s:%d", ss->client_ip, ss->client_port);
        return -1;
    }
    
    // Create storage directory
    if (create_directory_recursive(ss->storage_path) != 0) {
        fprintf(stderr, "Failed to create storage directory\n");
        return -1;
    }
    
    // Initialize state
    ss->ns_sockfd = -1;
    ss->client_sockfd = -1;
    ss->client_listen_fd = -1;
    ss->files = NULL;
    ss->running = 0;
    pthread_mutex_init(&ss->files_lock, NULL);
    pthread_mutex_init(&ss->sessions_lock, NULL);
    ss->sessions = NULL;
    ss->ns_thread_active = 0;
    ss->ns_thread_started = 0;

    ss_load_existing_files(ss);
    
    log_message(LOG_INFO, "SS", "Storage server initialized");
    return 0;
}

int ss_register_with_ns(StorageServer *ss) {
    if (!ss) {
        return -1;
    }

    if (ss_open_ns_socket(ss) != 0) {
        return -1;
    }

    if (ss_send_hello(ss) != 0) {
        close(ss->ns_sockfd);
        ss->ns_sockfd = -1;
        return -1;
    }

    log_message(LOG_INFO, "SS", "Registered with Name Server");
    return 0;
}

int ss_send_file_list(StorageServer *ss) {
    if (!ss) {
        return -1;
    }

    if (ss_emit_file_list(ss) != 0) {
        return -1;
    }

    log_message(LOG_INFO, "SS", "File list sent to Name Server");
    return 0;
}

int ss_start(StorageServer *ss) {
    if (!ss) {
        return -1;
    }

    if (ss_open_client_listener(ss) != 0) {
        return -1;
    }

    ss->running = 1;
    if (ss_start_ns_thread(ss) != 0) {
        ss->running = 0;
        if (ss->client_listen_fd >= 0) {
            close(ss->client_listen_fd);
            ss->client_listen_fd = -1;
        }
        return -1;
    }

    log_message(LOG_INFO, "SS", "Storage server started on %s:%d", ss->client_ip, ss->client_port);
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        log_message(LOG_ERROR, "NS", "Failed to init thread attributes: %s", strerror(errno));
    }

    size_t stack_size = 4 * 1024 * 1024; 
    if (pthread_attr_setstacksize(&attr, stack_size) != 0) {
        log_message(LOG_ERROR, "NS", "Failed to set stack size: %s", strerror(errno));
    }
    while (ss->running) {
        struct sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        int fd = accept(ss->client_listen_fd, (struct sockaddr *)&client_addr, &len);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_message(LOG_WARNING, "SS", "Accept failed: %s", strerror(errno));
            continue;
        }

        ClientThreadArgs *args = (ClientThreadArgs *)safe_malloc(sizeof(ClientThreadArgs));
        if (!args) {
            close(fd);
            continue;
        }

        args->ss = ss;
        args->client_fd = fd;
        inet_ntop(AF_INET, &client_addr.sin_addr, args->client_ip, sizeof(args->client_ip));
        args->client_port = ntohs(client_addr.sin_port);

        log_message(LOG_INFO, "SS", "Client connected from %s:%d", args->client_ip, args->client_port);

        pthread_t t;
        if (pthread_create(&t, NULL, client_thread, args) != 0) {
            log_message(LOG_WARNING, "SS", "Failed to spawn client thread");
            close(fd);
            free(args);
            continue;
        }
        pthread_detach(t);
    }

    return 0;
}

void ss_cleanup(StorageServer *ss) {
    if (!ss) {
        return;
    }
    
    ss->running = 0;

    if (ss->client_listen_fd >= 0) {
        close(ss->client_listen_fd);
        ss->client_listen_fd = -1;
    }

    if (ss->ns_sockfd >= 0) {
        close(ss->ns_sockfd);
        ss->ns_sockfd = -1;
    }

    if (ss->client_sockfd >= 0) {
        close(ss->client_sockfd);
        ss->client_sockfd = -1;
    }

    if (ss->ns_thread_started) {
        pthread_join(ss->ns_thread, NULL);
        ss->ns_thread_started = 0;
    }

    pthread_mutex_lock(&ss->sessions_lock);
    WriteSession *pending = ss->sessions;
    ss->sessions = NULL;
    pthread_mutex_unlock(&ss->sessions_lock);

    while (pending) {
        WriteSession *next = pending->next;
        ss_free_session(pending);
        pending = next;
    }

    pthread_mutex_destroy(&ss->files_lock);
    pthread_mutex_destroy(&ss->sessions_lock);
    ss_free_file_records(ss->files);
    ss->files = NULL;

    log_message(LOG_INFO, "SS", "Storage server cleanup complete");
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <ns_ip> <ns_port> <client_ip> <client_port>\n", argv[0]);
        return 1;
    }
    
    // Initialize logging
    log_init("storage_server.log", LOG_INFO);
    log_set_console(0);
    
    // Create storage server
    StorageServer ss;
    if (ss_init(&ss, argv[1], atoi(argv[2]), argv[3], atoi(argv[4])) != 0) {
        fprintf(stderr, "Failed to initialize storage server\n");
        return 1;
    }
    
    // Register with Name Server
    if (ss_register_with_ns(&ss) != 0) {
        fprintf(stderr, "Failed to register with Name Server\n");
        ss_cleanup(&ss);
        return 1;
    }
    
    // Send file list
    if (ss_send_file_list(&ss) != 0) {
        fprintf(stderr, "Failed to send file list\n");
        ss_cleanup(&ss);
        return 1;
    }
    
    // Start server
    printf("Storage Server running...\n");
    if (ss_start(&ss) != 0) {
        fprintf(stderr, "Server error\n");
        ss_cleanup(&ss);
        return 1;
    }
    
    // Cleanup
    ss_cleanup(&ss);
    log_cleanup();
    
    return 0;
}
