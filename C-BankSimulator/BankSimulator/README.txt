===============================
  Systems Programming Project
  Bank Server Simulation
===============================

Author   : Oğuz Alp Keleş  
Course   : Systems Programming  

===============================
  Project Overview
===============================

This project simulates a multi-process **banking server system** where clients can:
- Create new bank accounts
- Deposit into existing accounts
- Withdraw money from accounts

Key system programming concepts demonstrated include:
- Named FIFOs (client-server communication)
- Shared memory segments (account database)
- POSIX semaphores (data consistency)
- Forked processes (tellers)
- Inter-process communication via pipes
- Signal handling and cleanup
- Log file and database persistence


===============================
  To Compile The Project
===============================

Use makefile.

===============================
  How to Run the Server
===============================

Start the server with:

    ./bankserver

It will:
- Load existing `database.txt` if available.
- Listen for client connections via `server_fifo`.
- Create tellers for each client request.
- Manage requests via shared memory and log all successful actions.

The server only terminates with `CTRL+C`. This triggers:
- Writing the updated database to `database.txt`
- Closing all FIFOs, shared memory, and semaphores
- Killing all child processes
- Finalizing the log file


===============================
    Academic Honesty
===============================

This project was completed for educational and learning purposes only.  
Do not copy or submit this code as your own.

===============================
