/**
 * @file utils.h
 * @brief Shared utility library for the Distributed Document Collaboration System.
 *
 * This header is included by the Name Server, Storage Server, and Client components.
 * It provides a unified set of reusable building blocks covering:
 *   - Structured logging (multi-level, file + console)
 *   - Time formatting helpers
 *   - Safe string operations that prevent buffer overflows
 *   - POSIX file system helpers (stat wrappers, recursive mkdir, copy)
 *   - Path composition and normalization
 *   - Network address parsing and validation
 *   - Input validation (usernames, filenames, permissions)
 *   - Path flattening for translating logical "folder/file" names into
 *     single-level physical filenames stored on the Storage Server
 *   - Memory allocation wrappers with OOM logging
 *   - Text analysis: word, character, and sentence counting
 *   - Base64 encoding/decoding (used for binary payloads over the text protocol)
 */

#ifndef UTILS_H
#define UTILS_H

#include <time.h>

// ============================================================================
// UTILITY CONSTANTS
// ============================================================================

/** Maximum length of any filesystem path used in the system. */
#define MAX_PATH_LENGTH 4096
/** Maximum number of characters in a username (including null terminator). */
#define MAX_USERNAME_LENGTH 256
/** Maximum number of characters in a filename or logical path (including null). */
#define MAX_FILENAME_LENGTH 512
/** Maximum number of characters in a checkpoint tag name. */
#define MAX_TAG_LENGTH 128
/** Maximum number of characters in an IP address string (e.g., "255.255.255.255"). */
#define MAX_IP_LENGTH 64
/** Maximum number of characters in a port number string (e.g., "65535"). */
#define MAX_PORT_LENGTH 16

// ============================================================================
// LOGGING UTILITIES
// ============================================================================

/**
 * @brief Log severity levels, ordered from least to most severe.
 *
 * The active log level acts as a threshold: only messages at or above
 * the configured level are written to the log file / console.
 */
typedef enum {
    LOG_DEBUG,    /**< Verbose diagnostics, disabled in normal operation. */
    LOG_INFO,     /**< Routine operational events (connections, file ops). */
    LOG_WARNING,  /**< Non-fatal anomalies (unknown commands, minor failures). */
    LOG_ERROR     /**< Serious failures that may impair functionality. */
} LogLevel;

/**
 * @brief Open (or create) the log file and set the minimum log level.
 *
 * Must be called once at startup before any log_message() calls.
 *
 * @param log_file  Path to the log file. Pass NULL to disable file logging
 *                  (messages still appear on the console if enabled).
 * @param level     Minimum severity to record (e.g., LOG_INFO).
 * @return 0 on success, -1 if the log file could not be opened.
 */
int log_init(const char *log_file, LogLevel level);

/**
 * @brief Enable or disable mirroring of log output to stdout/stderr.
 *
 * Console logging is ON by default after log_init().
 * Disable it (enable = 0) in daemon-mode deployments that use only the file.
 *
 * @param enable  1 to mirror logs to console, 0 to suppress console output.
 */
void log_set_console(int enable);

/**
 * @brief Write a formatted log entry with timestamp, level tag, and component label.
 *
 * The output format is: [TIMESTAMP] [LEVEL] [COMPONENT] message
 *
 * @param level      Severity of this message.
 * @param component  Short label identifying the source (e.g., "NS", "SS", "Client").
 * @param format     printf-style format string followed by variadic arguments.
 */
void log_message(LogLevel level, const char *component, const char *format, ...);

/**
 * @brief Log a network request with full source/destination endpoint details.
 *
 * Used by the Name Server to record every inbound request with IP, port,
 * authenticated username, and the raw operation string for audit trails.
 *
 * @param component   Label of the logging component (e.g., "NS").
 * @param source_ip   IP address of the requesting peer.
 * @param source_port Port of the requesting peer.
 * @param dest_ip     IP address of the target (this server).
 * @param dest_port   Port of the target.
 * @param username    Authenticated username, or NULL if not yet known.
 * @param request     Human-readable description of the operation being logged.
 */
