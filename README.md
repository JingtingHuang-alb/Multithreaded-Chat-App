# Multithreaded Chat Application (Assignment 2)

Authors: Jingting, Fengye

Date: December 2025


> A robust, real-time chat application built with **C**, **UDP Sockets**, and **Pthreads**. Supports concurrent clients, private messaging, chat history, and automatic management of inactive users.

---

## Table of Contents
- [Overview](#-overview)
- [Features](#-features)
- [Project Architecture](#-project-architecture)
- [Proposed Extensions (PEs)](#-proposed-extensions-implemented)
- [How to Build & Run](#-how-to-build--run)
- [Command Guide](#-command-guide)

---

## Overview
This project implements a distributed chat system consisting of a **Server** and multiple **Clients**.

* The **Server** manages a linked list of active users, broadcasts messages, and handles synchronization using Read-Write locks.
* The **Client** uses two threads (Listener & Sender) to handle sending user input and receiving server messages simultaneously.

Communication is handled via **UDP** (User Datagram Protocol).

## Features

### Core Functionality
* **Multithreading:** Both client and server use `pthread` to handle I/O and logic concurrently.
* **User Management:** Thread-safe Linked List to store connected users (Name, IP, Port).
* **Synchronization:** Uses `pthread_rwlock` (Reader-Writer Lock) to ensure thread safety when accessing the user list.
* **Private Messaging:** Support for direct messaging to specific users (`sayto`).

### Proposed Extensions (Implemented)

#### ✅ PE 1: Chat History
* **Mechanism:** Implemented a **Circular Buffer** on the server side.
* **Function:** When a new user connects, they immediately receive the last **15 broadcast messages** exchanged prior to their arrival.
* **Safety:** Protected by a dedicated Mutex lock.

#### ✅ PE 2: Remove Inactive Clients (Kick)
* **Mechanism:** The server spawns a dedicated "Cleaner Thread" that monitors user activity.
* **Logic:**
    1.  **Monitor:** Checks user timestamps every 5 seconds.
    2.  **Ping:** If a user is inactive for **30s**, sends a `ping$` packet.
    3.  **Kick:** If the user fails to respond (inactive for another 10s), they are removed from the list and notified.
* **Client-Side:** Clients automatically reply with `ret-ping$` upon receiving a ping, keeping the connection alive while the user is idle.

## Project Architecture

| Component | Responsibility | Key Tech |
| :--- | :--- | :--- |
| **Server Main** | Listens for UDP packets, parses commands (`$`), dispatches tasks. | `socket`, `bind`, `recvfrom` |
| **Cleaner Thread** | Periodically checks `last_active` timestamps and kicks zombies. | `pthread_create`, `sleep` |
| **User List** | Shared data structure protected by RW-Locks. | `struct`, `pthread_rwlock` |
| **Client Listener** | Background thread that prints incoming messages/history. | `pthread`, `printf` |
| **Client Sender** | Main thread that reads user input and sends requests. | `fgets`, `sendto` |

---

## How to Build & Run

### 1. Compilation
Make sure you have `gcc` installed. Compile both the server and client using the `-pthread` flag.

```bash
# Compile Server
gcc chat_server.c -o chat_server -pthread

# Compile Client
gcc chat_client.c -o chat_client -pthread
```

### 2. Running the Server
Start the server first. It will listen on UDP port **12000** .


```bash
./chat_server
```

### 3. Running Clients
Open multiple terminal windows to simulate different users.

```bash
./chat_client
```

## Command Guide

All commands must follow the format `command$content` (using `$` as the delimiter).

| Command | Usage Example | Description |
| :--- | :--- | :--- |
| **conn** | `conn$Alice` | **Connect** to the server with a username. Must be done first. |
| **say** | `say$Hello World` | **Broadcast** a message to all connected users. |
| **sayto** | `sayto$Bob Hi there` | **Private Message**. Sends "Hi there" only to Bob. |
| **quit** | `quit` | **Exit** the client application locally. |


### Testing Notes for PE 2 (Kick Feature)
To verify the "Kick" feature:
1. Connect a client: `conn$TestUser`.
2. Terminate the client process immediately (Ctrl+C).
3. Watch the Server logs:
   - At **30s**: `SERVER LOG: Sending PING to [TestUser]`
   - At **40s**: `SERVER LOG: Kicking inactive user [TestUser]`
