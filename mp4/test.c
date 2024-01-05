#include "fmp4-writer.h"
#include "mov-format.h"
#include "mov-reader.h"


#define __USE_LARGEFILE64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>


#define fseek64 fseeko64
#define ftell64 ftello64

struct mov_file_cache_t
{
	FILE* fp;
	uint8_t ptr[800];
	unsigned int len;
	unsigned int off;
	uint64_t tell;
};

static int mov_file_read(void* fp, void* data, uint64_t bytes)
{
    if (bytes == fread(data, 1, bytes, (FILE*)fp))
        return 0;
	return 0 != ferror((FILE*)fp) ? ferror((FILE*)fp) : -1 /*EOF*/;
}

static int mov_file_write(void* fp, const void* data, uint64_t bytes)
{
	return bytes == fwrite(data, 1, bytes, (FILE*)fp) ? 0 : ferror((FILE*)fp);
}

static int mov_file_seek(void* fp, int64_t offset)
{
	return fseek64((FILE*)fp, offset, offset >= 0 ? SEEK_SET : SEEK_END);
}

static int64_t mov_file_tell(void* fp)
{
	return ftell64((FILE*)fp);
}

static int mov_file_cache_read(void* fp, void* data, uint64_t bytes)
{
	uint8_t* p = (uint8_t*)data;
	struct mov_file_cache_t* file = (struct mov_file_cache_t*)fp;
	while (bytes > 0)
	{
		assert(file->off <= file->len);
		if (file->off >= file->len)
		{
			if (bytes >= sizeof(file->ptr))
			{
				if (bytes == fread(p, 1, bytes, file->fp))
				{
					file->tell += bytes;
					return 0;
				}
				return 0 != ferror(file->fp) ? ferror(file->fp) : -1 /*EOF*/;
			}
			else
			{
				file->off = 0;
				file->len = (unsigned int)fread(file->ptr, 1, sizeof(file->ptr), file->fp);
				if (file->len < 1)
					return 0 != ferror(file->fp) ? ferror(file->fp) : -1 /*EOF*/;
			}
		}

		if (file->off < file->len)
		{
			unsigned int n = file->len - file->off;
			n = n > bytes ? (unsigned int)bytes : n;
			memcpy(p, file->ptr + file->off, n);
			file->tell += n;
			file->off += n;
			bytes -= n;
			p += n;
		}
	}

	return 0;
}

static int mov_file_cache_write(void* fp, const void* data, uint64_t bytes)
{
	struct mov_file_cache_t* file = (struct mov_file_cache_t*)fp;
	
	file->tell += bytes;

	if (file->off + bytes < sizeof(file->ptr))
	{
		memcpy(file->ptr + file->off, data, bytes);
		file->off += (unsigned int)bytes;
		return 0;
	}

	// write buffer
	if (file->off > 0)
	{
		if (file->off != fwrite(file->ptr, 1, file->off, file->fp))
			return ferror(file->fp);
		file->off = 0; // clear buffer
	}

	// write data;
	return bytes == fwrite(data, 1, bytes, file->fp) ? 0 : ferror(file->fp);
}

static int mov_file_cache_seek(void* fp, int64_t offset)
{
	int r;
	struct mov_file_cache_t* file = (struct mov_file_cache_t*)fp;
	if (offset != file->tell)
	{
		if (file->off > file->len)
		{
			// write bufferred data
			if(file->off != fwrite(file->ptr, 1, file->off, file->fp))
				return ferror(file->fp);
		}

		file->off = file->len = 0;
		r = fseek64(file->fp, offset, offset >= 0 ? SEEK_SET : SEEK_END);
		file->tell = ftell64(file->fp);
		return r;
	}
	return 0;
}

static int64_t mov_file_cache_tell(void* fp)
{
	struct mov_file_cache_t* file = (struct mov_file_cache_t*)fp;
	if (ftell64(file->fp) != (int64_t)(file->tell + (uint64_t)(int)(file->len - file->off)))
		return -1;
	return (int64_t)file->tell;
	//return ftell64(file->fp);
}

const struct mov_buffer_t* mov_file_buffer(void)
{
	static struct mov_buffer_t s_io = {
		mov_file_read,
		mov_file_write,
		mov_file_seek,
		mov_file_tell,
	};
	return &s_io;
}

const struct mov_buffer_t* mov_file_cache_buffer(void)
{
	static struct mov_buffer_t s_io = {
		mov_file_cache_read,
		mov_file_cache_write,
		mov_file_cache_seek,
		mov_file_cache_tell,
	};
	return &s_io;
}

static uint8_t s_buffer[2 * 1024 * 1024];
static int s_audio_track = -1;
static int s_video_track = -1;

static void mov_onread(void* param, uint32_t track, const void* buffer, size_t bytes, int64_t pts, int64_t dts, int flags)
{
	fmp4_writer_t* fmp4 = (fmp4_writer_t*)param;
    int r = fmp4_writer_write(fmp4, track-1, buffer, bytes, pts, dts, flags);
    // assert(0 == r);
}

static void mov_video_info(void* param, uint32_t track, uint8_t object, int width, int height, const void* extra, size_t bytes)
{
    fmp4_writer_t* fmp4 = (fmp4_writer_t*)param;
    s_video_track = fmp4_writer_add_video(fmp4, object, width, height, extra, bytes);
}

