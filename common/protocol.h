/**
 * @file protocol.h
 * @brief Wire protocol definition for the Distributed Document Collaboration System.
 *
 * All three components — Client, Name Server (NS), and Storage Server (SS) —
 * communicate over TCP sockets using a simple text-based protocol:
 *
 *   FIELD_0|FIELD_1|...|FIELD_N\n
 *
 * Messages are delimited by the '|' character and terminated by a newline '\n'.
 * FIELD_0 is always the "message type" constant (e.g., "CREATE", "OK", "ERR").
 *
 * MESSAGE FLOW OVERVIEW:
 *
 *   1. Client → NS (command or handshake)
 *   2. NS routes:
 *      a. NS handles directly (access control, metadata, user listing).
 *      b. NS sends a sub-command to the appropriate SS (create/delete/stats).
 *      c. NS sends the SS location back to the Client (REQ_LOC → OK_LOC),
 *         and the Client connects directly to the SS for data-intensive ops
 *         (read, write, stream, undo).
 *
 * ERROR HANDLING:
 *   Error responses always take the form:  ERR|<error_code>|<human_message>\n
 *   Use protocol_build_error() / protocol_is_error() for uniform handling.
 *
 * THREAD SAFETY:
 *   The protocol functions themselves are stateless and thus thread-safe.
 *   Callers are responsible for protecting the socket file descriptor.
 */

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>

// ============================================================================
// PROTOCOL CONSTANTS
// ============================================================================

/** Field separator character used in all protocol messages. */
#define PROTOCOL_DELIMITER "|"
/** Message terminator: every protocol message must end with '\n'. */
#define PROTOCOL_TERMINATOR "\n"
/** Maximum total size of a single protocol message in bytes (64 KB). */
#define MAX_MESSAGE_SIZE 65536
/** Maximum size of a single field within a message (4 KB). */
#define MAX_FIELD_SIZE 4096
/** Maximum number of '|'-separated fields in a single message. */
#define MAX_FIELDS 32

// ============================================================================
// ERROR CODES
// ============================================================================

/*
 * Error codes follow an HTTP-inspired convention:
 *   1xx → Client/user identification errors
 *   4xx → Request or resource errors (bad input, missing file, permission)
 *   5xx → Server-side internal or storage errors
 */

/** Username is malformed or is already in use by another connected session. */
#define ERR_USERNAME_INVALID 101
/** The referenced user does not exist in the system. */
#define ERR_USER_NOT_FOUND 102

/** The request is syntactically invalid or missing required fields. */
#define ERR_INVALID_REQUEST 400
/** The authenticated user lacks the required read or write permission. */
#define ERR_PERMISSION_DENIED 403
/** The requested file does not exist on the Name Server or Storage Server. */
#define ERR_FILE_NOT_FOUND 404
/** A file with the requested name already exists (CREATE conflict). */
#define ERR_FILE_EXISTS 409
/** The target sentence is locked by another active WRITE session. */
#define ERR_SENTENCE_LOCKED 423

/** A generic unrecoverable server-side error occurred. */
#define ERR_INTERNAL_ERROR 500
/** A disk read or write operation failed on the Storage Server. */
#define ERR_DISK_ERROR 501
/** No undo snapshot is available for the requested file. */
#define ERR_NO_UNDO_HISTORY 502
/** The Storage Server could not delete the file from disk. */
#define ERR_DELETE_FAILED 503
/** The EXEC command failed to run the shell script on the Name Server. */
#define ERR_EXEC_FAILED 504

// ============================================================================
// MESSAGE TYPES - HANDSHAKE & INITIALIZATION
// ============================================================================

/*
 * Handshake messages establish identities after a new TCP connection.
 * Clients send HELLO_CLIENT; Storage Servers send HELLO_SS.
 */

/** Client → NS: Identify as a client user.
 *  Format: HELLO_CLIENT|username\n */
#define MSG_HELLO_CLIENT "HELLO_CLIENT"

/** SS → NS: Announce the Storage Server's two listen endpoints (NS-facing and client-facing).
 *  Format: HELLO_SS|ns_ip|ns_port|client_ip|client_port\n */
#define MSG_HELLO_SS "HELLO_SS"

