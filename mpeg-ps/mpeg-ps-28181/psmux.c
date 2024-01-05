#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "psmux.h"
#include "utils.h"

//音频流stream_id分配初始化值
#define PSMUX_STREAM_ID_MPGA_MIN       0xc0
//音频流stream_id分配最大值
#define PSMUX_STREAM_ID_MPGA_MAX        0xcf
 
//视频流stream_id分配初始化值
#define PSMUX_STREAM_ID_MPGV_MIN       0xe0
//视频流stream_id分配最大值
#define PSMUX_STREAM_ID_MPGV_MAX        0xef

#define PSMUX_STREAM_ID_PRIVATE         0xbd

#define STREAM_NUM 24
#define AUDIO_START 0
#define VIDEO_START 8
#define PRIVATE_START 16

/*** 
 *@remark:   ps头的封装,里面的具体数据的填写已经占位，可以参考标准 
 *@param :   pData  [in] 填充ps头数据的地址 
 *           s64Src [in] 时间戳 
 *@return:   0 success, others failed 
*/  
int gb28181_make_ps_header(char *pData, unsigned long long s64Scr)  
{  
    unsigned long long lScrExt = (s64Scr) /*% 100*/;      //不用除以100也行？？...！
    s64Scr = s64Scr /*/ 100*/;  
    // 这里除以100是由于sdp协议返回的video的频率是90000，帧率是25帧/s，所以每次递增的量是3600,  
    // 所以实际你应该根据你自己编码里的时间戳来处理以保证时间戳的增量为3600即可，  
    //如果这里不对的话，就可能导致卡顿现象了  
    BITS_BUFFER_S   bitsBuffer;
    bitsBuffer.i_size = PS_HDR_LEN;
    bitsBuffer.i_data = 0;
    bitsBuffer.i_mask = 0x80; // 二进制：10000000 这里是为了后面对一个字节的每一位进行操作，避免大小端夸字节字序错乱  
    bitsBuffer.p_data = (unsigned char *)(pData);
    memset(bitsBuffer.p_data, 0, PS_HDR_LEN);
    bits_write(&bitsBuffer, 32, 0x000001BA);            /*start codes*/  
    bits_write(&bitsBuffer, 2,  1);                     /*marker bits '01b'*/  
    bits_write(&bitsBuffer, 3,  (s64Scr>>30)&0x07);     /*System clock [32..30]*/  
    bits_write(&bitsBuffer, 1,  1);                     /*marker bit*/  
    bits_write(&bitsBuffer, 15, (s64Scr>>15)&0x7FFF);   /*System clock [29..15]*/  
    bits_write(&bitsBuffer, 1,  1);                     /*marker bit*/  
    bits_write(&bitsBuffer, 15, s64Scr&0x7fff);         /*System clock [29..15]*/  
    bits_write(&bitsBuffer, 1,  1);                     /*marker bit*/  

    bits_write(&bitsBuffer, 9,  lScrExt&0x01ff);        /*System clock [14..0]*/  
    bits_write(&bitsBuffer, 1,  1);                     /*marker bit*/  

    bits_write(&bitsBuffer, 22, (255)&0x3fffff);        /*bit rate(n units of 50 bytes per second.)*/  
    bits_write(&bitsBuffer, 2,  3);                     /*marker bits '11'*/  

    bits_write(&bitsBuffer, 5,  0x1f);                  /*reserved(reserved for future use)*/  
    bits_write(&bitsBuffer, 3,  0);                     /*stuffing length*/  
    return 0;  
}

