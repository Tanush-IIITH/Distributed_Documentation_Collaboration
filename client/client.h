#ifndef CLIENT_H
#define CLIENT_H

#include "../common/protocol.h"
#include "../common/utils.h"

// Client configuration
#define CLIENT_BUFFER_SIZE 8192

// Client structure
typedef struct {
    char username[MAX_USERNAME_LENGTH];
    char ns_ip[MAX_IP_LENGTH];
    int ns_port;
    int ns_sockfd;
} Client;

/**
 * Initialize the client
 * Returns 0 on success, -1 on error
 */
int client_init(Client *client, const char *username, const char *ns_ip, int ns_port);

/**
 * Connect to Name Server
 * Returns 0 on success, -1 on error
 */
int client_connect_to_ns(Client *client);

/**
 * Send command to Name Server
 * Returns 0 on success, -1 on error
 */
int client_send_command(Client *client, const char *command);

/**
 * Process user input and execute commands
 * Returns 0 on success, -1 on error
 */
int client_process_command(Client *client, const char *input);

/**
 * Start the client interactive loop
 * Returns 0 on success, -1 on error
 */
int client_start(Client *client);

/**
 * Cleanup and disconnect
 */
void client_cleanup(Client *client);

#endif // CLIENT_H
