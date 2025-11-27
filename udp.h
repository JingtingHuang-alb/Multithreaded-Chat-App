
// libraries needed for various functions
// use man page for details
#include <sys/types.h>  // data types like size_t, socklen_t
#include <sys/socket.h> // socket(), bind(), connect(), listen(), accept()
#include <netinet/in.h> // sockaddr_in, htons(), htonl(), INADDR_ANY
#include <arpa/inet.h>  // inet_pton(), inet_ntop()
#include <unistd.h>     // close()
#include <string.h>     // memset(), memcpy()
#include <assert.h>
#include <pthread.h>

#define BUFFER_SIZE 1024
#define SERVER_PORT 12000

int set_socket_addr(struct sockaddr_in *addr, const char *ip, int port)
{
    // This is a helper function that fills
    // data into an address variable of the
    // type struct sockaddr_in

    // Basic initialization (sin stands for 'socket internet')
    memset(addr, 0, sizeof(*addr)); // zero the whole memory
    addr->sin_family = AF_INET;     // use IPv4
    addr->sin_port = htons(port);   // host-to-network short: required to store port in network byte order

    if (ip == NULL)
    { // special behaviour for using IP 0.0.0.0 (INADDR_ANY)
        addr->sin_addr.s_addr = INADDR_ANY;
    }
    else
    {
        // Convert dotted-decimal string (e.g., "127.0.0.1") to binary.
        // In other words: inet_pton will parse the human-readable IP string
        // and write the corresponding 32-bit binary value into sin_addr
        if (inet_pton(AF_INET, ip, &addr->sin_addr) <= 0)
        {
            return -1; // invalid IP string
        }
    }
    return 0;
}

int udp_socket_open(int port)
{

    // 1. Create a UDP socket and obtain a socket descriptor
    // (sd) to it
    int sd = socket(AF_INET, SOCK_DGRAM, 0);

    // 2. Create an address variable to associate
    // (or bind) with the socket
    struct sockaddr_in this_addr;

    // 3. Fill in the address variable with IP and Port.
    // Having ip = NULL (second parameter below), sets IP to 0.0.0.0
    // which represents all network interfaces (IP addresses)
    // for this machine.
    set_socket_addr(&this_addr, NULL, port);

    // 4. Bind (associate) this address with the socket
    // created earlier.
    /// Note: binding with 0.0.0.0 means that the socket will accept
    // packets coming in to any interface (ip address) of this machine
    bind(sd, (struct sockaddr *)&this_addr, sizeof(this_addr));

    return sd; // return the socket descriptor
}

int udp_socket_read(int sd, struct sockaddr_in *addr, char *buffer, int n)
{
    // Receive up to n bytes into buffer from the socket with descriptor sd.
    // The source address (addr) of the sender is stored in addr for later use.

    // Note: recvfrom is a blocking call, meaning that the function will not return
    // until a packet arrives (or an error/timeout occurs). During this time, the
    // calling thread is put to sleep by the kernel and placed on a wait queue, and
    // it resumes only when recvfrom completes.

    // The fourth parameter 'flags' of recvfrom is normally set to 0

    socklen_t len = sizeof(struct sockaddr_in);
    return recvfrom(sd, buffer, n, 0, (struct sockaddr *)addr, &len);
}

int udp_socket_write(int sd, struct sockaddr_in *addr, char *buffer, int n)
{
    // Send the contents of buffer (n bytes) to the given destination
    // address (addr) over UDP (through socket with descriptor sd)

    // addr_len: tells the kernel how many bytes of addr are valid
    // (needed to distinguish IPv4 vs IPv6 etc.)

    // Note: sendto may also block if the kernel’s send buffer is full, which usually
    // happens only under high load or network pressure. In that case, the function will
    // not return until buffer space becomes available (or an error/timeout occurs).
    // During this time, the calling thread is put to sleep by the kernel and placed on
    // a wait queue, and it resumes only when sendto completes.

    // The fourth parameter 'flags' of sendto is normally set to 0

    int addr_len = sizeof(struct sockaddr_in);
    return sendto(sd, buffer, n, 0, (struct sockaddr *)addr, addr_len);
}