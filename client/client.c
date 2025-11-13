#include "client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

static int client_handle_view(Client *client, const char *flags);
static int client_handle_list(Client *client);
static int client_handle_addaccess(Client *client, const char *perm_flag, const char *filename, const char *username);
static int client_handle_remaccess(Client *client, const char *filename, const char *username);
static int client_handle_create(Client *client, const char *filename);
static int client_handle_delete(Client *client, const char *filename);
static int client_handle_info(Client *client, const char *filename);
static int client_handle_read(Client *client, const char *filename);
static int client_handle_write(Client *client, const char *filename, int sentence_index);
static int client_request_location(Client *client, const char *operation, const char *filename,
                                   char *out_ip, size_t ip_len, int *out_port,
                                   char *out_filename, size_t filename_len);
static int client_connect_to_storage(const char *ip, int port);
static int client_receive_read_stream(int ss_fd, const char *display_name);
static int client_receive_ss_message(int fd, ProtocolMessage *out_msg, char **out_raw);

int client_init(Client *client, const char *username, const char *ns_ip, int ns_port) {
    if (!client || !username || !ns_ip) {
        return -1;
    }
    
    // Validate username
    if (!validate_username(username)) {
        fprintf(stderr, "Invalid username\n");
        return -1;
    }
    
    // Copy configuration
    strncpy(client->username, username, MAX_USERNAME_LENGTH - 1);
    strncpy(client->ns_ip, ns_ip, MAX_IP_LENGTH - 1);
    client->ns_port = ns_port;
    client->ns_sockfd = -1;
    
    log_message(LOG_INFO, "Client", "Client initialized for user: %s", username);
    return 0;
}

int client_connect_to_ns(Client *client) {
    if (!client) {
        return -1;
    }

    // Step 1: Create the TCP socket that will carry requests to the Name Server
    client->ns_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (client->ns_sockfd < 0) {
        log_message(LOG_ERROR, "Client", "Failed to create socket: %s", strerror(errno));
        return -1;
    }

    // Step 2: Populate the destination address using the configured IP/port
    struct sockaddr_in ns_addr;
    memset(&ns_addr, 0, sizeof(ns_addr));
    ns_addr.sin_family = AF_INET;
    ns_addr.sin_port = htons(client->ns_port);
    if (inet_pton(AF_INET, client->ns_ip, &ns_addr.sin_addr) <= 0) {
        log_message(LOG_ERROR, "Client", "Invalid Name Server IP address: %s", client->ns_ip);
        close(client->ns_sockfd);
        client->ns_sockfd = -1;
        return -1;
    }

    // Step 3: Establish the TCP connection to the Name Server
    if (connect(client->ns_sockfd, (struct sockaddr *)&ns_addr, sizeof(ns_addr)) < 0) {
        log_message(LOG_ERROR, "Client", "Failed to connect to Name Server %s:%d: %s", client->ns_ip,
                    client->ns_port, strerror(errno));
        close(client->ns_sockfd);
        client->ns_sockfd = -1;
        return -1;
    }

    // Step 4: Compose the HELLO_CLIENT handshake message
    const char *fields[] = {MSG_HELLO_CLIENT, client->username};
    char *handshake = protocol_build_message(fields, 2);
    if (!handshake) {
        log_message(LOG_ERROR, "Client", "Failed to build HELLO_CLIENT message");
        close(client->ns_sockfd);
        client->ns_sockfd = -1;
        return -1;
    }

    // Step 5: Send the handshake to the Name Server
    if (protocol_send_message(client->ns_sockfd, handshake) < 0) {
        log_message(LOG_ERROR, "Client", "Failed to send HELLO_CLIENT message");
        free(handshake);
        close(client->ns_sockfd);
        client->ns_sockfd = -1;
        return -1;
    }
    free(handshake);

    // Step 6: Await the Name Server's response and verify it
    char *raw_response = protocol_receive_message(client->ns_sockfd);
    if (!raw_response) {
        log_message(LOG_ERROR, "Client", "No response from Name Server during handshake");
        close(client->ns_sockfd);
        client->ns_sockfd = -1;
        return -1;
    }

    ProtocolMessage response;
    if (protocol_parse_message(raw_response, &response) != 0) {
        log_message(LOG_ERROR, "Client", "Failed to parse Name Server handshake response");
        free(raw_response);
        close(client->ns_sockfd);
        client->ns_sockfd = -1;
        return -1;
    }

    if (protocol_is_error(&response)) {
        int err_code = protocol_get_error_code(&response);
        const char *detail = (response.field_count >= 3) ? response.fields[2] : "Unknown error";
        log_message(LOG_ERROR, "Client", "Name Server rejected handshake (code %d): %s", err_code, detail);
        protocol_free_message(&response);
        free(raw_response);
        close(client->ns_sockfd);
        client->ns_sockfd = -1;
        return -1;
    }

    log_message(LOG_INFO, "Client", "Connected to Name Server");
    protocol_free_message(&response);
    free(raw_response);
    return 0;
}


