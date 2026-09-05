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

#include <lib/libkern/crc32c.h>

#include <btrfs/btrfs_var.h>

static int	btrfs_map_logical(const struct btrfs_chunk_map *, uint64_t,
		    uint32_t, struct btrfs_io_map *);
static int	btrfs_read_mapped(struct vnode *, const struct btrfs_io_map *,
		    uint32_t, uint64_t, btrfs_io_validate_fn, void *,
		    struct btrfs_io_result *, struct buf **);
static int	btrfs_validate_data_csum(const void *, size_t, void *);

static int
btrfs_map_logical(const struct btrfs_chunk_map *chunk, uint64_t logical,
    uint32_t length, struct btrfs_io_map *map)
{
	uint64_t delta;
	unsigned int i;

	if (length == 0 || chunk->length < length ||
	    logical < chunk->logical)
		return (ENOENT);
	delta = logical - chunk->logical;
	if (delta > chunk->length - length)
		return (ENOENT);
	if (chunk->nmirrors == 0 ||
	    chunk->nmirrors > BTRFS_MAX_MIRRORS)
		return (EINVAL);

	memset(map, 0, sizeof(*map));
	map->type = chunk->type;
	map->nmirrors = chunk->nmirrors;
	for (i = 0; i < chunk->nmirrors; i++) {
		if (chunk->physical[i] > UINT64_MAX - delta)
			return (EINVAL);
		map->physical[i] = chunk->physical[i] + delta;
	}
	return (0);
}

int
btrfs_lookup_logical(const struct btrfs_chunk_map *chunks,
    unsigned int nchunks, uint64_t logical, uint32_t length,
    struct btrfs_io_map *map)
{
	unsigned int i;
	int error;

	for (i = 0; i < nchunks; i++) {
		error = btrfs_map_logical(&chunks[i], logical, length, map);
		if (error == 0)
			return (0);
		if (error != ENOENT)
			return (error);
		if (logical < chunks[i].logical)
			break;
	}

	return (ENOENT);
}

int
btrfs_lookup_fs_logical(struct btrfs_fs *bmp, uint64_t logical,
    uint32_t length, struct btrfs_io_map *map)
{
	int error;

	/* A physical mapping is a value; no index pointer escapes this lock. */
	rw_enter_read(&bmp->bm_mapping_lock);
	error = btrfs_lookup_logical(bmp->bm_chunks, bmp->bm_nchunks,
	    logical, length, map);
	rw_exit_read(&bmp->bm_mapping_lock);
	return (error);
}

int
btrfs_read_logical(const struct btrfs_root *root,
    uint64_t logical, uint32_t length, uint64_t type_mask,
    btrfs_io_validate_fn validate, void *validate_arg,
    struct btrfs_io_result *result, struct buf **bpp)
{
	struct btrfs_io_map map;
	int error;

	*bpp = NULL;
	if (result != NULL) {
		memset(result, 0, sizeof(*result));
		result->bir_mirror = -1;
	}
	if (root->br_mount != NULL)
		error = btrfs_lookup_fs_logical(root->br_mount, logical,
		    length, &map);
	else if (root->br_bootstrap_chunks != NULL)
		error = btrfs_lookup_logical(root->br_bootstrap_chunks,
		    root->br_bootstrap_nchunks, logical, length, &map);
	else
		error = EINVAL;
	if (error != 0)
		return (error);
	return (btrfs_read_mapped(root->br_devvp, &map, length, type_mask,
	    validate, validate_arg, result, bpp));
}

