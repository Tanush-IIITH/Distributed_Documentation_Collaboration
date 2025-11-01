#include "name_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>

int ns_init(NameServer *ns, int port) {
    if (!ns) {
        return -1;
    }
    
    memset(ns, 0, sizeof(NameServer));
    ns->port = port;
    ns->sockfd = -1;
    ns->ss_count = 0;
    ns->client_count = 0;
    ns->file_count = 0;
    if (pthread_mutex_init(&ns->state_lock, NULL) != 0) {
        log_message(LOG_ERROR, "NS", "Failed to initialize mutex: %s", strerror(errno));
        return -1;
    }
    
    log_message(LOG_INFO, "NS", "Name Server initialized on port %d", port);
    return 0;
}

typedef struct {
    NameServer *ns;
    int conn_fd; //connection file descriptor
    char peer_ip[INET_ADDRSTRLEN]; //peer ip address
    int peer_port; //peer port number
    int is_client;
    int is_storage_server;
    char username[MAX_USERNAME_LENGTH];
} ConnectionContext;

//close connection and free context
static void close_connection(ConnectionContext *ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->conn_fd >= 0) {
        close(ctx->conn_fd);
    }
    free(ctx);
}

//send error message and log it
static void send_error_and_log(int fd, int code, const char *message, const char *peer_ip, int peer_port) {
    char *err_resp = protocol_build_error(code, message);
    if (err_resp) {
        protocol_send_message(fd, err_resp);
        free(err_resp);
    }
    log_message(LOG_WARNING, "NS", "Sent error %d to %s:%d -> %s", code, peer_ip, peer_port, message);
}

static void run_client_loop(ConnectionContext *ctx) {
    if (!ctx || !ctx->ns) {
        return;
    }

    NameServer *ns = ctx->ns;

    while (1) {
        char *raw_command = protocol_receive_message(ctx->conn_fd);
        if (!raw_command) {
            log_message(LOG_INFO, "NS", "Client %s:%d disconnected.", ctx->peer_ip, ctx->peer_port);
            break;
        }

        ProtocolMessage cmd_msg;
        if (protocol_parse_message(raw_command, &cmd_msg) != 0 || cmd_msg.field_count == 0) {
            send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid command format.", ctx->peer_ip, ctx->peer_port);
            free(raw_command);
            continue;
        }

        const char *command = cmd_msg.fields[0];
        log_request("NS", ctx->peer_ip, ctx->peer_port, "0.0.0.0", ns->port,
                    ctx->username[0] ? ctx->username : NULL, command);

        if (strcmp(command, MSG_LIST_USERS) == 0) {
            // TODO: Handle LIST_USERS
        } else if (strcmp(command, MSG_CREATE) == 0) {
            // TODO: Handle CREATE
        } else if (strcmp(command, MSG_REQ_LOC) == 0) {
            // TODO: Handle REQ_LOC
        } else {
            send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Unknown client command.", ctx->peer_ip, ctx->peer_port);
        }

        protocol_free_message(&cmd_msg);
        free(raw_command);
    }
}

static void run_ss_loop(ConnectionContext *ctx) {
    if (!ctx || !ctx->ns) {
        return;
    }

    NameServer *ns = ctx->ns;

    while (1) {
        char *file_msg_raw = protocol_receive_message(ctx->conn_fd);
        if (!file_msg_raw) {
            log_message(LOG_WARNING, "NS", "SS %s:%d disconnected during file sync.", ctx->peer_ip, ctx->peer_port);
            return;
        }

        ProtocolMessage file_msg;
        if (protocol_parse_message(file_msg_raw, &file_msg) != 0 || file_msg.field_count == 0) {
            send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid file sync message.", ctx->peer_ip, ctx->peer_port);
            free(file_msg_raw);
            continue;
        }

        if (strcmp(file_msg.fields[0], MSG_SS_HAS_FILE) == 0) {
            // TODO: Implement file registration from storage server
        } else if (strcmp(file_msg.fields[0], MSG_SS_FILES_DONE) == 0) {
            log_message(LOG_INFO, "NS", "SS %s:%d file sync complete.", ctx->peer_ip, ctx->peer_port);
            protocol_free_message(&file_msg);
            free(file_msg_raw);
            break;
        } else {
            send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Unknown storage server sync command.", ctx->peer_ip, ctx->peer_port);
        }

        protocol_free_message(&file_msg);
        free(file_msg_raw);
    }

    while (1) {
        char *raw_command = protocol_receive_message(ctx->conn_fd);
        if (!raw_command) {
            log_message(LOG_INFO, "NS", "SS %s:%d disconnected.", ctx->peer_ip, ctx->peer_port);
            break;
        }

        ProtocolMessage cmd_msg;
        if (protocol_parse_message(raw_command, &cmd_msg) != 0 || cmd_msg.field_count == 0) {
            send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid storage server command.", ctx->peer_ip, ctx->peer_port);
            free(raw_command);
            continue;
        }

        const char *command = cmd_msg.fields[0];
        log_request("NS", ctx->peer_ip, ctx->peer_port, "0.0.0.0", ns->port, NULL, command);

        // TODO: Handle storage server commands (e.g., heartbeats, write notifications)

        protocol_free_message(&cmd_msg);
        free(raw_command);
    }
}