/** SS → NS: Declare one file that this Storage Server already stores (replayed on startup).
 *  Format: SS_HAS_FILE|filename|owner|read_acl_csv|write_acl_csv\n */
#define MSG_SS_HAS_FILE "SS_HAS_FILE"

/** SS → NS: Signal that all SS_HAS_FILE messages for this boot have been sent.
 *  Format: SS_FILES_DONE\n */
#define MSG_SS_FILES_DONE "SS_FILES_DONE"

// ============================================================================
// MESSAGE TYPES - NS-HANDLED COMMANDS (Client → NS)
// ============================================================================

/*
 * These commands are fully processed by the Name Server without forwarding
 * data to a Storage Server. The NS responds directly to the client.
 */

/** Client → NS: List all currently connected users.
 *  Format: LIST_USERS\n */
#define MSG_LIST_USERS "LIST_USERS"

/** Client → NS: Grant read or write access to another user for a file.
 *  Format: ADDACCESS|filename|username_to_add|permission\n
 *  permission is "R" (read) or "W" (write; implies read). */
#define MSG_ADDACCESS "ADDACCESS"

/** Client → NS: Revoke all access (read and write) for a user from a file.
 *  Format: REMACCESS|filename|username_to_remove\n */
#define MSG_REMACCESS "REMACCESS"

/** Client → NS: Request a cross-server or same-server file copy.
 *  Format: COPY|source_filename|dest_filename\n */
#define MSG_COPY "COPY"

/* --- Access Request Mailbox --- */

/** Client → NS: Submit an access request for a file (owner must approve).
 *  Format: REQUESTACCESS|filename|permission\n */
#define MSG_REQUEST_ACCESS "REQUESTACCESS"

/** Client → NS: List pending access requests on files owned by this user.
 *  Format: LISTREQUESTS\n */
#define MSG_LIST_REQUESTS "LISTREQUESTS"

/** Client → NS: Approve a pending access request by its ID.
 *  Format: APPROVEACCESS|request_id\n */
#define MSG_APPROVE_ACCESS "APPROVEACCESS"

/** Client → NS: Reject a pending access request by its ID.
 *  Format: REJECTACCESS|request_id\n */
#define MSG_REJECT_ACCESS "REJECTACCESS"

// ============================================================================
// MESSAGE TYPES - COORDINATED COMMANDS (C-NS-SS)
// ============================================================================

/*
 * For file creation, deletion, metadata, and EXEC the NS acts as a coordinator:
 * it validates the request, forwards a sub-command to the responsible SS,
 * waits for the SS response, and then replies to the client.
 */

/* CREATE */
/** Client → NS: Create a new empty file.
 *  Format: CREATE|filename\n */
#define MSG_CREATE "CREATE"

/** NS → SS: Instruct the SS to create the file on disk.
 *  Format: CREATE_FILE|filename|owner_username\n */
#define MSG_CREATE_FILE "CREATE_FILE"

/* DELETE */
/** Client → NS: Delete a file (only the owner may do this).
 *  Format: DELETE|filename\n */
#define MSG_DELETE "DELETE"

/** NS → SS: Instruct the SS to remove the file from disk.
 *  Format: DELETE_FILE|filename\n */
#define MSG_DELETE_FILE "DELETE_FILE"

/* VIEW and INFO */
/** Client → NS: Request metadata for a single file.
 *  Format: INFO|filename\n */
#define MSG_INFO "INFO"

/** Client → NS: List accessible files with optional flags.
 *  Format: VIEW|flags\n   (flags can be empty, "-a", "-l", "-al") */
#define MSG_VIEW "VIEW"

/** NS → SS: Request file stats (size, word count, char count, timestamps) for INFO.
 *  Format: GET_STATS|filename\n */
#define MSG_GET_STATS "GET_STATS"

/* EXEC */
/** Client → NS: Execute a file's content as shell commands on the NS host.
 *  Format: EXEC|filename\n */
#define MSG_EXEC "EXEC"

/** NS → SS: Retrieve the raw file content for execution.
 *  Format: GET_CONTENT|filename\n */
#define MSG_GET_CONTENT "GET_CONTENT"

