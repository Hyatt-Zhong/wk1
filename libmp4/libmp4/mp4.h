
#include <stdio.h>
#include <stdint.h>

#define MP4_PATH_LEN 64
typedef struct _mp4_ctx
{
    void *fp;
    void *cache;

    int width;
    int height;

    int audio_track;
    int video_track;

    unsigned int last_update_moov;

	struct mp4_writer_t* mp4_wr;
}mp4_ctx;

typedef struct _read_ctx
{
    void *fp;

    void *buf;
    int buf_size;

    int audio_track;
    int video_track;

    void (*on_data)(void* param, int vchn, int type, void* data, size_t len, int64_t pts, int64_t dts, int flags);
    void* param;
    int vchn;
    uint32_t start_time;

	struct mov_reader_t* mp4_rd;
	struct mov_reader_t* mp4_sync;

    int64_t last_pts;
}read_ctx;

typedef struct _mov_file_cache_t
{
	FILE* fp;
	uint8_t *ptr;
	unsigned int len;
	unsigned int off;
	unsigned int size;

	uint64_t tell;
}mov_file_cache_t;

mov_file_cache_t *make_mov_file_cache(FILE* fp, int cache_size);
void destroy_mov_file_cache(mov_file_cache_t *cache);

void *custom_file_operator(int use_cache,
                           int (*read_data)(void *param, uint32_t type, void *data, uint64_t bytes),
                           int (*write_data)(void *param, int track, const void *data, uint64_t bytes, uint64_t *wrote_len));

mp4_ctx* create_mp4(FILE* fp, void *file_operator, int write_zeros);

mp4_ctx *mp4_continue(FILE *fp, void *file_operator);

void create_mp4_placeholder(FILE *fp, void *file_operator, int slow);

int add_video(mp4_ctx* ctx, char* spspps, int len);

int add_audio(mp4_ctx* ctx);

int init_segment(mp4_ctx *ctx);

int write_video_frame(mp4_ctx *ctx, char *data, int len, int64_t pts, int is_key);

int write_audio_frame(mp4_ctx *ctx, char *data, int len, int64_t pts);

int update_free_data(mp4_ctx *ctx, int offset, int len, void* val);

int update_moov(mp4_ctx *ctx);

void close_mp4(mp4_ctx *ctx);

read_ctx* create_mp4_readctx(FILE* fp, void *file_operator);

void set_mp4_readctx_info(read_ctx *ctx, void *fp, int vchn, void *buf, int buf_size,
                          void (*on_data)(void *param, int vchn, int type, void *data, size_t len, int64_t pts, int64_t dts, int flags), void *param);

int read_mp4(read_ctx* ctx);

void sync_context(read_ctx *ctx, mp4_ctx* wt_ctx);

uint32_t get_start_time(read_ctx* ctx);

int read_seek_mp4(read_ctx *ctx, int64_t* timestamp/* ms */);

void close_mp4_readctx(read_ctx *ctx);