//handle each connection in a separate thread
static void *connection_thread(void *arg) {
    ConnectionContext *ctx = (ConnectionContext *)arg;
    if (!ctx || !ctx->ns) {
        close_connection(ctx);
        return NULL;
    }

    NameServer *ns = ctx->ns;

    // 1. Read handshake message
    char *raw_message = protocol_receive_message(ctx->conn_fd);
    if (!raw_message) {
        log_message(LOG_WARNING, "NS", "Failed to read handshake from %s:%d", ctx->peer_ip, ctx->peer_port);
        close_connection(ctx);
        return NULL;
    }

    // 2. Parse message
    ProtocolMessage msg;
    if (protocol_parse_message(raw_message, &msg) != 0 || msg.field_count == 0) {
        send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid message format.", ctx->peer_ip, ctx->peer_port);
        free(raw_message);
        close_connection(ctx);
        return NULL;
    }

    log_request("NS", ctx->peer_ip, ctx->peer_port, "0.0.0.0", ns->port, NULL, msg.fields[0]);

    const char *command = msg.fields[0];
    int handled = 0;
    int handshake_success = 0;

    //handle the client
    if (strcmp(command, MSG_HELLO_CLIENT) == 0) {
        handled = 1;
        if (msg.field_count < 2) {
            send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Missing username.", ctx->peer_ip, ctx->peer_port);
        } else if (!validate_username(msg.fields[1])) {
            send_error_and_log(ctx->conn_fd, ERR_USERNAME_INVALID, "Username invalid or already connected.", ctx->peer_ip, ctx->peer_port);
        } else {
            pthread_mutex_lock(&ns->state_lock);
            int reg_result = ns_register_client(ns, msg.fields[1], ctx->conn_fd);
            pthread_mutex_unlock(&ns->state_lock);

            if (reg_result != 0) {
                send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to register client.", ctx->peer_ip, ctx->peer_port);
            } else {
                char welcome[MAX_FIELD_SIZE];
                snprintf(welcome, sizeof(welcome), "Welcome, %s.", msg.fields[1]);
                const char *fields[] = {RESP_OK, welcome};
                char *resp = protocol_build_message(fields, 2);
                if (resp) {
                    protocol_send_message(ctx->conn_fd, resp);
                    free(resp);
                }
                handshake_success = 1;
                ctx->is_client = 1;
                ctx->is_storage_server = 0;
                ctx->username[0] = '\0';
                strncpy(ctx->username, msg.fields[1], sizeof(ctx->username) - 1);
                ctx->username[sizeof(ctx->username) - 1] = '\0';
            }
        }
    } 
    //handle the storage server
    else if (strcmp(command, MSG_HELLO_SS) == 0) {
        handled = 1;
        if (msg.field_count < 5) {
            send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Missing storage server parameters.", ctx->peer_ip, ctx->peer_port);
        } else if (!is_valid_ip(msg.fields[1]) || !is_valid_port(atoi(msg.fields[2])) ||
                   !is_valid_ip(msg.fields[3]) || !is_valid_port(atoi(msg.fields[4]))) {
            send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Invalid storage server address info.", ctx->peer_ip, ctx->peer_port);
        } else {
            int ns_port = atoi(msg.fields[2]);
            int client_port = atoi(msg.fields[4]);
            pthread_mutex_lock(&ns->state_lock);
            int reg_result = ns_register_storage_server(ns, msg.fields[1], ns_port, msg.fields[3], client_port, ctx->conn_fd);
            pthread_mutex_unlock(&ns->state_lock);

            if (reg_result != 0) {
                send_error_and_log(ctx->conn_fd, ERR_INTERNAL_ERROR, "Failed to register storage server.", ctx->peer_ip, ctx->peer_port);
            } else {
                const char *fields[] = {RESP_OK, "SS registered. Awaiting file list."};
                char *resp = protocol_build_message(fields, 2);
                if (resp) {
                    protocol_send_message(ctx->conn_fd, resp);
                    free(resp);
                }
                handshake_success = 1;
                ctx->is_client = 0;
                ctx->is_storage_server = 1;
                ctx->username[0] = '\0';
            }
        }
    }

    if (!handled) {
        send_error_and_log(ctx->conn_fd, ERR_INVALID_REQUEST, "Unknown command.", ctx->peer_ip, ctx->peer_port);
    }

    protocol_free_message(&msg);
    free(raw_message);

    if (handshake_success && ctx->is_client) {
        log_message(LOG_INFO, "NS", "Client %s handshake complete. Entering command loop.", ctx->username);
        run_client_loop(ctx);
    } else if (handshake_success && ctx->is_storage_server) {
        log_message(LOG_INFO, "NS", "Storage server %s:%d handshake complete. Entering sync loop.", ctx->peer_ip, ctx->peer_port);
        run_ss_loop(ctx);
    } else {
        log_message(LOG_INFO, "NS", "Connection with %s:%d finished without persistent session.", ctx->peer_ip, ctx->peer_port);
    }

    // TODO: remove connection metadata from NameServer state before closing the socket
    close_connection(ctx);
    return NULL;
}

