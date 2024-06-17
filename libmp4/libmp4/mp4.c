#include "mp4.h"

#include "mp4-writer.h"
#include "mov-reader.h"
#include "mpeg4-avc.h"
#include "mov-internal.h"
#include "mp4-mutex.h"

#include <stdio.h>
#include <string.h>
#include <pthread.h>

extern struct mov_buffer_t *mov_file_buffer(void);
extern struct mov_buffer_t *mov_file_cache_buffer(void);

#define MP4_LOG(fmt, ...) printf("%s(): " fmt "\n", __func__, ##__VA_ARGS__)
#define CONDITION_LOG(con,fmt,...) if(con) {MP4_LOG(fmt, ##__VA_ARGS__);}

#define MOV_WRITER_H264_FMP4 0

void print_binary(unsigned char *data, int len)
{
    for (int i = 0; i < len; i++)
    {
        printf("%02x ", data[i]);
    }
    printf("\n");
}

void *custom_file_operator(int use_cache,
                           int (*read_data)(void *param, uint32_t type, void *data, uint64_t bytes),
                           int (*write_data)(void *param, int track, const void *data, uint64_t bytes, uint64_t *wrote_len))
{
    struct mov_buffer_t *op = use_cache?mov_file_cache_buffer():mov_file_buffer();
    op->write_data = write_data;
    op->read_data = read_data;

    return op;
}

#define CACHE_SIZE (40*1024)
mp4_ctx *create_mp4(FILE *fp, void *file_operator, int write_zeros)
{
    if (!fp || !file_operator)
    {
        MP4_LOG("fp or file_operator is null fp = 0x%x, fop = 0x%x",fp, file_operator);
        return 0;
    }

    mp4_ctx *ctx = (mp4_ctx *)malloc(sizeof(mp4_ctx));
    memset(ctx, 0, sizeof(mp4_ctx));
    ctx->audio_track = -1;
    ctx->video_track = -1;
    ctx->fp = fp;
    int flag = MOV_FLAG_COSTUM_FASTSTART;
    if (write_zeros)
    {
        flag |= MOV_FLAG_COSTUM_WRITZEROS;
    }

    ctx->cache = make_mov_file_cache(fp,CACHE_SIZE);

    struct mov_buffer_t *fop = file_operator;
    MP4_LOG("fp = 0x%x, fop = 0x%x",fp, file_operator);
    ctx->mp4_wr = mp4_writer_create(MOV_WRITER_H264_FMP4, fop, ctx->cache/* fp */, flag);
    
    MP4_LOG("create mp4,fp = 0x%x", (unsigned int)fp);

    return ctx;
}

void create_mp4_placeholder(FILE *fp, void *file_operator, int slow)
{
    if (!fp || !file_operator)
    {
        MP4_LOG("fp or file_operator is null");
        return ;
    }

    void* cache = make_mov_file_cache(fp,CACHE_SIZE);

    int flag = MOV_FLAG_COSTUM_FASTSTART | MOV_FLAG_COSTUM_WRITZEROS;
    if (slow)
    {
        flag |= MOV_FLAG_COSTUM_WRITESLOW;
    }

    struct mov_buffer_t *fop = file_operator;
    void* mp4_wr = mp4_writer_create(MOV_WRITER_H264_FMP4, fop, cache, flag);
    
    mp4_writer_destroy(mp4_wr);
    destroy_mov_file_cache(cache);

    MP4_LOG("create placeholder,fp = 0x%x", (unsigned int)fp);

}
/***************************/
struct mov_reader_t
{
	int flags;
	int have_read_mfra;
	
	struct mov_t mov;
};
/***************************/
mp4_ctx *mp4_continue(FILE *fp, void *file_operator)
{
    if (!fp || !file_operator)
    {
        MP4_LOG("fp or file_operator is null");
        return 0;
    }

    mp4_ctx *ctx = (mp4_ctx *)malloc(sizeof(mp4_ctx));
    memset(ctx, 0, sizeof(mp4_ctx));
    ctx->audio_track = -1;
    ctx->video_track = -1;
    ctx->fp = fp;
    int flag =MOV_FLAG_COSTUM_FASTSTART | MOV_FLAG_COSTUM_CONTINUEE;

    ctx->cache = make_mov_file_cache(fp,CACHE_SIZE);

    struct mov_buffer_t *fop = file_operator;
    
    MP4_LOG("try read last file");
    struct mov_reader_t* mp4_rd = mov_reader_create(mov_file_buffer(), fp);

    if (!mp4_rd || mp4_rd->mov.track_count < 2)
    {
        mov_reader_destroy(mp4_rd);
        int track_count = mp4_rd?mp4_rd->mov.track_count:0;
        MP4_LOG("read last mp4 failed, mp4_rd = 0x%x, track count = %d", mp4_rd, track_count);
        return 0;
    }

    fseek(fp,0,SEEK_SET);//重置fp游标
    ctx->mp4_wr = mp4_writer_create_ex(MOV_WRITER_H264_FMP4, fop, ctx->cache, flag, &(mp4_rd->mov));

    mov_reader_light_destroy(mp4_rd);
    
    MP4_LOG("create mp4,fp = 0x%x", (unsigned int)fp);

    return ctx;
}

