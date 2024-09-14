//
//  client.c
//  
//
//  Created by yunus emre avcı on 31.08.2024.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>
#include <sys/types.h>
#include <errno.h>
#include <stdarg.h>
#define MAX_PATH_LENGTH 1000

void log_message(const char *format, ...) {
    va_list args;
    va_start(args, format);
    time_t now = time(NULL);
    char timestamp[20];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
    printf("[%s] ", timestamp);
    vprintf(format, args);
    printf("\n");
    va_end(args);
}

int main(int argc,char *argv[]){
    if(argc != 9){
        fprintf(stderr, "Usage: %s -a IP_ADDRESS -p PORT -s SOURCE_NODE -d DEST_NODE\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    char *ip_address = NULL;
    int port = -1, source_node = -1, dest_node = -1;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i += 2) {
        if (strcmp(argv[i], "-a") == 0) {
            ip_address = argv[i + 1];
        } else if (strcmp(argv[i], "-p") == 0) {
            port = atoi(argv[i + 1]);
        } else if (strcmp(argv[i], "-s") == 0) {
            source_node = atoi(argv[i + 1]);
        } else if (strcmp(argv[i], "-d") == 0) {
            dest_node = atoi(argv[i + 1]);
        }
    }
    
    if (!ip_address || port < 0 || source_node < 0 || dest_node < 0) {
        fprintf(stderr, "Invalid arguments\n");
        exit(EXIT_FAILURE);
    }
    
    //Get the client PID
    pid_t pid = getpid();
    
    //Create socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0){
        fprintf(stderr, "Socket creation failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    
    //Define server address
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if(inet_pton(AF_INET, ip_address, &server_addr.sin_addr) <= 0){
        perror("Invalid IP address");
        exit(EXIT_FAILURE);
    }
    
    //Connect to server
    log_message("Client (%d) connecting to %s:%d", pid, ip_address, port);
    if(connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0){
        fprintf(stderr, "Connection failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    log_message("Client (%d) connected and requesting a path from node %d to %d", pid, source_node, dest_node);

    //Send source and destination nodes to server
    int nodes[2] = {source_node,dest_node};
    if(send(sock, nodes, sizeof(nodes), 0) < 0){
        perror("Send failed");
        exit(EXIT_FAILURE);
    }
    // Start timing
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    // Receive response from server
    int path_length;
    if (recv(sock, &path_length, sizeof(int), 0) < 0) {
        perror("Receive failed");
        exit(EXIT_FAILURE);
    }

    if (path_length == -1) {
        // No path found
        clock_gettime(CLOCK_MONOTONIC, &end);
        double time_taken = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
        log_message("Server's response to (%d): NO PATH, arrived in %.2f seconds, shutting down", pid, time_taken);
    } else {
        // Path found
        int path[MAX_PATH_LENGTH];
        if (recv(sock, path, path_length * sizeof(int), 0) < 0) {
            perror("Receive failed");
            exit(EXIT_FAILURE);
        }

        clock_gettime(CLOCK_MONOTONIC, &end);
        double time_taken = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;

        // Construct path string
        char path_str[MAX_PATH_LENGTH * 10] = {0};  // Assuming each node number is at most 10 characters
        for (int i = 0; i < path_length; i++) {
            char node_str[11];
            snprintf(node_str, sizeof(node_str), "%d", path[i]);
            strcat(path_str, node_str);
            if (i < path_length - 1) {
                strcat(path_str, "->");
            }
        }

        log_message("Server's response to (%d): %s, arrived in %.2f seconds", pid, path_str, time_taken);
    }
    // Close the socket
    close(sock);
    
    return 0;
}
