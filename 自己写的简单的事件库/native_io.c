
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

#define QUIC_LOG(fmt, ...) printf("##QUIC## "fmt "\n", ##__VA_ARGS__)
#define QUIC_TRACE QUIC_LOG("%s",__func__)

typedef struct contrl_ctx contrl_ctx_t;

// typedef void(*on_connect)(void* args, int status);
// typedef void(*on_close)(void* args);
typedef void(*on_send)(void* args, void* stream, int index);
typedef void(*on_data)(void* args, void* data, int len);

typedef int (*data_parse)(void *, void **);
typedef void(*data_free)(void *);
typedef void(*data_remake)(void* item, int len);

enum Cmd
{
    NONE,
    START,
    CONNECT,
    CLOSE,
    STOP,
};

#define STATUS_LIST \
    X(STATUS_NONE) \
    X(STATUS_RUNNING) \
    X(STATUS_CLOSE) \
    X(STATUS_CONNECTED)

// 定义枚举
enum Status
{
    #define X(status) status,
    STATUS_LIST
    #undef X
};

// 定义与枚举值对应的字符串数组
static char* StatusToString(int status)
{
    switch (status)
    {
        #define X(status) case status: return #status;
        STATUS_LIST
        #undef X
        default: return "UNKNOWN_STATUS";
    }
}

struct contrl_ctx
{
    int sync_flag;
    enum Cmd cmd;
    enum Status status;
    struct sockaddr peer;
    int runing;

    data_parse fn_parse;
    data_free fn_free;
    data_remake fn_remake;

    on_data fn_data;
    // on_send fn_send;
    // on_close fn_close;
    // on_connect fn_connect;
    void* cb_param;

	// sem_t sem;
    pthread_mutex_t mutex;
    // void* extern_data;
    // int extern_len;

    // struct timeval* send_delay;
//###################################################//
    on_send fn_send;
};

#define QUIC_API

#define MAX_STREAM_COUNT 3

/////////////////////////////////////////////////////////////////

typedef struct client_ctx client_ctx_t;

typedef sev_base evb;
typedef sev_io_event io_evt;
typedef sev_custom_event cus_evt;

struct client_ctx
{
    int fd;
    evb *eb;
    io_evt *ev_net;

    cus_evt *timer;
    cus_evt *contrl;

    struct sockaddr_in *local_addr;

    int stream_count;
    void *queue[MAX_STREAM_COUNT];
    struct timeval *delay[MAX_STREAM_COUNT];

    contrl_ctx_t *ctrl_ctx;
};

/////////////////////////////////////////////////////////////////
#define QUEUE_SIZE (360/MAX_STREAM_COUNT)
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

static void clear_queue(client_ctx_t* cctx)
{
    if (!cctx)
    {
        return ;
    }
    
            QUIC_LOG("clear_queue 1");
    for (int i = 0; i < MAX_STREAM_COUNT; i++)
    {
        void *que = cctx->queue[i];
            QUIC_LOG("clear_queue 2");
        if (!que)
        {
            continue;
        }
        
        contrl_ctx_t *ctrl_ctx = cctx->ctrl_ctx;

            QUIC_LOG("clear_queue 3");
        void *item = queue_head((queue *)que);
        while (item)
        {
            if (ctrl_ctx->fn_free)
            {
                ctrl_ctx->fn_free(item);
            }

            (void)dequeue((queue *)que);
            item = queue_head((queue *)que);
        }
            QUIC_LOG("clear_queue 4");
    }
}

