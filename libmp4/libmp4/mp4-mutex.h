
#define FILE_MUTEX 0
#define SYNC_MUTEX 1
#define MOOV_MUTEX 2

void mp4_mutex_lock(int id);

void mp4_mutex_unlock(int id);