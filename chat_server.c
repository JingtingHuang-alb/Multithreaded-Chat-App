#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>    // PE 2To get the current time using time()
#include <unistd.h>  // To use the sleep() function
#include "udp.h"

#define MAX_HISTORY 15            // PE Only store the latest 15 history messages
#define INACTIVITY_THRESHOLD 30   // PE 2 Defined timeout (seconds); if no message beyond this, the user is treated as inactive/zombie

//[Step 3] Data structure: user list (Linked List)

typedef struct UserNode {
    struct sockaddr_in addr;  // User address (IP + Port)
    char name[50];            // User name
    
    // [PE 2] Fields needed for kicking inactive users 
    time_t last_active;       // Timestamp of last activity (used to determine timeout)
    int has_been_pinged;      // Flag: whether a ping$ warning has already been sent (0 = not yet, 1 = already sent)
    
    struct UserNode *next;    // Pointer to the next user
} UserNode;

UserNode *head = NULL;        // First page of the user list; initially empty

// [Step 5] Synchronization locks
pthread_rwlock_t lock;        // Read-write lock: protects the user list (UserNode linked list)

// [Step 6 / PE 1] History-related variables
char chat_history[MAX_HISTORY][BUFFER_SIZE]; // Ring buffer storing the latest 15 messages
int history_count = 0;        // How many messages are currently stored
int history_end_index = 0;    // Index where the next message should be written
pthread_mutex_t history_lock; // Mutex: specifically protects the history array

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//   Helper functions area

// [Final Step / PE 2] Update last active time
// As long as the user sends a message (including heartbeat), call this function to “extend their life”
void update_last_active(struct sockaddr_in client_addr) {
    pthread_rwlock_wrlock(&lock); // Acquire write lock (because we modify last_active)
    
    UserNode *current = head;
    while (current != NULL) {
        // Find the user via IP and port
        if (current->addr.sin_addr.s_addr == client_addr.sin_addr.s_addr &&
            current->addr.sin_port == client_addr.sin_port) {
            
            // Update time to “now”
            current->last_active = time(NULL);
            // Reset warning flag because the user is active again; timeout will be recalculated next time
            current->has_been_pinged = 0;
            break;
        }
        current = current->next;
    }
    pthread_rwlock_unlock(&lock); // Release lock
}

// [Step 3] Add user to list (with [Step 5] lock and [Final] duplicate check)
void add_user_to_list(struct sockaddr_in addr, char *name) {
    pthread_rwlock_wrlock(&lock); // Acquire write lock: we are modifying the linked list

    //  S1] Duplicate check: prevent the same name from having multiple “ghost copies”
    UserNode *current = head;
    while (current != NULL) {
        if (strcmp(current->name, name) == 0) {
            // If the name already exists, just update its info instead of creating a new node
            current->addr = addr;
            current->last_active = time(NULL);
            current->has_been_pinged = 0;
            
            pthread_rwlock_unlock(&lock);
            printf("SERVER LOG: User [%s] reconnected/updated.\n", name);
            return; 
        }
        current = current->next;
    }

    // If this is a new user, allocate memory and create a node
    UserNode *new_user = (UserNode *)malloc(sizeof(UserNode));
    new_user->addr = addr;
    strcpy(new_user->name, name);
    new_user->last_active = time(NULL); // set time to now
    new_user->has_been_pinged = 0;
    
    // Insert at head: put the new node at the beginning of the list
    new_user->next = head;
    head = new_user;

    pthread_rwlock_unlock(&lock); // Release lock

    printf("SERVER LOG: User [%s] added.\n", name);
}

