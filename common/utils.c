#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <libgen.h>
#include <arpa/inet.h>
#include <stdint.h>

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

static FILE *log_file_ptr = NULL;
static LogLevel current_log_level = LOG_INFO;
static int console_logging_enabled = 1;

// ============================================================================
// LOGGING UTILITIES
// ============================================================================

int log_init(const char *log_file, LogLevel level) {
    current_log_level = level;
    
    if (log_file) {
        log_file_ptr = fopen(log_file, "a");
        if (!log_file_ptr) {
            fprintf(stderr, "Failed to open log file: %s\n", log_file);
            return -1;
        }
    }
    
    return 0;
}

void log_set_console(int enable) {
    console_logging_enabled = enable ? 1 : 0;
}

void log_message(LogLevel level, const char *component, const char *format, ...) {
    if (level < current_log_level) {
        return;
    }
    
    const char *level_str[] = {"DEBUG", "INFO", "WARNING", "ERROR"};
    char *timestamp = get_timestamp();

    char buffer[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (console_logging_enabled) {
        fprintf(stdout, "[%s] [%s] [%s] %s\n", timestamp, level_str[level], component, buffer);
        fflush(stdout);
    }

    if (log_file_ptr) {
        fprintf(log_file_ptr, "[%s] [%s] [%s] %s\n", timestamp, level_str[level], component, buffer);
        fflush(log_file_ptr);
    }
}

void log_request(const char *component, const char *source_ip, int source_port,
                 const char *dest_ip, int dest_port, const char *username,
                 const char *request) {
    char *timestamp = get_timestamp();

    char buffer[4096];
    snprintf(buffer, sizeof(buffer),
             "[%s] [%s] Request: %s:%d -> %s:%d | User: %s | %s",
             timestamp, component, source_ip, source_port, dest_ip, dest_port,
             username ? username : "N/A", request);

    if (console_logging_enabled) {
        fprintf(stdout, "%s\n", buffer);
        fflush(stdout);
    }

    if (log_file_ptr) {
        fprintf(log_file_ptr, "%s\n", buffer);
        fflush(log_file_ptr);
    }
}

void log_migration_event(const char *filename, const char *source_ip, int source_port,
                         const char *target_ip, int target_port, const char *status) {
    if (!filename || !status || !source_ip || !target_ip) {
        return;
    }

    LogLevel level = LOG_INFO;
    log_message(level, "NS", "Migration %s for '%s' (%s:%d -> %s:%d)",
                status, filename, source_ip, source_port, target_ip, target_port);
}

void log_cleanup(void) {
    if (log_file_ptr) {
        fclose(log_file_ptr);
        log_file_ptr = NULL;
    }
}

// ============================================================================
// TIME UTILITIES
// ============================================================================

char* get_timestamp(void) {
    static char buffer[64];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm_info);
    return buffer;
}

char* format_time(time_t time) {
    static char buffer[64];
    struct tm *tm_info = localtime(&time);
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm_info);
    return buffer;
}

time_t get_current_time(void) {
    return time(NULL);
}

// ============================================================================
// STRING UTILITIES
// ============================================================================

void trim_string(char *str) {
    if (!str) return;
    
    // Trim leading whitespace
    char *start = str;
    while (isspace((unsigned char)*start)) start++;
    
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
    
    // Trim trailing whitespace
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    *(end + 1) = '\0';
}

int string_ends_with(const char *str, const char *suffix) {
    if (!str || !suffix) return 0;
    
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    
    if (suffix_len > str_len) return 0;
    
    return strcmp(str + str_len - suffix_len, suffix) == 0;
}

