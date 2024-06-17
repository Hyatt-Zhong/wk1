#include "ImplementMp4.h"
#include "ZRT_Stor.h"
#include "ZRT_tcBlkBuf.h"
#include "playback.h"
#include "mp4-mutex.h"

#include <string.h>

#define CONTINUE_WRITE 0
#define _CHECK_POINT(p,ret) if(!p) { ZRT_LOG_ERR(#p" is null\n"); return ret; }
#define CHECK_POINT(p) _CHECK_POINT(p,)

#define GET_INFO(h) (Mp4Info *)((ZRT_StorHandler *)h)->pMp4Info
#define GET_READER(h) (Mp4Reader *)((playback_context_t *)h)->mp4_reader

#define TRACK_TAG(a, b, c, d) (((a) << 24) | ((b) << 16) | ((c) << 8) | (d))
#define TRACK_VIDEO TRACK_TAG('v', 'i', 'd', 'e')

#define LEN_ERR_LOG(len,param1,param2,param3) if(len<=0){ZRT_LOG_ERR("##len error##"#len"=%d "#param1"=%ld "#param2"=%ld "#param3"=%ld \n",len,param1,param2,param3);}

#define UPDATE_MOOV_INTERVAL 32000 //32s更新一次

#define TEST(X)
// #define TEST(X) X

TEST(
FILE *g_fp = 0;
char g_path[60] = {0};
int g_count = 201;

void test_open_file(char *prefix) {
    if (g_fp)
    {
        return;
    }
    time_t tm = time(0);
    sprintf(g_path, "%s%ld.alaw", prefix, tm);
    xprint("create test file %s", g_path);
    char *path = g_path;
    FILE *fp = fopen(path, "wb+");
    fflush(fp);
    char zeros[1024] = {0};
    for (size_t i = 0; i < g_count; i++)
    {
        fwrite(zeros, 1, 1024, fp);
    }
    long fszie = ftell(fp);
    assert(fszie >= g_count * 1024 - 1);
    fclose(fp);
    int fd = open(path, O_RDWR | O_SYNC | O_CLOEXEC);
    fp = fdopen(fd, "rb+");
    // fp = fopen(path, "rb+");
    if (!fp)
    {
        ZRT_LOG_INFO("cant open %s \n", path);
        return;
    }
    fflush(fp);
    fseek(fp, g_count * 1024 - 1, SEEK_SET);
    g_fp = fp;
} 
void test_close_file() {
    fclose(g_fp);
    g_fp = 0;
    xprint("close test file %s", g_path);
    memset(g_path, 0, 60);
} 
void test_write_data(void *data, int len) {
    char *blkData = NULL;
    tcblk_slots *frmBlk;
    int blkSize = 0;
    frmBlk = (tcblk_slots *)data;
    blkSize = tcblk_step_get_blk_data(frmBlk, 0, &blkData);
    // assert(len == blkSize && len == 1024);
    if (blkSize != fwrite(blkData, 1, blkSize, g_fp))
    {
        xprint("test write data err, file = %s, len = %d, fp = 0x%x, err = %s", g_path, blkSize, g_fp, strerror(errno));
    }
    int offset = ftell(g_fp);
    // xprint("cur offset = %d",offset);
    fseek(g_fp, 0, SEEK_SET);
    fwrite("blkData", 1, 3, g_fp);
    fseek(g_fp, offset, SEEK_SET);
}
)

TEST(
char* sync_test = "/media/2024-02-28/sync_test.file";
FILE* wr_fp = 0;
FILE* rd_fp = 0;
void open_file_wr()
{
    if (wr_fp)
    {
        return;
    }
    wr_fp = fopen(sync_test,"wb+");
    fflush(wr_fp);
}    
void write_file_()
{
    static int count = 0;
    if (!wr_fp)
    {
        return;
    }
    char buf[30] = {0};
    sprintf(buf, "hello 123456 =====> %07d\n", count++);
    fwrite(buf,strlen(buf), 1,wr_fp);
    printf("wr###%s",buf);
}
void open_file_rd()
{
    if (rd_fp)
    {
        return;
    }
    rd_fp = fopen(sync_test,"r");
}
void read_file_()
{
    if (!rd_fp)
    {
        return;
    }
    char line[100]={0};
    int res = fread(line,28,1,rd_fp);
    if (res!=1)
    {
        xprint("sync test read errr");
    }
    else
        printf("rd---%s",line);
}
)

