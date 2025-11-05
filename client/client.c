#include "client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static int client_handle_read(Client *client, const char *filename);

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

int client_send_command(Client *client, const char *command) {
    if (!client || !command) {
        return -1;
    }
    
    // TODO: Implement command sending
    // 1. Parse command
    // 2. Build protocol message
    // 3. Send to NS or SS (depending on command)
    // 4. Handle response
    
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
        // TODO: handle READ <filename>
    } else if (strcasecmp(command, "CREATE") == 0) {
        char filename[MAX_FILENAME_LENGTH];
        if (sscanf(input, "%*s %511s", filename) != 1) {
            printf("Usage: CREATE <filename>\n");
            return -1;
        }
        return client_handle_create(client, filename);
    } else if (strcasecmp(command, "WRITE") == 0) {
        // TODO: handle WRITE <filename> <sentence_number>
    } else if (strcasecmp(command, "UNDO") == 0) {
        // TODO: handle UNDO <filename>
    } else if (strcasecmp(command, "INFO") == 0) {
        // TODO: handle INFO <filename>
    } else if (strcasecmp(command, "DELETE") == 0) {
        // TODO: handle DELETE <filename>
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

    printf("%s files%s:\n", request_all ? "All" : "Accessible", request_details ? " (detailed)" : "");
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
            if (msg.field_count >= 7) {
                printf(" - %s\n", msg.fields[1]);
                printf("   owner: %s\n", msg.fields[2][0] ? msg.fields[2] : "N/A");
                printf("   words: %s, chars: %s\n", msg.fields[3], msg.fields[4]);
                printf("   last access: %s\n", msg.fields[5]);
                printf("   last modified: %s\n", msg.fields[6]);
            } else {
                printf(" - %s\n", msg.fields[1]);
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

    const char *fields[] = {MSG_ADDACCESS, filename, username, perm_flag};
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

int client_start(Client *client) {
    if (!client) {
        return -1;
    }
    
    printf("Welcome, %s!\n", client->username);
    printf("Type 'help' for available commands, 'quit' to exit.\n\n");
    
    char input[CLIENT_BUFFER_SIZE];
    
    // TODO: Implement main client loop
    // 1. Read user input
    // 2. Process commands
    // 3. Display results
    // 4. Handle errors
    
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
