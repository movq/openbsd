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

/*
 * Publication owns the transaction close/drain gate. Replace the target
 * inode's key range by copying affected paths in the previous immutable log.
 * Unaffected subtrees retain their disk addresses; no previous inode needs
 * recollection. All old log blocks remain excluded until full commit.
 * The superblock alone names the published forest, at the open generation.
 *
 * Submitted data leaves the ordered index with materialized references.
 * Subsequent writes COW it normally. Creation can log fresh ancestry; other
 * namespace mutations still force full commit.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/vnode.h>

#include <btrfs/btrfs_var.h>
#include <btrfs/btrfs_data.h>
#include <btrfs/btrfs_log.h>

struct log_block {
	TAILQ_ENTRY(log_block) entry;
	struct btrfs_key first;
	uint64_t bytenr;
	uint8_t level;
};
TAILQ_HEAD(log_blocks, log_block);

int
btrfs_log_key_compare(const struct btrfs_key *a, const struct btrfs_key *b)
{
	if (a->objectid != b->objectid)
		return (letoh64(a->objectid) < letoh64(b->objectid) ? -1 : 1);
	if (a->type != b->type)
		return (a->type < b->type ? -1 : 1);
	if (a->offset != b->offset)
		return (letoh64(a->offset) < letoh64(b->offset) ? -1 : 1);
	return (0);
}

static int
log_compare(const struct log_item *a, const struct log_item *b)
{
	return (btrfs_log_key_compare(&a->key, &b->key));
}
RBT_GENERATE(log_items, log_item, entry, log_compare);

struct log_item *
btrfs_log_find(struct log_items *items, uint64_t ino, uint8_t type,
    uint64_t offset)
{
	struct log_item match;

	memset(&match, 0, sizeof(match));
	match.key.objectid = htole64(ino);
	match.key.type = type;
	match.key.offset = htole64(offset);
	return (RBT_FIND(log_items, items, &match));
}

int
btrfs_log_add(struct log_items *items, const struct btrfs_key *key,
    const void *data, uint32_t size)
{
	struct log_item *item, *old;

	item = malloc(sizeof(*item) + size, M_BTRFS, M_WAITOK | M_ZERO);
	item->key = *key;
	item->size = size;
	if (size != 0)
		memcpy(item->data, data, size);
	old = RBT_INSERT(log_items, items, item);
	if (old != NULL) {
		int same = old->size == size &&
		    (size == 0 || memcmp(old->data, data, size) == 0);
		free(item, M_BTRFS, sizeof(*item) + size);
		return (same ? 0 : EINVAL);
	}
	return (0);
}

void
btrfs_log_free_items(struct log_items *items)
{
	struct log_item *item;

	while ((item = RBT_ROOT(log_items, items)) != NULL) {
		RBT_REMOVE(log_items, items, item);
		free(item, M_BTRFS, sizeof(*item) + item->size);
	}
}

void
btrfs_log_destroy(struct btrfs_fs *bmp)
{
	btrfs_space_log_release(bmp);
}

/*
 * Collect bounded contiguous ranges, skipping shared/compressed overlaps.
 * Leave spare capacity in the last record so adjacent file mappings append
 * without allocating one checksum item per mapping (or per sector).
 */
static int
log_collect_csums(struct btrfs_fs *bmp, struct log_items *items,
    const struct btrfs_file_extent *extent)
{
	struct log_item match = { 0 }, *item, *next;
	uint8_t *csums;
	uint64_t start, end, item_end, stop;
	uint32_t sector = letoh32(bmp->bm_super.sectorsize);
	uint32_t capacity = letoh32(bmp->bm_super.nodesize) / 4;
	uint32_t bytes, prefix;
	size_t csum = btrfs_csum_size(&bmp->bm_super);
	int error = 0;

	start = extent->bfe_disk_bytenr;
	if (start == 0)
		return (0);
	if (extent->bfe_compression != 0)
		end = start + extent->bfe_disk_num_bytes;
	else {
		start += extent->bfe_disk_offset;
		end = start + extent->bfe_length;
	}
	match.key.objectid = htole64(BTRFS_EXTENT_CSUM_OBJECTID);
	match.key.type = BTRFS_EXTENT_CSUM_KEY;
	csums = malloc(capacity, M_BTRFS, M_WAITOK);
	while (start < end) {
		match.key.offset = htole64(start);
		next = RBT_NFIND(log_items, items, &match);
		item = next == NULL ? RBT_MAX(log_items, items) :
		    RBT_PREV(log_items, next);
		prefix = 0;
		if (item != NULL) {
			item_end = letoh64(item->key.offset) +
			    item->size / csum * sector;
			if (start < item_end) {
				start = item_end;
				continue;
			}
			if (start == item_end && item->size < capacity) {
				prefix = item->size;
				memcpy(csums, item->data, prefix);
			}
		}
		if (next != NULL && next->key.offset == match.key.offset) {
			start += next->size / csum * sector;
			continue;
		}
		stop = start + MIN(end - start,
		    (capacity - prefix) / csum * sector);
		if (next != NULL)
			stop = MIN(stop, letoh64(next->key.offset));
		bytes = (stop - start) / sector * csum;
		error = btrfs_read_data_csums(bmp, start, stop - start,
		    csums + prefix);
		if (error != 0)
			break;
		if (prefix != 0) {
			match.key = item->key;
			RBT_REMOVE(log_items, items, item);
			free(item, M_BTRFS, sizeof(*item) + item->size);
		}
		error = btrfs_log_add(items, &match.key, csums, prefix + bytes);
		if (error != 0)
			break;
		start = stop;
	}
	free(csums, M_BTRFS, capacity);
	return (error);
}

