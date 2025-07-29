==============================
 Chat Application (Client/Server)
==============================

Author: [Oğuz Alp Keleş]
Course: [CSE 344] - Systems Programming
Description:
------------
This project implements a multi-client chat system in C using socket programming and POSIX threads. It includes:
- Room joining and leaving
- Broadcast messaging
- Private whispers
- File transfer between users with queuing and logging
- Server logging and graceful shutdown

===============================
TO RUN THE CODE
===============================

Use the make file.

===============================
 USAGE INSTRUCTIONS
===============================

Step 1: Start the server
-------------------------
Run the server by specifying a port number:
    ./chatserver <port> 

Example:
    ./chatserver 12345

Step 2: Start the client(s)
----------------------------
Run the client by providing the server's IP address (loopback e.g.) and the same port:
    ./chatclient <server_ip> <port>

Example (on same machine):
    ./chatclient 127.0.0.1 12345

Step 3: Interact using commands
-------------------------------

After providing a unique username, you can use the following commands:

- `/join <roomname>`  
  Join or switch to a chat room

- `/leave`  
  Leave the current room

- `/broadcast <message>`  
  Send a message to all users in your current room

- `/whisper <username> <message>`  
  Send a private message to a specific user

- `/sendfile <filename> <username>`  
  Send a file (max 3MB) to another user.  
  Files are received with a timestamped filename.

- `/exit`  
  Exit the chat client

===============================
 NOTES
===============================

- Log file `example_log.txt` records server events like logins, file transfers, room changes, etc.
- File transfer queue allows only 5 concurrent uploads; others wait in a queue.
- Received files are saved with a timestamp prefix in the working directory.
- The server supports up to 30 concurrent clients.

===============================
 LICENSE
===============================

This code was developed as part of an undergraduate Systems Programming course.  
It is shared for learning purposes only.  
DO NOT submit this work as your own in any academic environment.

===============================