static char *h264_startcode(char *data, int len)
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

static uint8_t *make_len(uint8_t *pbuf, int val)
{
    pbuf[0] = (uint8_t)((val >> 24) & 0xFF);
    pbuf[1] = (uint8_t)((val >> 16) & 0xFF);
    pbuf[2] = (uint8_t)((val >> 8) & 0xFF);
    pbuf[3] = (uint8_t)((val >> 0) & 0xFF);
    return pbuf;
}

void print_frame_info(void *data, int size)
{
    char *p = data;
    int l = size;
    char *nalu = 0;

    while (0 != (nalu = h264_startcode(p, l)))
    {
        l = l - (nalu - p);
        p = nalu;

        int nalutype = nalu[0] & 0x1f;
        printf("nalutype = %d\n", nalutype);
    }
}

static int check_video_frame(void *data, int size)
{
    char *p = data;
    int l = size;
    char *nalu = 0;

    int count = 0;
    while (0 != (nalu = h264_startcode(p, l)))
    {
        l = l - (nalu - p);
        p = nalu;

        count++;

        if (count > 4)
        {
            print_frame_info(data, size);
            ZRT_LOG_ERR("err pack, nalu count = %d\n", count);
            return 0;
        }
    }

    if (count != 2 && count != 4)
    {
        ZRT_LOG_ERR("err pack, nalu count = %d\n", count);
        return 0;
    }

    return 1;
}
static size_t safe_write(const void *data, size_t s, size_t n, void *ptr)
{
    mp4_mutex_lock(FILE_MUTEX);
    size_t res = fwrite(data, s, n, ptr);
    int count = 0;
    while (res != n && 5 > (count++))
    {
        ZRT_LOG_WARN("write err res = %d err = %s, %d time retry\n", res, strerror(errno), count);
        res = fwrite(data, s, n, ptr);
    }
    mp4_mutex_unlock(FILE_MUTEX);

    if (res != n)
    {
        size_t datalen = s * n;
        ZRT_LOG_ERR("cant write data res = %d, fp = 0x%x, datalen = %d, err: %s\n", res, ptr, datalen, strerror(errno));
        return 0;
    }
    return n;
}

static size_t (*data_write)(const void *data, size_t s, size_t n, void *ptr) = safe_write;
static void sync_rw_context(ZRT_StorHandler *stor, playback_context_t* pbc, int force_sync);

size_t cache_write(const void *data, size_t s, size_t n, void *ptr)
{
    mov_file_cache_t *fc = ptr;
    size_t count = s * n;
    memcpy(fc->ptr + fc->off, data, count);
    fc->off += count;
    return n;
}

int write_len(int len, void *fp)
{
    uint8_t pbuf[4] = {0};
    make_len(pbuf, len);
    int res = data_write(pbuf, 4, 1, fp);
    if (res != 1)
    {
        ZRT_LOG_ERR("write len error, res = %d, fp = 0x%x err: %s \n", res, fp, strerror(errno));
        return 0;
    }

    return 4;
}

int write_nalu(void *data, int len, void *fp)
{
    int res = data_write(data, len, 1, fp);
    if (res != 1)
    {
        ZRT_LOG_ERR("write data error, res = %d, fp = 0x%x, datalen = %d, data = 0x%x, err: %s\n", res, fp, len, data, strerror(errno));
        return 0;
    }
    return len;
}

// SEI SPS PPS 的大小加4个nalu分隔符(0x00000001)一般不会超过TC_BUF_BLK_SIZE
// 所以对第一个块做长度处理就行了
static int write_first_blk(void *data, int size, int full_len, void *fp)
{
    int wrote_len = 0;

    char *p = data;
    int l = size;
    char *nalu = 0;

    int count = 0;
    while (0 != (nalu = h264_startcode(p, l)))
    {
        char *next = h264_startcode(nalu, l - (nalu - p));
        if (next)
        {
            int len = next - nalu - 4;
            LEN_ERR_LOG(len, next, nalu, 0);

            wrote_len += write_len(len, fp);
            wrote_len += write_nalu(nalu, len, fp);
        // xprint("len = %d, type = %d",len,nalu[0] & 0x1f);
        }
        else
        {
            int len = full_len - (nalu - (char *)data);
            LEN_ERR_LOG(len, full_len, nalu, data);

            wrote_len += write_len(len, fp);

            int remain = size - (nalu - (char *)data);
            LEN_ERR_LOG(remain, size, nalu, data);

            wrote_len += write_nalu(nalu, remain, fp);
        // xprint("len = %d, type = %d",len,nalu[0] & 0x1f);
        }
        l = l - (nalu - p);
        p = nalu;

        count++;
    }
    if ((count != 2 && count != 4))
    {
        ZRT_LOG_ERR("err pack, nalu count = %d\n", count);
    }

    if (wrote_len != size)
    {
        // ZRT_LOG_ERR("wrote error wrote_len = %d size = %d\n", wrote_len, size);
    }

    return wrote_len;
}