void log_request(const char *component, const char *source_ip, int source_port,
                 const char *dest_ip, int dest_port, const char *username,
                 const char *request);

/**
 * @brief Log a Storage Server file migration event at INFO level.
 *
 * Called by the Name Server when initiating, completing, or rolling back a
 * cross-server file transfer, recording the source and target endpoints.
 *
 * @param filename    Logical name of the file being migrated.
 * @param source_ip   IP of the originating Storage Server.
 * @param source_port Port of the originating Storage Server.
 * @param target_ip   IP of the destination Storage Server.
 * @param target_port Port of the destination Storage Server.
 * @param status      Short status string (e.g., "STARTED", "COMPLETED", "ROLLED_BACK").
 */
void log_migration_event(const char *filename, const char *source_ip, int source_port,
                         const char *target_ip, int target_port, const char *status);

/**
 * @brief Flush and close the log file.
 *
 * Should be called during graceful shutdown to ensure all buffered log
 * entries are persisted before the process exits.
 */
void log_cleanup(void);

// ============================================================================
// TIME UTILITIES
// ============================================================================

/**
 * @brief Return the current local time as a formatted string.
 *
 * @warning Returns a pointer to a static buffer; not thread-safe.
 *          Copy the result before calling again or from multiple threads.
 *
 * @return Pointer to a null-terminated string in "YYYY-MM-DD HH:MM:SS" format.
 */
char* get_timestamp(void);

/**
 * @brief Convert a time_t value to a human-readable string.
 *
 * @warning Returns a pointer to a static buffer; same caveats as get_timestamp().
 *
 * @param time  The epoch timestamp to format.
 * @return Pointer to a null-terminated "YYYY-MM-DD HH:MM:SS" string.
 */
char* format_time(time_t time);

/**
 * @brief Return the current Unix epoch time.
 *
 * Thin wrapper around time(NULL) used throughout the system for consistent
 * timestamp generation.
 *
 * @return Seconds since the Unix epoch.
 */
time_t get_current_time(void);

// ============================================================================
// STRING UTILITIES
// ============================================================================

/**
 * @brief Strip leading and trailing ASCII whitespace from a string in-place.
 *
 * Uses memmove() to shift the content left after removing leading spaces,
 * then overwrites the trailing whitespace with a null terminator.
 *
 * @param str  The string to trim; modified in place.
 */
void trim_string(char *str);

/**
 * @brief Test whether a string ends with a given suffix.
 *
 * @param str     The string to test.
 * @param suffix  The expected suffix.
 * @return 1 if str ends with suffix, 0 otherwise (including NULL inputs).
 */
int string_ends_with(const char *str, const char *suffix);

/**
 * @brief Test whether a string starts with a given prefix.
 *
 * @param str     The string to test.
 * @param prefix  The expected prefix.
 * @return 1 if str starts with prefix, 0 otherwise (including NULL inputs).
 */
int string_starts_with(const char *str, const char *prefix);

/**
 * @brief Copy src into dest, always ensuring null termination within dest_size bytes.
 *
 * Unlike strncpy(), this function guarantees that dest[dest_size-1] == '\0'.
 *
 * @param dest       Destination buffer.
 * @param src        Source string.
 * @param dest_size  Total capacity of dest (including null terminator).
 * @return 0 on success, -1 if any argument is invalid or dest_size is 0.
 */
int safe_strcpy(char *dest, const char *src, size_t dest_size);

/**
 * @brief Append src to dest without exceeding dest_size bytes total.
 *
 * @param dest       Destination buffer (must already be null-terminated).
 * @param src        String to append.
 * @param dest_size  Total capacity of dest.
 * @return 0 on success, -1 if arguments are invalid or dest is already full.
 */
int safe_strcat(char *dest, const char *src, size_t dest_size);

// ============================================================================
// FILE UTILITIES
// ============================================================================

/**
 * @brief Check whether a file (or directory) exists at the given path.
 *
 * Uses access(path, F_OK) internally.
 *
 * @param path  Path to test.
 * @return 1 if the path exists and is accessible, 0 otherwise.
 */
int file_exists(const char *path);