char *nalu_startcode(char *data, int len)
{
    if (len < 4)
    {
        return NULL;
    }

    char *p = data;
    for (size_t i = 3; i < len; i++)
    {
        if (0x01 == p[i] && 0x00 == p[i - 1] && 0x00 == p[i - 2] && 0x00 == p[i - 3])
        {
            return p + i + 1;
        }
    }

    return NULL;
}

static void get_resolution(mp4_ctx *ctx, char *data, int size)
{
    char *p = data;
    int l = size;
    char *nalu = 0;

    while (0 != (nalu = nalu_startcode(p, l)))
    {
        l = l - (nalu - p);
        p = nalu;

        int nalutype = nalu[0] & 0x1f;

        if (nalutype == 7)
        {
            char *next = nalu_startcode(nalu, l);
            uint32 w = 0;
            uint32 h = 0;
            byte *sps = (byte *)nalu /* -4 */;
            size_t len = next - nalu - 4;
            parse_sps(sps, len, &w, &h);
            ctx->width = w;
            ctx->height = h;
            return;
        }
    }
}

int add_video(mp4_ctx *ctx, char *spspps, int len)
{
    // 00 00 00 01 67 64 00 28 ac 2c a8 07 80 22 5e 59 a8 08 08 08 10 00 00 00 01 68 ee 3c b0
    if (ctx->video_track == -1)
    {
        struct mpeg4_avc_t avc;
        memset(&avc, 0, sizeof(avc));

        get_resolution(ctx, spspps, len);
        MP4_LOG("mp4 resolution width = %d height = %d", ctx->width, ctx->height);

        int ret = mpeg4_avc_from_nalu((const uint8_t *)spspps, len, &avc);
        if (ret)
        {
            uint8_t extdata[64] = {0};
            int extlen = mpeg4_avc_decoder_configuration_record_save(&avc, extdata, 64);
            ctx->video_track = mp4_writer_add_video(ctx->mp4_wr, MOV_OBJECT_H264, ctx->width, ctx->height, extdata, extlen);

            // print_binary((unsigned char *)spspps, len);
            print_binary((unsigned char *)extdata, extlen);

            MP4_LOG("add video track success. extdata size = %d,track id = %d", extlen, ctx->video_track);
            return 0;
        }
    }

    return -1;
}

int add_audio(mp4_ctx *ctx)
{
    if (ctx->audio_track == -1)
    {
        ctx->audio_track = mp4_writer_add_audio(ctx->mp4_wr, MOV_OBJECT_G711a, 1, 16, 8000, 0, 0);
        MP4_LOG("add audio track success. track id = %d", ctx->audio_track);

        return 0;
    }
    return -1;
}

int init_segment(mp4_ctx *ctx)
{
    return mp4_writer_init_segment(ctx->mp4_wr);
}

int write_video_frame(mp4_ctx *ctx, char *data, int len, int64_t pts, int is_key)
{
    CONDITION_LOG(pts <= 0, "pts = %lld", pts);
    mp4_mutex_lock(SYNC_MUTEX);
    int ret = mp4_writer_write(ctx->mp4_wr, ctx->video_track, data, len, pts, pts, is_key);
    mp4_mutex_unlock(SYNC_MUTEX);
    return ret;
}

int write_audio_frame(mp4_ctx *ctx, char *data, int len, int64_t pts)
{
    CONDITION_LOG(pts <= 0, "pts = %lld", pts);
    mp4_mutex_lock(SYNC_MUTEX);
    int ret = mp4_writer_write(ctx->mp4_wr, ctx->audio_track, data, len, pts, pts, 0);
    mp4_mutex_unlock(SYNC_MUTEX);
    return ret;
}

int update_free_data(mp4_ctx *ctx, int offset, int len, void *val)
{
    return mp4_writer_update_free_data(ctx->mp4_wr, offset, len, val);
}
int update_moov(mp4_ctx *ctx)
{
    mp4_mutex_lock(MOOV_MUTEX);
    int ret = mp4_writer_update_moov(ctx->mp4_wr);
    mp4_mutex_unlock(MOOV_MUTEX);

    return ret;
}