//tcblk_slots是一个内存块结构，是由多个内存块组成的，这是嵌入式程序为了合理使用内存而做的，如果是普通程序，一般直接使用内存的起始指针
static void write_tcblk(void* fp, int track_type, tcblk_slots* frmBlk, int bytes, uint64_t *wrote_len)
{
    int blkSize = 0;
    char *blkData = NULL;
    int offset = 0;

    int first_blk = 1;
    while (1)
    {
        blkSize = tcblk_step_get_blk_data(frmBlk, offset, &blkData);
        if ((0 == blkSize) || (NULL == blkData))
        {
            break;
        }
        offset += blkSize;

        if (track_type == (int)TRACK_VIDEO && first_blk)
        {
            first_blk = 0;
            // SEI SPS PPS 的大小加4个nalu分隔符(0x00000001)一般不会超过TC_BUF_BLK_SIZE
            *wrote_len += write_first_blk(blkData, blkSize, bytes, fp);
            continue;
        }
        // xprint("blkSize = %d",blkSize);
        int res = data_write(blkData, blkSize, 1, fp);
        if (res != 1)
        {
            ZRT_LOG_ERR("write data error, fp = 0x%x, datalen = %d, err: %s\n", fp, blkSize, strerror(errno));
        }
        else
        {
            *wrote_len += blkSize;
        }

    }
}

//把原始视频数据的nalu分隔符(0x00000001)替换成长度nalu长度才能被播放器正确识别
static int write_data(void *param, int track_type, const void *data, uint64_t bytes, uint64_t *wrote_len)
{
    *wrote_len = 0;

    mov_file_cache_t *fc = param;

    void *ptr = 0;
#if 1
	if (fc->off + bytes > fc->size)
	{
		if (1 != safe_write(fc->ptr, fc->off, 1, fc->fp))
		{
            ZRT_LOG_ERR("write err, off = %u \n", fc->off);
			fc->off = 0;
			fc->tell = ftell(fc->fp) + fc->off;
			return ferror(fc->fp);
		}
        xprint("pos = %ld, fp = 0x%x", ftell(fc->fp), fc->fp);
		fc->off = 0; // clear buffer
	}

    if (fc->off + bytes <= fc->size)
    {//这一帧+缓存原有<缓存空间 写到缓存
        ptr = fc;
        data_write = cache_write;
    }
    else
    {//这一帧特别大，超过缓存 直接写文件
        ZRT_LOG_INFO("directly write data to file, fc->off = %u bytes = %llu\n", fc->off, bytes);
        ptr = fc->fp;
        data_write = safe_write;
    }
#else
    // 如果之前有seek，那么当前fc->off等于0，如果没有seek，那么写数据之前需要将缓存区的数据先写入
	if (fc->off > 0)
	{
		if (1 != safe_write(fc->ptr, fc->off, 1, fc->fp))
		{
			fc->off = 0;
			fc->tell = ftell(fc->fp) + fc->off;
            ZRT_LOG_ERR("write err wrote = %llu, off = %u \n",*wrote_len, fc->off);
			return ferror(fc->fp);
		}
		fc->off = 0; // clear buffer
	}

    ptr = fc->fp;
    data_write = safe_write;
#endif

    write_tcblk(ptr, track_type, (tcblk_slots*)data, bytes, wrote_len);

    //写完更新tell
	fc->tell = ftell(fc->fp) + fc->off;
    if (*wrote_len != bytes || *wrote_len == 0)
    {
        ZRT_LOG_ERR("wrote not eq bytes, wrote = %llu, bytes = %llu \n",*wrote_len, bytes);
        return -1;
    }

    return 0;
}

