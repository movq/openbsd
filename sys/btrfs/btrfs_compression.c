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
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/vnode.h>

#include <btrfs/btrfs_var.h>
#include <btrfs/btrfs_zstd.h>

static int
btrfs_decompress(uint8_t compression, void *destination, size_t capacity,
    const void *source, size_t source_size, size_t *result_size)
{
	switch (compression) {
	case BTRFS_COMPRESS_ZSTD:
		return (btrfs_zstd_decompress(destination, capacity, source,
		    source_size, result_size));
	case BTRFS_COMPRESS_ZLIB:
	case BTRFS_COMPRESS_LZO:
		return (EOPNOTSUPP);
	default:
		return (EINVAL);
	}
}

static int
btrfs_read_compressed_regular(struct btrfs_node *node,
    const struct btrfs_file_extent *extent, uint8_t *compressed)
{
	struct btrfs_fs *bmp = node->bn_mount;
	struct buf *bp = NULL;
	uint32_t *csums = NULL;
	uint64_t inode_flags, logical;
	const uint32_t *expectedp;
	uint32_t sectorsize;
	size_t nsectors;
	size_t offset;
	int error = 0;

	sectorsize = letoh32(bmp->bm_super.sectorsize);
	inode_flags = node->bn_inode.bi_flags;
	if ((extent->bfe_disk_num_bytes & (sectorsize - 1)) != 0)
		return (EINVAL);
	nsectors = extent->bfe_disk_num_bytes / sectorsize;
	if ((inode_flags & BTRFS_INODE_NODATASUM) == 0) {
		csums = mallocarray(nsectors, sizeof(*csums), M_BTRFS,
		    M_WAITOK);
		error = btrfs_read_data_csums(bmp, extent->bfe_disk_bytenr,
		    extent->bfe_disk_num_bytes, csums);
		if (error != 0) {
			if (error == ENOENT)
				error = EINVAL;
			goto out;
		}
	}

	for (offset = 0; offset < extent->bfe_disk_num_bytes;
	    offset += sectorsize) {
		logical = extent->bfe_disk_bytenr + offset;
		expectedp = NULL;
		if (csums != NULL)
			expectedp = &csums[offset / sectorsize];
		error = btrfs_read_data_block(bmp, logical, expectedp, &bp);
		if (error != 0)
			break;
		memcpy(compressed + offset, bp->b_data, sectorsize);
		brelse(bp);
		bp = NULL;
	}

out:
	if (bp != NULL)
		brelse(bp);
	if (csums != NULL)
		free(csums, M_BTRFS, nsectors * sizeof(*csums));
	return (error);
}

int
btrfs_read_compressed_extent(struct btrfs_node *node,
    const struct btrfs_file_extent *extent, uint64_t offset, size_t size,
    void *destination)
{
	uint8_t *compressed = NULL, *uncompressed = NULL;
	uint64_t relative;
	size_t output_offset, result_size, source_size;
	int error;

	if (extent->bfe_compression == BTRFS_COMPRESS_NONE ||
	    extent->bfe_ram_bytes == 0 ||
	    extent->bfe_ram_bytes > BTRFS_MAX_UNCOMPRESSED)
		return (EINVAL);

	if (offset < extent->bfe_logical || destination == NULL)
		return (EINVAL);
	relative = offset - extent->bfe_logical;
	if (relative > extent->bfe_length ||
	    size > extent->bfe_length - relative ||
	    extent->bfe_disk_offset > extent->bfe_ram_bytes ||
	    relative > extent->bfe_ram_bytes - extent->bfe_disk_offset ||
	    size > extent->bfe_ram_bytes - extent->bfe_disk_offset - relative)
		return (EINVAL);
	output_offset = extent->bfe_disk_offset + relative;

	switch (extent->bfe_type) {
	case BTRFS_FILE_EXTENT_INLINE:
		compressed = (uint8_t *)extent->bfe_inline_data;
		source_size = extent->bfe_inline_size;
		break;
	case BTRFS_FILE_EXTENT_REG:
		if (extent->bfe_disk_num_bytes == 0 ||
		    extent->bfe_disk_num_bytes > BTRFS_MAX_COMPRESSED)
			return (EINVAL);
		source_size = extent->bfe_disk_num_bytes;
		compressed = malloc(source_size, M_BTRFS, M_WAITOK);
		error = btrfs_read_compressed_regular(node, extent, compressed);
		if (error != 0)
			goto out;
		break;
	default:
		return (EINVAL);
	}

	uncompressed = malloc(extent->bfe_ram_bytes, M_BTRFS, M_WAITOK);
	error = btrfs_decompress(extent->bfe_compression, uncompressed,
	    extent->bfe_ram_bytes, compressed, source_size, &result_size);
	if (error != 0)
		goto out;
	if (result_size != extent->bfe_ram_bytes) {
		error = EINVAL;
		goto out;
	}
	memcpy(destination, uncompressed + output_offset, size);
out:
	if (uncompressed != NULL)
		free(uncompressed, M_BTRFS, extent->bfe_ram_bytes);
	if (extent->bfe_type == BTRFS_FILE_EXTENT_REG && compressed != NULL)
		free(compressed, M_BTRFS, source_size);
	return (error);
}