// [Final Step / PE 2] Inactive user cleanup thread
// This is a separate thread that patrols in the background every 5 seconds
void *check_inactive_clients(void *arg) {
    int sd = *(int *)arg; // Get the socket, because we need it to send ping messages

    while (1) {
        sleep(5); // Sleep for 5 seconds

        time_t now = time(NULL);
        
        // Acquire write lock: we may kick users (delete nodes), so we must use write lock
        pthread_rwlock_wrlock(&lock);

        UserNode *current = head;
        UserNode *prev = NULL;

        while (current != NULL) {
            // Calculate how long they have been inactive (current time - last active time)
            double seconds_inactive = difftime(now, current->last_active);

            //  Case 1: Inactive for too long and already warned before (kick user)
            if (seconds_inactive > (INACTIVITY_THRESHOLD + 10) && current->has_been_pinged) {
                
                printf("SERVER LOG: Kicking inactive user [%s]\n", current->name);
                
                // Send kicked notification
                char kick_msg[] = "You have been removed due to inactivity.";
                udp_socket_write(sd, &current->addr, kick_msg, BUFFER_SIZE);

                // Remove this node from the linked list
                UserNode *to_delete = current;
                if (prev == NULL) {
                    head = current->next; 
                    current = head;
                } else {
                    prev->next = current->next; 
                    current = current->next;
                }
                free(to_delete);
                continue; 
            }
            
            // Case 2: Just reached timeout and not warned yet (send Ping)
            else if (seconds_inactive > INACTIVITY_THRESHOLD && !current->has_been_pinged) {
                
                printf("SERVER LOG: Sending PING to [%s] (inactive for %.0f s)\n", 
                       current->name, seconds_inactive);
                
                char ping_msg[] = "ping$";
                udp_socket_write(sd, &current->addr, ping_msg, BUFFER_SIZE);
                
                // Mark as “warned” to avoid sending infinite pings
                // (During debugging we printed here to confirm the flag was working)
                current->has_been_pinged = 1;
                //printf()
            }

            // Continue checking the next user
            prev = current;
            if (current != NULL) {
                current = current->next;
            }
        }
        
        pthread_rwlock_unlock(&lock); // Patrol finished, release lock
    }
    return NULL;
}

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ :)
//   [Step 6 / PE 1] History functions area


// Save message into the ring buffer
void save_to_history(char *msg) {
    pthread_mutex_lock(&history_lock); // Lock dedicated to history
    
    // Copy the message into the current position
    strncpy(chat_history[history_end_index], msg, BUFFER_SIZE - 1);
    chat_history[history_end_index][BUFFER_SIZE - 1] = '\0';
    
    // Move index
    history_end_index++;
    if (history_end_index >= MAX_HISTORY) history_end_index = 0; // If over 15, wrap back to 0 (ring)
    
    // Increase count
    if (history_count < MAX_HISTORY) history_count++;
    
    pthread_mutex_unlock(&history_lock);
}

// Send the latest 15 messages to a new user
void send_history_to_client(int server_socket, struct sockaddr_in client_addr) {
    pthread_mutex_lock(&history_lock);
    
    if (history_count > 0) {
        char intro_msg[] = "\n--------Chat History (Last 15 messages)---------\n";
        udp_socket_write(server_socket, &client_addr, intro_msg, strlen(intro_msg) + 1);
        
        // Calculate where to start reading (if full, start from end_index, which is the oldest message)
        int start_index = (history_count == MAX_HISTORY) ? history_end_index : 0;
        
        for (int i = 0; i < history_count; i++) {
            int idx = (start_index + i) % MAX_HISTORY; // Use modulo to implement ring reading
            udp_socket_write(server_socket, &client_addr, chat_history[idx], BUFFER_SIZE);
        }
        
        char outro_msg[] = "------------ End of History--------- ---\n\n";
        udp_socket_write(server_socket, &client_addr, outro_msg, strlen(outro_msg) + 1);
    }
    
    pthread_mutex_unlock(&history_lock);
}


//   Command handling logic 


// [Step 3] Handle conn (login) 
void handle_login(int server_socket, struct sockaddr_in sender_addr, char *name) {
    char reply_buffer[BUFFER_SIZE];
    
    // 1. Add user to list
    add_user_to_list(sender_addr, name);
    
    // 2. Send welcome message
    snprintf(reply_buffer, BUFFER_SIZE, "Hi %s, you have successfully connected", name);
    udp_socket_write(server_socket, &sender_addr, reply_buffer, BUFFER_SIZE);
    
    // 3. [PE 1] Send chat history
    send_history_to_client(server_socket, sender_addr);
}