int ns_start(NameServer *ns) {
    if (!ns) {
        return -1;
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0); //ipv4 and tcp
    if (listen_fd < 0) {
        log_message(LOG_ERROR, "NS", "Failed to create socket: %s", strerror(errno));
        return -1;
    }

    // Enable the socket to reuse the address (port) immediately after the server restarts
    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        log_message(LOG_WARNING, "NS", "setsockopt failed: %s", strerror(errno));
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(ns->port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { //binding the socket 
        log_message(LOG_ERROR, "NS", "Bind failed on port %d: %s", ns->port, strerror(errno));
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, SOMAXCONN) < 0) { //one time setup to keep adding connections in the connection queue
        log_message(LOG_ERROR, "NS", "Listen failed: %s", strerror(errno));
        close(listen_fd);
        return -1;
    }

    ns->sockfd = listen_fd;
    log_message(LOG_INFO, "NS", "Name Server listening on port %d", ns->port);

    while (1) {
        struct sockaddr_in peer_addr;
        socklen_t peer_len = sizeof(peer_addr);
        int conn_fd = accept(listen_fd, (struct sockaddr *)&peer_addr, &peer_len); //accepting connection
        if (conn_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_message(LOG_WARNING, "NS", "Accept failed: %s", strerror(errno));
            continue;
        }

        char peer_ip[INET_ADDRSTRLEN] = "unknown";
        if (!inet_ntop(AF_INET, &peer_addr.sin_addr, peer_ip, sizeof(peer_ip))) {
            strncpy(peer_ip, "unknown", sizeof(peer_ip) - 1);
            peer_ip[sizeof(peer_ip) - 1] = '\0';
        }
        int peer_port = ntohs(peer_addr.sin_port);

        log_message(LOG_INFO, "NS", "Connection accepted from %s:%d", peer_ip, peer_port);
        ConnectionContext *ctx = (ConnectionContext *)malloc(sizeof(ConnectionContext));
        if (!ctx) {
            log_message(LOG_ERROR, "NS", "Failed to allocate thread context for %s:%d", peer_ip, peer_port);
            close(conn_fd);
            continue;
        }

        //Initialize the connection context
        ctx->ns = ns;
        ctx->conn_fd = conn_fd;
        strncpy(ctx->peer_ip, peer_ip, sizeof(ctx->peer_ip) - 1);
        ctx->peer_ip[sizeof(ctx->peer_ip) - 1] = '\0';
        ctx->peer_port = peer_port;
        ctx->is_client = 0;
        ctx->is_storage_server = 0;
        ctx->username[0] = '\0';

        pthread_t thread_id;
        //call the connection_thread function to handle the connection and detach subsequently
        if (pthread_create(&thread_id, NULL, connection_thread, ctx) != 0) {
            log_message(LOG_ERROR, "NS", "Failed to spawn thread for %s:%d: %s", peer_ip, peer_port, strerror(errno));
            close(conn_fd);
            free(ctx);
            continue;
        }

        pthread_detach(thread_id); //the main thread does not wait for this thread to finish
    }

    return 0;
}

