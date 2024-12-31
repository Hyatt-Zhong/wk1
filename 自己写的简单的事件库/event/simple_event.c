#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "simple_event_macro.h"
#include "simple_event.h"

extern void _make_list(sev_base *base);
extern void _clear_list(sev_base *base);

extern int _add_io_event(void *ls, sev_io_event *ev);
extern sev_io_event *_get_io_event(void *ls, int fd);
extern void _set_io_event_remove(void *ls, int fd, int free);
extern void _remove_io_event(void *ls, int (*epoll_remove_cb)(sev_base *base, int fd), sev_base *base);

extern int _add_cus_event(void *ls, sev_custom_event *ev);
extern void _remove_cus_event(void *ls, int event_id);
extern int _cus_event_loop(sev_base *base);

extern int _add_timer(void* ls, timer_param_free_callback tcb, timer_param_free_callback free_cb, void *param, struct timeval *overtime);
extern int _timer_loop(sev_base *base);

sev_base *sev_new_base()
{
    sev_base *base = (sev_base *)calloc(1, sizeof(sev_base));
    base->epoll_fd = epoll_create1(0);

    _make_list(base);
    return base;
}
void sev_free_base(sev_base *base)
{
    close(base->epoll_fd);
    _clear_list(base);
}

sev_custom_event *new_cus_event(int id, int event, int persist, cus_event_handler hd, void *ctx)
{
    sev_custom_event *ev = (sev_custom_event *)calloc(1, sizeof(sev_custom_event));
    ev->event_id = id;
    ev->listen = event;
    ev->handler = hd;
    ev->persist = persist;
    ev->ctx = ctx;

    return ev;
}

void free_cus_event(sev_custom_event *ev)
{
    memset(ev, 0, sizeof(sev_custom_event));
    free(ev);
}

int add_cus_event(sev_base *base, sev_custom_event *ev, struct timeval *overtime)
{
    if (overtime)
    {
        memcpy(&ev->_overtime, overtime, sizeof(struct timeval));
        ev->overtime = &ev->_overtime;
        gettimeofday(&ev->start, NULL);
    }
    else
    {
        memset(&ev->_overtime, 0, sizeof(struct timeval));
        memset(&ev->start, 0, sizeof(struct timeval));
        ev->overtime = 0;
    }
    return _add_cus_event(base->cus_event_list, ev);
}

int active_cus_event(sev_custom_event *ev, int event)
{
    ev->status |= event;
    return 0;
}

int remove_cus_event(sev_base *base, int event_id, int free)
{
    _remove_cus_event(base->cus_event_list, event_id);
    return 0;
}

int _io_event_loop(sev_base *base);
void sev_loop(sev_base *base)
{
    while (!base->stop)
    {
        _timer_loop(base);
        _cus_event_loop(base);
        _io_event_loop(base);
    }
}
void sev_stop(sev_base *base)
{
    base->stop = 1;
}

sev_io_event *new_io_event(int fd, int event, int persist, io_event_handler hd, void *ctx)
{
    sev_io_event *ev = (sev_io_event *)calloc(1, sizeof(sev_io_event));
    ev->persist = persist;
    ev->handler = hd;
    ev->ctx = ctx;
    ev->fd = fd;

    int listen = 0;
    event &SEV_IO_READABLE ? listen |= EPOLLIN : 0;
    event &SEV_IO_WRITEABLE ? listen |= EPOLLOUT : 0;
    event &SEV_IO_HANGUP ? listen |= EPOLLHUP : 0;
    event &SEV_IO_ERROR ? listen |= EPOLLERR : 0;
    persist == 0 ? listen |= EPOLLONESHOT : 0;
    ev->event.events = listen;
    ev->event.data.fd = fd;

    return ev;
}

void free_io_event(sev_io_event *ev)
{
    memset(ev, 0, sizeof(sev_io_event));
    free(ev);
}

int add_io_event(sev_base *base, sev_io_event *ev)
{
    int res = epoll_ctl(base->epoll_fd, EPOLL_CTL_ADD, ev->event.data.fd, &ev->event);
    if (res != 0)
    {
        LOG("epoll_ctl ERROR.");
        return -1;
    }
    
    _add_io_event(base->io_event_list, ev);
    return 0;
}

int epoll_remove_cb(sev_base *base, int fd)
{
    int res = epoll_ctl(base->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    if (res != 0)
    {
        LOG("epoll_ctl ERROR.");
        // return -1;
    }
    return res;
}

int remove_io_event(sev_base *base, int fd, int free)
{
    //只对要删除的事件打个标记
    _set_io_event_remove(base->io_event_list, fd, free);
    return 0;
}

#define MAX_EVENTS 10
int _io_event_loop(sev_base *base)
{
    _remove_io_event(base->io_event_list,epoll_remove_cb,base);//这里才是真正删除事件

    struct epoll_event events[MAX_EVENTS];
    int nfds = epoll_wait(base->epoll_fd, events, MAX_EVENTS, 1/*1*/ /*无事件时阻塞1ms*/);
    if (nfds == -1)
    {
        LOG("epoll_wait ERROR.");
        return -1;
    }

    for (int i = 0; i < nfds; i++)
    {
        uint32_t eev = events[i].events;

        int status = 0;
        eev &EPOLLIN ? status |= SEV_IO_READABLE : 0;
        eev &EPOLLOUT ? status |= SEV_IO_WRITEABLE : 0;
        eev & EPOLLHUP ? status |= SEV_IO_HANGUP : 0;
        eev & EPOLLERR ? status |= SEV_IO_ERROR : 0;

        int fd = events[i].data.fd;
        sev_io_event *ev = _get_io_event(base->io_event_list, fd);

        if (!ev->persist)
        {
            remove_io_event(base, fd, 0);
        }
        if (ev)
        {
            ev->handler(fd, status, ev->ctx);
        }
    }

    return 0;
}

int set_timer(sev_base *base, timer_callback tcb, timer_param_free_callback free_cb, void *param, struct timeval *overtime)
{
    return _add_timer(base->timer_list, tcb,free_cb,param,overtime);
}