static int write_data_nocache(void *param, int track_type, const void *data, uint64_t bytes, uint64_t *wrote_len)
{
    *wrote_len = 0;
    FILE *fp = param;

    write_tcblk(fp, track_type, (tcblk_slots*)data, bytes, wrote_len);
    // TEST(test_write_data(data,bytes));

    if (*wrote_len != bytes || *wrote_len == 0)
    {
        ZRT_LOG_ERR("wrote not eq bytes, wrote = %llu, bytes = %llu \n",*wrote_len, bytes);
        return -1;
    }

    return 0;
}

static void write_start_time(mp4_ctx *ctx, uint32_t tm)
{
    update_free_data(ctx, 0, 4, &tm);
}

//大概每隔30s更新一次moov，这是为了保证断电不会丢失整段数据
static int imp_update_moov(mp4_ctx *ctx, uint32_t tm, int chn)
{
    if (ctx->last_update_moov == 0)
    {
        ctx->last_update_moov = tm;
        return 0;
    }

    if (tm - ctx->last_update_moov >= UPDATE_MOOV_INTERVAL + chn*1000)
    {
        ctx->last_update_moov = tm;
        update_moov(ctx);
        return 1;
    }
    return 0;
}

//时间戳需要从0开始，而设备编码的时间戳并不是，需要重新计算时间戳
static uint32_t caculate_timestamp(int64_t *tm_base, int64_t tm_start, int64_t pts)
{
    if (*tm_base == 0)
    {
        *tm_base = pts;
        ZRT_LOG_INFO("tm_base = %lld, tm_start = %lld\n", *tm_base, tm_start);
    }

    int64_t dt = (pts - *tm_base);
    dt = dt < 0 ? 0 : dt;//避免pts-base出现负数
#if CONTINUE_WRITE
    uint32_t ret = dt / 1000 + tm_start;
#else
    uint32_t ret = dt / 1000 /* + 0 */;
#endif

    return ret;
}

static int try_create_continue(char *path, int chn, Mp4Info *pinfo)
{
    int i = chn;
    FILE *fp = 0;
    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC);
    fp = fdopen(fd, "rb+");
    if (!fp)
    {
        ZRT_LOG_INFO("cant open %s \n",path);
        return 0;
    }

    fflush(fp);
    ZRT_LOG_INFO("try read last file %s\n", path);
    void* ctx = mp4_continue(fp, custom_file_operator(1 /* use cache */, 0, write_data));
    if (ctx)
    {
        pinfo->fp[i] = fp;
        pinfo->ctx[i] = ctx;
        return 1;
    }
    else
    {//不能正常用reader打开，说明没有moov头，可以直接覆写
        fclose(fp);
        ZRT_LOG_INFO("file %s isnot whole mp4\n",path);
    }
    return 0;
}

//续写，但因为fdopen(fd, "rb+");总是发生写入错误，无法使用
#if CONTINUE_WRITE
static int create_mp4_by_path(char *path,char *prefix, int chn, Mp4Info *pinfo)
{
    FILE *fp = 0;
    int i = chn;
    int continuee = 0;
    char tmp[MP4_PATH_LEN]={0};
    sprintf(path, "%s_%d.mp4", prefix, i);
    sprintf(tmp, "%s_%d.mp4.tmp", prefix, i);
    // if (access(path, 0) == 0)
    if(0)
    {
       continuee = 1;
    }
    else if (access(tmp, 0) == 0)
    {
        rename(tmp, path);
    }
    else
    {
        fp = fopen(path, "wb+");
        
        create_mp4_placeholder(fp, custom_file_operator(1 /* use cache */, 0, write_data), 0);
        
        fclose(fp);
    }

    if (continuee&&try_create_continue(path,chn, pinfo))
    {
        return continuee;
    }

    ZRT_LOG_INFO("reopen %s, continuee = %d\n", path, continuee);
    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC);
    fp = fdopen(fd, "rb+");
    if (!fp)
    {
        ZRT_LOG_INFO("cant open %s \n",path);
        return -1;
    }

    fflush(fp);
    pinfo->fp[i] = fp;
    pinfo->ctx[i] = create_mp4(fp, custom_file_operator(1 /* use cache */, 0, write_data), 0);

    return 0;
}
#else
// static void create_mp4_by_path(char *path,char *prefix, int chn, Mp4Info *pinfo)
// {
//     FILE *fp = 0;
//     int i = chn;

