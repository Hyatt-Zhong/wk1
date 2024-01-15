#include "fmp4-writer.h"
#include "mov-writer.h"
#include "mov-format.h"

#include "mpeg4-avc.h"

#define __USE_LARGEFILE64
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>


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

static char *FindNextFlag(char *data, int len, char *flag)
{
    data += 11;
    char head[12] = {0};
    for (int i = 0; i < len - 11 - 11; i++)
    {
        memcpy(head, data, 11);
        if (strcmp(head, flag) == 0)
            return data;

        data++;
    }
    return 0;
}
static int ReadAVFile(FILE *fp, char **frame, char *buf, int size, int av, unsigned long long *pts, char *keyframe)
{
    int readlen = fread(buf, 1, size, fp); // 读取1*size个数据
    char *flag = av ? "audio_frame" : "video_frame";
    char head[12] = {0};
    memcpy(head, buf, 11);

    if (strcmp(head, flag) != 0)
        return -1;

    int dt;
    char *pos = FindNextFlag(buf, readlen, flag);
    if (!pos)
    {
        return -1;
    }
    else
    {
        dt = (pos - buf); // 高地址-低地址
        // 从当前位置前移rSize-frameSize个（第一次是文件头到第一个nalu包尾的数据）
        // 并且下次读取从该位置读取，所以不会一直是相同值
        fseek(fp, dt - readlen, SEEK_CUR);
    }
    int offset = av ? 19 : 20;
    memcpy(pts, buf + 11, 8);
    if (!av)
    {
        memcpy(keyframe, buf + 11 + 8, 1);
    }
    else
    {
        *keyframe = '\0';
    }

    *frame = buf + offset;

    return dt - offset;
}


static uint8_t* h264_startcode(uint8_t *data, size_t bytes,int* offset)
{
	size_t i;
	for (i = 2; i + 1 < bytes; i++)
	{
        if (i>=3)
        {
            if (0x01 == data[i] && 0x00 == data[i - 1] && 0x00 == data[i - 2]&& 0x00 == data[i - 3])
			    {*offset=4;return data + i + 1;}
        }
		if (0x01 == data[i] && 0x00 == data[i - 1] && 0x00 == data[i - 2])
			{*offset=3; return data + i + 1;}
	}

	return NULL;
}

void print_bin(unsigned char* data,int len)
{
    for (int i = 0; i < len; i++)
    {
        printf("%02x ",data[i]);
    }
    printf("\n");
}

uint8_t* get_data(uint8_t *data, size_t bytes,int *ilen)
{
    uint8_t *ret = data;
    int len=bytes;
    uint8_t *next = 0;
    int offset=0;
    while (next = h264_startcode(ret, len,&offset))
    {
        len = bytes - (next - data);
        int type = next[0] & 0x1f;
        // printf("%d \n",type);
        // if(type == 5){printf("----------------offset %d\n",offset);}
        ret = next;
    }
    ret -= offset;
    *ilen = bytes-(ret-data);

    // print_bin(data,(ret-data));
    return ret;
}

void change_nalu(char *pbuf, int size)
{
    //把每个nalu的00 00 00 01 替换成该nalu的长度
    uint8_t *p = pbuf;
    int offset = 0;
    uint8_t *next = h264_startcode(p + 4, size - 4, &offset);

    int len = (next ? next - offset - p : size) - 4;
    pbuf[0] = (uint8_t)((len >> 24) & 0xFF);
    pbuf[1] = (uint8_t)((len >> 16) & 0xFF);
    pbuf[2] = (uint8_t)((len >> 8) & 0xFF);
    pbuf[3] = (uint8_t)((len >> 0) & 0xFF);

    print_bin(pbuf,5);
}

//把每个nalu的00 00 00 01 替换成该nalu的长度
void change_filter(uint8_t *data, size_t bytes)
{
    uint8_t *p = data;
    int len = bytes;
    uint8_t *next = 0;
    int offset = 0;
    while (next = h264_startcode(p, len, &offset))
    {
        len = bytes - (next - data);
        p = next;

        char *nalu = next - offset;
        change_nalu(nalu, len + offset);
    }
}

void replace_nalu_header(char *pbuf, int size)
{
    //把每帧数据的00 00 00 01 替换成该帧的数据长度
    int len = size - 4;
    pbuf[0] = (uint8_t)((len >> 24) & 0xFF);
    pbuf[1] = (uint8_t)((len >> 16) & 0xFF);
    pbuf[2] = (uint8_t)((len >> 8) & 0xFF);
    pbuf[3] = (uint8_t)((len >> 0) & 0xFF);
}

// #define _FMP4
#ifdef _FMP4
#define mov_writer_write fmp4_writer_write
#define mov_writer_t fmp4_writer_t
#endif


int create_video_track(mov_writer_t *mov, char* data,int len)
{
#ifdef _FMP4
#define mov_writer_add_video fmp4_writer_add_video
#endif
    int v_track=-1;
    struct mpeg4_avc_t avc;
    memset(&avc, 0, sizeof(avc));

    char* spspps=data;
    int spsppslen=len;
    // {
    //     int ilen=0;
    //     char* idata = get_data(data,len,&ilen);
    //     spsppslen = len-ilen;
    // }
        // char buf[64]={0};
        // memcpy(buf,spspps,spsppslen);
    int ret = mpeg4_avc_from_nalu(spspps, spsppslen, &avc);
    if (ret)
    {
        uint8_t extdata[64] = {0};
        int extlen = mpeg4_avc_decoder_configuration_record_save(&avc, extdata, 64);
        print_bin(extdata,extlen);
        v_track = mov_writer_add_video(mov, MOV_OBJECT_H264, 1920, 1080, extdata, extlen);
        // v_track = mov_writer_add_video(mov, MOV_OBJECT_H264, 1920, 1080, 0, 0);
    }
    return v_track;
}

