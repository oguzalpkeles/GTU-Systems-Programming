#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <semaphore.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

#define SERVER_FIFO "server_fifo"
#define CLIENT_FIFO_TEMPLATE "client_fifo_%d"
#define CLIENT_FIFO_NAME_LEN 64
#define REQUEST_PIPE "teller_to_server.pipe"
#define MAX_BUFFER 256
#define SHM_KEY 1234
#define SEM_NAME "/bank_semaphore"
#define DB_FILE "database.txt"
#define LOG_FILE "AdaBank.bankLog"
#define MAX_ACCOUNTS 100

// Structures

// This is used to store information related to a bank account.
typedef struct
{
    char account_id[20];
    int balance;
} Account;

// These structures are the same ones as in client.c, for explanation please refer to that file.
typedef struct
{
    pid_t client_pid;
    char client_fifo[CLIENT_FIFO_NAME_LEN];
} Server_Connection_Request;

typedef struct
{
    pid_t client_pid;
    char account_id[20];
    char operation[10];
    int amount;
    int possible_request;
} Request;

typedef struct
{
    char message[100];
} Response;

// This is used for communication between tellers and the bank server as shared memory as required in the homework.
typedef struct
{
    Account accounts[MAX_ACCOUNTS];
    int db_size;
    int next_id;
} SharedData;

// Some globals to be used throughout the program.
// This will refer to the shared data.
SharedData *shared_data;
// This semaphore will be used to ensure that no race conditions occur on operations regarding the database.
sem_t *db_semaphore;
int shm_id;
// Handler pid, usage is explained inside handler function.
pid_t handler_pid;

// Explanations for functions are under main where definitions are done.
void init_log_file();
void log_transaction(const char *account_id, const char *operation, int amount);
void finalize_log_file();
void load_database_from_file();
void save_database_to_file(int sig);
int update_database(const char *account_id, const char *operation, int amount, int possible_request, char *response);
void *func(void *arg);
pid_t Teller(void *func, void *arg_func);
int waitTeller(pid_t pid, int *status);
void setup_sigaction(int signum, void (*handler)(int));

int main()
{
    // Handle termination signal which is "the only way" to terminate the bank server.
    setup_sigaction(SIGINT, save_database_to_file);
    // Initialize the log file, write the time stamp when it is updated.
    init_log_file();

    // Create server fifo (between server and client) and request pipe (between tellers and server)
    mkfifo(SERVER_FIFO, 0666);
    mkfifo(REQUEST_PIPE, 0666);
    printf("Creating the bank database...\n");
    printf("Adabank is active... Waiting for clients at %s\n", SERVER_FIFO);

    // Create the shared memory. These are used as shared memory because they should be consistent in all executions.
    shm_id = shmget(SHM_KEY, sizeof(SharedData), IPC_CREAT | 0666);
    if (shm_id < 0)
    {
        perror("shmget error");
        exit(1);
    }
    shared_data = (SharedData *)shmat(shm_id, NULL, 0);
    if (shared_data == (void *)-1)
    {
        perror("shmat failed");
        exit(1);
    }

    // Initialize database size to be zero first (it is determined by the line count in the database file later).
    shared_data->db_size = 0;
    // next_id variable is used to assign new account id's.
    shared_data->next_id = 1;

    // Load the existing database.
    load_database_from_file();
    shared_data->next_id = shared_data->db_size + 1;

    // Initialize the semaphore that will protect the data in the database.
    db_semaphore = sem_open(SEM_NAME, O_CREAT, 0666, 1);
    if (db_semaphore == SEM_FAILED)
    {
        perror("sem_open");
        exit(1);
    }

    // To help concurrency, server forks a handler that will listen and do the updates to the database so that server can continue accepting requests.
    handler_pid = fork();
    if (handler_pid == 0)
    {
        // Update the handler behaviour to be the default inside handler
        // because otherwise when server is terminated via ctrl+c, signal handler is executed twice
        // which is not what I want.
        setup_sigaction(SIGINT, SIG_DFL);

        // Listen appropriate requets from teller, and update the database.
        int pipe_fd = open(REQUEST_PIPE, O_RDONLY);
        while (1)
        {
            Request req;
            if (read(pipe_fd, &req, sizeof(Request)) > 0)
            {
                char response_msg[100];
                // Protect the database operation using a semaphore. (Detailed discussion is in the report)
                sem_wait(db_semaphore);
                update_database(req.account_id, req.operation, req.amount, req.possible_request, response_msg);
                sem_post(db_semaphore);
            }
        }
        close(pipe_fd);
        exit(0);
    }

    // Open server fifo to listen server connection requests.
    int server_fd = open(SERVER_FIFO, O_RDONLY);
    if (server_fd == -1)
    {
        perror("Server FIFO open");
        exit(EXIT_FAILURE);
    }

    // Server listens server connection requests. This is the main server loop.
    // ! Server can only be stopped using ctrl+c (SIGTERM) signal, otherwise will run forever !
    while (1)
    {
        Server_Connection_Request sc_request;
        if (read(server_fd, &sc_request, sizeof(sc_request)) > 0)
        {
            Server_Connection_Request *req_copy = malloc(sizeof(Server_Connection_Request));
            memcpy(req_copy, &sc_request, sizeof(Server_Connection_Request));
            // Call the teller function to fork child processes, just as required in the homework document.
            pid_t pid = Teller(func, req_copy);
            int status;
            // Wait the child processes to clean up the resources.
            waitTeller(pid, &status);
            free(req_copy);
        }
    }

    // All this resource cleaning is done when server is delivered SIGTERM signal which is the only way to stop it.
    /* close(server_fd);
    unlink(SERVER_FIFO);
    unlink(REQUEST_PIPE);
    sem_close(db_semaphore);
    sem_unlink(SEM_NAME);
    shmdt(shared_data);
    shmctl(shm_id, IPC_RMID, NULL); */
    return 0;
}