static int
log_hole(struct log_items *items, uint64_t ino, uint64_t start,
    uint64_t end, uint64_t generation)
{
	struct btrfs_file_extent_item extent = { 0 };
	struct btrfs_key key = { 0 };

	if (start >= end)
		return (0);
	key.objectid = htole64(ino);
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = htole64(start);
	extent.generation = htole64(generation);
	extent.ram_bytes = extent.num_bytes = htole64(end - start);
	extent.type = BTRFS_FILE_EXTENT_REG;
	return (btrfs_log_add(items, &key, &extent, sizeof(extent)));
}

static int
log_collect_inode(struct btrfs_root *root, struct log_items *items,
    struct log_items *csums, uint64_t ino, uint64_t *parent)
{
	struct btrfs_fs *bmp = root->br_mount;
	struct btrfs_path path = { 0 };
	struct btrfs_key target = { 0 };
	const struct btrfs_key *key;
	const uint8_t *data;
	struct btrfs_inode inode;
	struct btrfs_file_extent extent;
	uint64_t cursor = 0, end;
	uint64_t generation = bmp->bm_transaction->bt_generation;
	uint32_t size, sector = letoh32(bmp->bm_super.sectorsize);
	int error, inline_data = 0;

	error = btrfs_find_inode(root, ino, &inode);
	if (error != 0)
		return (error);
	if ((!S_ISREG(inode.bi_mode) && !S_ISDIR(inode.bi_mode)) ||
	    inode.bi_nlink == 0)
		return (EAGAIN);
	*parent = 0;
	end = roundup(inode.bi_size, sector);
	target.objectid = htole64(ino);
	error = btrfs_search_lower_bound(root, &target, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &key, &data, &size);
		if (error != 0 || key->objectid != target.objectid)
			break;
		/* Ancestry logging does not enumerate a directory's children. */
		if (S_ISDIR(inode.bi_mode) && key->type > BTRFS_XATTR_ITEM_KEY)
			break;
		if (key->type == BTRFS_INODE_ITEM_KEY ||
		    key->type == BTRFS_XATTR_ITEM_KEY ||
		    key->type == BTRFS_EXTENT_DATA_KEY ||
		    (inode.bi_generation > bmp->bm_last_transid &&
		    key->type == BTRFS_INODE_REF_KEY)) {
			error = btrfs_log_add(items, key, data, size);
			if (error != 0)
				break;
		}
		if (inode.bi_generation > bmp->bm_last_transid &&
		    key->type == BTRFS_INODE_REF_KEY) {
			/* Creation has one parent; link/rename force a commit. */
			if (*parent != 0) {
				error = EAGAIN;
				break;
			}
			*parent = letoh64(key->offset);
		}
		if (key->type == BTRFS_EXTENT_DATA_KEY) {
			error = btrfs_decode_file_extent(bmp, generation, key,
			    data, size, &extent);
			if (error != 0)
				break;
			inline_data =
			    extent.bfe_type == BTRFS_FILE_EXTENT_INLINE;
			if (extent.bfe_logical < cursor) {
				error = EINVAL;
				break;
			}
			error = log_hole(items, ino, cursor, extent.bfe_logical,
			    generation);
			cursor = extent.bfe_logical + extent.bfe_length;
			if (error == 0 &&
			    extent.bfe_type == BTRFS_FILE_EXTENT_REG &&
			    !(inode.bi_flags & BTRFS_INODE_NODATASUM))
				error = log_collect_csums(bmp, csums, &extent);
			if (error != 0)
				break;
		}
		error = btrfs_next_item(&path);
	}
	btrfs_release_path(&path);
	if (error == ENOENT)
		error = 0;
	if (error == 0 && !inline_data && S_ISREG(inode.bi_mode))
		error = log_hole(items, ino, cursor, end, generation);
	if (error == 0 && inode.bi_generation > bmp->bm_last_transid &&
	    *parent == 0)
		error = EAGAIN;
	/*
	 * Existing ancestry needs only an existence record. New directories
	 * carry xattrs and their own parent reference, but no directory ranges:
	 * replay installs only the child's logged name.
	 */
	if (error == 0 && S_ISDIR(inode.bi_mode) &&
	    inode.bi_generation <= bmp->bm_last_transid) {
		struct log_item *item = btrfs_log_find(items, ino,
		    BTRFS_INODE_ITEM_KEY, 0);
		((struct btrfs_inode_item *)item->data)->generation = 0;
	}
	return (error);
}

