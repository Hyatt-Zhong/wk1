
#include "lsquic_types.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <semaphore.h>

#define QUIC_LOG(fmt, ...) printf("##QUIC## "fmt "\n", ##__VA_ARGS__)
#define QUIC_TRACE QUIC_LOG("%s",__func__)

typedef struct contrl_ctx contrl_ctx_t;

// typedef void(*on_connect)(void* args, int status);
// typedef void(*on_close)(void* args);
// typedef void(*on_send)(void* args, void* stream);
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
enum Status
{
    STATUS_NONE,
    STATUS_RUNING,
    STATUS_CLOSE,
    STATUS_CONNECTED,
};

struct contrl_ctx
{
    enum Cmd cmd;
    enum Status status;
    struct sockaddr peer;
    lsquic_conn_t *conn;
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
};


// void quic_setting(contrl_ctx_t *ctrl_ctx, on_data fn_data, on_send fn_send, on_close fn_close, on_connect fn_connect, void* param);
void quic_setting(contrl_ctx_t *ctrl_ctx, on_data fn_data, data_parse fn_parse, data_free fn_free, data_remake fn_remake, void* param);

int quic_run(contrl_ctx_t *ctrl_ctx);

int quic_connect(contrl_ctx_t *ctrl_ctx,char *ip, int port);

int quic_close(contrl_ctx_t *ctrl_ctx);

int quic_status(contrl_ctx_t *ctrl_ctx);

int quic_push_data(int index, void *data);

// void quic_send(contrl_ctx_t *ctrl_ctx, void* data, int len);
// void quic_set_delay(contrl_ctx_t *ctrl_ctx, struct timeval *delay);
