#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/time.h>

typedef void (*cus_event_handler)(int fd, int event, int is_overtime, void *ctx);
typedef void (*io_event_handler)(int fd, int event, void *ctx);
typedef void (*timer_callback)(void *ctx);
typedef void (*timer_param_free_callback)(void *ctx);

typedef struct sev_base_
{
    int epoll_fd;
    void *cus_event_list;
    void *io_event_list;
    void *timer_list;
    int stop;
} sev_base;

typedef struct sev_io_event_
{
    struct epoll_event event;
    io_event_handler handler;
    void *ctx;
    int fd;
    int persist;
    int remove;
} sev_io_event;

enum CUSTOM_EVENT
{
    CUSTOM_STATUS1 = 0X1,
    CUSTOM_STATUS2 = 0X2,
    CUSTOM_STATUS3 = 0X4,
    CUSTOM_STATUS4 = 0X8,
};
enum SEV_IO_EVENT
{
    SEV_IO_READABLE = 0x1,
    SEV_IO_WRITEABLE = 0x2,
    SEV_IO_ERROR = 0x4,
};

typedef struct sev_custom_event_
{
    int event_id;
    int listen;
    int status;
    int persist;
    void *ctx;
    struct timeval *overtime;
    struct timeval _overtime;

    struct timeval start;

    int remove;

    cus_event_handler handler;
} sev_custom_event;

sev_base *sev_new_base();
void sev_free_base(sev_base *base);
void sev_loop(sev_base *base);

sev_custom_event *new_cus_event(int id, int event, int persist, cus_event_handler hd, void *ctx);
int add_cus_event(sev_base *base, sev_custom_event *ev, struct timeval *overtime);
int active_cus_event(sev_custom_event *ev, int event);
int remove_cus_event(sev_base *base, int event_id);
void free_cus_event(sev_custom_event *ev);

sev_io_event *new_io_event(int fd, int event, int persist, io_event_handler hd, void *ctx);
int add_io_event(sev_base *base, sev_io_event *ev);
int remove_io_event(sev_base *base, int fd);
void free_io_event(sev_io_event *ev);

int set_timer(sev_base *base, timer_callback tcb, timer_param_free_callback free_cb, void *param, struct timeval *overtime);