//     sprintf(path, "%s_%d.mp4", prefix, i);
//     // sprintf(path, "/media/%s_%d.mp4", prefix + 18, i);

//     fp = fopen(path, "wb+");
//     char zeros[1024] = {0};
//     for (size_t i = 0; i < 201; i++)
//     {
//         fwrite(zeros, 1, 1024, fp);
//     }
//     long fszie = ftell(fp);
//     assert(fszie >= 201 * 1024 - 1);

//     fclose(fp);

//     int fd = open(path, O_RDWR | O_SYNC | O_CLOEXEC);
//     fp = fdopen(fd, "rb+");
//     // fp = fopen(path, "rb+");

//     if (!fp)
//     {
//         ZRT_LOG_INFO("cant open %s \n", path);
//         return;
//     }

//     fseek(fp, 0, SEEK_END);
//     fszie = ftell(fp);
//     fseek(fp, 0, SEEK_SET);
//     ZRT_LOG_INFO("file size = %d \n", fszie);

//     fflush(fp);
//     pinfo->fp[i] = fp;
//     pinfo->ctx[i] = create_mp4(fp, custom_file_operator(1 /* use cache */, 0, write_data), 0);
//     // pinfo->ctx[i] = create_mp4(fp, custom_file_operator(0 /* use cache */, 0, write_data_nocache), 0);
// }

static int create_mp4_by_path(char *path,char *prefix, int chn, Mp4Info *pinfo)
{
    FILE *fp = 0;
    int i = chn;
    
    sprintf(path, "%s_%d.mp4", prefix, i);

    fp = fopen(path, "wb+");
    
    fflush(fp);
    pinfo->fp[i] = fp;
    pinfo->ctx[i] = create_mp4(fp, custom_file_operator(1 /* use cache */, 0, write_data), 1);

    return 0;
}
#endif

void imp_create_mp4(void *handle, char *prefix, int chn_count, int64_t tm_start)
{
    CHECK_POINT(handle);

    ((ZRT_StorHandler *)handle)->pMp4Info = calloc(1, sizeof(Mp4Info));

    Mp4Info *pinfo = GET_INFO(handle);
    CHECK_POINT(pinfo);

    pinfo->tm_start = tm_start < 200 ? 0:tm_start ;
    ZRT_LOG_INFO("tm_start = %lld\n", pinfo->tm_start);

    for (size_t i = 0; i < chn_count; i++)
    {
        char *path = pinfo->files[i];

        int continuee = create_mp4_by_path(path, prefix, i, pinfo);
        if (!continuee)
        {
            write_start_time(pinfo->ctx[i], (uint32_t)pinfo->tm_start);
            pinfo->tm_start = 0;
        }

        ZRT_LOG_INFO("create mp4 file: %s continuee = %d\n", path, continuee);
    }

    // TEST(test_open_file(prefix));
    // TEST(open_file_wr());
}

void imp_create_placeholder(void *handle, char *prefix, int chn_count)
{
    for (size_t i = 0; i < chn_count; i++)
    {
        char tmp[MP4_PATH_LEN]={0};
        sprintf(tmp, "%s_%d.mp4.tmp", prefix, i);

        void* fp = fopen(tmp, "wb+");
        
        create_mp4_placeholder(fp, custom_file_operator(1 /* use cache */, 0, write_data), 1);
        
        fclose(fp);
    }
    
}

void imp_add_video(void *handle, int chn, char *spspps, int len)
{
    CHECK_POINT(handle);

    Mp4Info *pinfo = GET_INFO(handle);
    CHECK_POINT(pinfo);

    mp4_ctx *ctx = pinfo->ctx[chn];

    add_video(ctx, spspps, len);
}

void imp_add_audio(void *handle, int chn)
{
    CHECK_POINT(handle);

    Mp4Info *pinfo = GET_INFO(handle);
    CHECK_POINT(pinfo);

    mp4_ctx *ctx = pinfo->ctx[chn];

    add_audio(ctx);
}