int client_process_command(Client *client, const char *input) {
    if (!client || !input) {
        return -1;
    }

    // Parse command keyword; arguments will be handled by respective helpers later
    char command[CLIENT_BUFFER_SIZE];
    if (sscanf(input, "%s", command) != 1) {
        return -1;
    }

    if (strcasecmp(command, "VIEW") == 0) {
        // Extract optional flags (e.g., "-a") if the user provided any.
        const char *flag_ptr = strchr(input, ' ');
        char flags_buffer[CLIENT_BUFFER_SIZE] = {0};
        if (flag_ptr) {
            flag_ptr++;
            while (*flag_ptr == ' ') {
                flag_ptr++;
            }
            if (*flag_ptr != '\0') {
                strncpy(flags_buffer, flag_ptr, sizeof(flags_buffer) - 1);
                trim_string(flags_buffer);
            }
        }
        const char *flags = flags_buffer[0] ? flags_buffer : NULL;
        return client_handle_view(client, flags);
    } else if (strcasecmp(command, "READ") == 0) {
        char filename[MAX_FILENAME_LENGTH];
        if (sscanf(input, "%*s %511s", filename) != 1) {
            printf("Usage: READ <filename>\n");
            return -1;
        }
        return client_handle_read(client, filename);
    } else if (strcasecmp(command, "CREATE") == 0) {
        char filename[MAX_FILENAME_LENGTH];
        if (sscanf(input, "%*s %511s", filename) != 1) {
            printf("Usage: CREATE <filename>\n");
            return -1;
        }
        return client_handle_create(client, filename);
    } else if (strcasecmp(command, "WRITE") == 0) {
        char filename[MAX_FILENAME_LENGTH];
        int sentence_index = -1;
        if (sscanf(input, "%*s %511s %d", filename, &sentence_index) != 2) {
            printf("Usage: WRITE <filename> <sentence_index>\n");
            return -1;
        }
        if (sentence_index < 0) {
            printf("Sentence index must be a non-negative integer.\n");
            return -1;
        }
        return client_handle_write(client, filename, sentence_index);
    } else if (strcasecmp(command, "UNDO") == 0) {
        // TODO: handle UNDO <filename>
    } else if (strcasecmp(command, "INFO") == 0) {
        char filename[MAX_FILENAME_LENGTH];
        if (sscanf(input, "%*s %511s", filename) != 1) {
            printf("Usage: INFO <filename>\n");
            return -1;
        }
        return client_handle_info(client, filename);
    } else if (strcasecmp(command, "DELETE") == 0) {
        char filename[MAX_FILENAME_LENGTH];
        if (sscanf(input, "%*s %511s", filename) != 1) {
            printf("Usage: DELETE <filename>\n");
            return -1;
        }
        return client_handle_delete(client, filename);
    } else if (strcasecmp(command, "STREAM") == 0) {
        // TODO: handle STREAM <filename>
    } else if (strcasecmp(command, "LIST") == 0) {
        return client_handle_list(client);
    } else if (strcasecmp(command, "ADDACCESS") == 0) {
        char perm[16];
        char filename[MAX_FILENAME_LENGTH];
        char target[MAX_USERNAME_LENGTH];
        if (sscanf(input, "%*s %15s %511s %255s", perm, filename, target) != 3) {
            printf("Usage: ADDACCESS -R|-W <filename> <username>\n");
            return -1;
        }
        return client_handle_addaccess(client, perm, filename, target);
    } else if (strcasecmp(command, "REMACCESS") == 0) {
        char filename[MAX_FILENAME_LENGTH];
        char target[MAX_USERNAME_LENGTH];
        if (sscanf(input, "%*s %511s %255s", filename, target) != 2) {
            printf("Usage: REMACCESS <filename> <username>\n");
            return -1;
        }
        return client_handle_remaccess(client, filename, target);
    } else if (strcasecmp(command, "EXEC") == 0) {
        // TODO: handle EXEC <filename>
    } else if (strcasecmp(command, "CREATEFOLDER") == 0) {
        // TODO: handle CREATEFOLDER <foldername>
    } else if (strcasecmp(command, "MOVE") == 0) {
        // TODO: handle MOVE <filename> <foldername>
    } else if (strcasecmp(command, "VIEWFOLDER") == 0) {
        // TODO: handle VIEWFOLDER <foldername>
    } else if (strcasecmp(command, "CHECKPOINT") == 0) {
        // TODO: handle CHECKPOINT <filename> <checkpoint_tag>
    } else if (strcasecmp(command, "VIEWCHECKPOINT") == 0) {
        // TODO: handle VIEWCHECKPOINT <filename> <checkpoint_tag>
    } else if (strcasecmp(command, "REVERT") == 0) {
        // TODO: handle REVERT <filename> <checkpoint_tag>
    } else if (strcasecmp(command, "LISTCHECKPOINTS") == 0) {
        // TODO: handle LISTCHECKPOINTS <filename>
    } else if (strcasecmp(command, "REQUESTACCESS") == 0) {
        // TODO: handle REQUESTACCESS <filename> <permission>
    } else if (strcasecmp(command, "APPROVEACCESS") == 0) {
        // TODO: handle APPROVEACCESS <filename> <username>
    } else if (strcasecmp(command, "REJECTACCESS") == 0) {
        // TODO: handle REJECTACCESS <filename> <username>
    } else {
        printf("Unknown command: %s\n", command);
        return -1;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Command handlers that talk to the Name Server directly
// ---------------------------------------------------------------------------

static int client_send_message(Client *client, char *message) {
    if (!client || client->ns_sockfd < 0 || !message) {
        return -1;
    }
    int rc = protocol_send_message(client->ns_sockfd, message);
    free(message);
    return rc;
}

static int client_read_response(Client *client, ProtocolMessage *out_msg, char **out_raw) {
    if (!client || client->ns_sockfd < 0 || !out_msg || !out_raw) {
        return -1;
    }

    char *raw = protocol_receive_message(client->ns_sockfd);
    if (!raw) {
        return -1;
    }
    if (protocol_parse_message(raw, out_msg) != 0) {
        free(raw);
        return -1;
    }

    *out_raw = raw;
    return 0;
}

static int client_receive_ss_message(int fd, ProtocolMessage *out_msg, char **out_raw) {
    if (fd < 0 || !out_msg || !out_raw) {
        return -1;
    }

    char *raw = protocol_receive_message(fd);
    if (!raw) {
        return -1;
    }

    if (protocol_parse_message(raw, out_msg) != 0 || out_msg->field_count == 0) {
        free(raw);
        return -1;
    }

    *out_raw = raw;
    return 0;
}

static void client_print_ns_response(const ProtocolMessage *msg) {
    if (!msg) {
        return;
    }

    if (protocol_is_error(msg)) {
        printf("Error: %s\n", msg->field_count >= 3 ? msg->fields[2] : "Unknown error");
    } else if (msg->field_count >= 2) {
        printf("%s\n", msg->fields[1]);
    } else {
        printf("OK\n");
    }
}

// Streams filenames the user can access via VIEW.
static int client_handle_view(Client *client, const char *flags) {
    if (!client || client->ns_sockfd < 0) {
        return -1;
    }

    int request_all = 0;
    int request_details = 0;
    int invalid_flag = 0;

    if (flags) {
        for (const char *p = flags; *p; p++) {
            if (*p == '-' || *p == ' ' || *p == '\t') {
                continue;
            }
            if (*p == 'a' || *p == 'A') {
                request_all = 1;
            } else if (*p == 'l' || *p == 'L') {
                request_details = 1;
            } else {
                invalid_flag = 1;
                break;
            }
        }
    }

    if (invalid_flag) {
        printf("Invalid VIEW flag. Use -a, -l, or -al.\n");
        return -1;
    }

    const char *fields[2] = {MSG_VIEW, NULL};
    int field_count = 1;
    if (flags && flags[0] != '\0') {
        fields[1] = flags;
        field_count = 2;
    }

    char *message = protocol_build_message(fields, field_count);
    if (!message) {
        return -1;
    }

    if (client_send_message(client, message) < 0) {
        printf("Failed to send VIEW command\n");
        return -1;
    }

    if (request_details) {
        printf("---------------------------------------------------------\n");
        printf("|  Filename  | Words | Chars | Last Access Time | Owner |\n");
        printf("|------------|-------|-------|------------------|-------|\n");
    }

    while (1) {
        ProtocolMessage msg;
        char *raw = NULL;
        if (client_read_response(client, &msg, &raw) != 0) {
            printf("Connection lost while viewing files\n");
            return -1;
        }

        if (msg.field_count > 0 && strcmp(msg.fields[0], RESP_OK_VIEW_END) == 0) {
            protocol_free_message(&msg);
            free(raw);
            break;
        }

        if (msg.field_count >= 2 && strcmp(msg.fields[0], RESP_OK_VIEW_L) == 0) {
            if (request_details && msg.field_count >= 6) {
                const char *filename = msg.fields[1];
                const char *owner = msg.field_count >= 3 ? msg.fields[2] : "";
                const char *words = msg.field_count >= 4 ? msg.fields[3] : "";
                const char *chars = msg.field_count >= 5 ? msg.fields[4] : "";
                const char *last_access = msg.field_count >= 6 ? msg.fields[5] : "";
                printf("| %-10s | %5s | %5s | %-16s | %-5s |\n",
                       filename ? filename : "",
                       words ? words : "",
                       chars ? chars : "",
                       last_access ? last_access : "",
                       (owner && owner[0]) ? owner : "N/A");
            } else if (!request_details) {
                printf("--> %s\n", msg.fields[1]);
            }
        } else if (protocol_is_error(&msg)) {
            client_print_ns_response(&msg);
            protocol_free_message(&msg);
            free(raw);
            return -1;
        }

        protocol_free_message(&msg);
        free(raw);
    }

    if (request_details) {
        printf("---------------------------------------------------------\n");
    }

    return 0;
}

static int client_handle_list(Client *client) {
    if (!client || client->ns_sockfd < 0) {
        return -1;
    }

    const char *fields[] = {MSG_LIST_USERS};
    char *message = protocol_build_message(fields, 1);
    if (!message) {
        return -1;
    }

    if (client_send_message(client, message) < 0) {
        printf("Failed to send LIST command\n");
        return -1;
    }

    printf("Users:\n");
    while (1) {
        ProtocolMessage msg;
        char *raw = NULL;
        if (client_read_response(client, &msg, &raw) != 0) {
            printf("Connection lost while listing users\n");
            return -1;
        }

        if (msg.field_count > 0 && strcmp(msg.fields[0], RESP_OK_LIST_END) == 0) {
            protocol_free_message(&msg);
            free(raw);
            break;
        }

        if (msg.field_count >= 2 && strcmp(msg.fields[0], RESP_OK_LIST) == 0) {
            printf(" - %s\n", msg.fields[1]);
        } else if (protocol_is_error(&msg)) {
            client_print_ns_response(&msg);
            protocol_free_message(&msg);
            free(raw);
            return -1;
        }

        protocol_free_message(&msg);
        free(raw);
    }

    return 0;
}

static int client_handle_addaccess(Client *client, const char *perm_flag, const char *filename, const char *username) {
    if (!client || client->ns_sockfd < 0) {
        return -1;
    }

    const char *permission_to_send = perm_flag;
    if (perm_flag && perm_flag[0] == '-') {
        permission_to_send = perm_flag + 1;
        if (!permission_to_send[0]) {
            permission_to_send = perm_flag;
        }
    }

    const char *fields[] = {MSG_ADDACCESS, filename, username, permission_to_send};
    char *message = protocol_build_message(fields, 4);
    if (!message) {
        return -1;
    }

    if (client_send_message(client, message) < 0) {
        printf("Failed to send ADDACCESS command\n");
        return -1;
    }

    ProtocolMessage resp;
    char *raw = NULL;
    if (client_read_response(client, &resp, &raw) != 0) {
        printf("No response from Name Server for ADDACCESS\n");
        return -1;
    }

    client_print_ns_response(&resp);
    protocol_free_message(&resp);
    free(raw);
    return 0;
}

static int client_handle_remaccess(Client *client, const char *filename, const char *username) {
    if (!client || client->ns_sockfd < 0) {
        return -1;
    }

    const char *fields[] = {MSG_REMACCESS, filename, username};
    char *message = protocol_build_message(fields, 3);
    if (!message) {
        return -1;
    }

    if (client_send_message(client, message) < 0) {
        printf("Failed to send REMACCESS command\n");
        return -1;
    }

    ProtocolMessage resp;
    char *raw = NULL;
    if (client_read_response(client, &resp, &raw) != 0) {
        printf("No response from Name Server for REMACCESS\n");
        return -1;
    }

    client_print_ns_response(&resp);
    protocol_free_message(&resp);
    free(raw);
    return 0;
}

// Function to handle the CREATE command from the client
static int client_handle_create(Client *client, const char *filename) {
    // Validate client and filename
    if (!client || client->ns_sockfd < 0 || !filename) {
        return -1;
    }

    // Ensure the filename is valid
    if (!validate_filename(filename)) {
        printf("Invalid filename.\n");
        return -1;
    }

    // Build the CREATE message to send to the Name Server
    const char *fields[] = {MSG_CREATE, filename};
    char *message = protocol_build_message(fields, 2);
    if (!message) {
        return -1;
    }

    // Send the CREATE message to the Name Server
    if (client_send_message(client, message) < 0) {
        printf("Failed to send CREATE command\n");
        return -1;
    }

    // Read the response from the Name Server
    ProtocolMessage resp;
    char *raw = NULL;
    if (client_read_response(client, &resp, &raw) != 0) {
        printf("No response from Name Server for CREATE\n");
        return -1;
    }

    // Print the response received from the Name Server
    client_print_ns_response(&resp);

    // Check if the response indicates an error
    int is_error = protocol_is_error(&resp);

    // Free allocated resources
    protocol_free_message(&resp);
    free(raw);

    // Return success or error based on the response
    return is_error ? -1 : 0;
}

static int client_handle_delete(Client *client, const char *filename) {
    if (!client || client->ns_sockfd < 0 || !filename) {
        return -1;
    }

    if (!validate_filename(filename)) {
        printf("Invalid filename.\n");
        return -1;
    }

    const char *fields[] = {MSG_DELETE, filename};
    char *message = protocol_build_message(fields, 2);
    if (!message) {
        return -1;
    }

    if (client_send_message(client, message) < 0) {
        printf("Failed to send DELETE command\n");
        return -1;
    }

    ProtocolMessage resp;
    char *raw = NULL;
    if (client_read_response(client, &resp, &raw) != 0) {
        printf("No response from Name Server for DELETE\n");
        return -1;
    }

    client_print_ns_response(&resp);
    int is_error = protocol_is_error(&resp);

    protocol_free_message(&resp);
    free(raw);

    return is_error ? -1 : 0;
}

static int client_handle_info(Client *client, const char *filename) {
    if (!client || client->ns_sockfd < 0 || !filename) {
        return -1;
    }

    if (!validate_filename(filename)) {
        printf("Invalid filename.\n");
        return -1;
    }

    const char *fields[] = {MSG_INFO, filename};
    char *message = protocol_build_message(fields, 2);
    if (!message) {
        return -1;
    }

    if (client_send_message(client, message) < 0) {
        printf("Failed to send INFO command\n");
        return -1;
    }

    int info_started = 0;
    char display_name[MAX_FILENAME_LENGTH] = {0};
    safe_strcpy(display_name, filename, sizeof(display_name));

    while (1) {
        ProtocolMessage msg;
        char *raw = NULL;
        if (client_read_response(client, &msg, &raw) != 0) {
            printf("Connection lost while receiving info\n");
            return -1;
        }

        if (msg.field_count == 0) {
            printf("Malformed response while receiving info\n");
            protocol_free_message(&msg);
            free(raw);
            return -1;
        }

        if (protocol_is_error(&msg)) {
            client_print_ns_response(&msg);
            protocol_free_message(&msg);
            free(raw);
            return -1;
        }

        const char *type = msg.fields[0];

        if (strcmp(type, RESP_OK_INFO_START) == 0) {
            if (msg.field_count >= 2 && msg.fields[1] && msg.fields[1][0]) {
                safe_strcpy(display_name, msg.fields[1], sizeof(display_name));
            }
            printf("---- %s ----\n", display_name[0] ? display_name : "File Info");
            info_started = 1;
        } else if (strcmp(type, RESP_INFO_LINE) == 0) {
            if (!info_started) {
                printf("Unexpected INFO_LINE without INFO_START\n");
                protocol_free_message(&msg);
                free(raw);
                return -1;
            }
            if (msg.field_count >= 2 && msg.fields[1]) {
                printf("%s\n", msg.fields[1]);
            }
        } else if (strcmp(type, RESP_OK_INFO_END) == 0) {
            if (!info_started) {
                printf("Unexpected INFO_END without INFO_START\n");
                protocol_free_message(&msg);
                free(raw);
                return -1;
            }
            printf("\n");
            protocol_free_message(&msg);
            free(raw);
            break;
        } else {
            printf("Unexpected response while receiving info: %s\n", type ? type : "<unknown>");
            protocol_free_message(&msg);
            free(raw);
            return -1;
        }

        protocol_free_message(&msg);
        free(raw);
    }

    return 0;
}

static int client_request_location(Client *client, const char *operation, const char *filename,
                                   char *out_ip, size_t ip_len, int *out_port,
                                   char *out_filename, size_t filename_len) {
    if (!client || client->ns_sockfd < 0 || !operation || !filename || !out_ip || !out_port) {
        return -1;
    }

    const char *fields[] = {MSG_REQ_LOC, operation, filename};
    char *message = protocol_build_message(fields, 3);
    if (!message) {
        return -1;
    }

    if (client_send_message(client, message) < 0) {
        printf("Failed to contact Name Server for REQ_LOC\n");
        return -1;
    }

    ProtocolMessage resp;
    char *raw = NULL;
    if (client_read_response(client, &resp, &raw) != 0) {
        printf("No response from Name Server for REQ_LOC\n");
        return -1;
    }

    int status = -1;
    if (protocol_is_error(&resp)) {
        client_print_ns_response(&resp);
    } else if (resp.field_count < 4 || strcmp(resp.fields[0], RESP_OK_LOC) != 0) {
        printf("Unexpected response from Name Server for REQ_LOC\n");
    } else {
        char *endptr = NULL;
        long port_val = strtol(resp.fields[3], &endptr, 10);
        if (!resp.fields[3][0] || (endptr && *endptr != '\0')) {
            printf("Invalid port in Name Server response\n");
        } else if (!is_valid_port((int)port_val)) {
            printf("Out-of-range port in Name Server response\n");
        } else if (safe_strcpy(out_ip, resp.fields[2], ip_len) != 0) {
            printf("Failed to copy storage server IP\n");
        } else if (out_filename && filename_len > 0 && safe_strcpy(out_filename, resp.fields[1], filename_len) != 0) {
            printf("Failed to copy resolved filename\n");
        } else {
            *out_port = (int)port_val;
            status = 0;
        }
    }

    protocol_free_message(&resp);
    free(raw);
    return status;
}

static int client_connect_to_storage(const char *ip, int port) {
    if (!ip || !is_valid_port(port)) {
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("Failed to create socket to Storage Server\n");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        printf("Invalid Storage Server IP address\n");
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("Failed to connect to Storage Server %s:%d\n", ip, port);
        close(fd);
        return -1;
    }

    return fd;
}

static int client_handle_write(Client *client, const char *filename, int sentence_index) {
    if (!client || client->ns_sockfd < 0 || !filename) {
        return -1;
    }

    if (!validate_filename(filename)) {
        printf("Invalid filename.\n");
        return -1;
    }

    if (sentence_index < 0) {
        printf("Sentence index must be a non-negative integer.\n");
        return -1;
    }

    char location_ip[MAX_IP_LENGTH] = {0};
    char resolved_filename[MAX_FILENAME_LENGTH] = {0};
    int location_port = 0;
    if (client_request_location(client, "WRITE", filename,
                                location_ip, sizeof(location_ip),
                                &location_port, resolved_filename,
                                sizeof(resolved_filename)) != 0) {
        return -1;
    }

    int ss_fd = client_connect_to_storage(location_ip, location_port);
    if (ss_fd < 0) {
        return -1;
    }

    int rc = -1;
    char *current_sentence = NULL;
    char locked_filename[MAX_FILENAME_LENGTH] = {0};
    int locked_sentence_index = sentence_index;
    const char *target_filename = resolved_filename[0] ? resolved_filename : filename;

    char sentence_buf[32];
    snprintf(sentence_buf, sizeof(sentence_buf), "%d", sentence_index);
    const char *lock_fields[] = {MSG_REQ_WRITE_LOCK, client->username, target_filename, sentence_buf};
    char *lock_msg = protocol_build_message(lock_fields, 4);
    if (!lock_msg) {
        printf("Failed to build WRITE lock request.\n");
        goto cleanup;
    }

    if (protocol_send_message(ss_fd, lock_msg) < 0) {
        printf("Failed to send WRITE lock request to Storage Server.\n");
        free(lock_msg);
        goto cleanup;
    }
    free(lock_msg);

    int got_lock = 0;
    int got_content = 0;
    while (!got_lock || !got_content) {
        ProtocolMessage msg;
        char *raw = NULL;
        if (client_receive_ss_message(ss_fd, &msg, &raw) != 0) {
            printf("Connection lost while acquiring write lock.\n");
            goto cleanup;
        }

        if (protocol_is_error(&msg)) {
            client_print_ns_response(&msg);
            protocol_free_message(&msg);
            free(raw);
            goto cleanup;
        }

        const char *type = msg.fields[0];
        if (strcmp(type, RESP_OK_LOCKED) == 0) {
            got_lock = 1;
            if (msg.field_count >= 2 && msg.fields[1]) {
                safe_strcpy(locked_filename, msg.fields[1], sizeof(locked_filename));
            }
            if (msg.field_count >= 3 && msg.fields[2]) {
                char *endptr = NULL;
                long idx_val = strtol(msg.fields[2], &endptr, 10);
                if (endptr && *endptr == '\0' && idx_val >= 0 && idx_val <= INT_MAX) {
                    locked_sentence_index = (int)idx_val;
                }
            }
        } else if (strcmp(type, RESP_OK_CONTENT) == 0) {
            got_content = 1;
            const char *payload = (msg.field_count >= 2 && msg.fields[1]) ? msg.fields[1] : "";
            current_sentence = strdup(payload);
            if (!current_sentence) {
                printf("Failed to allocate memory for sentence copy.\n");
                protocol_free_message(&msg);
                free(raw);
                goto cleanup;
            }
        } else {
            printf("Unexpected response from Storage Server: %s\n", type ? type : "<unknown>");
            protocol_free_message(&msg);
            free(raw);
            goto cleanup;
        }

        protocol_free_message(&msg);
        free(raw);
    }

    const char *display_name = locked_filename[0] ? locked_filename : target_filename;
    printf("Locked sentence %d in %s.\n", locked_sentence_index, display_name);
    printf("Current sentence: %s\n", current_sentence && current_sentence[0] ? current_sentence : "(empty)");
    printf("Enter '<word_index> <content>' to edit, or 'ETIRW' to finalize.\n");
    printf("Word indices start at 0; punctuation marks (., !, ?) mark sentence boundaries.\n");

    char input_line[CLIENT_BUFFER_SIZE];
    int session_active = 1;

    while (session_active) {
        printf("write> ");
        fflush(stdout);

        if (!fgets(input_line, sizeof(input_line), stdin)) {
            printf("\nInput closed; aborting write session.\n");
            goto cleanup;
        }

        size_t len = strlen(input_line);
        if (len > 0 && input_line[len - 1] == '\n') {
            input_line[len - 1] = '\0';
        }

        char *cursor = input_line;
        while (*cursor == ' ' || *cursor == '\t') {
            cursor++;
        }

        if (*cursor == '\0') {
            continue;
        }

        if (strcmp(cursor, MSG_ETIRW) == 0) {
            const char *etirw_fields[] = {MSG_ETIRW};
            char *etirw_msg = protocol_build_message(etirw_fields, 1);
            if (!etirw_msg) {
                printf("Failed to build ETIRW message.\n");
                goto cleanup;
            }

            if (protocol_send_message(ss_fd, etirw_msg) < 0) {
                printf("Failed to send ETIRW to Storage Server.\n");
                free(etirw_msg);
                goto cleanup;
            }
            free(etirw_msg);

            while (1) {
                ProtocolMessage msg;
                char *raw = NULL;
                if (client_receive_ss_message(ss_fd, &msg, &raw) != 0) {
                    printf("Connection lost while finalizing write.\n");
                    goto cleanup;
                }

                if (protocol_is_error(&msg)) {
                    client_print_ns_response(&msg);
                    protocol_free_message(&msg);
                    free(raw);
                    break;
                }

                const char *type = msg.fields[0];
                if (strcmp(type, RESP_OK_WRITE_DONE) == 0) {
                    printf("Write committed successfully.\n");
                    protocol_free_message(&msg);
                    free(raw);
                    rc = 0;
                    session_active = 0;
                    goto cleanup;
                } else if (strcmp(type, RESP_OK_CONTENT) == 0) {
                    if (msg.field_count >= 2 && msg.fields[1]) {
                        free(current_sentence);
                        current_sentence = strdup(msg.fields[1]);
                        if (current_sentence) {
                            printf("Current sentence: %s\n",
                                   current_sentence[0] ? current_sentence : "(empty)");
                        }
                    }
                } else if (strcmp(type, RESP_OK) == 0) {
                    if (msg.field_count >= 2) {
                        printf("%s\n", msg.fields[1]);
                    } else {
                        printf("OK\n");
                    }
                } else {
                    printf("Unexpected response from Storage Server: %s\n", type);
                }

                protocol_free_message(&msg);
                free(raw);
            }

            continue;
        }

        errno = 0;
        char *endptr = NULL;
        long idx_val = strtol(cursor, &endptr, 10);
        if (endptr == cursor || errno != 0 || idx_val < 0 || idx_val > INT_MAX) {
            printf("Invalid word index. Use a non-negative integer.\n");
            continue;
        }
        int word_index = (int)idx_val;

        while (*endptr == ' ' || *endptr == '\t') {
            endptr++;
        }
        const char *content = endptr;

        char word_buf[32];
        snprintf(word_buf, sizeof(word_buf), "%d", word_index);
        const char *write_fields[] = {MSG_WRITE_DATA, word_buf, content};
        char *write_msg = protocol_build_message(write_fields, 3);
        if (!write_msg) {
            printf("Failed to build WRITE_DATA message.\n");
            continue;
        }

        if (protocol_send_message(ss_fd, write_msg) < 0) {
            printf("Failed to send WRITE_DATA to Storage Server.\n");
            free(write_msg);
            goto cleanup;
        }
        free(write_msg);

        while (1) {
            ProtocolMessage msg;
            char *raw = NULL;
            if (client_receive_ss_message(ss_fd, &msg, &raw) != 0) {
                printf("Connection lost while applying edit.\n");
                goto cleanup;
            }

            if (protocol_is_error(&msg)) {
                client_print_ns_response(&msg);
                protocol_free_message(&msg);
                free(raw);
                break;
            }

            const char *type = msg.fields[0];
            if (strcmp(type, RESP_OK) == 0) {
                if (msg.field_count >= 2) {
                    printf("%s\n", msg.fields[1]);
                } else {
                    printf("Edit applied.\n");
                }
                protocol_free_message(&msg);
                free(raw);
                break;
            } else if (strcmp(type, RESP_OK_CONTENT) == 0) {
                if (msg.field_count >= 2 && msg.fields[1]) {
                    free(current_sentence);
                    current_sentence = strdup(msg.fields[1]);
                    if (current_sentence) {
                        printf("Current sentence: %s\n",
                               current_sentence[0] ? current_sentence : "(empty)");
                    }
                }
            } else if (strcmp(type, RESP_OK_WRITE_DONE) == 0) {
                printf("Write committed on server.\n");
                protocol_free_message(&msg);
                free(raw);
                rc = 0;
                session_active = 0;
                goto cleanup;
            } else {
                printf("Unexpected response from Storage Server: %s\n", type);
                protocol_free_message(&msg);
                free(raw);
                break;
            }

            protocol_free_message(&msg);
            free(raw);
        }
    }

cleanup:
    if (current_sentence) {
        free(current_sentence);
    }
    if (ss_fd >= 0) {
        close(ss_fd);
    }
    return rc;
}

static int client_receive_read_stream(int ss_fd, const char *display_name) {
    int saw_start = 0;
    int content_printed = 0;
    int last_char_newline = 1;

    while (1) {
        char *raw = protocol_receive_message(ss_fd);
        if (!raw) {
            printf("Connection lost while reading file\n");
            return -1;
        }

        ProtocolMessage msg;
        if (protocol_parse_message(raw, &msg) != 0 || msg.field_count == 0) {
            free(raw);
            printf("Received malformed response from Storage Server\n");
            return -1;
        }

        if (protocol_is_error(&msg)) {
            client_print_ns_response(&msg);
            protocol_free_message(&msg);
            free(raw);
            return -1;
        }

        const char *type = msg.fields[0];
        if (strcmp(type, RESP_OK_READ_START) == 0) {
            const char *name = (msg.field_count >= 2 && msg.fields[1][0]) ? msg.fields[1] : display_name;
            if (name && name[0]) {
                printf("---- %s ----\n", name);
            } else {
                printf("---- File ----\n");
            }
            saw_start = 1;
        } else if (strcmp(type, RESP_OK_CONTENT) == 0) {
            if (msg.field_count >= 2) {
                const char *payload = msg.fields[1];
                size_t len = strlen(payload);
                if (len > 0) {
                    fwrite(payload, 1, len, stdout);
                    content_printed = 1;
                    last_char_newline = (payload[len - 1] == '\n');
                }
            }
        } else if (strcmp(type, RESP_OK_READ_END) == 0) {
            if (!saw_start) {
                printf("Unexpected READ_END without READ_START\n");
                protocol_free_message(&msg);
                free(raw);
                return -1;
            }
            if (!content_printed) {
                printf("(empty file)\n");
            } else if (!last_char_newline) {
                printf("\n");
            }
            fflush(stdout);
            protocol_free_message(&msg);
            free(raw);
            return 0;
        } else {
            printf("Unexpected response from Storage Server: %s\n", type);
            protocol_free_message(&msg);
            free(raw);
            return -1;
        }

        protocol_free_message(&msg);
        free(raw);
    }
}

static int client_handle_read(Client *client, const char *filename) {
    if (!client || client->ns_sockfd < 0 || !filename) {
        return -1;
    }

    if (!validate_filename(filename)) {
        printf("Invalid filename.\n");
        return -1;
    }

    char location_ip[MAX_IP_LENGTH] = {0};
    char resolved_filename[MAX_FILENAME_LENGTH] = {0};
    int location_port = 0;
    if (client_request_location(client, "READ", filename,
                                location_ip, sizeof(location_ip),
                                &location_port, resolved_filename,
                                sizeof(resolved_filename)) != 0) {
        return -1;
    }

    int ss_fd = client_connect_to_storage(location_ip, location_port);
    if (ss_fd < 0) {
        return -1;
    }

    const char *req_fields[] = {MSG_REQ_READ, client->username, resolved_filename[0] ? resolved_filename : filename};
    char *req = protocol_build_message(req_fields, 3);
    if (!req) {
        close(ss_fd);
        return -1;
    }

    if (protocol_send_message(ss_fd, req) < 0) {
        printf("Failed to send READ request to Storage Server\n");
        free(req);
        close(ss_fd);
        return -1;
    }
    free(req);

    int rc = client_receive_read_stream(ss_fd, resolved_filename[0] ? resolved_filename : filename);
    close(ss_fd);
    return rc;
}

int client_start(Client *client) {
    if (!client) {
        return -1;
    }
    
    printf("Welcome, %s!\n", client->username);
    printf("Type 'help' for available commands, 'quit' to exit.\n\n");
    
    char input[CLIENT_BUFFER_SIZE];
    
    while (1) {
        printf("> ");
        fflush(stdout);
        
        if (fgets(input, sizeof(input), stdin) == NULL) {
            break;
        }
        
        // Remove trailing newline
        size_t len = strlen(input);
        if (len > 0 && input[len - 1] == '\n') {
            input[len - 1] = '\0';
        }
        
        // Check for quit
        if (strcmp(input, "quit") == 0 || strcmp(input, "exit") == 0) {
            break;
        }
        
        // Check for help
        if (strcmp(input, "help") == 0) {
            printf("Available commands:\n");
            printf("  CREATE <filename>\n");
            printf("  DELETE <filename>\n");
            printf("  READ <filename>\n");
            printf("  WRITE <filename> <sentence_index>\n");
            printf("  VIEW [-a] [-l] [-al]\n");
            printf("  INFO <filename>\n");
            printf("  STREAM <filename>\n");
            printf("  UNDO <filename>\n");
            printf("  EXEC <filename>\n");
            printf("  LIST\n");
            printf("  ADDACCESS -R|-W <filename> <username>\n");
            printf("  REMACCESS <filename> <username>\n");
            printf("  quit/exit - Exit the client\n");
            continue;
        }
        
        // Process command
        if (client_process_command(client, input) != 0) {
            printf("Error processing command\n");
        }
    }
    
    return 0;
}

void client_cleanup(Client *client) {
    if (!client) {
        return;
    }
    
    if (client->ns_sockfd >= 0) {
        close(client->ns_sockfd);
    }
    
    log_message(LOG_INFO, "Client", "Client cleanup complete");
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <username> <ns_ip> <ns_port>\n", argv[0]);
        return 1;
    }
    
    // Initialize logging
    log_init("client.log", LOG_INFO);
    
    // Create client
    Client client;
    if (client_init(&client, argv[1], argv[2], atoi(argv[3])) != 0) {
        fprintf(stderr, "Failed to initialize client\n");
        return 1;
    }
    
    // Connect to Name Server
    if (client_connect_to_ns(&client) != 0) {
        fprintf(stderr, "Failed to connect to Name Server\n");
        client_cleanup(&client);
        return 1;
    }
    
    // Start interactive session
    client_start(&client);
    
    // Cleanup
    client_cleanup(&client);
    log_cleanup();
    
    printf("Goodbye!\n");
    return 0;
}
