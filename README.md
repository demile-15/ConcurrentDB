# ConcurrentDB (C, Networking, Concurrency)

A multi-threaded, networked server in C that manages a concurrent key-value database, supporting multiple clients, a server-side REPL, and robust signal handling for safe termination and resource cleanup.

## 🚀 Features

### ✅ Multi-client networking
- Clients connect to the server via sockets and interact through a custom REPL or command scripts.  
- All clients can concurrently:
  - **Search** for items in the database
  - **Add** new entries
  - **Remove** existing entries

### ✅ Server-Side REPL  
- The server includes its own interactive REPL for runtime control:
  - `p` – Print the database (to terminal or file)
  - `s` – Stop all clients
  - `g` – Resume client operations

### ✅ Signal Handling & Graceful Shutdown  
- Supports:
  - **EOF (Ctrl-D)** – Cleanly shuts down the server and all clients
  - **SIGINT (Ctrl-C)** – Terminates all clients, server remains active
  - **SIGPIPE** – Ignored to prevent crashes when clients disconnect unexpectedly  
- All threads and resources are cleaned up properly on exit


## ⚙️ Architecture Overview

### Server Logic (`server.c`)
- Manages socket communication, REPL commands, and client threads
- Thread-safe signal handling using `sigwait` in a dedicated thread
- Client connections handled in detached threads (`run_client`), with lifecycle managed via `client_constructor` and `thread_cleanup`

### Database Management (`db.c`)
- In-memory key-value store structured as a binary tree
- Fine-grained **hand-over-hand locking** using `pthread_rwlock_t` per node:
  - Multiple readers allowed simultaneously
  - Single writer enforced when updating

### Thread Coordination
- Global state tracks connected clients and server mode (accepting clients or not)
- Clients check global flags before executing commands
- Cleanup handlers ensure mutexes and memory are released even on thread cancellation

## 🧰 Technical Highlights

- **Language:** C (POSIX threads, sockets)
- **Concurrency:** Thread-safe with read/write locks
- **Signal Handling:** Robust support for SIGINT, SIGPIPE, EOF
- **REPL Support:** Both server and client-side REPLs
- **File Output:** Supports writing database state to text files

## 🧩 Custom Data Structures

- `node_t`: Binary tree node with data fields and `pthread_rwlock_t` lock
- `client_t`: Wrapper struct for managing individual client threads
- `enum locktype`: Enum abstraction for read vs write lock usage