static void mov_audio_info(void* param, uint32_t track, uint8_t object, int channel_count, int bit_per_sample, int sample_rate, const void* extra, size_t bytes)
{
    fmp4_writer_t* fmp4 = (fmp4_writer_t*)param;
    s_audio_track = fmp4_writer_add_audio(fmp4, object, channel_count, bit_per_sample, sample_rate, extra, bytes);
}

#include "mpeg4-avc.h"
#include "mp4-writer.h"
#define MOV_WRITER_H264_FMP4 0
#define H264_NAL(v)	(v & 0x1F)

static uint8_t s_buffer[2 * 1024 * 1024];
static uint8_t s_extra_data[64 * 1024];

struct mov_h264_test_t
{
	struct mp4_writer_t* mov;
	struct mpeg4_avc_t avc;

	int track;
	int width;
	int height;
	uint32_t pts, dts;
	const uint8_t* ptr;
    
    int vcl;
};

static uint8_t* file_read(const char* file, long* size)
{
	FILE* fp = fopen(file, "rb");
	if (fp)
	{
		fseek(fp, 0, SEEK_END);
		*size = ftell(fp);
		fseek(fp, 0, SEEK_SET);

		uint8_t* ptr = (uint8_t*)malloc(*size);
		fread(ptr, 1, *size, fp);
		fclose(fp);

		return ptr;
	}

	return NULL;
}

void print_bin(unsigned char* data,int len)
{
    for (int i = 0; i < len; i++)
    {
        printf("%x ",data[i]);
    }
    printf("\n");
}

static int h264_write(struct mov_h264_test_t* ctx, const void* data, int bytes)
{
    int vcl = 0;
    int update = 0;
    int n = h264_annexbtomp4(&ctx->avc, data, bytes, s_buffer, sizeof(s_buffer), &vcl, &update);

    if (ctx->track < 0)
    {
        if (ctx->avc.nb_sps < 1 || ctx->avc.nb_pps < 1)
        {
            //ctx->ptr = end;
            return -2; // waiting for sps/pps
        }

        int extra_data_size = mpeg4_avc_decoder_configuration_record_save(&ctx->avc, s_extra_data, sizeof(s_extra_data));
        if (extra_data_size <= 0)
        {
            // invalid AVCC
            assert(0);
            return -1;
        }
        print_bin(s_extra_data,extra_data_size);

        // TODO: waiting for key frame ???
        ctx->track = mp4_writer_add_video(ctx->mov, MOV_OBJECT_H264, ctx->width, ctx->height, s_extra_data, extra_data_size);
        if (ctx->track < 0)
            return -1;
		mp4_writer_init_segment(ctx->mov);
    }

    mp4_writer_write(ctx->mov, ctx->track, s_buffer, n, ctx->pts, ctx->pts, 1 == vcl ? MOV_AV_FLAG_KEYFREAME : 0);
    ctx->pts += 40;
    ctx->dts += 40;
    return 0;
}

static void h264_handler(void* param, const uint8_t* nalu, size_t bytes)
{
	struct mov_h264_test_t* ctx = (struct mov_h264_test_t*)param;
	assert(ctx->ptr < nalu);

    const uint8_t* ptr = nalu - 3;
//	const uint8_t* end = (const uint8_t*)nalu + bytes;
	uint8_t nalutype = nalu[0] & 0x1f;
    if (ctx->vcl > 0 && h264_is_new_access_unit((const uint8_t*)nalu, bytes))
    {
        int r = h264_write(ctx, ctx->ptr, ptr - ctx->ptr);
        if (-1 == r)
            return; // wait for more data

        ctx->ptr = ptr;
        ctx->vcl = 0;
    }

	if (1 <= nalutype && nalutype <= 5)
        ++ctx->vcl;
}

void mov_writer_h264(const char* h264, int width, int height, const char* mp4)
{
	struct mov_h264_test_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.track = -1;
	ctx.width = width;
	ctx.height = height;

	long bytes = 0;
	uint8_t* ptr = file_read(h264, &bytes);
	if (NULL == ptr) return;
	ctx.ptr = ptr;

	FILE* fp = fopen(mp4, "wb+");
	ctx.mov = mp4_writer_create(MOV_WRITER_H264_FMP4, mov_file_buffer(), fp, 0 );
	mpeg4_h264_annexb_nalu(ptr, bytes, h264_handler, &ctx);
	mp4_writer_destroy(ctx.mov);

	fclose(fp);
	free(ptr);
}


void main()
{
    mov_writer_h264("raw.264",1920,1080,"raw.mp4");return;
    const char* mp4="src.mp4";
    const char* outmp4="avx.mp4";
    struct mov_file_cache_t file, wfile;
    memset(&file, 0, sizeof(file));
    memset(&wfile, 0, sizeof(wfile));
    file.fp = fopen(mp4, "rb");
    wfile.fp = fopen(outmp4, "wb+");
    mov_reader_t* mov = mov_reader_create(mov_file_cache_buffer(), &file);
    fmp4_writer_t* fmp4 = fmp4_writer_create(mov_file_cache_buffer(), &wfile, MOV_FLAG_SEGMENT);

    struct mov_reader_trackinfo_t info = { mov_video_info, /* mov_audio_info */0 };
    mov_reader_getinfo(mov, &info, fmp4);
    fmp4_writer_init_segment(fmp4);
    
    while (mov_reader_read(mov, s_buffer, sizeof(s_buffer), mov_onread, fmp4) > 0)
    {
    }

    fmp4_writer_destroy(fmp4);
    mov_reader_destroy(mov);
    fclose(file.fp);
    fclose(wfile.fp);
}
