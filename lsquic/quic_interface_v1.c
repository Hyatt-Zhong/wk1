

#include "lsquic.h"
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

#define QUIC_API

#define MAX_STREAM_COUNT 3

/////////////////////////////////////////////////////////////////

typedef struct client_ctx client_ctx_t;
typedef struct contrl_ctx contrl_ctx_t;

typedef sev_base evb;
typedef sev_io_event io_evt;
typedef sev_custom_event cus_evt;

struct lsquic_stream_ctx
{
    lsquic_stream_t *stream;
    client_ctx_t *cctx;
    int id;
};

struct lsquic_conn_ctx
{
    lsquic_conn_t *conn;
    client_ctx_t *cctx;
};

struct client_ctx
{
    int fd;
    evb *eb;
    io_evt *ev_net;

    cus_evt *timer;
    cus_evt *contrl;

    lsquic_engine_t *engine;
    struct lsquic_engine_settings *eng_cfg;
    struct sockaddr_in *local_addr;

    int stream_count;
    void *queue[MAX_STREAM_COUNT];
    struct timeval *delay[MAX_STREAM_COUNT];

    contrl_ctx_t *ctrl_ctx;
};

enum Cmd
{
    NONE,
    CONNECT,
    CLOSE,
    STOP,
};
enum Status
{
    STATUS_CLOSE,
    STATUS_CONNECTED,
};

struct contrl_ctx
{
    enum Cmd cmd;
    enum Status status;
    struct sockaddr peer;
    lsquic_conn_t *conn;
    int stop;

    int (*data_parse)(void *, void **);
    void (*data_free)(void *);
};

/////////////////////////////////////////////////////////////////
#define QUEUE_SIZE 40
typedef struct queue_
{
    int max;
    int front;
    int tail;
    void *data[QUEUE_SIZE];

    pthread_mutex_t mutex;
} queue;

queue data_queue[MAX_STREAM_COUNT];

// 初始化队列
void initQueue(queue *q)
{
    q->max = QUEUE_SIZE;
    q->front = 0;
    q->tail = 0;
}

// 检查队列是否为空
int isEmpty(queue *q)
{
    return q->front == q->tail;
}

// 检查队列是否已满
int isFull(queue *q)
{
    return (q->tail + 1) % q->max == q->front;
}

// 入队函数
int enqueue(queue *q, void *item)
{
    pthread_mutex_lock(&q->mutex);
    if (isFull(q))
    {
        pthread_mutex_unlock(&q->mutex);
        return 0; // 队列已满，入队失败
    }
    q->data[q->tail] = item;
    q->tail = (q->tail + 1) % q->max;
    pthread_mutex_unlock(&q->mutex);
    return 1; // 入队成功
}

// 出队函数
void *dequeue(queue *q)
{
    pthread_mutex_lock(&q->mutex);
    if (isEmpty(q))
    {
        pthread_mutex_unlock(&q->mutex);
        return NULL; // 队列为空，出队失败
    }
    void *item = q->data[q->front];
    q->front = (q->front + 1) % q->max;
    pthread_mutex_unlock(&q->mutex);
    return item; // 出队成功
}

int get_data_from_queue(void *cctx, int num, void **data, void **pitem)
{
    void *que = ((client_ctx_t *)cctx)->queue[num];
    contrl_ctx_t *ctrl_ctx = ((client_ctx_t *)cctx)->ctrl_ctx;
    void *item = dequeue((queue *)que);
    *pitem = item;
    if (item && ctrl_ctx->data_parse)
    {
        return ctrl_ctx->data_parse(item, data);
    }
    return 0;
}

/////////////////////////////////////////////////////////////////

