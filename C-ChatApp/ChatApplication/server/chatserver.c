#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <signal.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <stdarg.h>
#include <errno.h>

#define MAX_CLIENTS 30
#define MAX_USERNAME_LEN 16
#define MAX_ROOM_NAME 32
#define MAX_MESSAGE_LEN 512
#define MAX_FILE_SIZE 3 * 1024 * 1024
#define LOG_FILE "example_log.txt"

typedef struct
{
    int socket;
    char username[MAX_USERNAME_LEN];
    char room[MAX_ROOM_NAME];
} Client;

Client clients[MAX_CLIENTS];
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
FILE *log_file;
sem_t *file_transfer_sem;  
sig_atomic_t counter = 0;
void log_action(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    fprintf(log_file, "%04d-%02d-%02d %02d:%02d:%02d - ",
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
            t->tm_hour, t->tm_min, t->tm_sec);
    vfprintf(log_file, format, args);
    fprintf(log_file, "\n");
    fflush(log_file);
    va_end(args);
}

int is_username_taken(const char *username)
{
    for (int i = 0; i < MAX_CLIENTS; ++i)
    {
        if (clients[i].socket != 0 && strcmp(clients[i].username, username) == 0)
        {
            return 1;
        }
    }
    return 0;
}

void send_to_client(int sock, const char *message)
{
    send(sock, message, strlen(message), 0);
}

void broadcast_message(const char *room, const char *message, const char *sender)
{
    for (int i = 0; i < MAX_CLIENTS; ++i)
    {
        if (clients[i].socket != 0 && strcmp(clients[i].room, room) == 0 && strcmp(clients[i].username, sender) != 0)
        {
            send_to_client(clients[i].socket, message);
        }
    }
}