/*** 
 *@remark:   sys头的封装,里面的具体数据的填写已经占位，可以参考标准 
 *@param :   pData  [in] 填充ps头数据的地址 
 *@return:   0 success, others failed 
*/  
int gb28181_make_sys_header(char *pData, ps_stream_info* streaminfo, int audio_stream_count,int video_stream_count)  
{
    int streamcount=audio_stream_count+video_stream_count;
    BITS_BUFFER_S   bitsBuffer;  
    bitsBuffer.i_size = SYS_HDR_LEN;  
    bitsBuffer.i_data = 0;  
    bitsBuffer.i_mask = 0x80;  
    bitsBuffer.p_data = (unsigned char *)(pData);  
    memset(bitsBuffer.p_data, 0, SYS_HDR_LEN);  
    /*system header*/  
    bits_write( &bitsBuffer, 32, 0x000001BB);   /*start code*/  
    bits_write( &bitsBuffer, 16, SYS_HDR_LEN-12+streamcount*3);/*header_length 表示次字节后面的长度，后面的相关头也是次意思*/  
    bits_write( &bitsBuffer, 1,  1);            /*marker_bit*/  
    bits_write( &bitsBuffer, 22, 50000);        /*rate_bound*/  
    bits_write( &bitsBuffer, 1,  1);            /*marker_bit*/  
    bits_write( &bitsBuffer, 6,  1);            /*audio_bound*/  
    bits_write( &bitsBuffer, 1,  0);            /*fixed_flag */  
    bits_write( &bitsBuffer, 1,  0);            /*CSPS_flag */  
//    bits_write( &bitsBuffer, 1,  1);            /*CSPS_flag */  
    bits_write( &bitsBuffer, 1,  1);            /*system_audio_lock_flag*/  
    bits_write( &bitsBuffer, 1,  1);            /*system_video_lock_flag*/  
    bits_write( &bitsBuffer, 1,  1);            /*marker_bit*/  
    bits_write( &bitsBuffer, 5,  1);            /*video_bound*/  
    bits_write( &bitsBuffer, 1,  0);            /*dif from mpeg1*/  
    bits_write( &bitsBuffer, 7,  0x7F);         /*reserver*/  

    for( int i = 0;i<audio_stream_count;i++)
    {
        int index = AUDIO_START+i;
        bits_write( &bitsBuffer, 8,  streaminfo[index].streamId);         /*stream_id*/  
        print("audio[%d] %x",index, streaminfo[index].streamId);
        bits_write( &bitsBuffer, 2,  3);            /*marker_bit */  
        bits_write( &bitsBuffer, 1,  0);            /*PSTD_buffer_bound_scale*/  
        bits_write( &bitsBuffer, 13, 512);          /*PSTD_buffer_size_bound*/          
    }

    for( int i = 0;i<video_stream_count;i++)
    {
        int index = VIDEO_START+i;
        bits_write( &bitsBuffer, 8,  streaminfo[index].streamId);         /*stream_id*/  
        print("video[%d] %x",index, streaminfo[index].streamId);
        bits_write( &bitsBuffer, 2,  3);            /*marker_bit */  
        bits_write( &bitsBuffer, 1,  0);            /*PSTD_buffer_bound_scale*/  
        bits_write( &bitsBuffer, 13, 512);          /*PSTD_buffer_size_bound*/          
    }

    return 12 + streamcount * 3;  
}  

/*** 
 *@remark:   psm头的封装,里面的具体数据的填写已经占位，可以参考标准 
 *@param :   pData  [in] 填充ps头数据的地址 
 *@return:   0 success, others failed 
*/  
int gb28181_make_psm_header(char *pData, ps_stream_info* streaminfo, int audio_stream_count,int video_stream_count)  
{
    int streamcount=audio_stream_count+video_stream_count;
    BITS_BUFFER_S   bitsBuffer;  
    bitsBuffer.i_size = PSM_HDR_LEN;   
    bitsBuffer.i_data = 0;  
    bitsBuffer.i_mask = 0x80;  
    bitsBuffer.p_data = (unsigned char *)(pData);  
    memset(bitsBuffer.p_data, 0, PSM_HDR_LEN);  
    bits_write(&bitsBuffer, 24,0x000001);   /*start code*/  
    bits_write(&bitsBuffer, 8, 0xBC);       /*map stream id*/  

    bits_write(&bitsBuffer, 16,PSM_HDR_LEN - 10 + streamcount * 4);         /*program stream map length*/   
    //6
    bits_write(&bitsBuffer, 1, 1);          /*current next indicator */  
    bits_write(&bitsBuffer, 2, 3);          /*reserved*/  
    bits_write(&bitsBuffer, 5, 0);          /*program stream map version*/  
    bits_write(&bitsBuffer, 7, 0x7F);       /*reserved */  
    bits_write(&bitsBuffer, 1, 1);          /*marker bit */  
    bits_write(&bitsBuffer, 16,0);          /*programe stream info length*/  
    bits_write(&bitsBuffer, 16, streamcount * 4);         /*elementary stream map length  is*/  
    
    //8
    for( int i = 0;i<audio_stream_count;i++)
    {
        int index = AUDIO_START+i;
        bits_write(&bitsBuffer, 8, streaminfo[index].streamType);       /*stream_type*/  
        bits_write(&bitsBuffer, 8, streaminfo[index].streamId);       /*elementary_stream_id*/  
        print("audio[%d] %x type %x",index, streaminfo[index].streamId,streaminfo[index].streamType);
        bits_write(&bitsBuffer, 16, 0);         /*elementary_stream_info_length is*/  
    }

    for( int i = 0;i<video_stream_count;i++)
    {
        int index = VIDEO_START+i;
        bits_write(&bitsBuffer, 8, streaminfo[index].streamType);       /*stream_type*/  
        bits_write(&bitsBuffer, 8, streaminfo[index].streamId);       /*elementary_stream_id*/  
        print("video[%d] %x type %x",index, streaminfo[index].streamId,streaminfo[index].streamType);
        bits_write(&bitsBuffer, 16, 0);         /*elementary_stream_info_length is*/  
    }
    //4
    uint32_t crc = CRC_encode(pData+6, bitsBuffer.i_data-6);    
    bits_write(&bitsBuffer, 32, crc);       /*crc (24~31) bits*/  

    return 16 + streamcount * 4;  
}  

