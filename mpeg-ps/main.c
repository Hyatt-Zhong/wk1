#include <stdio.h>  
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include "mpeg-ps.h"
#include "mpeg-ts.h"

//我们的时间戳单位是微秒即1000kHz libmpeg库的单位是90kHz
//换算公式则是x * 1/1000k = y * 1/90k
//要求的y = x * 90k/1000k = x * 9/100

#define pts_bata (9/100)//时间戳乘法系数
#define pts_sigma 11.11//时间戳除法系数

#define print(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)

static void* ps_alloc(void* param, size_t bytes)
{
    static char s_buffer[2 * 1024 * 1024];
    assert(bytes <= sizeof(s_buffer));
    return s_buffer;
}

static void ps_free(void* param, void* packet)
{
    return;
}

static int ps_write(void* param, int stream, void* packet, size_t bytes)
{
    // fwrite(packet, bytes, 1, (FILE*)param);
    return 1 == fwrite(packet, bytes, 1, (FILE*)param) ? 0 : ferror((FILE*)param);
}

static char* FindNextFlag(char* data, int len, char* flag)
{
    data += 11;
    char head[12]={0};
    for(int i=0;i<len-11-11;i++)
    {
        memcpy(head,data,11);
        if(strcmp(head,flag)==0)
            return data;

        data++;
    }
    return 0;
}
static int ReadAVFile(FILE *fp, char**frame, char *buf, int size, int av,unsigned long long * pts,char* keyframe)
{
    int readlen = fread(buf, 1, size, fp); // 读取1*size个数据
    char *flag=av?"audio_frame":"video_frame";
    char head[12]={0};
    memcpy(head,buf,11);

    if(strcmp(head,flag)!=0)
        return -1;
    
    int dt;
    char* pos = FindNextFlag(buf,readlen,flag);
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
    int offset=av?19:20;
    memcpy(pts,buf+11,8);
    if (!av)
    {
        memcpy(keyframe,buf+11+8,1);
    }
    else
    {
        *keyframe = '\0';
    }
    
    *frame=buf+offset;

    return dt-offset;
}

int main()
{
    struct ps_muxer_func_t handler;
    handler.alloc = ps_alloc;
    handler.write = ps_write;
    handler.free = ps_free;

    FILE* fp = fopen("avps.ps", "wb");
    fwrite("IMKH00",6,1,fp);//ffmpeg 读到这个IMKH文件头时才会把音频的格式读成g711即pcm_alaw

    struct ps_muxer_t* ps = ps_muxer_create(&handler, fp);
    int v_id=ps_muxer_add_stream(ps,PSI_STREAM_H264,0,0);
    int a_id=ps_muxer_add_stream(ps,PSI_STREAM_AUDIO_G711A,0,0);

    FILE *vfp = fopen("VFrames.file", "rb"); // read binary
    FILE *afp = fopen("AFrames.file", "rb"); // read binary

    char *buf = (char *)malloc(500000);
    char* frame=0;
    unsigned long long pts=0;
    char keyFrame=0;

	while( 1 )
	{
        //从原始文件中读取一帧视频帧
        int vsize = ReadAVFile(vfp,&frame, buf, 500000,0, &pts, &keyFrame);
        if (vsize < 0)
        {
            break;
        }
        // print("vsize = %d pts = %lld",vsize,pts);
        //把一个vsize大小的视频帧放到264视频流里
        ps_muxer_input(ps,v_id,keyFrame,pts/pts_sigma,pts/pts_sigma,frame,vsize);

        //从原始音频文件中读取一帧音频帧
        int asize = ReadAVFile(afp,&frame, buf, 5000,1, &pts,&keyFrame);
        if (asize > 0)
        {
            // print("asize = %d pts = %lld",asize,pts);
            //把一个asize大小的音频帧放到音频流里
		    ps_muxer_input(ps,a_id,0,pts/pts_sigma,pts/pts_sigma,frame,asize);
        }else{
            // break;
        }
        
	}
    free(buf);
    fclose(vfp);
    fclose(afp);

    ps_muxer_destroy(ps);
	return 0;
}

extern int GetAACFrame(char* buf, FILE* fp);
int main1()
{
    struct ps_muxer_func_t handler;
    handler.alloc = ps_alloc;
    handler.write = ps_write;
    handler.free = ps_free;

    FILE* fp = fopen("avps.ps", "wb");
    struct ps_muxer_t* ps = ps_muxer_create(&handler, fp);
    int v_id=ps_muxer_add_stream(ps,PSI_STREAM_H264,0,0);
    // int a_id=ps_muxer_add_stream(ps,PSI_STREAM_AUDIO_G711A,0,0);
    int a_id=ps_muxer_add_stream(ps,PSI_STREAM_AAC,0,0);

    FILE *vfp = fopen("VFrames.file", "rb"); // read binary
    FILE *afp = fopen("audio.aac", "rb"); // read binary

    char *buf = (char *)malloc(500000);
    char* frame=0;
    unsigned long long pts=0;
    char keyFrame=0;
    int cnt = 0;
	while( 1 )
	{
        //从原始文件中读取一帧视频帧
        int vsize = ReadAVFile(vfp,&frame, buf, 500000,0, &pts, &keyFrame);
        if (vsize < 0)
        {
            break;
        }
        // // print("vsize = %d pts = %lld",vsize,pts);
        //把一个vsize大小的视频帧放到264视频流里
        ps_muxer_input(ps,v_id,keyFrame,pts/pts_sigma,pts/pts_sigma,frame,vsize);

        //从原始音频文件中读取一帧音频帧
        char aacbuf[8192]={0};
        int asize = GetAACFrame(aacbuf, afp);
        if (asize > 0)
        {
            // if(cnt++%30 == 0)
            // print("[%d]asize = %d pts = %lld",cnt,asize,pts);
            //把一个asize大小的音频帧放到音频流里
		    ps_muxer_input(ps,a_id,0,pts/pts_sigma,pts/pts_sigma,aacbuf,asize);
        }else{
            // break;
        }
        
	}
    free(buf);
    fclose(vfp);
    fclose(afp);

    ps_muxer_destroy(ps);
	return 0;
}




