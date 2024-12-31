
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#include "simple_event.h"
#include "simple_event_macro.h"

typedef sev_base evb;
typedef sev_io_event io_evt;
typedef sev_custom_event cus_evt;

int tm_compare(struct timeval *point_time, struct timeval *cur)
{
    if ((cur->tv_sec - point_time->tv_sec) > 0)
    {
        return 1;
    }
    if ((cur->tv_sec - point_time->tv_sec) == 0 &&
        (cur->tv_usec - point_time->tv_usec) > 0)
    {
        return 1;
    }

    return 0;
}

void on_tcp_data(int fd, int what, void *arg)
{
    static int total = 0;
    const int interval = 2;
    static struct timeval point = {0};
    struct timeval cur;
    gettimeofday(&cur, NULL);
    if (point.tv_sec == 0&& point.tv_usec == 0)
    {
        point = cur;
        point.tv_sec+=interval;
    }

    char buf[1024]={0};
    int buf_len = sizeof(buf);
    if (what & SEV_IO_READABLE)
    {
        while (1)
        {
            int ret = read(fd, buf, buf_len);
            if (ret == 0)//对端关闭了连接
            {
                remove_io_event(arg, fd, 1);
                break;
            }
            

            if ((ret < 0) && (errno != EAGAIN) && (errno != EINTR))
            {
                
            }
            else if(ret < 0)
            {
                break;
            }

            // LOG("%s",buf);
            total+=ret;
            if (tm_compare(&point,&cur))
            {
                point.tv_sec += interval;
                int speed = total/interval;
                total = 0;
                LOG("speed %d bytes/s",speed);
            }
            if (ret < buf_len)//读完了
            {
                break;
            }
        }
    }
    else if (what & SEV_IO_ERROR)
    {
        remove_io_event(arg, fd, 1);
    }
    
}

void on_conn(int fd, int what, void *arg)
{
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    if (what & SEV_IO_READABLE)
    {
        int conn_fd = accept(fd, (struct sockaddr *)&client_addr, &client_addr_len);

        int flags = fcntl(conn_fd, F_GETFL, 0);
        flags |= O_NONBLOCK;
        int res = fcntl(conn_fd, F_SETFL, flags);

        io_evt* ev_tcp = new_io_event(conn_fd, SEV_IO_READABLE|SEV_IO_WRITEABLE|SEV_IO_ERROR, 1, on_tcp_data, arg);
        add_io_event((sev_base*)arg, ev_tcp);
    }
}

static void make_addr(struct sockaddr_in *addr, char *ip, int port)
{
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    addr->sin_addr.s_addr = ip == 0 ? INADDR_ANY : inet_addr(ip);
}
int make_tcp_sock(struct sockaddr_in *local_addr)
{
    int res = 0;
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);

    int flags = fcntl(sockfd, F_GETFL, 0);
    flags |= O_NONBLOCK;
    res = fcntl(sockfd, F_SETFL, flags);
    if (res < 0)
    {
        close(sockfd);
        return -1;
    }

    res = bind(sockfd, (const struct sockaddr *)local_addr, sizeof(struct sockaddr_in));
    if (res < 0)
    {
        close(sockfd);
        return -1;
    }
    return sockfd;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <serverport> \n", argv[0]);
        return 1;
    }

    int serverport = atoi(argv[1]);

    sev_base *base = sev_new_base();

    struct sockaddr_in local;
    struct sockaddr_in *local_addr = &local;
    make_addr(local_addr, 0, serverport);
    int sock = make_tcp_sock(local_addr);
    
    listen(sock, 0);

    io_evt* ev_tcp = new_io_event(sock, SEV_IO_READABLE|SEV_IO_WRITEABLE|SEV_IO_ERROR, 1, on_conn, base);
    add_io_event(base, ev_tcp);


    // cus_evt *ev_data = new_cus_event(1, 0, 0, data_cb, (void *)&test);
    // test.ev_data = ev_data;

    // struct timeval tv = {.tv_sec = 0, .tv_usec = MAKE_DATA_INTERVAL};
    // add_cus_event(test.eb, ev_data, &tv);

    sev_loop(base);

    free_io_event(ev_tcp);
    sev_free_base(base);
}

/* ****************  test  ***************** */