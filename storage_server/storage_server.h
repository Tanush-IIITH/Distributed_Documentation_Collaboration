/**
 * @file storage_server.h
 * @brief Public interface and data structures for the Storage Server (SS).
 *
 * The Storage Server is responsible for:
 *   - Persisting actual file content to disk (in a flat directory per SS instance).
 *   - Maintaining per-file metadata (.meta) and undo snapshots (.undo) as
 *     sidecar files alongside each document.
 *   - Enforcing sentence-level write locking to allow concurrent users to
 *     safely edit different sentences of the same file.
 *   - Managing named checkpoints (tagged snapshots) for reverting files.
 *   - Propagating file data to other SS instances during COPY operations.
 *
 * Storage layout:
 *   ./ss_storage/server_<ip>_<port>/
 *     <physical_name>        <- file content
 *     <physical_name>.meta   <- metadata (owner, ACL, timestamps, checkpoints)
 *     <physical_name>.undo   <- pre-write snapshot for the UNDO command
 *     <physical_name>.ckpt_<tag>  <- named checkpoint snapshots
 *
 * Physical names are produced by flatten_logical_path(), which replaces '/'
 * with '_' so that hierarchical logical paths become flat filenames.
 *
 * Concurrency model:
 *   - An NS control thread (ns_control_loop) handles commands from the NS.
 *   - One thread per client connection handles REQ_READ / REQ_WRITE_LOCK etc.
 *   - files_lock protects the linked list of FileRecord entries.
 *   - Each FileRecord has its own file_lock for sentence-level operations.
 *   - sessions_lock protects the active WriteSession linked list.
 */
#ifndef STORAGE_SERVER_H
#define STORAGE_SERVER_H

#include "../common/protocol.h"
#include "../common/utils.h"
#include <pthread.h>

/** Maximum number of file records held in memory by a single SS instance. */
#define SS_MAX_FILES 10000
/** Root directory under which per-instance storage subdirectories are created. */
#define SS_STORAGE_PATH "./ss_storage/"
/** File extension for metadata sidecar files (e.g., "report.txt.meta"). */
#define SS_META_SUFFIX ".meta"
/** File extension for undo snapshot sidecar files (e.g., "report.txt.undo"). */
#define SS_UNDO_SUFFIX ".undo"
/** Maximum number of users that can be listed in a file's read or write ACL. */
#define SS_MAX_USERS_PER_FILE 128
/** Maximum number of named checkpoints per file (oldest is evicted on overflow). */
#define SS_MAX_CHECKPOINTS 32

/**
 * @brief A named point-in-time snapshot of a file.
 *
 * Created by the CHECKPOINT command.  The snapshot is stored as a separate
 * file on disk (path stored in filepath).  The tag is a sanitized user-
 * supplied label.  Up to SS_MAX_CHECKPOINTS checkpoints are kept; the oldest
 * is evicted (and its file deleted) when the limit is exceeded.
 */
typedef struct {
    char tag[MAX_FILENAME_LENGTH];   /**< Sanitized user-supplied checkpoint label. */
    char filepath[MAX_PATH_LENGTH];  /**< Absolute path to the snapshot file on disk. */
} CheckpointRecord;

/**
 * @brief In-memory metadata record for a single file on a Storage Server.
 *
 * One FileRecord is created for each file the SS manages.  Records are
 * linked in a singly-linked list (ss->files) protected by ss->files_lock.
 * The per-file file_lock allows concurrent readers while serializing writes.
 *
 * Path layout:
 *   filepath  <- actual file content
 *   metapath  <- filename + SS_META_SUFFIX (persistent metadata)
 *   undopath  <- filename + SS_UNDO_SUFFIX (pre-write snapshot for UNDO)
 */