//从队列取出数据并填满缓冲区
int send_data_from_queue(void* cctx, int num, int fd)
{
    static int index = 0;
    void *que = ((client_ctx_t *)cctx)->queue[num];
    index = (index + 1)%MAX_STREAM_COUNT;
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

        ssize_t wlen = send(fd, data, len,0);
        if (wlen < 0)
        {
            QUIC_LOG("lsquic_stream_write error");
            return 0;
        }
        else
        {
            ret += wlen;
            if (wlen < len)
            {
                // 缓冲区已满，剩余数据继续存在队头
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
            //else wlen == len
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

void print_send_info(int len)
{
    int interval = 2;

    static int slen = 0;
    slen += len;

    static struct timeval cur, tm;
    static struct timeval *p_cur = 0;

    if (p_cur == 0 || cur.tv_sec == 0)
    {
        p_cur = &cur;
        gettimeofday(&cur, NULL);
        tm = cur;
        tm.tv_sec += interval;
    }
    gettimeofday(&cur, NULL);
    if (tm_compare(&tm, &cur))
    {
        int speed = slen / interval;
        QUIC_LOG("speed is %d", speed);

        slen = 0;
        cur.tv_sec = 0;
    }
}


int is_stop(contrl_ctx_t *ctrl_ctx)
{
    return !ctrl_ctx->runing;
}




static void contrl_func(int fd, int event, int is_overtime, void *arg)
{
    // QUIC_TRACE;
    client_ctx_t *ctx = arg;
    contrl_ctx_t *ctrl_ctx = ctx->ctrl_ctx;

    pthread_mutex_lock(&ctrl_ctx->mutex);
    switch (ctrl_ctx->cmd)
    {
    case START:
        ctrl_ctx->status = STATUS_RUNNING;
        ctrl_ctx->runing = 1;
        break;

    case CONNECT:
        if (ctrl_ctx->status != STATUS_CONNECTED)
        {
            
            QUIC_LOG("connect to quic_sever");
        }
        break;

    case CLOSE:
        

        break;

    case STOP:
        /* sev_stop(ctx->eb); */
        break;

    default:
        break;
    }
    if(ctrl_ctx->cmd != NONE){QUIC_LOG("current status: %s", StatusToString(ctrl_ctx->status));}

    ctrl_ctx->cmd = NONE;
    pthread_mutex_unlock(&ctrl_ctx->mutex);
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



void loop(client_ctx_t *ctx)
{
    sev_loop(ctx->eb);
}

void clean_client_ctx(client_ctx_t *ctx)
{

    free_cus_event(ctx->timer);
    free_io_event(ctx->ev_net);
    sev_free_base(ctx->eb);
    close(ctx->fd);
}


static int wait_sync_flag(contrl_ctx_t *ctrl_ctx, int overtime/*ms*/)
{
    struct timeval tv, tv_cur;
    gettimeofday(&tv, NULL);
    tv.tv_sec += overtime / 1000;
    tv.tv_usec += (overtime % 1000) * 1000;
    while (ctrl_ctx->sync_flag != 0)
    {
        gettimeofday(&tv_cur, NULL);
        if (tm_compare(&tv, &tv_cur))
        {
            QUIC_LOG("wait sync_flag overtime");
            return 0;
        }
        usleep(1000);
    }
    return 1;
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
            QUIC_LOG("wait status %s overtime", StatusToString(status));
            return 0;
        }
        usleep(1000);
    }
    QUIC_LOG("status %s meets expect", StatusToString(status));
    return 1;
}




QUIC_API int quic_push_data(int index, void *data)
{
    queue *que = &data_queue[index%MAX_STREAM_COUNT];
    
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
    // QUIC_TRACE;
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

    int local_sock;
} test_ctx;

int make_frame(int index)
{
    int ret = 0;
    static struct timeval tv[MAX_STREAM_COUNT] = {0};
    struct timeval cur;
    gettimeofday(&cur, NULL);
    if (tm_compare(&tv[index], &cur))
    {
        tv[index]=cur;
        tv[index].tv_sec+=2;
        ret = 110000;
        // QUIC_LOG("make_frame %d",ret);
    }
    else
    {
        ret = 200;
    }

    return ret;
}

void print_drop_info(int drop_len)
{
    int interval = 5;

    static long long drop_data = 0;
    drop_data+=drop_len;

    static int drop = 0;
    drop++;

    static struct timeval cur, tm;
    static struct timeval *p_cur = 0;

    if (p_cur == 0 || cur.tv_sec == 0)
    {
        p_cur = &cur;
        gettimeofday(&cur, NULL);
        tm = cur;
        tm.tv_sec += interval;
    }
    gettimeofday(&cur, NULL);
    if (tm_compare(&tm, &cur))
    {
        QUIC_LOG("drop %d frame, drop data %lld", drop, drop_data);

        drop_data = 0;
        drop = 0;
        cur.tv_sec = 0;
    }
}



#define MAKE_DATA_INTERVAL_ 66000 / 1
#define MAKE_DATA_INTERVAL GetInterval()
#define FRM_LEN 1536

static int make_data_interval = 0;
int GetInterval()
{
    return (make_data_interval==0?MAKE_DATA_INTERVAL_:make_data_interval);
}
#if USE_PLAN_A
static void data_cb(int fd, int event, int is_overtime, void *arg)
{

    test_ctx *tctx = (test_ctx *)arg;

    for (int i = 0; i < MAX_STREAM_COUNT; i++)
    {
        int size = make_frame(i);
        while (size>0)
        {
            int ll = size>FRM_LEN?FRM_LEN:size;
            size -= ll;

            frame *frm = calloc(1, sizeof(frame));
            frm->data = calloc(1, ll);
            frm->len = ll;

            if (!quic_push_data(i, (void *)frm))
            {
                free_frm((void *)frm);
                print_drop_info(size+ll);
                break;
            }
        }
    }

    struct timeval tv = {.tv_sec = 0, .tv_usec = MAKE_DATA_INTERVAL};
    add_cus_event(tctx->eb, tctx->ev_data, &tv);
}
#else

queue x_que[MAX_STREAM_COUNT];
int init_queue = 0;
typedef struct x_data_{
    void* data;
    int len;
    int off;
}x_data;
static void data_cb(int fd, int event, int is_overtime, void *arg)
{
    if (!init_queue)
    {
        init_queue = 1;
        for (int i = 0; i < MAX_STREAM_COUNT; i++)
        {
            initQueue(&x_que[i]);
        }
    }
    
    test_ctx *tctx = (test_ctx *)arg;

    for (int i = 0; i < MAX_STREAM_COUNT; i++)
    {
        int size = make_frame(i);
        
        x_data *data = calloc(1,sizeof(x_data));
        data->data = calloc(1, size);
        data->len = size;
        if(!enqueue(&x_que[i],data))
        {
            // QUIC_LOG("drop a frame");
            free(data->data);
            free(data);
        }
    }

    struct timeval tv = {.tv_sec = 0, .tv_usec = MAKE_DATA_INTERVAL};
    add_cus_event(tctx->eb, tctx->ev_data, &tv);
}

void onsend(void* args, int fd, int num)
{
    static int index = 0;
    queue* que = &x_que[index];
    index = (index + 1)%MAX_STREAM_COUNT;
    while (1)
    {
        void* item = queue_head(que);
        if (!item)
        {
            break;
        }
        
        x_data *x = (x_data*)item;
        int wlen = send(fd, x->data+x->off, x->len-x->off,0);
        if (wlen<0){}
        else if(wlen<x->len-x->off)
        {
            x->off+=wlen;
            break;
        }
        
        (void)dequeue(que);
        free(x->data);
        free(x);
    }
}
#endif

void on_tcp_event(int fd, int what, void *arg);
static void stdin_cb(int fd, int what, void *arg)
{
    test_ctx *tctx = (test_ctx *)arg;

    char buf[1024] = {0};

    read(fd, buf, sizeof(buf));

    char *p = strchr(buf, '\n');
    p ? *p = 0 : 0;

    if (0 == strcmp("conn", buf))
    {
        if (tctx->local_sock == -1)
        {
            struct sockaddr_in local;
            struct sockaddr_in *local_addr = &local;
            make_addr(local_addr, 0, 0);
            int sock = make_tcp_sock(local_addr);
            tctx->local_sock = sock;
                
            io_evt* ev_tcp = new_io_event(sock, SEV_IO_READABLE|SEV_IO_WRITEABLE|SEV_IO_ERROR, 1, on_tcp_event, 0);
            add_io_event(tctx->eb, ev_tcp);
        }
        
        struct sockaddr peer_addr;
        make_addr((struct sockaddr_in *)&peer_addr, tctx->peer_ip, tctx->peer_port);
        if(-1 == connect(tctx->local_sock, &peer_addr, sizeof(peer_addr)) && (errno == EINPROGRESS))//非阻塞模式下调用 connect() 时，连接操作不会立即完成。如果连接需要一些时间，connect() 会返回 -1，并且 errno 被设置为 EINPROGRESS，表示连接正在进行中。
        {
            if (tctx->ev_data == 0)
            {
                cus_evt *ev_data = new_cus_event(1, 0, 0, data_cb, (void *)tctx);
                tctx->ev_data = ev_data;

                struct timeval tv = {.tv_sec = 0, .tv_usec = MAKE_DATA_INTERVAL};
                add_cus_event(tctx->eb, ev_data, &tv);
            }
        }
        else
        {
            int p = errno;
        }
    }
    else if (0 == strcmp("close", buf))
    {
        remove_io_event(tctx->eb, tctx->local_sock, 1);
        close(tctx->local_sock);
        tctx->local_sock = -1;
    }
    else if (0 == strcmp("stop", buf))
    {
        // quic_stop()
        sev_stop(tctx->eb);
    }
}


void on_recv(void* arg, void* data, int len)
{

}

void on_tcp_event(int fd, int what, void *arg)
{
    if (what&SEV_IO_WRITEABLE)
    {
        onsend(0,fd,0);
    }
    
}

static void stdin_test(int fd, int what, void *arg)
{
    char buf[1024] = {0};

    read(fd, buf, sizeof(buf));
    LOG("%s", buf);
}

int main1()
{
    sev_base *base = sev_new_base();
    // io_evt *ev_std = new_io_event(STDIN_FILENO, SEV_IO_READABLE, 1, stdin_test, 0);
    // add_io_event(base, ev_std);

    struct sockaddr_in local;
    struct sockaddr_in *local_addr = &local;
    make_addr(local_addr, 0, 0);
    int sock = make_tcp_sock(local_addr);

    io_evt* ev_tcp = new_io_event(sock, SEV_IO_READABLE, 1, on_tcp_event, 0);
    add_io_event(base, ev_tcp);

    sev_loop(base);

    // free_io_event(ev_tcp);
    // free_io_event(ev_std);
    sev_free_base(base);
}
int main(int argc, char *argv[])
{
    if (argc < 4)
    {
        fprintf(stderr, "Usage: %s <localport> <serverip> <serverport> <interval>\n", argv[0]);
        return 1;
    }

    int localport = atoi(argv[1]);
    char *serverip = argv[2];
    int serverport = atoi(argv[3]);
    if (argc >= 5)
    {
        make_data_interval = atoi(argv[4]);
    }

    contrl_ctx_t ctrl_ctx = {0};
    contrl_ctx_t *pctrl = &ctrl_ctx;

    sev_base *base = sev_new_base();

    test_ctx test = {.ctrl_ctx = pctrl, .eb = base,.peer_ip = serverip,.peer_port = serverport, .local_sock = -1};
    io_evt *ev_std = new_io_event(STDIN_FILENO, SEV_IO_READABLE, 1, stdin_cb, (void *)&test);
    add_io_event(base, ev_std);

    sev_loop(base);

    free_io_event(ev_std);
    if(test.ev_data) free_cus_event(test.ev_data);
    sev_free_base(base);
}

/* ****************  test  ***************** */