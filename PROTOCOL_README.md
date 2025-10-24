# Protocol Implementation - Distributed File System

This directory contains the complete protocol implementation for the LangOS distributed file system project.

## 📁 Project Structure

```
course-project-naam-mein-kya-rakha-hai/
├── common/
│   ├── protocol.h          # Protocol definitions and constants
│   ├── protocol.c          # Protocol implementation
│   ├── utils.h             # Utility functions header
│   ├── utils.c             # Utility functions implementation
│   └── test_protocol.c     # Protocol test suite
├── name_server/
│   ├── name_server.h       # Name server header
│   └── name_server.c       # Name server implementation
├── storage_server/
│   ├── storage_server.h    # Storage server header (to be created)
│   └── storage_server.c    # Storage server implementation (to be created)
├── client/
│   ├── client.h            # Client header (to be created)
│   └── client.c            # Client implementation (to be created)
├── Makefile                # Build configuration
├── PROTOCOL_EXAMPLES.md    # Detailed protocol examples
└── README.md               # This file
```

## 🚀 Quick Start

### Building the Project

```bash
# Build all components
make

# Build only protocol tests
make test_protocol

# Clean build artifacts
make clean

# Rebuild everything
make rebuild
```

### Running Tests

```bash
# Run protocol tests
make test_protocol
```

Expected output:
```
===========================================
     Protocol Implementation Tests
===========================================

Testing message building...
  ✓ HELLO_CLIENT message: HELLO_CLIENT|john_doe
  ✓ HELLO_SS message: HELLO_SS|192.168.1.10|8001|192.168.1.10|8002
  ...
All Tests Passed Successfully! ✓
===========================================
```

## 📋 Protocol Overview

### Transport Protocol
- **Transport Layer**: TCP
- **Message Format**: Text-based with pipe (`|`) delimiter
- **Message Termination**: Single newline character (`\n`)

### Message Structure
```
COMMAND|field1|field2|...|fieldN\n
```

### Error Format
```
ERR|error_code|human_readable_message\n
```

## 🔌 Components

### 1. Actors

- **C**: Client - User interface for file operations
- **NS**: Name Server - Central coordinator
- **SS**: Storage Server - File storage and retrieval

### 2. Communication Patterns

#### NS-Handled (Client ↔ Name Server)
Commands that are fully processed by the Name Server:
- LIST_USERS
- ADDACCESS / REMACCESS
- VIEW (various flags)
- INFO

#### Coordinated (Client → NS → SS)
Commands that require Name Server coordination:
- CREATE
- DELETE
- EXEC

#### Direct (Client → NS, then Client ↔ SS)
Commands with direct client-storage server communication:
- READ
- WRITE
- STREAM
- UNDO

## 📚 API Reference

### Core Functions

#### Message Building
```c
char* protocol_build_message(const char **fields, int field_count);
```
Build a protocol message from an array of field strings.

**Parameters:**
- `fields`: Array of string pointers
- `field_count`: Number of fields

**Returns:** Newly allocated message string (caller must free)

**Example:**
```c
const char *fields[] = {"CREATE", "newfile.txt"};
char *msg = protocol_build_message(fields, 2);
// Result: "CREATE|newfile.txt\n"
free(msg);
```

#### Message Parsing
```c
int protocol_parse_message(const char *raw, ProtocolMessage *msg);
```
Parse a raw protocol message into structured format.

**Parameters:**
- `raw`: Raw message string
- `msg`: Pointer to ProtocolMessage structure to fill

**Returns:** 0 on success, -1 on error

**Example:**
```c
ProtocolMessage msg;
protocol_parse_message("HELLO_CLIENT|john_doe\n", &msg);
// Access: msg.fields[0] = "HELLO_CLIENT"
//         msg.fields[1] = "john_doe"
protocol_free_message(&msg);
```

#### Error Handling
```c
char* protocol_build_error(int error_code, const char *error_msg);
int protocol_is_error(const ProtocolMessage *msg);
int protocol_get_error_code(const ProtocolMessage *msg);
```

**Example:**
```c
char *err = protocol_build_error(ERR_FILE_NOT_FOUND, "file.txt not found");
// Result: "ERR|404|file.txt not found\n"

ProtocolMessage msg;
protocol_parse_message(err, &msg);
if (protocol_is_error(&msg)) {
    int code = protocol_get_error_code(&msg);
    printf("Error %d: %s\n", code, msg.fields[2]);
}
free(err);
protocol_free_message(&msg);
```

#### Network I/O
```c
int protocol_send_message(int sockfd, const char *message);
char* protocol_receive_message(int sockfd);
```

