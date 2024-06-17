#include "mpeg4-avc.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

/*
ISO/IEC 14496-15:2010(E) 5.2.4.1.1 Syntax (p16)

aligned(8) class AVCDecoderConfigurationRecord {
	unsigned int(8) configurationVersion = 1;
	unsigned int(8) AVCProfileIndication;
	unsigned int(8) profile_compatibility;
	unsigned int(8) AVCLevelIndication;
	bit(6) reserved = '111111'b;
	unsigned int(2) lengthSizeMinusOne;
	bit(3) reserved = '111'b;

	unsigned int(5) numOfSequenceParameterSets;
	for (i=0; i< numOfSequenceParameterSets; i++) {
		unsigned int(16) sequenceParameterSetLength ;
		bit(8*sequenceParameterSetLength) sequenceParameterSetNALUnit;
	}

	unsigned int(8) numOfPictureParameterSets;
	for (i=0; i< numOfPictureParameterSets; i++) {
		unsigned int(16) pictureParameterSetLength;
		bit(8*pictureParameterSetLength) pictureParameterSetNALUnit;
	}

	if( profile_idc == 100 || profile_idc == 110 || 
		profile_idc == 122 || profile_idc == 144 )
	{
		bit(6) reserved = '111111'b;
		unsigned int(2) chroma_format;
		bit(5) reserved = '11111'b;
		unsigned int(3) bit_depth_luma_minus8;
		bit(5) reserved = '11111'b;
		unsigned int(3) bit_depth_chroma_minus8;
		unsigned int(8) numOfSequenceParameterSetExt;
		for (i=0; i< numOfSequenceParameterSetExt; i++) {
			unsigned int(16) sequenceParameterSetExtLength;
			bit(8*sequenceParameterSetExtLength) sequenceParameterSetExtNALUnit;
		}
	}
}
*/
static int _mpeg4_avc_decoder_configuration_record_load(const uint8_t* data, size_t bytes, struct mpeg4_avc_t* avc)
{
    uint8_t i;
	uint32_t j;
	uint16_t len;
    uint8_t *p, *end;
	
	if (bytes < 7) return -1;
	assert(1 == data[0]);
//	avc->version = data[0];
	avc->profile = data[1];
	avc->compatibility = data[2];
	avc->level = data[3];
	avc->nalu = (data[4] & 0x03) + 1;
	avc->nb_sps = data[5] & 0x1F;
	if (avc->nb_sps > sizeof(avc->sps) / sizeof(avc->sps[0]))
	{
		assert(0);
		return -1; // sps <= 32
	}

	j = 6;
    p = avc->data;
    end = avc->data + sizeof(avc->data);
	for (i = 0; i < avc->nb_sps && j + 2 < bytes; ++i)
	{
		len = (data[j] << 8) | data[j + 1];
		if (j + 2 + len >= bytes || p + len > end)
		{
			assert(0);
			return -1;
		}

		memcpy(p, data + j + 2, len);
        avc->sps[i].data = p;
		avc->sps[i].bytes = len;
		j += len + 2;
        p += len;
	}

	if (j >= bytes || (unsigned int)data[j] > sizeof(avc->pps) / sizeof(avc->pps[0]))
	{
		assert(0);
		return -1;
	}

	avc->nb_pps = data[j++]; 
	for (i = 0; i < avc->nb_pps && j + 2 < bytes; i++)
	{
		len = (data[j] << 8) | data[j + 1];
        if (j + 2 + len > bytes || p + len > end)
        {
            assert(0);
            return -1;
        }

		memcpy(p, data + j + 2, len);
        avc->pps[i].data = p;
		avc->pps[i].bytes = len;
		j += len + 2;
        p += len;
	}

	avc->off = (int)(p - avc->data);
	return j;
}

