#include "mp4-mutex.h"
#include <pthread.h>

static pthread_mutex_t file_mutex;
static pthread_mutex_t sync_mutex;
static pthread_mutex_t moov_mutex;

static pthread_mutex_t* get_mutex(int id)
{
    switch (id)
    {
    case FILE_MUTEX:
        return &file_mutex;
        break;

    case SYNC_MUTEX:
        return &sync_mutex;
        break;
    
    case MOOV_MUTEX:
        return &moov_mutex;
        break;

    default:
        break;
    }
    return 0;
}

void mp4_mutex_lock(int id)
{
    pthread_mutex_t* mu = get_mutex(id);
    if (!mu)
    {
        return;
    }
    
    pthread_mutex_lock(mu);
}

void mp4_mutex_unlock(int id)
{
    pthread_mutex_t* mu = get_mutex(id);
    if (!mu)
    {
        return;
    }
    
    pthread_mutex_unlock(mu);
}