static lsquic_conn_ctx_t *quic_client_on_new_conn(void *stream_if_ctx, lsquic_conn_t *conn)
{
    client_ctx_t *cctx = stream_if_ctx;
    contrl_ctx_t *ctrl_ctx = cctx->ctrl_ctx;

    lsquic_conn_ctx_t *conn_ctx = malloc(sizeof(lsquic_conn_ctx_t));

    conn_ctx->conn = conn;
    conn_ctx->cctx = cctx;

    for (int i = 0; i < cctx->stream_count; i++)
    {
        lsquic_conn_make_stream(conn);
    }

    ctrl_ctx->status = STATUS_CONNECTED;
    // LOG("new conn");

    return conn_ctx;
}
static int stream_id = 0;
static void quic_client_on_conn_closed(lsquic_conn_t *conn)
{
    lsquic_conn_ctx_t *conn_ctx = lsquic_conn_get_ctx(conn);
    contrl_ctx_t *ctrl_ctx = conn_ctx->cctx->ctrl_ctx;

    lsquic_conn_set_ctx(conn, NULL);
    free(conn_ctx);

    stream_id = 0;
    // LOG("close conn");
    ctrl_ctx->status = STATUS_CLOSE;
}

static lsquic_stream_ctx_t *quic_client_on_new_stream(void *stream_if_ctx, lsquic_stream_t *stream)
{
    lsquic_stream_ctx_t *steam_ctx = calloc(1, sizeof(*steam_ctx));
    steam_ctx->stream = stream;

    client_ctx_t *cctx = stream_if_ctx;
    steam_ctx->cctx = cctx;
    steam_ctx->id = stream_id;

    stream_id++;

    // LOG("new stream");
    lsquic_stream_wantread(stream, 0);
    lsquic_stream_wantwrite(stream, 1);

    return steam_ctx;
}
static void quic_client_on_read(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h)
{
    // #define BUF_LEN 1024
    // char buf[BUF_LEN] = {0};
    // int buf_sz = BUF_LEN;

    // int rlen = 0;
    // while (rlen = lsquic_stream_read(stream, buf, buf_sz) > 0)
    // {

    // }

    /* lsquic_stream_wantread(stream, 0); */
}

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

static int get_data(client_ctx_t *cctx, int num, void **data, void **item)
{
    struct timeval *point_tm = cctx->delay[num];
    if (point_tm == 0)
    {
        return get_data_from_queue(cctx, num, data, item);
    }
    else
    {
        struct timeval cur;
        gettimeofday(&cur, NULL);

        if (tm_compare(point_tm, &cur))
        {
            free(point_tm);
            cctx->delay[num] = 0;

            // printf("delay quit\n");
            return get_data_from_queue(cctx, num, data, item);
        }
    }

    return 0;
}

static void set_delay(client_ctx_t *cctx, int num, struct timeval *tm)
{
    struct timeval *point_tm = (struct timeval *)calloc(1, sizeof(struct timeval));
    gettimeofday(point_tm, NULL);
    point_tm->tv_sec += tm->tv_sec;
    point_tm->tv_usec += tm->tv_usec;

    cctx->delay[num] = point_tm;
}

static void quic_client_on_write(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h)
{
    client_ctx_t *cctx = st_h->cctx;
    int num = st_h->id;

    void *data = 0;
    void *item = 0;
    int datalen = get_data(cctx, num, &data, &item);
    if (datalen <= 0)
    {
        cctx->ctrl_ctx->data_free(item);
        return;
    }

    ssize_t slen = lsquic_stream_write(stream, data, datalen);
    cctx->ctrl_ctx->data_free(item);

    if (slen < datalen)
    {
        struct timeval tm = {.tv_sec = 0, .tv_usec = 10000};
        set_delay(cctx, num, &tm);
    }

    lsquic_stream_flush(stream);
    lsquic_stream_wantwrite(stream, 1);
}

static void quic_client_on_close(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h)
{
    // LOG("close stream");
    //  client_ctx_t *cctx = st_h->cctx;
    free(st_h);
}

const struct lsquic_stream_if client_echo_stream_if = {
    .on_new_conn = quic_client_on_new_conn,
    .on_conn_closed = quic_client_on_conn_closed,
    .on_new_stream = quic_client_on_new_stream,
    .on_read = quic_client_on_read,
    .on_write = quic_client_on_write,
    .on_close = quic_client_on_close,
};

