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
 * Publication owns the transaction close/drain gate. The writer builds fresh,
 * immutable trees, keeps every older log block excluded until full commit,
 * and never advances the committed filesystem generation. Already submitted
 * ordered payloads cannot be edited: the data path commits before an
 * overlapping write. Namespace mutations force full commit.
 *
 * Between publications retain only the identities of previously logged
 * inodes. Collect their current items afresh, one subvolume at a time, and
 * discard those items after packing. Replace the identity set only after
 * durable publication; allocation exclusions have a separate lifetime.
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

struct log_inode {
	RBT_ENTRY(log_inode) entry;
	uint64_t treeid;
	uint64_t ino;
};
RBT_HEAD(btrfs_log, log_inode);

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

static int
log_inode_compare(const struct log_inode *a, const struct log_inode *b)
{
	if (a->treeid != b->treeid)
		return (a->treeid < b->treeid ? -1 : 1);
	if (a->ino != b->ino)
		return (a->ino < b->ino ? -1 : 1);
	return (0);
}
RBT_PROTOTYPE(btrfs_log, log_inode, entry, log_inode_compare);
RBT_GENERATE(btrfs_log, log_inode, entry, log_inode_compare);

static void
log_track(struct btrfs_log *log, uint64_t treeid, uint64_t ino)
{
	struct log_inode *inode;

	inode = malloc(sizeof(*inode), M_BTRFS, M_WAITOK | M_ZERO);
	inode->treeid = treeid;
	inode->ino = ino;
	if (RBT_INSERT(btrfs_log, log, inode) != NULL)
		free(inode, M_BTRFS, sizeof(*inode));
}

static void
log_free(struct btrfs_log *log)
{
	struct log_inode *inode;

	if (log == NULL)
		return;
	while ((inode = RBT_ROOT(btrfs_log, log)) != NULL) {
		RBT_REMOVE(btrfs_log, log, inode);
		free(inode, M_BTRFS, sizeof(*inode));
	}
	free(log, M_BTRFS, sizeof(*log));
}

void
btrfs_log_destroy(struct btrfs_fs *bmp)
{
	log_free(bmp->bm_log);
	bmp->bm_log = NULL;
	btrfs_space_log_release(bmp);
}

/*
 * Checksums in a log share the file tree's key space. Store sector records
 * while collecting to deduplicate shared/compressed allocations.
 */
static int
log_collect_csums(struct btrfs_fs *bmp, struct log_items *items,
    const struct btrfs_file_extent *extent)
{
	struct btrfs_key key = { 0 };
	uint8_t csum[BTRFS_SUPPORTED_CSUM_MAX];
	uint64_t start, end;
	uint32_t sector = letoh32(bmp->bm_super.sectorsize);
	int error;

	start = extent->bfe_disk_bytenr;
	if (extent->bfe_compression != 0)
		end = start + extent->bfe_disk_num_bytes;
	else {
		start += extent->bfe_disk_offset;
		end = start + extent->bfe_length;
	}
	key.objectid = htole64(BTRFS_EXTENT_CSUM_OBJECTID);
	key.type = BTRFS_EXTENT_CSUM_KEY;
	for (; start < end; start += sector) {
		key.offset = htole64(start);
		if (btrfs_log_find(items, BTRFS_EXTENT_CSUM_OBJECTID,
		    key.type, start) != NULL)
			continue;
		error = btrfs_lookup_data_csum(bmp, start, csum);
		if (error != 0)
			return (error);
		error = btrfs_log_add(items, &key, csum,
		    btrfs_csum_size(&bmp->bm_super));
		if (error != 0)
			return (error);
	}
	return (0);
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
log_collect_inode(struct btrfs_root *root, struct log_items *items, uint64_t ino)
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
	if (!S_ISREG(inode.bi_mode) || inode.bi_nlink == 0 ||
	    inode.bi_generation > bmp->bm_last_transid)
		return (EAGAIN);
	end = roundup(inode.bi_size, sector);
	target.objectid = htole64(ino);
	error = btrfs_search_lower_bound(root, &target, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &key, &data, &size);
		if (error != 0 || key->objectid != target.objectid)
			break;
		if (key->type == BTRFS_INODE_ITEM_KEY ||
		    key->type == BTRFS_XATTR_ITEM_KEY ||
		    key->type == BTRFS_EXTENT_DATA_KEY) {
			error = btrfs_log_add(items, key, data, size);
			if (error != 0)
				break;
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
				error = log_collect_csums(bmp, items, &extent);
			if (error != 0)
				break;
		}
		error = btrfs_next_item(&path);
	}
	btrfs_release_path(&path);
	if (error == ENOENT)
		error = 0;
	if (error == 0 && !inline_data)
		error = log_hole(items, ino, cursor, end, generation);
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
	if (error != 0)
		return (error);
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

/* Bottom-up packing avoids normal tree COW and extent-reference accounting. */
static int
log_pack(struct btrfs_fs *bmp, struct log_items *items,
    struct btrfs_root_item *root_item, struct btrfs_write_batch *batch)
{
	struct log_blocks blocks, parents;
	struct log_block *block;
	struct log_item *item;
	struct btrfs_header *header;
	struct btrfs_item *disk;
	struct btrfs_key_ptr *ptr;
	struct btrfs_key first = { 0 };
	uint32_t nodesize = letoh32(bmp->bm_super.nodesize);
	uint32_t capacity = nodesize - sizeof(*header), count = 0, tail;
	uint8_t level = 0;
	int error = 0;

