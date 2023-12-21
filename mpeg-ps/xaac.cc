#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
 
/**
https://wiki.multimedia.cx/index.php/ADTS
 
AAAAAAAA AAAABCCD EEFFFFGH HHIJKLMM MMMMMMMM MMMOOOOO OOOOOOPP (QQQQQQQQ
QQQQQQQQ)
 
Header consists of 7 or 9 bytes (without or with CRC).
 
Letter	Length (bits)	Description
A	12	Syncword, all bits must be set to 1.
B	1	MPEG Version, set to 0 for MPEG-4 and 1 for MPEG-2.
C	2	Layer, always set to 0.
D	1	Protection absence, set to 1 if there is no CRC and 0 if there
        is CRC. 
E	2	Profile, the MPEG-4 Audio Object Type minus 1.
F	4	MPEG-4 Sampling Frequency Index (15 is forbidden).
G	1	Private bit, guaranteed never to be used by MPEG, set to 0 when
        encoding, ignore when decoding. 
H	3	MPEG-4 Channel Configuration (in
        the case of 0, the channel configuration is sent via an inband PCE (Program
        Config Element)). 
I	1	Originality, set to 1 to signal originality of
        the audio and 0 otherwise. 
J	1	Home, set to 1 to signal home usage of
        the audio and 0 otherwise. 
K	1	Copyright ID bit, the next bit of a
        centrally registered copyright identifier. This is transmitted by sliding over
        the bit-string in LSB-first order and putting the current bit value in this
        field and wrapping to start if reached end (circular buffer).
L	1	Copyright ID start, signals that this frame's Copyright ID bit
        is the first one by setting 1 and 0 otherwise.
M	13	Frame length, length of the ADTS frame including headers and CRC
        check. 
O	11	Buffer fullness, states the bit-reservoir per frame.
        max_bit_reservoir = minimum_decoder_input_size - mean_bits_per_RDB; // for CBR
        // bit reservoir state/available bits (≥0 and <max_bit_reservoir); for the i-th
        frame. bit_reservoir_state[i] = (int)(bit_reservoir_state[i - 1] +
        mean_framelength - framelength[i]);
        // NCC is the number of channels.
        adts_buffer_fullness = bit_reservoir_state[i] / (NCC * 32);
        However, a special value of 0x7FF denotes a variable bitrate, for which buffer
        fullness isn't applicable.
 
P	2	Number of AAC frames (RDBs (Raw Data Blocks)) in ADTS frame
        minus 1. For maximum compatibility always use one AAC frame per ADTS frame. 
Q   16	CRC check (as of ISO/IEC 11172-3, subclause 2.4.3.1), if Protection
        absent is 0.
 */
 
struct AdtsHeader {
    // 12 bit 同步字 '1111 1111 1111'，说明一个ADTS帧的开始
    unsigned short syncword : 12;
 
    // 1 bit MPEG 标示符， 0 for MPEG-4，1 for MPEG-2
    unsigned short id : 1;
    unsigned short layer : 2;              // 2 bit 总是'00'
    unsigned short protection_absent : 1; // 1 bit 1表示没有crc，0表示有crc
 
    unsigned short profile : 2; // 1 bit 表示使用哪个级别的AAC
    unsigned short sampling_frequency_index : 4; // 4 bit 表示使用的采样频率
    unsigned short private_bit : 1;              // 1 bit
    unsigned short channel_configuration : 3;                  // 3 bit 表示声道数
    unsigned short originality : 1;              // 1 bit
    unsigned short home : 1;                     // 1 bit
 
    /*下面的为改变的参数即每一帧都不同*/
    unsigned short copyright_id_bit : 1;   // 1 bit
    unsigned short copyright_id_start : 1; // 1 bit
    // 13 bit 一个ADTS帧的长度包括ADTS头和AAC原始流
    unsigned short frame_length : 13;
    unsigned short buffer_fullness : 11; // 11 bit 0x7FF 说明是码率可变的码流
 