int mpeg4_avc_decoder_configuration_record_save(const struct mpeg4_avc_t* avc, uint8_t* data, size_t bytes)
{
	uint8_t i;
	uint8_t *p = data;

	assert(0 < avc->nalu && avc->nalu <= 4);
	if (bytes < 7 || avc->nb_sps > 32) return -1;
	bytes -= 7;

	// AVCDecoderConfigurationRecord
	// ISO/IEC 14496-15:2010
	// 5.2.4.1.1 Syntax
	p[0] = 1; // configurationVersion
	p[1] = avc->profile; // AVCProfileIndication
	p[2] = avc->compatibility; // profile_compatibility
	p[3] = avc->level; // AVCLevelIndication
	p[4] = 0xFC | (avc->nalu - 1); // lengthSizeMinusOne: 3
	p += 5;

	// sps
	*p++ = 0xE0 | avc->nb_sps;
	for (i = 0; i < avc->nb_sps && bytes >= (size_t)avc->sps[i].bytes + 2; i++)
	{
		*p++ = (avc->sps[i].bytes >> 8) & 0xFF;
		*p++ = avc->sps[i].bytes & 0xFF;
		memcpy(p, avc->sps[i].data, avc->sps[i].bytes);

		p += avc->sps[i].bytes;
		bytes -= avc->sps[i].bytes + 2;
	}
	if (i < avc->nb_sps) return -1; // check length

	// pps
	*p++ = avc->nb_pps;
	for (i = 0; i < avc->nb_pps && bytes >= (size_t)avc->pps[i].bytes + 2; i++)
	{
		*p++ = (avc->pps[i].bytes >> 8) & 0xFF;
		*p++ = avc->pps[i].bytes & 0xFF;
		memcpy(p, avc->pps[i].data, avc->pps[i].bytes);

		p += avc->pps[i].bytes;
		bytes -= avc->pps[i].bytes + 2;
	}
	if (i < avc->nb_pps) return -1; // check length

	if (bytes >= 4)
	{
		if (avc->profile == 100 || avc->profile == 110 ||
			avc->profile == 122 || avc->profile == 244 || avc->profile == 44 ||
			avc->profile == 83 || avc->profile == 86 || avc->profile == 118 ||
			avc->profile == 128 || avc->profile == 138 || avc->profile == 139 ||
			avc->profile == 134)
		{
			*p++ = 0xFC | avc->chroma_format_idc;
			*p++ = 0xF8 | avc->bit_depth_luma_minus8;
			*p++ = 0xF8 | avc->bit_depth_chroma_minus8;
			*p++ = 0; // numOfSequenceParameterSetExt
		}
	}

	return (int)(p - data);
}

#define H264_STARTCODE(p) (p[0]==0 && p[1]==0 && (p[2]==1 || (p[2]==0 && p[3]==1)))

int mpeg4_avc_from_nalu(const uint8_t* data, size_t bytes, struct mpeg4_avc_t* avc)
{
	int r;
	r = h264_annexbtomp4(avc, data, bytes, NULL, 0, NULL, NULL);
	return avc->nb_sps > 0 && avc->nb_pps > 0 ? bytes : r;
}

int mpeg4_avc_to_nalu(const struct mpeg4_avc_t* avc, uint8_t* data, size_t bytes)
{
	uint8_t i;
	size_t k = 0;
	uint8_t* h264 = data;

	// sps
	for (i = 0; i < avc->nb_sps && bytes >= k + avc->sps[i].bytes + 4; i++)
	{
		if (avc->sps[i].bytes < 4 || !H264_STARTCODE(avc->sps[i].data))
		{
			h264[k++] = 0;
			h264[k++] = 0;
			h264[k++] = 0;
			h264[k++] = 1;
		}
		memcpy(h264 + k, avc->sps[i].data, avc->sps[i].bytes);

		k += avc->sps[i].bytes;
	}
	if (i < avc->nb_sps) return -1; // check length

	// pps
	for (i = 0; i < avc->nb_pps && bytes >= k + avc->pps[i].bytes + 2; i++)
	{
		if (avc->pps[i].bytes < 4 || !H264_STARTCODE(avc->pps[i].data))
		{
			h264[k++] = 0;
			h264[k++] = 0;
			h264[k++] = 0;
			h264[k++] = 1;
		}
		memcpy(h264 + k, avc->pps[i].data, avc->pps[i].bytes);

		k += avc->pps[i].bytes;
	}
	if (i < avc->nb_pps) return -1; // check length

	assert(k < 0x7FFF);
	return (int)k;
}

int mpeg4_avc_codecs(const struct mpeg4_avc_t* avc, char* codecs, size_t bytes)
{
	// https://tools.ietf.org/html/rfc6381#section-3.3
	// https://developer.mozilla.org/en-US/docs/Web/Media/Formats/codecs_parameter
    return snprintf(codecs, bytes, "avc1.%02x%02x%02x", avc->profile, avc->compatibility, avc->level);
}

