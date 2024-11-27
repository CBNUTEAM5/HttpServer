#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>

#define PORT 8080
#define BUFFER_SIZE 1024
#define THREAD_POOL_SIZE 4    // 스레드 풀 크기
#define QUEUE_SIZE 10         // 작업 큐 크기

// 작업 큐 데이터 구조
typedef struct {
    int client_socket;
} Task;

// 작업 큐 및 동기화 도구
Task task_queue[QUEUE_SIZE];
int queue_front = 0, queue_rear = 0, queue_count = 0;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_not_empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t queue_not_full = PTHREAD_COND_INITIALIZER;

// 로그 파일 및 동기화 도구
FILE *log_file;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;

// 성능 데이터 전역 변수
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

// 작업 큐에 태스크 추가
void enqueue_task(Task task) {
    pthread_mutex_lock(&queue_mutex);
    while (queue_count == QUEUE_SIZE) { // 큐가 가득 찬 경우 대기
        pthread_cond_wait(&queue_not_full, &queue_mutex);
    }

    task_queue[queue_rear] = task;
    queue_rear = (queue_rear + 1) % QUEUE_SIZE;
    queue_count++;

    pthread_cond_signal(&queue_not_empty); // 작업이 추가되었음을 알림
    pthread_mutex_unlock(&queue_mutex);
}

// 작업 큐에서 태스크 제거
Task dequeue_task() {
    pthread_mutex_lock(&queue_mutex);
    while (queue_count == 0) { // 큐가 비어 있는 경우 대기
        pthread_cond_wait(&queue_not_empty, &queue_mutex);
    }

    Task task = task_queue[queue_front];
    queue_front = (queue_front + 1) % QUEUE_SIZE;
    queue_count--;

    pthread_cond_signal(&queue_not_full); // 큐에 공간이 생겼음을 알림
    pthread_mutex_unlock(&queue_mutex);

    return task;
}

// 클라이언트 요청 처리 함수
void handle_client(int client_socket) {
    char buffer[BUFFER_SIZE];
    clock_t start_time, end_time;

    pthread_mutex_lock(&stats_mutex);
    active_connections++;
    pthread_mutex_unlock(&stats_mutex);

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

    double response_time = (double)(end_time - start_time) / CLOCKS_PER_SEC;

    pthread_mutex_lock(&stats_mutex);
    total_requests++;
    total_response_time += response_time;
    active_connections--;
    pthread_mutex_unlock(&stats_mutex);

    close(client_socket);
}

// 스레드 풀에서 작업 처리
void *worker_thread(void *arg) {
    while (1) {
        Task task = dequeue_task(); // 작업 큐에서 태스크 꺼내기
        handle_client(task.client_socket);
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

    printf("Server is running. Waiting for connections on port %d...\n", PORT);

    // 스레드 풀 생성
    pthread_t thread_pool[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        pthread_create(&thread_pool[i], NULL, worker_thread, NULL);
    }

    while (1) {
        // 클라이언트 연결 수락
        int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_socket == -1) {
            perror("Accept failed");
            log_message("Error: Failed to accept client connection.");
            continue;
        }

        Task task;
        task.client_socket = client_socket;
        enqueue_task(task); // 작업 큐에 추가
    }

    close(server_socket);
    fclose(log_file);

    return 0;
}

