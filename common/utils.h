#ifndef UTILS_H
#define UTILS_H

#include <time.h>

// ============================================================================
// UTILITY CONSTANTS
// ============================================================================

#define MAX_PATH_LENGTH 4096
#define MAX_USERNAME_LENGTH 256
#define MAX_FILENAME_LENGTH 512
#define MAX_TAG_LENGTH 128
#define MAX_IP_LENGTH 64
#define MAX_PORT_LENGTH 16

// ============================================================================
// LOGGING UTILITIES
// ============================================================================

typedef enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARNING,
    LOG_ERROR
} LogLevel;

/**
 * Initialize logging system
 * log_file: Path to log file (NULL for stderr only)
 * level: Minimum log level to record
 */
int log_init(const char *log_file, LogLevel level);

/**
 * Enable or disable console logging (stdout/stderr).
 * enable = 1 to mirror logs to console (default), 0 to disable.
 */
void log_set_console(int enable);

/**
 * Log a message with timestamp and metadata
 */
void log_message(LogLevel level, const char *component, const char *format, ...);

/**
 * Log a request/response with full details
 */
void log_request(const char *component, const char *source_ip, int source_port, 
                 const char *dest_ip, int dest_port, const char *username,
                 const char *request);

/**
 * Close logging system
 */
void log_cleanup(void);

// ============================================================================
// TIME UTILITIES
// ============================================================================

/**
 * Get current timestamp as string
 * Returns pointer to static buffer
 */
char* get_timestamp(void);

/**
 * Format time_t as human-readable string
 * Returns pointer to static buffer
 */
char* format_time(time_t time);

/**
 * Get current time in seconds since epoch
 */
time_t get_current_time(void);

// ============================================================================
// STRING UTILITIES
// ============================================================================

/**
 * Trim whitespace from both ends of string
 * Modifies string in place
 */
void trim_string(char *str);

/**
 * Check if string ends with suffix
 * Returns 1 if true, 0 otherwise
 */
int string_ends_with(const char *str, const char *suffix);

/**
 * Check if string starts with prefix
 * Returns 1 if true, 0 otherwise
 */
int string_starts_with(const char *str, const char *prefix);

/**
 * Safe string copy with size limit
 * Returns 0 on success, -1 on error
 */
int safe_strcpy(char *dest, const char *src, size_t dest_size);

/**
 * Safe string concatenation with size limit
 * Returns 0 on success, -1 on error
 */
int safe_strcat(char *dest, const char *src, size_t dest_size);

// ============================================================================
// FILE UTILITIES
// ============================================================================

/**
 * Check if file exists
 * Returns 1 if exists, 0 otherwise
 */
int file_exists(const char *path);

/**
 * Get file size in bytes
 * Returns file size, or -1 on error
 */
long get_file_size(const char *path);

/**
 * Get last modification time of file
 * Returns time_t, or -1 on error
 */
time_t get_file_mtime(const char *path);

/**
 * Get last access time of file
 * Returns time_t, or -1 on error
 */
time_t get_file_atime(const char *path);

/**
 * Create directory recursively (like mkdir -p)
 * Returns 0 on success, -1 on error
 */
int create_directory_recursive(const char *path);

/**
 * Delete file
 * Returns 0 on success, -1 on error
 */
int delete_file(const char *path);

/**
 * Copy file
 * Returns 0 on success, -1 on error
 */
int copy_file(const char *src, const char *dest);

// ============================================================================
// PATH UTILITIES
// ============================================================================

/**
 * Join path components
 * Returns pointer to newly allocated string (caller must free)
 */
char* path_join(const char *base, const char *component);

/**
 * Get basename from path
 * Returns pointer to static buffer
 */
char* path_basename(const char *path);

/**
 * Get directory name from path
 * Returns pointer to static buffer
 */
char* path_dirname(const char *path);

/**
 * Normalize path (resolve . and .., remove trailing slashes)
 * Returns pointer to newly allocated string (caller must free)
 */
char* path_normalize(const char *path);

/**
 * Check if path is absolute
 * Returns 1 if absolute, 0 otherwise
 */
int path_is_absolute(const char *path);

// ============================================================================
// NETWORK UTILITIES
// ============================================================================

/**
 * Parse IP and port from string "ip:port"
 * Returns 0 on success, -1 on error
 */
int parse_address(const char *address, char *ip, int *port);

/**
 * Format IP and port as "ip:port"
 * Returns pointer to static buffer
 */
char* format_address(const char *ip, int port);

/**
 * Check if port is valid
 * Returns 1 if valid, 0 otherwise
 */
int is_valid_port(int port);

/**
 * Check if IP address is valid
 * Returns 1 if valid, 0 otherwise
 */
int is_valid_ip(const char *ip);

// ============================================================================
// VALIDATION UTILITIES
// ============================================================================

/**
 * Validate username
 * Returns 1 if valid, 0 otherwise
 */
int validate_username(const char *username);

/**
 * Validate filename
 * Returns 1 if valid, 0 otherwise
 */
int validate_filename(const char *filename);

/**
 * Validate permission string ("R" or "W")
 * Returns 1 if valid, 0 otherwise
 */
int validate_permission(const char *permission);

// ============================================================================
// MEMORY UTILITIES
// ============================================================================

/**
 * Safe malloc with error checking
 * Returns pointer to allocated memory, or NULL on error
 */
void* safe_malloc(size_t size);

/**
 * Safe calloc with error checking
 * Returns pointer to allocated memory, or NULL on error
 */
void* safe_calloc(size_t count, size_t size);

/**
 * Safe realloc with error checking
 * Returns pointer to reallocated memory, or NULL on error
 */
void* safe_realloc(void *ptr, size_t size);

/**
 * Safe free (checks for NULL before freeing)
 */
void safe_free(void *ptr);

// ============================================================================
// TEXT PROCESSING UTILITIES
// ============================================================================

/**
 * Count words in text
 * Returns word count
 */
int count_words(const char *text);

/**
 * Count characters in text (excluding whitespace)
 * Returns character count
 */
int count_chars(const char *text);

/**
 * Count sentences in text
 * A sentence ends with '.', '!', or '?'
 * Returns sentence count
 */
int count_sentences(const char *text);

/**
 * Extract sentence at given index
 * Returns pointer to newly allocated string (caller must free), or NULL on error
 */
char* extract_sentence(const char *text, int sentence_index);

/**
 * Replace word at given position in sentence
 * Returns pointer to newly allocated string (caller must free), or NULL on error
 */
char* replace_word_in_sentence(const char *sentence, int word_index, const char *new_word);

/**
 * Check if character is sentence delimiter
 * Returns 1 if delimiter, 0 otherwise
 */
int is_sentence_delimiter(char c);

#endif // UTILS_H