void imp_init_segment(void *handle, int chn)
{
    CHECK_POINT(handle);

    Mp4Info *pinfo = GET_INFO(handle);
    CHECK_POINT(pinfo);

    mp4_ctx *ctx = pinfo->ctx[chn];

    init_segment(ctx);
}

void imp_write_video(void *handle, int chn, char *data, int len, int64_t pts, int is_key)
{
    CHECK_POINT(handle);

    Mp4Info *pinfo = GET_INFO(handle);
    CHECK_POINT(pinfo);

    mp4_ctx *ctx = pinfo->ctx[chn];

    char *blk_data = NULL;
    int offset = 0;
    int blk_size = tcblk_step_get_blk_data((tcblk_slots *)data, offset, &blk_data);

    if (!check_video_frame(blk_data, blk_size))
    {
        return;
    }

    is_key ? add_video(ctx, blk_data, blk_size) : 0;
    uint32_t tm = caculate_timestamp(&pinfo->tm_base[chn], pinfo->tm_start, pts);
    int res = write_video_frame(ctx, data, len, tm, is_key);
    if (res != 0)
    {
        ZRT_LOG_ERR("write video frame err\n");
    }

    if(imp_update_moov(ctx, tm, chn))
    {
        ZRT_LOG_INFO("%s update moov\n", pinfo->files[chn]);
        pinfo->sync_rd[chn] = 1;
    }   
    
}

void imp_write_audio(void *handle, int chn_count, char *data, int len, int64_t pts)
{
    CHECK_POINT(handle);

    Mp4Info *pinfo = GET_INFO(handle);
    CHECK_POINT(pinfo);

    // xprint("imp_write_audio tm = %lld, len = %d", pts, len);
    for (size_t i = 0; i < chn_count; i++)
    {
        mp4_ctx *ctx = pinfo->ctx[i];
        add_audio(ctx);
        uint32_t tm = caculate_timestamp(&pinfo->tm_base[i], pinfo->tm_start, pts);
        int res = write_audio_frame(ctx, data, len, tm);
        if (res != 0)
        {
            ZRT_LOG_ERR("write audio frame err\n");
        }

        // imp_update_moov(ctx, tm);
    }
    // TEST(test_write_data(data,len));
    // TEST(write_file_());
}

void imp_close_mp4(void *handle, int chn_count)
{
    CHECK_POINT(handle);

    Mp4Info *pinfo = GET_INFO(handle);
    CHECK_POINT(pinfo);

	ZRT_PBHandler *PBH = ZRT_PB_Obj();
    sync_rw_context(handle, &PBH->context, 1);

    for (size_t i = 0; i < chn_count; i++)
    {
        mp4_ctx *ctx = pinfo->ctx[i];
        FILE *fp = pinfo->fp[i];
        char *filename = pinfo->files[i];

        ZRT_LOG_INFO("close mp4 file: %s\n", filename);
        close_mp4(ctx);

        fclose(fp);
    }

    free(((ZRT_StorHandler *)handle)->pMp4Info);
    ((ZRT_StorHandler *)handle)->pMp4Info = 0;
    // TEST(test_close_file());
}

static void init_mp4_reader(Mp4Reader *reader)
{
    memset(reader->ctx, 0, sizeof(reader->ctx));
    memset(reader->files, 0, sizeof(reader->files));
    memset(reader->fp, 0, sizeof(reader->fp));
    reader->read_cnt = 0;
}

static uint32_t byte_2_uint32(uint8_t *byte, int len) {
    if (len != 4)
    {
        return 0;
    }
    
    return (byte[0] << 24) | (byte[1] << 16) | (byte[2] << 8) | byte[3];
}

const uint32_t HEAD = 0x01000000;
static int replace_header(char* data, int size)
{
    if (size < 4)
    {
        return -2;
    }

    int count = 0;
    char *p = data;
    // xprint("pack size = %d",size);
    uint32_t record[10]={0};
    while (1)
    {
        if (p + 4 >= data + size)
        {
            break;
        }
        uint32_t len = byte_2_uint32((uint8_t*)p, 4);
        
        memcpy(p, &HEAD, 4);
        p += (len + 4);

        record[count] = len;
        count ++;

        if (p >= data + size)
        {
            break;
        }
    }

    if ((count != 2 && count != 4))
    {
        xprint("%d len = %u %u %u %u %u %u",count, record[0],record[1],record[2],record[3],record[4],record[5]);
        ZRT_LOG_ERR("err pack, nalu count = %d, pack size = %d\n", count, size);
        return -1;
    }
    return 0;
}

