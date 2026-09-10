//server.cpp
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <climits>
#include <sys/stat.h>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>

#define SERVERPORT 8989     //port number for the server to listen on
#define BUFFSIZE 4096       //size of buffer for receiving data from clients
#define SOCKETERROR (-1)    //value returned by socket functions on error
#define SERVERBACKLOG 100   //number of pending connections the server can have in its queue
#define THREADPOOLSIZE 20   //number of threads in thread pool for handling client connections

typedef struct sockaddr_in SA_IN;   //alias for struct sockaddr_in
typedef struct sockaddr SA;         //alias for struct sockaddr

const char* get_mime_type(const char* path);   //function prototype for determining the MIME type based on file extension
const char* get_header_value(const char* buffer, const char* header_name);   //function prototype for extracting the value of a specific HTTP header from the request buffer
void handle_connection(int client_socket);   //function prototype for handling client connections
int check(int exp, const char* msg);          //function prototype for checking return values of socket functions and printing error messages

//ThreadPool, owns worker threads, the client-socket queue, and all
//synchronization primitives needed to coordinate them, constructed once in
//main(); its destructor joins every thread cleanly (RAII)
class ThreadPool {
public:
    //launch 'size' worker threads, each of which loops waiting for client sockets
    ThreadPool(int size) {
        for (int i = 0; i < size; i++) {
            //each thread runs thread_function, capturing 'this' so it can reach
            //queue, mutex, and condition variable
            threads.emplace_back(&ThreadPool::thread_function, this);
        }
    }

    //signal threads to stop, then join each one before the object is destroyed
    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(mutex);
            stop = true;    //tell all waiting threads wake up and exit
        }
        condition_var.notify_all();     //wake every thread so it can see stop == true
        for (std::thread& t : threads) {
            t.join();   //wait for each thread to finish before returning
        }
    }

    //called by main() to hand off an accepted client socket to pool
    void enqueue(int client_socket) {
        {
            std::unique_lock<std::mutex> lock(mutex);
            client_queue.push(client_socket);   //add client socket descriptor to queue for threads to handle
        }
        condition_var.notify_one();     //wake exactly one idle thread to service new connection
    }

private:
    std::vector<std::thread> threads;           //pool of worker threads, replaces pthread_t array
    std::queue<int> client_queue;               //queue of accepted client socket descriptors waiting to be handled
    std::mutex mutex;                           //protects client_queue from concurrent access, replaces pthread_mutex_t
    std::condition_variable condition_var;      //signals idle threads when a new socket is enqueued, replaces pthread_cond_t
    bool stop = false;                          //set true in destructor to break every thread's wait loop

    //entry point for every worker thread; loops indefinitely until stop is set
    void thread_function() {
        while (true) {
            int client_socket;

            {
                //unique_lock replaces pthread_mutex_lock/unlock and is released
                //automatically when the block exits, even if an exception is thrown
                std::unique_lock<std::mutex> lock(mutex);

                //cv.wait() atomically releases lock and suspends thread;
                //it re-acquires lock and returns only when lambda is true
                //replacing manual pthread_cond_wait() loop
                condition_var.wait(lock, [this] {
                    return !client_queue.empty() || stop;   //wake if there is work or shutdown was requested
                });

                if (stop && client_queue.empty()) return;   //drain queue before exiting so no connections are dropped

                client_socket = client_queue.front();   //take next client socket descriptor from front of queue
                client_queue.pop();                     //remove it so another thread won't pick it up
            }   //lock released here, other threads can now enqueue or dequeue

            handle_connection(client_socket);   //handle client connection outside lock so other threads aren't blocked
        }
    }
};

//Socket, wraps a single server-side TCP socket, constructor creates,
//binds, and starts listening; destructor closes file descriptor
//automatically (RAII), so main() never needs to call close() explicitly
class Socket {
public:
    //create the socket, bind it to SERVERPORT on all interfaces, and begin listening
    Socket(int port) {
        check((fd = socket(AF_INET, SOCK_STREAM, 0)), "Failed to create socket");   //create a TCP socket for the server to listen on

        //initialize the address structure for the server
        SA_IN server_addr;
        server_addr.sin_family = AF_INET;           //set the address family to IPv4
        server_addr.sin_addr.s_addr = INADDR_ANY;   //accept connections on all available network interfaces
        server_addr.sin_port = htons(port);         //convert the port number to network byte order

        check(bind(fd, (SA*)&server_addr, sizeof(server_addr)), "Failed to bind socket");   //bind the socket to the specified address and port
        check(listen(fd, SERVERBACKLOG), "Failed to listen on socket");                     //start listening for incoming connections on the socket
    }

