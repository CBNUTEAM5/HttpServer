#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

#define PORT 8080
#define BUFFER_SIZE 1024

// 로그 파일 및 동기화 도구
FILE *log_file;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

// 로그 기록 함수
void log_message(const char *message) {
    pthread_mutex_lock(&log_mutex);

    // 로그에 시간 정보 추가
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    fprintf(log_file, "[%02d-%02d-%04d %02d:%02d:%02d] %s\n",
            t->tm_mday, t->tm_mon + 1, t->tm_year + 1900,
            t->tm_hour, t->tm_min, t->tm_sec, message);
    fflush(log_file); // 즉시 기록

    pthread_mutex_unlock(&log_mutex);
}

// 클라이언트 요청을 처리하는 함수
void *handle_client(void *arg) {
    int client_socket = *(int *)arg;
    free(arg);

    // 요청 로그 기록
    log_message("Client connected.");

    // HTTP 응답 전송
    const char *response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "Hello, World!";
    ssize_t bytes_written = write(client_socket, response, strlen(response));
    if (bytes_written < 0) {
        perror("Write failed");
        log_message("Error: Failed to send HTTP response.");
    } else {
        log_message("HTTP response sent to client.");
    }

    close(client_socket);
    log_message("Client connection closed.");

    return NULL;
}

int main() {
    int server_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    // 로그 파일 열기
    log_file = fopen("server.log", "a");
    if (!log_file) {
        perror("Failed to open log file");
        exit(EXIT_FAILURE);
    }

    log_message("Server started.");

    // 소켓 생성
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        perror("Socket creation failed");
        log_message("Error: Failed to create socket.");
        fclose(log_file);
        exit(EXIT_FAILURE);
    }

    // 서버 주소 설정
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    // 소켓 바인딩
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("Bind failed");
        log_message("Error: Failed to bind socket.");
        close(server_socket);
        fclose(log_file);
        exit(EXIT_FAILURE);
    }

    // 대기 상태 설정
    if (listen(server_socket, 5) == -1) {
        perror("Listen failed");
        log_message("Error: Failed to listen on socket.");
        close(server_socket);
        fclose(log_file);
        exit(EXIT_FAILURE);
    }

    log_message("Server is running. Waiting for connections...");

    while (1) {
        // 클라이언트 연결 수락
        int *client_socket = malloc(sizeof(int));
        if (!client_socket) {
            perror("Memory allocation failed");
            log_message("Error: Memory allocation failed for client socket.");
            continue;
        }

        *client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_addr_len);
        if (*client_socket == -1) {
            perror("Accept failed");
            log_message("Error: Failed to accept client connection.");
            free(client_socket);
            continue;
        }

        // 새로운 스레드 생성
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, client_socket) != 0) {
            perror("Thread creation failed");
            log_message("Error: Failed to create thread for client.");
            close(*client_socket);
            free(client_socket);
            continue;
        }

        pthread_detach(thread_id); // 스레드가 종료되면 리소스를 자동 해제
    }

    close(server_socket);
    fclose(log_file);

    return 0;
}

