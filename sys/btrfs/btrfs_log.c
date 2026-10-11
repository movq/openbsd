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
 * Subsequent writes COW it normally. New inodes log their names and fresh
 * ancestry; committed inodes log names only once they change. Names which
 * replay must reconcile across several parents still force full commit.
 * A directory fsync logs the directory's index and its new entries' inodes.
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
#include <btrfs/btrfs_dir.h>
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

static int
btrfs_log_name_compare(const struct btrfs_log_name *a,
    const struct btrfs_log_name *b)
{
	if (a->bln_treeid != b->bln_treeid)
		return (a->bln_treeid < b->bln_treeid ? -1 : 1);
	if (a->bln_ino != b->bln_ino)
		return (a->bln_ino < b->bln_ino ? -1 : 1);
	return (0);
}
RBT_GENERATE(btrfs_log_names, btrfs_log_name, bln_entry,
    btrfs_log_name_compare);

static int
log_name_flags(struct btrfs_transaction *trans, uint64_t treeid, uint64_t ino)
{
	struct btrfs_log_name match, *entry;
	int flags = 0;

	match.bln_treeid = treeid;
	match.bln_ino = ino;
	mtx_enter(&trans->bt_lock);
	entry = RBT_FIND(btrfs_log_names, &trans->bt_log_names, &match);
	if (entry != NULL)
		flags = entry->bln_flags;
	mtx_leave(&trans->bt_lock);
	return (flags);
}

static void
log_name_update(struct btrfs_transaction *trans, uint64_t treeid,
    uint64_t ino, int set, int clear)
{
	struct btrfs_log_name *entry, *spare;

	spare = malloc(sizeof(*spare), M_BTRFS, M_WAITOK | M_ZERO);
	spare->bln_treeid = treeid;
	spare->bln_ino = ino;
	mtx_enter(&trans->bt_lock);
	entry = RBT_INSERT(btrfs_log_names, &trans->bt_log_names, spare);
	if (entry == NULL) {
		entry = spare;
		spare = NULL;
	}
	entry->bln_flags = (entry->bln_flags & ~clear) | set;
	mtx_leave(&trans->bt_lock);
	if (spare != NULL)
		free(spare, M_BTRFS, sizeof(*spare));
}

/*
 * Replay links each logged name over whatever the committed tree has there,
 * and replaces an inode's committed names under its logged parent. Logged
 * names which change are relogged by the next publication. A committed inode
 * whose names changed logs them whenever it is logged. The directory dir,
 * if not NULL, lost a name.
 */
void
btrfs_log_names_changed(struct btrfs_trans_handle *handle,
    struct btrfs_node *dir, struct btrfs_node *node)
{
	struct btrfs_transaction *trans = handle->bth_transaction;
	struct btrfs_log_name match, *entry, *spare = NULL;
	int changed;

	changed = node->bn_inode.bi_generation <=
	    trans->bt_mount->bm_last_transid && node->bn_inode.bi_nlink != 0;
	if (changed) {
		spare = malloc(sizeof(*spare), M_BTRFS, M_WAITOK | M_ZERO);
		spare->bln_treeid = node->bn_treeid;
		spare->bln_ino = node->bn_ino;
	}
	match.bln_treeid = node->bn_treeid;
	match.bln_ino = node->bn_ino;
	mtx_enter(&trans->bt_lock);
	entry = RBT_FIND(btrfs_log_names, &trans->bt_log_names, &match);
	if (entry == NULL && spare != NULL) {
		RBT_INSERT(btrfs_log_names, &trans->bt_log_names, spare);
		entry = spare;
		spare = NULL;
	}
	if (entry != NULL && (entry->bln_flags & BTRFS_LOG_NAME_LOGGED))
		entry->bln_flags |= BTRFS_LOG_NAME_STALE;
	if (changed)
		entry->bln_flags |= BTRFS_LOG_NAME_CHANGED;
	mtx_leave(&trans->bt_lock);
	if (spare != NULL)
		free(spare, M_BTRFS, sizeof(*spare));
	if (dir != NULL)
		btrfs_log_dir_changed(handle, dir);
}

/*
 * A directory lost a name or its inode changed. A synced directory's logged
 * index is relogged by the next publication.
 */
void
btrfs_log_dir_changed(struct btrfs_trans_handle *handle,
    struct btrfs_node *dir)
{
	struct btrfs_transaction *trans = handle->bth_transaction;
	struct btrfs_log_name match, *entry;