int handle_data(void* data,int size, void *buf,int buflen)
{
    int vcl = 0;
    int update = 0;

    struct mpeg4_avc_t avc;
    memset(&avc, 0, sizeof(avc));
    return h264_annexbtomp4(&avc, data, size, buf, buflen, &vcl, &update);
}

void put_video_audio(mov_writer_t *mov, int audio_track_id,int video_track_id)
{
    // {
    //     char foo[5]={0,0,1,0,0};
    //     int offset=0;
    //     h264_startcode(foo, 5,&offset);
    //     printf("offset %d\n",offset);
    // }
    FILE *vfp = fopen("VFrames.file", "rb"); // read binary
    FILE *afp = fopen("AFrames.file", "rb"); // read binary

    char *vbuf = (char *)malloc(500000);
    char *vmp4buf = (char *)malloc(500000);
    char *abuf = (char *)malloc(5000);
    char *aframe = 0;
    char *vframe = 0;

    int vsize=0;
    int asize=0;
    unsigned long long apts = 0;
    unsigned long long vpts = 0;
    char key_frame = 0;
    int iskey=0;

    unsigned long long s_vpts = 0;
    unsigned long long s_apts = 0;

    int audio_eof=0;
    // int write_video=0;
    while (1)
    {
        if (vframe == 0)
        {
            // 从原始文件中读取一帧视频帧
            vsize = ReadAVFile(vfp, &vframe, vbuf, 500000, 0, &vpts, &key_frame);
            iskey=key_frame;
            if (vsize < 0)
            {
                break;
            }
            if (s_vpts == 0)
            {
                s_vpts = vpts;
            }
        }

        if (aframe == 0)
        {
            asize = ReadAVFile(afp, &aframe, abuf, 5000, 1, &apts, &key_frame);
            if (s_apts == 0)
            {
                s_apts = apts;
            }
        }
        
        if (vpts<apts||audio_eof)
        {
            vpts-=s_vpts;
            vpts/=1000;

            #define COPY_SPSPPS 1
            #define USE_API 0

            if (!COPY_SPSPPS)
            {
                if (iskey)
                {
                    int ilen = 0;
                    uint8_t *data = get_data(vframe, vsize, &ilen);
                    vframe = data;
                    vsize = ilen;
                }
                else
                {
                }
            }

            if (USE_API)
            {
                vsize = handle_data(vframe, vsize, vmp4buf, 500000);
                vframe = vmp4buf;
            }
            else
            {
                change_filter(vframe,vsize);
            }
            
            mov_writer_write(mov,video_track_id,vframe,vsize,vpts,vpts,iskey);
            vframe = 0;
        }
        else
        {
            if (asize > 0)
            {
                apts-=s_apts;
                apts/=1000;
                mov_writer_write(mov, audio_track_id, aframe, asize, apts, apts, 0);
                aframe = 0;
            }
            else
            {
                audio_eof=1;
            }
        }
    }
    free(vbuf);
    free(abuf);
    free(vmp4buf);
    fclose(vfp);
    fclose(afp);
}

int main()
{
    FILE *fp = fopen("avx.mp4", "wb+");
    mov_writer_t *mov = mov_writer_create(mov_file_buffer(), fp, /* MOV_FLAG_FASTSTART */0);

    int v_track = -1;
    {
        char *buf = (char *)malloc(500000);
        char *frame = 0;
        unsigned long long pts = 0;
        char key_frame = 0;

        FILE *vfp = fopen("VFrames.file", "rb"); // read binary
        int vsize = ReadAVFile(vfp, &frame, buf, 500000, 0, &pts, &key_frame);
        assert(key_frame);
        v_track = create_video_track(mov, frame, vsize);
        fclose(vfp);
        free(buf);
    }

    int a_track = mov_writer_add_audio(mov, MOV_OBJECT_G711a, 1, 16, 8000, 0, 0);
    // int a_track = 0;
    put_video_audio(mov,a_track,v_track);

    mov_writer_destroy(mov);
    fclose(fp);
}

// int main()
// {
//     FILE *fp = fopen("avx.mp4", "wb+");
//     fmp4_writer_t *mov = fmp4_writer_create(mov_file_buffer(), fp, MOV_FLAG_SEGMENT);

//     int v_track = -1;
//     {
//         char *buf = (char *)malloc(500000);
//         char *frame = 0;
//         unsigned long long pts = 0;
//         char key_frame = 0;

//         FILE *vfp = fopen("VFrames.file", "rb"); // read binary
//         int vsize = ReadAVFile(vfp, &frame, buf, 500000, 0, &pts, &key_frame);
//         assert(key_frame);
//         v_track = create_video_track(mov, frame, vsize);
//         fclose(vfp);
//         free(buf);
//     }

//     int a_track = fmp4_writer_add_audio(mov, MOV_OBJECT_G711a, 1, 16, 8000, 0, 0);

//     fmp4_writer_init_segment(mov);

//     put_video_audio(mov,a_track,v_track);

//     fmp4_writer_destroy(mov);
//     fclose(fp);

//     return 0;
// }