static int
log_write_block(struct btrfs_fs *bmp, void *buffer, uint8_t level,
    uint32_t count, const struct btrfs_key *first, struct log_blocks *blocks,
    struct btrfs_write_batch *batch)
{
	struct btrfs_header *header = buffer;
	struct log_block *block;
	uint64_t bytenr;
	uint32_t nodesize = letoh32(bmp->bm_super.nodesize);
	int error;

	error = btrfs_space_log_alloc(bmp, &bytenr);
	/* Only log-space exhaustion is a safe full-commit fallback. */
	if (error != 0)
		return (error == ENOSPC ? EAGAIN : error);
	memset(header, 0, sizeof(*header));
	memcpy(header->fsid, bmp->bm_super.fsid, BTRFS_UUID_SIZE);
	memcpy(header->chunk_tree_uuid, bmp->bm_chunk_tree_uuid,
	    BTRFS_UUID_SIZE);
	header->bytenr = htole64(bytenr);
	header->flags = htole64(BTRFS_HEADER_FLAG_WRITTEN |
	    (BTRFS_MIXED_BACKREF_REV << BTRFS_BACKREF_REV_SHIFT));
	header->generation = htole64(bmp->bm_transaction->bt_generation);
	header->owner = htole64(BTRFS_TREE_LOG_OBJECTID);
	header->level = level;
	header->nritems = htole32(count);
	btrfs_csum(&bmp->bm_super, (uint8_t *)header + sizeof(header->csum),
	    nodesize - sizeof(header->csum), header->csum);
	error = btrfs_write_logical(bmp, bytenr, nodesize,
	    BTRFS_BLOCK_GROUP_METADATA, buffer, batch);
	if (error != 0)
		return (error);
	block = malloc(sizeof(*block), M_BTRFS, M_WAITOK | M_ZERO);
	block->bytenr = bytenr;
	block->first = *first;
	block->level = level;
	TAILQ_INSERT_TAIL(blocks, block, entry);
	return (0);
}

static void
log_free_blocks(struct log_blocks *blocks)
{
	struct log_block *block;

	while ((block = TAILQ_FIRST(blocks)) != NULL) {
		TAILQ_REMOVE(blocks, block, entry);
		free(block, M_BTRFS, sizeof(*block));
	}
}

/* Packing never enters normal extent-reference accounting. */
static int
log_pack_leaves(struct btrfs_fs *bmp, struct log_items *items,
    struct log_blocks *blocks, struct btrfs_write_batch *batch)
{
	struct log_item *item;
	struct btrfs_header *header;
	struct btrfs_item *disk;
	struct btrfs_key first = { 0 };
	uint32_t nodesize = letoh32(bmp->bm_super.nodesize);
	uint32_t capacity = nodesize - sizeof(*header), count = 0, tail;
	int error = 0;

