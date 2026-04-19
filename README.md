# Distributed Document Collaboration System

A modular distributed file storage and collaboration platform with a Name Server, Storage Servers, and multi-user Clients.

## Overview

This project implements a distributed document collaboration system supporting multiple users, concurrent access, centralized metadata management, and distributed storage. The system comprises three major components:

- **Client**: User-facing interface for viewing, editing, streaming, executing, and managing files
- **Name Server (NS)**: Central coordinator responsible for metadata, access control, indexing, and routing
- **Storage Servers (SS)**: Persistent storage nodes responsible for file data and concurrency management

Users can connect simultaneously, Storage Servers can join and leave dynamically, and files can be edited with sentence-level locking for safe concurrent updates.


## Client Features and Commands

### File Listing (VIEW)

```
VIEW            # Show accessible files
VIEW -a         # Show all system files
VIEW -l         # Show metadata
VIEW -al        # All files with detailed information
```

Metadata includes:
- Owner
- Word count
- Character count
- Last access timestamp
- Storage server location
- Permissions

### Read File (READ)

```
READ <filename>
```

Fetches complete file content from the Storage Server.

### Create File (CREATE)

```
CREATE <filename>
```

### Sentence-Level Concurrent Editing (WRITE)

```
WRITE <filename> <sentence_number>
<word_index> <content>
...
ETIRW
```

Features:
- Sentence-level locking
- Automatic sentence splitting on `.`, `!`, `?` (even inside words like "e.g.")
- Safe handling of concurrent writes
- Uses temporary write buffers to prevent corruption

### Undo (UNDO)

```
UNDO <filename>
```

Reverts the latest modification on a file. Undo history is maintained per file.

### File Metadata (INFO)

```
INFO <filename>
```

### Delete File (DELETE)

```
DELETE <filename>
```

Allowed only for file owners.

### Stream File (STREAM)

```
STREAM <filename>
```

Streams the file word-by-word with a 0.1 second delay. Handles mid-stream server failures gracefully.

### List Users (LIST)

```
LIST
```

### Access Control

```
ADDACCESS -R <filename> <username>  # Grant read access
ADDACCESS -W <filename> <username>  # Grant write access
REMACCESS <filename> <username>     # Remove access
```

Write access implies read access.

### Execute File (EXEC)

```
EXEC <filename>
```

Executes file content as shell commands on the Name Server, returning output to the client.

## Name Server (NS)

### Responsibilities

Maintains:
- File to Storage Server mapping
- Access Control Lists (ACLs)
- File metadata

Implements efficient search via hashmaps, tries, and caching.

Logs every event with timestamp, username, IP, port, and operation details.

Coordinates:
- File creation and deletion
- Access checks
- Lookups
- Execution commands
- Routes appropriate requests directly to Storage Servers

## Storage Servers (SS)

### Responsibilities

- Persistent file storage
- Undo history management
- Handle Name Server-initiated commands (create/move/delete/write)
- Handle direct client operations (read/write/stream)
- Enforce sentence-level locking
- Send STOP signals when operations complete

Storage Servers may join the system at any time and register themselves with the Name Server.

## Efficient Search

The Name Server uses:

- **Hashmaps** for exact filename lookup (O(1))
- **LRU cache** for recent queries

This ensures low lookup latency even with many files.

## Logging and Error Handling

### Log Format

Every operation is logged with:
- Timestamps
- Request origin (IP/port)
- Username
- Operation type
- Outcome and error codes

### Supported Error Classes

- Unauthorized access
- File not found
- Write lock conflicts
- Storage server unavailable
- Invalid command
- Mapping or metadata inconsistencies

## Cross-Server File Copy (Unique Feature)

A custom enhancement providing efficient data movement across servers:

```
COPY <srcfile> <destfile>
```

### Behavior

- **Same Storage Server**: Copy happens internally
- **Different Storage Servers**: 
  - A direct Storage Server to Storage Server connection is established
  - File content is transferred across servers
  - Metadata is synchronized back through the Name Server

### Benefits

- Enables efficient data movement across servers
- Supports load balancing and improved scalability
- Showcases modular multi-server coordination

This is an additional feature beyond the base specification.


## Build and Execution

### Name Server

```bash
make ns
./ns <port>
```

### Storage Server

```bash
make ss
./ss <ns_ip> <ns_port> <client_port>
```

### Client

```bash
make client
./client <ns_ip> <ns_port>
```

## Key Features Summary

- Multi-user concurrent access with centralized coordination
- Sentence-level locking for safe concurrent editing
- Dynamic Storage Server registration and management
- Comprehensive logging and error handling
- Efficient metadata search with caching
- Cross-server file operations
- Access control and permissions management
- Command-line interface with intuitive commands