static int read_data_from_file(void *param, uint32_t handler_type, void *data, uint64_t bytes)
{
    FILE* fp = (FILE *)param;
    tcblk_slots *frmblk = (tcblk_slots *)data;

    // ZRT_LOG_INFO("fp = 0x%x\n",(unsigned int)fp);
    
    int dataOffset = 0;
    for (int i = 0; i < frmblk->blks_used; i++)
    {
        if (dataOffset >= bytes)
        {
            break;
        }

        int remainSize = bytes - dataOffset;
        int readSize = (remainSize < TC_BUF_BLK_SIZE) ? remainSize : TC_BUF_BLK_SIZE;

        long pos = ftell(fp);
        mp4_mutex_lock(FILE_MUTEX);
        size_t res = fread(frmblk->blks[i], readSize, 1, fp);
        mp4_mutex_unlock(FILE_MUTEX);
        if(1 != res)
        {
            ZRT_LOG_ERR("read err,res = %lu\n", res);
            return -1;
        }
        if (handler_type == TRACK_VIDEO && i == 0)
        {
            if(-1 == replace_header(frmblk->blks[i], readSize))
            {
                ZRT_LOG_INFO("replace header err pos = %ld bytes = %llu, fp = 0x%x\n", pos, bytes, fp);
                return -1;
            }
        }
        
        dataOffset += readSize;
    }
    return 0;
}

void imp_read_mp4(void* handle, char *prefix, int chn_count, void *buf, int buf_size, void (*on_data)(void* param, int chn, int type, void* data, size_t len, int64_t pts, int64_t dts, int flags), void* param)
{
    // TEST(open_file_rd());
    CHECK_POINT(handle);

    ((playback_context_t *)handle)->mp4_reader = calloc(1, sizeof(Mp4Reader));

    Mp4Reader *reader = GET_READER(handle);
    CHECK_POINT(reader);

    init_mp4_reader(reader);

    reader->read_cnt = chn_count;
    for (size_t i = 0; i < chn_count; i++)
    {
        sprintf(reader->files[i], "%s_%d.mp4", prefix, i);
        // sprintf(reader->files[i], "%s", "/media/1640x.mp4");
        if (0 != access(reader->files[i], 0))
        {
            ZRT_LOG_ERR("mp4 file not exist %s\n", reader->files[i]);
            goto CLEAN;
        }

        FILE* fp = fopen(reader->files[i], "rb");
        if (!fp)
        {
            ZRT_LOG_ERR("open file err, path = %s\n", reader->files[i]);
            goto CLEAN;
        }
        fseek(fp,0,SEEK_END);
        xprint("fp 0x%x pos = %ld", fp, ftell(fp));
        fseek(fp,0,SEEK_SET);

        reader->fp[i] = fp;
        ZRT_LOG_INFO("create mp4 reader: %s, fp = 0x%x\n", reader->files[i],(unsigned int)fp);
        read_ctx* ctx = create_mp4_readctx(fp, custom_file_operator(0/* no use cache */, read_data_from_file, 0));
        if(!ctx)
        {
            ZRT_LOG_ERR("read mp4 file err, path = %s\n", reader->files[i]);
            goto CLEAN;
        }

        set_mp4_readctx_info(ctx, fp, i, buf, buf_size, on_data, param);
        reader->ctx[i] = ctx;

        i == 0 ? reader->tm_start = get_start_time(ctx) : 0;

        ZRT_LOG_INFO("create mp4 reader success, start time = %lld\n", reader->tm_start);
    }

    return;

CLEAN:
    imp_close_mp4_reader(handle);
}

static void sync_rw_context(ZRT_StorHandler *stor, playback_context_t* pbc, int force_sync)
{
    Mp4Reader *reader = GET_READER(pbc);
    Mp4Info *pinfo = GET_INFO(stor);
    if ( !pinfo|| !reader)
    {
        return;
    }

    if (!force_sync && strcmp(pinfo->files[0], reader->files[0]) != 0)
    {
        return;
    }

    for (size_t i = 0; i < reader->read_cnt; i++)
    {
        if (pinfo->sync_rd[i] == 1 || force_sync)
        {
            read_ctx *ctx = reader->ctx[i];
            read_ctx *wt_ctx = pinfo->ctx[i];
            sync_context(ctx, wt_ctx);
            pinfo->sync_rd[i] = 0;
            ZRT_LOG_INFO("sync read context, file: %s\n", pinfo->files[i]);
        }
    }
}