/*** 
 *@remark:   pes头的封装,里面的具体数据的填写已经占位，可以参考标准 
 *@param :   pData      [in] 填充ps头数据的地址 
 *           stream_id  [in] 码流类型 
 *           paylaod_len[in] 负载长度 
 *           pts        [in] 时间戳 
 *           dts        [in] 
 *@return:   0 success, others failed 
*/  
int gb28181_make_pes_header(char *pData, int stream_id, int payload_len, unsigned long long pts, unsigned long long dts)  
{  
    BITS_BUFFER_S   bitsBuffer;  
    bitsBuffer.i_size = PES_HDR_LEN;  
    bitsBuffer.i_data = 0;  
    bitsBuffer.i_mask = 0x80;  
    bitsBuffer.p_data = (unsigned char *)(pData);  
    memset(bitsBuffer.p_data, 0, PES_HDR_LEN);  
    /*system header*/  
    bits_write( &bitsBuffer, 24,0x000001);  /*start code*/  
    bits_write( &bitsBuffer, 8, (stream_id));   /*streamID*/  
    bits_write( &bitsBuffer, 16,(payload_len)+13);  /*packet_len*/ //指出pes分组中数据长度和该字节后的长度和  
 
    bits_write( &bitsBuffer, 2, 2 );        /*'10'*/  
    bits_write( &bitsBuffer, 2, 0 );        /*scrambling_control*/  
    bits_write( &bitsBuffer, 1, 0 );        /*priority*/  
    bits_write( &bitsBuffer, 1, 0 );        /*data_alignment_indicator*/  
    bits_write( &bitsBuffer, 1, 0 );        /*copyright*/  
    bits_write( &bitsBuffer, 1, 0 );        /*original_or_copy*/  

    bits_write( &bitsBuffer, 1, 1 );        /*PTS_flag*/  
    bits_write( &bitsBuffer, 1, 1 );        /*DTS_flag*/  
    bits_write( &bitsBuffer, 1, 0 );        /*ESCR_flag*/  
    bits_write( &bitsBuffer, 1, 0 );        /*ES_rate_flag*/  
    bits_write( &bitsBuffer, 1, 0 );        /*DSM_trick_mode_flag*/  
    bits_write( &bitsBuffer, 1, 0 );        /*additional_copy_info_flag*/  
    bits_write( &bitsBuffer, 1, 0 );        /*PES_CRC_flag*/  
    bits_write( &bitsBuffer, 1, 0 );        /*PES_extension_flag*/  

    bits_write( &bitsBuffer, 8, 10);        /*header_data_length*/   
    // 指出包含在 PES 分组标题中的可选字段和任何填充字节所占用的总字节数。该字段之前  
    //的字节指出了有无可选字段。  
    /*PTS,DTS*/   
	bits_write( &bitsBuffer, 4, 3 );                    /*'0011'*/  
    bits_write( &bitsBuffer, 3, ((pts)>>30)&0x07 );     /*PTS[32..30]*/  
    bits_write( &bitsBuffer, 1, 1 );  

    bits_write( &bitsBuffer, 15,((pts)>>15)&0x7FFF);    /*PTS[29..15]*/  
    bits_write( &bitsBuffer, 1, 1 );  

    bits_write( &bitsBuffer, 15,(pts)&0x7FFF);          /*PTS[14..0]*/  
    bits_write( &bitsBuffer, 1, 1 );  

    bits_write( &bitsBuffer, 4, 1 );                    /*'0001'*/  
    bits_write( &bitsBuffer, 3, ((dts)>>30)&0x07 );     /*DTS[32..30]*/  
    bits_write( &bitsBuffer, 1, 1 );  

    bits_write( &bitsBuffer, 15,((dts)>>15)&0x7FFF);    /*DTS[29..15]*/  
    bits_write( &bitsBuffer, 1, 1 );  

    bits_write( &bitsBuffer, 15,(dts)&0x7FFF);          /*DTS[14..0]*/  
    bits_write( &bitsBuffer, 1, 1 );  

    return 0;  
}  

typedef struct 
{
    FILE* fp;
    ps_stream_info streaminfo[STREAM_NUM];
    int audio_stream_count;
    int video_stream_count;
}psmux_tag_t;

