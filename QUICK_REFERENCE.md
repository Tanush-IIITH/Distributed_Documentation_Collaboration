# Protocol Quick Reference Card

## 🚀 Quick Start

```bash
# Build and test
make test_protocol

# Clean and rebuild
make rebuild
```

## 📦 Include in Your Code

```c
#include "common/protocol.h"
#include "common/utils.h"
```

## 🔧 Common Operations

### Send a Message
```c
const char *fields[] = {MSG_CREATE, "file.txt"};
char *msg = protocol_build_message(fields, 2);
protocol_send_message(sockfd, msg);
free(msg);
```

### Receive and Parse
```c
char *raw = protocol_receive_message(sockfd);
ProtocolMessage msg;
protocol_parse_message(raw, &msg);
// Use msg.fields[0], msg.fields[1], etc.
protocol_free_message(&msg);
free(raw);
```

### Build Error Response
```c
char *err = protocol_build_error(ERR_FILE_NOT_FOUND, "file.txt not found");
protocol_send_message(sockfd, err);
free(err);
```

### Build OK Response
```c
char *ok = protocol_build_ok("File created successfully");
protocol_send_message(sockfd, ok);
free(ok);
```

## 📋 Message Format

```
COMMAND|field1|field2|...|fieldN\n
```

## 🔴 Error Format

```
ERR|error_code|human_readable_message\n
```

## 🗂️ Key Message Types

### Handshake
| Message | Format |
|---------|--------|
| HELLO_CLIENT | `HELLO_CLIENT\|username\n` |
| HELLO_SS | `HELLO_SS\|ns_ip\|ns_port\|client_ip\|client_port\n` |

### File Operations
| Message | Format |
|---------|--------|
| CREATE | `CREATE\|filename\n` |
| DELETE | `DELETE\|filename\n` |
| READ | `REQ_READ\|username\|filename\n` |
| WRITE_LOCK | `REQ_WRITE_LOCK\|username\|filename\|sentence_idx\n` |
| WRITE_DATA | `WRITE_DATA\|word_index\|content\n` |
| ETIRW | `ETIRW\n` |

### Info & Access
| Message | Format |
|---------|--------|
| VIEW | `VIEW\|[flags]\n` |
| INFO | `INFO\|filename\n` |
| LIST_USERS | `LIST_USERS\n` |
| ADDACCESS | `ADDACCESS\|filename\|username\|R\|W\n` |
| REMACCESS | `REMACCESS\|filename\|username\n` |

### Advanced
| Message | Format |
|---------|--------|
| STREAM | `REQ_STREAM\|username\|filename\n` |
| UNDO | `REQ_UNDO\|username\|filename\n` |
| EXEC | `EXEC\|filename\n` |

## ⚠️ Error Codes

| Code | Constant | Meaning |
|------|----------|---------|
| 101 | ERR_USERNAME_INVALID | Invalid username |
| 102 | ERR_USER_NOT_FOUND | User not found |
| 400 | ERR_INVALID_REQUEST | Invalid arguments |
| 403 | ERR_PERMISSION_DENIED | Permission denied |
| 404 | ERR_FILE_NOT_FOUND | File not found |
| 409 | ERR_FILE_EXISTS | File already exists |
| 423 | ERR_SENTENCE_LOCKED | Sentence locked |
| 500 | ERR_INTERNAL_ERROR | Internal error |
| 501 | ERR_DISK_ERROR | Disk error |
| 502 | ERR_NO_UNDO_HISTORY | No undo history |
| 503 | ERR_DELETE_FAILED | Delete failed |
| 504 | ERR_EXEC_FAILED | Exec failed |

## 🛠️ Utility Functions

### Logging
```c
log_init("server.log", LOG_INFO);
log_message(LOG_INFO, "NS", "Server started on port %d", port);
log_cleanup();
```

### File Operations
```c
if (file_exists(path)) {
    long size = get_file_size(path);
    time_t mtime = get_file_mtime(path);
}
```

### Validation
```c
if (validate_username(name) && validate_filename(file)) {
    // Valid
}
```

### Path Manipulation
```c
char *full_path = path_join("/base", "file.txt");
// full_path = "/base/file.txt"
free(full_path);
```

### Text Processing
```c
int words = count_words(text);
int chars = count_chars(text);
int sentences = count_sentences(text);
```

## 🔄 Common Flows

### CREATE Flow
```
C -> NS: CREATE|file.txt\n
NS -> SS: CREATE_FILE|file.txt|owner\n
SS -> NS: OK_CREATE|File created.\n
NS -> C: OK|File created successfully.\n
```

### WRITE Flow
```
C -> NS: REQ_LOC|WRITE|file.txt\n
NS -> C: OK_LOC|file.txt|ip|port\n
C -> SS: REQ_WRITE_LOCK|user|file.txt|0\n
SS -> C: OK_LOCKED|0|content\n
C -> SS: WRITE_DATA|5|word\n
C -> SS: ETIRW\n
SS -> C: OK_WRITE_DONE|Write successful.\n
```

### READ Flow
```
C -> NS: REQ_LOC|READ|file.txt\n
NS -> C: OK_LOC|file.txt|ip|port\n
C -> SS: REQ_READ|user|file.txt\n
SS -> C: OK_READ_START\n
SS -> C: [content]\n
SS -> C: OK_READ_END\n
```

## 📝 Best Practices

1. ✅ **Always check return values**
2. ✅ **Free allocated memory** (protocol_free_message, free)
3. ✅ **Validate inputs** before processing
4. ✅ **Use error codes** consistently
5. ✅ **Log all operations** for debugging
6. ✅ **Handle network errors** gracefully
7. ✅ **Test thoroughly** with protocol tests

## 🧪 Testing

```bash
# Run all tests
make test_protocol

# Expected output:
# All Tests Passed Successfully! ✓
```

## 📚 Full Documentation

- **PROTOCOL_EXAMPLES.md** - Detailed examples
- **PROTOCOL_README.md** - Complete guide
- **IMPLEMENTATION_SUMMARY.md** - Status report

## 🎯 Constants

```c
// Protocol
#define PROTOCOL_DELIMITER "|"
#define PROTOCOL_TERMINATOR "\n"

// Limits
#define MAX_MESSAGE_SIZE 65536
#define MAX_USERNAME_LENGTH 256
#define MAX_FILENAME_LENGTH 512

// Permissions
#define PERM_READ "R"
#define PERM_WRITE "W"
```

---

**Quick Help**: For detailed examples and full API reference, see PROTOCOL_README.md
