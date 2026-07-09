/**
 * @file name_server.h
 * @brief Public interface and data structures for the Name Server (NS).
 *
 * The Name Server is the central coordinator of the distributed system. It:
 *   - Maintains an in-memory registry of all files, their owners, and which
 *     Storage Server holds each file.
 *   - Enforces Access Control Lists (ACLs) for read and write permissions.
 *   - Tracks all connected clients and registered Storage Servers.
 *   - Handles file creation, deletion, metadata queries, EXEC, and access
 *     management commands directly.
 *   - Routes data-intensive operations (READ, WRITE, STREAM, UNDO) by
 *     responding to REQ_LOC with the responsible Storage Server's address.
 *   - Uses a hashmap + LRU cache for O(1) average-case file lookups.
 *   - Persists metadata to a SQLite-compatible flat-file DB (ns_metadata.db)
 *     so file records survive restarts.
 *
 * Concurrency: each incoming TCP connection (client or SS) is handled in its
 * own pthread.  All shared state (files[], clients[], storage_servers[],
 * file_index) is protected by a single ns->state_lock mutex.
 */
#ifndef NAME_SERVER_H
#define NAME_SERVER_H

#include "../common/protocol.h"
#include "../common/utils.h"
#include <pthread.h>

/** Maximum number of Storage Servers that can register simultaneously. */
#define NS_MAX_STORAGE_SERVERS 100
/** Maximum number of concurrent client connections. */
#define NS_MAX_CLIENTS 1000
/** Maximum number of files tracked in the Name Server's in-memory registry. */
#define NS_MAX_FILES 10000
/** Maximum number of pending access-request mailbox entries. */
#define NS_MAX_ACCESS_REQUESTS 1024

/**
 * @brief Metadata record for a single file managed by the Name Server.
 *
 * The NS stores one FileMetadata entry per file (and per virtual folder).
 * This includes all the information needed to:
 *   - Route client requests to the correct Storage Server.
 *   - Enforce ACL checks without contacting the SS.
 *   - Display file information via VIEW -l and INFO commands.
 *
 * ACL design: owners implicitly have full read+write access. Other users
 * must be listed in read_access_users or write_access_users to gain access.
 * Write access implies read access.
 */
typedef struct {
    char filename[MAX_FILENAME_LENGTH];         /**< Logical filename / path (e.g., "docs/report.txt"). */
    char owner[MAX_USERNAME_LENGTH];            /**< Username of the file creator/owner. */
    char ss_ip[MAX_IP_LENGTH];                  /**< Client-facing IP of the responsible Storage Server. */
    int  ss_port;                               /**< Client-facing port of the responsible Storage Server. */
    long long size;                             /**< File size in bytes (updated after writes). */
    long long word_count;                       /**< Cached word count (updated after writes). */
    long long char_count;                       /**< Cached non-whitespace character count. */
    int  is_directory;                          /**< 1 if this entry represents a virtual folder. */
    time_t created;                             /**< Unix timestamp of file creation. */
    time_t modified;                            /**< Unix timestamp of last modification. */
    time_t last_access;                         /**< Unix timestamp of last read or write access. */
    char last_access_user[MAX_USERNAME_LENGTH]; /**< Username who last accessed the file. */
    char last_modified_user[MAX_USERNAME_LENGTH];/**< Username who last modified the file. */
    char read_access_users[NS_MAX_CLIENTS][MAX_USERNAME_LENGTH];  /**< Users with read permission. */
    char write_access_users[NS_MAX_CLIENTS][MAX_USERNAME_LENGTH]; /**< Users with write permission. */
    int  read_access_count;                     /**< Number of entries in read_access_users[]. */
    int  write_access_count;                    /**< Number of entries in write_access_users[]. */
} FileMetadata;