// Initializes the log file, creating it and writing timestamp and some message to it.
void init_log_file()
{
    FILE *log = fopen(LOG_FILE, "w");
    if (!log)
    {
        perror("Could not open log file");
        return;
    }
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%H:%M %B %d %Y", tm_info);
    fprintf(log, "# Adabank Log file updated @%s\n", time_str);
    fclose(log);
}

// This function is used to create a log message which corresponds to a line in the log file.
void log_transaction(const char *account_id, const char *operation, int amount)
{
    FILE *log = fopen(LOG_FILE, "a");
    if (!log)
        return;
    fprintf(log, "%s %c %d\n", account_id, (operation[0] == 'd' || operation[0] == 'D') ? 'D' : 'W', amount);
    fclose(log);
}

// It prints the end statmenet for the log file and closes it.
void finalize_log_file()
{
    FILE *log = fopen(LOG_FILE, "a");
    if (log)
    {
        fprintf(log, "## end of log.\n");
        fclose(log);
    }
}

// This function reads a pre-existing database file to fill up a datastructure referring to db in the server.
void load_database_from_file()
{
    FILE *file = fopen(DB_FILE, "r");
    if (!file)
    {
        perror("Could not open database.txt");
        return;
    }
    char id[20];
    int amount;
    while (fscanf(file, "%s %d", id, &amount) == 2 && shared_data->db_size < MAX_ACCOUNTS)
    {
        strcpy(shared_data->accounts[shared_data->db_size].account_id, id);
        shared_data->accounts[shared_data->db_size].balance = amount;
        shared_data->db_size++;
    }
    fclose(file);
}

// This function writes the current database stored in the datastructures in the server to database file.
// It is also the signal handler for sigterm, so all the cleanup's are done here.
void save_database_to_file(int sig)
{
    printf("\nSignal received, cleaning up and closing active tellers.\n");
    printf("Adabank says “Bye”...\n");
    FILE *file = fopen(DB_FILE, "w");
    if (!file)
    {
        perror("Could not write to database.txt");
        exit(1);
    }
    for (int i = 0; i < shared_data->db_size; i++)
    {
        fprintf(file, "%s %d\n", shared_data->accounts[i].account_id, shared_data->accounts[i].balance);
    }
    // Clean up all the resources.

    // Kill all child processes (tellers + handler)
    signal(SIGTERM, SIG_IGN);   // Ignore SIGTERM for ourselves because we already received one.
    killpg(getpgrp(), SIGTERM); // Kill all tellers
    fclose(file);
    finalize_log_file();
    // handler_pid is used here to kill the handler when server dies in order to avoid orphaned process.
    kill(handler_pid, SIGINT);
    sem_close(db_semaphore);
    sem_unlink(SEM_NAME);
    shmdt(shared_data);
    shmctl(shm_id, IPC_RMID, NULL);
    unlink(SERVER_FIFO);
    unlink(REQUEST_PIPE);
    exit(0);
}