static int send_packets_out(void *ctx, const struct lsquic_out_spec *specs,
                            unsigned n_specs)
{
    struct msghdr msg;
    int sockfd;
    unsigned n;

    memset(&msg, 0, sizeof(msg));
    sockfd = (int)(uintptr_t)ctx;

    for (n = 0; n < n_specs; ++n)
    {
        msg.msg_name = (void *)specs[n].dest_sa;
        msg.msg_namelen = sizeof(struct sockaddr_in);
        msg.msg_iov = specs[n].iov;
        msg.msg_iovlen = specs[n].iovlen;
        if (sendmsg(sockfd, &msg, 0) < 0)
            break;
    }

    return (int)n;
}

int get_ecn(struct msghdr *msg)
{
    int ecn = -1;
    struct cmsghdr *cmsg;
    for (cmsg = CMSG_FIRSTHDR(msg); cmsg != NULL; cmsg = CMSG_NXTHDR(msg, cmsg))
    {
        if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_TOS)
        {
            ecn = *(int *)CMSG_DATA(cmsg);
            printf("ECN information: %d\n", ecn);
        }
    }
    return ecn;
}
void client_process_conns(client_ctx_t *cctx);

#define CTL_SZ 64
static void client_read_net_data(int fd, int what, void *arg)
{
    client_ctx_t *cctx = arg;

    ssize_t nread;
    struct sockaddr_storage peer_sas;
    unsigned char buf[4096];
    unsigned char ctl_buf[CTL_SZ];
    struct iovec vec[1] = {{buf, sizeof(buf)}};

    struct msghdr msg = {
        .msg_name = &peer_sas,
        .msg_namelen = sizeof(peer_sas),
        .msg_iov = vec,
        .msg_iovlen = 1,
        .msg_control = ctl_buf,
        .msg_controllen = sizeof(ctl_buf),
    };
    nread = recvmsg(fd, &msg, 0);
    if (-1 == nread)
    {
        return;
    }

    int ecn = get_ecn(&msg);
    ecn = ecn < 0 ? 0 : ecn;

    (void)lsquic_engine_packet_in(cctx->engine, buf, nread,
                                  (struct sockaddr *)cctx->local_addr,
                                  (struct sockaddr *)&peer_sas,
                                  (void *)cctx, ecn);

    client_process_conns(cctx);
}

int is_stop(contrl_ctx_t *ctrl_ctx)
{
    return ctrl_ctx->stop;
}

void client_process_conns(client_ctx_t *cctx)
{
    lsquic_engine_t *engine = cctx->engine;
    cus_evt *timer = cctx->timer;

    int diff;
    struct timeval timeout;

    lsquic_engine_process_conns(engine);

    if (lsquic_engine_earliest_adv_tick(engine, &diff))
    {
        if (diff < 0 || (unsigned)diff < cctx->eng_cfg->es_clock_granularity)
        {
            timeout.tv_sec = 0;
            timeout.tv_usec = cctx->eng_cfg->es_clock_granularity;
        }
        else
        {
            timeout.tv_sec = (unsigned)diff / 1000000;
            timeout.tv_usec = (unsigned)diff % 1000000;
        }
        if (!is_stop(cctx->ctrl_ctx))
            add_cus_event(cctx->eb, timer, &timeout);
    }
}

static void timer_handler(int fd, int event, int is_overtime, void *arg)
{
    client_ctx_t *cctx = arg;
    if (!is_stop(cctx->ctrl_ctx))
        client_process_conns(cctx);
}

