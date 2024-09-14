//
//  server.c
//  FinalProject
//
//  Created by yunus emre avcı on 31.08.2024.
//

#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<pthread.h>
#include<sys/types.h>
#include<sys/socket.h>
#include<sys/stat.h>
#include<netinet/in.h>
#include<signal.h>
#include<fcntl.h>
#include<string.h>
#include<limits.h>
#include<arpa/inet.h>
#include<stdbool.h>
#include<time.h>
#include<sys/file.h>
#include<getopt.h>
#include <errno.h>
#include <stdarg.h>
    
#define MAX_THREADS 1024
#define MIN_THREADS 2
#define LOAD_THRESHOLD 0.75
#define GROWTH_FACTOR 0.25
#define MAX_NODES 1000000
#define MAX_SIZE 100
#define LOCKFILE "tmp/server.lock"
#define CACHE_SIZE 1000

FILE *log_file;

typedef struct Node{
    int dest;
    struct Node* next;
}Node;

typedef struct{
    int num_nodes;
    int num_edges;
    Node** adj_list;
}Graph;

typedef struct CacheEntry{
    int start;
    int end;
    int *path;
    int path_length;
    struct CacheEntry *next;
}CacheEntry;

typedef struct{
    pthread_mutex_t lock;
    pthread_cond_t read_cond;
    pthread_cond_t write_cond;
    int readers;
    int writers;
    int waiting_writers;
}RWLock;

//Globals
int server_socket;
pthread_t *threads;
int num_threads;
int max_threads;
int *client_sockets;
int *thread_status; // 0 for available, 1 for busy
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t pool_mutex = PTHREAD_MUTEX_INITIALIZER;
Graph *graph;
CacheEntry *cache[CACHE_SIZE] = {NULL};
RWLock cache_lock;
int initial_threads;
int current_threads;
int busy_threads = 0;
int priority_mode = 1; // Default to writer priority
pthread_t resizer_thread;
bool server_running = true;

//Function Prototypes
void *thread_function(void *arg);
void daemonize();
void signal_handler(int signum);
void handle_connection(int client_socket);
void load_graph(const char *file_path);
void add_edge(int src, int dest);
bool bfs(int start_node, int end_note, int *path);
void initialize_thread_pool();
void create_lockfile();
void redirect_output(const char *log_path);
void add_to_cache(int start, int end, int *path, int path_length);
bool get_from_cache(int start, int end, int **path, int *path_length);
unsigned int hash(int start, int end);
void rwlock_init(RWLock *lock);
void rwlock_readlock(RWLock *lock);
void rwlock_readunlock(RWLock *lock);
void rwlock_writelock(RWLock *lock);
void rwlock_unlock(RWLock *lock);
void *resizer_function(void *arg);
void resize_pool();
void log_message(const char *format, ...);

