#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>

#define PORT 8080
#define BUFFER_SIZE 1024
#define THREAD_POOL_SIZE 4
#define ROOT_DIR "./www" // 정적 파일 루트 디렉토리
#define MAX_EVENTS 10

// Epoll 이벤트 구조체
struct epoll_event ev, events[MAX_EVENTS];

// 전역 변수 및 파일 핸들러
FILE *log_file;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;

long total_requests = 0;
double total_response_time = 0;
long active_connections = 0;

// 로그 기록 함수
void log_message(const char *message) {
    pthread_mutex_lock(&log_mutex);
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    fprintf(log_file, "[%02d-%02d-%04d %02d:%02d:%02d] %s\n",
            t->tm_mday, t->tm_mon + 1, t->tm_year + 1900,
            t->tm_hour, t->tm_min, t->tm_sec, message);
    fflush(log_file);
    pthread_mutex_unlock(&log_mutex);
}

// 요청에서 파일 이름 추출
void get_requested_file(const char *request, char *filepath) {
    sscanf(request, "GET /%s HTTP/1.1", filepath);
    if (strlen(filepath) == 0 || strcmp(filepath, "/") == 0) {
        strcpy(filepath, "index.html"); // 기본 파일로 index.html 제공
    }
    char fullpath[BUFFER_SIZE];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", ROOT_DIR, filepath);
    strncpy(filepath, fullpath, BUFFER_SIZE);
}

// MIME 타입 결정 함수
// 이미지 파일도 가능하도록 함
const char* get_mime_type(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0) return "text/html";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, ".png") == 0) return "image/png";
    if (strcmp(dot, ".gif") == 0) return "image/gif";
    return "application/octet-stream";  // 기본값
}

// 클라이언트 요청 처리 함수
void handle_client(int client_socket) {
    char buffer[BUFFER_SIZE];
    char filepath[BUFFER_SIZE];
    struct stat file_stat;

    pthread_mutex_lock(&stats_mutex);
    active_connections++;
    pthread_mutex_unlock(&stats_mutex);

    int bytes_read = read(client_socket, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        close(client_socket);
        pthread_mutex_lock(&stats_mutex);
        active_connections--;
        pthread_mutex_unlock(&stats_mutex);
        return;
    }
    buffer[bytes_read] = '\0';
    printf("Request:\n%s\n", buffer);

    get_requested_file(buffer, filepath);

    if (stat(filepath, &file_stat) == -1 || S_ISDIR(file_stat.st_mode)) {
        const char *response =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 13\r\n"
            "\r\n"
            "404 Not Found";
        write(client_socket, response, strlen(response));
        log_message("404 Not Found: File not found");
    } else {
        FILE *file = fopen(filepath, "rb");
        if (!file) {
            const char *response =
                "HTTP/1.1 500 Internal Server Error\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 25\r\n"
                "\r\n"
                "500 Internal Server Error";
            write(client_socket, response, strlen(response));
            log_message("500 Internal Server Error: File open failed");
        } else {
            // MIME 타입 결정
            const char *mime_type = get_mime_type(filepath);

            // HTTP 응답 헤더 작성
            char header[BUFFER_SIZE];
            snprintf(header, sizeof(header),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: %s\r\n"
                     "\r\n", mime_type);
            write(client_socket, header, strlen(header));

            // 파일 내용 전송
            char file_buffer[BUFFER_SIZE];
            size_t bytes;
            while ((bytes = fread(file_buffer, 1, sizeof(file_buffer), file)) > 0) {
                write(client_socket, file_buffer, bytes);
            }
            fclose(file);
            log_message("200 OK: File served successfully");
        }
    }

    close(client_socket);
    pthread_mutex_lock(&stats_mutex);
    active_connections--;
    pthread_mutex_unlock(&stats_mutex);
}

// main 함수에서 Epoll 설정
int main() {
    int server_socket, epoll_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    // 로그 파일 열기
    log_file = fopen("server.log", "a");
    if (!log_file) {
        perror("Failed to open log file");
        exit(EXIT_FAILURE);
    }
    printf("Server started.\n");

    // 소켓 생성
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        perror("Socket creation failed");
        log_message("Error: Failed to create socket.");
        fclose(log_file);
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("Bind failed");
        log_message("Error: Failed to bind socket.");
        close(server_socket);
        fclose(log_file);
        exit(EXIT_FAILURE);
    }

    if (listen(server_socket, 5) == -1) {
        perror("Listen failed");
        log_message("Error: Failed to listen on socket.");
        close(server_socket);
        fclose(log_file);
        exit(EXIT_FAILURE);
    }

    // Epoll 설정
    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("Epoll creation failed");
        close(server_socket);
        fclose(log_file);
        exit(EXIT_FAILURE);
    }

    ev.events = EPOLLIN;
    ev.data.fd = server_socket;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_socket, &ev) == -1) {
        perror("Epoll control failed");
        close(server_socket);
        fclose(log_file);
        exit(EXIT_FAILURE);
    }

    // 이벤트 대기
    while (1) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (n == -1) {
            perror("Epoll wait failed");
            break;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == server_socket) {
                // 새로운 클라이언트 연결 수락
                int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_addr_len);
                if (client_socket == -1) {
                    perror("Accept failed");
                    continue;
                }
                printf("Client connected.\n");

                // 클라이언트 소켓을 Epoll에 등록
                ev.events = EPOLLIN | EPOLLET;
                ev.data.fd = client_socket;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_socket, &ev) == -1) {
                    perror("Epoll control failed for client socket");
                    close(client_socket);
                }
            } else {
                // 클라이언트 소켓에서 데이터 처리
                handle_client(events[i].data.fd);
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
                close(events[i].data.fd);
            }
        }
    }

    close(server_socket);
    fclose(log_file);
    return 0;
}

