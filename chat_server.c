//chat_server.c 

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>    // 【PE 2】为了获取当前时间 time()
#include <unistd.h>  // 【PE 2】为了使用 sleep() 函数
#include "udp.h"

// --- 配置参数 ---
#define MAX_HISTORY 15            // 【PE 1】历史记录只存最近 15 条
#define INACTIVITY_THRESHOLD 30   // 【PE 2】定义的超时时间 (秒)，超过这个没说话就是僵尸

// --- [Step 3] 数据结构：花名册 (Linked List) ---

typedef struct UserNode {
    struct sockaddr_in addr;  // 用户的地址 (IP + Port)
    char name[50];            // 用户的名字
    
    // --- [PE 2] 踢人功能需要的字段 ---
    time_t last_active;       // 最后一次活跃的时间戳 (用来算是不是超时了)
    int has_been_pinged;      // 标记：是否已经发过 ping$ 警告信了 (0=没发过, 1=发过)
    
    struct UserNode *next;    // 指向下一个用户的指针
} UserNode;

UserNode *head = NULL;        // 名册的第一页，一开始是空的

// --- [Step 5] 同步锁 ---
pthread_rwlock_t lock;        // 读写锁：保护花名册 (UserNode 链表)

// --- [Step 6 / PE 1] 历史记录相关变量 ---
char chat_history[MAX_HISTORY][BUFFER_SIZE]; // 环形缓冲区，存最近的 15 条消息
int history_count = 0;        // 当前存了多少条
int history_end_index = 0;    // 下一条消息该写在哪个格子
pthread_mutex_t history_lock; // 互斥锁：专门保护历史记录数组

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//   辅助功能区

// --- [Final Step / PE 2] 更新活跃时间 ---
// 只要用户发了消息 (包括心跳包)，就调用这个函数，给他“续命”
void update_last_active(struct sockaddr_in client_addr) {
    pthread_rwlock_wrlock(&lock); // 上写锁 (因为要修改 last_active)
    
    UserNode *current = head;
    while (current != NULL) {
        // 通过 IP 和 端口 找到这个人
        if (current->addr.sin_addr.s_addr == client_addr.sin_addr.s_addr &&
            current->addr.sin_port == client_addr.sin_port) {
            
            // 更新时间为“现在”
            current->last_active = time(NULL);
            // 重置警告标记 (Flag)，因为他活过来了，下次重新计算超时
            current->has_been_pinged = 0;
            break;
        }
        current = current->next;
    }
    pthread_rwlock_unlock(&lock); // 解锁
}

// --- [Step 3] 把人加进名单 (含 [Step 5] 锁 和 [Final] 查重) ---
void add_user_to_list(struct sockaddr_in addr, char *name) {
    pthread_rwlock_wrlock(&lock); // 上写锁：因为要修改链表结构

    // 1. [Final Fix] 查重逻辑：防止同一个名字变成多个“幽灵分身”
    UserNode *current = head;
    while (current != NULL) {
        if (strcmp(current->name, name) == 0) {
            // 如果名字已存在，直接更新他的信息，不创建新节点
            current->addr = addr;
            current->last_active = time(NULL);
            current->has_been_pinged = 0;
            
            pthread_rwlock_unlock(&lock);
            printf("SERVER LOG: User [%s] reconnected/updated.\n", name);
            return; 
        }
        current = current->next;
    }

    // 2. 如果是新用户，申请内存创建节点
    UserNode *new_user = (UserNode *)malloc(sizeof(UserNode));
    new_user->addr = addr;
    strcpy(new_user->name, name);
    new_user->last_active = time(NULL); // 刚进来，时间设为现在
    new_user->has_been_pinged = 0;
    
    // 头插法：放到链表最前面
    new_user->next = head;
    head = new_user;

    pthread_rwlock_unlock(&lock); // 解锁

    printf("SERVER LOG: User [%s] added.\n", name);
}

