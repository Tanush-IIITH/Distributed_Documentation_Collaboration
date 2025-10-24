#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>

// ============================================================================
// ERROR MESSAGES
// ============================================================================

static const char* error_messages[] = {
    [ERR_USERNAME_INVALID] = "Username invalid or already connected.",
    [ERR_USER_NOT_FOUND] = "User not found.",
    [ERR_INVALID_REQUEST] = "Invalid index or arguments.",
    [ERR_PERMISSION_DENIED] = "Permission denied.",
    [ERR_FILE_NOT_FOUND] = "File not found.",
    [ERR_FILE_EXISTS] = "File already exists.",
    [ERR_SENTENCE_LOCKED] = "Sentence is locked by another user.",
    [ERR_INTERNAL_ERROR] = "Internal server error.",
    [ERR_DISK_ERROR] = "Disk error.",
    [ERR_NO_UNDO_HISTORY] = "No undo history available for this file.",
    [ERR_DELETE_FAILED] = "Failed to delete file from disk.",
    [ERR_EXEC_FAILED] = "Command execution failed on server.",
};

#define ERROR_MESSAGES_SIZE (sizeof(error_messages) / sizeof(error_messages[0]))

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

const char* protocol_get_error_message(int error_code) {
    if (error_code >= 0 && error_code < ERROR_MESSAGES_SIZE && error_messages[error_code] != NULL) {
        return error_messages[error_code];
    }
    return "Unknown error.";
}

// ============================================================================
// MESSAGE PARSING AND BUILDING
// ============================================================================

int protocol_parse_message(const char *raw, ProtocolMessage *msg) {
    if (!raw || !msg) {
        return -1;
    }

    // Initialize message structure
    memset(msg, 0, sizeof(ProtocolMessage));
    strncpy(msg->raw_message, raw, MAX_MESSAGE_SIZE - 1);
    msg->raw_message[MAX_MESSAGE_SIZE - 1] = '\0';

    // Remove trailing newline if present
    size_t len = strlen(msg->raw_message);
    if (len > 0 && msg->raw_message[len - 1] == '\n') {
        msg->raw_message[len - 1] = '\0';
    }

    // Create a working copy for tokenization
    char *work_copy = strdup(msg->raw_message);
    if (!work_copy) {
        return -1;
    }

    // Tokenize by delimiter
    char *token = strtok(work_copy, PROTOCOL_DELIMITER);
    msg->field_count = 0;

    while (token != NULL && msg->field_count < MAX_FIELDS) {
        msg->fields[msg->field_count] = strdup(token);
        if (!msg->fields[msg->field_count]) {
            free(work_copy);
            protocol_free_message(msg);
            return -1;
        }
        msg->field_count++;
        token = strtok(NULL, PROTOCOL_DELIMITER);
    }

    free(work_copy);
    return 0;
}

char* protocol_build_message(const char **fields, int field_count) {
    if (!fields || field_count <= 0) {
        return NULL;
    }

    // Calculate total size needed
    size_t total_size = 0;
    for (int i = 0; i < field_count; i++) {
        if (!fields[i]) {
            return NULL;
        }
        total_size += strlen(fields[i]);
        if (i < field_count - 1) {
            total_size += strlen(PROTOCOL_DELIMITER);
        }
    }
    total_size += strlen(PROTOCOL_TERMINATOR) + 1; // +1 for null terminator

    // Allocate buffer
    char *message = (char *)malloc(total_size);
    if (!message) {
        return NULL;
    }

    // Build message
    message[0] = '\0';
    for (int i = 0; i < field_count; i++) {
        strcat(message, fields[i]);
        if (i < field_count - 1) {
            strcat(message, PROTOCOL_DELIMITER);
        }
    }
    strcat(message, PROTOCOL_TERMINATOR);

    return message;
}

char* protocol_build_error(int error_code, const char *error_msg) {
    char code_str[16];
    snprintf(code_str, sizeof(code_str), "%d", error_code);

    const char *fields[3];
    fields[0] = RESP_ERR;
    fields[1] = code_str;
    fields[2] = error_msg ? error_msg : protocol_get_error_message(error_code);

    return protocol_build_message(fields, 3);
}

char* protocol_build_ok(const char *message) {
    if (message) {
        const char *fields[2] = {RESP_OK, message};
        return protocol_build_message(fields, 2);
    } else {
        const char *fields[1] = {RESP_OK};
        return protocol_build_message(fields, 1);
    }
}

void protocol_free_message(ProtocolMessage *msg) {
    if (!msg) {
        return;
    }

    for (int i = 0; i < msg->field_count; i++) {
        if (msg->fields[i]) {
            free(msg->fields[i]);
            msg->fields[i] = NULL;
        }
    }
    msg->field_count = 0;
}

// ============================================================================
// NETWORK I/O
// ============================================================================

int protocol_send_message(int sockfd, const char *message) {
    if (sockfd < 0 || !message) {
        return -1;
    }

    size_t len = strlen(message);
    ssize_t total_sent = 0;

    while (total_sent < len) {
        ssize_t sent = send(sockfd, message + total_sent, len - total_sent, 0);
        if (sent < 0) {
            if (errno == EINTR) {
                continue; // Interrupted, retry
            }
            return -1; // Error
        }
        total_sent += sent;
    }

    return total_sent;
}

char* protocol_receive_message(int sockfd) {
    if (sockfd < 0) {
        return NULL;
    }

    char *buffer = (char *)malloc(MAX_MESSAGE_SIZE);
    if (!buffer) {
        return NULL;
    }

    size_t received = 0;
    int found_terminator = 0;

    while (received < MAX_MESSAGE_SIZE - 1) {
        ssize_t n = recv(sockfd, buffer + received, 1, 0);
        
        if (n < 0) {
            if (errno == EINTR) {
                continue; // Interrupted, retry
            }
            free(buffer);
            return NULL; // Error
        } else if (n == 0) {
            // Connection closed
            if (received == 0) {
                free(buffer);
                return NULL;
            }
            break;
        }

        // Check if we received the terminator
        if (buffer[received] == '\n') {
            found_terminator = 1;
            received++;
            break;
        }

        received++;
    }

    buffer[received] = '\0';

    if (!found_terminator && received == MAX_MESSAGE_SIZE - 1) {
        // Message too long
        free(buffer);
        return NULL;
    }

    return buffer;
}

// ============================================================================
// MESSAGE VALIDATION
// ============================================================================

int protocol_validate_message(const char *message) {
    if (!message) {
        return 0;
    }

    size_t len = strlen(message);
    
    // Check if message ends with terminator
    if (len == 0 || message[len - 1] != '\n') {
        return 0;
    }

    // Check message size
    if (len > MAX_MESSAGE_SIZE) {
        return 0;
    }

    return 1;
}

int protocol_is_error(const ProtocolMessage *msg) {
    if (!msg || msg->field_count == 0) {
        return 0;
    }

    return strcmp(msg->fields[0], RESP_ERR) == 0;
}

int protocol_get_error_code(const ProtocolMessage *msg) {
    if (!protocol_is_error(msg) || msg->field_count < 2) {
        return -1;
    }

    return atoi(msg->fields[1]);
}
