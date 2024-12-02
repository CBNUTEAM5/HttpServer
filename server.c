#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <time.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/resource.h> // For getrusage

#define PORT 5296
#define BUFFER_SIZE 1024
#define MAX_EVENTS 10
#define ROOT_DIR "./www"

// 로그 및 상태 관련 변수
FILE *log_file;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;

// 상태 변수
long total_requests = 0;
long successful_requests = 0;
long failed_requests = 0;
long active_connections = 0;

// 로그 기록 함수
void log_message(const char *message) {
    pthread_mutex_lock(&log_mutex);
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    // CPU 및 메모리 사용량 가져오기
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);

    // 메모리 사용량: Resident Set Size (RSS)
    long memory_usage_kb = usage.ru_maxrss;

    // CPU 시간: 사용자 및 시스템 모드에서 사용한 시간
    long cpu_user_time_ms = usage.ru_utime.tv_sec * 1000 + usage.ru_utime.tv_usec / 1000;
    long cpu_system_time_ms = usage.ru_stime.tv_sec * 1000 + usage.ru_stime.tv_usec / 1000;

    fprintf(log_file, "[%02d-%02d-%04d %02d:%02d:%02d] %s\n",
            t->tm_mday, t->tm_mon + 1, t->tm_year + 1900,
            t->tm_hour, t->tm_min, t->tm_sec, message);

    pthread_mutex_lock(&stats_mutex);
    fprintf(log_file, "[INFO] Total Requests: %ld, Successful: %ld, Failed: %ld, Active Connections: %ld\n",
            total_requests, successful_requests, failed_requests, active_connections);

    // CPU 및 메모리 사용량 기록
    fprintf(log_file, "[RESOURCE] CPU User Time: %ld ms, CPU System Time: %ld ms, Memory Usage: %ld KB\n",
            cpu_user_time_ms, cpu_system_time_ms, memory_usage_kb);

    pthread_mutex_unlock(&stats_mutex);
    fflush(log_file);
    pthread_mutex_unlock(&log_mutex);
}

// 소켓을 non-blocking으로 설정
void set_nonblocking(int socket) {
    int flags = fcntl(socket, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl get failed");
        exit(EXIT_FAILURE);
    }
    if (fcntl(socket, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl set failed");
        exit(EXIT_FAILURE);
    }
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
const char* get_mime_type(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0) return "text/html";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, ".png") == 0) return "image/png";
    if (strcmp(dot, ".gif") == 0) return "image/gif";
    return "application/octet-stream";
}

// 클라이언트 요청 처리
void handle_request(int client_socket) {
    char buffer[BUFFER_SIZE];
    char filepath[BUFFER_SIZE];
    struct stat file_stat;

    // 활성 연결 및 총 요청 증가
    pthread_mutex_lock(&stats_mutex);
    active_connections++;
    total_requests++;
    pthread_mutex_unlock(&stats_mutex);

    int bytes_read = read(client_socket, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        pthread_mutex_lock(&stats_mutex);
        failed_requests++;
        active_connections--;
        pthread_mutex_unlock(&stats_mutex);
        close(client_socket);
        log_message("Failed to read request: Client disconnected");
        return;
    }
    buffer[bytes_read] = '\0';

    get_requested_file(buffer, filepath);

    if (stat(filepath, &file_stat) == -1 || S_ISDIR(file_stat.st_mode)) {
        const char *response =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 13\r\n"
            "\r\n"
            "404 Not Found";
        write(client_socket, response, strlen(response));

        pthread_mutex_lock(&stats_mutex);
        failed_requests++;
        pthread_mutex_unlock(&stats_mutex);

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

            pthread_mutex_lock(&stats_mutex);
            failed_requests++;
            pthread_mutex_unlock(&stats_mutex);

            log_message("500 Internal Server Error: File open failed");
        } else {
            const char *mime_type = get_mime_type(filepath);
            char header[BUFFER_SIZE];
            snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: %s\r\n"
                "Content-Length: %ld\r\n"
                "\r\n", mime_type, file_stat.st_size);
            write(client_socket, header, strlen(header));

            char file_buffer[BUFFER_SIZE];
            size_t bytes;
            while ((bytes = fread(file_buffer, 1, sizeof(file_buffer), file)) > 0) {
                write(client_socket, file_buffer, bytes);
            }
            fclose(file);

            pthread_mutex_lock(&stats_mutex);
            successful_requests++;
            pthread_mutex_unlock(&stats_mutex);

            log_message("200 OK: File served successfully");
        }
    }

    close(client_socket);

    // 활성 연결 감소
    pthread_mutex_lock(&stats_mutex);
    active_connections--;
    pthread_mutex_unlock(&stats_mutex);
}

int main() {
    mkdir(ROOT_DIR, 0755); // www 디렉토리 생성
    int server_socket, epoll_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    log_file = fopen("server.log", "a");
    if (!log_file) {
        perror("Failed to open log file");
        exit(EXIT_FAILURE);
    }

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    set_nonblocking(server_socket);

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("Bind failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    if (listen(server_socket, SOMAXCONN) == -1) {
        perror("Listen failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("Epoll creation failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    struct epoll_event event, events[MAX_EVENTS];
    event.events = EPOLLIN;
    event.data.fd = server_socket;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_socket, &event) == -1) {
        perror("Epoll control failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    printf("Server is running on port %d...\n", PORT);

    while (1) {
        int event_count = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (event_count == -1) {
            perror("Epoll wait failed");
            break;
        }

        for (int i = 0; i < event_count; i++) {
            if (events[i].data.fd == server_socket) {
                int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_addr_len);
                if (client_socket == -1) {
                    perror("Accept failed");
                    continue;
                }

                set_nonblocking(client_socket);
                event.events = EPOLLIN | EPOLLET;
                event.data.fd = client_socket;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_socket, &event) == -1) {
                    perror("Epoll control failed for client");
                    close(client_socket);
                }
            } else {
                handle_request(events[i].data.fd);
            }
        }
    }

    close(server_socket);
    fclose(log_file);
    return 0;
}