typedef struct FileRecord {
    char filename[MAX_FILENAME_LENGTH];      /**< Logical (user-visible) filename. */
    char physical_name[MAX_FILENAME_LENGTH]; /**< Flattened filename (no '/'). */
    char filepath[MAX_PATH_LENGTH];          /**< Absolute path to the content file. */
    char metapath[MAX_PATH_LENGTH];          /**< Absolute path to the .meta sidecar. */
    char undopath[MAX_PATH_LENGTH];          /**< Absolute path to the .undo snapshot. */
    char owner[MAX_USERNAME_LENGTH];         /**< File owner username. */
    char read_users[SS_MAX_USERS_PER_FILE][MAX_USERNAME_LENGTH];  /**< Read ACL (mirrors NS). */
    int  read_count;                         /**< Number of valid entries in read_users[]. */
    char write_users[SS_MAX_USERS_PER_FILE][MAX_USERNAME_LENGTH]; /**< Write ACL (mirrors NS). */
    int  write_count;                        /**< Number of valid entries in write_users[]. */
    time_t created;                          /**< Unix timestamp of file creation. */
    time_t modified;                         /**< Unix timestamp of last write commit. */
    time_t last_access;                      /**< Unix timestamp of last read or write. */
    char last_access_user[MAX_USERNAME_LENGTH]; /**< User who last accessed the file. */
    int  undo_available;                     /**< 1 if a valid .undo snapshot exists. */
    CheckpointRecord checkpoints[SS_MAX_CHECKPOINTS]; /**< Named snapshot records. */
    int  checkpoint_count;                   /**< Number of valid checkpoint entries. */
    int  migrating;                          /**< 1 while the file is being migrated (write-frozen). */
    pthread_mutex_t file_lock;               /**< Serializes concurrent operations on this file. */
    struct FileRecord *next;                 /**< Next record in the ss->files linked list. */
} FileRecord;

/**
 * @brief Active write session for a client performing a sentence-level edit.
 *
 * Created when a client sends REQ_WRITE_LOCK for a (file, sentence_index)
 * pair.  Represents the lock held by that client on a specific sentence.
 *
 * Write protocol lifecycle:
 *   1. Client sends REQ_WRITE_LOCK  → SS creates WriteSession, replies OK_LOCKED.
 *   2. Client sends zero or more WRITE_DATA messages → SS buffers edits in
 *      sentence_working (the working copy of the locked sentence).
 *   3. Client sends ETIRW → SS commits sentence_working back into the file
 *      content, saves an undo snapshot, and destroys the WriteSession.
 *
 * The file snapshot (file_snapshot) captures the full file content at lock
 * time so it can be used as the .undo source on commit.
 */
typedef struct WriteSession {
    int  client_fd;                    /**< Socket fd of the client holding this lock. */
    FileRecord *file;                  /**< The file being edited. */
    char username[MAX_USERNAME_LENGTH];/**< The user who acquired the lock. */
    int  sentence_index;               /**< Zero-based index of the locked sentence. */
    size_t sentence_start;             /**< Byte offset of sentence start in the file. */
    size_t sentence_end;               /**< Byte offset of sentence end in the file. */
    char *file_snapshot;               /**< Heap copy of the full file at lock time (for UNDO). */
    char *sentence_working;            /**< Working copy of the sentence being edited. */
    struct WriteSession *next;         /**< Next session in the ss->sessions linked list. */
} WriteSession;

/**
 * @brief Top-level Storage Server state structure.
 *
 * One StorageServer instance is created at startup and lives for the
 * entire process lifetime.
 */
typedef struct {
    char ns_ip[MAX_IP_LENGTH];       /**< IP address of the Name Server (to connect to). */
    int  ns_port;                    /**< Port of the Name Server. */
    char client_ip[MAX_IP_LENGTH];   /**< IP this SS advertises to clients for direct connections. */
    int  client_port;                /**< Port this SS listens on for client connections. */
    int  ns_sockfd;                  /**< Socket connected to the Name Server for control commands. */
    int  client_sockfd;              /**< (Unused field; client connections are per-thread.) */
    char storage_path[MAX_PATH_LENGTH]; /**< Absolute path to this SS's storage directory. */
    int  client_listen_fd;           /**< Listening socket accepting incoming client connections. */
    pthread_mutex_t files_lock;      /**< Protects the files linked list. */
    FileRecord *files;               /**< Head of the singly-linked list of FileRecord entries. */
    pthread_t ns_thread;             /**< Thread running the NS command loop (ns_control_loop). */
    int  running;                    /**< 1 while the server is operating normally; 0 to stop. */
    pthread_mutex_t sessions_lock;   /**< Protects the sessions linked list. */
    WriteSession *sessions;          /**< Head of active write sessions. */
    int  ns_thread_active;           /**< 1 after the NS thread has been started. */
    int  ns_thread_started;          /**< 1 once pthread_create succeeded for ns_thread. */
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
