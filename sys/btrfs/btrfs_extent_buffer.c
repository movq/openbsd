/* Copyright (C) 2026 Mike Jones <mike@mjones.org>
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
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

static int	btrfs_extent_buffer_load(const struct btrfs_root *,
		    struct btrfs_extent_buffer *, uint64_t);
static int	btrfs_validate_tree_block(const struct btrfs_super_block *,
		    const struct btrfs_header *, uint64_t, uint64_t, uint64_t,
		    uint64_t, uint8_t);
static int	btrfs_key_cmp(const struct btrfs_key *,
		    const struct btrfs_key *);

static int
btrfs_extent_buffer_matches(const struct btrfs_extent_buffer *eb,
    uint64_t generation, uint64_t owner, uint8_t level)
{
	return (eb->eb_generation == generation && eb->eb_owner == owner &&
	    eb->eb_level == level);
}

int
btrfs_extent_buffer_read(const struct btrfs_root *root, uint64_t logical,
    uint64_t generation, uint64_t view_generation, uint8_t level,
    struct btrfs_extent_buffer **ebp)
{
	struct btrfs_extent_buffer *eb, *new;
	struct btrfs_mount *bmp = root->br_mount;
	uint32_t sectorsize;
	int error;

	*ebp = NULL;
	sectorsize = letoh32(root->br_super->sectorsize);
	if (logical == 0 || (logical & (sectorsize - 1)) != 0 ||
	    generation == 0 || generation > view_generation ||
	    level >= BTRFS_MAX_LEVEL)
		return (EINVAL);

#ifdef DIAGNOSTIC
	if (bmp != NULL) {
		KASSERT(root->br_devvp == bmp->bm_devvp);
		KASSERT(root->br_super == &bmp->bm_super);
		KASSERT(root->br_chunks == bmp->bm_chunks);
		KASSERT(root->br_nchunks == bmp->bm_nchunks);
	}
#endif

	new = malloc(sizeof(*new), M_BTRFS, M_WAITOK | M_ZERO);
	new->eb_mount = bmp;
	new->eb_bytenr = logical;
	new->eb_generation = generation;
	new->eb_owner = root->br_owner;
	new->eb_level = level;
	new->eb_refs = 1;
	rw_init_flags(&new->eb_lock, "btreebuf", RWL_DUPOK);
	rw_enter_write(&new->eb_lock);

	if (bmp != NULL) {
		mtx_enter(&bmp->bm_ebmtx);
		LIST_FOREACH(eb, &bmp->bm_extent_buffers, eb_entry) {
			if (eb->eb_bytenr == logical)
				break;
		}
		if (eb != NULL) {
			if (!btrfs_extent_buffer_matches(eb, generation,
			    root->br_owner, level)) {
				mtx_leave(&bmp->bm_ebmtx);
				rw_exit_write(&new->eb_lock);
				free(new, M_BTRFS, sizeof(*new));
				return (EINVAL);
			}
			KASSERT(eb->eb_refs != UINT_MAX);
			eb->eb_refs++;
			mtx_leave(&bmp->bm_ebmtx);
			rw_exit_write(&new->eb_lock);
			free(new, M_BTRFS, sizeof(*new));

			rw_enter_read(&eb->eb_lock);
			KASSERT(eb->eb_loaded);
			if (eb->eb_error != 0) {
				error = eb->eb_error;
				btrfs_extent_buffer_put(eb);
				return (error);
			}
			*ebp = eb;
			return (0);
		}
		LIST_INSERT_HEAD(&bmp->bm_extent_buffers, new, eb_entry);
		mtx_leave(&bmp->bm_ebmtx);
	}

	error = btrfs_extent_buffer_load(root, new, view_generation);
	new->eb_error = error;
	new->eb_loaded = 1;
	rw_exit_write(&new->eb_lock);
	rw_enter_read(&new->eb_lock);
	if (error != 0) {
		btrfs_extent_buffer_put(new);
		return (error);
	}
	*ebp = new;
	return (0);
}

const void *
btrfs_extent_buffer_data(const struct btrfs_extent_buffer *eb)
{
#ifdef DIAGNOSTIC
	const struct btrfs_header *header;

	KASSERT(eb->eb_refs > 0);
	rw_assert_anylock((struct rwlock *)&eb->eb_lock);
	KASSERT(eb->eb_loaded);
	KASSERT(eb->eb_error == 0);
	KASSERT(eb->eb_buf != NULL);
	header = (const struct btrfs_header *)eb->eb_buf->b_data;
	KASSERT(letoh64(header->bytenr) == eb->eb_bytenr);
	KASSERT(letoh64(header->generation) == eb->eb_generation);
	KASSERT(letoh64(header->owner) == eb->eb_owner);
	KASSERT(header->level == eb->eb_level);
#endif
	return (eb->eb_buf->b_data);
}

void
btrfs_extent_buffer_put(struct btrfs_extent_buffer *eb)
{
	struct btrfs_mount *bmp = eb->eb_mount;
	int last;

	KASSERT(eb->eb_refs > 0);
	rw_assert_anylock(&eb->eb_lock);
	rw_exit(&eb->eb_lock);

	if (bmp != NULL) {
		mtx_enter(&bmp->bm_ebmtx);
		KASSERT(eb->eb_refs > 0);
		last = --eb->eb_refs == 0;
		if (last)
			LIST_REMOVE(eb, eb_entry);
		mtx_leave(&bmp->bm_ebmtx);
	} else {
		KASSERT(eb->eb_refs == 1);
		eb->eb_refs = 0;
		last = 1;
	}
	if (!last)
		return;

	rw_assert_unlocked(&eb->eb_lock);
	if (eb->eb_buf != NULL)
		brelse(eb->eb_buf);
	free(eb, M_BTRFS, sizeof(*eb));
}

static int
btrfs_extent_buffer_load(const struct btrfs_root *root,
    struct btrfs_extent_buffer *eb, uint64_t view_generation)
{
	struct btrfs_io_map map;
	struct buf *bp;
	unsigned int i;
	int error;

	rw_assert_wrlock(&eb->eb_lock);
	error = btrfs_lookup_logical(root->br_chunks, root->br_nchunks,
	    eb->eb_bytenr, letoh32(root->br_super->nodesize), &map);
	if (error != 0)
		return (error == ENOENT ? EINVAL : error);
	if ((map.type & (BTRFS_BLOCK_GROUP_METADATA |
	    BTRFS_BLOCK_GROUP_SYSTEM)) == 0)
		return (EINVAL);

	error = EIO;
	for (i = 0; i < map.nmirrors; i++) {
		bp = NULL;
		error = bread(root->br_devvp, map.physical[i] / DEV_BSIZE,
		    letoh32(root->br_super->nodesize), &bp);
		if (error == 0 && bp->b_resid != 0)
			error = EIO;
		if (error == 0)
			error = btrfs_validate_tree_block(root->br_super,
			    (const struct btrfs_header *)bp->b_data,
			    eb->eb_bytenr, eb->eb_generation, view_generation,
			    eb->eb_owner, eb->eb_level);
		if (error == 0) {
			eb->eb_buf = bp;
			return (0);
		}
		if (bp != NULL)
			brelse(bp);
	}

	return (error);
}

static int
btrfs_validate_tree_block(const struct btrfs_super_block *sb,
    const struct btrfs_header *header, uint64_t bytenr, uint64_t generation,
    uint64_t view_generation, uint64_t owner, uint8_t level)
{
	const struct btrfs_item *items;
	const struct btrfs_key_ptr *ptrs;
	const uint8_t *fsid;
	uint32_t csum, disk_csum, i, nritems, nodesize, offset, size;
	size_t array_end, data_end;

	nodesize = letoh32(sb->nodesize);
	memcpy(&disk_csum, header->csum, sizeof(disk_csum));
	disk_csum = letoh32(disk_csum);
	csum = crc32c(0, (const uint8_t *)header + sizeof(header->csum),
	    nodesize - sizeof(header->csum));
	if (csum != disk_csum)
		return (EINVAL);

	fsid = sb->fsid;
	if (letoh64(sb->incompat_flags) &
	    BTRFS_FEATURE_INCOMPAT_METADATA_UUID)
		fsid = sb->metadata_uuid;
	if (memcmp(header->fsid, fsid, BTRFS_UUID_SIZE) != 0 ||
	    bytenr == 0 ||
	    (bytenr & (letoh32(sb->sectorsize) - 1)) != 0 ||
	    generation == 0 || generation > view_generation ||
	    letoh64(header->bytenr) != bytenr ||
	    letoh64(header->generation) != generation ||
	    letoh64(header->owner) != owner || header->level != level ||
	    level >= BTRFS_MAX_LEVEL)
		return (EINVAL);

	nritems = letoh32(header->nritems);
	if (level != 0 && nritems == 0)
		return (EINVAL);

	if (level == 0) {
		if (nritems > (nodesize - sizeof(*header)) / sizeof(*items))
			return (EINVAL);
		array_end = nritems * sizeof(*items);
		data_end = nodesize - sizeof(*header);
		items = (const struct btrfs_item *)(header + 1);
		for (i = 0; i < nritems; i++) {
			offset = letoh32(items[i].offset);
			size = letoh32(items[i].size);
			if (offset < array_end || offset > data_end ||
			    size > data_end - offset)
				return (EINVAL);
			if (i != 0 &&
			    btrfs_key_cmp(&items[i - 1].key, &items[i].key) >= 0)
				return (EINVAL);
			data_end = offset;
		}
	} else {
		if (nritems > (nodesize - sizeof(*header)) / sizeof(*ptrs))
			return (EINVAL);
		ptrs = (const struct btrfs_key_ptr *)(header + 1);
		for (i = 0; i < nritems; i++) {
			if (letoh64(ptrs[i].blockptr) == 0 ||
			    (letoh64(ptrs[i].blockptr) &
			    (letoh32(sb->sectorsize) - 1)) != 0 ||
			    letoh64(ptrs[i].generation) == 0 ||
			    letoh64(ptrs[i].generation) > view_generation)
				return (EINVAL);
			if (i != 0 &&
			    btrfs_key_cmp(&ptrs[i - 1].key,
			    &ptrs[i].key) >= 0)
				return (EINVAL);
		}
	}

	return (0);
}

static int
btrfs_key_cmp(const struct btrfs_key *a, const struct btrfs_key *b)
{
	uint64_t a_objectid, a_offset, b_objectid, b_offset;

	a_objectid = letoh64(a->objectid);
	b_objectid = letoh64(b->objectid);
	if (a_objectid < b_objectid)
		return (-1);
	if (a_objectid > b_objectid)
		return (1);
	if (a->type < b->type)
		return (-1);
	if (a->type > b->type)
		return (1);
	a_offset = letoh64(a->offset);
	b_offset = letoh64(b->offset);
	if (a_offset < b_offset)
		return (-1);
	if (a_offset > b_offset)
		return (1);
	return (0);
}
