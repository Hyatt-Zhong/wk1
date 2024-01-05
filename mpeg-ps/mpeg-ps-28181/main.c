#include <stdio.h>  
#include <unistd.h>
#include "psmux.h"

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
	void* pshandle = psmux_alloc("avps.ps");
    psmux_writeraw(pshandle,"IMKH00",6);
	int video_id = psmux_addvideostream(pshandle, PSMUX_ST_VIDEO_H264);
	int audio_id = psmux_addaudiostream(pshandle, PSMUX_ST_PS_AUDIO_G711A);

    FILE *vfp = fopen("VFrames.file", "rb"); // read binary
    FILE *afp = fopen("AFrames.file", "rb"); // read binary

    char *buf = (char *)malloc(500000);
    char* frame=0;
    unsigned long long pts=0;
    char keyFrame=0;

    int afrm_cnt=0;
	while( 1 )
	{
        int vsize = ReadAVFile(vfp,&frame, buf, 500000,0, &pts, &keyFrame);
        if (vsize < 0)
        {
            break;
        }

		psmux_writeframe(pshandle, video_id, frame, vsize, pts/10, keyFrame);        

        int asize = ReadAVFile(afp,&frame, buf, 5000,1, &pts,&keyFrame);
        if (asize > 0)
        {
		    psmux_writeframe(pshandle, audio_id, frame, asize, pts/10, 0);
        }		
        else
        {
        //     break;
        }
        
	}
    free(buf);
    fclose(vfp);
    fclose(afp);

	psmux_free(pshandle);
	return 0;
}
