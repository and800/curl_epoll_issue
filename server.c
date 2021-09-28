#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>

void log_timestamp() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    char timestamp[16];
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", gmtime(&tv.tv_sec));
    fprintf(stderr, "[%s.%d] ", timestamp, tv.tv_usec);
}

#define LOG(msg) do {     \
    log_timestamp();      \
    fprintf(stderr, msg); \
} while (0)

#define PANIC(msg) do { \
    LOG(msg);           \
    perror(NULL);       \
    exit(1);            \
} while (0)

char http_buffer[65536];

char *response_head =
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: 17\r\n"
    "Content-Type: application/json\r\n";

char *response_body = "\r\n{\"payload\": 1337}";

void handle_request(int sock, int close);

int main(int argc, char **argv) {
    int port = 8000;
    if (argc > 1) {
        port = atoi(argv[1]);
        LOG("Using port ");
        fprintf(stderr, "%d\n", port);
    } else {
        LOG("Using default port ");
        fprintf(stderr, "%d\n", port);
    }

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (struct sockaddr *)&addr, sizeof(addr))) {
        PANIC("bind\n");
    }

    listen(server_sock, 8);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);

        int sock = accept(
            server_sock,
            (struct sockaddr *)&client_addr,
            &addrlen
        );
        if (sock == -1) {
            PANIC("accept\n");
        }
        LOG("TCP connection established!\n");

        int shall_close = 0;
        for (int i = 0; i < 3; i++) {
            LOG("Waiting for http request #");
            fprintf(stderr, "%d\n", i + 1);
            handle_request(sock, shall_close);
        }
        shall_close = 1;
        LOG("Waiting for last http request, then close\n");
        handle_request(sock, shall_close);
    }
    return 0;
}

#define min(a, b) (a < b ? a : b)

void handle_request(int sock, int shall_close) {
    int http_buffer_size = sizeof(http_buffer);

    int buf_len = 0;
    int content_len = -1;
    int header_len = -1;

    memset(http_buffer, 0, http_buffer_size);

    fprintf(stderr, "<<<\n");

    while (header_len == -1 || buf_len < content_len + header_len) {
        sleep(1);
        int chunksize = read(
            sock,
            (void *)(http_buffer + buf_len),
            min(http_buffer_size - buf_len, 25)
        );
        buf_len += chunksize;

        if (chunksize == -1) {
            PANIC("read\n");
        }
        if (chunksize == 0) {
            PANIC("Error: incomplete http request, exit\n");
        }

        fprintf(stderr, "%.*s", chunksize, http_buffer + buf_len - chunksize);

        for (int i = 0; i < buf_len; i++) {
            if (header_len != -1) {
                break;
            }
            if (strncmp(http_buffer + i, "\r\n\r\n", 4) == 0) {
                header_len = i + 4;
                if (content_len == -1) {
                    content_len = 0;
                }
                break;
            }
            char *cl_header = "Content-Length:";
            char cl_value[8];
            if (strncmp(http_buffer + i, cl_header, strlen(cl_header)) == 0) {
                int j = i + strlen(cl_header);
                while (http_buffer[j] == ' ') {
                    j++;
                }
                int k = 0;
                while (http_buffer[j] != '\r' && http_buffer[j] != 0) {
                    cl_value[k++] = http_buffer[j++];
                }
                if (http_buffer[j] == 0) {
                    break;
                }
                cl_value[k] = 0;
                content_len = atoi(cl_value);
            }
        }
    }
    fprintf(stderr, "\n");
    LOG("recv ok\n");

    sleep(1);

    fprintf(stderr, ">>>\n");

    memset(http_buffer, 0, http_buffer_size);

    strcpy(http_buffer, response_head);
    buf_len = strlen(response_head);
    char *ka_header;
    if (shall_close) {
        ka_header = "Connection: close\r\n";
    } else {
        ka_header = "Connection: keep-alive\r\n";
    }
    strcpy(http_buffer + buf_len, ka_header);
    buf_len += strlen(ka_header);
    strcpy(http_buffer + buf_len, response_body);
    buf_len += strlen(response_body);

    int send_start = 0;
    while (send_start < buf_len) {
        sleep(1);
        int chunksize = write(
            sock,
            (void *)(http_buffer + send_start),
            min(buf_len - send_start, 25)
        );
        send_start += chunksize;

        if (chunksize == -1) {
            PANIC("write\n");
        }
        if (chunksize == 0) {
            PANIC("Error: peer not consuming response, exit\n");
        }

        fprintf(stderr, "%.*s", chunksize, http_buffer + send_start - chunksize);
    }
    fprintf(stderr, "\n");
    LOG("send ok\n");
    sleep(1);

    if (shall_close) {
//        LOG("Closing\n");
//        close(sock);
    }
}
