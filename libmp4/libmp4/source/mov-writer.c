#include "mov-writer.h"
#include "mov-internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <time.h>

struct mov_writer_t
{
	struct mov_t mov;
	uint64_t mdat_size;
	uint64_t mdat_offset;
	uint64_t moov_offset;
	uint64_t free_data_offset;
};

static int mov_write_tail(struct mov_t* mov)
{
	mov_buffer_w32(&mov->io, 8 + strlen(MOV_APP)); /* size */
	mov_buffer_write(&mov->io, "free", 4);
	mov_buffer_write(&mov->io, MOV_APP, strlen(MOV_APP));
	return 0;
}

static size_t mov_write_moov(struct mov_t* mov)
{
	int i;
	size_t size;
	uint64_t offset;

	size = 8 /* Box */;
	offset = mov_buffer_tell(&mov->io);
	mov_buffer_w32(&mov->io, 0); /* size */
	mov_buffer_write(&mov->io, "moov", 4);

	size += mov_write_mvhd(mov);
//	size += mov_write_iods(mov);
	for(i = 0; i < mov->track_count; i++)
	{
		mov->track = mov->tracks + i;
		if (mov->track->sample_count < 1)
			continue;
		size += mov_write_trak(mov);
	}

	size += mov_write_udta(mov);
	mov_write_size(mov, offset, size); /* update size */
	return size;
}

static size_t mov_write_moov_ex(struct mov_t* mov)
{
	int i;
	size_t size;
	uint64_t offset;

	size = 8 /* Box */;
	offset = mov_buffer_tell(&mov->io);
	mov_buffer_w32(&mov->io, 8/* 0 */); /* size */
	mov_buffer_write(&mov->io, "moov", 4);

	size += mov_write_mvhd(mov);
//	size += mov_write_iods(mov);
	for(i = 0; i < mov->track_count; i++)
	{
		mov->track = mov->tracks + i;
		if (mov->track->sample_count < 1)
			continue;
		size += mov_write_trak(mov);
	}

	size += mov_write_udta(mov);
	mov_write_size(mov, offset, size); /* update size */
	xprint("moov size = %u", size);

	//预留的200kb空间里，除真实的moov数据外都是skip box
	//写 skip
	assert(8 + MOOV_LEN - size > 0);
	size = 8 + MOOV_LEN - size;
	mov_buffer_w32(&mov->io, size); /* size */
	mov_buffer_write(&mov->io, "skip", 4);

	// xprint("mov_write_moov_ex");

	return size;
}

void mov_write_size(const struct mov_t* mov, uint64_t offset, size_t size)
{
	uint64_t offset2;
    assert(size < UINT32_MAX);
	offset2 = mov_buffer_tell(&mov->io);
	mov_buffer_seek(&mov->io, offset);
	mov_buffer_w32(&mov->io, (uint32_t)size);
	mov_buffer_seek(&mov->io, offset2);
}

static int mov_writer_init(struct mov_t* mov)
{
	mov->ftyp.major_brand = MOV_BRAND_ISOM;
	mov->ftyp.minor_version = 0x200;
	mov->ftyp.brands_count = 4;
	mov->ftyp.compatible_brands[0] = MOV_BRAND_ISOM;
	mov->ftyp.compatible_brands[1] = MOV_BRAND_ISO2;
	mov->ftyp.compatible_brands[2] = MOV_BRAND_AVC1;
	mov->ftyp.compatible_brands[3] = MOV_BRAND_MP41;
	mov->header = 0;
	return 0;
}

struct mov_writer_t* mov_writer_create_ex(const struct mov_buffer_t* buffer, void* param, int flags, void* mov_rd)
{
	struct mov_t* mov;
	struct mov_writer_t* writer;
	writer = (struct mov_writer_t*)calloc(1, sizeof(struct mov_writer_t));
	if (NULL == writer)
		return NULL;

	mov = &writer->mov;
	memcpy(mov,mov_rd,sizeof(struct mov_t));

	mov->flags = flags;
	mov->io.param = param;
	memcpy(&mov->io.io, buffer, sizeof(mov->io.io));
	mov_buffer_clear_ioerr(&(mov->io));
	
	mov_buffer_seek(&mov->io, 0);

	mov_writer_init(mov);
	mov_write_ftyp(mov);