// This function updates the database according to request arrived.
// It is only called from server-handler, so database is updated only by server, as required in the homework document.
int update_database(const char *account_id, const char *operation, int amount, int possible_request, char *response)
{
    if (possible_request == 1)
    {
        if (strcmp(account_id, "N") == 0)
        {
            char new_id[20];
            int id_counter = shared_data->next_id;
            while (1)
            {
                snprintf(new_id, sizeof(new_id), "BankID_%02d", id_counter);

                int exists = 0;
                for (int i = 0; i < shared_data->db_size; i++)
                {
                    if (strcmp(shared_data->accounts[i].account_id, new_id) == 0)
                    {
                        exists = 1;
                        break;
                    }
                }

                if (!exists)
                    break; // Found a unique BankID

                id_counter++; // Try next ID
            }

            strcpy(shared_data->accounts[shared_data->db_size].account_id, new_id);
            shared_data->accounts[shared_data->db_size].balance = amount;
            log_transaction(new_id, operation, amount);
            snprintf(response, 100, "New account %s created with balance %d", new_id, amount);
            printf("%s\n", response);
            shared_data->db_size++;
            shared_data->next_id = id_counter + 1;
            return 1;
        }

        for (int i = 0; i < shared_data->db_size; i++)
        {
            if (strcmp(shared_data->accounts[i].account_id, account_id) == 0)
            {
                if (strcmp(operation, "withdraw") == 0)
                {
                    if (shared_data->accounts[i].balance >= amount)
                    {
                        shared_data->accounts[i].balance -= amount;
                        log_transaction(account_id, operation, amount);
                        if (shared_data->accounts[i].balance == 0)
                        {
                            for (int j = i; j < shared_data->db_size - 1; j++)
                            {
                                shared_data->accounts[j] = shared_data->accounts[j + 1];
                            }
                            shared_data->db_size--;
                            snprintf(response, 100, "Withdrawal successful. Account %s removed.", account_id);
                            printf("%s\n", response);
                        }
                        else
                        {
                            snprintf(response, 100, "%s Withdrawal successful. Remaining balance: %d", account_id, shared_data->accounts[i].balance);
                            printf("%s\n", response);
                        }
                        return 1;
                    }
                    else
                    {
                        snprintf(response, 100, "%s Insufficient balance.", account_id);
                        printf("%s\n", response);
                        return 0;
                    }
                }
                else if (strcmp(operation, "deposit") == 0)
                {
                    shared_data->accounts[i].balance += amount;
                    log_transaction(account_id, operation, amount);
                    snprintf(response, 100, "%s Deposit successful. New balance: %d", account_id, shared_data->accounts[i].balance);
                    printf("%s\n", response);
                    return 1;
                }
            }
        }
        snprintf(response, 100, "Account not found.");
        printf("%s\n", response);
        return 0;
    }
    snprintf(response, 100, "Invalid request for %s", account_id);
    printf("%s\n", response);
    return 0;
}

// This is the function that teller forks which handels withdrawig and depositing operations.
// Reading operations regarding the shared memory are again protected with semaphores!
void *func(void *arg)
{
    // Read actual request from the client.
    Server_Connection_Request *sc_request = (Server_Connection_Request *)arg;
    Request request;

    int fd = open(sc_request->client_fifo, O_RDONLY);
    if (fd == -1)
    {
        perror("Teller open read");
        unlink(sc_request->client_fifo);
        exit(EXIT_FAILURE);
    }
    if (read(fd, &request, sizeof(Request)) <= 0)
    {
        perror("Teller read request");
        unlink(sc_request->client_fifo);
        close(fd);
        exit(EXIT_FAILURE);
    }
    close(fd);

    request.possible_request = 0;

    // Critical section starts, while reading data integrity should be ensured.
    sem_wait(db_semaphore);

    // Make integrity checks regarding if the request can be implemented or not and set variable possible_request accordingly.
    if (strcmp(request.account_id, "N") == 0 && strcmp(request.operation, "deposit") == 0)
    {
        request.possible_request = 1;
    }
    else
    {
        for (int i = 0; i < shared_data->db_size; i++)
        {
            if (strcmp(shared_data->accounts[i].account_id, request.account_id) == 0)
            {
                if ((strcmp(request.operation, "withdraw") == 0 && shared_data->accounts[i].balance >= request.amount) ||
                    strcmp(request.operation, "deposit") == 0)
                {
                    request.possible_request = 1;
                }
                break;
            }
        }
    }

    sem_post(db_semaphore);
    // Critical section for reading ends.

    // Send the possible request to server for database update.
    int pipe_fd = open(REQUEST_PIPE, O_WRONLY);
    if (pipe_fd == -1)
    {
        perror("Teller pipe open failed");
        exit(EXIT_FAILURE);
    }
    write(pipe_fd, &request, sizeof(Request));
    close(pipe_fd);

    // Send the response regarding the result of the operation to the client back.
    Response response;
    if (request.possible_request == 0)
    {
        if (strcmp(request.operation, "withdraw") == 0)
            strcpy(response.message, "Insufficient balance or invalid account.");
        else if (strcmp(request.operation, "deposit") == 0)
            strcpy(response.message, "Invalid account.");
        else
            strcpy(response.message, "Invalid operation.");
    }
    else
    {
        strcpy(response.message, "Request accepted and being processed.");
    }

    int client_fd = open(sc_request->client_fifo, O_WRONLY);
    if (client_fd != -1)
    {
        write(client_fd, &response, sizeof(Response));
        close(client_fd);
    }

    exit(EXIT_SUCCESS);
    return NULL;
}

// This function is requried in the homework document, this forks a child teller that will execute the code specified in its argument.
pid_t Teller(void *func, void *arg_func)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        printf("\nTeller PID%d is active serving the client.\n", getpid());
        ((void *(*)(void *))func)(arg_func);
        exit(EXIT_SUCCESS);
    }
    return pid;
}

// This function is requried in the homework document, and used to wait the child processes.
int waitTeller(pid_t pid, int *status)
{
    return waitpid(pid, status, 0);
}

// Sets up the signals
void setup_sigaction(int signum, void (*handler)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // default options

    if (sigaction(signum, &sa, NULL) == -1)
    {
        perror("sigaction failed");
        exit(EXIT_FAILURE);
    }
}