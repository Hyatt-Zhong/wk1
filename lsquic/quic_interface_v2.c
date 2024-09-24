
#include "lsquic.h"
#include "quic_interface.h"
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

#define MAX_STREAM_COUNT 1

/////////////////////////////////////////////////////////////////

typedef struct client_ctx client_ctx_t;

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

/////////////////////////////////////////////////////////////////
#define QUEUE_SIZE 500
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

void *queue_head(queue *q)
{
    pthread_mutex_lock(&q->mutex);
    if (isEmpty(q))
    {
        pthread_mutex_unlock(&q->mutex);
        return NULL; 
    }
    void *item = q->data[q->front];
    pthread_mutex_unlock(&q->mutex);
    return item; 
}

//从队列取出数据并填满缓冲区
int send_data_from_queue(void* cctx, int num, lsquic_stream_t* stream)
{
    void *que = ((client_ctx_t *)cctx)->queue[num];
    contrl_ctx_t *ctrl_ctx = ((client_ctx_t *)cctx)->ctrl_ctx;

    int ret = 0;
    while (1)
    {
        void *item = queue_head((queue *)que);
        if (!item)
        {
            break;
        }

        int len = 0;
        void *data = 0;

        if (ctrl_ctx->fn_parse)
        {
            len = ctrl_ctx->fn_parse(item, &data);
        }

        ssize_t wlen = lsquic_stream_write(stream, data, len);
        ret += wlen;
        if (wlen < len)
        {
            //缓冲区已满，剩余数据继续存在队头
            if (ctrl_ctx->fn_remake)
            {
                ctrl_ctx->fn_remake(item, len - wlen);
                break;
            }
            else
            {
                QUIC_LOG("fn_remake is null");
            }
        }
        //数据完全拷到了缓冲区，出队
        (void)dequeue((queue *)que);
        if (ctrl_ctx->fn_free)
        {
            ctrl_ctx->fn_free(item);
        }
    }

    return ret;
}

