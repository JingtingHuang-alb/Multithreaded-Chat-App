
#include <stdio.h>
#include "udp.h"
#include <stdlib.h>  
#include <string.h>  
#include <pthread.h>

#define CLIENT_PORT 10000

// 全局变量这样大家都能用
int sd;

//chat_client.c 的 listen_task 函数

void *listen_task(void *arg) {
    struct sockaddr_in server_addr;
    char server_response[BUFFER_SIZE];
    int rc;

    while (1) {
        // 死等消息
        rc = udp_socket_read(sd, &server_addr, server_response, BUFFER_SIZE);
        
        if (rc > 0) {
            //自动回复心跳包
            if (strcmp(server_response, "ping$") == 0) {
                // 如果服务器发来 "ping$"，我们马上回一个 "ret-ping$"
                //提醒服务器还在线
                char pong_msg[] = "ret-ping$";
                udp_socket_write(sd, &server_addr, pong_msg, BUFFER_SIZE);
                
                continue; // 处理完了，直接进入下一次循环，不打印 "Server says..."
            }

            // 正常聊天消息才打印
            printf("\rServer says: %s\n> ", server_response); 
            fflush(stdout);
        }
    }
    return NULL;
}


// client code
int main(int argc, char *argv[])
{
    // 1. 改成 0，让系统自动分配空闲端口，这样你可以开很多个客户端
    sd = udp_socket_open(0); 
    
    // 2. 设置服务器地址 
    struct sockaddr_in server_addr;
    int rc = set_socket_addr(&server_addr, "127.0.0.1", SERVER_PORT);

    // 3. 启动耳朵线程
    pthread_t listener_thread;
    // 创建一个线程，去跑 listen_task 函数
    pthread_create(&listener_thread, NULL, listen_task, NULL);

    // 4. 嘴巴循环：读取用户输入并发送
    char client_request[BUFFER_SIZE];
    printf("Connected to server. Start typing (Ctrl+C to quit):\n> ");
    
    while (1) {
        // 读取键盘输入
        fgets(client_request, BUFFER_SIZE, stdin);
        client_request[strcspn(client_request, "\n")] = 0;
        

        // 发送给服务器
        rc = udp_socket_write(sd, &server_addr, client_request, BUFFER_SIZE);
    }

    return 0;
}