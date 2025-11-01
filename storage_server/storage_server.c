#include "storage_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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
    
    // Set storage path
    strncpy(ss->storage_path, SS_STORAGE_PATH, MAX_PATH_LENGTH - 1);
    
    // Create storage directory
    if (create_directory_recursive(ss->storage_path) != 0) {
        fprintf(stderr, "Failed to create storage directory\n");
        return -1;
    }
    
    // Initialize sockets
    ss->ns_sockfd = -1;
    ss->client_sockfd = -1;
    
    log_message(LOG_INFO, "SS", "Storage server initialized");
    return 0;
}

int ss_register_with_ns(StorageServer *ss) {
    if (!ss) {
        return -1;
    }
    
    // TODO: Implement NS registration
    // 1. Create socket and connect to NS
    // 2. Send HELLO_SS message
    // 3. Wait for OK response
    
    log_message(LOG_INFO, "SS", "Registered with Name Server");
    return 0;
}

int ss_send_file_list(StorageServer *ss) {
    if (!ss) {
        return -1;
    }
    
    // TODO: Implement file list sending
    // 1. Scan storage directory
    // 2. Send SS_HAS_FILE for each file
    // 3. Send SS_FILES_DONE
    
    log_message(LOG_INFO, "SS", "File list sent to Name Server");
    return 0;
}

int ss_start(StorageServer *ss) {
    if (!ss) {
        return -1;
    }
    
    // TODO: Implement main server loop
    // 1. Create listening socket for clients
    // 2. Accept client connections
    // 3. Handle protocol messages
    // 4. Process file operations
    
    log_message(LOG_INFO, "SS", "Storage server started");
    return 0;
}

void ss_cleanup(StorageServer *ss) {
    if (!ss) {
        return;
    }
    
    if (ss->ns_sockfd >= 0) {
        close(ss->ns_sockfd);
    }
    
    if (ss->client_sockfd >= 0) {
        close(ss->client_sockfd);
    }
    
    log_message(LOG_INFO, "SS", "Storage server cleanup complete");
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <ns_ip> <ns_port> <client_ip> <client_port>\n", argv[0]);
        return 1;
    }
    
    // Initialize logging
    log_init("storage_server.log", LOG_INFO);
    
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
