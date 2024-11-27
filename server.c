#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>

#define PORT 8080
#define BUFFER_SIZE 1024

// 로그 파일 및 동기화 도구
FILE *log_file;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;

// 성능 데이터 전역 변수
long total_requests = 0; // 총 요청 수
double total_response_time = 0; // 총 응답 시간
long active_connections = 0; // 현재 연결 중인 클라이언트 수

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

// 성능 데이터 기록 함수
void log_performance_data() {
    pthread_mutex_lock(&stats_mutex);

    double avg_response_time = (total_requests > 0)
                                ? total_response_time / total_requests
                                : 0.0;

    char stats[BUFFER_SIZE];
    snprintf(stats, sizeof(stats),
             "Performance Data - Total Requests: %ld, Avg Response Time: %.2f sec, Active Connections: %ld",
             total_requests, avg_response_time, active_connections);
    log_message(stats);

    pthread_mutex_unlock(&stats_mutex);
}

// 클라이언트 요청을 처리하는 함수
void *handle_client(void *arg) {
    int client_socket = *(int *)arg;
    free(arg);

    char buffer[BUFFER_SIZE];
    clock_t start_time, end_time;

    // 클라이언트 연결 증가
    pthread_mutex_lock(&stats_mutex);
    active_connections++;
    pthread_mutex_unlock(&stats_mutex);

    // 요청 처리
    start_time = clock();
    read(client_socket, buffer, sizeof(buffer) - 1); // 요청 읽기

    const char *response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "Hello, World!";
    write(client_socket, response, strlen(response)); // 응답 전송
    end_time = clock();

    // 응답 시간 계산
    double response_time = (double)(end_time - start_time) / CLOCKS_PER_SEC;

    pthread_mutex_lock(&stats_mutex);
    total_requests++;
    total_response_time += response_time;
    pthread_mutex_unlock(&stats_mutex);

    // 클라이언트 연결 종료
    close(client_socket);

    pthread_mutex_lock(&stats_mutex);
    active_connections--;
    pthread_mutex_unlock(&stats_mutex);

    return NULL;
}

// 성능 데이터를 주기적으로 기록하는 스레드
void *performance_logger(void *arg) {
    while (1) {
        sleep(10); // 10초마다 성능 데이터 기록
        log_performance_data();
    }
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

    // 성능 로깅 스레드 생성
    pthread_t logger_thread;
    if (pthread_create(&logger_thread, NULL, performance_logger, NULL) != 0) {
        perror("Failed to create performance logger thread");
        log_message("Error: Failed to create performance logger thread.");
        close(server_socket);
        fclose(log_file);
        exit(EXIT_FAILURE);
    }

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

        // 클라이언트 요청 처리 스레드 생성
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, client_socket) != 0) {
            perror("Thread creation failed");
            log_message("Error: Failed to create thread for client.");
            close(*client_socket);
            free(client_socket);
            continue;
        }

        pthread_detach(thread_id); // 스레드 리소스를 자동으로 정리
    }

    close(server_socket);
    fclose(log_file);

    return 0;
}