	match.bln_treeid = dir->bn_treeid;
	match.bln_ino = dir->bn_ino;
	mtx_enter(&trans->bt_lock);
	entry = RBT_FIND(btrfs_log_names, &trans->bt_log_names, &match);
	if (entry != NULL) {
		entry->bln_flags |= BTRFS_LOG_NAME_DIRTY;
		if (entry->bln_flags & BTRFS_LOG_NAME_SYNCED)
			entry->bln_flags |= BTRFS_LOG_NAME_STALE;
	}
	mtx_leave(&trans->bt_lock);
}

void
btrfs_log_names_free(struct btrfs_transaction *trans)
{
	struct btrfs_log_name *entry;

	while ((entry = RBT_ROOT(btrfs_log_names, &trans->bt_log_names)) !=
	    NULL) {
		RBT_REMOVE(btrfs_log_names, &trans->bt_log_names, entry);
		free(entry, M_BTRFS, sizeof(*entry));
	}
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

#define LOG_PENDING_MAX	16

/* One publication: the target, its ancestry, and other affected names. */
struct log_writer {
	struct btrfs_trans_handle *handle;
	struct btrfs_root *root;
	/* The committed subvolume tree, against which replay links names. */
	struct btrfs_root commit;
	struct btrfs_root_item subtree;
	struct log_items items, csums;
	/* Inodes to log after the target: stale or displaced names. */
	uint64_t pending[LOG_PENDING_MAX];
	unsigned int npending;
	/* A synced directory, whose inode is logged in full. */
	uint64_t dir;
};

struct log_owner {
	const uint8_t *name;
	uint64_t ino;
	uint16_t len;
};

static int
log_commit_root(struct log_writer *w)
{
	struct btrfs_fs *bmp = w->root->br_mount;
	struct btrfs_root tree;
	struct btrfs_root_item item;
	int error;

	if (w->commit.br_mount != NULL)
		return (0);
	memset(&tree, 0, sizeof(tree));
	tree.br_mount = bmp;
	tree.br_super = &bmp->bm_super;
	tree.br_owner = BTRFS_ROOT_TREE_OBJECTID;
	tree.br_bytenr = letoh64(bmp->bm_super.root);
	tree.br_generation = tree.br_view_generation =
	    letoh64(bmp->bm_super.generation);
	tree.br_level = bmp->bm_super.root_level;
	error = btrfs_find_root_item(&tree, w->root->br_owner,
	    BTRFS_FIRST_FREE_OBJECTID, &item, NULL);
	/* Root changes force a commit; this is only a safety net. */
	if (error == ENOENT)
		error = EAGAIN;
	if (error != 0)
		return (error);
	w->commit = tree;
	w->commit.br_owner = w->root->br_owner;
	w->commit.br_bytenr = letoh64(item.bytenr);
	w->commit.br_generation = letoh64(item.generation);
	w->commit.br_level = item.level;
	return (0);
}

static int
log_pending_add(struct log_writer *w, uint64_t ino)
{
	unsigned int i;

	for (i = 0; i < w->npending; i++)
		if (w->pending[i] == ino)
			return (0);
	if (w->npending == LOG_PENDING_MAX)
		return (EAGAIN);
	w->pending[w->npending++] = ino;
	return (0);
}

static int
log_owner_match(const struct btrfs_dir_entry *entry, void *arg)
{
	struct log_owner *owner = arg;

	if (entry->bde_namelen == owner->len &&
	    memcmp(entry->bde_name, owner->name, owner->len) == 0)
		owner->ino = entry->bde_subvolume ? UINT64_MAX :
		    entry->bde_objectid;
	return (0);
}

/*
 * A logged name replaces any committed entry. If that named a committed inode
 * which has since moved, its new name must be logged too: otherwise replay
 * unlinks it.
 */
static int
log_displaced(struct log_writer *w, uint64_t ino, uint64_t parent)
{
	struct btrfs_inode_ref_record ref;
	struct btrfs_inode inode;
	struct log_owner owner;
	struct log_item *item;
	uint32_t pos;
	int error;

	item = btrfs_log_find(&w->items, ino, BTRFS_INODE_REF_KEY, parent);
	if (item == NULL)
		return (EINVAL);
	error = log_commit_root(w);
	for (pos = 0; error == 0 && pos < item->size; pos += ref.bytes) {
		error = btrfs_decode_inode_ref(&item->key, item->data,
		    item->size, pos, &ref);
		if (error == 0)
			error = btrfs_validate_inode_ref(&item->key, &ref);
		if (error != 0)
			break;
		owner.name = ref.name;
		owner.len = ref.len;
		owner.ino = 0;
		error = btrfs_lookup_directory(&w->commit, parent,
		    (const char *)ref.name, ref.len, log_owner_match, &owner);
		if (error == 0 && owner.ino == UINT64_MAX)
			error = EAGAIN;
		if (error != 0 || owner.ino == 0 || owner.ino == ino)
			continue;
		/* An unlinked owner is meant to lose the name. */
		error = btrfs_find_inode(w->root, owner.ino, &inode);
		if (error == 0 && inode.bi_nlink != 0)
			error = log_pending_add(w, owner.ino);
		else if (error == ENOENT)
			error = 0;
	}
	return (error);
}

/* Replay replaces committed names only under the logged parent. */
static int
log_commit_refs(struct log_writer *w, uint64_t ino, uint64_t parent)
{
	struct btrfs_path path = { 0 };
	struct btrfs_key target = { 0 };
	const struct btrfs_key *key;
	int error;

	error = log_commit_root(w);
	if (error != 0)
		return (error);
	target.objectid = htole64(ino);
	target.type = BTRFS_INODE_REF_KEY;
	error = btrfs_search_lower_bound(&w->commit, &target, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &key, NULL, NULL);
		if (error != 0 || key->objectid != target.objectid ||
		    key->type > BTRFS_INODE_EXTREF_KEY)
			break;
		if (key->type != BTRFS_INODE_REF_KEY ||
		    letoh64(key->offset) != parent) {
			error = EAGAIN;
			break;
		}
		error = btrfs_next_item(&path);
	}
	btrfs_release_path(&path);
	return (error == ENOENT ? 0 : error);
}

