#ifndef NAME_SERVER_H
#define NAME_SERVER_H

#include "../common/protocol.h"
#include "../common/utils.h"

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
    // Access control list would go here
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

// Name Server structure
typedef struct {
    int port;
    int sockfd;
    StorageServerInfo storage_servers[NS_MAX_STORAGE_SERVERS];
    int ss_count;
    ClientInfo clients[NS_MAX_CLIENTS];
    int client_count;
    FileMetadata files[NS_MAX_FILES];
    int file_count;
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