/**
 * @brief Return the size of a file in bytes using stat().
 *
 * @param path  Path to the file.
 * @return File size in bytes, or -1 on error (bad path, stat failure).
 */
long get_file_size(const char *path);

/**
 * @brief Return the last modification time of a file.
 *
 * @param path  Path to the file.
 * @return st_mtime as a time_t, or -1 on error.
 */
time_t get_file_mtime(const char *path);

/**
 * @brief Return the last access time of a file.
 *
 * @param path  Path to the file.
 * @return st_atime as a time_t, or -1 on error.
 */
time_t get_file_atime(const char *path);

/**
 * @brief Create all components of a directory path, similar to `mkdir -p`.
 *
 * Iterates through the path string, inserting temporary null terminators at
 * each '/' to create intermediate directories one level at a time.
 * Existing directories (EEXIST) are silently ignored.
 *
 * @param path  The directory path to create.
 * @return 0 on success, -1 on any mkdir failure other than EEXIST.
 */
int create_directory_recursive(const char *path);

/**
 * @brief Delete a file from the filesystem using unlink().
 *
 * @param path  Path to the file to delete.
 * @return 0 on success, -1 on error (sets errno).
 */
int delete_file(const char *path);

/**
 * @brief Copy the contents of one file to another in 8 KB chunks.
 *
 * Reads src in binary mode and writes to dest, truncating dest if it exists.
 * Both file handles are properly closed on success or error.
 *
 * @param src   Source file path.
 * @param dest  Destination file path.
 * @return 0 on success, -1 if either file cannot be opened or a write fails.
 */
int copy_file(const char *src, const char *dest);

// ============================================================================
// PATH UTILITIES
// ============================================================================

/**
 * @brief Concatenate base and component with a '/' separator into a new string.
 *
 * Automatically inserts a '/' between base and component unless base already
 * ends with one.
 *
 * @param base       Base directory path.
 * @param component  Relative path to append.
 * @return Newly heap-allocated path string; caller must free() it. NULL on error.
 */
char* path_join(const char *base, const char *component);

/**
 * @brief Extract the filename portion of a path (POSIX basename).
 *
 * @warning Returns a pointer to a static buffer; not thread-safe.
 *
 * @param path  The full path string.
 * @return Pointer to the basename portion (static buffer).
 */
char* path_basename(const char *path);

/**
 * @brief Extract the directory portion of a path (POSIX dirname).
 *
 * @warning Returns a pointer to a static buffer; not thread-safe.
 *
 * @param path  The full path string.
 * @return Pointer to the directory portion (static buffer).
 */
char* path_dirname(const char *path);

/**
 * @brief Remove trailing slashes from a path (simplified normalization).
 *
 * A full resolver for '..' and '.' components is NOT performed; this only
 * strips trailing '/' characters from the end.
 *
 * @param path  The path to normalize.
 * @return Newly heap-allocated normalized path; caller must free() it.
 */
char* path_normalize(const char *path);

/**
 * @brief Test whether a path is absolute (begins with '/').
 *
 * @param path  The path to test.
 * @return 1 if path[0] == '/', 0 otherwise or if path is NULL/empty.
 */
int path_is_absolute(const char *path);

// ============================================================================
// NETWORK UTILITIES
// ============================================================================

/**
 * @brief Split an "ip:port" address string into its components.
 *
 * Locates the first ':' in the input, terminates the IP portion there,
 * and converts the remainder to an integer port number via atoi().
 *
 * @param address  Input string in "ip:port" format.
 * @param ip       Output buffer for the IP address string.
 * @param port     Output pointer for the integer port.
 * @return 0 on success, -1 if no ':' found or arguments are NULL.
 */
int parse_address(const char *address, char *ip, int *port);

/**
 * @brief Format an IP address and port number as "ip:port".
 *
 * @warning Returns a pointer to a static buffer; not thread-safe.
 *
 * @param ip    IP address string.
 * @param port  Port number.
 * @return Pointer to the formatted string in a static buffer.
 */
char* format_address(const char *ip, int port);

/**
 * @brief Validate that a port number is within the legal TCP/UDP range (1-65535).
 *
 * @param port  The port number to check.
 * @return 1 if valid, 0 otherwise.
 */