void close_mp4(mp4_ctx *ctx)
{
    mp4_writer_destroy(ctx->mp4_wr);
    destroy_mov_file_cache((mov_file_cache_t *)ctx->cache);
    MP4_LOG("close mp4, fp = 0x%x", (unsigned int)ctx->fp);
    free(ctx);
}

static void mov_video_info(void *param, uint32_t track, uint8_t object, int width, int height, const void *extra, size_t bytes)
{
    read_ctx *ctx = (read_ctx *)param;
    ctx->video_track = track;
    MP4_LOG("track %u: type = %d, width = %d, height = %d",track,(int)object,width,height);
}    

static void mov_audio_info(void *param, uint32_t track, uint8_t object, int channel_count, int bit_per_sample, int sample_rate, const void *extra, size_t bytes)
{
    read_ctx *ctx = (read_ctx *)param;
    ctx->audio_track = track;
    MP4_LOG("track %u: type = %d, chn_cnt = %d, bit_deth = %d, samp_rate = %d",track,(int)object,channel_count,bit_per_sample,sample_rate);
}

read_ctx *create_mp4_readctx(FILE *fp, void *file_operator)
{
    read_ctx *ctx = calloc(1, sizeof(read_ctx));

    mp4_mutex_lock(MOOV_MUTEX);
    ctx->mp4_rd = mov_reader_create(file_operator ? (struct mov_buffer_t *)file_operator : mov_file_buffer(), fp);
    mp4_mutex_unlock(MOOV_MUTEX);
    if (!ctx->mp4_rd)
    {
        free(ctx);
        return 0;
    }

    struct mov_reader_trackinfo_t info = {mov_video_info, mov_audio_info};
    mov_reader_getinfo(ctx->mp4_rd, &info, ctx);

    return ctx;
}

void set_mp4_readctx_info(read_ctx *ctx, void *fp, int vchn, void *buf, int buf_size,
                          void (*on_data)(void *param, int vchn, int type, void *data, size_t len, int64_t pts, int64_t dts, int flags), void *param)
{
    ctx->fp = fp;
    ctx->vchn = vchn;
    ctx->buf = buf;
    ctx->buf_size = buf_size;
    ctx->on_data = on_data;
    ctx->param = param;
}

static void ondata(void *param, uint32_t track, const void *buffer, size_t bytes, int64_t pts, int64_t dts, int flags)
{
    // xprint("ondata track %u: bytes = %u, pts = %lld, dts = %lld, flags = %d", track, bytes, pts, dts, flags);
    read_ctx *ctx = (read_ctx *)param;
    if (ctx->on_data)
    {
        ctx->last_pts = pts;
        ctx->on_data(ctx->param, ctx->vchn, track == ctx->audio_track, (void*)buffer, bytes, pts, dts, flags);
    }
}

int read_mp4(read_ctx *ctx)
{
    struct mov_reader_t* rd = ctx->mp4_sync?ctx->mp4_sync:ctx->mp4_rd;
    return mov_reader_read(rd,ctx->buf,ctx->buf_size, ondata, ctx);
}

struct mov_writer_t
{
	struct mov_t mov;
	uint64_t mdat_size;
	uint64_t mdat_offset;
	uint64_t moov_offset;
	uint64_t free_data_offset;
};

void sync_context(read_ctx *ctx, mp4_ctx* wt_ctx)
{
    mp4_mutex_lock(SYNC_MUTEX);
    if (ctx->mp4_sync == 0)
    {
        ctx->mp4_sync = mov_read_copy_reader(ctx->mp4_rd, &(wt_ctx->mp4_wr->mov->mov));
        MP4_LOG("create sync context");
    }
    else
    {
        mov_read_sync_reader(ctx->mp4_sync, &(wt_ctx->mp4_wr->mov->mov));
        MP4_LOG("sync context");
    }
    mp4_mutex_unlock(SYNC_MUTEX);
}

uint32_t get_start_time(read_ctx* ctx)
{
    char* data = ctx->mp4_rd->mov.pw_data;
    return (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
}

int read_seek_mp4(read_ctx *ctx, int64_t *timestamp /* ms */)
{
    struct mov_reader_t* rd = ctx->mp4_sync?ctx->mp4_sync:ctx->mp4_rd;
    return mov_reader_seek(rd, timestamp);
}

void close_mp4_readctx(read_ctx *ctx)
{
    MP4_LOG("close mp4 read ctx, fp = 0x%x", (unsigned int)ctx->fp);
    mov_reader_destroy(ctx->mp4_rd);
    mov_read_destroy_copy_reader(ctx->mp4_sync);
    memset(ctx, 0, sizeof(read_ctx));
    free(ctx);
}