    /* number_of_raw_data_blocks_in_frame
     * 表示ADTS帧中有number_of_raw_data_blocks_in_frame + 1个AAC原始帧
     * 所以说number_of_raw_data_blocks_in_frame == 0
     * 表示说ADTS帧中有一个AAC数据块并不是说没有。(一个AAC原始帧包含一段时间内1024个采样及相关数据)
     */
    unsigned short num_raw_data_blocks : 2;
    unsigned short crc;
};
 
#define AAC_MAX_FRAME_LENGTH 8192
 
static int AUDIO_SAMPLING_RATES[] = {
    96000, // 0
    88200, // 1
    64000, // 2
    48000, // 3
    44100, // 4
    32000, // 5
    24000, // 6
    22050, // 7
    16000, // 8
    12000, // 9
    11025, // 10
    8000,  // 11
    7350,  // 12
    -1,    // 13
    -1,    // 14
    -1,    // 15
};
 
FILE *bitstream = NULL; //!< the bit stream file
 
struct AdtsFrame {
    AdtsHeader header;
    unsigned char *body;
    unsigned short body_length;
};
 
bool MatchStartCode(unsigned char *pdata) {
    return pdata[0] == 0xFF && (pdata[1] & 0xF0) == 0xF0;
}
 
 
int GetNextAacFrame(AdtsFrame *frame) {
    ::bzero(&frame->header, sizeof(frame->header));
    frame->body_length = 0;
    frame->body = nullptr;
 
    unsigned char *data_buf = new unsigned char[AAC_MAX_FRAME_LENGTH];
 
    size_t read_len = fread(data_buf, 1, AAC_MAX_FRAME_LENGTH, bitstream);
    if (read_len < 7) {
        delete[] data_buf;
        return 0;
    }
 
    // data cursor
    unsigned char *pdata = data_buf;
    // pdata must be smaller than this
    unsigned char *pdata_end = data_buf + read_len;
 
    if (!MatchStartCode(pdata)) {
        delete[] data_buf;
        return 0;
    } else {
        pdata += 1;
    }
 
    frame->header.syncword = 0xFFF;
    // AAAABCCD
    frame->header.id = (*pdata & 0x08) >> 3; // B
    frame->header.layer = (*pdata & 0x06) >> 1;        // C
    frame->header.protection_absent = *pdata & 0x01;  // D
    ++pdata;
 
    // EEFFFFGH
    frame->header.profile = (*pdata & 0xC0) >> 6;                  // E
    frame->header.sampling_frequency_index = (*pdata & 0x3C) >> 2; // F
    frame->header.private_bit = (*pdata & 0x02) >> 1;              // G
    // EEFFFFGH HHIJKLMM
    frame->header.channel_configuration =
        (pdata[0] & 0x01) << 2 | (pdata[1] & 0xC0) >> 6; // H
    ++pdata;
    // HHIJKLMM
    frame->header.originality = (pdata[0] & 0x20) >> 5;        // I
    frame->header.home = (pdata[0] & 0x10) >> 4;               // J
    frame->header.copyright_id_bit = (pdata[0] & 0x08) >> 3;   // K
    frame->header.copyright_id_start = (pdata[0] & 0x04) >> 2; // L
 
    // HHIJKLMM MMMMMMMM MMMOOOOO
    frame->header.frame_length =
        (pdata[0] & 0x03) << 11 | pdata[1] << 3 | (pdata[2] & 0xE0) >> 5; // M
    pdata += 2;
    // MMMOOOOO OOOOOOPP
    frame->header.buffer_fullness =
        (pdata[0] & 0x1F) << 6 | (pdata[1] & 0xFC) >> 2; // O
    ++pdata;
    // OOOOOOPP
    frame->header.num_raw_data_blocks = pdata[0] & 0x03; // P
    ++pdata;
    if (frame->header.protection_absent == 0) {
        frame->header.crc = pdata[0]<<8 & pdata[1]; // Q
        pdata += 2;
    }
    frame->body_length = frame->header.frame_length - (frame->header.protection_absent==0? 9: 7);
    frame->body = new unsigned char[frame->body_length];
    memcpy(frame->body, pdata, frame->body_length);
    pdata += frame->body_length;
    // std::cout << "syncword " << frame->header.syncword << std::endl;
    // std::cout << "mpeg_version " << frame->header.id << std::endl;
    // std::cout << "layer " << frame->header.layer << std::endl;
    // std::cout << "protection_absence " << frame->header.protection_absent
    //           << std::endl;
    // std::cout << "profile " << frame->header.profile << std::endl;
    // std::cout << "sampling_frequency_index "
    //           << frame->header.sampling_frequency_index << std::endl;
    // std::cout << "private_bit " << frame->header.private_bit << std::endl;
    // std::cout << "channel " << frame->header.channel_configuration << std::endl;
    // std::cout << "originality " << frame->header.originality << std::endl;
    // std::cout << "home " << frame->header.home << std::endl;
    // std::cout << "copyright_id_bit " << frame->header.copyright_id_bit
    //           << std::endl;
    // std::cout << "copyright_id_start " << frame->header.copyright_id_start
    //           << std::endl;
    // std::cout << "frame_length " << frame->header.frame_length << std::endl;
    // std::cout << "buffer_fullness " << frame->header.buffer_fullness
    //           << std::endl;
    // std::cout << "frames " << frame->header.num_raw_data_blocks << std::endl;
    // Here, we have found another start code (and read length of startcode
    // bytes more than we should have.  Hence, go back in the file
    ssize_t rewind = pdata - pdata_end;
    if (0 != fseek(bitstream, rewind, SEEK_CUR)) {
        delete[] data_buf;
        printf("GetAnnexbNALU: Cannot fseek in the bit stream file");
        return -1;
    }
    delete[] data_buf;
    return frame->header.frame_length;
}
int simplest_aac_parser(char *url) {
    // FILE *myout=fopen("output_log.txt","wb+");
    FILE *myout = stdout;
    bitstream = fopen(url, "rb+");
    if (bitstream == NULL) {
        printf("Open file error\n");
        return 0;
    }
    AdtsFrame *frame = new AdtsFrame;
    int data_offset = 0;
    int nal_num = 0;
    printf("-----+------------------------------------------ ADTS Table ---------------------------------------------------+\n");
    printf(" NUM |    POS     | Ver | Layer | Abst | Prof | Frq | Pri | Chn | Org | Home | Idb | Ids | Length | Full | Num |\n");
    printf("-----+------------+-----+-------+------+------+-----+-----+-----+-----+------+-----+-----+--------+------+-----+\n");
    while (!feof(bitstream)) {
        int data_lenth;
        data_lenth = GetNextAacFrame(frame);
        if (data_lenth == 0)
            break;
        auto* header = &frame->header;
        fprintf(myout, "%5d| 0x%08X | %4d| %6d| %5d| %5d| %4d| %4d| %4d| %4d| %5d| %4d| %4d| %7d| %5d| %4d|\n", nal_num, data_offset, header->id, header->layer, header->protection_absent,
        header->profile, header->sampling_frequency_index, header->private_bit, header->channel_configuration,
        header->originality, header->home, header->copyright_id_bit, header->copyright_id_start, header->frame_length,
        header->buffer_fullness, header->num_raw_data_blocks);
        data_offset = data_offset + data_lenth;
        nal_num++;
        if (frame->body) 
            delete []frame->body;
    }
    // Free
    if (frame) {
        if (frame->body) 
            delete []frame->body;
        delete frame;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    char *url = argv[1];
    std::cout << "url: " << url << std::endl;
    simplest_aac_parser(url);

    return 0;
}