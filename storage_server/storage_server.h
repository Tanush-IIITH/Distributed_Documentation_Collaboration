#ifndef STORAGE_SERVER_H
#define STORAGE_SERVER_H

#include "../common/protocol.h"
#include "../common/utils.h"
#include <pthread.h>

// Storage Server configuration
#define SS_MAX_FILES 10000
#define SS_STORAGE_PATH "./ss_storage/"
#define SS_META_SUFFIX ".meta"
#define SS_UNDO_SUFFIX ".undo"
#define SS_MAX_USERS_PER_FILE 128

typedef struct FileRecord {
    char filename[MAX_FILENAME_LENGTH];
    char filepath[MAX_PATH_LENGTH];
    char metapath[MAX_PATH_LENGTH];
    char undopath[MAX_PATH_LENGTH];
    char owner[MAX_USERNAME_LENGTH];
    char read_users[SS_MAX_USERS_PER_FILE][MAX_USERNAME_LENGTH];
    int read_count;
    char write_users[SS_MAX_USERS_PER_FILE][MAX_USERNAME_LENGTH];
    int write_count;
    time_t created;
    time_t modified;
    time_t last_access;
    char last_access_user[MAX_USERNAME_LENGTH];
    int undo_available;
    pthread_mutex_t file_lock;
    int sentence_locked;
    int locked_sentence_index;
    char lock_owner[MAX_USERNAME_LENGTH];
    struct FileRecord *next;
} FileRecord;

typedef struct WriteSession {
    int client_fd;
    FileRecord *file;
    char username[MAX_USERNAME_LENGTH];
    int sentence_index;
    size_t sentence_start;
    size_t sentence_end;
    char *file_snapshot;
    char *sentence_working;
    struct WriteSession *next;
} WriteSession;

// Storage Server structure
typedef struct {
    char ns_ip[MAX_IP_LENGTH];
    int ns_port;
    char client_ip[MAX_IP_LENGTH];
    int client_port;
    int ns_sockfd;
    int client_sockfd;
    char storage_path[MAX_PATH_LENGTH];
    int client_listen_fd;
    pthread_mutex_t files_lock;
    FileRecord *files;
    pthread_t ns_thread;
    int running;
    pthread_mutex_t sessions_lock;
    WriteSession *sessions;
    int ns_thread_active;
    int ns_thread_started;
} StorageServer;

/**
 * Initialize the storage server
 * Returns 0 on success, -1 on error
 */
int ss_init(StorageServer *ss, const char *ns_ip, int ns_port, 
            const char *client_ip, int client_port);

/**
 * Register with the Name Server
 * Returns 0 on success, -1 on error
 */
int ss_register_with_ns(StorageServer *ss);

/**
 * Send file list to Name Server
 * Returns 0 on success, -1 on error
 */
int ss_send_file_list(StorageServer *ss);

/**
 * Start the storage server
 * Returns 0 on success, -1 on error
 */
int ss_start(StorageServer *ss);

/**
 * Cleanup and shutdown
 */
void ss_cleanup(StorageServer *ss);

#endif // STORAGE_SERVER_H
