
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include "mp4.h"
#include "mov-buffer.h"
#include "mp4-mutex.h"

#define sfwrite safe_write

static size_t safe_write(const void *data, size_t s, size_t n, void *ptr)
{
	mp4_mutex_lock(FILE_MUTEX);
    size_t res = fwrite(data, s, n, ptr);
    int count = 0;
    while (res != n && 5 > (count++))
    {
        printf("write err res = %d err = %s, %d time retry\n", res, strerror(errno), count);
        res = fwrite(data, s, n, ptr);
    }
	mp4_mutex_unlock(FILE_MUTEX);
    if (res != n)
    {
        size_t datalen = s * n;
        printf("cant write data res = %d, fp = 0x%x, datalen = %d, err: %s\n", res, ptr, datalen, strerror(errno));
        return 0;
    }
    return n;
}

static int mov_file_read(void* fp, void* data, uint64_t bytes)
{
    if (bytes == fread(data, 1, bytes, (FILE*)fp))
        return 0;
	return 0 != ferror((FILE*)fp) ? ferror((FILE*)fp) : -1 /*EOF*/;
}

static int mov_file_write(void* fp, const void* data, uint64_t bytes)
{
	return 1 == sfwrite(data, bytes, 1, (FILE*)fp) ? 0 : ferror((FILE*)fp);
}

static int mov_file_seek(void* fp, int64_t offset)
{
	// xprint("seek to %lld", offset);
	return fseek((FILE*)fp, offset, offset >= 0 ? SEEK_SET : SEEK_END);
}

static int64_t mov_file_tell(void* fp)
{
	return ftell((FILE*)fp);
}

static void* mov_file_get_fp(void* fp)
{
	return fp;
}

static int mov_file_cache_read(void* fp, void* data, uint64_t bytes)
{
	uint8_t* p = (uint8_t*)data;
	mov_file_cache_t* file = (mov_file_cache_t*)fp;
	while (bytes > 0)
	{
		assert(file->off <= file->len);
		if (file->off >= file->len)
		{
			if (bytes >= (file->size))
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
				file->len = (unsigned int)fread(file->ptr, 1, (file->size), file->fp);
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
	mov_file_cache_t* file = (mov_file_cache_t*)fp;
	
	file->tell += bytes;

	if (file->off + bytes < (file->size))
	{
		memcpy(file->ptr + file->off, data, bytes);
		file->off += (unsigned int)bytes;
		return 0;
	}

	// write buffer
	if (file->off > 0)
	{
		if (1 != sfwrite(file->ptr, file->off, 1, file->fp))
		{
			file->off = 0;
			file->tell = ftell(file->fp) + file->off;
			return ferror(file->fp);
		}
		file->off = 0; // clear buffer
	}

	//清空buffer后再尝试拷贝到buffer，这次数据还是大，就直接写
	if (file->off + bytes < (file->size))
	{
		memcpy(file->ptr + file->off, data, bytes);
		file->off += (unsigned int)bytes;
		return 0;
	}

	// write data;
	if (1 != sfwrite(data, bytes, 1, file->fp))
	{
		file->tell = ftell(file->fp) + file->off;
		return ferror(file->fp);
	}
	
	return 0;
}

static int mov_file_cache_seek(void* fp, int64_t offset)
{
	int r;
	mov_file_cache_t* file = (mov_file_cache_t*)fp;
	if (offset != file->tell)
	{
		if (file->off > file->len)
		{
			// write bufferred data
			if(1 != sfwrite(file->ptr, file->off, 1, file->fp))
			{
				file->off = 0;
				file->tell = ftell(file->fp) + file->off;
				return ferror(file->fp);
			}
			// xprint("write data to file size = %u", file->off);
		}

		file->off = file->len = 0;
		r = fseek(file->fp, offset, offset >= 0 ? SEEK_SET : SEEK_END);
		file->tell = ftell(file->fp);
		return r;
	}
	return 0;
}

static int64_t mov_file_cache_tell(void* fp)
{
	mov_file_cache_t* file = (mov_file_cache_t*)fp;
	long real_tell = ftell(file->fp);
	long dt = (int64_t)(file->tell - (uint64_t)file->off + (uint64_t)file->len);
	if (real_tell != dt)
	{
		xprint("real_tell = %ln tell = %llu, off = %u", real_tell, file->tell, file->off);
		return -1;
	}
	return (int64_t)file->tell;
	//return ftell(file->fp);
}

static void* mov_file_cache_get_fp(void* fp)
{
	mov_file_cache_t* file = (mov_file_cache_t*)fp;
	return file->fp;
}

mov_file_cache_t *make_mov_file_cache(FILE* fp, int cache_size)
{
	mov_file_cache_t *cache = calloc(1,sizeof(mov_file_cache_t));

	cache->fp = fp;
	cache->size = cache_size;
	cache->ptr = malloc(cache_size);

	return cache;
}
void destroy_mov_file_cache(mov_file_cache_t *cache)
{
	free(cache->ptr);
	free(cache);
}

struct mov_buffer_t* mov_file_buffer(void)
{
	static struct mov_buffer_t s_io = {
		mov_file_read,
		0,
		mov_file_write,
		0,
		mov_file_seek,
		mov_file_tell,
		mov_file_get_fp,
	};
	return &s_io;
}

const struct mov_buffer_t* mov_file_cache_buffer(void)
{
	static struct mov_buffer_t s_io = {
		mov_file_cache_read,
		0,
		mov_file_cache_write,
		0,
		mov_file_cache_seek,
		mov_file_cache_tell,
		mov_file_cache_get_fp,
	};
	return &s_io;
}