/**
 * @brief Runtime record for a registered Storage Server.
 *
 * Each SS has two endpoints:
 *   - ns_ip / ns_port:     The address the SS used to connect to the NS
 *                          (used by the NS to send sub-commands).
 *   - client_ip / client_port: The address published to clients for direct
 *                          data-plane connections (READ, WRITE, STREAM, UNDO).
 *
 * Synchronization:
 *   comm_lock + response_cond implement a request-reply synchronization
 *   primitive so that NS handler threads can send a command to the SS and
 *   block until the dedicated SS reader thread delivers the response.
 *   refcount tracks how many NS threads are currently using this entry;
 *   the entry must not be freed while refcount > 0.
 */
typedef struct {
    char ns_ip[MAX_IP_LENGTH];      /**< IP the SS connected from (NS-facing endpoint). */
    int  ns_port;                   /**< Port the SS connected from. */
    char client_ip[MAX_IP_LENGTH];  /**< IP clients should connect to for data. */
    int  client_port;               /**< Port clients should connect to for data. */
    int  sockfd;                    /**< NS-side socket for NS→SS commands (-1 if disconnected). */
    int  is_alive;                  /**< 1 if the SS is considered reachable. */
    time_t last_heartbeat;          /**< Timestamp of the last successful PING/PONG exchange. */
    long long total_bytes;          /**< Estimated total bytes stored on this SS (for balancing). */
    int  total_files;               /**< Number of files assigned to this SS (for balancing). */
    pthread_mutex_t comm_lock;      /**< Protects all fields below + awaiting_response state. */
    pthread_cond_t  response_cond;  /**< Signaled when a SS response arrives or the SS dies. */
    int awaiting_response;          /**< 1 if an NS thread is waiting for a reply from this SS. */
    int response_ready;             /**< 1 when the response has been received and stored. */
    int response_status;            /**< 0 on success, negative on error (e.g., SS disconnected). */
    char *response_raw;             /**< Heap-allocated raw response string; owner must free(). */
    int  refcount;                  /**< Number of NS threads currently referencing this entry. */
} StorageServerInfo;

/**
 * @brief Runtime record for a connected client.
 *
 * Populated during the HELLO_CLIENT handshake and used to:
 *   - Log user-attributed operations.
 *   - Enforce per-user ACL checks.
 *   - List active users in response to the LIST command.
 */
typedef struct {
    char username[MAX_USERNAME_LENGTH]; /**< Authenticated username. */
    char ip[MAX_IP_LENGTH];             /**< IP address of the client's TCP connection. */
    int  port;                          /**< Ephemeral port of the client's TCP connection. */
    int  sockfd;                        /**< Socket fd for the NS↔Client connection. */
    int  is_connected;                  /**< 1 while the client session is active. */
} ClientInfo;

/**
 * @brief A single node in the hashmap bucket list for fast file index lookups.
 *
 * The file_index is a chained hash table: FILE_INDEX_SIZE buckets, each
 * containing a linked list of FileIndexNode entries.
 * Each node maps a logical filename to its array index in ns->files[].
 */
typedef struct FileIndexNode {
    char filename[MAX_FILENAME_LENGTH]; /**< The logical filename key. */
    int  file_array_index;             /**< Index into ns->files[] for this file. */
    struct FileIndexNode *next;        /**< Next node in the same hash bucket (chaining). */
} FileIndexNode;

/** Number of buckets in the file lookup hashmap. */
#define FILE_INDEX_SIZE 1024

/**
 * @brief An entry in the LRU file lookup cache.
 *
 * The cache sits in front of the hashmap: a linear scan of FILE_CACHE_SIZE
 * entries is checked first. Hits update last_used to implement LRU eviction.
 */
typedef struct {
    char filename[MAX_FILENAME_LENGTH]; /**< Cached filename key. */
    int  file_array_index;             /**< Corresponding index in ns->files[]. */
    int  valid;                        /**< 1 if this cache slot is occupied. */
    unsigned long last_used;           /**< Monotonically increasing access counter for LRU. */
} FileCacheEntry;



/** Number of entries in the LRU file lookup cache. */
#define FILE_CACHE_SIZE 64

/**
 * @brief An entry in the per-file access-request mailbox.
 *
 * When a user lacks access to a file, they can submit a REQUESTACCESS
 * message.  The owner sees these requests via LISTREQUESTS and can
 * APPROVEACCESS or REJECTACCESS them.
 */
