#include <mp4.h>

#define MAX_STREAM 5
#define MP4_PATH_LEN 64
typedef struct _Mp4Info {
    char files[MAX_STREAM][MP4_PATH_LEN];
    FILE *fp[MAX_STREAM];
    mp4_ctx *ctx[MAX_STREAM];
    int64_t tm_start;//600000ms以内 一般为0，续写时为非0
    int64_t tm_base[MAX_STREAM];
    int sync_rd[MAX_STREAM];
} Mp4Info;

typedef struct _Mp4Reader {
    char files[MAX_STREAM][MP4_PATH_LEN];
    FILE *fp[MAX_STREAM];
    read_ctx *ctx[MAX_STREAM];
    int read_cnt;
    int64_t tm_start;//600000ms以内
} Mp4Reader;


void imp_create_mp4(void *handle, char *prefix, int chn_count, int64_t tm_start);

void imp_create_placeholder(void *handle, char *prefix, int chn_count);

void imp_add_video(void *handle, int chn, char *spspps, int len);

void imp_add_audio(void *handle, int chn);

void imp_init_segment(void *handle, int chn);

void imp_write_video(void *handle, int chn, char *data, int len, int64_t pts, int is_key);

void imp_write_audio(void *handle, int chn_count, char *data, int len, int64_t pts);

void imp_close_mp4(void *handle, int chn_count);

void imp_read_mp4(void* handle, char *prefix, int chn_count, void *buf, int buf_size, void (*on_data)(void* param, int chn, int type, void* data, size_t len, int64_t pts, int64_t dts, int flags), void* param);

int imp_read_once(void* handle);

int imp_mp4_seek(void *handle, int64_t tm /* ms */);

void imp_close_mp4_reader(void *handle);