/*
 * New inodes carry their names, as do committed inodes whose names changed
 * or were already logged. Other committed inodes keep their committed names.
 */
static int
log_collect_inode(struct log_writer *w, uint64_t ino, uint64_t *parent)
{
	struct btrfs_root *root = w->root;
	struct log_items *items = &w->items, *csums = &w->csums;
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
	int committed, error, flags, inline_data = 0, names;

	error = btrfs_find_inode(root, ino, &inode);
	if (error != 0)
		return (error);
	if ((!S_ISREG(inode.bi_mode) && !S_ISDIR(inode.bi_mode)) ||
	    inode.bi_nlink == 0)
		return (EAGAIN);
	committed = inode.bi_generation <= bmp->bm_last_transid;
	flags = log_name_flags(bmp->bm_transaction, root->br_owner, ino);
	names = !committed ||
	    (flags & (BTRFS_LOG_NAME_LOGGED | BTRFS_LOG_NAME_CHANGED));
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
		    (names && key->type == BTRFS_INODE_REF_KEY)) {
			error = btrfs_log_add(items, key, data, size);
			if (error != 0)
				break;
		}
		/* Names under several parents force a commit. */
		if (names && (key->type == BTRFS_INODE_EXTREF_KEY ||
		    (key->type == BTRFS_INODE_REF_KEY && *parent != 0))) {
			error = EAGAIN;
			break;
		}
		if (names && key->type == BTRFS_INODE_REF_KEY)
			*parent = letoh64(key->offset);
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
	if (error == 0 && names && *parent == 0)
		error = EAGAIN;
	if (error == 0 && names && committed)
		error = log_commit_refs(w, ino, *parent);
	/*
	 * Existing ancestry needs only an existence record. New directories
	 * carry xattrs and their own parent reference, but no directory ranges:
	 * replay installs only the child's logged name.
	 */
	if (error == 0) {
		struct log_item *item = btrfs_log_find(items, ino,
		    BTRFS_INODE_ITEM_KEY, 0);
		struct btrfs_inode_item *disk = (void *)item->data;

		if (S_ISDIR(inode.bi_mode) && committed && ino != w->dir &&
		    !(flags & BTRFS_LOG_NAME_SYNCED))
			disk->generation = 0;
		else if (inode.bi_generation == 0) {
			/*
			 * mkfs imports may have generation zero. In the log
			 * that means existence only, so use this transaction
			 * for a regular inode whose metadata must be replayed.
			 */
			disk->generation = htole64(generation);
		}
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
	/* The next publication edits this block. */
	btrfs_extent_buffer_cache(bmp, buffer);
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

/* Log an inode and any new ancestry, marking which names are now logged. */
static int
log_inode_path(struct log_writer *w, uint64_t ino)
{
	struct btrfs_fs *bmp = w->root->br_mount;
	struct btrfs_key low = { 0 }, high = { 0 };
	struct log_item *item;
	uint64_t parent;
	unsigned int depth = 0;
	int error;

	do {
		error = log_collect_inode(w, ino, &parent);
		if (error == 0 && parent != 0)
			error = log_displaced(w, ino, parent);
		if (error != 0)
			return (error);
		item = btrfs_log_find(&w->items, ino, BTRFS_INODE_ITEM_KEY, 0);
		low.objectid = high.objectid = htole64(ino);
		high.type = BTRFS_DIR_LOG_ITEM_KEY;
		if (!S_ISDIR(letoh32(
		    ((struct btrfs_inode_item *)item->data)->mode))) {
			high.objectid = htole64(ino + 1);
			high.type = 0;
		}
		error = log_update(bmp, &w->subtree, &w->items, &low, &high, 0);
		if (error != 0)
			return (error);
		if (parent != 0)
			log_name_update(bmp->bm_transaction, w->root->br_owner,
			    ino, BTRFS_LOG_NAME_LOGGED, BTRFS_LOG_NAME_STALE);
		ino = parent;
		/* Corrupt ancestry must not loop or exhaust the kernel stack. */
		if (++depth > MAXPATHLEN)
			return (EAGAIN);
	} while (ino != 0);
	return (0);
}

#define LOG_DIRECTORY_MAX	256

/*
 * Queue the owners of a directory's entries added in this generation, and the
 * owners of removed committed entries which are still linked: replay deletes
 * committed entries missing from the logged range.
 */
static int
log_directory_entries(struct log_writer *w, struct btrfs_root *root,
    uint64_t dir, int committed)
{
	struct btrfs_fs *bmp = w->root->br_mount;
	struct btrfs_path path = { 0 };
	struct btrfs_key target = { 0 };
	const struct btrfs_key *key;
	const uint8_t *data;
	struct btrfs_dir_record record;
	struct btrfs_inode inode;
	struct log_item *item;
	uint64_t ino;
	uint32_t size;
	unsigned int count = 0;
	int error;

	target.objectid = htole64(dir);
	target.type = BTRFS_DIR_INDEX_KEY;
	error = btrfs_search_lower_bound(root, &target, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &key, &data, &size);
		if (error != 0 || key->objectid != target.objectid ||
		    key->type != BTRFS_DIR_INDEX_KEY)
			break;
		error = btrfs_decode_dir_record(data, size, &record);
		if (error == 0)
			error = btrfs_validate_dir_record(key,
			    bmp->bm_transaction->bt_generation, size, &record);
		if (error == 0 &&
		    (record.item->location.type != BTRFS_INODE_ITEM_KEY ||
		    ++count > LOG_DIRECTORY_MAX))
			error = EAGAIN;
		if (error != 0)
			break;
		ino = letoh64(record.item->location.objectid);
		if (!committed) {
			if (letoh64(record.item->transid) > bmp->bm_last_transid)
				error = log_pending_add(w, ino);
			if (error == 0)
				error = btrfs_log_add(&w->items, key, data, size);
		} else {
			item = btrfs_log_find(&w->items, dir,
			    BTRFS_DIR_INDEX_KEY, letoh64(key->offset));
			if (item == NULL || item->size != size ||
			    memcmp(item->data, data, size) != 0) {
				error = btrfs_find_inode(w->root, ino, &inode);
				if (error == 0 && inode.bi_nlink != 0)
					error = log_pending_add(w, ino);
				else if (error == ENOENT)
					error = 0;
			}
		}
		if (error != 0)
			break;
		error = btrfs_next_item(&path);
	}
	btrfs_release_path(&path);
	return (error == ENOENT ? 0 : error);
}