    //close the socket file descriptor when the Socket object goes out of scope
    ~Socket() {
        if (fd != SOCKETERROR) {
            close(fd);
        }
    }

    //accept the next incoming connection and return the resulting client socket descriptor
    int accept_connection(SA_IN& client_addr) {
        socklen_t addr_size = sizeof(client_addr);
        return check(accept(fd, (SA*)&client_addr, &addr_size), "Failed to accept connection");
    }

    //prevent copying, a socket file descriptor must not be owned by two objects at once
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

private:
    int fd;     //the underlying socket file descriptor managed by this object
};

int main(int argc, char **argv) {
    ThreadPool pool(THREADPOOLSIZE);    //start the worker threads; the pool lives for the duration of main()
    Socket server(SERVERPORT);          //create, bind, and listen on SERVERPORT; closed automatically when main() returns

    while (true) {
        printf("Waiting for a connection...\n");

        SA_IN client_addr;
        int client_socket = server.accept_connection(client_addr);  //block until a client connects, then get its socket descriptor

        printf("Connection accepted from %s:%d\n",
               inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port));    //print the IP address and port number of the connected client

        pool.enqueue(client_socket);    //hand the socket off to the thread pool, an idle worker will pick it up
    }

    return 0;
}

int check(int exp, const char* msg) {
    if (exp == SOCKETERROR) {   //if the expression evaluates to SOCKETERROR, print the error message and exit the program
        perror(msg);
        exit(1);
    }
    return exp;     //return the value of the expression if it does not evaluate to SOCKETERROR
}

//returns the correct MIME type string for a given file path
//based on its extension. Defaults to "application/octet-stream"
//which tells the browser to treat it as raw binary data if unknown
const char* get_mime_type(const char* path) {
    //strrchr finds the LAST occurrence of '.' in the path
    //this gives us the file extension
    const char* ext = strrchr(path, '.');

    //if there is no extension, return a safe default
    if (ext == NULL) return "application/octet-stream";

    //move past the dot so ext now points to "html", "css", etc.
    ext++;

    //strcmp returns 0 when strings match
    if (strcmp(ext, "html") == 0 || strcmp(ext, "htm") == 0) { return "text/html"; }
    if (strcmp(ext, "css") == 0) { return "text/css"; }
    if (strcmp(ext, "js") == 0) { return "text/javascript"; }
    if (strcmp(ext, "png") == 0) { return "image/png"; }
    if (strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0) { return "image/jpeg"; }
    if (strcmp(ext, "ico") == 0) { return "image/x-icon"; }
    if (strcmp(ext, "json") == 0) { return "application/json"; }
    if (strcmp(ext, "txt") == 0) { return "text/plain"; }

    //unknown extension, tell browser to treat as raw binary
    return "application/octet-stream";
}

//searches the raw request buffer for a header by name and returns its value
//for example: get_header_value(buffer, "Content-Length") returns "27"
//returns NULL if the header is not found
const char* get_header_value(const char* buffer, const char* header_name) {
    // search for the header name in the buffer
    const char* pos = strstr(buffer, header_name);
    if (pos == NULL) return NULL;   // header not present

    // move past the header name and the ": " separator
    pos += strlen(header_name);
    if (*pos != ':') return NULL;   // malformed header
    pos++;                          // skip the colon
    while (*pos == ' ') pos++;      // skip any spaces after the colon

    return pos;     // now pointing at the start of the value
}