**Example:**
```c
// Send
const char *fields[] = {"HELLO_CLIENT", "user"};
char *msg = protocol_build_message(fields, 2);
protocol_send_message(sockfd, msg);
free(msg);

// Receive
char *response = protocol_receive_message(sockfd);
ProtocolMessage parsed;
protocol_parse_message(response, &parsed);
// Process message...
protocol_free_message(&parsed);
free(response);
```

### Utility Functions

#### Logging
```c
int log_init(const char *log_file, LogLevel level);
void log_message(LogLevel level, const char *component, const char *format, ...);
void log_request(const char *component, const char *source_ip, int source_port,
                 const char *dest_ip, int dest_port, const char *username,
                 const char *request);
void log_cleanup(void);
```

**Example:**
```c
log_init("system.log", LOG_INFO);
log_message(LOG_INFO, "NS", "Server started on port %d", 8000);
log_request("NS", "192.168.1.100", 5000, "192.168.1.10", 8000, 
            "john_doe", "CREATE|file.txt");
log_cleanup();
```

#### File Operations
```c
int file_exists(const char *path);
long get_file_size(const char *path);
time_t get_file_mtime(const char *path);
time_t get_file_atime(const char *path);
int create_directory_recursive(const char *path);
int delete_file(const char *path);
int copy_file(const char *src, const char *dest);
```

#### Path Manipulation
```c
char* path_join(const char *base, const char *component);
char* path_basename(const char *path);
char* path_dirname(const char *path);
char* path_normalize(const char *path);
int path_is_absolute(const char *path);
```

#### Text Processing
```c
int count_words(const char *text);
int count_chars(const char *text);
int count_sentences(const char *text);
char* extract_sentence(const char *text, int sentence_index);
int is_sentence_delimiter(char c);
```

## 📖 Protocol Message Reference

### Handshake Messages

| Message | Format | Direction | Description |
|---------|--------|-----------|-------------|
| HELLO_CLIENT | `HELLO_CLIENT\|username\n` | C → NS | Client handshake |
| HELLO_SS | `HELLO_SS\|ns_ip\|ns_port\|client_ip\|client_port\n` | SS → NS | Storage server handshake |
| SS_HAS_FILE | `SS_HAS_FILE\|filename\n` | SS → NS | File list sync |
| SS_FILES_DONE | `SS_FILES_DONE\n` | SS → NS | File list complete |

### File Operations