int main(int argc, char *argv[]){
    char *input_file = NULL;
    char *log_file_path = NULL;
    int port = 0;

    int opt;
    while((opt = getopt(argc, argv, "i:p:o:s:x:r:")) != -1){
        switch(opt){
            case 'i':
                input_file = optarg;
                break;
            case 'p':
                port = atoi(optarg);
                break;
            case 'o':
                log_file_path = optarg;
                break;
            case 's':
                initial_threads = atoi(optarg);
                if(initial_threads < MIN_THREADS){
                    fprintf(stderr, "Initial thread count must be at least %d\n", MIN_THREADS);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'x':
                max_threads = atoi(optarg);

                break;

            case 'r':
                priority_mode = atoi(optarg);
                if (priority_mode < 0 || priority_mode > 2) {
                    fprintf(stderr, "Invalid priority mode. Use 0, 1, or 2.\n");
                    exit(EXIT_FAILURE);
                }
                break;
            default:
                fprintf(stderr, "Usage: %s -i pathToFile -p PORT -o pathToLogFile -s initialThreads -x maxThreads [-r priorityMode]\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }
    if (!input_file || !port || !log_file_path || !initial_threads || !max_threads) {
        fprintf(stderr, "All arguments are required except -r.\n");
        fprintf(stderr, "Usage: %s -i pathToFile -p PORT -o pathToLogFile -s initialThreads -x maxThreads [-r priorityMode]\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    log_message("Executing with parameters:");
    log_message("-i %s", input_file);
    log_message("-p %d", port);
    log_message("-o %s", log_file_path);
    log_message("-s %d", initial_threads);
    log_message("-x %d", max_threads);
    log_message("-r %d", priority_mode);

    log_message("Loading graph...");
    clock_t start = clock();
    create_lockfile();
    daemonize();
    redirect_output(log_file_path);
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    load_graph(input_file);
    clock_t end = clock();
    double time_taken = ((double)(end - start)) / CLOCKS_PER_SEC;
 
    initialize_thread_pool();
    log_message("A pool of %d threads has been created", initial_threads);

    rwlock_init(&cache_lock);
    
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    // Create socket
    if ((server_socket = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    // Bind socket
    if (bind(server_socket, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    // Listen for connections
    if (listen(server_socket, 3) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    log_message("Server is running and listening on port %d", port);

    while (server_running) {
        int new_socket;
        if ((new_socket = accept(server_socket, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            perror("Accept failed");
            continue;
        }

        pthread_mutex_lock(&pool_mutex);
        int thread_index = -1;
        while (thread_index == -1) {
            for (int i = 0; i < max_threads; i++) {
                if (thread_status[i] == 0) {
                    thread_index = i;
                    thread_status[i] = 1;
                    client_sockets[i] = new_socket;
                    break;
                }
            }
            if (thread_index == -1) {
                log_message("No thread is available! Waiting for one.");
                pthread_cond_wait(&cond, &pool_mutex);
            }
        }
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&pool_mutex);
    }

    return 0;
}
void create_lockfile(){
    int fd = open(LOCKFILE, O_CREAT | O_RDWR, 0640);
    if(fd < 0){
        perror("Error creating lockfile");
        exit(EXIT_FAILURE);
    }
    
    
    if(flock(fd, LOCK_EX | LOCK_NB) < 0){
        if(errno == EWOULDBLOCK || errno == EAGAIN){
            fprintf(stderr, "Another instance of the server is already running.\n"); 
        }
        else{
            perror("Error locking file");
        }
        exit(EXIT_FAILURE);
    }
    
}
void redirect_output(const char *log_path){
    log_file = fopen(log_path, "a");
    if(log_file == NULL){
        perror("Error opening log file");
        exit(EXIT_FAILURE);
    }
    //Redirect stdout and stderr to log file
    dup2(fileno(log_file), STDOUT_FILENO);
    dup2(fileno(log_file), STDERR_FILENO);
    
}
void initialize_thread_pool(){
    max_threads = MAX_THREADS;
    threads = malloc(max_threads * sizeof(pthread_t));
    client_sockets = malloc(max_threads * sizeof(int));
    thread_status = malloc(max_threads * sizeof(int));
    for(int i = 0; i < max_threads; i++){
        if(pthread_create(&threads[i], NULL, thread_function, (void*)&i) != 0){
            perror("Error creating thread");
            exit(EXIT_FAILURE);
        }
    }
}
void *thread_function(void *arg){
    int thread_index = (int)(intptr_t)arg;
    log_message("Thread #%d: waiting for connection", thread_index);
    while(server_running){
        pthread_mutex_lock(&pool_mutex);
        while(thread_status[thread_index] == 0 && server_running){
            pthread_cond_wait(&cond, &pool_mutex);
        }
        if (!server_running) {
            pthread_mutex_unlock(&pool_mutex);
            break;
        }
        int client_socket = client_sockets[thread_index];
        busy_threads++;
        pthread_mutex_unlock(&pool_mutex);
        
        handle_connection(client_socket);
        
        pthread_mutex_lock(&pool_mutex);
        thread_status[thread_index] = 0;
        busy_threads--;
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&pool_mutex);
        log_message("Thread #%d: waiting for connection", thread_index);
    }
    return NULL;
}
void handle_connection(int client_socket){
    int i1,i2;
    if(recv(client_socket, &i1, sizeof(int), 0) <= 0 || recv(client_socket,&i2, sizeof(int), 0) <= 0){
        perror("Error receiving data");
        close(client_socket);
        return;
    }
    int *path = NULL;
    bool path_found = false;
    int path_length = 0;
    bool from_cache = false;

log_message("Thread #%d: searching database for a path from node %d to node %d", (int)(intptr_t)pthread_self(), i1, i2);

    // Try to get path from cache
    if (get_from_cache(i1, i2, &path, &path_length)) {
        path_found = true;
        from_cache = true;
        log_message("Thread #%d: path found in database: %d->%d->...", (int)(intptr_t)pthread_self(), i1, i2);

    } else {
        // If not in cache, calculate path
        path = malloc(MAX_NODES * sizeof(int));
        path_found = bfs(i1, i2, path);

        if (path_found) {
            log_message("Thread #%d: path calculated: %d->%d->...", (int)(intptr_t)pthread_self(), i1, i2);

            path_length = 0;
            while (path[path_length] != -1 && path_length < MAX_NODES) {
                path_length++;
            }
            // Add to cache
            add_to_cache(i1, i2, path, path_length);
            log_message("Thread #%d: responding to client and adding path to database...", (int)(intptr_t)pthread_self());
        }
    }

    if (path_found) {
        send(client_socket, &path_length, sizeof(int), 0);
        send(client_socket, path, path_length * sizeof(int), 0);
    } else {
        int no_path = -1;
         send(client_socket, &no_path, sizeof(int), 0);
        log_message("Thread #%d: path not possible from node %d to %d", (int)(intptr_t)pthread_self(), i1, i2);

    }

    if (!get_from_cache(i1, i2, &path, &path_length)) {
        free(path);
    }
    close(client_socket);
}
void signal_handler(int signum) {
    
    log_message("Termination signal received, waiting for ongoing threads to complete.");
    server_running = false;
    pthread_cond_broadcast(&cond);
    for (int i = 0; i < current_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    pthread_join(resizer_thread, NULL);
    log_message("All threads have terminated, server shutting down.");
    fflush(log_file);
    close(server_socket);
    unlink(LOCKFILE);
    fclose(log_file);
    exit(EXIT_SUCCESS);
}
void log_message(const char *format, ...) {
    va_list args;
    va_start(args, format);
    time_t now = time(NULL);
    char timestamp[20];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(log_file, "[%s] ", timestamp);
    vfprintf(log_file, format, args);
    fprintf(log_file, "\n");
    fflush(log_file);
    va_end(args);
}
void daemonize(){
    pid_t pid = fork();
    if(pid < 0){
        perror("Error forking");
        exit(EXIT_FAILURE);
    }
    if(pid > 0){
        exit(EXIT_SUCCESS); // Exit parent
    }
    //Create new session
    if(setsid() < 0){
        perror("Error creating new session");
        exit(EXIT_FAILURE);
    }
    // Fork again to ensure daemon cannot acquire a controlling terminal
    pid = fork();
    if (pid < 0) {
        perror("Error forking");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS); // Exit second parent
    }
    // Set file permissions
    umask(0);

    //Change to root directory
    if(chdir("/") < 0){
        perror("Error changing directory to root");
        exit(EXIT_FAILURE);
    }
    //close all open file descriptors
    for(int x = sysconf(_SC_OPEN_MAX); x > 0; x-- ){
        close(x);
    }
}
void add_edge(int src, int dest){
    Node *new_node = (Node *)malloc(sizeof(Node));
    new_node->dest = dest;
    new_node->next = graph->adj_list[src];
    graph->adj_list[src] = new_node;
    graph->num_edges++;
}
void load_graph(const char *file_path){
    FILE *file = fopen(file_path,"r");
    if(!file){
        perror("error opening graph file\n");
        exit(EXIT_FAILURE);
    }

    int src,dest;
    graph = (Graph*)malloc(sizeof(Graph));
    graph->adj_list = (Node**)malloc(MAX_NODES * sizeof(Node*));
    for(int i=0; i < MAX_NODES; i++){
        graph->adj_list[i] = NULL;
    }
    graph->num_nodes = 0;
    graph->num_edges = 0;
    while(fscanf(file, "%d %d", &src, &dest) !=EOF){
        add_edge(src,dest);
        if(src > graph->num_nodes) graph->num_nodes = src;
        if(dest > graph->num_nodes) graph->num_nodes = dest;
    }
    graph->num_nodes++;
    fclose(file);
}
bool bfs(int start_node, int end_node, int *path) {
    bool visited[MAX_NODES] = {false};
    int parent[MAX_NODES];
    for (int i = 0; i < MAX_NODES; i++) {
        parent[i] = -1;
    }

    int queue[MAX_NODES];
    int front = 0, rear = 0;

    visited[start_node] = true;
    queue[rear++] = start_node;

    while (front < rear) {
        int current = queue[front++];

        if (current == end_node) {
            // Reconstruct the path
            int path_length = 0;
            for (int at = end_node; at != -1; at = parent[at]) {
                path[path_length++] = at;
            }
            // Reverse the path
            for (int i = 0; i < path_length / 2; i++) {
                int temp = path[i];
                path[i] = path[path_length - 1 - i];
                path[path_length - 1 - i] = temp;
            }
            return true;
        }

        for (Node *neighbor = graph->adj_list[current]; neighbor != NULL; neighbor = neighbor->next) {
            if (!visited[neighbor->dest]) {
                visited[neighbor->dest] = true;
                parent[neighbor->dest] = current;
                queue[rear++] = neighbor->dest;
            }
        }
    }

    return false;
}
void add_to_cache(int start, int end, int *path, int path_length){
    unsigned int index = hash(start, end);
    CacheEntry *new_entry = malloc(sizeof(CacheEntry));
    new_entry->start = start;
    new_entry->end = end;
    new_entry->path = malloc(path_length * sizeof(int));
    memcpy(new_entry->path, path, path_length * sizeof(int));
    new_entry->path_length = path_length;
    new_entry->next = NULL;

    rwlock_writelock(&cache_lock);
    if(cache[index] == NULL){
        cache[index] = new_entry;
    }
    else{
        new_entry->next = cache[index];
        cache[index] = new_entry;
    }
    rwlock_unlock(&cache_lock);
}
bool get_from_cache(int start, int end, int **path, int *path_length){
    unsigned int index = hash(start, end);
    rwlock_readlock(&cache_lock);
    CacheEntry *entry = cache[index];
    while(entry != NULL){
        if(entry->start == start && entry->end == end){
            *path = entry->path;
            *path_length = entry->path_length;
            rwlock_unlock(&cache_lock);
            return true;
        }
        entry = entry->next;
    }
    rwlock_unlock(&cache_lock);
    return false;
}
unsigned int hash(int start, int end) {
    return (start * 31 + end) % CACHE_SIZE;
}
void rwlock_init(RWLock *lock){
    pthread_mutex_init(&lock->lock, NULL);
    pthread_cond_init(&lock->read_cond, NULL);
    pthread_cond_init(&lock->write_cond, NULL);
    lock->readers = 0;
    lock->writers = 0;
    lock->waiting_writers = 0;
}
void rwlock_readlock(RWLock *lock) {
    pthread_mutex_lock(&lock->lock);
    while (lock->writers > 0 || lock->waiting_writers > 0) {
        pthread_cond_wait(&lock->read_cond, &lock->lock);
    }
    lock->readers++;
    pthread_mutex_unlock(&lock->lock);
}

void rwlock_readunlock(RWLock *lock) {
    pthread_mutex_lock(&lock->lock);
    lock->readers--;
    if (lock->readers == 0 && lock->waiting_writers > 0) {
        pthread_cond_signal(&lock->write_cond);
    }
    pthread_mutex_unlock(&lock->lock);
}

void rwlock_writelock(RWLock *lock) {
    pthread_mutex_lock(&lock->lock);
    lock->waiting_writers++;
    while (lock->readers > 0 || lock->writers > 0) {
        pthread_cond_wait(&lock->write_cond, &lock->lock);
    }
    lock->waiting_writers--;
    lock->writers++;
    pthread_mutex_unlock(&lock->lock);
}

void rwlock_unlock(RWLock *lock) {
    pthread_mutex_lock(&lock->lock);
    lock->writers--;
    if (lock->waiting_writers > 0) {
        pthread_cond_signal(&lock->write_cond);
    } else {
        pthread_cond_broadcast(&lock->read_cond);
    }
    pthread_mutex_unlock(&lock->lock);
}
void *resizer_function(void *arg){
    while(server_running){
        sleep(1);
        double load = (double)busy_threads / current_threads;
        if(load >= LOAD_THRESHOLD && current_threads < max_threads){
            resize_pool();
        }
    }
    return NULL;
}
void resize_pool(){
    int new_threads = current_threads * GROWTH_FACTOR;
    if(new_threads + current_threads > max_threads){
        new_threads = max_threads - current_threads;
    }
    if(new_threads <= 0) return;

    pthread_mutex_lock(&pool_mutex);
    int old_size = current_threads;
    current_threads += new_threads;
    threads = realloc(threads, current_threads * sizeof(pthread_t));
    client_sockets = realloc(client_sockets, current_threads * sizeof(int));
    thread_status = realloc(thread_status, current_threads * sizeof(int));
    
    for(int i = old_size; i < current_threads; i++){
        thread_status[i] = 1;
        if(pthread_create(&threads[i], NULL, thread_function, (void*)(intptr_t)i) != 0){
            log_message("Error creating new thread during resize");
            current_threads--;
            break;
        }
    }
    pthread_mutex_unlock(&pool_mutex);
    log_message("System load %.1f%%,pool extended to %d threads", (double)busy_threads / current_threads * 100, current_threads);

}