int is_valid_port(int port);

/**
 * @brief Validate that a string represents a well-formed IPv4 address.
 *
 * Uses inet_pton(AF_INET, ...) for strict parsing.
 *
 * @param ip  The IP address string to validate.
 * @return 1 if the string is a valid IPv4 address, 0 otherwise.
 */
int is_valid_ip(const char *ip);

// ============================================================================
// VALIDATION UTILITIES
// ============================================================================

/**
 * @brief Validate that a username is non-empty, within length limits,
 *        and contains only alphanumeric characters or underscores.
 *
 * @param username  The username string to validate.
 * @return 1 if valid, 0 otherwise.
 */
int validate_username(const char *username);

/**
 * @brief Validate that a filename is non-empty, within length limits,
 *        and does not contain forbidden characters (<>:"|?*).
 *
 * Note: '/' is allowed because filenames in this system use '/' as a
 * logical folder separator (e.g., "docs/report.txt").
 *
 * @param filename  The filename (or logical path) to validate.
 * @return 1 if valid, 0 otherwise.
 */
int validate_filename(const char *filename);

/**
 * @brief Validate that a permission token is exactly "R" or "W".
 *
 * The protocol uses "R" for read-only access and "W" for write access
 * (which also implies read).
 *
 * @param permission  The permission string to validate.
 * @return 1 if valid, 0 otherwise.
 */
int validate_permission(const char *permission);

/**
 * @brief Convert a logical filename (which may contain '/') to a flat
 *        filesystem-safe name by replacing '/' with '_'.
 *
 * Storage Servers store all files in a single flat directory, so logical
 * paths like "docs/report.txt" must be mapped to "docs_report.txt" on disk.
 * This avoids the need to create subdirectories on the Storage Server.
 *
 * @param logical   The logical path (e.g., "folder/file.txt").
 * @param physical  Output buffer for the flattened physical name.
 * @param size      Size of the physical buffer.
 * @return 0 on success, -1 if arguments are invalid or the buffer is too small.
 */
int flatten_logical_path(const char *logical, char *physical, size_t size);

// ============================================================================
// MEMORY UTILITIES
// ============================================================================

/**
 * @brief malloc() with an OOM error message on failure.
 *
 * @param size  Number of bytes to allocate.
 * @return Pointer to allocated memory, or NULL if the allocation failed.
 */
void* safe_malloc(size_t size);

/**
 * @brief calloc() with an OOM error message on failure.
 *
 * Allocates count * size bytes, all initialized to zero.
 *
 * @param count  Number of elements.
 * @param size   Size of each element in bytes.
 * @return Pointer to zeroed allocated memory, or NULL on failure.
 */
void* safe_calloc(size_t count, size_t size);

/**
 * @brief realloc() with an OOM error message on failure.
 *
 * Note: if realloc() fails, the original pointer is NOT freed (standard behavior).
 *
 * @param ptr   Existing allocated pointer (may be NULL).
 * @param size  New desired size in bytes.
 * @return Pointer to reallocated memory, or NULL on failure.
 */
void* safe_realloc(void *ptr, size_t size);

/**
 * @brief free() that silently handles NULL pointers.
 *
 * Checks ptr != NULL before calling free(), preventing undefined behavior
 * on double-free attempts.
 *
 * @param ptr  Pointer to free (no-op if NULL).
 */
void safe_free(void *ptr);

// ============================================================================
// TEXT PROCESSING UTILITIES
// ============================================================================

/**
 * @brief Count the number of whitespace-delimited words in text.
 *
 * A "word" is any maximal sequence of non-whitespace characters.
 * Used by both the Name Server and Storage Server to populate metadata
 * word count fields shown by the VIEW -l and INFO commands.
 *
 * @param text  The text to analyze.
 * @return Number of words, or 0 if text is NULL or empty.
 */
int count_words(const char *text);

/**
 * @brief Count non-whitespace characters in text.
 *
 * Complements count_words() for character-count metadata.
 *
 * @param text  The text to analyze.
 * @return Number of non-whitespace characters, or 0 for NULL/empty input.
 */
