/**
 * @file client.h
 * @brief Public interface for the Client component.
 *
 * The Client is the user-facing process. It:
 *   1. Connects to the Name Server (NS) and authenticates with a username.
 *   2. Reads commands from stdin and dispatches them to the appropriate handler.
 *   3. For metadata/access-control commands: communicates directly with the NS.
 *   4. For data-intensive commands (READ, WRITE, STREAM, UNDO): first asks the
 *      NS for the Storage Server location (REQ_LOC), then opens a fresh
 *      connection directly to that Storage Server.
 *
 * Thread model: single-threaded; all operations are synchronous.
 */
#ifndef CLIENT_H
#define CLIENT_H

#include "../common/protocol.h"
#include "../common/utils.h"

/** Size of input/scratch buffers used when parsing user commands. */
#define CLIENT_BUFFER_SIZE 8192

/**
 * @brief Encapsulates the persistent state of a connected client session.
 *
 * A single Client instance is initialized once, connected to the Name Server,
 * and then used for the lifetime of the interactive session.
 */
typedef struct {
    char username[MAX_USERNAME_LENGTH]; /**< Authenticated username for this session. */
    char ns_ip[MAX_IP_LENGTH];          /**< IP address of the Name Server. */
    int  ns_port;                       /**< TCP port of the Name Server. */
    int  ns_sockfd;                     /**< Open TCP socket to the Name Server (-1 if not connected). */
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