	header = malloc(nodesize, M_BTRFS, M_WAITOK | M_ZERO);
	disk = (struct btrfs_item *)(header + 1);
	tail = capacity;
	RBT_FOREACH(item, log_items, items) {
		if (item->size + sizeof(*disk) > capacity) {
			error = EINVAL;
			goto out;
		}
		if ((count + 1) * sizeof(*disk) + item->size > tail) {
			error = log_write_block(bmp, header, 0, count, &first,
			    blocks, batch);
			if (error != 0)
				goto out;
			memset(header, 0, nodesize);
			count = 0;
			tail = capacity;
		}
		if (count == 0)
			first = item->key;
		tail -= item->size;
		disk[count].key = item->key;
		disk[count].offset = htole32(tail);
		disk[count++].size = htole32(item->size);
		memcpy((uint8_t *)(header + 1) + tail, item->data, item->size);
	}
	if (count != 0)
		error = log_write_block(bmp, header, 0, count, &first,
		    blocks, batch);
out:
	free(header, M_BTRFS, nodesize);
	return (error);
}

static int
log_pack_nodes(struct btrfs_fs *bmp, struct log_blocks *children,
    struct log_blocks *parents, uint8_t level, struct btrfs_write_batch *batch)
{
	struct btrfs_header *header;
	struct btrfs_key_ptr *ptr;
	struct log_block *block;
	struct btrfs_key first = { 0 };
	uint32_t nodesize = letoh32(bmp->bm_super.nodesize);
	uint32_t capacity = (nodesize - sizeof(*header)) / sizeof(*ptr);
	uint32_t count = 0;
	int error = 0;

	if (level >= BTRFS_MAX_LEVEL)
		return (EOVERFLOW);
	header = malloc(nodesize, M_BTRFS, M_WAITOK | M_ZERO);
	ptr = (struct btrfs_key_ptr *)(header + 1);
	while ((block = TAILQ_FIRST(children)) != NULL) {
		KASSERT(block->level + 1 == level);
		if (count == 0)
			first = block->first;
		ptr[count].key = block->first;
		ptr[count].blockptr = htole64(block->bytenr);
		ptr[count++].generation =
		    htole64(bmp->bm_transaction->bt_generation);
		TAILQ_REMOVE(children, block, entry);
		free(block, M_BTRFS, sizeof(*block));
		if (count == capacity || TAILQ_EMPTY(children)) {
			error = log_write_block(bmp, header, level, count,
			    &first, parents, batch);
			if (error != 0)
				break;
			count = 0;
			memset(header, 0, nodesize);
		}
	}
	free(header, M_BTRFS, nodesize);
	return (error);
}

struct log_edit {
	struct btrfs_root root;
	struct btrfs_key low, high;
	struct log_items *items;
	/* Checksum replacement preserves prefix/suffix of intersecting items. */
	uint64_t csum_start, csum_end;
	struct btrfs_write_batch batch;
};

static void
log_root(struct btrfs_fs *bmp, struct btrfs_root *root,
    const struct btrfs_root_item *item)
{
	memset(root, 0, sizeof(*root));
	root->br_mount = bmp;
	root->br_super = &bmp->bm_super;
	root->br_owner = BTRFS_TREE_LOG_OBJECTID;
	root->br_generation = root->br_view_generation =
	    bmp->bm_transaction->bt_generation;
	root->br_bytenr = letoh64(item->bytenr);
	root->br_level = item->level;
}

static int
log_keep_item(struct log_edit *edit, struct log_items *items,
    const struct btrfs_key *key, const uint8_t *data, uint32_t size)
{
	struct btrfs_fs *bmp = edit->root.br_mount;
	struct btrfs_key suffix = *key;
	uint64_t start, end;
	uint32_t bytes, sector = letoh32(bmp->bm_super.sectorsize);
	size_t csum = btrfs_csum_size(&bmp->bm_super);
	int error = 0;

	if (btrfs_log_key_compare(key, &edit->low) < 0 ||
	    btrfs_log_key_compare(key, &edit->high) >= 0)
		return (btrfs_log_add(items, key, data, size));
	if (edit->csum_end == 0)
		return (0);
	start = letoh64(key->offset);
	end = start + size / csum * sector;
	if (end <= edit->csum_start)
		return (btrfs_log_add(items, key, data, size));
	if (start < edit->csum_start) {
		bytes = (edit->csum_start - start) / sector * csum;
		error = btrfs_log_add(items, key, data, bytes);
	}
	if (error == 0 && end > edit->csum_end) {
		bytes = (edit->csum_end - start) / sector * csum;
		suffix.offset = htole64(edit->csum_end);
		error = btrfs_log_add(items, &suffix, data + bytes, size - bytes);
	}
	return (error);
}

/*
 * Return replacement blocks at this level. Only intersecting paths are read
 * or copied. The first child's lower bound can be -infinity, allowing inserts
 * before the old minimum. Empty children disappear; splits propagate upwards.
 */