int gb28181_make_psheader(void* handle, uint64_t timestamp, int Iframe)  
{  
    psmux_tag_t *inst = (psmux_tag_t*)handle;

    char szTempPacketHead[256] = {0};  
    int  nSizePos = 0;  
    // 1 package for ps header   
    gb28181_make_ps_header(szTempPacketHead + nSizePos, timestamp);  
    nSizePos += PS_HDR_LEN;   
    //2 system header   
//    if( pPacker->IFrame == 1 )  
    if(Iframe)
    {  
        // 如果是I帧的话，则添加系统头  
        int sys_head_len = gb28181_make_sys_header(szTempPacketHead + nSizePos, inst->streaminfo, inst->audio_stream_count, inst->video_stream_count);  
        nSizePos += sys_head_len;  
        //这个地方我是不管是I帧还是p帧都加上了map的，貌似只是I帧加也没有问题  
        int psm_head_len = gb28181_make_psm_header(szTempPacketHead + nSizePos, inst->streaminfo, inst->audio_stream_count, inst->video_stream_count);  
        nSizePos += psm_head_len;
    }  
  
    ////////////todo output szTempPacketHead
  	//inst->fsps.write(szTempPacketHead, nSizePos);
    fwrite(szTempPacketHead, 1, nSizePos, inst->fp);
    return 0;  
}

//////////////////////////////////////////////////////////////
void *psmux_alloc(const char* filename)
{
    psmux_tag_t *inst =  (psmux_tag_t*)malloc(sizeof(psmux_tag_t));

    inst->audio_stream_count = 0;
    inst->video_stream_count = 0;
    inst->fp = fopen(filename, "wb");

    return inst;
}

void psmux_writeraw(void* handle,const char* data,int len)
{
    psmux_tag_t *inst = (psmux_tag_t*)handle;
    if (!inst)
        return;
    
    fwrite(data,len,1,inst->fp);
}

int psmux_addvideostream(void *handle, int payload)
{
    psmux_tag_t *inst = (psmux_tag_t*)handle;

    int index = VIDEO_START + inst->video_stream_count;
    inst->streaminfo[index].streamId = PSMUX_STREAM_ID_MPGV_MIN+inst->video_stream_count;
    inst->streaminfo[index].streamType = payload;//PSMUX_ST_VIDEO_H264;//h264

    inst->video_stream_count++;

    return inst->streaminfo[index].streamId;    
}

int psmux_addaudiostream(void *handle, int payload)
{
    psmux_tag_t *inst = (psmux_tag_t*)handle;

    int index = AUDIO_START + inst->audio_stream_count;
    inst->streaminfo[index].streamId = PSMUX_STREAM_ID_MPGA_MIN+inst->audio_stream_count;
    inst->streaminfo[index].streamType = payload;//PSMUX_ST_PS_AUDIO_G711A;//g711

    inst->audio_stream_count++;

    return inst->streaminfo[index].streamId;    
}

int psmux_addprivatestream(void *handle, int payload)
{
    psmux_tag_t *inst = (psmux_tag_t*)handle;

    int index = AUDIO_START + inst->audio_stream_count;
    inst->streaminfo[index].streamId = PSMUX_STREAM_ID_PRIVATE+inst->audio_stream_count;
    inst->streaminfo[index].streamType = payload;//PSMUX_ST_PS_AUDIO_G711A;//g711

    inst->audio_stream_count++;

    return inst->streaminfo[index].streamId;    
}


int psmux_writeframe(void* handle, int stream_id, const char *pData, int nFrameLen, uint64_t timestamp, int Iframe)  
{
    psmux_tag_t *inst = (psmux_tag_t*)handle;

    char szTempPacketHead[256] = {0};  
    int  nSizePos = PES_HDR_LEN;  

    gb28181_make_psheader(handle, timestamp, Iframe);

    // 这里向后移动是为了方便拷贝pes头  
    //这里是为了减少后面音视频裸数据的大量拷贝浪费空间，所以这里就向后移动，在实际处理的时候，要注意地址是否越界以及覆盖等问题  
    while(nFrameLen > 0)  
    {  
        //每次帧的长度不要超过short类型，过了就得分片进循环行发送  
        int nSize = (nFrameLen > PS_PES_PAYLOAD_SIZE) ? PS_PES_PAYLOAD_SIZE : nFrameLen;  
        // 添加pes头  
        gb28181_make_pes_header(szTempPacketHead, stream_id, nSize, timestamp, timestamp);
        fwrite(szTempPacketHead, 1, nSizePos, inst->fp);

        fwrite(pData, 1, nSize, inst->fp);

        //分片后每次发送的数据移动指针操作  
        nFrameLen -= nSize;  
        //这里也只移动nSize,因为在while向后移动的pes头长度，正好重新填充pes头数据  
        pData     += nSize;  
    }  
    return 0;  
}

int psmux_free(void *handle)
{
    psmux_tag_t *inst = (psmux_tag_t*)handle;

    fclose(inst->fp);
    free(inst);

    return 0;
}