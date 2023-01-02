#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65535
#define MAX_EVENT_NUMBER 10000

extern void addfd(int epollfd, int fd, bool one_shot);
extern void removefd(int epollfd, int fd);
extern void modfd(int epollfd, int fd, int ev);

void addsig(int sig, void(*header)(int) ) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = header;
    sigfillset(&sa.sa_mask);
    assert( sigaction(sig, &sa, NULL) != -1 );
}

int main(int argc, char* argv[]) {
    if (argc <= 1) {
        printf( "usage: %s port_number\n", basename(argv[0]));
        return 1;
    }

    //接收端口，处理SIGPIPE信号
    int port = atoi(argv[1]);
    addsig(SIGPIPE, SIG_IGN);
    
    //创建线程池
    threadpool<http_conn>* pool = NULL;
    try{
        pool = new threadpool<http_conn>;
    }catch(...) {
        return 1;
    }

    //创建http_conn用户组
    http_conn* users = new http_conn[MAX_FD];

    //创建监听的文件描述符
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    if (listenfd == -1) {
        perror("socket");
        return 1;
    }

    //绑定、监听端口
    struct sockaddr_in saddr;
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(port);
    saddr.sin_addr.s_addr = INADDR_ANY;

    int ret = 0;
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)); //端口复用
    ret = bind(listenfd, (struct sockaddr *)&saddr, sizeof(saddr));
    ret = listen(listenfd, 5);

    //创建epoll
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    //添加到epoll中
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    while(1) {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);

        if (number < 0 && errno != EINTR) {
            printf("epoll failure\n");
            break;
        }

        for (int i = 0; i < number; ++ i) {
            int sockfd = events[i].data.fd;

            if (sockfd == listenfd) {
                struct sockaddr_in caddr;
                socklen_t len = sizeof(caddr);
                int connfd = accept(listenfd, (struct sockaddr*)&caddr, &len);

                if (connfd < 0) {
                    printf("erron is %d\n", errno);
                    continue;
                }

                if (http_conn::m_user_count >= MAX_FD) {
                    close(connfd);
                    continue;
                }

                users[connfd].init(connfd, caddr);
            } else if (events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR )){
                users[sockfd].close_conn();
            } else if (events[i].events & EPOLLIN) {
                if (users[sockfd].read()) //一次性读完数据
                   pool -> append(users + sockfd);
                else
                   users[sockfd].close_conn();
            } else if (events[i].events & EPOLLOUT) {
                if (!users[sockfd].write()) {
                    users[sockfd].close_conn();
                }
            }
        }
    }  

    close(epollfd);
    close(listenfd);
    delete []users;
    delete pool;

    return 0;
}

