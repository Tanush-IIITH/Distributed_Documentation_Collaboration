#ifndef NAME_SERVER_H
#define NAME_SERVER_H

#include "../common/protocol.h"
#include "../common/utils.h"
#include <pthread.h>

// Name Server configuration
#define NS_MAX_STORAGE_SERVERS 100
#define NS_MAX_CLIENTS 1000
#define NS_MAX_FILES 10000

// File metadata structure
typedef struct {
    char filename[MAX_FILENAME_LENGTH];
    char owner[MAX_USERNAME_LENGTH];
    char ss_ip[MAX_IP_LENGTH];
    int ss_port;
    time_t created;
    time_t modified;
    char read_access_users[NS_MAX_CLIENTS][MAX_USERNAME_LENGTH];
    char write_access_users[NS_MAX_CLIENTS][MAX_USERNAME_LENGTH];
    int read_access_count;
    int write_access_count;
} FileMetadata;

// Storage Server information
typedef struct {
    char ns_ip[MAX_IP_LENGTH];
    int ns_port;
    char client_ip[MAX_IP_LENGTH];
    int client_port;
    int sockfd;
    int is_alive;
    time_t last_heartbeat;
} StorageServerInfo;

// Client information
typedef struct {
    char username[MAX_USERNAME_LENGTH];
    char ip[MAX_IP_LENGTH];
    int port;
    int sockfd;
    int is_connected;
} ClientInfo;

// Node for filename -> metadata index lookups
typedef struct FileIndexNode {
    char filename[MAX_FILENAME_LENGTH];
    int file_array_index;
    struct FileIndexNode *next;
} FileIndexNode;

#define FILE_INDEX_SIZE 1024

typedef struct {
    char filename[MAX_FILENAME_LENGTH];
    int file_array_index;
    int valid;
    unsigned long last_used;
} FileCacheEntry;

#define FILE_CACHE_SIZE 64

// Name Server structure
typedef struct {
    int port; // Port number the Name Server listens on
    int sockfd; // Socket file descriptor for the Name Server

    StorageServerInfo storage_servers[NS_MAX_STORAGE_SERVERS]; // Array to store information about registered storage servers
    int ss_count; // Count of currently registered storage servers

    ClientInfo clients[NS_MAX_CLIENTS]; // Array to store information about connected clients
    int client_count; // Count of currently connected clients

    FileMetadata files[NS_MAX_FILES]; // Array to store metadata of files managed by the Name Server
    int file_count; // Count of files currently managed by the Name Server

    pthread_mutex_t state_lock; // Mutex to protect shared state (e.g., storage_servers, clients, files)

    FileIndexNode *file_index[FILE_INDEX_SIZE]; // Hash map buckets for fast file lookups
    FileCacheEntry file_cache[FILE_CACHE_SIZE]; // Cache for recent lookups
    unsigned long cache_tick; // Monotonic counter for cache usage tracking
} NameServer;

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
