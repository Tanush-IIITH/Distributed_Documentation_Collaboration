#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>

// ============================================================================
// PROTOCOL CONSTANTS
// ============================================================================

#define PROTOCOL_DELIMITER "|"
#define PROTOCOL_TERMINATOR "\n"
#define MAX_MESSAGE_SIZE 65536
#define MAX_FIELD_SIZE 4096
#define MAX_FIELDS 32

// ============================================================================
// ERROR CODES
// ============================================================================

// Client/User Errors (100-199)
#define ERR_USERNAME_INVALID 101
#define ERR_USER_NOT_FOUND 102

// File/Resource Errors (400-499)
#define ERR_INVALID_REQUEST 400
#define ERR_PERMISSION_DENIED 403
#define ERR_FILE_NOT_FOUND 404
#define ERR_FILE_EXISTS 409
#define ERR_SENTENCE_LOCKED 423

// Server Errors (500-599)
#define ERR_INTERNAL_ERROR 500
#define ERR_DISK_ERROR 501
#define ERR_NO_UNDO_HISTORY 502
#define ERR_DELETE_FAILED 503
#define ERR_EXEC_FAILED 504

// ============================================================================
// MESSAGE TYPES - HANDSHAKE & INITIALIZATION
// ============================================================================

// Client -> NS: HELLO_CLIENT|username\n
#define MSG_HELLO_CLIENT "HELLO_CLIENT"

// SS -> NS: HELLO_SS|ip_for_ns_comms|port_for_ns_comms|ip_for_client_comms|port_for_client_comms\n
#define MSG_HELLO_SS "HELLO_SS"

// SS -> NS: SS_HAS_FILE|filename\n
#define MSG_SS_HAS_FILE "SS_HAS_FILE"

// SS -> NS: SS_FILES_DONE\n
#define MSG_SS_FILES_DONE "SS_FILES_DONE"

// ============================================================================
// MESSAGE TYPES - NS-HANDLED COMMANDS (Client -> NS)
// ============================================================================

// C -> NS: LIST_USERS\n
#define MSG_LIST_USERS "LIST_USERS"

// C -> NS: ADDACCESS|filename|username_to_add|permission\n
// permission: "R" for Read, "W" for Write
#define MSG_ADDACCESS "ADDACCESS"

// C -> NS: REMACCESS|filename|username_to_remove\n
#define MSG_REMACCESS "REMACCESS"

// ============================================================================
// MESSAGE TYPES - COORDINATED COMMANDS (C-NS-SS)
// ============================================================================

// CREATE
// C -> NS: CREATE|filename\n
#define MSG_CREATE "CREATE"
// NS -> SS: CREATE_FILE|filename|owner_username\n
#define MSG_CREATE_FILE "CREATE_FILE"

// DELETE
// C -> NS: DELETE|filename\n
#define MSG_DELETE "DELETE"
// NS -> SS: DELETE_FILE|filename\n
#define MSG_DELETE_FILE "DELETE_FILE"

// VIEW and INFO
// C -> NS: INFO|filename\n
#define MSG_INFO "INFO"
// C -> NS: VIEW|flags (flags can be empty, "-a", "-l", "-al")
#define MSG_VIEW "VIEW"
// NS -> SS: GET_STATS|filename\n
#define MSG_GET_STATS "GET_STATS"

// EXEC
// C -> NS: EXEC|filename\n
#define MSG_EXEC "EXEC"
// NS -> SS: GET_CONTENT|filename\n
#define MSG_GET_CONTENT "GET_CONTENT"

// ============================================================================
// MESSAGE TYPES - LOCATION REQUEST (C -> NS)
// ============================================================================

// C -> NS: REQ_LOC|COMMAND_NAME|filename\n
// COMMAND_NAME can be: READ, WRITE, STREAM, UNDO
#define MSG_REQ_LOC "REQ_LOC"

// ============================================================================
// MESSAGE TYPES - DIRECT C-SS COMMUNICATION
// ============================================================================

// READ
// C -> SS: REQ_READ|username|filename\n
#define MSG_REQ_READ "REQ_READ"

// STREAM
// C -> SS: REQ_STREAM|username|filename\n
#define MSG_REQ_STREAM "REQ_STREAM"

// UNDO
// C -> SS: REQ_UNDO|username|filename\n
#define MSG_REQ_UNDO "REQ_UNDO"

// WRITE
// C -> SS: REQ_WRITE_LOCK|username|filename|sentence_index\n
#define MSG_REQ_WRITE_LOCK "REQ_WRITE_LOCK"
// C -> SS: WRITE_DATA|word_index|content_string\n
#define MSG_WRITE_DATA "WRITE_DATA"
// C -> SS: ETIRW\n
#define MSG_ETIRW "ETIRW"

// ============================================================================
// MESSAGE TYPES - BONUS FUNCTIONALITY
// ============================================================================

// Hierarchical Folders
// C -> NS: CREATEFOLDER|path/foldername\n
#define MSG_CREATEFOLDER "CREATEFOLDER"
// C -> NS: MOVE|source_path/filename|dest_path/foldername\n
#define MSG_MOVE "MOVE"
// C -> NS: VIEWFOLDER|path/foldername\n
#define MSG_VIEWFOLDER "VIEWFOLDER"