| Message | Format | Direction | Description |
|---------|--------|-----------|-------------|
| CREATE | `CREATE\|filename\n` | C → NS | Create new file |
| DELETE | `DELETE\|filename\n` | C → NS | Delete file |
| READ | `REQ_READ\|username\|filename\n` | C → SS | Read file content |
| WRITE | See [WRITE Flow](#write-flow) | C ↔ SS | Write to file |
| INFO | `INFO\|filename\n` | C → NS | Get file metadata |
| VIEW | `VIEW\|[flags]\n` | C → NS | List files |

### Access Control

| Message | Format | Direction | Description |
|---------|--------|-----------|-------------|
| ADDACCESS | `ADDACCESS\|filename\|username\|permission\n` | C → NS | Add user access |
| REMACCESS | `REMACCESS\|filename\|username\n` | C → NS | Remove user access |
| LIST_USERS | `LIST_USERS\n` | C → NS | List all users |

### Advanced Operations

| Message | Format | Direction | Description |
|---------|--------|-----------|-------------|
| STREAM | `REQ_STREAM\|username\|filename\n` | C → SS | Stream file word-by-word |
| UNDO | `REQ_UNDO\|username\|filename\n` | C → SS | Undo last change |
| EXEC | `EXEC\|filename\n` | C → NS | Execute file as script |

## 🔄 Common Protocol Flows

### CREATE Flow
```
Client                    Name Server              Storage Server
  |                            |                          |
  |---CREATE|file.txt--------->|                          |
  |                            |---CREATE_FILE|file------>|
  |                            |                          |
  |                            |<--OK_CREATE|File---------|
  |<--OK|File created----------|                          |
```

### WRITE Flow
```
Client                    Name Server              Storage Server
  |                            |                          |
  |---REQ_LOC|WRITE|file------>|                          |
  |<--OK_LOC|file|ip|port------|                          |
  |                            |                          |
  |------------------REQ_WRITE_LOCK|u|f|idx-------------->|
  |<-----------------OK_LOCKED|idx|content----------------|
  |                            |                          |
  |------------------WRITE_DATA|idx|content-------------->|
  |------------------ETIRW---------------------------------|
  |<-----------------OK_WRITE_DONE|Write------------------|
```

### READ Flow
```
Client                    Name Server              Storage Server
  |                            |                          |
  |---REQ_LOC|READ|file------->|                          |
  |<--OK_LOC|file|ip|port------|                          |
  |                            |                          |
  |------------------REQ_READ|user|file------------------>|
  |<-----------------OK_READ_START-------------------------|
  |<-----------------file content--------------------------|
  |<-----------------OK_READ_END---------------------------|
```

## ⚠️ Error Codes

| Code | Constant | Description |
|------|----------|-------------|
| 101 | ERR_USERNAME_INVALID | Username invalid or already connected |
| 102 | ERR_USER_NOT_FOUND | User not found |
| 400 | ERR_INVALID_REQUEST | Invalid index or arguments |
| 403 | ERR_PERMISSION_DENIED | Permission denied |
| 404 | ERR_FILE_NOT_FOUND | File not found |
| 409 | ERR_FILE_EXISTS | File already exists |
| 423 | ERR_SENTENCE_LOCKED | Sentence is locked by another user |
| 500 | ERR_INTERNAL_ERROR | Internal server error |
| 501 | ERR_DISK_ERROR | Disk error |
| 502 | ERR_NO_UNDO_HISTORY | No undo history available |
| 503 | ERR_DELETE_FAILED | Failed to delete file from disk |
| 504 | ERR_EXEC_FAILED | Command execution failed on server |

## 🎯 Usage Examples

### Complete Client Example

```c
#include "common/protocol.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main() {
    // Connect to Name Server
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(8000),
        .sin_addr.s_addr = inet_addr("127.0.0.1")
    };
    connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    
    // Send HELLO_CLIENT
    const char *hello_fields[] = {MSG_HELLO_CLIENT, "john_doe"};
    char *hello_msg = protocol_build_message(hello_fields, 2);
    protocol_send_message(sockfd, hello_msg);
    free(hello_msg);
    
    // Receive response
    char *response = protocol_receive_message(sockfd);
    ProtocolMessage parsed;
    protocol_parse_message(response, &parsed);
    
    if (protocol_is_error(&parsed)) {
        printf("Error: %s\n", parsed.fields[2]);
    } else {
        printf("Success: %s\n", parsed.fields[1]);
    }
    
    protocol_free_message(&parsed);
    free(response);
    close(sockfd);
    return 0;
}
```

## 📝 Development Guidelines

### Best Practices

1. **Always free allocated memory**: Use `protocol_free_message()` and `free()` appropriately
2. **Check return values**: Validate all function returns
3. **Use error codes consistently**: Follow the defined error code scheme
4. **Log all communications**: Use logging utilities for debugging
5. **Validate inputs**: Check message format before processing
6. **Handle timeouts**: Don't block indefinitely on network I/O
7. **Thread safety**: Use proper synchronization for shared resources

### Adding New Commands

To add a new protocol command:

1. **Define constant in `protocol.h`:**
```c
#define MSG_NEW_COMMAND "NEW_COMMAND"
```

2. **Document in `PROTOCOL_EXAMPLES.md`:**
```markdown
### NEW_COMMAND
C -> NS: NEW_COMMAND|param1|param2\n
NS -> C: OK|Success message\n
```

3. **Add test in `test_protocol.c`:**
```c
const char *fields[] = {MSG_NEW_COMMAND, "param1", "param2"};
char *msg = protocol_build_message(fields, 3);
assert(strcmp(msg, "NEW_COMMAND|param1|param2\n") == 0);
free(msg);
```

## 🧪 Testing

Run the comprehensive test suite:

```bash
make test_protocol
```

The test suite validates:
- ✓ Message building
- ✓ Message parsing
- ✓ Error handling
- ✓ OK responses
- ✓ Message validation
- ✓ Specific command formats
- ✓ Complete protocol flows

## 📚 Additional Resources

- **PROTOCOL_EXAMPLES.md**: Detailed examples of every protocol message
- **Protocol Specification**: See project requirements document
- **API Documentation**: Function-level documentation in header files

## 🤝 Contributing

When implementing new features:

1. Update protocol definitions in `protocol.h`
2. Add examples to `PROTOCOL_EXAMPLES.md`
3. Write tests in `test_protocol.c`
4. Update this README
5. Test thoroughly with `make test_protocol`

## 📄 License

This is an academic project for OSN course.

---

**Note**: This implementation provides the complete protocol layer for the distributed file system. The Name Server, Storage Server, and Client components need to be implemented using these protocol definitions.