/*
 * A directory logs its whole index under one range, so replay reproduces
 * exactly its current entries. Ancestry updates keep these records.
 */
static int
log_directory(struct log_writer *w, uint64_t dir)
{
	struct btrfs_fs *bmp = w->root->br_mount;
	struct btrfs_dir_log_item range;
	struct btrfs_key key = { 0 }, high = { 0 };
	struct btrfs_inode inode;
	int error;

	error = btrfs_find_inode(w->root, dir, &inode);
	if (error == 0)
		error = log_directory_entries(w, w->root, dir, 0);
	if (error == 0 && inode.bi_generation <= bmp->bm_last_transid) {
		error = log_commit_root(w);
		if (error == 0)
			error = log_directory_entries(w, &w->commit, dir, 1);
	}
	key.objectid = htole64(dir);
	key.type = BTRFS_DIR_LOG_INDEX_KEY;
	range.end = htole64(UINT64_MAX);
	if (error == 0)
		error = btrfs_log_add(&w->items, &key, &range, sizeof(range));
	if (error != 0)
		return (error);
	key.type = BTRFS_DIR_LOG_ITEM_KEY;
	high.objectid = htole64(dir + 1);
	error = log_update(bmp, &w->subtree, &w->items, &key, &high, 0);
	if (error == 0)
		log_name_update(bmp->bm_transaction, w->root->br_owner, dir,
		    BTRFS_LOG_NAME_SYNCED,
		    BTRFS_LOG_NAME_DIRTY | BTRFS_LOG_NAME_STALE);
	return (error);
}