int mpeg4_avc_decoder_configuration_record_load(const uint8_t* data, size_t bytes, struct mpeg4_avc_t* avc)
{
	int r;
	r = _mpeg4_avc_decoder_configuration_record_load(data, bytes, avc);
	if (r > 0 && avc->nb_sps > 0 && avc->nb_pps > 0)
		return r;

	// try annexb
	memset(avc, 0, sizeof(*avc));
	return mpeg4_avc_from_nalu(data, bytes, avc);
}

#if defined(_DEBUG) || defined(DEBUG)
void mpeg4_annexbtomp4_test(void);
void mpeg4_avc_test(void)
{
	const unsigned char src[] = {
		0x01,0x42,0xe0,0x1e,0xff,0xe1,0x00,0x21,0x67,0x42,0xe0,0x1e,0xab,0x40,0xf0,0x28,
		0xd0,0x80,0x00,0x00,0x00,0x80,0x00,0x00,0x19,0x70,0x20,0x00,0x78,0x00,0x00,0x0f,
		0x00,0x16,0xb1,0xb0,0x3c,0x50,0xaa,0x80,0x80,0x01,0x00,0x04,0x28,0xce,0x3c,0x80
	};
	const unsigned char nalu[] = {
		0x00,0x00,0x00,0x01,0x67,0x42,0xe0,0x1e,0xab,0x40,0xf0,0x28,0xd0,0x80,0x00,0x00,
		0x00,0x80,0x00,0x00,0x19,0x70,0x20,0x00,0x78,0x00,0x00,0x0f,0x00,0x16,0xb1,0xb0,
		0x3c,0x50,0xaa,0x80,0x80,0x00,0x00,0x00,0x01,0x28,0xce,0x3c,0x80
	};
	unsigned char data[sizeof(src)];

	struct mpeg4_avc_t avc;
	assert(sizeof(src) == mpeg4_avc_decoder_configuration_record_load(src, sizeof(src), &avc));
	assert(0x42 == avc.profile && 0xe0 == avc.compatibility && 0x1e == avc.level);
	assert(4 == avc.nalu && 1 == avc.nb_sps && 1 == avc.nb_pps);
	assert(sizeof(src) == mpeg4_avc_decoder_configuration_record_save(&avc, data, sizeof(data)));
	assert(0 == memcmp(src, data, sizeof(src)));
    mpeg4_avc_codecs(&avc, (char*)data, sizeof(data));
    assert(0 == memcmp("avc1.42e01e", data, 11));

	assert(sizeof(nalu) == mpeg4_avc_to_nalu(&avc, data, sizeof(data)));
	assert(0 == memcmp(nalu, data, sizeof(nalu)));

	mpeg4_annexbtomp4_test();
}
#endif

void skip_bits(bit_buffer * bb, size_t nbits) {
    bb->current = bb->current + (nbits + bb->read_bits) / 8;
    bb->read_bits = (uint8)((bb->read_bits + nbits) % 8);
}

sint8 get_bit(bit_buffer * bb) {
    sint8 ret;

    if (bb->current - bb->start > (ptrdiff_t)(bb->size - 1)) {
        return -1;
    }

    ret = (*(bb->current) >> (7 - bb->read_bits)) & 0x1;
    if (bb->read_bits == 7) {
        bb->read_bits = 0;
        bb->current++;
    }
    else {
        bb->read_bits++;
    }
    return ret;
}

static uint8 get_bits(bit_buffer * bb, size_t nbits, uint32 * ret) {
    uint32 i;
    sint8 bit;

	if (nbits > sizeof(uint32) * 8) {
        nbits = sizeof(uint32) * 8;
	}

    *ret = 0;
    for (i = 0; i < nbits; i++) {
        bit = get_bit(bb);
        if (bit == -1) {
            return 0;
        }

        *ret = (*ret << 1) + bit;
    }
    return 1;
}

uint32 exp_golomb_ue(bit_buffer * bb) {
    sint8 bit;
    uint8 significant_bits;
    uint32 bits;

    significant_bits = 0;

    do {
        bit = get_bit(bb);
        if (bit == -1) {
            return 0;
        }
        if (bit == 0) {
            significant_bits++;
        }
    } while (bit == 0);

    if (!get_bits(bb, significant_bits, &bits))
        return 0;

    return (1 << significant_bits) + bits - 1;
}

sint32 exp_golomb_se(bit_buffer * bb) {
    sint32 ret;
    ret = exp_golomb_ue(bb);
    if ((ret & 0x1) == 0) {
        return -(ret >> 1);
    }

    return (ret + 1) >> 1;
}