	assert(mov->flags & MOV_FLAG_COSTUM_CONTINUEE);
	mov_buffer_w32(&mov->io, 8 + PW_DATA_LEN); /* size */
	mov_buffer_write(&mov->io, "pwdt", 4);
	writer->free_data_offset = mov_buffer_tell(&mov->io);
	mov_buffer_write_zero(&mov->io, PW_DATA_LEN, 0);

	writer->moov_offset = mov_buffer_tell(&mov->io);
	assert(!(mov->flags & MOV_FLAG_COSTUM_WRITZEROS));
	writer->mdat_offset = writer->moov_offset + 8 + MOOV_LEN;

	#if 0
	//通过moov头的mdat_size计算续写的媒体数据写入起点
	writer->mdat_size = mov->mdat_size;
	uint64_t newbeg = writer->mdat_offset + writer->mdat_size + 8;
	mov_buffer_seek(&mov->io, newbeg);
	#else
	//续写的媒体数据写入起点为文件尾,并通过该起点计算续写的媒体数据写入起点
	mov_buffer_seek(&mov->io, -1);
	uint64_t newbeg = mov_buffer_tell(&mov->io) + 1;
	mov_buffer_seek(&mov->io, newbeg);
	writer->mdat_size = newbeg - writer->mdat_offset - 8;
	#endif

	xprint("moov offset = %llu, mdat offset = %llu, new data begin = %llu, mdat_size = %llu", writer->moov_offset, writer->mdat_offset, newbeg, writer->mdat_size);
	
	return writer;
}

struct mov_writer_t* mov_writer_create(const struct mov_buffer_t* buffer, void* param, int flags)
{
	struct mov_t* mov;
	struct mov_writer_t* writer;
	writer = (struct mov_writer_t*)calloc(1, sizeof(struct mov_writer_t));
	if (NULL == writer)
		return NULL;

	mov = &writer->mov;
	mov->flags = flags;
	mov->io.param = param;
	memcpy(&mov->io.io, buffer, sizeof(mov->io.io));

	mov->mvhd.next_track_ID = 1;
	mov->mvhd.creation_time = time(NULL) + 0x7C25B080; // 1970 based -> 1904 based;
	mov->mvhd.modification_time = mov->mvhd.creation_time;
	mov->mvhd.timescale = 1000;
	mov->mvhd.duration = 0; // placeholder

	mov_writer_init(mov);
	mov_write_ftyp(mov);

	if (mov->flags&MOV_FLAG_COSTUM_FASTSTART)
	{
		mov_buffer_w32(&mov->io, 8 + PW_DATA_LEN); /* size */
		mov_buffer_write(&mov->io, "pwdt", 4);
		writer->free_data_offset = mov_buffer_tell(&mov->io);
		mov_buffer_write_zero(&mov->io, PW_DATA_LEN, 0);

		writer->moov_offset = mov_buffer_tell(&mov->io);

		//预留200kb空间，用来写moov
		if (mov->flags & MOV_FLAG_COSTUM_WRITZEROS)
		{
			mov_buffer_w32(&mov->io, 8 /* 8 + MOOV_LEN */); /* size */
			mov_buffer_write(&mov->io, "moov", 4);
			int slow = mov->flags & MOV_FLAG_COSTUM_WRITESLOW;
			mov_buffer_write_zero(&mov->io, MOOV_LEN, slow ? 1 : 0);
		}
		else
		{
			writer->mdat_offset = writer->moov_offset + 8 + MOOV_LEN;
			mov_buffer_seek(&mov->io, writer->mdat_offset);
		}
	}
	else
	{
		// free(reserved for 64bit mdat)
		mov_buffer_w32(&mov->io, 8); /* size */
		mov_buffer_write(&mov->io, "free", 4);
	}

	// mdat
	writer->mdat_offset = mov_buffer_tell(&mov->io);
	xprint("mdat offset = %llu", writer->mdat_offset);
	mov_buffer_w32(&mov->io, 0); /* size */
	mov_buffer_write(&mov->io, "mdat", 4);
	return writer;
}

