#ifndef _mov_ioutil_h_
#define _mov_ioutil_h_

#include <stdio.h>
#include <unistd.h>
#include "assert.h"
#include "mov-buffer.h"

struct mov_ioutil_t
{
	struct mov_buffer_t io;
	void *param;
	int error;
	uint64_t pos;
};

static inline int mov_buffer_error(const struct mov_ioutil_t *io)
{
	return io->error;
}

static inline void mov_buffer_clear_ioerr(const struct mov_ioutil_t *io)
{
	((struct mov_ioutil_t *)io)->error = 0;
}

static inline uint64_t mov_buffer_tell(const struct mov_ioutil_t *io)
{
	int64_t v;
	v = io->io.tell(io->param);
	if (v < 0)
	{
		void *fp = io->io.get_fp ? io->io.get_fp(io->param) : 0;
		xprint("tell err v = %lld, fp = 0x%x", v, fp);
		// return mov_buffer_tell(io);
		((struct mov_ioutil_t*)io)->error = -1;
	}
	return v;
}

static inline void mov_buffer_seek(const struct mov_ioutil_t *io, int64_t offset)
{
	// xprint("seek to..., offset = %lld", offset);
	((struct mov_ioutil_t *)io)->error = io->io.seek(io->param, offset);
	if (io->error != 0)
	{
		xprint("seek err ..., offset = %lld", offset);
		// mov_buffer_seek(io, offset);
	}
}

static inline void mov_buffer_skip(struct mov_ioutil_t *io, uint64_t bytes)
{
	uint64_t offset;
	if (0 == io->error)
	{
		offset = mov_buffer_tell(io);
		// xprint("seek param offset = %lld bytes = %llu", offset, bytes);
		mov_buffer_seek(io, offset + bytes);
	}
}

static inline void mov_buffer_read(struct mov_ioutil_t *io, void *data, uint64_t bytes)
{
	if (0 == io->error)
		io->error = io->io.read(io->param, data, bytes);
}

static inline void mov_buffer_read_data(struct mov_ioutil_t *io, uint32_t handler_type, void *data, uint64_t bytes)
{
	assert(io->io.read_data);
	if (0 == io->error)
		io->error = io->io.read_data(io->param, handler_type, data, bytes);
}

static inline void mov_buffer_write(struct mov_ioutil_t *io, const void *data, uint64_t bytes)
{
	// io->pos = io->pos == 0 ? mov_buffer_tell(io) : io->pos;
	uint64_t pos = io->pos;
	if (0 == io->error)
		((struct mov_ioutil_t *)io)->error = io->io.write(io->param, data, bytes);
	else
		xprint("io error, err = %d", io->error);

	if (0 != io->error)
	{
		// void *fp = io->io.get_fp ? io->io.get_fp(io->param) : 0;
		// xprint("io write err ...,fp = 0x%x, pos = %llu, data = 0x%x, len = %llu", fp, pos, data, bytes);
		mov_buffer_clear_ioerr(io);
		// mov_buffer_seek(io, pos);
		// mov_buffer_write(io, data, bytes);
	}
	io->pos = 0;
}

static inline void mov_buffer_write_data(struct mov_ioutil_t *io, int track_type, const void *data, uint64_t bytes, uint64_t *wrote_len)
{
	// io->pos = io->pos == 0 ? mov_buffer_tell(io) : io->pos;
	uint64_t pos = io->pos;
	if (0 == io->error)
		((struct mov_ioutil_t *)io)->error = io->io.write_data(io->param, track_type, data, bytes, wrote_len);

	if (0 != io->error)
	{
		// void *fp = io->io.get_fp ? io->io.get_fp(io->param) : 0;
		// xprint("data write err...,fp = 0x%x, pos = %llu, data = 0x%x, len = %llu", fp, pos, data, bytes);
		mov_buffer_clear_ioerr(io);
		// mov_buffer_seek(io, pos);
		// mov_buffer_write_data(io, track_type, data, bytes, wrote_len);
	}
	io->pos = 0;
}

static inline void mov_buffer_write_zero(struct mov_ioutil_t *io, uint64_t bytes, int sleep/* ms */)
{
	// io->pos = io->pos == 0 ? mov_buffer_tell(io) : io->pos;
	uint64_t pos = io->pos;
	if (0 == io->error)
	{
		const int buflen = 1024 * 4;
		char zeros[1024 * 4] = {0};
		uint64_t remain = bytes;
		while (remain > 0)
		{
			int len = remain > buflen ? buflen : remain;
			((struct mov_ioutil_t *)io)->error = io->io.write(io->param, zeros, len);
			if (0 != io->error)
			{
				break;
			}
			remain -= len;

			if (sleep)
			{
				usleep(1000 * sleep);
			}
			
		}
	}

	if (0 != io->error)
	{
		xprint("zero write err ...");
		mov_buffer_clear_ioerr(io);
		// mov_buffer_seek(io, pos);
		// mov_buffer_write_zero(io, bytes, sleep);
	}
	io->pos = 0;
}

static inline uint8_t mov_buffer_r8(struct mov_ioutil_t* io)
{
	uint8_t v = 0;
	mov_buffer_read(io, &v, 1);
	return v;
}

static inline uint16_t mov_buffer_r16(struct mov_ioutil_t* io)
{
	uint16_t v;
	v = mov_buffer_r8(io);
	v = (v << 8) | mov_buffer_r8(io);
	return v;
}

static inline uint32_t mov_buffer_r24(struct mov_ioutil_t* io)
{
	uint32_t v;
	v = mov_buffer_r8(io);
	v = (v << 16) | mov_buffer_r16(io);
	return v;
}

static inline uint32_t mov_buffer_r32(struct mov_ioutil_t* io)
{
	uint32_t v;
	v = mov_buffer_r16(io);
	v = (v << 16) | mov_buffer_r16(io);
	return v;
}

static inline uint64_t mov_buffer_r64(struct mov_ioutil_t* io)
{
	uint64_t v;
	v = mov_buffer_r32(io);
	v = (v << 32) | mov_buffer_r32(io);
	return v;
}

static inline void mov_buffer_w8(const struct mov_ioutil_t* io, uint8_t v)
{
	mov_buffer_write(io, &v, 1);
}

static inline void mov_buffer_w16(const struct mov_ioutil_t* io, uint16_t v)
{
	mov_buffer_w8(io, (uint8_t)(v >> 8));
	mov_buffer_w8(io, (uint8_t)v);
}

static inline void mov_buffer_w24(const struct mov_ioutil_t* io, uint32_t v)
{
	mov_buffer_w16(io, (uint16_t)(v >> 8));
	mov_buffer_w8(io, (uint8_t)v);
}

static inline void mov_buffer_w32(const struct mov_ioutil_t* io, uint32_t v)
{
	mov_buffer_w16(io, (uint16_t)(v >> 16));
	mov_buffer_w16(io, (uint16_t)v);
}

static inline void mov_buffer_w64(const struct mov_ioutil_t* io, uint64_t v)
{
	mov_buffer_w32(io, (uint32_t)(v >> 32));
	mov_buffer_w32(io, (uint32_t)v);
}

#endif /* !_mov_ioutil_h_ */