// int get_data_from_queue(void *cctx, int num, void **data, void **pitem)
// {
//     void *que = ((client_ctx_t *)cctx)->queue[num];
//     contrl_ctx_t *ctrl_ctx = ((client_ctx_t *)cctx)->ctrl_ctx;
//     void *item = dequeue((queue *)que);
//     *pitem = item;
//     if (item && ctrl_ctx->fn_parse)
//     {
//         return ctrl_ctx->fn_parse(item, data);
//     }
//     return 0;
// }

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

    // if(ctrl_ctx->fn_connect){
    //     ctrl_ctx->fn_connect(ctrl_ctx->cb_param, ctrl_ctx->status);
    // }
    ctrl_ctx->status = STATUS_CONNECTED;
    QUIC_LOG("new conn");

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
    QUIC_LOG("close conn");
    // if(ctrl_ctx->fn_close){
    //     ctrl_ctx->fn_close(ctrl_ctx->cb_param);
    // }
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

    QUIC_LOG("new stream");
    lsquic_stream_wantread(stream, 1);
    lsquic_stream_wantwrite(stream, 1);

    return steam_ctx;
}
static void quic_client_on_read(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h)
{
    // QUIC_LOG("on read");
    client_ctx_t *cctx = st_h->cctx;
    contrl_ctx_t *ctrl_ctx = cctx->ctrl_ctx;

    char buf[1024] = {0};

    int rlen = 0;
    while (rlen = lsquic_stream_read(stream, buf, sizeof(buf)) > 0)
    {
        ctrl_ctx->fn_data(ctrl_ctx->cb_param, buf, rlen);
    }

    lsquic_stream_wantread(stream, 1);
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

// static int get_data(client_ctx_t *cctx, int num, void **data, void **item)
// {
//     struct timeval *point_tm = cctx->delay[num];
//     if (point_tm == 0)
//     {
//         return get_data_from_queue(cctx, num, data, item);
//     }
//     else
//     {
//         struct timeval cur;
//         gettimeofday(&cur, NULL);

//         if (tm_compare(point_tm, &cur))
//         {
//             free(point_tm);
//             cctx->delay[num] = 0;

//             // printf("delay quit\n");
//             return get_data_from_queue(cctx, num, data, item);
//         }
//     }

//     return 0;
// }

// static void set_delay(client_ctx_t *cctx, int num, struct timeval *tm)
// {
//     struct timeval *point_tm = (struct timeval *)calloc(1, sizeof(struct timeval));
//     gettimeofday(point_tm, NULL);
//     point_tm->tv_sec += tm->tv_sec;
//     point_tm->tv_usec += tm->tv_usec;

//     cctx->delay[num] = point_tm;
// }

static void quic_client_on_write(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h)
{
    client_ctx_t *cctx = st_h->cctx;
    int num = st_h->id;

    int slen = send_data_from_queue(cctx, num, stream);
    if (slen <= 0)
    {
        return;
    }

    lsquic_stream_flush(stream);
    lsquic_stream_wantwrite(stream, 1);
}

// static void send_extern_data(contrl_ctx_t *ctrl_ctx, lsquic_stream_t *stream)
// {
//     pthread_mutex_lock(&ctrl_ctx->mutex);
    
//     int len = ctrl_ctx->extern_len;
//     void *data = ctrl_ctx->extern_data;
//     if (data == 0)
//     {
//         goto end;
//     }
    
//     int slen = lsquic_stream_write(stream, data, len);
//     if (slen<len)
//     {
//         QUIC_LOG("send_extern_data send failed");
//     }
//     ctrl_ctx->extern_data = 0;
//     ctrl_ctx->extern_len = 0;

// end:
//     pthread_mutex_unlock(&ctrl_ctx->mutex);
//     sem_post(&ctrl_ctx->sem);
// }

// static void quic_client_on_write(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h)
// {
//     // QUIC_LOG("on write");
//     client_ctx_t *cctx = st_h->cctx;
//     contrl_ctx_t *ctrl_ctx = cctx->ctrl_ctx;

//     send_extern_data(ctrl_ctx, stream);

//     pthread_mutex_lock(&ctrl_ctx->mutex);

//     struct timeval *delay = ctrl_ctx->send_delay;
//     if (delay)
//     {
//         struct timeval cur;
//         gettimeofday(&cur, NULL);

//         if (tm_compare(delay, &cur) == 0)
//         {
//             pthread_mutex_unlock(&ctrl_ctx->mutex);
//             return;
//         }
//         free(delay);
//         ctrl_ctx->send_delay = 0;
//     }
//     pthread_mutex_unlock(&ctrl_ctx->mutex);

//     ctrl_ctx->fn_send(ctrl_ctx->cb_param, stream);
// }


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
    return !ctrl_ctx->runing;
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

    pthread_mutex_lock(&ctrl_ctx->mutex);
    switch (ctrl_ctx->cmd)
    {
    case START:
        ctrl_ctx->status = STATUS_RUNING;
        ctrl_ctx->runing = 1;
        break;

    case CONNECT:
        if (ctrl_ctx->status != STATUS_CONNECTED)
        {
            ctrl_ctx->conn = lsquic_engine_connect(ctx->engine, N_LSQVER, (struct sockaddr *)ctx->local_addr, (struct sockaddr *)&ctrl_ctx->peer,
                                                   0, 0, 0, 0, 0, 0, 0, 0);
            client_process_conns(ctx);
            QUIC_LOG("connect to quic_sever\n");
        }
        break;

    case CLOSE:
        if (ctrl_ctx->conn)
        {
            lsquic_conn_close(ctrl_ctx->conn);
            ctrl_ctx->conn = 0;
            QUIC_LOG("connect close\n");
        }
        else
        {
            ctrl_ctx->status = STATUS_CLOSE;
        }

        break;

    case STOP:
        /* sev_stop(ctx->eb); */
        break;

    default:
        break;
    }

    ctrl_ctx->cmd = NONE;
    pthread_mutex_unlock(&ctrl_ctx->mutex);
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
    QUIC_LOG("start loop");
    loop(&ctx);
    ctrl_ctx->runing = 0;
    clean_client_ctx(&ctx);
}

static int wait_status(contrl_ctx_t *ctrl_ctx, int status, int overtime/*ms*/)
{
    struct timeval tv, tv_cur;
    gettimeofday(&tv, NULL);
    tv.tv_sec += overtime / 1000;
    tv.tv_usec += (overtime % 1000) * 1000;
    while (ctrl_ctx->status != status)
    {
        gettimeofday(&tv_cur, NULL);
        if (tm_compare(&tv, &tv_cur))
        {
            QUIC_LOG("wait status %d overtime",status);
            return 0;
        }
    }
    return 1;
}

QUIC_API void quic_setting(contrl_ctx_t *ctrl_ctx, on_data fn_data, data_parse fn_parse, data_free fn_free, data_remake fn_remake, void* param)
{
    QUIC_TRACE;
    ctrl_ctx->fn_data = fn_data;
    ctrl_ctx->fn_parse = fn_parse;
    ctrl_ctx->fn_free = fn_free;
    ctrl_ctx->fn_remake = fn_remake;
    ctrl_ctx->cb_param = param;
}

// QUIC_API void quic_setting(contrl_ctx_t *ctrl_ctx, on_data fn_data, on_send fn_send, on_close fn_close, on_connect fn_connect, void* param)
// {
//     QUIC_TRACE;
//     ctrl_ctx->fn_data = fn_data;
//     ctrl_ctx->fn_send = fn_send;
//     ctrl_ctx->fn_close = fn_close;
//     ctrl_ctx->fn_connect = fn_connect;
//     ctrl_ctx->cb_param = param;
// }