typedef struct {
    int  id;                           /**< Unique auto-incremented request ID. */
    char filename[MAX_FILENAME_LENGTH];/**< The file the requester wants access to. */
    char from_user[MAX_USERNAME_LENGTH];/**< The user submitting the request. */
    char to_user[MAX_USERNAME_LENGTH]; /**< The owner who must approve/reject. */
    int  grant_read;                   /**< 1 if read access is being requested. */
    int  grant_write;                  /**< 1 if write access is being requested. */
    int  is_pending;                   /**< 1 while the request awaits a decision. */
} AccessRequest;

/**
 * @brief The top-level Name Server state structure.
 *
 * One NameServer instance is created at startup in main() and lives for
 * the entire process lifetime.  All fields must be accessed under
 * state_lock except where documented otherwise.
 */
typedef struct {
    int port;    /**< TCP port the NS listens on for incoming connections. */
    int sockfd;  /**< Listening socket file descriptor. */

    StorageServerInfo *storage_servers[NS_MAX_STORAGE_SERVERS]; /**< Registered SS entries (heap-allocated). */
    int ss_count; /**< Number of currently registered Storage Servers. */

    ClientInfo clients[NS_MAX_CLIENTS]; /**< Connected client session records. */
    int client_count; /**< Number of active client sessions. */

    FileMetadata files[NS_MAX_FILES]; /**< All file metadata records (flat array). */
    int file_count; /**< Number of valid entries in files[]. */

    AccessRequest access_requests[NS_MAX_ACCESS_REQUESTS]; /**< Pending access-request mailbox. */
    int request_count;   /**< Number of valid entries in access_requests[]. */
    int next_request_id; /**< Auto-increment counter for new request IDs. */

    pthread_mutex_t state_lock; /**< Coarse-grained mutex protecting all shared arrays. */

    FileIndexNode *file_index[FILE_INDEX_SIZE]; /**< Hash buckets: filename → files[] index. */
    FileCacheEntry file_cache[FILE_CACHE_SIZE];  /**< LRU cache in front of the hashmap. */
    unsigned long cache_tick;       /**< Monotonic counter used for LRU last_used comparisons. */
    int ss_round_robin_index;       /**< Next SS index to consider in balanced placement. */
} NameServer;


/**
 * @brief Describes one file-rename step during a hierarchical MOVE operation.
 *
 * When moving a folder, the NS must rename every file inside it on the
 * responsible SS.  Each MovePlanEntry captures one such rename operation
 * so the NS can transactionally apply them.
 */
typedef struct {
    char old_path[MAX_FILENAME_LENGTH]; /**< Original logical path (e.g., "folder/file.txt"). */
    char new_path[MAX_FILENAME_LENGTH]; /**< Destination logical path. */
    int  is_directory;                  /**< 1 if this entry is a folder (no SS rename needed). */
    StorageServerInfo *ss;             /**< The SS responsible for this file. */
    char old_flat[MAX_FILENAME_LENGTH]; /**< Physical (flattened) name before rename. */
    char new_flat[MAX_FILENAME_LENGTH]; /**< Physical (flattened) name after rename. */
} MovePlanEntry;

/**
 * Initialize the Name Server
 * Returns 0 on success, -1 on error
 */
int ns_init(NameServer *ns, int port);

/**
 * Start the Name Server
 * Returns 0 on success, -1 on error
 */
int ns_start(NameServer *ns);

/**
 * Register a storage server
 * Returns 0 on success, -1 on error
 */
int ns_register_storage_server(NameServer *ns, const char *ns_ip, int ns_port,
                                const char *client_ip, int client_port, int sockfd);

/**
 * Register a client
 * Returns 0 on success, -1 on error
 */
int ns_register_client(NameServer *ns, const char *username, int sockfd);

/**
 * Find storage server for a file
 * Returns index of SS, or -1 if not found
 */
int ns_find_storage_server(NameServer *ns, const char *filename);

/**
 * Cleanup and shutdown
 */
void ns_cleanup(NameServer *ns);

#endif // NAME_SERVER_H
