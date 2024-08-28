#include <map>
#include "simple_event_macro.h"
#include "simple_event.h"

using namespace std;

typedef map<int, sev_custom_event *> cus_ev_list;
typedef map<int, sev_io_event *> io_ev_list;
typedef map<int, sev_custom_event *>::iterator cus_ev_itor;
typedef map<int, sev_io_event *>::iterator io_ev_itor;

extern "C"
{
    void _make_list(sev_base *base);
    void _clear_list(sev_base *base);

    int _add_io_event(void *ls, sev_io_event *ev);
    sev_io_event *_get_io_event(void *ls, int fd);
    void _set_io_event_remove(void *ls, int fd);
    void _remove_io_event(void *ls, int (*epoll_remove_cb)(sev_base *base, int fd), sev_base *base);

    int _add_cus_event(void *ls, sev_custom_event *ev);
    void _remove_cus_event(void *ls, int event_id);
    int _cus_event_loop(sev_base *base);
}

void _make_list(sev_base *base)
{
    if (!base)
    {
        return;
    }

    base->cus_event_list = (void *)new cus_ev_list;
    base->io_event_list = (void *)new io_ev_list;
}

void _del_all_cus_event(void *ls);
void _del_all_io_event(void *ls);
void _clear_list(sev_base *base)
{
    if (!base)
    {
        return;
    }

    _del_all_cus_event(base->cus_event_list);
    _del_all_io_event(base->io_event_list);

    delete (cus_ev_list *)base->cus_event_list;
    delete (io_ev_list *)base->io_event_list;

    base->cus_event_list = 0;
    base->io_event_list = 0;
}

int _add_cus_event(void *ls, sev_custom_event *ev)
{
    cus_ev_list *ev_list = (cus_ev_list *)ls;
    ev->remove = 0;//移除标志复位
    if (ev_list == nullptr || ev_list->find(ev->event_id) != ev_list->end())
    {
        // LOG("list is null or ev is exist ev_list = 0x%p", ev_list);
        return -1;
    }

    ev_list->insert(make_pair(ev->event_id, ev));

    return 0;
}
void _remove_cus_event(void *ls, int event_id)
{
    cus_ev_list *ev_list = (cus_ev_list *)ls;
    if (!ev_list)
    {
        return;
    }

    cus_ev_itor it = ev_list->find(event_id);
    if (it != ev_list->end())
    {
        it->second->remove = 1;
    }
}
void _del_all_cus_event(void *ls)
{
    cus_ev_list *ev_list = (cus_ev_list *)ls;
    ev_list->clear();
}

int _is_overtime(struct timeval *start, struct timeval *end, struct timeval *overtime)
{
    if ((end->tv_sec - start->tv_sec) > overtime->tv_sec)
    {
        return 1;
    }
    if ((end->tv_sec - start->tv_sec) == overtime->tv_sec &&
        (end->tv_usec - start->tv_usec) > overtime->tv_usec)
    {
        return 1;
    }

    return 0;
}

int _cus_event_loop(sev_base *base)
{
    cus_ev_list *ev_list = (cus_ev_list *)base->cus_event_list;
    INTERVAL_LOG(2000,"cus event size = %ld", ev_list->size());
    for (cus_ev_itor it = ev_list->begin(); it != ev_list->end();)
    {
        sev_custom_event *ev = it->second;

        if (ev->remove)
        {
            it = ev_list->erase(it);
            ev->remove = 0;//移除标志重置为0，因为事件是可以复用的
            continue;
        }
        else {++it;}
    }

    cus_ev_list ev_list_copy = *ev_list;
    cus_ev_list* pev_list_copy = &ev_list_copy;

    for (cus_ev_itor it = pev_list_copy->begin(); it != pev_list_copy->end(); ++it)
    {
        sev_custom_event *ev = it->second;
        int trigger = ev->status & ev->listen;
        struct timeval cur;
        gettimeofday(&cur, NULL);

        int is_overtime = ev->overtime == NULL ? 1 : _is_overtime(&ev->start, &cur, ev->overtime);
        if (trigger || is_overtime) // 超时或者触发
        {
            ev->start = cur; // 已经 超时或者触发，重置超时计时
            ev->status = 0;  // 状态清零

            if (!ev->persist) // 不是一个常驻的事件
            {
                _remove_cus_event(ev_list,ev->event_id);
            }
            ev->handler(ev->event_id, trigger, is_overtime, ev->ctx);
        }
    }
    return 0;
}


int _add_io_event(void *ls, sev_io_event *ev)
{
    io_ev_list *ev_list = (io_ev_list *)ls;
    ev->remove = 0;//移除标志复位
    if (ev_list == nullptr || ev_list->find(ev->fd) != ev_list->end())
    {
        LOG("list is null or ev is exist. ev_list = 0x%p", ls);
        return -1;
    }

    ev_list->insert(make_pair(ev->fd, ev));

    return 0;
}
sev_io_event *_get_io_event(void *ls, int fd)
{
    io_ev_list *ev_list = (io_ev_list *)ls;
    if (!ev_list)
    {
        return nullptr;
    }
    io_ev_itor it = ev_list->find(fd);
    if (it != ev_list->end())
    {
        return it->second;
    }

    return nullptr;
}

void _remove_io_event(void *ls, int (*epoll_remove_cb)(sev_base *base, int fd), sev_base *base)
{
    io_ev_list *ev_list = (io_ev_list *)ls;
    INTERVAL_LOG(2000,"io event size = %ld", ev_list->size());
    for (io_ev_itor it = ev_list->begin(); it != ev_list->end(); )
    {
        sev_io_event *ev = it->second;
        if (ev->remove)
        {
            epoll_remove_cb(base, ev->fd);
            it = ev_list->erase(it);
            ev->remove = 0;//移除标志重置为0，因为事件是可以复用的
        }
        else
        {
            ++it;
        }
    }
}

void _set_io_event_remove(void *ls, int fd)
{
    io_ev_list *ev_list = (io_ev_list *)ls;
    if (!ev_list)
    {
        return;
    }
    io_ev_itor it = ev_list->find(fd);
    if (it != ev_list->end())
    {
        it->second->remove = 1;
    }
}
void _del_all_io_event(void *ls)
{
    io_ev_list *ev_list = (io_ev_list *)ls;
    ev_list->clear();
}