/*
 * A synced directory remains durable if the log holds its own name, every
 * entry added since is logged through its owner's names, and it has not lost
 * a name which the log does not drop or changed its inode.
 */
static int
log_directory_logged(struct log_writer *w, uint64_t dir)
{
	struct btrfs_transaction *trans = w->handle->bth_transaction;
	struct btrfs_fs *bmp = trans->bt_mount;
	struct btrfs_log_name *entry;
	struct btrfs_path path = { 0 };
	struct btrfs_key target = { 0 };
	const struct btrfs_key *key;
	const uint8_t *data;
	struct btrfs_dir_record record;
	struct btrfs_inode inode;
	uint32_t size;
	unsigned int count = 0;
	int error, flags;

	flags = log_name_flags(trans, w->root->br_owner, dir);
	if ((flags & BTRFS_LOG_NAME_DIRTY) || !(flags & BTRFS_LOG_NAME_SYNCED))
		return (0);
	if (btrfs_find_inode(w->root, dir, &inode) != 0)
		return (0);
	if ((inode.bi_generation > bmp->bm_last_transid ||
	    (flags & BTRFS_LOG_NAME_CHANGED)) &&
	    !(flags & BTRFS_LOG_NAME_LOGGED))
		return (0);
	mtx_enter(&trans->bt_lock);
	RBT_FOREACH(entry, btrfs_log_names, &trans->bt_log_names)
		if (entry->bln_flags & BTRFS_LOG_NAME_STALE)
			break;
	mtx_leave(&trans->bt_lock);
	if (entry != NULL)
		return (0);
	target.objectid = htole64(dir);
	target.type = BTRFS_DIR_INDEX_KEY;
	error = btrfs_search_lower_bound(w->root, &target, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &key, &data, &size);
		if (error != 0 || key->objectid != target.objectid ||
		    key->type != BTRFS_DIR_INDEX_KEY)
			break;
		error = btrfs_decode_dir_record(data, size, &record);
		if (error == 0 && ++count > LOG_DIRECTORY_MAX)
			error = EAGAIN;
		if (error == 0 &&
		    letoh64(record.item->transid) > bmp->bm_last_transid &&
		    (record.item->location.type != BTRFS_INODE_ITEM_KEY ||
		    !(log_name_flags(trans, w->root->br_owner,
		    letoh64(record.item->location.objectid)) &
		    BTRFS_LOG_NAME_LOGGED)))
			error = EAGAIN;
		if (error != 0)
			break;
		error = btrfs_next_item(&path);
	}
	btrfs_release_path(&path);
	return (error == 0 || error == ENOENT);
}

/*
 * Queue inodes whose logged names changed since publication. Drop those which
 * lost their last name; replay must not resurrect them.
 */
static int
log_stale_names(struct log_writer *w)
{
	struct btrfs_transaction *trans = w->handle->bth_transaction;
	struct btrfs_log_name *entry;
	struct btrfs_inode inode;
	struct btrfs_key low = { 0 }, high = { 0 };
	uint64_t treeid, ino;
	int error;

	for (;;) {
		mtx_enter(&trans->bt_lock);
		RBT_FOREACH(entry, btrfs_log_names, &trans->bt_log_names)
			if (entry->bln_flags & BTRFS_LOG_NAME_STALE)
				break;
		if (entry != NULL) {
			treeid = entry->bln_treeid;
			ino = entry->bln_ino;
			entry->bln_flags &= ~BTRFS_LOG_NAME_STALE;
		}
		mtx_leave(&trans->bt_lock);
		if (entry == NULL)
			return (0);
		if (treeid != w->root->br_owner)
			return (EAGAIN);
		error = btrfs_find_inode(w->root, ino, &inode);
		if (error == 0 && inode.bi_nlink != 0)
			error = log_pending_add(w, ino);
		else if (error == 0 || error == ENOENT) {
			low.objectid = htole64(ino);
			high.objectid = htole64(ino + 1);
			error = log_update(trans->bt_mount, &w->subtree,
			    &w->items, &low, &high, 0);
			log_name_update(trans, treeid, ino, 0,
			    BTRFS_LOG_NAME_LOGGED);
		}
		if (error != 0)
			return (error);
	}
}