// Checkpoints
// C -> SS: REQ_CHECKPOINT|username|filename|tag_name\n
#define MSG_REQ_CHECKPOINT "REQ_CHECKPOINT"
// C -> SS: REQ_REVERT|username|filename|tag_name\n
#define MSG_REQ_REVERT "REQ_REVERT"
// C -> SS: REQ_LIST_CHECKPOINTS|username|filename\n
#define MSG_REQ_LIST_CHECKPOINTS "REQ_LIST_CHECKPOINTS"

// Fault Tolerance
// NS -> SS_Replica: REPLICATE_FILE|filename|primary_ss_ip|primary_ss_port\n
#define MSG_REPLICATE_FILE "REPLICATE_FILE"
// SS_Primary -> NS: WRITE_COMPLETE|filename\n
#define MSG_WRITE_COMPLETE "WRITE_COMPLETE"
// NS -> SS_Replica: SYNC_FILE|filename|primary_ss_ip|primary_ss_port\n
#define MSG_SYNC_FILE "SYNC_FILE"
// NS -> SS: PING\n
#define MSG_PING "PING"
// SS -> NS: PONG\n
#define MSG_PONG "PONG"

// ============================================================================
// RESPONSE TYPES - SUCCESS
// ============================================================================

// Generic OK response
#define RESP_OK "OK"

// Specific OK responses
#define RESP_OK_CREATE "OK_CREATE"
#define RESP_OK_DELETE "OK_DELETE"
#define RESP_OK_STATS "OK_STATS"
#define RESP_OK_CONTENT "OK_CONTENT"
#define RESP_OK_LOCKED "OK_LOCKED"
#define RESP_OK_WRITE_DONE "OK_WRITE_DONE"
#define RESP_OK_UNDO "OK_UNDO"
#define RESP_OK_CHECKPOINT "OK_CHECKPOINT"
#define RESP_OK_REVERT "OK_REVERT"

// List responses
#define RESP_OK_LIST "OK_LIST"
#define RESP_OK_LIST_END "OK_LIST_END"

// Location response
#define RESP_OK_LOC "OK_LOC"

// Read responses
#define RESP_OK_READ_START "OK_READ_START"
#define RESP_OK_READ_END "OK_READ_END"

// Stream responses
#define RESP_OK_STREAM "OK_STREAM"
#define RESP_OK_STREAM_END "OK_STREAM_END"

// Exec responses
#define RESP_OK_EXEC_START "OK_EXEC_START"
#define RESP_EXEC_OUT "EXEC_OUT"
#define RESP_OK_EXEC_END "OK_EXEC_END"

// Info responses
#define RESP_OK_INFO_START "OK_INFO_START"
#define RESP_INFO_LINE "INFO_LINE"
#define RESP_OK_INFO_END "OK_INFO_END"

// View -l responses
#define RESP_OK_VIEW_L "OK_VIEW_L"
#define RESP_OK_VIEW_END "OK_VIEW_END"

// Checkpoint list responses
#define RESP_OK_LIST_CHECKPOINT "OK_LIST_CHECKPOINT"
#define RESP_OK_LIST_CHECKPOINT_END "OK_LIST_CHECKPOINT_END"

// ============================================================================
// RESPONSE TYPES - ERROR
// ============================================================================

// Error format: ERR|error_code|human_readable_message\n
#define RESP_ERR "ERR"

// ============================================================================
// PERMISSION TYPES
// ============================================================================

#define PERM_READ "R"
#define PERM_WRITE "W"

// ============================================================================
// PROTOCOL MESSAGE STRUCTURE
// ============================================================================

typedef struct {
    char *fields[MAX_FIELDS];
    int field_count;
    char raw_message[MAX_MESSAGE_SIZE];
} ProtocolMessage;

// ============================================================================
// FUNCTION DECLARATIONS
// ============================================================================

/**
 * Parse a protocol message from a raw string
 * Returns 0 on success, -1 on error
 */
int protocol_parse_message(const char *raw, ProtocolMessage *msg);

/**
 * Build a protocol message from fields
 * Returns pointer to the message string (caller must free), or NULL on error
 */
char* protocol_build_message(const char **fields, int field_count);

/**
 * Build an error message
 * Returns pointer to the error message string (caller must free), or NULL on error
 */
char* protocol_build_error(int error_code, const char *error_msg);

/**
 * Build a simple OK response with optional message
 * Returns pointer to the response string (caller must free), or NULL on error
 */
char* protocol_build_ok(const char *message);

/**
 * Free a protocol message and its allocated fields
 */
void protocol_free_message(ProtocolMessage *msg);

/**
 * Get error message for error code
 */
const char* protocol_get_error_message(int error_code);

/**
 * Send a protocol message over a socket
 * Returns number of bytes sent, or -1 on error
 */
int protocol_send_message(int sockfd, const char *message);

/**
 * Receive a protocol message from a socket
 * Returns pointer to received message (caller must free), or NULL on error
 */
char* protocol_receive_message(int sockfd);

/**
 * Check if a message is an error message
 * Returns 1 if error, 0 otherwise
 */
int protocol_is_error(const ProtocolMessage *msg);

/**
 * Extract error code from error message
 * Returns error code, or -1 if not an error message
 */
int protocol_get_error_code(const ProtocolMessage *msg);

/**
 * Validate message format
 * Returns 1 if valid, 0 otherwise
 */
int protocol_validate_message(const char *message);

#endif // PROTOCOL_H