void remove_client(int sock)
{
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; ++i)
    {
        if (clients[i].socket == sock)
        {
            log_action("[DISCONNECT] user '%s' lost connection. Cleaned up the resources.", clients[i].username);
            printf("[DISCONNECT] user '%s' lost connection.Cleaned up the resources.\n", clients[i].username);
            fflush(stdout);
            clients[i].socket = 0;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    close(sock);
}

void *handle_client(void *arg)
{
    int sock = *(int *)arg;
    char buffer[1024], username[MAX_USERNAME_LEN];
    char temp[100];
    while (1)
    {
        memset(temp, 0, sizeof(temp));
        int len = recv(sock, temp, sizeof(temp) - 1, 0);
        if (len <= 0) {
            close(sock);
            return NULL;
        }
        temp[len] = '\0';
        if (strcspn(temp, "\n") >= MAX_USERNAME_LEN - 1) {
            send_to_client(sock, "[ERROR] Username exceeds the size limit. Try a shorter one: ");
        }
        else{
            strncpy(username, temp, MAX_USERNAME_LEN - 1);
            username[MAX_USERNAME_LEN - 1] = '\0';
            username[strcspn(username, "\n")] = 0;

            pthread_mutex_lock(&clients_mutex);
            int taken = is_username_taken(username);
            pthread_mutex_unlock(&clients_mutex);

            if (taken) {
                send_to_client(sock, "[ERROR] Username already taken. Try another: ");
                log_action("[REJECTED] Duplicate user name attempted: %s", username);
                printf("[REJECTED] Duplicate user name attempted: %s\n", username);
                fflush(stdout);
            } else {
                pthread_mutex_lock(&clients_mutex);
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    if (clients[i].socket == 0) {
                        clients[i].socket = sock;
                        strncpy(clients[i].username, username, MAX_USERNAME_LEN);
                        clients[i].room[0] = '\0';
                        break;
                    }
                }
                pthread_mutex_unlock(&clients_mutex);
                break;
            }  
        }      
    }

    pthread_mutex_unlock(&clients_mutex);

    log_action("[LOGIN] user '%s' connected", username);
    printf("[LOGIN] user '%s' connected\n", username);
    fflush(stdout);
    send_to_client(sock, "[INFO] Connected.\n");

    while (1)
    {
        memset(buffer, 0, sizeof(buffer));
        int len = recv(sock, buffer, sizeof(buffer), 0);
        if (len <= 0)
            break;
        buffer[strcspn(buffer, "\n")] = 0;

        if (strncmp(buffer, "/sendfile ", 10) == 0) {
            
            char *filename = strtok(buffer + 10, " ");
            char *target = strtok(NULL, "");
            if (!filename || !target) {
                send_to_client(sock, "[ERROR] Usage: /sendfile <filename> <username>\n");
                continue;
            }
            printf("[INFO] '%s' initiated file transfer to '%s'\n", username, target);
            fflush(stdout);
            int receiver_sock = 0;
            pthread_mutex_lock(&clients_mutex);
            for (int i = 0; i < MAX_CLIENTS; ++i) {
                if (clients[i].socket != 0 && strcmp(clients[i].username, target) == 0) {
                    receiver_sock = clients[i].socket;
                    break;
                }
            }
            pthread_mutex_unlock(&clients_mutex);
            if (!receiver_sock) {
                send_to_client(sock, "[ERROR] User not found.\n");
                continue;
            }

            int acquired = 0;
            errno = 0;
            if (sem_trywait(file_transfer_sem) == 0) {
                acquired = 1;
                send_to_client(sock, "[INFO] Upload started.\n");
                //sleep(20);
            } else if (errno == EAGAIN) {
                char msg[256];
                snprintf(msg, sizeof(msg), "[FILE-QUEUE] Upload '%s' from %s added to queue. Queue size: 5\n", filename, username);
                send_to_client(sock, msg);
                log_action("[FILE-QUEUE] Upload '%s' from %s added to queue. Queue size: 5", filename, username);

                time_t wait_start = time(NULL);  // Start measuring wait time
                sem_wait(file_transfer_sem);     // Block until a slot becomes available
                time_t wait_end = time(NULL);    // End wait time

                int wait_time = (int)(wait_end - wait_start);
                char info_msg[128];
                snprintf(info_msg, sizeof(info_msg), "[INFO] Upload started after waiting %d seconds in queue.\n", wait_time);
                send_to_client(sock, info_msg);

                log_action("[FILE-QUEUE] '%s' from %s started upload after waiting %d seconds", filename, username, wait_time);
                acquired = 1;
            } else {
                send_to_client(sock, "[ERROR] Internal server error during file queue.\n");
                continue;
            }

            uint32_t filesize;
            recv(sock, &filesize, sizeof(filesize), 0);
            filesize = ntohl(filesize);
            if (filesize > MAX_FILE_SIZE) {
                send_to_client(sock, "[ERROR] File exceeds 3MB.\n");
                log_action("[ERROR] File exceeds 3MB.");
                printf("[ERROR] File exceeds 3MB.");
                fflush(stdout);
                if (acquired) sem_post(file_transfer_sem);
                continue;
            }

            uint32_t fname_len = strlen(filename);
            uint32_t fname_len_net = htonl(fname_len);
            send(receiver_sock, "[FILE]", 6, 0);
            send(receiver_sock, &fname_len_net, sizeof(fname_len_net), 0);
            send(receiver_sock, filename, fname_len, 0);

            uint32_t size_net = htonl(filesize);
            send(receiver_sock, &size_net, sizeof(size_net), 0);

            char file_buffer[1024];
            size_t received = 0;
            while (received < filesize) {
                int to_read = (filesize - received) > sizeof(file_buffer) ? sizeof(file_buffer) : (filesize - received);
                int r = recv(sock, file_buffer, to_read, 0);
                if (r <= 0) break;
                send(receiver_sock, file_buffer, r, 0);
                received += r;
            }

            if (acquired) sem_post(file_transfer_sem);

            char notify[256];
            snprintf(notify, sizeof(notify), "[INFO] File '%s' sent to %s.\n", filename, target);
            send_to_client(sock, notify);
            log_action("[SEND FILE] '%s' sent from %s to %s", filename, username, target);
        }
        else if (strncmp(buffer, "/join ", 6) == 0)
        {
            char *room = buffer + 6;
            pthread_mutex_lock(&clients_mutex);
            for (int i = 0; i < MAX_CLIENTS; ++i)
            {
                if (clients[i].socket == sock)
                {   char old_room[100];
                    if(clients[i].room[i] != '\0'){ // It means rejoin
                        char msg[128];
                        strncpy(old_room, clients[i].room, MAX_ROOM_NAME);
                        snprintf(msg, sizeof(msg), "[ROOM] User '%s' left room '%s', joined '%s'", old_room ,clients[i].room, room);
                        strncpy(clients[i].room, room, MAX_ROOM_NAME);
                        char join_msg[128];
                        snprintf(join_msg, sizeof(join_msg), "[JOINED] You joined room '%s'\n", room);
                        send_to_client(sock, join_msg);
                        log_action("[ROOM] User '%s' left room '%s', joined '%s'", clients[i].username ,old_room, room);
                        printf("%s\n", msg);
                        fflush(stdout);
                        break;
                    }
                    else{ // Normal join logic and printing
                        strncpy(clients[i].room, room, MAX_ROOM_NAME);
                        char join_msg[128];
                        snprintf(join_msg, sizeof(join_msg), "[JOINED] You joined room '%s'\n", room);
                        send_to_client(sock, join_msg);
                        log_action("[JOIN] user '%s' joined room '%s'", clients[i].username, room);
                        printf("[JOIN] user '%s' joined room '%s'\n", clients[i].username, room);
                        fflush(stdout);
                        break;
                    }
                    
                }
            }
            pthread_mutex_unlock(&clients_mutex);
        }
        else if (strncmp(buffer, "/broadcast ", 11) == 0)
        {
            char *msg = buffer + 11;
            pthread_mutex_lock(&clients_mutex);
            for (int i = 0; i < MAX_CLIENTS; ++i)
            {
                if (clients[i].socket == sock)
                {
                    if (strlen(clients[i].room) == 0)
                    {
                        send_to_client(sock, "[ERROR] Join a room first using /join <room>.\n");
                        break;
                    }
                    char fullmsg[512];
                    snprintf(fullmsg, sizeof(fullmsg), "[%s] %s\n", clients[i].username, msg);
                    broadcast_message(clients[i].room, fullmsg, clients[i].username);
                    log_action("[BROADCAST] user '%s': %s", clients[i].username, msg);
                    printf("[BROADCAST] user '%s': %s\n", clients[i].username, msg);
                    fflush(stdout);
                    break;
                }
            }
            pthread_mutex_unlock(&clients_mutex);
        }
        else if (strcmp(buffer, "/leave") == 0)
        {
            pthread_mutex_lock(&clients_mutex);
            for (int i = 0; i < MAX_CLIENTS; ++i)
            {
                if (clients[i].socket == sock)
                {   
                    if(clients[i].room[0] != '\0'){ // If already inside a room
                        log_action("[ROOM] user '%s': left room %s", clients[i].username, clients[i].room);
                        printf("[ROOM] user '%s': left room %s\n", clients[i].username, clients[i].room);
                        fflush(stdout);
                        clients[i].room[0] = '\0';
                        send_to_client(sock, "[INFO] You have left the room.\n");
                        break;
                    }
                    else{ // If not in any room
                        log_action("[ROOM] user '%s': attempt to leave room when it is in no room", clients[i].username);
                        printf("[ROOM] user '%s': attempt to leave room when it is in no room\n", clients[i].username);
                        fflush(stdout);
                        clients[i].room[0] = '\0';
                        send_to_client(sock, "[INFO] You are not in any room.\n");
                        break;
                    }
                    
                }
            }
            pthread_mutex_unlock(&clients_mutex);
        }
        else if (strncmp(buffer, "/whisper ", 9) == 0)
        {
            char *target = strtok(buffer + 9, " ");
            char *msg = strtok(NULL, "");
            if (!target || !msg)
            {
                send_to_client(sock, "[ERROR] Usage: /whisper <username> <message>\n");
            }
            else
            {
                int found = 0;
                pthread_mutex_lock(&clients_mutex);
                for (int i = 0; i < MAX_CLIENTS; ++i)
                {
                    if (clients[i].socket != 0 && strcmp(clients[i].username, target) == 0)
                    {
                        char priv_msg[512];
                        snprintf(priv_msg, sizeof(priv_msg), "[WHISPER] %s: %s\n", username, msg);
                        send_to_client(clients[i].socket, priv_msg);
                        send_to_client(sock, "[INFO] Whisper sent.\n");
                        log_action("[WHISPER] from '%s' to '%s': %s", username, target, msg);
                        printf("[WHISPER] from '%s' to '%s': %s\n", username, target, msg);
                        fflush(stdout);
                        found = 1;
                        break;
                    }
                }
                pthread_mutex_unlock(&clients_mutex);
                if (!found)
                    send_to_client(sock, "[ERROR] User not found.\n");
            }
        }
        else
        {
            send_to_client(sock, "[ERROR] Unknown command.\n");
        }
    }

    remove_client(sock);
    return NULL;
}