static void contrl_func(int fd, int event, int is_overtime, void *arg)
{
    client_ctx_t *ctx = arg;
    contrl_ctx_t *ctrl_ctx = ctx->ctrl_ctx;

    switch (ctrl_ctx->cmd)
    {
    case CONNECT:
        ctrl_ctx->stop = 0;
        ctrl_ctx->conn = lsquic_engine_connect(ctx->engine, N_LSQVER, (struct sockaddr *)ctx->local_addr, (struct sockaddr *)&ctrl_ctx->peer,
                                               0, 0, 0, 0, 0, 0, 0, 0);
        client_process_conns(ctx);

        ctrl_ctx->cmd = NONE;
        break;
    case CLOSE:
        lsquic_conn_close(ctrl_ctx->conn);
        ctrl_ctx->conn = 0;
        ctrl_ctx->cmd = NONE;
        break;
    case STOP:
        /* sev_stop(ctx->eb); */
        break;
    default:
        break;
    }
}

static void make_addr(struct sockaddr_in *addr, char *ip, int port)
{
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    addr->sin_addr.s_addr = ip == 0 ? INADDR_ANY : inet_addr(ip);
}

int make_udp_sock(struct sockaddr_in *local_addr)
{
    int res = 0;
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);

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

int make_client_ctx(contrl_ctx_t *ctrl_ctx, client_ctx_t *ctx, int sock, struct sockaddr_in *local_addr, struct lsquic_engine_settings *setting, struct lsquic_engine_api *engine_api)
{
    ctx->fd = sock;

    ctx->eb = sev_new_base();

    ctx->ev_net = new_io_event(sock, SEV_IO_READABLE, 1, client_read_net_data, (void *)ctx);
    add_io_event(ctx->eb, ctx->ev_net);

    ctx->contrl = new_cus_event(1, 0, 1, contrl_func, ctx);
    add_cus_event(ctx->eb, ctx->contrl, 0);

    ctx->timer = new_cus_event(2, 0, 0, timer_handler, ctx);

    lsquic_engine_init_settings(setting, 0);

    ctx->engine = lsquic_engine_new(0, engine_api);
    ctx->eng_cfg = setting;

    ctx->local_addr = local_addr;

    ctx->stream_count = MAX_STREAM_COUNT;
    for (int i = 0; i < ctx->stream_count; i++)
    {
        ctx->queue[i] = &data_queue[i];
    }

    ctx->ctrl_ctx = ctrl_ctx;
}

void loop(client_ctx_t *ctx)
{
    sev_loop(ctx->eb);
}

void clean_client_ctx(client_ctx_t *ctx)
{
    lsquic_engine_destroy(ctx->engine);
    free_cus_event(ctx->timer);
    free_io_event(ctx->ev_net);
    sev_free_base(ctx->eb);
    close(ctx->fd);
}

static void *quic_thread(void *args)
{
    contrl_ctx_t *ctrl_ctx = (contrl_ctx_t *)args;

    int fd = 0;
    client_ctx_t ctx = {0};
    struct sockaddr_in local;
    struct lsquic_engine_settings setting;

    struct sockaddr_in *local_addr = &local;
    make_addr(local_addr, 0, 0);
    fd = make_udp_sock(local_addr);

    struct lsquic_engine_api engine_api = {
        .ea_settings = &setting,
        .ea_packets_out = send_packets_out,
        .ea_packets_out_ctx = (void *)(uintptr_t)fd,
        .ea_stream_if = &client_echo_stream_if,
        .ea_stream_if_ctx = &ctx,
    };

    for (int i = 0; i < MAX_STREAM_COUNT; i++)
    {
        initQueue(&data_queue[i]);
    }

    make_client_ctx(ctrl_ctx, &ctx, fd, &local, &setting, &engine_api);

    loop(&ctx);
    clean_client_ctx(&ctx);
}

QUIC_API void quic_setting(contrl_ctx_t *ctrl_ctx, char *ip, int port, int (*data_parse)(void *, void **), void(data_free)(void *))
{
    make_addr((struct sockaddr_in *)&ctrl_ctx->peer, ip, port);
    ctrl_ctx->data_parse = data_parse;
    ctrl_ctx->data_free = data_free;
}

QUIC_API int quic_run(contrl_ctx_t *ctrl_ctx)
{
    pthread_t thread1;

    if (pthread_create(&thread1, NULL, quic_thread, (void *)ctrl_ctx))
    {
        // fprintf(stderr, "Error creating thread 1\n");
        return 0;
    }
    return 1;
}