static int mov_writer_move(struct mov_t* mov, uint64_t to, uint64_t from, size_t bytes);
void mov_writer_destroy(struct mov_writer_t* writer)
{
	int i;
	uint64_t offset, offset2;
	struct mov_t* mov;
	struct mov_track_t* track;
	mov = &writer->mov;

	if (mov->flags&MOV_FLAG_COSTUM_FASTSTART)
	{
		mov_writer_update_moov(writer);
		goto END;
	}

	// finish mdat box
	if (writer->mdat_size + 8 <= UINT32_MAX)
	{
		mov_write_size(mov, writer->mdat_offset, (uint32_t)(writer->mdat_size + 8)); /* update size */
	}
	else
	{
		offset2 = mov_buffer_tell(&mov->io);
		writer->mdat_offset -= 8; // overwrite free box
		mov_buffer_seek(&mov->io, writer->mdat_offset);
		mov_buffer_w32(&mov->io, 1);
		mov_buffer_write(&mov->io, "mdat", 4);
		mov_buffer_w64(&mov->io, writer->mdat_size + 16);
		mov_buffer_seek(&mov->io, offset2);
	}

	// finish sample info
	for (i = 0; i < mov->track_count; i++)
	{
		track = &mov->tracks[i];
		if(track->sample_count < 1)
			continue;

		// pts in ms
		track->mdhd.duration = (track->samples[track->sample_count - 1].dts - track->samples[0].dts);
		if (track->sample_count > 1)
		{
			// duration += 3/4 * avg-duration + 1/4 * last-frame-duration
			track->mdhd.duration += track->mdhd.duration * 3 / (track->sample_count - 1) / 4 + (track->samples[track->sample_count - 1].dts - track->samples[track->sample_count - 2].dts) / 4;
		}
		//track->mdhd.duration = track->mdhd.duration * track->mdhd.timescale / 1000;
		track->tkhd.duration = track->mdhd.duration * mov->mvhd.timescale / track->mdhd.timescale;
		if (track->tkhd.duration > mov->mvhd.duration)
			mov->mvhd.duration = track->tkhd.duration; // maximum track duration
	}

	// write moov box
	offset = mov_buffer_tell(&mov->io);
	mov_write_moov(mov);
	offset2 = mov_buffer_tell(&mov->io);
	
	if (MOV_FLAG_FASTSTART & mov->flags)
	{
		// check stco -> co64
		uint64_t co64 = 0;
		for (i = 0; i < mov->track_count; i++)
		{
			co64 += mov_stco_size(&mov->tracks[i], offset2 - offset);
		}

		if (co64)
		{
			uint64_t sz;
			do
			{
				sz = co64;
				co64 = 0;
				for (i = 0; i < mov->track_count; i++)
				{
					co64 += mov_stco_size(&mov->tracks[i], offset2 - offset + sz);
				}
			} while (sz != co64);
		}

		// rewrite moov
		for (i = 0; i < mov->track_count; i++)
			mov->tracks[i].offset += (offset2 - offset) + co64;

		mov_buffer_seek(&mov->io, offset);
		mov_write_moov(mov);
		assert(mov_buffer_tell(&mov->io) == offset2 + co64);
		offset2 = mov_buffer_tell(&mov->io);

		mov_writer_move(mov, writer->mdat_offset, offset, (size_t)(offset2 - offset));
	}

END:
	// mov_write_tail(mov); #####change#####
	for (i = 0; i < mov->track_count; i++)
        mov_free_track(mov->tracks + i);
	if (mov->tracks)
		free(mov->tracks);
	free(writer);
}

static int mov_writer_move(struct mov_t* mov, uint64_t to, uint64_t from, size_t bytes)
{
	uint8_t* ptr;
	uint64_t i, j;
	void* buffer[2];

	assert(bytes < INT32_MAX);
	ptr = malloc((size_t)(bytes * 2));
	if (NULL == ptr)
		return -ENOMEM;
	buffer[0] = ptr;
	buffer[1] = ptr + bytes;

	mov_buffer_seek(&mov->io, from);
	mov_buffer_read(&mov->io, buffer[0], bytes);
    mov_buffer_seek(&mov->io, to);
    mov_buffer_read(&mov->io, buffer[1], bytes);

	j = 0;
	for (i = to; i < from; i += bytes)
	{
		mov_buffer_seek(&mov->io, i);
		mov_buffer_write(&mov->io, buffer[j], bytes);
        // MSDN: fopen https://msdn.microsoft.com/en-us/library/yeby3zcb.aspx
        // When the "r+", "w+", or "a+" access type is specified, both reading and 
        // writing are enabled (the file is said to be open for "update"). 
        // However, when you switch from reading to writing, the input operation 
        // must encounter an EOF marker. If there is no EOF, you must use an intervening 
        // call to a file positioning function. The file positioning functions are 
        // fsetpos, fseek, and rewind. 
        // When you switch from writing to reading, you must use an intervening 
        // call to either fflush or to a file positioning function.
        mov_buffer_seek(&mov->io, i+bytes);
        mov_buffer_read(&mov->io, buffer[j], bytes);
        j ^= 1;
	}

    mov_buffer_seek(&mov->io, i);
	mov_buffer_write(&mov->io, buffer[j], bytes - (size_t)(i - from));

	free(ptr);
	return mov_buffer_error(&mov->io);
}