static void reopen_mp4(Mp4Reader* reader, int res[])
{
    int chn_count = reader->read_cnt;
    for (size_t i = 0; i < chn_count; i++)
    {
        if (res[i] == -1)
        {
            read_ctx record;

            {
                read_ctx *ctx = reader->ctx[i];
                memcpy(&record, ctx, sizeof(read_ctx));
                close_mp4_readctx(ctx);
                FILE *fp = reader->fp[i];
                fclose(fp);
            }

            {
                if (0 != access(reader->files[i], 0))
                {
                    ZRT_LOG_ERR("mp4 file not exist %s\n", reader->files[i]);
                    // continue;
                }

                FILE *fp = fopen(reader->files[i], "rb");
                if (!fp)
                {
                    ZRT_LOG_ERR("open file err, path = %s\n", reader->files[i]);
                    continue;
                }
                reader->fp[i] = fp;
                
                ZRT_LOG_INFO("create mp4 reader: %s, fp = 0x%x\n", reader->files[i], (unsigned int)fp);
                read_ctx *ctx = create_mp4_readctx(fp, custom_file_operator(0 /* no use cache */, read_data_from_file, 0));
                if (!ctx)
                {
                    ZRT_LOG_ERR("read mp4 file err, path = %s\n", reader->files[i]);
                    close(fp);
                    continue;
                }

                set_mp4_readctx_info(ctx, fp, i, record.buf, record.buf_size, record.on_data, record.param);
                reader->ctx[i] = ctx;
            }

            {
                read_ctx *ctx = reader->ctx[i];

                int64_t seek_tm = record.last_pts;
                int64_t seek_res = seek_tm;
                read_seek_mp4(ctx, &seek_res);

                ZRT_LOG_INFO("reopen mp4 success, seek to time = %lld result = %lld \n", seek_tm, seek_res);
            }
        }
    }
}

int imp_read_once(void* handle)
{
    // TEST(read_file_());
    _CHECK_POINT(handle, -1);

    Mp4Reader *reader = GET_READER(handle);
    _CHECK_POINT(reader, -1);

	ZRT_StorHandler *stor = ZRT_STOR_Obj();
    sync_rw_context(stor, handle, 0);

    int res[MAX_STREAM] = {0};
    for (size_t i = 0; i < reader->read_cnt; i++)
    {
        read_ctx* ctx = reader->ctx[i];
        res[i] = read_mp4(ctx);
        if (res[i] != 1)
        {
            ZRT_LOG_INFO("read mp4 return %d, index %d\n", res[i], i);
        }

        if (res[i] == 0)
        {
            return -1;
        }
    }

    reopen_mp4(reader, res);

    return 0;
}

int imp_mp4_seek(void *handle, int64_t tm)
{
    _CHECK_POINT(handle, -1);

    Mp4Reader *reader = GET_READER(handle);
    _CHECK_POINT(reader, -1);

    int64_t seek_tm = tm - reader->tm_start;
    seek_tm = seek_tm > 0 ? seek_tm : 0;

    int64_t seek_res = seek_tm;
    for (size_t i = 0; i < reader->read_cnt; i++)
    {
        read_ctx* ctx = reader->ctx[i];
        read_seek_mp4(ctx, &seek_res);
    }
    ZRT_LOG_INFO("seek to %lld, tm_start = %lld, seek_tm = %lld, seek_res = %lld\n",tm, reader->tm_start, seek_tm, seek_res);

    return 0;
}

void imp_close_mp4_reader(void *handle)
{
    CHECK_POINT(handle);

    Mp4Reader *reader = GET_READER(handle);
    CHECK_POINT(reader);

    for (size_t i = 0; i < reader->read_cnt; i++)
    {
        read_ctx *ctx = reader->ctx[i];
        close_mp4_readctx(ctx);
        FILE *fp = reader->fp[i];
        fclose(fp);

        ZRT_LOG_INFO("close mp4 reader: %s\n", reader->files[i]);
    }

    free(reader);
    ((playback_context_t *)handle)->mp4_reader = 0;
}