	TAILQ_INIT(&blocks);
	TAILQ_INIT(&parents);
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
			    &blocks, batch);
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
	error = log_write_block(bmp, header, 0, count, &first, &blocks, batch);
	if (error != 0)
		goto out;
	while (TAILQ_NEXT(TAILQ_FIRST(&blocks), entry) != NULL) {
		if (++level >= BTRFS_MAX_LEVEL) {
			error = EOVERFLOW;
			goto out;
		}
		count = 0;
		memset(header, 0, nodesize);
		ptr = (struct btrfs_key_ptr *)(header + 1);
		while ((block = TAILQ_FIRST(&blocks)) != NULL) {
			if (count == 0)
				first = block->first;
			ptr[count].key = block->first;
			ptr[count].blockptr = htole64(block->bytenr);
			ptr[count++].generation =
			    htole64(bmp->bm_transaction->bt_generation);
			TAILQ_REMOVE(&blocks, block, entry);
			free(block, M_BTRFS, sizeof(*block));
			if (count == capacity / sizeof(*ptr) ||
			    TAILQ_EMPTY(&blocks)) {
				error = log_write_block(bmp, header, level,
				    count, &first, &parents, batch);
				if (error != 0)
					goto out;
				count = 0;
				memset(header, 0, nodesize);
			}
		}
		TAILQ_CONCAT(&blocks, &parents, entry);
	}
	block = TAILQ_FIRST(&blocks);
	memset(root_item, 0, sizeof(*root_item));
	root_item->bytenr = htole64(block->bytenr);
	root_item->generation = htole64(bmp->bm_transaction->bt_generation);
	root_item->level = block->level;
out:
	while ((block = TAILQ_FIRST(&blocks)) != NULL) {
		TAILQ_REMOVE(&blocks, block, entry);
		free(block, M_BTRFS, sizeof(*block));
	}
	while ((block = TAILQ_FIRST(&parents)) != NULL) {
		TAILQ_REMOVE(&parents, block, entry);
		free(block, M_BTRFS, sizeof(*block));
	}
	free(header, M_BTRFS, nodesize);
	return (error);
}

int
btrfs_log_write(struct btrfs_trans_handle *handle, struct btrfs_node *node,
    struct proc *p)
{
	struct btrfs_fs *bmp = node->bn_mount;
	struct btrfs_log *log;
	struct log_inode *inode;
	struct log_items roots, items;
	struct btrfs_root *root;
	struct btrfs_root_item root_item;
	struct btrfs_key key = { 0 };
	struct btrfs_super_block *super;
	struct btrfs_write_batch batch;
	uint64_t treeid;
	int error, end_error, publishing = 0;

	log = malloc(sizeof(*log), M_BTRFS, M_WAITOK | M_ZERO);
	RBT_INIT(btrfs_log, log);
	RBT_INIT(log_items, &roots);
	RBT_INIT(log_items, &items);
	error = btrfs_write_ordered_extents(handle);
	if (error != 0)
		goto out;
	if (bmp->bm_log != NULL) {
		RBT_FOREACH(inode, btrfs_log, bmp->bm_log)
			log_track(log, inode->treeid, inode->ino);
	}
	log_track(log, node->bn_treeid, node->bn_ino);

	btrfs_write_batch_init(&batch);
	inode = RBT_MIN(btrfs_log, log);
	while (inode != NULL) {
		treeid = inode->treeid;
		error = btrfs_get_root(bmp, treeid, &root);
		if (error != 0)
			break;
		do {
			error = log_collect_inode(root, &items, inode->ino);
			if (error != 0)
				break;
			inode = RBT_NEXT(btrfs_log, inode);
		} while (inode != NULL && inode->treeid == treeid);
		if (error == 0)
			error = log_pack(bmp, &items, &root_item, &batch);
		btrfs_log_free_items(&items);
		if (error != 0)
			break;
		key.objectid = htole64(BTRFS_TREE_LOG_OBJECTID);
		key.type = BTRFS_ROOT_ITEM_KEY;
		key.offset = htole64(treeid);
		error = btrfs_log_add(&roots, &key, &root_item, sizeof(root_item));
		if (error != 0)
			break;
	}
	if (error == 0)
		error = log_pack(bmp, &roots, &root_item, &batch);
	end_error = btrfs_write_batch_wait(&batch);
	if (end_error != 0)
		error = end_error;
	if (error != 0)
		goto out;
	error = btrfs_sync_device(bmp, p);
	if (error != 0)
		goto out;
	super = malloc(sizeof(*super), M_BTRFS, M_WAITOK);
	memcpy(super, &bmp->bm_super, sizeof(*super));
	super->log_root = root_item.bytenr;
	super->log_root_level = root_item.level;
	super->__unused_log_root_transid = 0;
	publishing = 1;
	error = btrfs_write_super_mirrors(bmp, super);
	end_error = btrfs_sync_device(bmp, p);
	if (error == 0)
		error = end_error;
	if (error == 0) {
		memcpy(&bmp->bm_super, super, sizeof(*super));
		log_free(bmp->bm_log);
		bmp->bm_log = log;
		log = NULL;
	}
	free(super, M_BTRFS, sizeof(*super));
out:
	btrfs_log_free_items(&items);
	btrfs_log_free_items(&roots);
	log_free(log);
	/* Unpublished log blocks remain excluded until the fallback commits. */
	return (error == ENOSPC && !publishing ? EAGAIN : error);
}