// --- [Final Step / PE 2] 僵尸粉清理线程 ---
// 这是一个独立的线程，专门在后台每隔 5 秒巡逻一次
void *check_inactive_clients(void *arg) {
    int sd = *(int *)arg; // 获取 socket，因为要用来发 ping 消息

    while (1) {
        sleep(5); // 休息 5 秒

        time_t now = time(NULL);
        
        // 上写锁：因为可能要踢人(删除节点)，必须用写锁
        pthread_rwlock_wrlock(&lock);

        UserNode *current = head;
        UserNode *prev = NULL;

        while (current != NULL) {
            // 计算由于多久没说话了 (当前时间 - 上次活跃时间)
            double seconds_inactive = difftime(now, current->last_active);

            //  情况 1: 超时太久，且之前已经警告过 (踢人)
            if (seconds_inactive > (INACTIVITY_THRESHOLD + 10) && current->has_been_pinged) {
                
                printf("SERVER LOG: Kicking inactive user [%s]\n", current->name);
                
                // 发送被踢通知
                char kick_msg[] = "You have been removed due to inactivity.";
                udp_socket_write(sd, &current->addr, kick_msg, BUFFER_SIZE);

                // 从链表中删除该节点
                UserNode *to_delete = current;
                if (prev == NULL) {
                    head = current->next; // 删的是头节点
                    current = head;
                } else {
                    prev->next = current->next; // 删的是中间节点
                    current = current->next;
                }
                free(to_delete);
                continue; // 删完了直接进入下一次循环
            }
            
            // --- 情况 2: 刚刚超时，还没警告过 (发 Ping) ---
            else if (seconds_inactive > INACTIVITY_THRESHOLD && !current->has_been_pinged) {
                
                printf("SERVER LOG: Sending PING to [%s] (inactive for %.0f s)\n", 
                       current->name, seconds_inactive);
                
                char ping_msg[] = "ping$";
                udp_socket_write(sd, &current->addr, ping_msg, BUFFER_SIZE);
                
                // 【关键】标记为“已警告”，防止无限发 Ping
                current->has_been_pinged = 1;
                // (调试时我们曾在这里加打印，确认 Flag 是否生效)
            }

            // 继续检查下一个人
            prev = current;
            if (current != NULL) {
                current = current->next;
            }
        }
        
        pthread_rwlock_unlock(&lock); // 巡逻结束，解锁
    }
    return NULL;
}

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//   [Step 6 / PE 1] 历史记录功能区


// 把消息存进环形缓冲区
void save_to_history(char *msg) {
    pthread_mutex_lock(&history_lock); // 历史记录专用锁
    
    // 复制消息到当前位置
    strncpy(chat_history[history_end_index], msg, BUFFER_SIZE - 1);
    chat_history[history_end_index][BUFFER_SIZE - 1] = '\0';
    
    // 移动指针
    history_end_index++;
    if (history_end_index >= MAX_HISTORY) history_end_index = 0; // 超过 15 就回到 0 (环形)
    
    // 计数
    if (history_count < MAX_HISTORY) history_count++;
    
    pthread_mutex_unlock(&history_lock);
}

// 给新用户发送最近 15 条消息
void send_history_to_client(int server_socket, struct sockaddr_in client_addr) {
    pthread_mutex_lock(&history_lock);
    
    if (history_count > 0) {
        char intro_msg[] = "\n--- Chat History (Last 15 messages) ---\n";
        udp_socket_write(server_socket, &client_addr, intro_msg, strlen(intro_msg) + 1);
        
        // 计算从哪里开始读 (如果是满的，就从 end_index 开始读，那是它是最旧的消息)
        int start_index = (history_count == MAX_HISTORY) ? history_end_index : 0;
        
        for (int i = 0; i < history_count; i++) {
            int idx = (start_index + i) % MAX_HISTORY; // 取余数实现环形读取
            udp_socket_write(server_socket, &client_addr, chat_history[idx], BUFFER_SIZE);
        }
        
        char outro_msg[] = "--- End of History ---\n\n";
        udp_socket_write(server_socket, &client_addr, outro_msg, strlen(outro_msg) + 1);
    }
    
    pthread_mutex_unlock(&history_lock);
}


//   指令处理逻辑区


// --- [Step 3] 处理 conn (登录) ---
void handle_login(int server_socket, struct sockaddr_in sender_addr, char *name) {
    char reply_buffer[BUFFER_SIZE];
    
    // 1. 加人进名单
    add_user_to_list(sender_addr, name);
    
    // 2. 回复欢迎
    snprintf(reply_buffer, BUFFER_SIZE, "Hi %s, you have successfully connected", name);
    udp_socket_write(server_socket, &sender_addr, reply_buffer, BUFFER_SIZE);
    
    // 3. [PE 1] 发送历史记录
    send_history_to_client(server_socket, sender_addr);
}