static void parse_scaling_list(uint32 size, bit_buffer * bb) {
    uint32 last_scale, next_scale, i;
    sint32 delta_scale;
    last_scale = 8;
    next_scale = 8;
    for (i = 0; i < size; i++) {
        if (next_scale != 0) {
            delta_scale = exp_golomb_se(bb);
            next_scale = (last_scale + delta_scale + 256) % 256;
        }
        if (next_scale != 0) {
            last_scale = next_scale;
        }
    }
}

void parse_sps(byte * sps, size_t sps_size, uint32 * width, uint32 * height) {
    bit_buffer bb;
    uint32 profile, pic_order_cnt_type, width_in_mbs, height_in_map_units;
    uint32 i, size, left, right, top, bottom;
    sint8 frame_mbs_only_flag;
    sint8 bit;

    bb.start = sps;
    bb.size = sps_size;
    bb.current = sps;
    bb.read_bits = 0;

    /* skip first byte, since we already know we're parsing a SPS */
    skip_bits(&bb, 8);
    /* get profile */
    if (!get_bits(&bb, 8, &profile)) {
        return;
    }

    /* skip 4 bits + 4 zeroed bits + 8 bits = 16 bits = 2 bytes */
    skip_bits(&bb, 16);

    /* read sps id, first exp-golomb encoded value */
    exp_golomb_ue(&bb);

    if (profile == 100 || profile == 110 || profile == 122 || profile == 144) {
        /* chroma format idx */
        if (exp_golomb_ue(&bb) == 3) {
            skip_bits(&bb, 1);
        }
        /* bit depth luma minus8 */
        exp_golomb_ue(&bb);
        /* bit depth chroma minus8 */
        exp_golomb_ue(&bb);
        /* Qpprime Y Zero Transform Bypass flag */
        skip_bits(&bb, 1);
        /* Seq Scaling Matrix Present Flag */
        bit = get_bit(&bb);
        if (bit == -1) {
            return;
        }
        if (bit) {
            for (i = 0; i < 8; i++) {
                /* Seq Scaling List Present Flag */
                bit = get_bit(&bb);
                if (bit == -1) {
                    return;
                }
                if (bit) {
                    parse_scaling_list(i < 6 ? 16 : 64, &bb);
                }
            }
        }
    }
    /* log2_max_frame_num_minus4 */
    exp_golomb_ue(&bb);
    /* pic_order_cnt_type */
    pic_order_cnt_type = exp_golomb_ue(&bb);
    if (pic_order_cnt_type == 0) {
        /* log2_max_pic_order_cnt_lsb_minus4 */
        exp_golomb_ue(&bb);
    }
    else if (pic_order_cnt_type == 1) {
        /* delta_pic_order_always_zero_flag */
        skip_bits(&bb, 1);
        /* offset_for_non_ref_pic */
        exp_golomb_se(&bb);
        /* offset_for_top_to_bottom_field */
        exp_golomb_se(&bb);
        size = exp_golomb_ue(&bb);
        for (i = 0; i < size; i++) {
            /* offset_for_ref_frame */
            exp_golomb_se(&bb);
        }
    }
    /* num_ref_frames */
    exp_golomb_ue(&bb);
    /* gaps_in_frame_num_value_allowed_flag */
    skip_bits(&bb, 1);
    /* pic_width_in_mbs */
    width_in_mbs = exp_golomb_ue(&bb) + 1;
    /* pic_height_in_map_units */
    height_in_map_units = exp_golomb_ue(&bb) + 1;
    /* frame_mbs_only_flag */
    frame_mbs_only_flag = get_bit(&bb);
    if (frame_mbs_only_flag == -1) {
        return;
    }
    if (!frame_mbs_only_flag) {
        /* mb_adaptive_frame_field */
        skip_bits(&bb, 1);
    }
    /* direct_8x8_inference_flag */
    skip_bits(&bb, 1);
    /* frame_cropping */
    left = right = top = bottom = 0;
    bit = get_bit(&bb);
    if (bit == -1) {
        return;
    }
    if (bit) {
        left = exp_golomb_ue(&bb) * 2;
        right = exp_golomb_ue(&bb) * 2;
        top = exp_golomb_ue(&bb) * 2;
        bottom = exp_golomb_ue(&bb) * 2;
        if (!frame_mbs_only_flag) {
            top *= 2;
            bottom *= 2;
        }
    }
    /* width */
    *width = width_in_mbs * 16 - (left + right);
    /* height */
    *height = height_in_map_units * 16 - (top + bottom);
    if (!frame_mbs_only_flag) {
        *height *= 2;
    }
}