/* ACL sync from NS to SS */
/** NS → SS: Add a user to the file's ACL on the SS side.
 *  Format: SS_ADDACCESS|filename|username|permission_token\n */
#define MSG_SS_ADDACCESS "SS_ADDACCESS"

/** NS → SS: Remove a user from the file's ACL on the SS side.
 *  Format: SS_REMACCESS|filename|username\n */
#define MSG_SS_REMACCESS "SS_REMACCESS"

/** NS → SS: Rename (move) a file's logical path and physical filename.
 *  Format: SS_RENAME|old_logical|new_logical|old_flat|new_flat\n */
#define MSG_SS_RENAME "SS_RENAME"

/** NS → SS: Initiate a cross-server copy; destination SS fetches data from source SS.
 *  Format: SS_COPY|dest_file|source_ip|source_port|source_file|username\n */
#define MSG_SS_COPY "SS_COPY"

// ============================================================================
// MESSAGE TYPES - LOCATION REQUEST (C → NS)
// ============================================================================

/**
 * For data-intensive operations (READ, WRITE, STREAM, UNDO), the client
 * asks the NS for the Storage Server address and then connects directly,
 * avoiding NS as a data relay and keeping it free for coordination.
 *
 * Client → NS: REQ_LOC|COMMAND_NAME|filename\n
 * NS → Client: OK_LOC|ss_ip|ss_port|resolved_filename\n
 */
#define MSG_REQ_LOC "REQ_LOC"

// ============================================================================
// MESSAGE TYPES - DIRECT C-SS COMMUNICATION
// ============================================================================

/*
 * After a successful REQ_LOC exchange, the client opens a fresh TCP connection
 * to the Storage Server and uses these message types directly.
 */

/** Client → SS: Fetch the complete file content.
 *  Format: REQ_READ|username|filename\n */
#define MSG_REQ_READ "REQ_READ"

/** Client → SS: Stream the file word-by-word with 100ms inter-word delay.
 *  Format: REQ_STREAM|username|filename\n */
#define MSG_REQ_STREAM "REQ_STREAM"

/** Client → SS: Revert the file to its previous (pre-write) snapshot.
 *  Format: REQ_UNDO|username|filename\n */
#define MSG_REQ_UNDO "REQ_UNDO"

/* WRITE protocol (three-phase: lock → edits → commit) */
/** Client → SS: Acquire a sentence-level write lock.
 *  Format: REQ_WRITE_LOCK|username|filename|sentence_index\n */
#define MSG_REQ_WRITE_LOCK "REQ_WRITE_LOCK"

/** Client → SS: Submit one word replacement within the locked sentence.
 *  Format: WRITE_DATA|word_index|content_string\n */
#define MSG_WRITE_DATA "WRITE_DATA"

/** Client → SS: Commit and release the write lock (end of write session).
 *  Format: ETIRW\n   (WRITE spelled backwards — the end-of-write sentinel) */
#define MSG_ETIRW "ETIRW"

// ============================================================================
// MESSAGE TYPES - BONUS FUNCTIONALITY
// ============================================================================

/* --- Hierarchical Folders --- */

/** Client → NS: Create a new virtual folder in the namespace.
 *  Format: CREATEFOLDER|path/foldername\n */
#define MSG_CREATEFOLDER "CREATEFOLDER"

/** Client → NS: Move a file to a different folder in the namespace.
 *  Format: MOVE|source_path/filename|dest_path/foldername\n */
#define MSG_MOVE "MOVE"

/** Client → NS: List the contents of a virtual folder.
 *  Format: VIEWFOLDER|path/foldername\n */
#define MSG_VIEWFOLDER "VIEWFOLDER"

/* --- Checkpoints (named snapshots) --- */

/** Client → SS: Save a named snapshot of the current file state.
 *  Format: REQ_CHECKPOINT|username|filename|tag_name\n */
#define MSG_REQ_CHECKPOINT "REQ_CHECKPOINT"

/** Client → SS: View the content of a named checkpoint without reverting.
 *  Format: REQ_VIEWCHECKPOINT|username|filename|tag_name\n */
#define MSG_REQ_VIEWCHECKPOINT "REQ_VIEWCHECKPOINT"

