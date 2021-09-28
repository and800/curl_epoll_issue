#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <curl/curl.h>
#include <sys/errno.h>

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

#define PANIC_C(msg, code) do {                      \
    LOG(msg);                                        \
    fprintf(stderr, "%s", curl_easy_strerror(code)); \
    exit(1);                                         \
} while (0)

#define PANIC_CM(msg, code) do {                      \
    LOG(msg);                                         \
    fprintf(stderr, "%s", curl_multi_strerror(code)); \
    exit(1);                                          \
} while (0)

int epfd;
int curl_timerfd;
int app_timerfd;

void acknowledge_timer(int timerfd) {
    uint64_t count;
    if (read(timerfd, &count, sizeof(count)) == -1) {
        if (errno != EAGAIN) {
            PANIC("timer read\n");
        }
    }
}

int socket_callback(CURL *easy, curl_socket_t s, int what, void *userp, void *socketp) {
    LOG("socket monitor requested ");
    fprintf(stderr, "fd %d action %d\n", s, what);

    struct epoll_event event;
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, s, &event)) {
        if (errno != ENOENT) {
            PANIC("epoll_ctl_del failure\n");
        }
        LOG("warning: epoll_ctl_del fd not found\n");
    }

    event.events = ((what & CURL_POLL_IN) ? EPOLLIN : 0) |
                   ((what & CURL_POLL_OUT) ? EPOLLOUT : 0);
    event.data.fd = s;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, s, &event)) {
        PANIC("epoll_ctl_add failure\n");
    }

    return 0;
}


int timer_callback(CURLM *multi, long timeout_ms, void *userp) {
    LOG("curl timeout requested\n");
    struct itimerspec its;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 0;

    if (timeout_ms > 0) {
        its.it_value.tv_sec = timeout_ms / 1000;
        its.it_value.tv_nsec = (timeout_ms % 1000) * 1000 * 1000;
    } else if (timeout_ms == 0) {
        its.it_value.tv_sec = 0;
        its.it_value.tv_nsec = 1;
    } else {
        its.it_value.tv_sec = 3600;
        its.it_value.tv_nsec = 0;
    }
    timerfd_settime(curl_timerfd, 0, &its, NULL);
    return 0;
}

void app_sleep() {
    LOG("app sleep start\n");
    struct itimerspec its;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 0;
    its.it_value.tv_sec = 5;
    its.it_value.tv_nsec = 0;
    timerfd_settime(app_timerfd, 0, &its, NULL);
}

size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    return nmemb;
}

void check_multi_info(CURLM *multi) {
    LOG("check multi info\n");
    CURLMsg *msg;
    int msgq;

    while ((msg = curl_multi_info_read(multi, &msgq))) {
        if (msg->msg == CURLMSG_DONE) {
            LOG("request completed!\n");
            curl_multi_remove_handle(multi, msg->easy_handle);

            // pretend to process received response in a child process
            int pid = fork();
            if (pid == 0) {
                sleep(3600);
                exit(0);
            } else if (pid < 0) {
                PANIC("fork\n");
            } else {
                LOG("child process created\n");
            }

            // schedule next request:
            app_sleep();
        }
    }
}

int main(int argc, char **argv) {
    char *port = "8000";
    if (argc > 1) {
        port = argv[1];
        LOG("Using port ");
        fprintf(stderr, "%s\n", port);
    } else {
        LOG("Using default port ");
        fprintf(stderr, "%s\n", port);
    }

    // init epoll, timers

    epfd = epoll_create1(0);
    struct epoll_event event;

    curl_timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    event.events = EPOLLIN;
    event.data.fd = curl_timerfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, curl_timerfd, &event);

    app_timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    event.events = EPOLLIN;
    event.data.fd = app_timerfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, app_timerfd, &event);

    // init curl
    char url[32];
    snprintf(url, sizeof(url), "http://localhost:%s/", port);

    CURL *easy = curl_easy_init();
    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_VERBOSE, 1);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, "{\"payload\": 1337}");

    CURLM *multi = curl_multi_init();
    curl_multi_setopt(multi, CURLMOPT_SOCKETFUNCTION, socket_callback);
    curl_multi_setopt(multi, CURLMOPT_TIMERFUNCTION, timer_callback);
    int pending = 0;

    // kickstart the app
    app_sleep();

    // enter event loop
    struct epoll_event events[8];
    while (1) {
        LOG("entering epoll_wait\n");
        int ev_count = epoll_wait(
            epfd,
            events,
            sizeof(events)/sizeof(struct epoll_event),
            -1
        );
        if (ev_count == -1) {
            PANIC("epoll_wait\n");
        }
        LOG("epoll_wait returned\n");

        for (int i = 0; i < ev_count; i++) {
            if (events[i].data.fd == app_timerfd) {
                LOG("app sleep end, perform request\n");
                acknowledge_timer(app_timerfd);
                curl_multi_add_handle(multi, easy);
            } else if (events[i].data.fd == curl_timerfd) {
                LOG("curl timeout fired\n");
                acknowledge_timer(curl_timerfd);
                LOG("SOCKET ACTION ENTER\n");
                int err = curl_multi_socket_action(multi, CURL_SOCKET_TIMEOUT, 0, &pending);
                if (err != CURLM_OK) {
                    PANIC_CM("socket action by timer\n", err);
                }
                LOG("SOCKET ACTION EXIT\n");
                check_multi_info(multi);
            } else {
                LOG("some socket became ready\n");
                int sock = events[i].data.fd;
                int ep_action = events[i].events;
                int curl_action = ((ep_action & EPOLLIN) ? CURL_CSELECT_IN : 0) |
                                  ((ep_action & EPOLLOUT) ? CURL_CSELECT_OUT : 0);
                LOG("SOCKET ACTION ENTER\n");
                int err = curl_multi_socket_action(multi, sock, curl_action, &pending);
                if (err != CURLM_OK) {
                    PANIC_CM("socket action by socket\n", err);
                }
                LOG("SOCKET ACTION EXIT\n");
                check_multi_info(multi);
            }
        }
    }
    return 0;
}