int string_starts_with(const char *str, const char *prefix) {
    if (!str || !prefix) return 0;
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

int safe_strcpy(char *dest, const char *src, size_t dest_size) {
    if (!dest || !src || dest_size == 0) return -1;
    
    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
    return 0;
}

int safe_strcat(char *dest, const char *src, size_t dest_size) {
    if (!dest || !src || dest_size == 0) return -1;
    
    size_t dest_len = strlen(dest);
    if (dest_len >= dest_size - 1) return -1;
    
    strncat(dest, src, dest_size - dest_len - 1);
    return 0;
}

// ============================================================================
// FILE UTILITIES
// ============================================================================

int file_exists(const char *path) {
    if (!path) return 0;
    return access(path, F_OK) == 0;
}

long get_file_size(const char *path) {
    if (!path) return -1;
    
    struct stat st;
    if (stat(path, &st) == 0) {
        return st.st_size;
    }
    return -1;
}

time_t get_file_mtime(const char *path) {
    if (!path) return -1;
    
    struct stat st;
    if (stat(path, &st) == 0) {
        return st.st_mtime;
    }
    return -1;
}

time_t get_file_atime(const char *path) {
    if (!path) return -1;
    
    struct stat st;
    if (stat(path, &st) == 0) {
        return st.st_atime;
    }
    return -1;
}

int create_directory_recursive(const char *path) {
    if (!path) return -1;
    
    char tmp[MAX_PATH_LENGTH];
    char *p = NULL;
    size_t len;
    
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = 0;
    }
    
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    
    return 0;
}

int delete_file(const char *path) {
    if (!path) return -1;
    return unlink(path);
}

int copy_file(const char *src, const char *dest) {
    if (!src || !dest) return -1;
    
    FILE *source = fopen(src, "rb");
    if (!source) return -1;
    
    FILE *destination = fopen(dest, "wb");
    if (!destination) {
        fclose(source);
        return -1;
    }
    
    char buffer[8192];
    size_t bytes;
    
    while ((bytes = fread(buffer, 1, sizeof(buffer), source)) > 0) {
        if (fwrite(buffer, 1, bytes, destination) != bytes) {
            fclose(source);
            fclose(destination);
            return -1;
        }
    }
    
    fclose(source);
    fclose(destination);
    return 0;
}

// ============================================================================
// PATH UTILITIES
// ============================================================================

char* path_join(const char *base, const char *component) {
    if (!base || !component) return NULL;
    
    size_t base_len = strlen(base);
    size_t comp_len = strlen(component);
    int needs_separator = (base_len > 0 && base[base_len - 1] != '/');
    
    size_t total_len = base_len + comp_len + (needs_separator ? 1 : 0) + 1;
    char *result = (char *)malloc(total_len);
    
    if (!result) return NULL;
    
    strcpy(result, base);
    if (needs_separator) {
        strcat(result, "/");
    }
    strcat(result, component);
    
    return result;
}