QUIC_API void quic_connect(contrl_ctx_t *ctrl_ctx)
{
    ctrl_ctx->cmd = CONNECT;
}

QUIC_API void quic_close(contrl_ctx_t *ctrl_ctx)
{
    ctrl_ctx->cmd = CLOSE;
}

QUIC_API int quic_status(contrl_ctx_t *ctrl_ctx)
{
    return ctrl_ctx->status;
}

QUIC_API int quic_push_data(int index, void *data)
{
    queue *que = &data_queue[index];
    return enqueue(que, (void *)data);
}

/* ****************  test  ***************** */

typedef struct frame_
{
    void *data;
    int len;
} frame;

int parse_frm(void *item, void **data)
{
    frame *frm = (frame *)item;
    *data = frm->data;
    return frm->len;
}

void free_frm(void *item)
{
    if (!item)
    {
        return;
    }

    frame *frm = (frame *)item;
    if (frm->data)
        free(frm->data);
    free(frm);
}

typedef struct test_ctx_
{
    sev_base *eb;
    contrl_ctx_t *ctrl_ctx;
    cus_evt *ev_data;
} test_ctx;

static void stdin_cb(int fd, int what, void *arg)
{
    test_ctx *tctx = (test_ctx *)arg;

    char buf[1024] = {0};

    read(fd, buf, sizeof(buf));

    char *p = strchr(buf, '\n');
    p ? *p = 0 : 0;

    if (0 == strcmp("conn", buf))
    {
        quic_status(tctx->ctrl_ctx) == STATUS_CLOSE ? quic_connect(tctx->ctrl_ctx) : 0;
    }
    else if (0 == strcmp("close", buf))
    {
        quic_status(tctx->ctrl_ctx) == STATUS_CONNECTED ? quic_close(tctx->ctrl_ctx) : 0;
    }
    else if (0 == strcmp("stop", buf))
    {
        // quic_stop()
        sev_stop(tctx->eb);
    }
}

#define MAKE_DATA_INTERVAL 66000 / 100
static void data_cb(int fd, int event, int is_overtime, void *arg)
{
    test_ctx *tctx = (test_ctx *)arg;

    for (int i = 0; i < MAX_STREAM_COUNT; i++)
    {
        frame *frm = calloc(1, sizeof(frame));
        frm->data = calloc(1, 1024);
        frm->len = 1024;

        if (!quic_push_data(i, (void *)frm))
        {
            free_frm((void *)frm);
        }
    }

    struct timeval tv = {.tv_sec = 0, .tv_usec = MAKE_DATA_INTERVAL};
    add_cus_event(tctx->eb, tctx->ev_data, &tv);
}

int main(int argc, char *argv[])
{
    if (argc != 4)
    {
        fprintf(stderr, "Usage: %s <localport> <serverip> <serverport>\n", argv[0]);
        return 1;
    }

    int localport = atoi(argv[1]);
    char *serverip = argv[2];
    int serverport = atoi(argv[3]);

    contrl_ctx_t ctrl_ctx = {0};
    contrl_ctx_t *pctrl = &ctrl_ctx;

    quic_run(pctrl);
    quic_setting(pctrl, serverip, serverport, parse_frm, free_frm);

    sev_base *base = sev_new_base();

    test_ctx test = {.ctrl_ctx = pctrl, .eb = base};
    io_evt *ev_std = new_io_event(STDIN_FILENO, SEV_IO_READABLE, 1, stdin_cb, (void *)&test);
    add_io_event(base, ev_std);

    cus_evt *ev_data = new_cus_event(1, 0, 0, data_cb, (void *)&test);
    test.ev_data = ev_data;

    struct timeval tv = {.tv_sec = 0, .tv_usec = MAKE_DATA_INTERVAL};
    add_cus_event(base, ev_data, &tv);

    sev_loop(base);

    free_io_event(ev_std);
    free_cus_event(ev_data);
    sev_free_base(base);
}

/* ****************  test  ***************** */