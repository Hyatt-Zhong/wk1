#include "simple_event_macro.h"
#include "simple_event.h"
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>

typedef struct custom_test_ctx_
{
    int x;
    void (*foo)(void *data);

    sev_base *base;
    sev_custom_event *ev1, *ev2, *ev3;
    sev_io_event *ioev1, *ioev2, *ioev3;
} custom_test_ctx;

void handler1(int fd, int event, int is_overtime, void *ctx)
{
    if (is_overtime || event)
    {
        if (is_overtime)
        {
            LOG("fd = %d ,event = %d, is_overtime = %d", fd, event, is_overtime);
        }
        else
        {
            LOG("%s event = %d", __func__, event);
        }

        custom_test_ctx *con = (custom_test_ctx *)ctx;

        struct timeval ot1 = {.tv_sec = 1, .tv_usec = 0};
        add_cus_event(con->base, con->ev3, &ot1);
        active_cus_event(con->ev3, CUSTOM_STATUS1);
        active_cus_event(con->ev3, CUSTOM_STATUS3);
        con->foo(con);
    }
}
void handler2(int fd, int event, int is_overtime, void *ctx)
{
    sev_base *base = (sev_base *)ctx;
    base->stop = 1;
    LOG("stop");
}
void handler3(int fd, int event, int is_overtime, void *ctx)
{
    static int flag = 0;
    if (event)
    {
        LOG("%s event = %d", __func__, event);
        custom_test_ctx *con = (custom_test_ctx *)ctx;
        flag = !flag;
        if (flag)
        {
            active_cus_event(con->ev1, CUSTOM_STATUS1);
        }
    }
    if (is_overtime)
    {
        LOG("%s is_overtime", __func__);
    }
}

void handler4(int fd, int event, int is_overtime, void *ctx)
{
    static int flag = 0;
    flag++;
    if (flag >= 10000)
    {
        flag = 0;
        TRACE;
    }
}

void io_handler1(int fd, int event, void *ctx)
{
    char buf[1024] = {0};
    int rlen = read(fd, buf, sizeof(buf));

    LOG("%s, event = %d", __func__, event);
    printf("%s", buf);
}
void io_handler2(int fd, int event, void *ctx)
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    char buf[1024] = {0};
    recvfrom(fd, buf, 1024, 0, (struct sockaddr *)&client_addr, &addr_len);

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
    int client_port = ntohs(client_addr.sin_port);

    LOG("from %s:%d %s", client_ip, client_port, buf);

    sev_base *base = (sev_base *)ctx;
    if (strcmp(buf, "stop") == 0)
    {
        base->stop = 1;
        LOG("stop");
    }

    if (strcmp(buf, "del stdin") == 0)
    {
        remove_io_event(base, STDIN_FILENO);
        LOG("del stdin");
    }
}

void io_handler3(int fd, int event, void *arg)
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    char buf[1024] = {0};
    recvfrom(fd, buf, 1024, 0, (struct sockaddr *)&client_addr, &addr_len);
    
    custom_test_ctx *ctx = (custom_test_ctx *)arg;
    if (strcmp(buf, "add stdin") == 0)
    {
        add_io_event(ctx->base,ctx->ioev1);
        LOG("add stdin");
    }
}

void foo(void *data)
{
    custom_test_ctx *con = (custom_test_ctx *)data;
    // LOG("data->x = %d", con->x);
}

void make_addr(struct sockaddr_in *addr, char *ip, int port)
{
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    addr->sin_addr.s_addr = ip == 0 ? INADDR_ANY : inet_addr(ip);
}

void test()
{
    sev_base *base = sev_new_base();
    custom_test_ctx ctx = {0};
    ctx.x = 10;
    ctx.foo = foo;
    ctx.base = base;

    sev_custom_event *ev1 = 0, *ev2 = 0, *ev3 = 0, *ev4 = 0;
    int sockfd=0, errfd=0;
    {
        ev1 = new_cus_event(-1, CUSTOM_STATUS1, 1, handler1, &ctx);
        struct timeval ot1 = {.tv_sec = 4, .tv_usec = 0};
        add_cus_event(base, ev1, &ot1);
        ctx.ev1 = ev1;
    }

    // {
    //     ev2 = new_cus_event(-2, 0, 0, handler2, base);
    //     struct timeval ot2 = {.tv_sec = 3, .tv_usec = 0};
    //     add_cus_event(base, ev2, &ot2);
    // }
    {
        ev3 = new_cus_event(-3, CUSTOM_STATUS3, 0, handler3, &ctx);
        ctx.ev3 = ev3;
        // struct timeval ot3 = {.tv_sec = 2, .tv_usec = 0};
        // add_cus_event(base, ev3, &ot3);
    }
    {
        ev4 = new_cus_event(-4, 0, 1, handler4, base);
        add_cus_event(base, ev4, 0);
    }

    sev_io_event *ioev1 = 0, *ioev2 = 0, *ioev3 = 0, *ioev4 = 0;
    {
        ioev1 = new_io_event(STDIN_FILENO, SEV_IO_READABLE, 1, io_handler1, 0);
        ctx.ioev1 = ioev1;
        add_io_event(base, ioev1);
    }

    {
        sockfd = socket(AF_INET, SOCK_DGRAM, 0);

        int flags = fcntl(sockfd, F_GETFL, 0);
        flags |= O_NONBLOCK;
        fcntl(sockfd, F_SETFL, flags);

        struct sockaddr_in local_addr = {0};
        make_addr(&local_addr, 0, 54321);
        bind(sockfd, (const struct sockaddr *)&local_addr, sizeof(local_addr));

        ioev2 = new_io_event(sockfd, SEV_IO_READABLE, 1, io_handler2, base);
        add_io_event(base, ioev2);
    }
    {
        errfd = socket(AF_INET, SOCK_DGRAM, 0);

        int flags = fcntl(errfd, F_GETFL, 0);
        flags |= O_NONBLOCK;
        fcntl(errfd, F_SETFL, flags);

        struct sockaddr_in local_addr = {0};
        make_addr(&local_addr, 0, 54322);
        bind(errfd, (const struct sockaddr *)&local_addr, sizeof(local_addr));

        ioev3 = new_io_event(errfd, SEV_IO_READABLE, 1, io_handler3, &ctx);
        add_io_event(base, ioev3);
    }

    sev_loop(base);

    {
        if (ev1)
            free_cus_event(ev1);
        if (ev2)
            free_cus_event(ev2);
        if (ev3)
            free_cus_event(ev3);
        if (ev4)
            free_cus_event(ev4);
    }
    {
        if (ioev1)
        {
            free_io_event(ioev1);
        }
        if (ioev2)
        {
            free_io_event(ioev2);
        }
        if (ioev3)
        {
            free_io_event(ioev3);
        }

        close(sockfd);
        close(errfd);
    }
    sev_free_base(base);
}

int main()
{
    test();
    return 0;
}