int count_chars(const char *text);

/**
 * @brief Count the number of sentences in text.
 *
 * A sentence boundary is defined as any occurrence of '.', '!', or '?'.
 * Note: this is a simple delimiter count and does NOT handle edge cases
 * like abbreviations (e.g., "Dr. Smith") — the README documents this
 * behavior explicitly.
 *
 * @param text  The text to analyze.
 * @return Number of sentence delimiters encountered.
 */
int count_sentences(const char *text);

/**
 * @brief Extract the Nth sentence (0-indexed) from a block of text.
 *
 * Iterates through sentence delimiters ('.', '!', '?') and extracts the
 * substring corresponding to the requested sentence index.
 * The last "sentence" that lacks a trailing delimiter is also returned.
 *
 * Used by Storage Servers to locate the region of a file that a WRITE
 * operation targets before applying sentence-level locking.
 *
 * @param text            The full text to search.
 * @param sentence_index  Zero-based index of the sentence to extract.
 * @return Newly heap-allocated string containing the sentence (caller must free),
 *         or NULL if the index is out of range or text is NULL.
 */
char* extract_sentence(const char *text, int sentence_index);

/**
 * @brief Replace the word at word_index within sentence with new_word.
 *
 * @note This is a simplified stub. The current implementation returns a copy
 *       of the original sentence without actually performing a word replacement.
 *       Full implementation is deferred.
 *
 * @param sentence    The original sentence text.
 * @param word_index  Zero-based position of the word to replace.
 * @param new_word    The replacement word.
 * @return Newly heap-allocated result string; caller must free().
 */
char* replace_word_in_sentence(const char *sentence, int word_index, const char *new_word);

/**
 * @brief Check if a character is one of the recognized sentence-end delimiters.
 *
 * The system recognizes '.', '!', and '?' as sentence delimiters. This helper
 * is used by count_sentences() and extract_sentence().
 *
 * @param c  The character to test.
 * @return 1 if c is a sentence delimiter, 0 otherwise.
 */
int is_sentence_delimiter(char c);

/**
 * @brief Parse a decimal integer string into a long long with strict validation.
 *
 * Uses strtoll() and rejects strings with trailing non-numeric characters or
 * overflow conditions.
 *
 * @param text       The string to parse.
 * @param out_value  Output pointer for the parsed value.
 * @return 0 on success, -1 if the string is invalid or out of range.
 */
int parse_long_long(const char *text, long long *out_value);

/**
 * @brief Parse a decimal floating-point string into a double with strict validation.
 *
 * Uses strtod() and rejects strings with trailing non-numeric characters or
 * overflow/underflow conditions.
 *
 * @param text       The string to parse.
 * @param out_value  Output pointer for the parsed value.
 * @return 0 on success, -1 if the string is invalid.
 */
int parse_double(const char *text, double *out_value);

/**
 * @brief Encode binary data as a Base64 string.
 *
 * The output is RFC 4648 Base64 with '=' padding. Used when binary content
 * (e.g., file data) must be transported over the text-based protocol without
 * breaking the '|' delimiter parsing.
 *
 * @param input         Pointer to the binary input buffer.
 * @param input_length  Length of the input buffer in bytes.
 * @param output        Output pointer; receives a newly heap-allocated null-terminated
 *                      Base64 string. Caller must free() it on success.
 * @return 0 on success, -1 on invalid arguments or allocation failure.
 */
int base64_encode(const unsigned char *input, size_t input_length, char **output);

/**
 * @brief Decode a Base64 string back into binary data.
 *
 * Validates that the input length is a multiple of 4, then decodes each
 * 4-character group into 3 bytes. Padding characters ('=') are handled.
 *
 * @param input          The Base64-encoded input string.
 * @param output         Output pointer; receives a newly heap-allocated byte buffer.
 *                       Caller must free() it on success.
 * @param output_length  Set to the number of decoded bytes on success.
 * @return 0 on success, -1 on invalid input or allocation failure.
 */
int base64_decode(const char *input, unsigned char **output, size_t *output_length);

#endif // UTILS_H