static int
log_edit_block(struct log_edit *edit, uint64_t bytenr, uint8_t level,
    const struct btrfs_key *first, const struct btrfs_key *low,
    const struct btrfs_key *high, struct log_blocks *result)
{
	struct btrfs_fs *bmp = edit->root.br_mount;
	struct btrfs_extent_buffer *eb = NULL;
	const struct btrfs_header *header;
	const struct btrfs_key_ptr *ptr;
	const struct btrfs_item *disk;
	struct log_items items;
	struct log_blocks children;
	struct log_item *item;
	struct log_block *block;
	uint32_t i, count;
	int error = 0;

	if (bytenr != 0 &&
	    ((low != NULL && btrfs_log_key_compare(low, &edit->high) >= 0) ||
	    (high != NULL && btrfs_log_key_compare(high, &edit->low) <= 0))) {
		block = malloc(sizeof(*block), M_BTRFS, M_WAITOK | M_ZERO);
		block->bytenr = bytenr;
		block->level = level;
		block->first = *first;
		TAILQ_INSERT_TAIL(result, block, entry);
		return (0);
	}
	RBT_INIT(log_items, &items);
	TAILQ_INIT(&children);
	if (bytenr != 0) {
		error = btrfs_extent_buffer_read(&edit->root, bytenr,
		    edit->root.br_generation, edit->root.br_view_generation,
		    level, &eb);
		if (error != 0)
			goto out;
		header = btrfs_extent_buffer_data(eb);
		count = letoh32(header->nritems);
		ptr = (const struct btrfs_key_ptr *)(header + 1);
		disk = (const struct btrfs_item *)(header + 1);
		for (i = 0; i < count && error == 0; i++) {
			if (level != 0)
				error = log_edit_block(edit,
				    letoh64(ptr[i].blockptr), level - 1,
				    &ptr[i].key, i == 0 ? low : &ptr[i].key,
				    i + 1 < count ? &ptr[i + 1].key : high,
				    &children);
			else
				error = log_keep_item(edit, &items, &disk[i].key,
				    (const uint8_t *)(header + 1) +
				    letoh32(disk[i].offset),
				    letoh32(disk[i].size));
		}
		if (error != 0)
			goto out;
	}
	if (level != 0)
		error = log_pack_nodes(bmp, &children, result, level, &edit->batch);
	else {
		while ((item = RBT_MIN(log_items, edit->items)) != NULL &&
		    (high == NULL ||
		    btrfs_log_key_compare(&item->key, high) < 0)) {
			RBT_REMOVE(log_items, edit->items, item);
			if (RBT_INSERT(log_items, &items, item) != NULL) {
				free(item, M_BTRFS, sizeof(*item) + item->size);
				error = EINVAL;
				goto out;
			}
		}
		error = log_pack_leaves(bmp, &items, result, &edit->batch);
	}
out:
	if (eb != NULL)
		btrfs_extent_buffer_put(eb);
	btrfs_log_free_items(&items);
	log_free_blocks(&children);
	return (error);
}

static int
log_update(struct btrfs_fs *bmp, struct btrfs_root_item *root,
    struct log_items *items, const struct btrfs_key *low,
    const struct btrfs_key *high, int csums)
{
	struct log_edit edit = { 0 };
	struct log_blocks blocks, parents;
	struct log_block *block;
	uint64_t span;
	int error, end_error;

	TAILQ_INIT(&blocks);
	TAILQ_INIT(&parents);
	log_root(bmp, &edit.root, root);
	edit.items = items;
	edit.low = *low;
	edit.high = *high;
	if (csums) {
		edit.csum_start = letoh64(low->offset);
		edit.csum_end = letoh64(high->offset);
		/* Include the predecessor which may straddle the new range. */
		span = letoh32(bmp->bm_super.nodesize) / 4 /
		    btrfs_csum_size(&bmp->bm_super) *
		    letoh32(bmp->bm_super.sectorsize);
		edit.low.offset = htole64(edit.csum_start -
		    MIN(edit.csum_start, span));
	}
	btrfs_write_batch_init(&edit.batch);
	error = log_edit_block(&edit, edit.root.br_bytenr, root->level,
	    NULL, NULL, NULL, &blocks);
	while (error == 0 && !TAILQ_EMPTY(&blocks) &&
	    TAILQ_NEXT(TAILQ_FIRST(&blocks), entry) != NULL) {
		error = log_pack_nodes(bmp, &blocks, &parents,
		    TAILQ_FIRST(&blocks)->level + 1, &edit.batch);
		TAILQ_CONCAT(&blocks, &parents, entry);
	}
	end_error = btrfs_write_batch_wait(&edit.batch);
	if (end_error != 0)
		error = end_error;
	if (error == 0) {
		KASSERT(RBT_EMPTY(log_items, items));
		block = TAILQ_FIRST(&blocks);
		if (block == NULL)
			error = EINVAL;
		else {
			memset(root, 0, sizeof(*root));
			root->bytenr = htole64(block->bytenr);
			root->generation = htole64(edit.root.br_generation);
			root->level = block->level;
		}
	}
	log_free_blocks(&blocks);
	log_free_blocks(&parents);
	return (error);
}

