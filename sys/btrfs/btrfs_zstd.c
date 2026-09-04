/*	$OpenBSD$	*/

/*
 * Copyright (c) 2026 Mike Jones <mike@mjones.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/systm.h>

#include <btrfs/btrfs_zstd.h>
#include <btrfs/zstd/zstd.h>

union zstd_allocation {
	size_t		 za_size;
	uint8_t		 za_alignment[16];
};

void *
btrfs_zstd_kmalloc(size_t size)
{
	union zstd_allocation *allocation;
	size_t total;

	if (size > SIZE_MAX - sizeof(*allocation))
		return (NULL);
	total = sizeof(*allocation) + size;
	allocation = malloc(total, M_BTRFS, M_WAITOK);
	allocation->za_size = total;
	return (allocation + 1);
}

void *
btrfs_zstd_kcalloc(size_t number, size_t size)
{
	void *allocation;
	size_t total;

	if (size != 0 && number > SIZE_MAX / size)
		return (NULL);
	total = number * size;
	allocation = btrfs_zstd_kmalloc(total);
	if (allocation != NULL)
		memset(allocation, 0, total);
	return (allocation);
}

void
btrfs_zstd_kfree(void *pointer)
{
	union zstd_allocation *allocation;

	if (pointer == NULL)
		return;
	allocation = (union zstd_allocation *)pointer - 1;
	free(allocation, M_BTRFS, allocation->za_size);
}

static int
btrfs_zstd_error(size_t result)
{
	if (ZSTD_getErrorCode(result) == ZSTD_error_memory_allocation)
		return (ENOMEM);
	return (EINVAL);
}

int
btrfs_zstd_compress(void *destination, size_t capacity, const void *source,
    size_t source_size, int level, size_t *result_size)
{
	size_t result;

	result = ZSTD_compress(destination, capacity, source, source_size, level);
	if (ZSTD_isError(result))
		return (btrfs_zstd_error(result));
	*result_size = result;
	return (0);
}

int
btrfs_zstd_decompress(void *destination, size_t capacity, const void *source,
    size_t source_size, size_t *result_size)
{
	size_t frame_size, result;

	frame_size = ZSTD_findFrameCompressedSize(source, source_size);
	if (ZSTD_isError(frame_size))
		return (btrfs_zstd_error(frame_size));
	if (frame_size == 0 || frame_size > source_size)
		return (EINVAL);

	result = ZSTD_decompress(destination, capacity, source, frame_size);
	if (ZSTD_isError(result))
		return (btrfs_zstd_error(result));
	*result_size = result;
	return (0);
}