/* The target goes first, so a drop cannot empty the subtree. */
static int
log_names(struct log_writer *w, uint64_t target)
{
	struct btrfs_transaction *trans = w->handle->bth_transaction;
	uint64_t ino;
	unsigned int i;
	int error, flags;

	error = log_inode_path(w, target);
	if (error == 0 && w->dir != 0)
		error = log_directory(w, target);
	if (error == 0)
		error = log_stale_names(w);
	for (i = 0; error == 0 && i < w->npending; i++) {
		ino = w->pending[i];
		/* Synced directories relog their index, not their names. */
		flags = log_name_flags(trans, w->root->br_owner, ino);
		if (!(flags & BTRFS_LOG_NAME_SYNCED))
			log_name_update(trans, w->root->br_owner, ino,
			    BTRFS_LOG_NAME_LOGGED, 0);
		error = btrfs_write_inode_ordered(w->handle,
		    w->root->br_owner, ino);
		if (error == 0)
			error = log_inode_path(w, ino);
		if (error == 0 && (flags & BTRFS_LOG_NAME_SYNCED))
			error = log_directory(w, ino);
	}
	return (error);
}

int
btrfs_log_write(struct btrfs_trans_handle *handle, struct btrfs_node *node,
    struct proc *p)
{
	struct btrfs_fs *bmp = node->bn_mount;
	struct log_writer *w;
	struct log_items *items;
	struct log_item *item;
	struct btrfs_root log;
	struct btrfs_root_item forest = { 0 }, *subtree;
	struct btrfs_path path = { 0 };
	struct btrfs_key key = { 0 }, low = { 0 }, high = { 0 };
	struct btrfs_super_block *super;
	const uint8_t *data;
	uint32_t size;
	int error, end_error;

	/* The writer is too large for the kernel stack. */
	w = malloc(sizeof(*w), M_BTRFS, M_WAITOK | M_ZERO);
	w->handle = handle;
	w->root = node->bn_root;
	RBT_INIT(log_items, &w->items);
	RBT_INIT(log_items, &w->csums);
	items = &w->items;
	subtree = &w->subtree;
	if (S_ISDIR(node->bn_inode.bi_mode)) {
		error = 0;
		if (log_directory_logged(w, node->bn_ino))
			goto out;
		w->dir = node->bn_ino;
	}
	error = btrfs_write_inode_ordered(handle, node->bn_treeid,
	    node->bn_ino);
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
			if (error == 0 && size != sizeof(*subtree))
				error = EINVAL;
			if (error == 0)
				memcpy(subtree, data, size);
		} else if (error == ENOENT)
			error = 0;
		btrfs_release_path(&path);
		if (error != 0)
			goto out;
	}
	error = log_names(w, node->bn_ino);
	if (error != 0)
		goto out;
	while ((item = RBT_MIN(log_items, &w->csums)) != NULL) {
		RBT_REMOVE(log_items, &w->csums, item);
		RBT_INSERT(log_items, items, item);
		low = high = item->key;
		high.offset = htole64(letoh64(low.offset) +
		    item->size / btrfs_csum_size(&bmp->bm_super) *
		    letoh32(bmp->bm_super.sectorsize));
		error = log_update(bmp, subtree, items, &low, &high, 1);
		if (error != 0)
			goto out;
	}
	error = btrfs_log_add(items, &key, subtree, sizeof(*subtree));
	low = high = key;
	high.offset = htole64(node->bn_treeid + 1);
	if (error == 0)
		error = log_update(bmp, &forest, items, &low, &high, 0);
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
	btrfs_log_free_items(&w->items);
	btrfs_log_free_items(&w->csums);
	free(w, M_BTRFS, sizeof(*w));
	/* Unpublished log blocks remain excluded until the fallback commits. */
	return (error);
}