int
btrfs_log_write(struct btrfs_trans_handle *handle, struct btrfs_node *node,
    struct proc *p)
{
	struct btrfs_fs *bmp = node->bn_mount;
	struct log_items items, csums;
	struct log_item *item;
	struct btrfs_root log;
	struct btrfs_root_item forest = { 0 }, subtree = { 0 };
	struct btrfs_path path = { 0 };
	struct btrfs_key key = { 0 }, low = { 0 }, high = { 0 };
	struct btrfs_super_block *super;
	const uint8_t *data;
	uint64_t ino = node->bn_ino, parent;
	uint32_t size;
	unsigned int depth = 0;
	int error, end_error;

	RBT_INIT(log_items, &items);
	RBT_INIT(log_items, &csums);
	error = btrfs_write_inode_ordered(handle, node);
	if (error != 0)
		goto out;
	forest.bytenr = bmp->bm_super.log_root;
	forest.level = bmp->bm_super.log_root_level;
	key.objectid = htole64(BTRFS_TREE_LOG_OBJECTID);
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = htole64(node->bn_treeid);
	if (forest.bytenr != 0) {
		log_root(bmp, &log, &forest);
		error = btrfs_search_slot(&log, &key, &path);
		if (error == 0) {
			error = btrfs_path_item(&path, NULL, &data, &size);
			if (error == 0 && size != sizeof(subtree))
				error = EINVAL;
			if (error == 0)
				memcpy(&subtree, data, size);
		} else if (error == ENOENT)
			error = 0;
		btrfs_release_path(&path);
		if (error != 0)
			goto out;
	}
	do {
		error = log_collect_inode(node->bn_root, &items, &csums,
		    ino, &parent);
		if (error != 0)
			goto out;
		low.objectid = htole64(ino);
		high.objectid = htole64(ino + 1);
		error = log_update(bmp, &subtree, &items, &low, &high, 0);
		if (error != 0)
			goto out;
		ino = parent;
		/* Corrupt ancestry must not loop or exhaust the kernel stack. */
		if (++depth > MAXPATHLEN) {
			error = EAGAIN;
			goto out;
		}
	} while (ino != 0);
	while ((item = RBT_MIN(log_items, &csums)) != NULL) {
		RBT_REMOVE(log_items, &csums, item);
		RBT_INSERT(log_items, &items, item);
		low = high = item->key;
		high.offset = htole64(letoh64(low.offset) +
		    item->size / btrfs_csum_size(&bmp->bm_super) *
		    letoh32(bmp->bm_super.sectorsize));
		error = log_update(bmp, &subtree, &items, &low, &high, 1);
		if (error != 0)
			goto out;
	}
	error = btrfs_log_add(&items, &key, &subtree, sizeof(subtree));
	low = high = key;
	high.offset = htole64(node->bn_treeid + 1);
	if (error == 0)
		error = log_update(bmp, &forest, &items, &low, &high, 0);
	if (error != 0)
		goto out;
	error = btrfs_sync_device(bmp, p);
	if (error != 0)
		goto out;
	super = malloc(sizeof(*super), M_BTRFS, M_WAITOK);
	memcpy(super, &bmp->bm_super, sizeof(*super));
	super->log_root = forest.bytenr;
	super->log_root_level = forest.level;
	super->__unused_log_root_transid = 0;
	error = btrfs_write_super_mirrors(bmp, super);
	end_error = btrfs_sync_device(bmp, p);
	if (error == 0)
		error = end_error;
	if (error == 0)
		memcpy(&bmp->bm_super, super, sizeof(*super));
	free(super, M_BTRFS, sizeof(*super));
out:
	btrfs_log_free_items(&items);
	btrfs_log_free_items(&csums);
	/* Unpublished log blocks remain excluded until the fallback commits. */
	return (error);
}