/** Client → SS: Revert the file to a named checkpoint snapshot.
 *  Format: REQ_REVERT|username|filename|tag_name\n */
#define MSG_REQ_REVERT "REQ_REVERT"

/** Client → SS: List all checkpoint tags for a file.
 *  Format: REQ_LIST_CHECKPOINTS|username|filename\n */
#define MSG_REQ_LIST_CHECKPOINTS "REQ_LIST_CHECKPOINTS"

/* --- Fault Tolerance & Replication --- */

/** NS → SS_Replica: Instruct the replica to fetch a file from the primary SS.
 *  Format: REPLICATE_FILE|filename|primary_ss_ip|primary_ss_port\n */
#define MSG_REPLICATE_FILE "REPLICATE_FILE"

/** SS_Primary → NS: Notify the NS that a write has been committed so replicas can sync.
 *  Format: WRITE_COMPLETE|filename|size|words|chars\n */
#define MSG_WRITE_COMPLETE "WRITE_COMPLETE"

/** NS → SS_Replica: Trigger an incremental file sync from the primary.
 *  Format: SYNC_FILE|filename|primary_ss_ip|primary_ss_port\n */
#define MSG_SYNC_FILE "SYNC_FILE"

/** NS → SS: Heartbeat probe to check liveness.
 *  Format: PING\n */
#define MSG_PING "PING"

/** SS → NS: Heartbeat reply.
 *  Format: PONG\n */
#define MSG_PONG "PONG"

/* --- Load Balancing / Migration --- */

/** NS → SS_Source: Freeze the file for migration; no more writes accepted.
 *  Format: PREP_MIGRATION|filename\n */
#define MSG_PREP_MIGRATION "PREP_MIGRATION"

/** NS → SS_Target: Import a file from the source SS, including all metadata.
 *  Format: IMPORT_FILE|filename|source_ip|source_port|owner|read_acl|write_acl|
 *           created|modified|last_access|size|words|chars\n */
#define MSG_IMPORT_FILE "IMPORT_FILE"

/** NS → SS_Source: Either commit (delete source after successful import) or
 *  rollback (unfreeze the file if the target import failed).
 *  Format: MIGRATION_CLEANUP|filename|action(COMMIT|ROLLBACK)\n */
#define MSG_MIGRATION_CLEANUP "MIGRATION_CLEANUP"

// ============================================================================
// RESPONSE TYPES - SUCCESS
// ============================================================================

/** Generic success acknowledgement (e.g., after PING → PONG-equivalent). */
#define RESP_OK "OK"

/** Specific success tokens (FIELD_0 of a response message). */
#define RESP_OK_CREATE         "OK_CREATE"          /**< File created. */
#define RESP_OK_DELETE         "OK_DELETE"          /**< File deleted. */
#define RESP_OK_ACCESS_CHANGED "OK_ACCESS_CHANGED"  /**< ACL updated. */
#define RESP_OK_STATS          "OK_STATS"           /**< File stats follow. */
#define RESP_OK_CONTENT        "OK_CONTENT"         /**< File content follows. */
#define RESP_OK_LOCKED         "OK_LOCKED"          /**< Write lock granted. */
#define RESP_OK_WRITE_DONE     "OK_WRITE_DONE"      /**< Write session committed. */
#define RESP_OK_UNDO           "OK_UNDO"            /**< Undo applied. */
#define RESP_OK_CHECKPOINT     "OK_CHECKPOINT"      /**< Checkpoint saved. */
#define RESP_OK_REVERT         "OK_REVERT"          /**< Reverted to checkpoint. */

/* Migration response tokens */
#define RESP_OK_MIGRATION_READY   "OK_MIGRATION_READY"   /**< Source frozen, ready. */
#define RESP_OK_IMPORT_DONE       "OK_IMPORT_DONE"       /**< Target import complete. */
#define RESP_OK_MIGRATION_CLEANED "OK_MIGRATION_CLEANED" /**< Source committed/rolled back. */

/* Multi-message streaming responses (each entry on its own line until _END) */
#define RESP_OK_LIST     "OK_LIST"     /**< One item in a LIST_USERS response stream. */
#define RESP_OK_LIST_END "OK_LIST_END" /**< Terminates a LIST_USERS response stream. */