// --- [Step 4] 处理 say (群聊广播) ---
void handle_broadcast(int server_socket, char *sender_name, char *msg) {
    char server_response[BUFFER_SIZE];
    snprintf(server_response, BUFFER_SIZE, "%s: %s", sender_name, msg);
    
    // [PE 1] 广播前，先存一份到历史记录
    save_to_history(server_response);

    // [Step 5] 加读锁 (Read Lock)，因为我们要遍历链表发消息
    pthread_rwlock_rdlock(&lock);
    
    UserNode *current = head;
    while (current != NULL) {
        // 给名单上的每个人都发一份
        udp_socket_write(server_socket, &current->addr, server_response, BUFFER_SIZE);
        current = current->next;
    }
    
    pthread_rwlock_unlock(&lock); // 发送完毕，解锁
    printf("SERVER LOG: Broadcast from [%s]\n", sender_name);
}

// --- [Step 4] 处理 sayto (私聊) ---
void handle_private_msg(int server_socket, struct sockaddr_in sender_addr, char *arg_string, char *sender_name) {
    char recipient_name[50], msg[BUFFER_SIZE];
    
    // 解析参数：把 "Bob Hello" 拆成 "Bob" 和 "Hello"
    if (sscanf(arg_string, "%s %[^\n]", recipient_name, msg) < 2) return;

    // [Step 5] 加读锁，开始查人
    pthread_rwlock_rdlock(&lock);
    
    UserNode *current = head;
    int found = 0;
    while (current != NULL) {
        // 查找名字匹配的人
        if (strcmp(current->name, recipient_name) == 0) {
            char server_response[BUFFER_SIZE];
            snprintf(server_response, BUFFER_SIZE, "%s: %s (private)", sender_name, msg);
            udp_socket_write(server_socket, &current->addr, server_response, BUFFER_SIZE);
            found = 1; 
            break; // 找到了就退出循环
        }
        current = current->next;
    }
    
    pthread_rwlock_unlock(&lock); // 查完解锁
    
    if (!found) {
        char err[] = "User not found";
        udp_socket_write(server_socket, &sender_addr, err, BUFFER_SIZE);
    }
}

//   [Step 1 & Step 3] 主程序 Main
// ======================================
int main(int argc, char *argv[]) {
    // 1. 开启 Socket
    int server_socket = udp_socket_open(SERVER_PORT);
    if (server_socket < 0) return 1;

    // 2. [Step 5] 初始化锁
    pthread_rwlock_init(&lock, NULL);
    pthread_mutex_init(&history_lock, NULL);

    // 3. [Final / PE 2] 启动清理僵尸粉的线程
    pthread_t cleaner_thread;
    pthread_create(&cleaner_thread, NULL, check_inactive_clients, &server_socket);

    printf("Server listening on port %d...\n", SERVER_PORT);

    // 4. 主循环：死等消息
    while (1) {
        struct sockaddr_in client_addr;
        char client_request[BUFFER_SIZE];
        
        int rc = udp_socket_read(server_socket, &client_addr, client_request, BUFFER_SIZE);
        
        if (rc > 0) {
            // Final / PE 2只要收到消息，就证明这个用户还活着，更新时间
            update_last_active(client_addr);

            // 5. Step 3 切割消息 (指令 $ 内容)
            char temp_buffer[BUFFER_SIZE];
            strcpy(temp_buffer, client_request);
            char *command = strtok(temp_buffer, "$");
            char *arg = strtok(NULL, "$");
            
            if (command != NULL) {
                // [Final / PE 2]处理心跳包回复
                if (strcmp(command, "ret-ping") == 0) {
                    // 忽略，因为 update_last_active 已经把时间更新了
                    continue;
                }

                if (arg != NULL) {
                    // 情况 A: 登录 
                    if (strcmp(command, "conn") == 0) {
                        handle_login(server_socket, client_addr, arg);
                    } 
                    // 情况 B: 聊天 (需要先查名字)
                    else {
                        char *sender_name = NULL;
                        
                        // [Step 4] 根据 IP 查名字 (读锁)
                        pthread_rwlock_rdlock(&lock);
                        UserNode *current = head;
                        while (current != NULL) {
                            if (current->addr.sin_addr.s_addr == client_addr.sin_addr.s_addr &&
                                current->addr.sin_port == client_addr.sin_port) {
                                sender_name = current->name;
                                break;
                            }
                            current = current->next;
                        }
                        pthread_rwlock_unlock(&lock);

                        if (sender_name) {
                            if (strcmp(command, "say") == 0) 
                                handle_broadcast(server_socket, sender_name, arg);
                            else if (strcmp(command, "sayto") == 0) 
                                handle_private_msg(server_socket, client_addr, arg, sender_name);
                        } else {
                            char err[] = "Please login first";
                            udp_socket_write(server_socket, &client_addr, err, BUFFER_SIZE);
                        }
                    }
                }
            }
        }
    }
    return 0;
}