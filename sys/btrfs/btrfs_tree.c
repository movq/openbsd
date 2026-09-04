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
#include <sys/vnode.h>

#include <lib/libkern/crc32c.h>

#include <btrfs/btrfs_var.h>

static int	btrfs_map_logical(const struct btrfs_chunk_map *, uint64_t,
		    uint32_t, struct btrfs_io_map *);
static const struct btrfs_key *
		btrfs_block_key(const struct btrfs_header *, uint32_t);
static int	btrfs_read_child(struct btrfs_path *, uint8_t, uint32_t,
		    struct btrfs_extent_buffer **);
static int	btrfs_key_cmp(const struct btrfs_key *,
		    const struct btrfs_key *);

static int
btrfs_map_logical(const struct btrfs_chunk_map *chunk, uint64_t logical,
    uint32_t length, struct btrfs_io_map *map)
{
	uint64_t delta;
	unsigned int i;

	if (chunk->length < length || logical < chunk->logical)
		return (ENOENT);
	delta = logical - chunk->logical;
	if (delta > chunk->length - length)
		return (ENOENT);

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

void
btrfs_init_root_tree(struct btrfs_mount *bmp, struct btrfs_root *root)
{
	memset(root, 0, sizeof(*root));
	root->br_mount = bmp;
	root->br_devvp = bmp->bm_devvp;
	root->br_super = &bmp->bm_super;
	root->br_chunks = bmp->bm_chunks;
	root->br_nchunks = bmp->bm_nchunks;
	root->br_bytenr = letoh64(bmp->bm_super.root);
	root->br_generation = letoh64(bmp->bm_super.generation);
	root->br_view_generation = letoh64(bmp->bm_super.generation);
	root->br_owner = BTRFS_ROOT_TREE_OBJECTID;
	root->br_level = bmp->bm_super.root_level;
}

int
btrfs_init_fs_root(struct btrfs_mount *bmp, uint64_t treeid,
    struct btrfs_root *root)
{
	struct btrfs_root_item item;
	struct btrfs_root root_tree;
	int error;

	memset(root, 0, sizeof(*root));
	root->br_mount = bmp;
	root->br_devvp = bmp->bm_devvp;
	root->br_super = &bmp->bm_super;
	root->br_chunks = bmp->bm_chunks;
	root->br_nchunks = bmp->bm_nchunks;
	root->br_view_generation = letoh64(bmp->bm_super.generation);
	root->br_owner = treeid;

	if (treeid == bmp->bm_treeid) {
		root->br_bytenr = bmp->bm_fs_root;
		root->br_generation = bmp->bm_fs_root_generation;
		root->br_level = bmp->bm_fs_root_level;
		return (0);
	}

	btrfs_init_root_tree(bmp, &root_tree);
	error = btrfs_find_root_item(&root_tree, treeid,
	    BTRFS_FIRST_FREE_OBJECTID, &item);
	if (error != 0)
		return (error);
	root->br_bytenr = letoh64(item.bytenr);
	root->br_generation = letoh64(item.generation);
	root->br_level = item.level;
	return (0);
}

void
btrfs_init_csum_root(struct btrfs_mount *bmp, struct btrfs_root *root)
{
	memset(root, 0, sizeof(*root));
	root->br_mount = bmp;
	root->br_devvp = bmp->bm_devvp;
	root->br_super = &bmp->bm_super;
	root->br_chunks = bmp->bm_chunks;
	root->br_nchunks = bmp->bm_nchunks;
	root->br_bytenr = bmp->bm_csum_root;
	root->br_generation = bmp->bm_csum_root_generation;
	root->br_view_generation = letoh64(bmp->bm_super.generation);
	root->br_owner = BTRFS_CSUM_TREE_OBJECTID;
	root->br_level = bmp->bm_csum_root_level;
}

int
btrfs_init_special_root(struct btrfs_mount *bmp, uint64_t owner,
    struct btrfs_root *root)
{
	const struct btrfs_root_location *location;

	location = NULL;
	switch (owner) {
	case BTRFS_ROOT_TREE_OBJECTID:
		btrfs_init_root_tree(bmp, root);
		return (0);
	case BTRFS_CHUNK_TREE_OBJECTID:
		memset(root, 0, sizeof(*root));
		root->br_bytenr = letoh64(bmp->bm_super.chunk_root);
		root->br_generation =
		    letoh64(bmp->bm_super.chunk_root_generation);
		root->br_level = bmp->bm_super.chunk_root_level;
		break;
	case BTRFS_CSUM_TREE_OBJECTID:
		btrfs_init_csum_root(bmp, root);
		return (0);
	case BTRFS_EXTENT_TREE_OBJECTID:
		location = &bmp->bm_extent_root;
		break;
	case BTRFS_DEV_TREE_OBJECTID:
		location = &bmp->bm_dev_root;
		break;
	case BTRFS_FREE_SPACE_TREE_OBJECTID:
		location = &bmp->bm_free_space_root;
		break;
	case BTRFS_BLOCK_GROUP_TREE_OBJECTID:
		location = &bmp->bm_block_group_root;
		break;
	default:
		return (EINVAL);
	}

	if (owner != BTRFS_CHUNK_TREE_OBJECTID) {
		if (location->brl_bytenr == 0)
			return (ENOENT);
		memset(root, 0, sizeof(*root));
		root->br_bytenr = location->brl_bytenr;
		root->br_generation = location->brl_generation;
		root->br_level = location->brl_level;
	}
	root->br_mount = bmp;
	root->br_devvp = bmp->bm_devvp;
	root->br_super = &bmp->bm_super;
	root->br_chunks = bmp->bm_chunks;
	root->br_nchunks = bmp->bm_nchunks;
	root->br_view_generation = letoh64(bmp->bm_super.generation);
	root->br_owner = owner;
	return (0);
}

static const struct btrfs_key *
btrfs_block_key(const struct btrfs_header *header, uint32_t slot)
{
	const struct btrfs_item *items;
	const struct btrfs_key_ptr *ptrs;

	if (header->level == 0) {
		items = (const struct btrfs_item *)(header + 1);
		return (&items[slot].key);
	}
	ptrs = (const struct btrfs_key_ptr *)(header + 1);
	return (&ptrs[slot].key);
}

static int
btrfs_read_child(struct btrfs_path *path, uint8_t parent_level,
    uint32_t slot, struct btrfs_extent_buffer **ebp)
{
	const struct btrfs_header *child, *parent;
	const struct btrfs_header *ancestor;
	const struct btrfs_key_ptr *ancestor_ptrs, *ptrs;
	const struct btrfs_key *first, *last, *upper = NULL;
	uint32_t ancestor_slot, nritems;
	uint8_t level;
	int error;

	parent = btrfs_extent_buffer_data(path->bp_eb[parent_level]);
	ptrs = (const struct btrfs_key_ptr *)(parent + 1);
	error = btrfs_extent_buffer_read(path->bp_root,
	    letoh64(ptrs[slot].blockptr), letoh64(ptrs[slot].generation),
	    path->bp_root->br_view_generation, parent_level - 1, ebp);
	if (error != 0)
		return (error);

	child = btrfs_extent_buffer_data(*ebp);
	nritems = letoh32(child->nritems);
	first = btrfs_block_key(child, 0);
	last = btrfs_block_key(child, nritems - 1);
	if (slot + 1 < letoh32(parent->nritems))
		upper = &ptrs[slot + 1].key;
	for (level = parent_level + 1;
	    upper == NULL && level <= path->bp_root->br_level; level++) {
		ancestor = btrfs_extent_buffer_data(path->bp_eb[level]);
		ancestor_slot = path->bp_slot[level];
		if (ancestor_slot + 1 >= letoh32(ancestor->nritems))
			continue;
		ancestor_ptrs =
		    (const struct btrfs_key_ptr *)(ancestor + 1);
		upper = &ancestor_ptrs[ancestor_slot + 1].key;
	}
	if (btrfs_key_cmp(first, &ptrs[slot].key) != 0 ||
	    (upper != NULL && btrfs_key_cmp(last, upper) >= 0)) {
		btrfs_extent_buffer_put(*ebp);
		*ebp = NULL;
		return (EINVAL);
	}
	return (0);
}

void
btrfs_release_path(struct btrfs_path *path)
{
	unsigned int level;

	for (level = 0; level < BTRFS_MAX_LEVEL; level++) {
		if (path->bp_eb[level] != NULL) {
#ifdef DIAGNOSTIC
			KASSERT(path->bp_root != NULL);
			KASSERT(path->bp_eb[level]->eb_level == level);
#endif
			btrfs_extent_buffer_put(path->bp_eb[level]);
			path->bp_eb[level] = NULL;
		}
		path->bp_slot[level] = 0;
	}
	path->bp_root = NULL;
#ifdef DIAGNOSTIC
	for (level = 0; level < BTRFS_MAX_LEVEL; level++)
		KASSERT(path->bp_eb[level] == NULL);
#endif
}

int
btrfs_search_slot(struct btrfs_root *root, const struct btrfs_key *target,
    struct btrfs_path *path)
{
	const struct btrfs_header *header;
	const struct btrfs_key_ptr *ptrs;
	const struct btrfs_item *items;
	uint32_t high, low, mid, slot;
	uint8_t level;
	int cmp, error;

	if (path->bp_root != NULL)
		btrfs_release_path(path);
	if (root->br_level >= BTRFS_MAX_LEVEL)
		return (EINVAL);
	path->bp_root = root;
	error = btrfs_extent_buffer_read(root, root->br_bytenr,
	    root->br_generation, root->br_view_generation, root->br_level,
	    &path->bp_eb[root->br_level]);
	if (error != 0)
		goto fail;

	for (level = root->br_level; level != 0; level--) {
		header = btrfs_extent_buffer_data(path->bp_eb[level]);
		ptrs = (const struct btrfs_key_ptr *)(header + 1);
		low = 0;
		high = letoh32(header->nritems);
		while (low < high) {
			mid = low + (high - low) / 2;
			if (btrfs_key_cmp(&ptrs[mid].key, target) <= 0)
				low = mid + 1;
			else
				high = mid;
		}
		slot = low == 0 ? 0 : low - 1;
		path->bp_slot[level] = slot;
		error = btrfs_read_child(path, level, slot,
		    &path->bp_eb[level - 1]);
		if (error != 0)
			goto fail;
	}

	header = btrfs_extent_buffer_data(path->bp_eb[0]);
	items = (const struct btrfs_item *)(header + 1);
	low = 0;
	high = letoh32(header->nritems);
	while (low < high) {
		mid = low + (high - low) / 2;
		if (btrfs_key_cmp(&items[mid].key, target) < 0)
			low = mid + 1;
		else
			high = mid;
	}
	path->bp_slot[0] = low;
	if (low < letoh32(header->nritems)) {
		cmp = btrfs_key_cmp(&items[low].key, target);
		if (cmp == 0)
			return (0);
	}
	return (ENOENT);

fail:
	btrfs_release_path(path);
	return (error);
}

int
btrfs_path_item(const struct btrfs_path *path,
    const struct btrfs_key **keyp, const uint8_t **datap, uint32_t *sizep)
{
	const struct btrfs_header *header;
	const struct btrfs_item *items;
	uint32_t offset, slot;

	if (keyp != NULL)
		*keyp = NULL;
	if (datap != NULL)
		*datap = NULL;
	if (sizep != NULL)
		*sizep = 0;
	if (path->bp_eb[0] == NULL)
		return (ENOENT);
	header = btrfs_extent_buffer_data(path->bp_eb[0]);
	slot = path->bp_slot[0];
	if (slot >= letoh32(header->nritems))
		return (ENOENT);
	items = (const struct btrfs_item *)(header + 1);
	offset = letoh32(items[slot].offset);
	if (keyp != NULL)
		*keyp = &items[slot].key;
	if (datap != NULL)
		*datap = (const uint8_t *)(header + 1) + offset;
	if (sizep != NULL)
		*sizep = letoh32(items[slot].size);
	return (0);
}

int
btrfs_next_item(struct btrfs_path *path)
{
	const struct btrfs_header *header;
	uint32_t nritems;
	uint8_t level, child_level;
	int error;

	if (path->bp_root == NULL || path->bp_eb[0] == NULL)
		return (ENOENT);
	header = btrfs_extent_buffer_data(path->bp_eb[0]);
	nritems = letoh32(header->nritems);
	if (path->bp_slot[0] < nritems &&
	    path->bp_slot[0] + 1 < nritems) {
		path->bp_slot[0]++;
		return (0);
	}

	for (level = 1; level <= path->bp_root->br_level; level++) {
		header = btrfs_extent_buffer_data(path->bp_eb[level]);
		if (path->bp_slot[level] + 1 >=
		    letoh32(header->nritems))
			continue;
		path->bp_slot[level]++;
		for (child_level = level; child_level != 0; child_level--) {
			if (path->bp_eb[child_level - 1] != NULL) {
				btrfs_extent_buffer_put(
				    path->bp_eb[child_level - 1]);
				path->bp_eb[child_level - 1] = NULL;
			}
			error = btrfs_read_child(path, child_level,
			    path->bp_slot[child_level],
			    &path->bp_eb[child_level - 1]);
			if (error != 0) {
				btrfs_release_path(path);
				return (error);
			}
			path->bp_slot[child_level - 1] = 0;
		}
		return (0);
	}

	path->bp_slot[0] = nritems;
	return (ENOENT);
}

int
btrfs_prev_item(struct btrfs_path *path)
{
	const struct btrfs_header *header;
	uint8_t level, child_level;
	int error;

	if (path->bp_root == NULL || path->bp_eb[0] == NULL)
		return (ENOENT);
	if (path->bp_slot[0] != 0) {
		path->bp_slot[0]--;
		return (0);
	}

	for (level = 1; level <= path->bp_root->br_level; level++) {
		if (path->bp_slot[level] == 0)
			continue;
		path->bp_slot[level]--;
		for (child_level = level; child_level != 0; child_level--) {
			if (path->bp_eb[child_level - 1] != NULL) {
				btrfs_extent_buffer_put(
				    path->bp_eb[child_level - 1]);
				path->bp_eb[child_level - 1] = NULL;
			}
			error = btrfs_read_child(path, child_level,
			    path->bp_slot[child_level],
			    &path->bp_eb[child_level - 1]);
			if (error != 0) {
				btrfs_release_path(path);
				return (error);
			}
			header = btrfs_extent_buffer_data(
			    path->bp_eb[child_level - 1]);
			path->bp_slot[child_level - 1] =
			    letoh32(header->nritems) - 1;
		}
		return (0);
	}

	return (ENOENT);
}

int
btrfs_search_lower_bound(struct btrfs_root *root,
    const struct btrfs_key *target, struct btrfs_path *path)
{
	int error;

	error = btrfs_search_slot(root, target, path);
	if (error == 0)
		return (0);
	if (error != ENOENT)
		return (error);
	error = btrfs_path_item(path, NULL, NULL, NULL);
	if (error == 0)
		return (0);
	return (btrfs_next_item(path));
}

int
btrfs_search_predecessor(struct btrfs_root *root,
    const struct btrfs_key *target, struct btrfs_path *path)
{
	int error;

	error = btrfs_search_slot(root, target, path);
	if (error == 0)
		return (0);
	if (error != ENOENT)
		return (error);
	return (btrfs_prev_item(path));
}

int
btrfs_read_data_csums(struct btrfs_mount *bmp, uint64_t logical,
    uint64_t length, uint32_t *csums)
{
	const struct btrfs_key *key;
	const uint8_t *data;
	struct btrfs_path path = { 0 };
	struct btrfs_root root;
	struct btrfs_key target;
	uint64_t cursor, end, item_end, span, start;
	uint32_t csum, item_size, sectorsize;
	size_t csum_index, csum_offset, navailable, ncopy, i;
	int first = 1;
	int error;

	sectorsize = letoh32(bmp->bm_super.sectorsize);
	if ((logical & (sectorsize - 1)) != 0 ||
	    (length & (sectorsize - 1)) != 0)
		return (EINVAL);
	if (length == 0)
		return (0);
	if (csums == NULL)
		return (EINVAL);
	if (logical > UINT64_MAX - length)
		return (EINVAL);
	end = logical + length;

	memset(&target, 0, sizeof(target));
	target.objectid = htole64(BTRFS_EXTENT_CSUM_OBJECTID);
	target.type = BTRFS_EXTENT_CSUM_KEY;
	target.offset = htole64(logical);
	btrfs_init_csum_root(bmp, &root);
	error = btrfs_search_predecessor(&root, &target, &path);
	if (error == ENOENT)
		error = btrfs_search_lower_bound(&root, &target, &path);
	if (error != 0)
		goto out;

	cursor = logical;
	csum_index = 0;
	while (cursor < end) {
		error = btrfs_path_item(&path, &key, &data, &item_size);
		if (error != 0)
			goto out;
		if (letoh64(key->objectid) != BTRFS_EXTENT_CSUM_OBJECTID ||
		    key->type != BTRFS_EXTENT_CSUM_KEY) {
			error = ENOENT;
			goto out;
		}

		start = letoh64(key->offset);
		if ((start & (sectorsize - 1)) != 0 || item_size == 0 ||
		    item_size % sizeof(csum) != 0) {
			error = EINVAL;
			goto out;
		}
		span = (uint64_t)(item_size / sizeof(csum)) * sectorsize;
		if (start > UINT64_MAX - span) {
			error = EINVAL;
			goto out;
		}
		item_end = start + span;
		if (cursor >= item_end) {
			first = 0;
			error = btrfs_next_item(&path);
			if (error != 0)
				goto out;
			continue;
		}
		if ((!first && start != cursor) || cursor < start) {
			error = start > cursor ? ENOENT : EINVAL;
			goto out;
		}

		csum_offset = (cursor - start) / sectorsize;
		navailable = item_size / sizeof(csum) - csum_offset;
		ncopy = MIN(navailable, (size_t)((end - cursor) / sectorsize));
		for (i = 0; i < ncopy; i++) {
			memcpy(&csum, data +
			    (csum_offset + i) * sizeof(csum), sizeof(csum));
			csums[csum_index + i] = letoh32(csum);
		}
		csum_index += ncopy;
		cursor += (uint64_t)ncopy * sectorsize;
		if (cursor == end)
			break;
		first = 0;
		error = btrfs_next_item(&path);
		if (error != 0)
			goto out;
	}
	error = 0;
out:
	btrfs_release_path(&path);
	return (error);
}

int
btrfs_lookup_data_csum(struct btrfs_mount *bmp, uint64_t logical,
    uint32_t *csump)
{
	return (btrfs_read_data_csums(bmp, logical,
	    letoh32(bmp->bm_super.sectorsize), csump));
}

int
btrfs_read_data_block(struct btrfs_mount *bmp, uint64_t logical,
    const uint32_t *expected_csum, struct buf **bpp)
{
	struct btrfs_io_map map;
	struct buf *bp;
	uint32_t actual_csum;
	uint32_t sectorsize;
	unsigned int i;
	int error = EIO;

	*bpp = NULL;
	sectorsize = letoh32(bmp->bm_super.sectorsize);
	if ((logical & (sectorsize - 1)) != 0)
		return (EINVAL);
	error = btrfs_lookup_logical(bmp->bm_chunks, bmp->bm_nchunks,
	    logical, sectorsize, &map);
	if (error != 0)
		return (error == ENOENT ? EINVAL : error);
	if ((map.type & BTRFS_BLOCK_GROUP_DATA) == 0)
		return (EINVAL);

	for (i = 0; i < map.nmirrors; i++) {
		bp = NULL;
		error = bread(bmp->bm_devvp, map.physical[i] / DEV_BSIZE,
		    sectorsize, &bp);
		if (error == 0 && bp->b_resid == 0) {
			if (expected_csum == NULL) {
				*bpp = bp;
				return (0);
			}
			actual_csum = crc32c(0, bp->b_data, sectorsize);
			if (actual_csum == *expected_csum) {
				*bpp = bp;
				return (0);
			}
			error = EIO;
		}
		if (bp != NULL)
			brelse(bp);
		if (error == 0)
			error = EIO;
	}

	return (error);
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