static int
btrfs_read_mapped(struct vnode *devvp, const struct btrfs_io_map *map,
    uint32_t length, uint64_t type_mask, btrfs_io_validate_fn validate,
    void *validate_arg, struct btrfs_io_result *result, struct buf **bpp)
{
	struct buf *bp;
	unsigned int i;
	int error;

	if (devvp == NULL || length == 0 || type_mask == 0)
		return (EINVAL);
	if ((map->type & type_mask) == 0)
		return (EINVAL);
	if (result != NULL)
		result->bir_nmirrors = map->nmirrors;

	error = EIO;
	for (i = 0; i < map->nmirrors; i++) {
		bp = NULL;
		error = bread(devvp, map->physical[i] / DEV_BSIZE, length, &bp);
		if (error == 0 && bp->b_resid != 0)
			error = EIO;
		if (error == 0 && validate != NULL)
			error = validate(bp->b_data, length, validate_arg);
		if (result != NULL)
			result->bir_error[i] = error;
		if (error == 0) {
			if (result != NULL)
				result->bir_mirror = i;
			*bpp = bp;
			return (0);
		}
		if (bp != NULL)
			brelse(bp);
	}

	return (error);
}

int
btrfs_write_logical(struct btrfs_fs *bmp,
    uint64_t logical, uint32_t length, uint64_t type_mask,
    const void *data, struct btrfs_io_result *result)
{
	struct vnode *devvp = bmp->bm_devvp;
	struct btrfs_io_map map;
	struct buf *bp;
	void *copy;
	unsigned int i;
	int error, first_error = 0;

	if (result != NULL) {
		memset(result, 0, sizeof(*result));
		result->bir_mirror = -1;
	}
	if (devvp == NULL || data == NULL || length == 0 ||
	    length > MAXBSIZE || type_mask == 0)
		return (EINVAL);

	error = btrfs_lookup_fs_logical(bmp, logical, length, &map);
	if (error != 0)
		return (error);
	if ((map.type & type_mask) == 0)
		return (EINVAL);

	for (i = 0; i < map.nmirrors; i++) {
		if ((map.physical[i] & (DEV_BSIZE - 1)) != 0)
			return (EINVAL);
	}
	if (result != NULL)
		result->bir_nmirrors = map.nmirrors;

	/*
	 * Preserve one immutable source while each target buffer is acquired,
	 * submitted, and released.
	 */
	copy = malloc(length, M_BTRFS, M_WAITOK);
	memcpy(copy, data, length);
	for (i = 0; i < map.nmirrors; i++) {
		bp = getblk(devvp, map.physical[i] / DEV_BSIZE, length, 0,
		    INFSLP);
		memcpy(bp->b_data, copy, length);
		/*
		 * B_NOCACHE prevents bwrite() from becoming a delayed write
		 * when the device vnode belongs to an asynchronous mount.
		 */
		SET(bp->b_flags, B_NOCACHE);
		error = bwrite(bp);
		if (result != NULL)
			result->bir_error[i] = error;
		if (error != 0 && first_error == 0)
			first_error = error;
	}
	free(copy, M_BTRFS, length);

	return (first_error);
}

static int
btrfs_validate_data_csum(const void *data, size_t length, void *arg)
{
	const uint32_t *expected = arg;

	if (crc32c(0, data, length) != *expected)
		return (EIO);
	return (0);
}

int
btrfs_read_data_block(struct btrfs_fs *bmp, uint64_t logical,
    const uint32_t *expected_csum, struct buf **bpp)
{
	btrfs_io_validate_fn validate = NULL;
	struct btrfs_io_map map;
	uint32_t sectorsize;
	int error;

	*bpp = NULL;
	sectorsize = letoh32(bmp->bm_super.sectorsize);
	if ((logical & (sectorsize - 1)) != 0)
		return (EINVAL);
	if (expected_csum != NULL)
		validate = btrfs_validate_data_csum;
	error = btrfs_lookup_fs_logical(bmp, logical, sectorsize, &map);
	if (error == 0)
		error = btrfs_read_mapped(bmp->bm_devvp, &map, sectorsize,
		    BTRFS_BLOCK_GROUP_DATA, validate, (void *)expected_csum,
		    NULL, bpp);
	if (error == ENOENT)
		error = EINVAL;
	return (error);
}