// [Step 4] Handle say (broadcast)
void handle_broadcast(int server_socket, char *sender_name, char *msg) {
    char server_response[BUFFER_SIZE];
    snprintf(server_response, BUFFER_SIZE, "%s: %s", sender_name, msg);
    
    // [PE 1] Before broadcasting, save a copy into history
    save_to_history(server_response);

    // [Step 5] Acquire read lock because we traverse the list to send messages
    pthread_rwlock_rdlock(&lock);
    
    UserNode *current = head;
    while (current != NULL) {
        // Send a copy to everyone on the list
        udp_socket_write(server_socket, &current->addr, server_response, BUFFER_SIZE);
        current = current->next;
    }
    
    pthread_rwlock_unlock(&lock); // Finished sending, release lock
    printf("SERVER LOG: Broadcast from [%s]\n", sender_name);
}

// [Step 4] Handle sayto (private message) 
void handle_private_msg(int server_socket, struct sockaddr_in sender_addr, char *arg_string, char *sender_name) {
    char recipient_name[50], msg[BUFFER_SIZE];
    
    // Parse arguments: split "Bob Hello" into "Bob" and "Hello"
    if (sscanf(arg_string, "%s %[^\n]", recipient_name, msg) < 2) return;

    // [Step 5] Acquire read lock and start searching
    pthread_rwlock_rdlock(&lock);
    
    UserNode *current = head;
    int found = 0;
    while (current != NULL) {
        // Find the user with matching name
        if (strcmp(current->name, recipient_name) == 0) {
            char server_response[BUFFER_SIZE];
            snprintf(server_response, BUFFER_SIZE, "%s: %s (private)", sender_name, msg);
            udp_socket_write(server_socket, &current->addr, server_response, BUFFER_SIZE);
            found = 1; 
            break; // Found, exit loop
        }
        current = current->next;
    }
    
    pthread_rwlock_unlock(&lock); // Done searching, release lock
    
    if (!found) {
        char err[] = "User not found";
        udp_socket_write(server_socket, &sender_addr, err, BUFFER_SIZE);
    }
}

//   [Step 1 & Step 3] Main program
// ======================================
int main(int argc, char *argv[]) {
    // 1. Open socket
    int server_socket = udp_socket_open(SERVER_PORT);
    if (server_socket < 0) return 1;

    // 2. [Step 5] Initialize locks
    pthread_rwlock_init(&lock, NULL);
    pthread_mutex_init(&history_lock, NULL);

    // 3. [Final / PE 2] Start the inactive-user cleanup thread
    pthread_t cleaner_thread;
    pthread_create(&cleaner_thread, NULL, check_inactive_clients, &server_socket);

    printf("Server listening on port %d...\n", SERVER_PORT);

    // 4. Main loop: block and wait for messages
    while (1) {
        struct sockaddr_in client_addr;
        char client_request[BUFFER_SIZE];
        
        int rc = udp_socket_read(server_socket, &client_addr, client_request, BUFFER_SIZE);
        
        if (rc > 0) {
            // Final / PE 2 As long as a message is received, the user is alive; update their active time
            update_last_active(client_addr);

            // 5. Step 3 Split message (command $ content)
            char temp_buffer[BUFFER_SIZE];
            strcpy(temp_buffer, client_request);
            char *command = strtok(temp_buffer, "$");
            char *arg = strtok(NULL, "$");
            
            if (command != NULL) {
                // [Final / PE 2] Handle heartbeat reply
                if (strcmp(command, "ret-ping") == 0) {
                    // Ignore, because update_last_active has already updated the time
                    continue;
                }

                if (arg != NULL) {
                    // Case A: login 
                    if (strcmp(command, "conn") == 0) {
                        handle_login(server_socket, client_addr, arg);
                    } 
                    // Case B: chat (need to look up the name first)
                    else {
                        char *sender_name = NULL;
                        
                        // [Step 4] Look up name by IP (read lock)
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