/** NS → Client: Storage Server location for direct client-SS connection.
 *  Format: OK_LOC|ss_ip|ss_port|resolved_filename\n */
#define RESP_OK_LOC "OK_LOC"

/** SS → Client: Start-of-read-data sentinel. File content lines follow. */
#define RESP_OK_READ_START "OK_READ_START"
/** SS → Client: End-of-read-data sentinel. */
#define RESP_OK_READ_END "OK_READ_END"

/** SS → Client: One word in a STREAM response. */
#define RESP_OK_STREAM     "OK_STREAM"
/** SS → Client: End-of-stream sentinel. */
#define RESP_OK_STREAM_END "OK_STREAM_END"

/** NS → Client: One entry in a VIEWFOLDER response. */
#define RESP_OK_VIEWFOLDER     "OK_VIEWFOLDER"
/** NS → Client: End-of-folder-listing sentinel. */
#define RESP_OK_VIEWFOLDER_END "OK_VIEWFOLDER_END"

/** NS → Client: Start-of-EXEC-output sentinel. */
#define RESP_OK_EXEC_START "OK_EXEC_START"
/** NS → Client: One line of shell output from an EXEC command. */
#define RESP_EXEC_OUT      "EXEC_OUT"
/** NS → Client: End-of-EXEC-output sentinel. */
#define RESP_OK_EXEC_END   "OK_EXEC_END"

/** NS → Client: Start-of-INFO block for a single file. */
#define RESP_OK_INFO_START "OK_INFO_START"
/** NS → Client: One key-value metadata line within an INFO block. */
#define RESP_INFO_LINE     "INFO_LINE"
/** NS → Client: End-of-INFO block sentinel. */
#define RESP_OK_INFO_END   "OK_INFO_END"

/** NS → Client: One entry in a VIEW -l (detailed) listing. */
#define RESP_OK_VIEW_L   "OK_VIEW_L"
/** NS → Client: End-of-VIEW listing sentinel. */
#define RESP_OK_VIEW_END "OK_VIEW_END"

/** NS → Client: One pending access request entry. */
#define RESP_OK_REQUEST_LIST     "OK_REQUEST_LIST"
/** NS → Client: End of access request listing. */
#define RESP_OK_REQUEST_LIST_END "OK_REQUEST_LIST_END"

/** SS → Client: One checkpoint tag entry. */
#define RESP_OK_LIST_CHECKPOINT     "OK_LIST_CHECKPOINT"
/** SS → Client: End of checkpoint list. */
#define RESP_OK_LIST_CHECKPOINT_END "OK_LIST_CHECKPOINT_END"

// ============================================================================
// RESPONSE TYPES - ERROR
// ============================================================================

/**
 * All error responses use this prefix as FIELD_0.
 * Full format: ERR|<error_code>|<human_readable_message>\n
 * Use protocol_build_error() to construct and protocol_is_error() to detect.
 */
#define RESP_ERR "ERR"

// ============================================================================
// PERMISSION TYPES
// ============================================================================

/** Read-only permission token sent in ADDACCESS and REQUESTACCESS messages. */
#define PERM_READ "R"
/** Write (and implicit read) permission token. */
#define PERM_WRITE "W"

// ============================================================================
// PROTOCOL MESSAGE STRUCTURE
// ============================================================================

/**
 * @brief Parsed representation of a single protocol message.
 *
 * After calling protocol_parse_message(), the raw text is split on '|'
 * and each fragment is stored as a separately heap-allocated string in
 * the fields array.
 *
 * Memory management:
 *   - Each non-NULL fields[i] must be freed individually.
 *   - Call protocol_free_message() when done with the parsed message.
 */
typedef struct {
    char *fields[MAX_FIELDS]; /**< Parsed field pointers (heap-allocated). */
    int field_count;          /**< Number of valid entries in fields[]. */
    char raw_message[MAX_MESSAGE_SIZE]; /**< Original received bytes (may be modified). */
} ProtocolMessage;

// ============================================================================
// FUNCTION DECLARATIONS
// ============================================================================