void sigint_handler(int sig)
{
    for (int i = 0; i < MAX_CLIENTS; ++i)
    {
        if (clients[i].socket != 0)
        {
            counter = counter + 1;
            send_to_client(clients[i].socket, "[SERVER SHUTDOWN]\n");
            close(clients[i].socket);
        }
    }

    log_action("[SHUTDOWN] SIGINT received. Disconnecting %d clients, saving logs.", counter);
    //write(STDOUT_FILENO, "[SHUTDOWN] SIGINT received. Disconnecting %d clients, saving logs.", strlen("[SHUTDOWN] SIGINT received. Disconnecting %d clients, saving logs."));
    //fflush(stdout);
    sem_close(file_transfer_sem);
    sem_unlink("/file_transfer_sem");  // Removes the named semaphore
    fclose(log_file);
    exit(0);
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        printf("Usage: ./chatserver <port>\n");
        exit(1);
    }

    log_file = fopen(LOG_FILE, "w");
    signal(SIGINT, sigint_handler);
    
    // Create/open named semaphore with initial value 5
    file_transfer_sem = sem_open("/file_transfer_sem", O_CREAT | O_EXCL, 0644, 5);
    
    if (file_transfer_sem == SEM_FAILED) {
        // If semaphore already exists, try to open it
        file_transfer_sem = sem_open("/file_transfer_sem", 0);
        if (file_transfer_sem == SEM_FAILED) {
            perror("sem_open failed");
            exit(1);
        }
    }


    int port = atoi(argv[1]);
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    listen(server_fd, MAX_CLIENTS);
    printf("[INFO] Server listening on port %d...\n", port);
    fflush(stdout);
    while (1)
    {
        struct sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        int client_sock = accept(server_fd, (struct sockaddr *)&client_addr, &len);
        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, (void *)&client_sock) == 0) {
            pthread_detach(tid);  // detach the thread
        } else {
            perror("[ERROR] Failed to create thread");
            close(client_sock);   // Clean up socket if thread creation fails
        }

    }
    
    sem_close(file_transfer_sem);
    sem_unlink("/file_transfer_sem");  // Removes the named semaphore
    return 0;
}