void handle_connection(int client_socket) {
    char buffer[BUFFSIZE];  //buffer for receiving data from the client
    ssize_t bytes_read;     //variable to store the number of bytes read from the client
    int msgsize = 0;        //variable to keep track of the total size of the message received from the client

    while ((bytes_read = read(client_socket, buffer + msgsize, sizeof(buffer) - msgsize - 1)) > 0) {    //read until we find \r\n\r\n (end of HTTP headers)
        msgsize += bytes_read;
        buffer[msgsize] = '\0';     //keep it null-terminated so strstr works
        if (strstr(buffer, "\r\n\r\n") != NULL) {   //end of headers found, stop reading
            break;
        }
        if (msgsize >= BUFFSIZE - 1) {  //if the buffer is full and we haven't found the end of headers, stop reading to avoid overflow
            break;
        }
    }

    //initialize all three buffers to empty strings so we can detect if sscanf failed to fill them
    char method[16] = {0}, path[256] = {0}, version[16] = {0};

    //sscanf returns the number of items successfully parsed a valid HTTP request line always has exactly 3 fields
    int parsed = sscanf(buffer, "%15s %255s %15s", method, path, version);

    //if we didn't get all 3 fields, the request is malformed
    if (parsed != 3 || strlen(method) == 0 || strlen(path) == 0) {
        const char* response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
        send(client_socket, response, strlen(response), 0);
        close(client_socket);
        return;
    }

    printf("Method: %s  Path: %s  Version: %s\n", method, path, version);

    if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0) {   //handle GET and POST requests, return 405 Method Not Allowed for other methods
        const char* response = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n";
        send(client_socket, response, strlen(response), 0);
        close(client_socket);
        return;
    }

    // handle POST requests
    if (strcmp(method, "POST") == 0) {
        // find Content-Length header to know how many bytes the body is
        const char* content_length_str = get_header_value(buffer, "Content-Length");
        int content_length = 0;
        if (content_length_str != NULL) {
            content_length = atoi(content_length_str);  // convert string to int
        }

        // the body starts after the \r\n\r\n in the buffer
        // strstr finds that position for us
        char* body_start = strstr(buffer, "\r\n\r\n");
        if (body_start != NULL) {
            body_start += 4;    // move past the \r\n\r\n itself
            printf("POST body (%d bytes): %.*s\n", content_length, content_length, body_start);
            // %.*s prints exactly content_length characters, even if there's no null terminator
        }

        // for now, acknowledge the POST with a simple 200 OK
        // in Stage 5 this will be replaced with actual routing logic
        const char* response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 15\r\n"
            "\r\n"
            "{\"status\":\"ok\"}";
        send(client_socket, response, strlen(response), 0);
        close(client_socket);
        return;
    }

    //build the file path, serve files from current directory
    char filepath[PATH_MAX];
    if (strcmp(path, "/") == 0) {
        snprintf(filepath, sizeof(filepath), "./index.html");
    } else {
        snprintf(filepath, sizeof(filepath), ".%s", path);
    }
    //.%s prepends a dot to the path, so /index.html becomes ./index.html

    //check if the file exists and get its size for the Content-Length header
    struct stat file_stat;
    if (stat(filepath, &file_stat) < 0) {   //if the file does not exist, return 404 Not Found
        const char* response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        send(client_socket, response, strlen(response), 0);
        close(client_socket);
        return;
    }

    //send HTTP response headers
    char headers[256];
    const char* mime_type = get_mime_type(filepath);   // look up the MIME type
    snprintf(headers, sizeof(headers),
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: %lld\r\n"
        "Content-Type: %s\r\n"           // ← %s filled in by mime_type
        "\r\n",
        (long long)file_stat.st_size,
        mime_type);
    send(client_socket, headers, strlen(headers), 0);   //send the HTTP response headers to the client, including the Content-Length header with the size of the file being served

    //open and send the file
    FILE* file = fopen(filepath, "r");
    if (file == NULL) {
        const char* response = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
        send(client_socket, response, strlen(response), 0);
        close(client_socket);
        return;
    }

    //read the file in chunks and send it to the client until the entire file has been sent
    char filebuffer[BUFFSIZE];
    size_t bytes_sent;
    while ((bytes_sent = fread(filebuffer, 1, BUFFSIZE, file)) > 0) {
        send(client_socket, filebuffer, bytes_sent, 0);
    }

    fclose(file);
    close(client_socket);
    printf("Response sent and connection closed.\n");
}