QUIC_API int quic_run(contrl_ctx_t *ctrl_ctx)
{
    QUIC_TRACE;
    if (ctrl_ctx->runing)
    {
        return 1;
    }

    pthread_t thread1;
    ctrl_ctx->cmd = START;
    if (pthread_create(&thread1, NULL, quic_thread, (void *)ctrl_ctx))
    {
        return 0;
    }

    if (!wait_status(ctrl_ctx, STATUS_RUNING, 2000))
    {
        return 0;
    }

    QUIC_TRACE;
    return 1;
}

QUIC_API int quic_connect(contrl_ctx_t *ctrl_ctx, char *ip, int port)
{
    QUIC_TRACE;
    if (STATUS_CONNECTED == quic_status(ctrl_ctx))
    {
        return 1;
    }

    pthread_mutex_lock(&ctrl_ctx->mutex);
    make_addr((struct sockaddr_in *)&ctrl_ctx->peer, ip, port);
    ctrl_ctx->cmd = CONNECT;
    pthread_mutex_unlock(&ctrl_ctx->mutex);
    if (!wait_status(ctrl_ctx, STATUS_CONNECTED, 1000))
    {
        return 0;
    }

    QUIC_TRACE;
    return 1;
}

QUIC_API int quic_close(contrl_ctx_t *ctrl_ctx)
{
    QUIC_TRACE;

    pthread_mutex_lock(&ctrl_ctx->mutex);
    ctrl_ctx->cmd = CLOSE;
    pthread_mutex_unlock(&ctrl_ctx->mutex);

    if (!wait_status(ctrl_ctx, STATUS_CLOSE, 1000))
    {
        return 0;
    }

    QUIC_TRACE;
    return 1;
}

QUIC_API int quic_status(contrl_ctx_t *ctrl_ctx)
{
    return ctrl_ctx->status;
}

QUIC_API int quic_push_data(int index, void *data)
{
    queue *que = &data_queue[index];
    
    int ret = enqueue(que, (void *)data);
    if (!ret)
    {
        // QUIC_LOG("push data failed");
    }
    return ret;
}

// QUIC_API void quic_set_delay(contrl_ctx_t *ctrl_ctx, struct timeval *delay)
// {
//     if (ctrl_ctx->send_delay)
//     {
//         free(ctrl_ctx->send_delay);
//     }

//     ctrl_ctx->send_delay = (struct timeval *)malloc(sizeof(struct timeval));
//     memcpy(ctrl_ctx->send_delay, delay, sizeof(struct timeval));
// }

// QUIC_API void quic_send(contrl_ctx_t *ctrl_ctx, void *data, int len)
// {
//     QUIC_TRACE;
//     sem_init(&ctrl_ctx->sem,0,0);

//     pthread_mutex_lock(&ctrl_ctx->mutex);
//     ctrl_ctx->extern_data = data;
//     ctrl_ctx->extern_len = len;
//     pthread_mutex_unlock(&ctrl_ctx->mutex);

//     sem_wait(&ctrl_ctx->sem);
//     QUIC_TRACE;
// }


/* ****************  test  ***************** */
/*
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

static void QuicRemakeFrame(void* item, int len)
{
    if (!item)
    {
        return;
    }
    frame *frm = (frame *)item;

    void *remain = malloc(len);
    memcpy(remain, frm->data + (frm->len - len), len);
    if (frm->data)
        free(frm->data);

    frm->data = remain;
    frm->len = len;    
}

typedef struct test_ctx_
{
    sev_base *eb;
    contrl_ctx_t *ctrl_ctx;
    cus_evt *ev_data;

    char *peer_ip;
    int peer_port;
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
        quic_connect(tctx->ctrl_ctx,tctx->peer_ip,tctx->peer_port);
    }
    else if (0 == strcmp("close", buf))
    {
        quic_close(tctx->ctrl_ctx);
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
        for (int k = 0; k < 3; k++)
        {
            frame *frm = calloc(1, sizeof(frame));
            frm->data = calloc(1, 1024);
            frm->len = 1024;

            if (!quic_push_data(i, (void *)frm))
            {
                free_frm((void *)frm);
            }
        }
        
    }

    struct timeval tv = {.tv_sec = 0, .tv_usec = MAKE_DATA_INTERVAL};
    add_cus_event(tctx->eb, tctx->ev_data, &tv);
}
void on_recv(void* arg, void* data, int len)
{

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
    quic_setting(pctrl, on_recv, parse_frm, free_frm, QuicRemakeFrame, 0);

    sev_base *base = sev_new_base();

    test_ctx test = {.ctrl_ctx = pctrl, .eb = base,.peer_ip = serverip,.peer_port = serverport };
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
*/
/* ****************  test  ***************** */