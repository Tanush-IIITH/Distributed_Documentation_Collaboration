#ifndef STORAGE_SERVER_H
#define STORAGE_SERVER_H

#include "../common/protocol.h"
#include "../common/utils.h"

// Storage Server configuration
#define SS_MAX_FILES 10000
#define SS_STORAGE_PATH "./ss_storage/"

// Storage Server structure
typedef struct {
    char ns_ip[MAX_IP_LENGTH];
    int ns_port;
    char client_ip[MAX_IP_LENGTH];
    int client_port;
    int ns_sockfd;
    int client_sockfd;
    char storage_path[MAX_PATH_LENGTH];
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
