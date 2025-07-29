#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>

#define SERVER_FIFO "server_fifo"
#define CLIENT_FIFO_TEMPLATE "client_fifo_%d"
#define CLIENT_FIFO_NAME_LEN 64

// Structures

// This is used when client first send a connection request to the server.
typedef struct
{
    pid_t client_pid;
    char client_fifo[CLIENT_FIFO_NAME_LEN];
} Server_Connection_Request;

// This is used to hold actual request information to be sent to a teller after connection to server is acceppted.
typedef struct
{
    pid_t client_pid;
    char account_id[20];
    char operation[10];
    int amount;
    int possible_request;
} Request;

// A Simple message structure to hold response returned from the teller.
typedef struct
{
    char message[100];
} Response;

// Explanations for functions are under main where definitions are done.
void printMsg(char operations[][10], int amounts[], int client_num);
void cleanup_fifo(int sig);
void readClientFile(const char *filename, char bank_ids[][20], char operations[][10], int amounts[], int *line_count);
void setup_sigaction(int signum, void (*handler)(int));

//This function takes client's file name as argument.
int main(int argc, char *argv[])
{
    //Argument check. 
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <client_file>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Data structures related to client requests
    char bank_ids[100][20];
    char operations[100][10];
    int amounts[100];
    int line_count;

    // Read the clients file to obtain the requests.
    readClientFile(argv[1], bank_ids, operations, amounts, &line_count);
    printf("Reading clients.file... Found %d requests.\n", line_count);

    // Handle signals when delivered clean the resources such as fifos created.
    setup_sigaction(SIGINT, cleanup_fifo);
    setup_sigaction(SIGTERM, cleanup_fifo);


    // For conquerrency, create a child for each request so that they can be handled conquerrently.
    for (int i = 0; i < line_count; i++)
    {
        if (fork() == 0)
        {
            // Create client FIFO, each specific to a client (using pid).
            char client_fifo[CLIENT_FIFO_NAME_LEN];
            pid_t pid = getpid();
            snprintf(client_fifo, CLIENT_FIFO_NAME_LEN, CLIENT_FIFO_TEMPLATE, pid);
            if (mkfifo(client_fifo, 0666) == -1 && errno != EEXIST)
            {
                perror("mkfifo failed");
                exit(EXIT_FAILURE);
            }

            // Child process handles one request
            Server_Connection_Request sc_request;
            sc_request.client_pid = pid;
            strcpy(sc_request.client_fifo, client_fifo);

            // Send connection request first to bank server.
            int server_fd = open(SERVER_FIFO, O_WRONLY);
            if (server_fd == -1)
            {
                printf("Client %d cannot connect to server FIFO\n", i);
                unlink(client_fifo); // clean up FIFO on failure
                exit(EXIT_FAILURE);
            }
            printMsg(operations, amounts, i);
            write(server_fd, &sc_request, sizeof(sc_request));
            close(server_fd);

            // Send actual transaction request to teller.(Create a request structure)
            Request request;
            request.client_pid = pid;
            strcpy(request.account_id, bank_ids[i]);
            strcpy(request.operation, operations[i]);
            request.amount = amounts[i];
            request.possible_request = 0; // Initialize it to zero, if transaction is possible, teller will change it to 1.

            int client_fd = open(client_fifo, O_WRONLY);
            if (client_fd == -1)
            {
                perror("Failed to open client FIFO for writing");
                exit(EXIT_FAILURE);
            }
            write(client_fd, &request, sizeof(Request));
            close(client_fd);

            // Read the response from the teller.
            client_fd = open(client_fifo, O_RDONLY);
            if (client_fd == -1)
            {
                perror("Failed to open client FIFO for reading");
                exit(EXIT_FAILURE);
            }

            Response response;
            read(client_fd, &response, sizeof(Response));
            close(client_fd);

            printf("Response from server for request %d: %s\n", i + 1, response.message);
            unlink(client_fifo);
            exit(0);
        }
    }

    // Parent waits for all child processes
    for (int i = 0; i < line_count; i++)
    {
        wait(NULL);
    }
    return 0;
}

// This functions prints the message in the client side.
void printMsg(char operations[][10], int amounts[], int client_num)
{
    printf("Client0%d connected..%sing %d credits\n", client_num, operations[client_num], amounts[client_num]);
}

// This function cleans up the resourdes if SIGINT or SIGTERM is delivered.
void cleanup_fifo(int sig)
{
    char fifo[64];
    snprintf(fifo, sizeof(fifo), CLIENT_FIFO_TEMPLATE, getpid());
    unlink(fifo);
    exit(1);
}

// Function to read the client file into arrays
void readClientFile(const char *filename, char bank_ids[][20], char operations[][10], int amounts[], int *line_count)
{
    int fd = open(filename, O_RDONLY);
    if (fd == -1)
    {
        perror("open failed");
        exit(EXIT_FAILURE);
    }

    char buffer[4096];
    ssize_t bytes_read = read(fd, buffer, sizeof(buffer));
    if (bytes_read < 0)
    {
        perror("read failed");
        close(fd);
        exit(EXIT_FAILURE);
    }
    buffer[bytes_read] = '\0';

    char *line = strtok(buffer, "\n");
    *line_count = 0;

    while (line != NULL)
    {
        if (sscanf(line, "%s %s %d", bank_ids[*line_count], operations[*line_count], &amounts[*line_count]) == 3)
        {
            (*line_count)++;
        }
        else
        {
            fprintf(stderr, "Invalid format in line: %s\n", line);
        }
        line = strtok(NULL, "\n");
    }

    close(fd);
}

//Sets up the signals
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