# Protocol Examples and Usage Guide

This document provides examples of how to use the protocol definitions in the distributed file system.

## Table of Contents

1. [Initialization and Handshakes](#initialization-and-handshakes)
2. [NS-Handled Commands](#ns-handled-commands)
3. [Coordinated Commands](#coordinated-commands)
4. [Direct Client-SS Communication](#direct-client-ss-communication)
5. [Bonus Functionality](#bonus-functionality)

---

## Initialization and Handshakes

### Client Handshake

**Client -> NS:**
```
HELLO_CLIENT|john_doe\n
```

**NS -> Client (Success):**
```
OK|Welcome, john_doe.\n
```

**NS -> Client (Error):**
```
ERR|101|Username invalid or already connected.\n
```

### Storage Server Handshake

**SS -> NS:**
```
HELLO_SS|192.168.1.10|8001|192.168.1.10|8002\n
```

**NS -> SS (Success):**
```
OK|SS registered. Awaiting file list.\n
```

**SS -> NS (File Sync):**
```
SS_HAS_FILE|file1.txt\n
SS_HAS_FILE|file2.txt\n
SS_HAS_FILE|folder/file3.txt\n
SS_FILES_DONE\n
```

**NS -> SS (Sync Complete):**
```
OK|File list synchronized.\n
```

---

## NS-Handled Commands

### LIST Users

**Client -> NS:**
```
LIST_USERS\n
```

**NS -> Client (Success):**
```
OK_LIST|user1\n
OK_LIST|user2\n
OK_LIST|kaevi\n
OK_LIST_END\n
```

### ADDACCESS

**Client -> NS (Add Read Access):**
```
ADDACCESS|notes.txt|jane_doe|R\n
```

**NS -> Client (Success):**
```
OK|Access modified for notes.txt.\n
```

**NS -> Client (Error - Not Owner):**
```
ERR|403|Permission denied. Only the owner can change access.\n
```

**NS -> Client (Error - User Not Found):**
```
ERR|102|User 'jane_doe' not found.\n
```

**Client -> NS (Add Write Access):**
```
ADDACCESS|notes.txt|jane_doe|W\n
```

### REMACCESS

**Client -> NS:**
```
REMACCESS|notes.txt|jane_doe\n
```

**NS -> Client (Success):**
```
OK|Access removed for jane_doe.\n
```

---

## Coordinated Commands

### CREATE File

**Client -> NS:**
```
CREATE|newfile.txt\n
```

**NS -> Client (Error - File Exists):**
```
ERR|409|File already exists.\n
```

**NS -> SS (If OK):**
```
CREATE_FILE|newfile.txt|john_doe\n
```

**SS -> NS (Success):**
```
OK_CREATE|File created.\n
```

**NS -> Client (Final):**
```
OK|File 'newfile.txt' created successfully.\n
```

### DELETE File

**Client -> NS:**
```
DELETE|oldfile.txt\n
```

**NS -> Client (Error - Permission Denied):**
```
ERR|403|Permission denied. Only the owner can delete the file.\n
```

**NS -> Client (Error - Not Found):**
```
ERR|404|File not found.\n
```

**NS -> SS (If OK):**
```
DELETE_FILE|oldfile.txt\n
```

**SS -> NS (Success):**
```
OK_DELETE|File deleted.\n
```

**NS -> Client (Final):**
```
OK|File 'oldfile.txt' deleted successfully.\n
```

### VIEW and INFO

**Client -> NS (Basic View):**
```
VIEW\n
```

**Client -> NS (View All):**
```
VIEW|-a\n
```

**Client -> NS (View with Details):**
```
VIEW|-l\n
```

**Client -> NS (View All with Details):**
```
VIEW|-al\n
```

**NS -> SS (Get Stats):**
```
GET_STATS|file.txt\n
```

**SS -> NS (Success):**
```
OK_STATS|file.txt|150|823|1024|1729785000|1729784500\n
```
(Format: filename|word_count|char_count|size_bytes|last_access_time|last_mod_time)

**NS -> Client (View -l Response):**
```
OK_VIEW_L|file1.txt|150|823|1729785000|john_doe\n
OK_VIEW_L|file2.txt|89|456|1729784000|jane_doe\n
OK_VIEW_END\n
```

**Client -> NS (Info):**
```
INFO|file.txt\n
```

**NS -> Client (Info Response):**
```
OK_INFO_START|file.txt\n
INFO_LINE|Owner: john_doe\n
INFO_LINE|Size: 1024 bytes\n
INFO_LINE|Last Modified: 2025-10-24 14:32:00\n
INFO_LINE|Access: john_doe (RW), jane_doe (R)\n
OK_INFO_END\n
```

### EXEC

**Client -> NS:**
```
EXEC|script.txt\n
```

**NS -> Client (Error - Permission Denied):**
```
ERR|403|Permission denied (Read access required).\n
```

**NS -> SS (If OK):**
```
GET_CONTENT|script.txt\n
```

**SS -> NS (Success):**
```
OK_CONTENT|echo "Hello World"\nls -la\n
```

**NS -> Client (Streaming Output):**
```
OK_EXEC_START\n
EXEC_OUT|Hello World\n
EXEC_OUT|total 48\n
EXEC_OUT|drwxr-xr-x 2 user user 4096 Oct 24 14:30 .\n
OK_EXEC_END\n
```

---

## Direct Client-SS Communication

### Phase 1: Location Request

**Client -> NS:**
```
REQ_LOC|READ|notes.txt\n
```

**NS -> Client (Success):**
```
OK_LOC|notes.txt|192.168.1.10|8002\n
```

**NS -> Client (Error - Permission Denied):**
```
ERR|403|Permission denied.\n
```

**NS -> Client (Error - Not Found):**
```
ERR|404|File not found.\n
```

### Phase 2: Direct Communication

#### READ

**Client -> SS:**
```
REQ_READ|john_doe|notes.txt\n
```

**SS -> Client (Success):**
```
OK_READ_START\n
This is the content of the file.
It can span multiple lines.
OK_READ_END\n
```

**SS -> Client (Error):**
```
ERR|500|File read error on server.\n
```

#### STREAM

**Client -> SS:**
```
REQ_STREAM|john_doe|notes.txt\n
```

**SS -> Client (Success - one word every 0.1s):**
```
OK_STREAM|This\n
OK_STREAM|is\n
OK_STREAM|the\n
OK_STREAM|content\n
OK_STREAM_END\n
```

#### UNDO

**Client -> SS:**
```
REQ_UNDO|john_doe|notes.txt\n
```

**SS -> Client (Success):**
```
OK_UNDO|File reverted to previous state.\n
```

**SS -> Client (Error - No History):**
```
ERR|502|No undo history available for this file.\n
```

**SS -> Client (Error - Locked):**
```
ERR|423|File is locked, cannot undo right now.\n
```

#### WRITE

**Step 1: Request Lock**

**Client -> SS:**
```
REQ_WRITE_LOCK|john_doe|notes.txt|0\n
```

**SS -> Client (Success):**
```
OK_LOCKED|0|This is the first sentence.\n
```

**SS -> Client (Error - Locked):**
```
ERR|423|Sentence is locked by another user.\n
```

**SS -> Client (Error - Invalid Index):**
```
ERR|400|Invalid sentence index.\n
```

**Step 2: Send Edits**

**Client -> SS:**
```
WRITE_DATA|2|awesome\n
WRITE_DATA|5|really\n
```

**Step 3: Commit**

**Client -> SS:**
```
ETIRW\n
```

**SS -> Client (Success):**
```
OK_WRITE_DONE|Write successful.\n
```

**SS -> Client (Error):**
```
ERR|500|Failed to write changes to disk.\n
```

---

## Bonus Functionality

### Hierarchical Folders

**Client -> NS (Create Folder):**
```
CREATEFOLDER|documents/work\n
```

**Client -> NS (Move File):**
```
MOVE|report.txt|documents/work\n
```

**Client -> NS (View Folder):**
```
VIEWFOLDER|documents/work\n
```

### Checkpoints

**Client -> NS (Location Request):**
```
REQ_LOC|CHECKPOINT|document.txt\n
```

**NS -> Client:**
```
OK_LOC|document.txt|192.168.1.10|8002\n
```

**Client -> SS (Create Checkpoint):**
```
REQ_CHECKPOINT|john_doe|document.txt|version1\n
```

**SS -> Client:**
```
OK_CHECKPOINT|Checkpoint 'version1' created.\n
```

**Client -> SS (Revert to Checkpoint):**
```
REQ_REVERT|john_doe|document.txt|version1\n
```

**SS -> Client:**
```
OK_REVERT|File reverted to 'version1'.\n
```

**Client -> SS (List Checkpoints):**
```
REQ_LIST_CHECKPOINTS|john_doe|document.txt\n
```

**SS -> Client:**
```
OK_LIST_CHECKPOINT|version1\n
OK_LIST_CHECKPOINT|version2\n
OK_LIST_CHECKPOINT|backup_oct24\n
OK_LIST_CHECKPOINT_END\n
```

### Fault Tolerance

**NS -> SS_Replica (Replicate):**
```
REPLICATE_FILE|important.txt|192.168.1.10|8002\n
```

**SS_Primary -> NS (Write Complete):**
```
WRITE_COMPLETE|important.txt\n
```

**NS -> SS_Replica (Sync):**
```
SYNC_FILE|important.txt|192.168.1.10|8002\n
```

**NS -> SS (Heartbeat):**
```
PING\n
```

**SS -> NS (Heartbeat Response):**
```
PONG\n
```

---

## Code Examples

### Building a Message

```c
#include "protocol.h"

// Create a HELLO_CLIENT message
const char *fields[] = {MSG_HELLO_CLIENT, "john_doe"};
char *message = protocol_build_message(fields, 2);
// Result: "HELLO_CLIENT|john_doe\n"

// Send the message
protocol_send_message(sockfd, message);
free(message);
```

### Parsing a Message

```c
#include "protocol.h"

// Receive a message
char *raw_message = protocol_receive_message(sockfd);

// Parse the message
ProtocolMessage msg;
if (protocol_parse_message(raw_message, &msg) == 0) {
    // Access fields
    printf("Command: %s\n", msg.fields[0]);
    
    // Check if it's an error
    if (protocol_is_error(&msg)) {
        int error_code = protocol_get_error_code(&msg);
        printf("Error %d: %s\n", error_code, msg.fields[2]);
    }
    
    // Clean up
    protocol_free_message(&msg);
}

free(raw_message);
```

### Building an Error Response

```c
#include "protocol.h"

// Build an error message
char *error_msg = protocol_build_error(ERR_FILE_NOT_FOUND, 
                                       "The file 'test.txt' does not exist.");
// Result: "ERR|404|The file 'test.txt' does not exist.\n"

protocol_send_message(sockfd, error_msg);
free(error_msg);
```

### Building an OK Response

```c
#include "protocol.h"

// Simple OK
char *ok_msg = protocol_build_ok(NULL);
// Result: "OK\n"

// OK with message
char *ok_msg2 = protocol_build_ok("File created successfully.");
// Result: "OK|File created successfully.\n"

protocol_send_message(sockfd, ok_msg);
free(ok_msg);
free(ok_msg2);
```

---

## Error Codes Reference

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

---

## Protocol Flow Diagrams

### CREATE Flow
```
Client              Name Server           Storage Server
  |                      |                       |
  |--CREATE|file.txt---->|                       |
  |                      |--CREATE_FILE|file---->|
  |                      |                       |
  |                      |<--OK_CREATE|File------|
  |<--OK|File created----|                       |
  |                      |                       |
```

### WRITE Flow
```
Client              Name Server           Storage Server
  |                      |                       |
  |--REQ_LOC|WRITE|f---->|                       |
  |<--OK_LOC|f|ip|port---|                       |
  |                      |                       |
  |------------------REQ_WRITE_LOCK|u|f|idx----->|
  |<-----------------OK_LOCKED|idx|content-------|
  |                      |                       |
  |------------------WRITE_DATA|idx|content----->|
  |------------------WRITE_DATA|idx|content----->|
  |------------------ETIRW----------------------->|
  |                      |                       |
  |<-----------------OK_WRITE_DONE|Write---------|
  |                      |                       |
```

### READ Flow
```
Client              Name Server           Storage Server
  |                      |                       |
  |--REQ_LOC|READ|file-->|                       |
  |<--OK_LOC|f|ip|port---|                       |
  |                      |                       |
  |------------------REQ_READ|user|file--------->|
  |<-----------------OK_READ_START---------------|
  |<-----------------file content----------------|
  |<-----------------OK_READ_END-----------------|
  |                      |                       |
```

---

## Best Practices

1. **Always validate messages** before processing them
2. **Free allocated memory** after using protocol functions
3. **Check return values** from protocol functions
4. **Use error codes consistently** across all components
5. **Log all protocol messages** for debugging and auditing
6. **Handle network errors gracefully** (timeouts, disconnections)
7. **Implement message size limits** to prevent buffer overflows
8. **Use timeouts** when receiving messages to avoid blocking indefinitely

---

## Testing

To test the protocol implementation, you can use the following approach:

```c
#include "protocol.h"
#include <assert.h>

void test_protocol() {
    // Test message building
    const char *fields[] = {"HELLO_CLIENT", "test_user"};
    char *msg = protocol_build_message(fields, 2);
    assert(msg != NULL);
    assert(strcmp(msg, "HELLO_CLIENT|test_user\n") == 0);
    free(msg);
    
    // Test message parsing
    ProtocolMessage parsed;
    protocol_parse_message("HELLO_CLIENT|test_user\n", &parsed);
    assert(parsed.field_count == 2);
    assert(strcmp(parsed.fields[0], "HELLO_CLIENT") == 0);
    assert(strcmp(parsed.fields[1], "test_user") == 0);
    protocol_free_message(&parsed);
    
    // Test error building
    char *err = protocol_build_error(404, "File not found.");
    assert(err != NULL);
    assert(strcmp(err, "ERR|404|File not found.\n") == 0);
    free(err);
    
    printf("All protocol tests passed!\n");
}
```