int mov_writer_write(struct mov_writer_t* writer, int track, const void* data, size_t bytes, int64_t pts, int64_t dts, int flags)
{
	struct mov_t* mov;
	struct mov_sample_t* sample;

    assert(bytes < UINT32_MAX);
	if (track < 0 || track >= (int)writer->mov.track_count)
		return -ENOENT;
	
	mov = &writer->mov;
	mov->track = &mov->tracks[track];

	if (mov->track->sample_count + 1 >= mov->track->sample_offset)
	{
		void* ptr = realloc(mov->track->samples, sizeof(struct mov_sample_t) * (mov->track->sample_offset + 1024));
		if (NULL == ptr) return -ENOMEM;
		mov->track->samples = ptr;
		mov->track->sample_offset += 1024;
	}

	pts = pts * mov->track->mdhd.timescale / 1000;
	dts = dts * mov->track->mdhd.timescale / 1000;

	sample = &mov->track->samples[mov->track->sample_count++];
	sample->offset = mov_buffer_tell(&mov->io);

	uint64_t wrote_len = 0;
	mov_buffer_write_data(&mov->io, mov->track->handler_type, data, bytes, &wrote_len);
	// assert(wrote_len==bytes);

	sample->sample_description_index = 1;
	sample->bytes = (uint32_t)/* bytes */wrote_len;
	sample->flags = flags;
    sample->data = NULL;
	sample->pts = pts;
	sample->dts = dts;
	
	// sample->offset = mov_buffer_tell(&mov->io);
	// mov_buffer_write_data(&mov->io, mov->track->handler_type, data, bytes);

	if (INT64_MIN == mov->track->start_dts)
		mov->track->start_dts = /* sample->dts */0;
	writer->mdat_size += /* bytes */wrote_len; // update media data size
	return mov_buffer_error(&mov->io);
}

static int mov_get_track_id(struct mov_t* mov, uint32_t type)
{
	int i;
	uint32_t j;
	struct mov_track_t* track;
    struct mov_sample_entry_t* entry;
	xprint("track_count = %d", mov->track_count);

	for (i = 0; i < mov->track_count; i++)
	{
		track = &mov->tracks[i];
		for (j = 0; j < track->stsd.entry_count && j < 1 /* only the first */; j++)
		{
			// xprint("handler_type = %u type = %u", track->handler_type, type);
            entry = &track->stsd.entries[j];
			if (track->handler_type == type)
			{
				track->sample_offset = track->sample_count + 1;//为了续写
				return track->tkhd.track_ID - 1;
			}
		}	
	}
	return -1;
}

int mov_writer_add_audio(struct mov_writer_t* writer, uint8_t object, int channel_count, int bits_per_sample, int sample_rate, const void* extra_data, size_t extra_data_size)
{
	struct mov_t* mov;
	struct mov_track_t* track;

    mov = &writer->mov;
	if (mov->flags& MOV_FLAG_COSTUM_CONTINUEE)
	{
		return mov_get_track_id(mov, MOV_AUDIO);
	}

    track = mov_add_track(mov);
    if (NULL == track)
        return -ENOMEM;

    if (0 != mov_add_audio(track, &mov->mvhd, 1000, object, channel_count, bits_per_sample, sample_rate, extra_data, extra_data_size))
        return -ENOMEM;

    mov->mvhd.next_track_ID++;
    return mov->track_count++;
}