int ns_register_storage_server(NameServer *ns, const char *ns_ip, int ns_port,
                                const char *client_ip, int client_port, int sockfd) {
    if (!ns || !ns_ip || !client_ip) {
        return -1;
    }
    
    if (ns->ss_count >= NS_MAX_STORAGE_SERVERS) {
        log_message(LOG_ERROR, "NS", "Maximum storage servers reached");
        return -1;
    }

    for (int i = 0; i < ns->ss_count; i++) {
        StorageServerInfo *info = &ns->storage_servers[i];
        if (info->is_alive && strcmp(info->ns_ip, ns_ip) == 0 && info->ns_port == ns_port) {
            log_message(LOG_WARNING, "NS", "Storage server %s:%d already registered", ns_ip, ns_port);
            return -1;
        }
    }

    StorageServerInfo *ss = &ns->storage_servers[ns->ss_count];
    memset(ss, 0, sizeof(StorageServerInfo));
    strncpy(ss->ns_ip, ns_ip, sizeof(ss->ns_ip) - 1);
    ss->ns_ip[sizeof(ss->ns_ip) - 1] = '\0';
    ss->ns_port = ns_port;
    strncpy(ss->client_ip, client_ip, sizeof(ss->client_ip) - 1);
    ss->client_ip[sizeof(ss->client_ip) - 1] = '\0';
    ss->client_port = client_port;
    ss->sockfd = sockfd;
    ss->is_alive = 1;
    ss->last_heartbeat = get_current_time();

    ns->ss_count++;

    log_message(LOG_INFO, "NS", "Registered storage server: %s:%d", client_ip, client_port);
    return 0;
}

int ns_register_client(NameServer *ns, const char *username, int sockfd) {
    if (!ns || !username) {
        return -1;
    }
    
    if (ns->client_count >= NS_MAX_CLIENTS) {
        log_message(LOG_ERROR, "NS", "Maximum clients reached");
        return -1;
    }

    for (int i = 0; i < ns->client_count; i++) {
        if (ns->clients[i].is_connected && strcmp(ns->clients[i].username, username) == 0) {
            log_message(LOG_WARNING, "NS", "Client %s already connected", username);
            return -1;
        }
    }

    ClientInfo *client = &ns->clients[ns->client_count];
    memset(client, 0, sizeof(ClientInfo));
    strncpy(client->username, username, sizeof(client->username) - 1);
    client->username[sizeof(client->username) - 1] = '\0';
    client->sockfd = sockfd;
    client->is_connected = 1;

    ns->client_count++;

    log_message(LOG_INFO, "NS", "Registered client: %s", username);
    return 0;
}

int ns_find_storage_server(NameServer *ns, const char *filename) {
    if (!ns || !filename) {
        return -1;
    }
    
    // TODO: Implement file location lookup
    // 1. Search file metadata
    // 2. Return SS index
    
    return -1;
}

void ns_cleanup(NameServer *ns) {
    if (!ns) {
        return;
    }
    
    if (ns->sockfd >= 0) {
        close(ns->sockfd);
    }
    
    // Close all client and SS connections
    for (int i = 0; i < ns->client_count; i++) {
        if (ns->clients[i].sockfd >= 0) {
            close(ns->clients[i].sockfd);
        }
    }
    
    for (int i = 0; i < ns->ss_count; i++) {
        if (ns->storage_servers[i].sockfd >= 0) {
            close(ns->storage_servers[i].sockfd);
        }
    }
    
    pthread_mutex_destroy(&ns->state_lock);
    
    log_message(LOG_INFO, "NS", "Name Server cleanup complete");
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }
    
    int port = atoi(argv[1]);
    if (!is_valid_port(port)) {
        fprintf(stderr, "Invalid port number\n");
        return 1;
    }
    
    // Initialize logging
    log_init("name_server.log", LOG_INFO);
    
    // Create Name Server
    NameServer ns;
    if (ns_init(&ns, port) != 0) {
        fprintf(stderr, "Failed to initialize Name Server\n");
        return 1;
    }
    
    // Start server
    printf("Name Server starting on port %d...\n", port);
    if (ns_start(&ns) != 0) {
        fprintf(stderr, "Server error\n");
        ns_cleanup(&ns);
        return 1;
    }
    
    // Cleanup
    ns_cleanup(&ns);
    log_cleanup();
    
    return 0;
}
