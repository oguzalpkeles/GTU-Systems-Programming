#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

#define MAX_USERNAME_LEN 16
#define MAX_INPUT_LEN 512
#define MAX_FILE_SIZE 3*1024*1024

int sock;
char username[MAX_USERNAME_LEN];

void save_incoming_file() {
    uint32_t fname_len_net, filesize_net;
    if (recv(sock, &fname_len_net, sizeof(fname_len_net), MSG_WAITALL) <= 0) return;
    uint32_t fname_len = ntohl(fname_len_net);

    char filename[256] = {0};
    if (recv(sock, filename, fname_len, MSG_WAITALL) <= 0) return;

    if (recv(sock, &filesize_net, sizeof(filesize_net), MSG_WAITALL) <= 0) return;
    uint32_t filesize = ntohl(filesize_net);

    time_t now = time(NULL);
    char timestamped_name[300];
    snprintf(timestamped_name, sizeof(timestamped_name), "%ld_%s", now, filename);

    FILE *fp = fopen(timestamped_name, "wb");
    if (!fp) {
        printf("[ERROR] Cannot create file %s\n", timestamped_name);
        return;
    }

    size_t received = 0;
    char buffer[1024];
    while (received < filesize) {
        int to_read = (filesize - received) > sizeof(buffer) ? sizeof(buffer) : (filesize - received);
        int r = recv(sock, buffer, to_read, MSG_WAITALL);
        if (r <= 0) break;
        fwrite(buffer, 1, r, fp);
        received += r;
    }
    fclose(fp);
    printf("[FILE] Received and saved as %s\n", timestamped_name);
}

void *receive_messages(void *arg) {
    char marker[6];
    while (1) {
        memset(marker, 0, sizeof(marker));
        int len = recv(sock, marker, 6, MSG_PEEK);
        if (len <= 0) {
            printf("Disconnected from server.\n");
            exit(0);
        }

        if (len >= 6 && strncmp(marker, "[FILE]", 6) == 0) {
            recv(sock, marker, 6, MSG_WAITALL); // consume marker
            save_incoming_file();
        } else {
            char buffer[1024] = {0};
            len = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (len <= 0) break;
            printf("%s\n", buffer);
        }
    }
    return NULL;
}

void send_file(const char *filename, const char *target) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        printf("[ERROR] File not found.\n");
        return;
    }

    fseek(fp, 0, SEEK_END);
    long filesize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (filesize > MAX_FILE_SIZE) {
        printf("[ERROR] File exceeds 3MB limit.\n");
        fclose(fp);
        return;
    }

    char command[512];
    snprintf(command, sizeof(command), "/sendfile %s %s", filename, target);
    send(sock, command, strlen(command), 0);
    usleep(100000);

    uint32_t size_net = htonl(filesize);
    send(sock, &size_net, sizeof(size_net), 0);

    char buffer[1024];
    size_t sent = 0;
    while (sent < filesize) {
        size_t read = fread(buffer, 1, sizeof(buffer), fp);
        if (read <= 0) break;
        send(sock, buffer, read, 0);
        sent += read;
    }
    fclose(fp);
    printf("[INFO] File sent.\n");
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: ./chatclient <server_ip> <port>\n");
        return 1;
    }

    char *ip = argv[1];
    int port = atoi(argv[2]);

    sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &server_addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        return 1;
    }

    while (1) {
        printf("Enter username: ");
        fgets(username, 100, stdin);
        username[strcspn(username, "\n")] = 0;
        send(sock, username, strlen(username), 0);

        char response[256] = {0};
        recv(sock, response, sizeof(response), 0);
        if (strncmp(response, "[ERROR]", 7) == 0) {
            printf("%s", response);
        } else {
            printf("%s", response);
            break;
        }
    }
    pthread_t recv_thread;
    pthread_create(&recv_thread, NULL, receive_messages, NULL);

    char input[MAX_INPUT_LEN];
    while (1) {
        fgets(input, MAX_INPUT_LEN, stdin);
        input[strcspn(input, "\n")] = 0;
        if (strcmp(input, "/exit") == 0) break;

        if (strncmp(input, "/sendfile ", 10) == 0) {
            char *filename = strtok(input + 10, " ");
            char *target = strtok(NULL, "");
            if (filename && target) send_file(filename, target);
            else printf("[ERROR] Usage: /sendfile <filename> <username>\n");
        } else {
            send(sock, input, strlen(input), 0);
        }
    }

    close(sock);
    return 0;
}
