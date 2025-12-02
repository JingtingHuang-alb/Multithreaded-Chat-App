
#include <stdio.h>
#include "udp.h"
#include <stdlib.h>  
#include <string.h>  
#include <pthread.h>

#define CLIENT_PORT 10000

// Global variable 
int sd;

//listen_task function from chat_client.c

void *listen_task(void *arg) {
    struct sockaddr_in server_addr;
    char server_response[BUFFER_SIZE];
    int rc;

    while (1) {
        // Block and wait for messages
        rc = udp_socket_read(sd, &server_addr, server_response, BUFFER_SIZE);
        
        if (rc > 0) {
            //Automatically reply 
            if (strcmp(server_response, "ping$") == 0) {
                char pong_msg[] = "ret-ping$"; // to let the server know we are still online: if the server sends "ping$", we immediately reply with "ret-ping$"
                udp_socket_write(sd, &server_addr, pong_msg, BUFFER_SIZE);
                
                continue; 
            }
            printf("\rServer says: %s\n> ", server_response); 
            fflush(stdout);
        }
    }
    return NULL;
}


// client code
int main(int argc, char *argv[])
{
   // Change to 0 so the system automatically allocates a free port,
    sd = udp_socket_open(0); 
    
    //SERVER ADDRESS
    struct sockaddr_in server_addr;
    int rc = set_socket_addr(&server_addr, "127.0.0.1", SERVER_PORT);
    //thread for listening
    pthread_t listener_thread;
    pthread_create(&listener_thread, NULL, listen_task, NULL);
    char client_request[BUFFER_SIZE];
    printf("Connected to server. Start typing (Ctrl+C to quit):\n> ");
    
    while (1) {
        fgets(client_request, BUFFER_SIZE, stdin);
        client_request[strcspn(client_request, "\n")] = 0;
        

        rc = udp_socket_write(sd, &server_addr, client_request, BUFFER_SIZE);
    }

    return 0;
}
