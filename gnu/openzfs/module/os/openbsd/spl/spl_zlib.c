// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS compression entry points using OpenBSD's in-kernel zlib. */

#include <sys/types.h>
#include <sys/limits.h>
#include <sys/zmod.h>

int
z_compress_level(void *dest, size_t *destlen, const void *source,
    size_t sourcelen, int level)
{

	return (compress2_z(dest, destlen, source, sourcelen, level));
}

int
z_compress(void *dest, size_t *destlen, const void *source, size_t sourcelen)
{

	return (z_compress_level(dest, destlen, source, sourcelen,
	    Z_DEFAULT_COMPRESSION));
}

int
z_uncompress(void *dest, size_t *destlen, const void *source,
    size_t sourcelen)
{
	z_stream stream = { 0 };
	int error;

	if (destlen == NULL || *destlen > UINT_MAX || sourcelen > UINT_MAX)
		return (Z_BUF_ERROR);
	stream.next_in = (Bytef *)(uintptr_t)source;
	stream.avail_in = (uInt)sourcelen;
	stream.next_out = dest;
	stream.avail_out = (uInt)*destlen;

	error = inflateInit(&stream);
	if (error != Z_OK)
		return (error);
	error = inflate(&stream, Z_FINISH);
	if (error != Z_STREAM_END) {
		(void)inflateEnd(&stream);
		if (error == Z_NEED_DICT ||
		    (error == Z_BUF_ERROR && stream.avail_in == 0))
			return (Z_DATA_ERROR);
		return (error == Z_OK ? Z_BUF_ERROR : error);
	}
	*destlen = stream.total_out;
	return (inflateEnd(&stream));
}

const char *
z_strerror(int error)
{

	return (zError(error));
}
