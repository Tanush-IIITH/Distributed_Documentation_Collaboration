#include "client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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
    
    // TODO: Implement NS connection
    // 1. Create socket
    // 2. Connect to NS
    // 3. Send HELLO_CLIENT message
    // 4. Wait for OK response
    
    log_message(LOG_INFO, "Client", "Connected to Name Server");
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
    
    // TODO: Implement command processing
    // Parse input and route to appropriate handler
    // Commands: CREATE, DELETE, READ, WRITE, VIEW, INFO, STREAM, 
    //           UNDO, EXEC, LIST, ADDACCESS, REMACCESS, etc.
    
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
