#include "client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

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
        // TODO: handle VIEW (with optional flags -a, -l)
    } else if (strcasecmp(command, "READ") == 0) {
        // TODO: handle READ <filename>
    } else if (strcasecmp(command, "CREATE") == 0) {
        // TODO: handle CREATE <filename>
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
        // TODO: handle LIST users (LIST or LIST USERS)
    } else if (strcasecmp(command, "ADDACCESS") == 0) {
        // TODO: handle ADDACCESS -R|-W <filename> <username>
    } else if (strcasecmp(command, "REMACCESS") == 0) {
        // TODO: handle REMACCESS <filename> <username>
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