int mov_writer_add_video(struct mov_writer_t* writer, uint8_t object, int width, int height, const void* extra_data, size_t extra_data_size)
{
	struct mov_t* mov;
	struct mov_track_t* track;

    mov = &writer->mov;
	if (mov->flags& MOV_FLAG_COSTUM_CONTINUEE)
	{
		return mov_get_track_id(mov, MOV_VIDEO);
	}
    track = mov_add_track(mov);
    if (NULL == track)
        return -ENOMEM;

    if (0 != mov_add_video(track, &mov->mvhd, 1000, object, width, height, extra_data, extra_data_size))
        return -ENOMEM;

    mov->mvhd.next_track_ID++;
    return mov->track_count++;
}

int mov_writer_add_subtitle(struct mov_writer_t* writer, uint8_t object, const void* extra_data, size_t extra_data_size)
{
	struct mov_t* mov;
	struct mov_track_t* track;

	mov = &writer->mov;
    track = mov_add_track(mov);
	if (NULL == track)
		return -ENOMEM;

    if (0 != mov_add_subtitle(track, &mov->mvhd, 1000, object, extra_data, extra_data_size))
        return -ENOMEM;

    mov->mvhd.next_track_ID++;
	return mov->track_count++;
}

int mov_writer_add_udta(mov_writer_t* mov, const void* data, size_t size)
{
	mov->mov.udta = data;
	mov->mov.udta_size = size;
	return 0;
}

int mov_writer_update_free_data(struct mov_writer_t* writer, int offset, int len, void* val)
{
	struct mov_t* mov;
	struct mov_track_t* track;
	mov = &writer->mov;
	if (!(mov->flags&MOV_FLAG_COSTUM_FASTSTART))
	{
		return 1;
	}

	uint64_t pos = mov_buffer_tell(&mov->io);
	mov_buffer_seek(&mov->io, writer->free_data_offset + offset);
	
	switch (len)
	{
	case 1:
		mov_buffer_w8(&mov->io,*(uint8_t*)val);
		break;
	case 2:
		mov_buffer_w16(&mov->io,*(uint16_t*)val);
		break;
	case 4:
		mov_buffer_w32(&mov->io,*(uint32_t*)val);
		break;
	case 8:
		mov_buffer_w64(&mov->io,*(uint64_t*)val);
		break;

	default:
		break;
	}

	mov_buffer_seek(&mov->io, pos);

	return 0;
}

int mov_writer_update_moov(mov_writer_t* writer)
{
	int i;
	uint64_t offset, offset2;
	struct mov_t* mov;
	struct mov_track_t* track;
	mov = &writer->mov;

	if (!(mov->flags&MOV_FLAG_COSTUM_FASTSTART))
	{
		return 0;
	}
	
	assert(writer->mdat_size + 8 <= UINT32_MAX);
	
	mov_write_size(mov, writer->mdat_offset, (uint32_t)(writer->mdat_size + 8)); /* update size */

	// finish sample info
	xprint("track_count = %d", mov->track_count);
	for (i = 0; i < mov->track_count; i++)
	{
		track = &mov->tracks[i];
		if(track->sample_count < 1)
			continue;

		// pts in ms
		track->mdhd.duration = (track->samples[track->sample_count - 1].dts - track->samples[0].dts);
		if (track->sample_count > 1)
		{
			// duration += 3/4 * avg-duration + 1/4 * last-frame-duration
			track->mdhd.duration += track->mdhd.duration * 3 / (track->sample_count - 1) / 4 + (track->samples[track->sample_count - 1].dts - track->samples[track->sample_count - 2].dts) / 4;
		}
		//track->mdhd.duration = track->mdhd.duration * track->mdhd.timescale / 1000;
		track->tkhd.duration = track->mdhd.duration * mov->mvhd.timescale / track->mdhd.timescale;
		if (track->tkhd.duration > mov->mvhd.duration)
			mov->mvhd.duration = track->tkhd.duration; // maximum track duration
	}

	// update moov box
	offset = mov_buffer_tell(&mov->io);
	xprint("offset = %llu, moov offset = %llu", offset, writer->moov_offset);
	mov_buffer_seek(&mov->io, writer->moov_offset);
	mov_write_moov_ex(mov);
	mov_buffer_seek(&mov->io, offset);

	return 0;
}