/**
 * @brief Parse a raw '|'-delimited, newline-terminated string into a ProtocolMessage.
 *
 * Strips the trailing '\n', splits on '|', and fills msg->fields[] with
 * heap-allocated copies of each field. Always call protocol_free_message()
 * after use.
 *
 * @param raw  The raw null-terminated string received from the socket.
 * @param msg  Output structure to populate.
 * @return 0 on success, -1 on allocation failure or NULL arguments.
 */
int protocol_parse_message(const char *raw, ProtocolMessage *msg);

/**
 * @brief Construct a '|'-delimited, '\n'-terminated protocol message string.
 *
 * Concatenates the provided fields with '|' separators and appends '\n'.
 *
 * @param fields       Array of field strings.
 * @param field_count  Number of fields.
 * @return Newly heap-allocated message string; caller must free(). NULL on error.
 */
char* protocol_build_message(const char **fields, int field_count);

/**
 * @brief Construct a standard error response message.
 *
 * Format: ERR|error_code|error_msg\n
 * If error_msg is NULL, the built-in human-readable message for error_code is used.
 *
 * @param error_code  One of the ERR_* constants defined above.
 * @param error_msg   Optional override for the human-readable message.
 * @return Heap-allocated error message string; caller must free(). NULL on error.
 */
char* protocol_build_error(int error_code, const char *error_msg);

/**
 * @brief Construct a simple OK response with an optional detail message.
 *
 * If message is non-NULL, the format is: OK|message\n
 * Otherwise the format is: OK\n
 *
 * @param message  Optional detail string (may be NULL).
 * @return Heap-allocated OK response string; caller must free(). NULL on error.
 */
char* protocol_build_ok(const char *message);

/**
 * @brief Free all heap-allocated field strings within a ProtocolMessage.
 *
 * Does NOT free the ProtocolMessage struct itself (which may be stack-allocated).
 * Resets field_count to 0.
 *
 * @param msg  The message to free fields of.
 */
void protocol_free_message(ProtocolMessage *msg);

/**
 * @brief Look up the human-readable message for a given error code.
 *
 * @param error_code  One of the ERR_* constants.
 * @return Pointer to a static error description string; never NULL.
 */
const char* protocol_get_error_message(int error_code);

/**
 * @brief Send a complete protocol message over a TCP socket.
 *
 * Loops around send() to handle partial writes and EINTR interruptions.
 *
 * @param sockfd   Connected socket file descriptor.
 * @param message  Null-terminated message string (must end with '\n').
 * @return Total bytes sent (>= 0) on success, -1 on a socket error.
 */
int protocol_send_message(int sockfd, const char *message);

/**
 * @brief Receive one protocol message from a TCP socket (up to '\n').
 *
 * Reads byte-by-byte until a '\n' terminator is found, the connection closes,
 * or the buffer limit (MAX_MESSAGE_SIZE) is reached.
 *
 * @param sockfd  Connected socket file descriptor.
 * @return Heap-allocated null-terminated message string (caller must free()),
 *         or NULL if the connection was closed, an error occurred, or the
 *         message exceeded MAX_MESSAGE_SIZE.
 */
char* protocol_receive_message(int sockfd);

/**
 * @brief Check whether a parsed message is an error response.
 *
 * Tests whether fields[0] equals RESP_ERR ("ERR").
 *
 * @param msg  A parsed ProtocolMessage.
 * @return 1 if msg is an error response, 0 otherwise.
 */
int protocol_is_error(const ProtocolMessage *msg);

/**
 * @brief Extract the numeric error code from an error response message.
 *
 * @param msg  A parsed ProtocolMessage that protocol_is_error() returns true for.
 * @return The integer error code from fields[1], or -1 if msg is not an error
 *         or the code field is missing.
 */
int protocol_get_error_code(const ProtocolMessage *msg);

/**
 * @brief Perform a basic structural validation of a raw protocol message string.
 *
 * Checks that:
 *   - The string is non-NULL and non-empty.
 *   - The last character is '\n' (the required terminator).
 *   - The total length does not exceed MAX_MESSAGE_SIZE.
 *
 * @param message  The raw message string to validate.
 * @return 1 if the message is structurally valid, 0 otherwise.
 */
int protocol_validate_message(const char *message);

#endif // PROTOCOL_H