char* path_basename(const char *path) {
    if (!path) return NULL;
    
    static char buffer[MAX_FILENAME_LENGTH];
    char temp[MAX_PATH_LENGTH];
    
    strncpy(temp, path, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';
    
    char *base = basename(temp);
    strncpy(buffer, base, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    
    return buffer;
}

char* path_dirname(const char *path) {
    if (!path) return NULL;
    
    static char buffer[MAX_PATH_LENGTH];
    char temp[MAX_PATH_LENGTH];
    
    strncpy(temp, path, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';
    
    char *dir = dirname(temp);
    strncpy(buffer, dir, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    
    return buffer;
}

char* path_normalize(const char *path) {
    if (!path) return NULL;
    
    char *result = (char *)malloc(MAX_PATH_LENGTH);
    if (!result) return NULL;
    
    // Simple normalization - just copy and remove trailing slashes
    strncpy(result, path, MAX_PATH_LENGTH - 1);
    result[MAX_PATH_LENGTH - 1] = '\0';
    
    size_t len = strlen(result);
    while (len > 1 && result[len - 1] == '/') {
        result[len - 1] = '\0';
        len--;
    }
    
    return result;
}

int path_is_absolute(const char *path) {
    if (!path || path[0] == '\0') return 0;
    return path[0] == '/';
}

// ============================================================================
// NETWORK UTILITIES
// ============================================================================

int parse_address(const char *address, char *ip, int *port) {
    if (!address || !ip || !port) return -1;
    
    char temp[MAX_IP_LENGTH + MAX_PORT_LENGTH];
    strncpy(temp, address, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';
    
    char *colon = strchr(temp, ':');
    if (!colon) return -1;
    
    *colon = '\0';
    strcpy(ip, temp);
    *port = atoi(colon + 1);
    
    return 0;
}

char* format_address(const char *ip, int port) {
    static char buffer[MAX_IP_LENGTH + MAX_PORT_LENGTH];
    snprintf(buffer, sizeof(buffer), "%s:%d", ip, port);
    return buffer;
}

int is_valid_port(int port) {
    return port > 0 && port <= 65535;
}

int is_valid_ip(const char *ip) {
    if (!ip) return 0;
    
    struct sockaddr_in sa;
    return inet_pton(AF_INET, ip, &(sa.sin_addr)) == 1;
}

// ============================================================================
// VALIDATION UTILITIES
// ============================================================================

int validate_username(const char *username) {
    if (!username || strlen(username) == 0 || strlen(username) >= MAX_USERNAME_LENGTH) {
        return 0;
    }
    
    // Username should contain only alphanumeric characters and underscores
    for (const char *p = username; *p; p++) {
        if (!isalnum(*p) && *p != '_') {
            return 0;
        }
    }
    
    return 1;
}

int validate_filename(const char *filename) {
    if (!filename || strlen(filename) == 0 || strlen(filename) >= MAX_FILENAME_LENGTH) {
        return 0;
    }
    
    // Filename should not contain certain special characters
    const char *invalid_chars = "<>:\"|?*";
    for (const char *p = filename; *p; p++) {
        if (strchr(invalid_chars, *p)) {
            return 0;
        }
    }
    
    return 1;
}

int validate_permission(const char *permission) {
    if (!permission) return 0;
    return strcmp(permission, "R") == 0 || strcmp(permission, "W") == 0;
}

// ============================================================================
// MEMORY UTILITIES
// ============================================================================

void* safe_malloc(size_t size) {
    void *ptr = malloc(size);
    if (!ptr && size > 0) {
        fprintf(stderr, "Memory allocation failed\n");
    }
    return ptr;
}

void* safe_calloc(size_t count, size_t size) {
    void *ptr = calloc(count, size);
    if (!ptr && count > 0 && size > 0) {
        fprintf(stderr, "Memory allocation failed\n");
    }
    return ptr;
}

void* safe_realloc(void *ptr, size_t size) {
    void *new_ptr = realloc(ptr, size);
    if (!new_ptr && size > 0) {
        fprintf(stderr, "Memory reallocation failed\n");
    }
    return new_ptr;
}

void safe_free(void *ptr) {
    if (ptr) {
        free(ptr);
    }
}

// ============================================================================
// TEXT PROCESSING UTILITIES
// ============================================================================

int count_words(const char *text) {
    if (!text) return 0;
    
    int count = 0;
    int in_word = 0;
    
    for (const char *p = text; *p; p++) {
        if (isspace(*p)) {
            in_word = 0;
        } else {
            if (!in_word) {
                count++;
                in_word = 1;
            }
        }
    }
    
    return count;
}

int count_chars(const char *text) {
    if (!text) return 0;
    
    int count = 0;
    for (const char *p = text; *p; p++) {
        if (!isspace(*p)) {
            count++;
        }
    }
    
    return count;
}

int is_sentence_delimiter(char c) {
    return c == '.' || c == '!' || c == '?';
}

int parse_long_long(const char *text, long long *out_value) {
    if (!text || !out_value || !*text) {
        return -1;
    }

    errno = 0;
    char *endptr = NULL;
    long long value = strtoll(text, &endptr, 10);
    if (errno != 0 || !endptr || *endptr != '\0') {
        return -1;
    }

    *out_value = value;
    return 0;
}

int parse_double(const char *text, double *out_value) {
    if (!text || !out_value || !*text) {
        return -1;
    }

    errno = 0;
    char *endptr = NULL;
    double value = strtod(text, &endptr);
    if (errno != 0 || !endptr || *endptr != '\0') {
        return -1;
    }

    *out_value = value;
    return 0;
}

static const char BASE64_TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    if (c == '=') {
        return -2; // padding
    }
    return -1;
}

int base64_encode(const unsigned char *input, size_t input_length, char **output) {
    if (!input || !output) {
        return -1;
    }

    size_t output_length = 4 * ((input_length + 2) / 3);
    char *encoded = (char *)malloc(output_length + 1);
    if (!encoded) {
        return -1;
    }

    size_t j = 0;
    for (size_t i = 0; i < input_length; ) {
        uint32_t octet_a = i < input_length ? input[i++] : 0;
        uint32_t octet_b = i < input_length ? input[i++] : 0;
        uint32_t octet_c = i < input_length ? input[i++] : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        encoded[j++] = BASE64_TABLE[(triple >> 18) & 0x3F];
        encoded[j++] = BASE64_TABLE[(triple >> 12) & 0x3F];
        encoded[j++] = BASE64_TABLE[(triple >> 6) & 0x3F];
        encoded[j++] = BASE64_TABLE[triple & 0x3F];
    }

    size_t mod = input_length % 3;
    if (mod > 0) {
        encoded[output_length - 1] = '=';
        if (mod == 1) {
            encoded[output_length - 2] = '=';
        }
    }

    encoded[output_length] = '\0';
    *output = encoded;
    return 0;
}

int base64_decode(const char *input, unsigned char **output, size_t *output_length) {
    if (!input || !output || !output_length) {
        return -1;
    }

    size_t input_length = strlen(input);
    if (input_length == 0 || input_length % 4 != 0) {
        return -1;
    }

    size_t padding = 0;
    if (input_length >= 2) {
        if (input[input_length - 1] == '=') {
            padding++;
        }
        if (input[input_length - 2] == '=') {
            padding++;
        }
    }

    size_t decoded_length = (input_length / 4) * 3 - padding;
    unsigned char *decoded = (unsigned char *)malloc(decoded_length + 1);
    if (!decoded) {
        return -1;
    }

    size_t in_index = 0;
    size_t out_index = 0;
    while (in_index < input_length) {
        int sextet_a = base64_value((unsigned char)input[in_index++]);
        int sextet_b = base64_value((unsigned char)input[in_index++]);
        int sextet_c = base64_value((unsigned char)input[in_index++]);
        int sextet_d = base64_value((unsigned char)input[in_index++]);

        if (sextet_a < 0 || sextet_b < 0 || sextet_c < -1 || sextet_d < -1) {
            free(decoded);
            return -1;
        }

        uint32_t triple = ((uint32_t)sextet_a << 18) | ((uint32_t)sextet_b << 12);
        if (sextet_c >= 0) {
            triple |= ((uint32_t)sextet_c << 6);
        }
        if (sextet_d >= 0) {
            triple |= (uint32_t)sextet_d;
        }

        if (out_index < decoded_length) {
            decoded[out_index++] = (unsigned char)((triple >> 16) & 0xFF);
        }
        if (sextet_c >= 0 && out_index < decoded_length) {
            decoded[out_index++] = (unsigned char)((triple >> 8) & 0xFF);
        }
        if (sextet_d >= 0 && out_index < decoded_length) {
            decoded[out_index++] = (unsigned char)(triple & 0xFF);
        }
    }

    decoded[decoded_length] = '\0';
    *output = decoded;
    *output_length = decoded_length;
    return 0;
}

int count_sentences(const char *text) {
    if (!text) return 0;
    
    int count = 0;
    for (const char *p = text; *p; p++) {
        if (is_sentence_delimiter(*p)) {
            count++;
        }
    }
    
    return count;
}

char* extract_sentence(const char *text, int sentence_index) {
    if (!text || sentence_index < 0) return NULL;
    
    int current_sentence = 0;
    const char *start = text;
    const char *end = text;
    
    // Skip leading whitespace
    while (*start && isspace(*start)) start++;
    
    if (sentence_index == 0 && *start == '\0') {
        return strdup("");
    }
    
    end = start;
    
    while (*end) {
        if (is_sentence_delimiter(*end)) {
            if (current_sentence == sentence_index) {
                // Found the sentence
                size_t len = end - start + 1;
                char *result = (char *)malloc(len + 1);
                if (!result) return NULL;
                
                strncpy(result, start, len);
                result[len] = '\0';
                return result;
            }
            
            current_sentence++;
            end++;
            
            // Skip whitespace after delimiter
            while (*end && isspace(*end)) end++;
            start = end;
        } else {
            end++;
        }
    }
    
    // Handle last sentence without delimiter
    if (current_sentence == sentence_index && end > start) {
        size_t len = end - start;
        char *result = (char *)malloc(len + 1);
        if (!result) return NULL;
        
        strncpy(result, start, len);
        result[len] = '\0';
        return result;
    }
    
    return NULL;
}

char* replace_word_in_sentence(const char *sentence, int word_index, const char *new_word) {
    if (!sentence || !new_word || word_index < 0) return NULL;
    
    // This is a simplified implementation
    // A full implementation would need to handle various edge cases
    
    char *result = (char *)malloc(strlen(sentence) + strlen(new_word) + 256);
    if (!result) return NULL;
    
    strcpy(result, sentence);
    return result;
}
