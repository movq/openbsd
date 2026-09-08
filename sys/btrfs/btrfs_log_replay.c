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
 * Linux tree-log recovery (the disk protocol is described in btrfs_log.h).
 * Recovery first reads and validates the entire forest and excludes its
 * blocks and new data allocations. Only then can it COW the committed trees.
 * All replay stages share one transaction; no intermediate recovery state
 * can become durable. Failure leaves the original superblock/log intact.
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

/* This forest and its accounting worklists exist only during recovery. */
struct log_tree {
	TAILQ_ENTRY(log_tree) entry;
	uint64_t id;
	struct log_items items;
	struct log_items fixups;
	struct btrfs_root *root;
};
TAILQ_HEAD(log_trees, log_tree);

static struct log_tree *
log_tree_get(struct log_trees *trees, uint64_t id)
{
	struct log_tree *tree;

	TAILQ_FOREACH(tree, trees, entry)
		if (tree->id == id)
			return (tree);
	tree = malloc(sizeof(*tree), M_BTRFS, M_WAITOK | M_ZERO);
	tree->id = id;
	RBT_INIT(log_items, &tree->items);
	RBT_INIT(log_items, &tree->fixups);
	TAILQ_INSERT_TAIL(trees, tree, entry);
	return (tree);
}

static void
log_free(struct log_trees *trees)
{
	struct log_tree *tree;

	while ((tree = TAILQ_FIRST(trees)) != NULL) {
		TAILQ_REMOVE(trees, tree, entry);
		btrfs_log_free_items(&tree->items);
		btrfs_log_free_items(&tree->fixups);
		free(tree, M_BTRFS, sizeof(*tree));
	}
}

/* Copy a tree item without borrowing a path across any mutation. */
static int
log_read_item(struct btrfs_root *root, const struct btrfs_key *key,
    struct log_item **result)
{
	struct btrfs_path path = { 0 };
	const uint8_t *data;
	struct log_item *item;
	uint32_t size;
	int error;

	*result = NULL;
	error = btrfs_search_slot(root, key, &path);
	if (error == 0)
		error = btrfs_path_item(&path, NULL, &data, &size);
	if (error == 0) {
		item = malloc(sizeof(*item) + size, M_BTRFS, M_WAITOK | M_ZERO);
		item->key = *key;
		item->size = size;
		memcpy(item->data, data, size);
		*result = item;
	}
	btrfs_release_path(&path);
	return (error);
}

static void
log_item_free(struct log_item *item)
{
	if (item != NULL)
		free(item, M_BTRFS, sizeof(*item) + item->size);
}

static int
log_put(struct btrfs_trans_handle *handle, struct btrfs_root *root,
    const struct btrfs_key *key, const void *data, uint32_t size)
{
	int error;

	error = btrfs_space_replay_reserve(handle);
	if (error != 0)
		return (error);
	error = btrfs_replace_item(handle, root, key, data, size);
	if (error == ENOENT)
		error = btrfs_insert_item(handle, root, key, data, size);
	return (error);
}

/*
 * The exclusion also rejects cycles, multiply referenced log blocks, and
 * log blocks overlapping any committed allocation. Validate separators and
 * every leaf's global key interval, as well as the extent-buffer checksum.
 */
static int
log_load_block(struct btrfs_root *root, struct log_items *items,
    uint64_t bytenr, uint64_t generation, uint8_t level,
    const struct btrfs_key *low, const struct btrfs_key *high)
{
	struct btrfs_fs *bmp = root->br_mount;
	struct btrfs_extent_buffer *eb;
	const struct btrfs_header *header;
	const struct btrfs_item *disk;
	const struct btrfs_key_ptr *ptr;
	const struct btrfs_key *key;
	const uint8_t *data;
	uint32_t i, count, size;
	int error;

	if (level >= BTRFS_MAX_LEVEL ||
	    generation != bmp->bm_last_transid + 1)
		return (EINVAL);
	error = btrfs_space_log_claim(bmp, bytenr,
	    letoh32(bmp->bm_super.nodesize), 0);
	if (error != 0)
		return (error);
	error = btrfs_extent_buffer_read(root, bytenr, generation,
	    generation, level, &eb);
	if (error != 0)
		return (error);
	header = btrfs_extent_buffer_data(eb);
	count = letoh32(header->nritems);
	disk = (const struct btrfs_item *)(header + 1);
	ptr = (const struct btrfs_key_ptr *)(header + 1);
	for (i = 0; i < count; i++) {
		key = level == 0 ? &disk[i].key : &ptr[i].key;
		if ((i == 0 && low != NULL &&
		    btrfs_log_key_compare(key, low) != 0) ||
		    (high != NULL && btrfs_log_key_compare(key, high) >= 0)) {
			error = EINVAL;
			break;
		}
		if (level != 0)
			error = log_load_block(root, items,
			    letoh64(ptr[i].blockptr),
			    letoh64(ptr[i].generation),
			    level - 1, key,
			    i + 1 < count ? &ptr[i + 1].key : high);
		else {
			size = letoh32(disk[i].size);
			data = (const uint8_t *)(header + 1) +
			    letoh32(disk[i].offset);
			if (btrfs_log_find(items, letoh64(key->objectid),
			    key->type, letoh64(key->offset)) != NULL)
				error = EINVAL;
			else
				error = btrfs_log_add(items, key, data, size);
		}
		if (error != 0)
			break;
	}
	btrfs_extent_buffer_put(eb);
	return (error);
}

static int
log_ignored(struct log_tree *tree, uint64_t ino)
{
	struct log_item *item = btrfs_log_find(&tree->items, ino,
	    BTRFS_INODE_ITEM_KEY, 0);
	const struct btrfs_inode_item *inode;

	if (item == NULL || item->size != sizeof(*inode))
		return (0);
	inode = (const void *)item->data;
	return (inode->nlink == 0);
}

static int
log_fixup(struct log_tree *tree, uint64_t ino)
{
	struct btrfs_key key = { 0 };

	key.objectid = htole64(ino);
	key.type = BTRFS_INODE_ITEM_KEY;
	return (btrfs_log_add(&tree->fixups, &key, NULL, 0));
}

static int
log_claim_data(struct btrfs_fs *bmp, struct log_tree *tree,
    const struct log_item *item)
{
	struct btrfs_file_extent extent;
	struct btrfs_root *root;
	struct btrfs_key key = { 0 };
	struct log_item *existing = NULL;
	const struct btrfs_extent_item *disk;
	int error;

	error = btrfs_decode_file_extent(bmp, bmp->bm_last_transid + 1,
	    &item->key, item->data, item->size, &extent);
	if (error != 0 || extent.bfe_disk_bytenr == 0 ||
	    log_ignored(tree, letoh64(item->key.objectid)))
		return (error);
	error = btrfs_get_root(bmp, BTRFS_EXTENT_TREE_OBJECTID, &root);
	if (error != 0)
		return (error);
	key.objectid = htole64(extent.bfe_disk_bytenr);
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = htole64(extent.bfe_disk_num_bytes);
	error = log_read_item(root, &key, &existing);
	if (error == ENOENT)
		return (btrfs_space_log_claim(bmp, extent.bfe_disk_bytenr,
		    extent.bfe_disk_num_bytes, 1));
	if (error == 0) {
		disk = (const void *)existing->data;
		if (existing->size < sizeof(*disk) ||
		    letoh64(disk->flags) != BTRFS_EXTENT_FLAG_DATA ||
		    disk->refs == 0)
			error = EINVAL;
	}
	log_item_free(existing);
	return (error);
}

static int
log_drop_range(struct btrfs_trans_handle *handle, struct log_tree *tree,
    uint64_t ino, uint64_t start, uint64_t end)
{
	struct btrfs_path path = { 0 };
	struct btrfs_file_extent extent;
	struct btrfs_extent_plan plan;
	uint64_t cursor = start, from, to;
	int error;

	while (cursor < end) {
		error = btrfs_find_file_extent(tree->root->br_mount, tree->root,
		    &path, ino, cursor, end, &extent);
		btrfs_release_path(&path);
		if (error != 0)
			return (error);
		from = MAX(cursor, extent.bfe_logical);
		to = MIN(end, extent.bfe_logical + extent.bfe_length);
		if (to <= cursor)
			return (EINVAL);
		cursor = to;
		if (!extent.bfe_item_present)
			continue;
		if (extent.bfe_type == BTRFS_FILE_EXTENT_INLINE) {
			if (from != 0 || to != extent.bfe_length)
				return (EINVAL);
		}
		error = btrfs_space_replay_reserve(handle);
		if (error == 0)
			error = btrfs_extent_plan_prepare(&plan,
			    tree->root, ino, &extent, from, to - from,
			    NULL, UINT64_MAX);
		if (error == 0)
			error = btrfs_extent_plan_apply(handle, &plan);
		if (error != 0)
			return (error);
	}
	return (0);
}

static int
log_delete_type(struct btrfs_trans_handle *handle, struct btrfs_root *root,
    uint64_t ino, uint8_t type)
{
	struct btrfs_path path = { 0 };
	struct btrfs_key target = { 0 }, key;
	const struct btrfs_key *found;
	int error;

	target.objectid = htole64(ino);
	target.type = type;
	for (;;) {
		error = btrfs_search_lower_bound(root, &target, &path);
		if (error == ENOENT) {
			btrfs_release_path(&path);
			return (0);
		}
		if (error == 0)
			error = btrfs_path_item(&path, &found, NULL, NULL);
		if (error == 0)
			key = *found;
		btrfs_release_path(&path);
		if (error != 0)
			return (error);
		if (key.objectid != target.objectid || key.type != type)
			return (0);
		error = btrfs_space_replay_reserve(handle);
		if (error == 0)
			error = btrfs_delete_item(handle, root, &key);
		if (error != 0)
			return (error);
	}
}

static int
log_replay_inode(struct btrfs_trans_handle *handle, struct log_tree *tree,
    struct log_item *item)
{
	struct btrfs_fs *bmp = tree->root->br_mount;
	struct btrfs_inode_item inode;
	struct log_item *old = NULL;
	uint64_t ino = letoh64(item->key.objectid), size;
	int error, exists;

	if (item->size != sizeof(inode) || item->key.offset != 0)
		return (EINVAL);
	memcpy(&inode, item->data, sizeof(inode));
	if (inode.nlink == 0)
		return (0);
	error = log_read_item(tree->root, &item->key, &old);
	exists = error == 0;
	if (error != 0 && error != ENOENT)
		return (error);
	if (exists && old->size != sizeof(inode)) {
		log_item_free(old);
		return (EINVAL);
	}
	/*
	 * A zero generation is an existence-only record. In particular it
	 * must not truncate an already committed ancestor directory.
	 */
	if (inode.generation == 0 && exists) {
		log_item_free(old);
		return (log_fixup(tree, ino));
	}
	if (inode.generation == 0)
		inode.generation =
		    htole64(handle->bth_transaction->bt_generation);
	if (S_ISDIR(letoh32(inode.mode)) && exists)
		inode.size = ((struct btrfs_inode_item *)old->data)->size;
	inode.transid = htole64(handle->bth_transaction->bt_generation);
	log_item_free(old);
	error = log_put(handle, tree->root, &item->key, &inode, sizeof(inode));
	if (error == 0)
		error = log_delete_type(handle, tree->root, ino,
		    BTRFS_XATTR_ITEM_KEY);
	if (error == 0 && S_ISREG(letoh32(inode.mode))) {
		size = letoh64(inode.size);
		if (size > INT64_MAX)
			return (EINVAL);
		size = roundup(size, letoh32(bmp->bm_super.sectorsize));
		error = log_drop_range(handle, tree, ino, size, UINT64_MAX);
	}
	if (error == 0)
		error = log_fixup(tree, ino);
	return (error);
}

static int
log_replay_csums(struct btrfs_trans_handle *handle, struct log_tree *tree,
    uint64_t ino, const struct btrfs_file_extent *extent)
{
	struct btrfs_fs *bmp = tree->root->br_mount;
	struct btrfs_inode inode;
	struct log_item match, *item;
	uint8_t csum[BTRFS_SUPPORTED_CSUM_MAX];
	uint64_t start, end, delta, span;
	uint32_t sector = letoh32(bmp->bm_super.sectorsize);
	size_t csum_size = btrfs_csum_size(&bmp->bm_super);
	int error;

	if (extent->bfe_type != BTRFS_FILE_EXTENT_REG)
		return (0);
	error = btrfs_find_inode(tree->root, ino, &inode);
	if (error != 0 || (inode.bi_flags & BTRFS_INODE_NODATASUM))
		return (error);
	start = extent->bfe_disk_bytenr;
	if (extent->bfe_compression != 0)
		end = start + extent->bfe_disk_num_bytes;
	else {
		start += extent->bfe_disk_offset;
		end = start + extent->bfe_length;
	}
	memset(&match, 0, sizeof(match));
	match.key.objectid = htole64(BTRFS_EXTENT_CSUM_OBJECTID);
	match.key.type = BTRFS_EXTENT_CSUM_KEY;
	for (; start < end; start += sector) {
		match.key.offset = htole64(start);
		item = RBT_NFIND(log_items, &tree->items, &match);
		if (item == NULL)
			item = RBT_MAX(log_items, &tree->items);
		else if (btrfs_log_key_compare(&item->key, &match.key) > 0)
			item = RBT_PREV(log_items, item);
		if (item == NULL || item->key.objectid != match.key.objectid ||
		    item->key.type != match.key.type ||
		    letoh64(item->key.offset) > start)
			goto committed;
		span = (uint64_t)(item->size / csum_size) * sector;
		delta = start - letoh64(item->key.offset);
		if (delta >= span)
			goto committed;
		error = btrfs_space_replay_reserve(handle);
		if (error == 0)
			error = btrfs_delete_data_csums(handle, start, sector);
		if (error == 0)
			error = btrfs_insert_data_csums(handle, start,
			    item->data + delta / sector * csum_size, 1);
		if (error != 0)
			return (error);
		continue;
committed:
		/* Unchanged mappings may borrow their committed checksums. */
		error = btrfs_lookup_data_csum(bmp, start, csum);
		if (error != 0)
			return (error);
	}
	return (0);
}

static int
log_replay_extent(struct btrfs_trans_handle *handle, struct log_tree *tree,
    const struct log_item *item)
{
	struct btrfs_fs *bmp = tree->root->br_mount;
	struct btrfs_file_extent extent;
	struct log_item *old = NULL;
	uint64_t ino = letoh64(item->key.objectid), end;
	int error;

	error = btrfs_decode_file_extent(bmp, bmp->bm_last_transid + 1,
	    &item->key, item->data, item->size, &extent);
	if (error != 0)
		return (error);
	error = log_read_item(tree->root, &item->key, &old);
	if (error == 0 && old->size == item->size &&
	    memcmp(old->data, item->data, item->size) == 0) {
		log_item_free(old);
		return (log_replay_csums(handle, tree, ino, &extent));
	}
	log_item_free(old);
	if (error != 0 && error != ENOENT)
		return (error);
	end = extent.bfe_logical + extent.bfe_length;
	if (extent.bfe_type == BTRFS_FILE_EXTENT_INLINE)
		end = roundup(end, letoh32(bmp->bm_super.sectorsize));
	error = log_drop_range(handle, tree, ino, extent.bfe_logical, end);
	if (error == 0 && (extent.bfe_type != BTRFS_FILE_EXTENT_HOLE ||
	    !(letoh64(bmp->bm_super.incompat_flags) &
	    BTRFS_FEATURE_INCOMPAT_NO_HOLES))) {
		error = btrfs_space_replay_reserve(handle);
		if (error == 0)
			error = btrfs_insert_item(handle, tree->root, &item->key,
			    item->data, item->size);
	}
	if (error == 0 && extent.bfe_disk_bytenr != 0)
		error = btrfs_delayed_data_ref_add(handle,
		    extent.bfe_disk_bytenr, extent.bfe_disk_num_bytes,
		    btrfs_ref_data(tree->id, ino,
		    extent.bfe_logical - extent.bfe_disk_offset), 1);
	if (error == 0)
		error = log_replay_csums(handle, tree, ino, &extent);
	return (error);
}

static int
log_ref_decode(const struct btrfs_key *key, const uint8_t *data,
    uint32_t size, uint32_t offset, struct btrfs_inode_ref_record *ref)
{
	int error;

	error = btrfs_decode_inode_ref(key, data, size, offset, ref);
	if (error != 0)
		return (error);
	/* The subvolume root has one special self-reference. */
	if (key->objectid == htole64(BTRFS_FIRST_FREE_OBJECTID) &&
	    key->type == BTRFS_INODE_REF_KEY &&
	    ref->parent == BTRFS_FIRST_FREE_OBJECTID && ref->index == 0 &&
	    ref->len == 2 && ref->bytes == size && offset == 0 &&
	    memcmp(ref->name, "..", 2) == 0)
		return (0);
	return (btrfs_validate_inode_ref(key, ref));
}

static int
log_name_lookup(struct btrfs_root *root, uint64_t parent, const uint8_t *name,
    size_t len, uint64_t *ino, uint8_t *type)
{
	struct btrfs_key key = { 0 };
	struct btrfs_dir_record record;
	struct log_item *item;
	uint32_t offset;
	int error;

	key.objectid = htole64(parent);
	key.type = BTRFS_DIR_ITEM_KEY;
	key.offset = htole64(btrfs_name_hash(name, len));
	error = log_read_item(root, &key, &item);
	if (error != 0)
		return (error);
	error = ENOENT;
	for (offset = 0; offset < item->size; offset += record.size) {
		error = btrfs_decode_dir_record(item->data + offset,
		    item->size - offset, &record);
		if (error == 0)
			error = btrfs_validate_dir_record(&key,
			    root->br_view_generation, item->size, &record);
		if (error != 0)
			break;
		if (record.namelen == len &&
		    memcmp(record.name, name, len) == 0) {
			*ino = letoh64(record.item->location.objectid);
			*type = record.item->location.type;
			break;
		}
		error = ENOENT;
	}
	log_item_free(item);
	return (error);
}

static int
log_unlink(struct btrfs_trans_handle *handle, struct log_tree *tree,
    uint64_t parent, uint64_t ino, const uint8_t *name, size_t len)
{
	struct btrfs_name_plan *plan;
	uint64_t index;
	int error;

	error = btrfs_space_replay_reserve(handle);
	if (error != 0)
		return (error);
	plan = btrfs_name_plan_alloc(tree->root);
	error = btrfs_plan_remove(plan, parent, ino, (const char *)name,
	    len, &index, NULL);
	if (error == 0)
		error = btrfs_name_plan_apply(handle, plan);
	btrfs_name_plan_free(plan);
	if (error == 0)
		error = log_fixup(tree, ino);
	if (error == 0)
		error = log_fixup(tree, parent);
	return (error);
}

static int
log_link(struct btrfs_trans_handle *handle, struct log_tree *tree,
    uint64_t parent, uint64_t ino, uint64_t index,
    const uint8_t *name, size_t len)
{
	struct btrfs_name_plan *plan;
	struct btrfs_dir_item record = { 0 };
	struct btrfs_dir_record decoded;
	struct btrfs_inode inode;
	struct btrfs_key key = { 0 };
	struct log_item *old = NULL;
	uint64_t victim;
	uint8_t type;
	int error;

	/* A directory or target created after the last fsync may be absent. */
	error = btrfs_find_inode(tree->root, parent, &inode);
	if (error == ENOENT)
		return (0);
	if (error != 0 || !S_ISDIR(inode.bi_mode))
		return (error != 0 ? error : EINVAL);
	error = btrfs_find_inode(tree->root, ino, &inode);
	if (error == ENOENT)
		return (0);
	if (error != 0)
		return (error);
	record.location.objectid = htole64(ino);
	record.location.type = BTRFS_INODE_ITEM_KEY;
	record.type = btrfs_dir_type(inode.bi_mode);
	record.name_len = htole16(len);
	key.objectid = htole64(parent);
	key.type = BTRFS_DIR_INDEX_KEY;
	key.offset = htole64(index);
	error = log_read_item(tree->root, &key, &old);
	if (error == 0) {
		error = btrfs_decode_dir_record(old->data, old->size, &decoded);
		if (error == 0)
			error = btrfs_validate_dir_record(&key,
			    tree->root->br_view_generation,
			    old->size, &decoded);
		if (error == 0 &&
		    decoded.item->location.type != BTRFS_INODE_ITEM_KEY)
			error = EOPNOTSUPP;
		if (error == 0 && decoded.item->location.objectid ==
		    record.location.objectid && decoded.namelen == len &&
		    memcmp(decoded.name, name, len) == 0) {
			log_item_free(old);
			return (0);
		}
		if (error == 0)
			error = log_unlink(handle, tree, parent,
			    letoh64(decoded.item->location.objectid),
			    decoded.name, decoded.namelen);
	}
	log_item_free(old);
	if (error != 0 && error != ENOENT)
		return (error);
	error = log_name_lookup(tree->root, parent, name, len, &victim, &type);
	if (error == 0) {
		if (type != BTRFS_INODE_ITEM_KEY)
			return (EOPNOTSUPP);
		error = log_unlink(handle, tree, parent, victim, name, len);
	}
	if (error != 0 && error != ENOENT)
		return (error);
	error = btrfs_space_replay_reserve(handle);
	if (error != 0)
		return (error);
	plan = btrfs_name_plan_alloc(tree->root);
	error = btrfs_plan_add(plan, parent, ino, (const char *)name, len,
	    index, &record);
	if (error == 0)
		error = btrfs_name_plan_apply(handle, plan);
	btrfs_name_plan_free(plan);
	if (error == 0)
		error = log_fixup(tree, parent);
	if (error == 0)
		error = log_fixup(tree, ino);
	return (error);
}

/* A directory's logged index intervals are inclusive at both ends. */
static int
log_dir_deletes(struct btrfs_trans_handle *handle, struct log_tree *tree,
    uint64_t ino, uint64_t first, uint64_t last, int all)
{
	struct btrfs_path path = { 0 };
	struct btrfs_key target = { 0 }, key;
	const struct btrfs_key *found;
	struct btrfs_dir_record record;
	struct log_item *old = NULL, *logged;
	uint64_t offset;
	int error;

	target.objectid = htole64(ino);
	target.type = BTRFS_DIR_INDEX_KEY;
	target.offset = htole64(first);
	for (;;) {
		error = btrfs_search_lower_bound(tree->root, &target, &path);
		if (error == ENOENT) {
			btrfs_release_path(&path);
			return (0);
		}
		if (error == 0)
			error = btrfs_path_item(&path, &found, NULL, NULL);
		if (error == 0)
			key = *found;
		btrfs_release_path(&path);
		if (error != 0)
			return (error);
		offset = letoh64(key.offset);
		if (key.objectid != target.objectid ||
		    key.type != target.type || offset > last)
			return (0);
		error = log_read_item(tree->root, &key, &old);
		if (error != 0)
			return (error);
		logged = all ? NULL :
		    btrfs_log_find(&tree->items, ino, BTRFS_DIR_INDEX_KEY,
		    offset);
		if (logged == NULL || logged->size != old->size ||
		    memcmp(logged->data, old->data, old->size) != 0) {
			error = btrfs_decode_dir_record(old->data,
			    old->size, &record);
			if (error == 0)
				error = btrfs_validate_dir_record(&key,
				    tree->root->br_view_generation,
				    old->size, &record);
			if (error == 0 &&
			    record.item->location.type != BTRFS_INODE_ITEM_KEY)
				error = EOPNOTSUPP;
			if (error == 0)
				error = log_unlink(handle, tree, ino,
				    letoh64(record.item->location.objectid),
				    record.name, record.namelen);
		}
		log_item_free(old);
		old = NULL;
		if (error != 0 || offset == UINT64_MAX)
			return (error);
		target.offset = htole64(offset + 1);
	}
}

static int
log_replay_refs(struct btrfs_trans_handle *handle, struct log_tree *tree,
    struct log_item *item)
{
	struct log_item *old = NULL;
	struct btrfs_inode_ref_record ref, other;
	uint64_t ino = letoh64(item->key.objectid);
	uint32_t offset, pos;
	int error, found;

	if (ino == BTRFS_FIRST_FREE_OBJECTID &&
	    item->key.type == BTRFS_INODE_REF_KEY &&
	    item->key.offset == item->key.objectid)
		return (0);
	error = log_read_item(tree->root, &item->key, &old);
	if (error != 0 && error != ENOENT)
		return (error);
	/* Logged reference buckets replace the corresponding old buckets. */
	for (offset = 0; old != NULL && offset < old->size;
	    offset += ref.bytes) {
		error = log_ref_decode(&old->key, old->data, old->size,
		    offset, &ref);
		if (error != 0)
			goto out;
		found = 0;
		for (pos = 0; pos < item->size; pos += other.bytes) {
			error = log_ref_decode(&item->key, item->data,
			    item->size, pos, &other);
			if (error != 0)
				goto out;
			if (ref.parent == other.parent &&
			    ref.len == other.len &&
			    memcmp(ref.name, other.name, ref.len) == 0) {
				found = 1;
				break;
			}
		}
		if (!found) {
			error = log_unlink(handle, tree, ref.parent, ino,
			    ref.name, ref.len);
			if (error != 0)
				goto out;
		}
	}
	for (offset = 0; offset < item->size; offset += ref.bytes) {
		error = log_ref_decode(&item->key, item->data, item->size,
		    offset, &ref);
		if (error == 0)
			error = log_link(handle, tree, ref.parent, ino,
			    ref.index, ref.name, ref.len);
		if (error != 0)
			goto out;
	}
	error = 0;
out:
	log_item_free(old);
	return (error);
}

/*
 * Recompute accounting from the result, never from log estimates. Namespace
 * conflict removal can add further fixups; zero-link directories are drained
 * here before the normal mount-time orphan reaper sees their markers.
 */
static int
log_fix_inodes(struct btrfs_trans_handle *handle, struct log_tree *tree)
{
	struct log_item *fix, *item;
	struct btrfs_inode_item inode;
	struct btrfs_path path = { 0 };
	struct btrfs_key target = { 0 };
	const struct btrfs_key *key;
	const uint8_t *data;
	struct btrfs_file_extent extent;
	struct btrfs_dir_record record;
	struct btrfs_inode_ref_record ref;
	uint64_t ino, links, bytes, dirsize;
	uint32_t size, offset;
	int error;

	while ((fix = RBT_MIN(log_items, &tree->fixups)) != NULL) {
		target = fix->key;
		ino = letoh64(target.objectid);
		RBT_REMOVE(log_items, &tree->fixups, fix);
		log_item_free(fix);
		error = log_read_item(tree->root, &target, &item);
		if (error == ENOENT)
			continue;
		if (error != 0)
			return (error);
		if (item->size != sizeof(inode)) {
			log_item_free(item);
			return (EINVAL);
		}
		memcpy(&inode, item->data, sizeof(inode));
		log_item_free(item);
		links = bytes = dirsize = 0;
		target.type = BTRFS_INODE_REF_KEY;
		error = btrfs_search_lower_bound(tree->root, &target, &path);
		while (error == 0) {
			error = btrfs_path_item(&path, &key, &data, &size);
			if (error != 0 || key->objectid != target.objectid)
				break;
			if (key->type == BTRFS_INODE_REF_KEY ||
			    key->type == BTRFS_INODE_EXTREF_KEY) {
				/* Count the root's special ".." reference. */
				if (ino == BTRFS_FIRST_FREE_OBJECTID &&
				    key->type == BTRFS_INODE_REF_KEY &&
				    key->offset == key->objectid)
					links++;
				else {
					for (offset = 0; offset < size;
					    offset += ref.bytes) {
						error = log_ref_decode(key,
						    data, size, offset, &ref);
						if (error != 0)
							break;
						links++;
					}
				}
			} else if (key->type == BTRFS_EXTENT_DATA_KEY) {
				error = btrfs_decode_file_extent(
				    tree->root->br_mount,
				    tree->root->br_view_generation,
				    key, data, size, &extent);
				if (error == 0 &&
				    extent.bfe_type != BTRFS_FILE_EXTENT_HOLE) {
					if (bytes >
					    UINT64_MAX - extent.bfe_length)
						error = EOVERFLOW;
					else
						bytes += extent.bfe_length;
				}
			} else if (key->type == BTRFS_DIR_INDEX_KEY) {
				error = btrfs_decode_dir_record(data, size,
				    &record);
				if (error == 0)
					dirsize += 2 * record.namelen;
			}
			if (error != 0)
				break;
			error = btrfs_next_item(&path);
		}
		btrfs_release_path(&path);
		if (error != 0 && error != ENOENT)
			return (error);
		if (links > UINT32_MAX)
			return (EOVERFLOW);
		if (links == 0 && S_ISDIR(letoh32(inode.mode)) &&
		    dirsize != 0) {
			error = log_dir_deletes(handle, tree, ino, 0,
			    UINT64_MAX, 1);
			if (error != 0)
				return (error);
			dirsize = 0;
		}
		inode.nlink = htole32(links);
		inode.nbytes = htole64(bytes);
		inode.transid = htole64(handle->bth_transaction->bt_generation);
		if (S_ISDIR(letoh32(inode.mode)))
			inode.size = htole64(dirsize);
		target.type = BTRFS_INODE_ITEM_KEY;
		target.offset = 0;
		error = btrfs_space_replay_reserve(handle);
		if (error == 0)
			error = btrfs_replace_item(handle, tree->root, &target,
			    &inode, sizeof(inode));
		if (error == 0 && links == 0) {
			target.objectid = htole64(BTRFS_ORPHAN_OBJECTID);
			target.type = BTRFS_ORPHAN_ITEM_KEY;
			target.offset = htole64(ino);
			error = log_put(handle, tree->root, &target, NULL, 0);
		}
		if (error != 0)
			return (error);
	}
	return (0);
}

/* Validate variable-length records before making any persistent tree edits. */
static int
log_validate(struct btrfs_fs *bmp, struct log_tree *tree)
{
	struct log_item *item, *inode_item;
	const struct btrfs_inode_item *inode;
	struct btrfs_dir_record record;
	struct btrfs_file_extent extent;
	struct btrfs_inode_ref_record ref;
	uint64_t ino, extent_ino = 0, extent_end = 0, span, offset, generation;
	uint64_t csum_end = 0;
	uint32_t pos, sector = letoh32(bmp->bm_super.sectorsize);
	size_t csum = btrfs_csum_size(&bmp->bm_super);
	int error;

	generation = bmp->bm_last_transid + 1;
	RBT_FOREACH(item, log_items, &tree->items) {
		ino = letoh64(item->key.objectid);
		offset = letoh64(item->key.offset);
		if (item->key.type == BTRFS_EXTENT_CSUM_KEY) {
			span = (uint64_t)(item->size / csum) * sector;
			if (ino != BTRFS_EXTENT_CSUM_OBJECTID ||
			    item->size == 0 || item->size % csum != 0 ||
			    (offset & (sector - 1)) != 0 ||
			    offset > UINT64_MAX - span || offset < csum_end)
				return (EINVAL);
			csum_end = offset + span;
			continue;
		}
		if (ino < BTRFS_FIRST_FREE_OBJECTID ||
		    ino > BTRFS_LAST_FREE_OBJECTID)
			return (EINVAL);
		inode_item = btrfs_log_find(&tree->items, ino,
		    BTRFS_INODE_ITEM_KEY, 0);
		if (inode_item == NULL || inode_item->size != sizeof(*inode))
			return (EINVAL);
		inode = (const void *)inode_item->data;
		switch (item->key.type) {
		case BTRFS_INODE_ITEM_KEY:
			if (offset != 0 ||
			    btrfs_dir_type(letoh32(inode->mode)) ==
			    BTRFS_FT_UNKNOWN ||
			    letoh64(inode->generation) > generation ||
			    letoh64(inode->size) > INT64_MAX)
				return (EINVAL);
			break;
		case BTRFS_EXTENT_DATA_KEY:
			if (!S_ISREG(letoh32(inode->mode)) &&
			    !S_ISLNK(letoh32(inode->mode)))
				return (EINVAL);
			error = btrfs_decode_file_extent(bmp, generation,
			    &item->key, item->data, item->size, &extent);
			if (error != 0)
				return (error);
			if ((extent.bfe_type != BTRFS_FILE_EXTENT_INLINE &&
			    ((extent.bfe_logical | extent.bfe_length) &
			    (sector - 1))) ||
			    (extent_ino == ino && offset < extent_end))
				return (EINVAL);
			extent_ino = ino;
			extent_end = offset + extent.bfe_length;
			break;
		case BTRFS_INODE_REF_KEY:
		case BTRFS_INODE_EXTREF_KEY:
			if (item->size == 0)
				return (EINVAL);
			for (pos = 0; pos < item->size; pos += ref.bytes) {
				error = log_ref_decode(&item->key, item->data,
				    item->size, pos, &ref);
				if (error != 0)
					return (error);
			}
			break;
		case BTRFS_DIR_INDEX_KEY:
		case BTRFS_DIR_ITEM_KEY:
			if (!S_ISDIR(letoh32(inode->mode)) || item->size == 0)
				return (EINVAL);
			for (pos = 0; pos < item->size; pos += record.size) {
				error = btrfs_decode_dir_record(
				    item->data + pos, item->size - pos,
				    &record);
				if (error == 0)
					error = btrfs_validate_dir_record(
					    &item->key, generation,
					    item->size, &record);
				if (error != 0)
					return (error);
			}
			break;
		case BTRFS_DIR_LOG_ITEM_KEY:
		case BTRFS_DIR_LOG_INDEX_KEY:
			if (!S_ISDIR(letoh32(inode->mode)) ||
			    item->size != sizeof(struct btrfs_dir_log_item) ||
			    letoh64(((struct btrfs_dir_log_item *)
			    item->data)->end) < offset)
				return (EINVAL);
			break;
		case BTRFS_XATTR_ITEM_KEY:
			if (item->size == 0)
				return (EINVAL);
			for (pos = 0; pos < item->size; pos += record.size) {
				error = btrfs_decode_xattr(&item->key,
				    item->data + pos, item->size - pos,
				    &record);
				if (error != 0)
					return (error);
			}
			break;
		default:
			return (EOPNOTSUPP);
		}
	}
	return (0);
}

/*
 * Each phase runs across the entire forest. Inodes precede names; directory
 * deletions precede index installation; references and mappings are reconciled
 * before accounting is recomputed from the resulting trees.
 */
enum log_replay_phase {
	LOG_REPLAY_INODES,
	LOG_REPLAY_DIR_DELETES,
	LOG_REPLAY_DIR_INDEXES,
	LOG_REPLAY_ITEMS,
	LOG_REPLAY_ACCOUNTING
};

static int
log_replay_item(struct btrfs_trans_handle *handle, struct log_tree *tree,
    struct log_item *item, enum log_replay_phase phase)
{
	struct btrfs_dir_record record;
	uint64_t ino = letoh64(item->key.objectid);
	int error;

	if (log_ignored(tree, ino))
		return (0);
	/* Mutation helpers replenish reservations only when they have work. */
	switch (item->key.type) {
	case BTRFS_INODE_ITEM_KEY:
		if (phase == LOG_REPLAY_INODES)
			return (log_replay_inode(handle, tree, item));
		break;
	case BTRFS_DIR_LOG_INDEX_KEY:
		if (phase == LOG_REPLAY_DIR_DELETES)
			return (log_dir_deletes(handle, tree, ino,
			    letoh64(item->key.offset),
			    letoh64(((struct btrfs_dir_log_item *)
			    item->data)->end), 0));
		break;
	case BTRFS_DIR_INDEX_KEY:
		if (phase != LOG_REPLAY_DIR_INDEXES)
			break;
		error = btrfs_decode_dir_record(item->data, item->size,
		    &record);
		if (error == 0 &&
		    record.item->location.type == BTRFS_INODE_ITEM_KEY)
			error = log_link(handle, tree, ino,
			    letoh64(record.item->location.objectid),
			    letoh64(item->key.offset),
			    record.name, record.namelen);
		return (error);
	case BTRFS_INODE_REF_KEY:
	case BTRFS_INODE_EXTREF_KEY:
		if (phase == LOG_REPLAY_ITEMS)
			return (log_replay_refs(handle, tree, item));
		break;
	case BTRFS_EXTENT_DATA_KEY:
		if (phase == LOG_REPLAY_ITEMS)
			return (log_replay_extent(handle, tree, item));
		break;
	case BTRFS_XATTR_ITEM_KEY:
		if (phase == LOG_REPLAY_ITEMS)
			return (log_put(handle, tree->root, &item->key,
			    item->data, item->size));
		break;
	case BTRFS_DIR_ITEM_KEY:
	case BTRFS_DIR_LOG_ITEM_KEY:
		/*
		 * Older Linux logs also contain hash-keyed directory records.
		 * The index records and ranges suffice: namespace plans rebuild
		 * the corresponding hash items while replaying the indexes.
		 */
		break;
	case BTRFS_EXTENT_CSUM_KEY:
		/* Restore only checksums referenced by replayed file extents. */
		break;
	}
	return (0);
}

int
btrfs_log_recover(struct btrfs_fs *bmp, struct proc *p)
{
	struct log_trees trees;
	struct log_items roots;
	struct log_item *item;
	struct log_tree *tree;
	struct btrfs_root root = { 0 };
	const struct btrfs_root_item *disk;
	struct btrfs_trans_handle *handle = NULL;
	struct btrfs_transaction *trans = bmp->bm_transaction;
	uint64_t id, generation;
	enum log_replay_phase phase;
	int error = 0, end_error;

	if (bmp->bm_super.log_root == 0)
		return (0);
	if (bmp->bm_readonly) {
		printf("btrfs: tree log requires writable recovery\n");
		return (EROFS);
	}
	generation = trans->bt_generation;
	TAILQ_INIT(&trees);
	RBT_INIT(log_items, &roots);
	root.br_mount = bmp;
	root.br_super = &bmp->bm_super;
	root.br_owner = BTRFS_TREE_LOG_OBJECTID;
	root.br_generation = root.br_view_generation = generation;
	error = log_load_block(&root, &roots, letoh64(bmp->bm_super.log_root),
	    generation, bmp->bm_super.log_root_level, NULL, NULL);
	if (error != 0)
		goto out;
	RBT_FOREACH(item, log_items, &roots) {
		id = letoh64(item->key.offset);
		if (letoh64(item->key.objectid) != BTRFS_TREE_LOG_OBJECTID ||
		    item->key.type != BTRFS_ROOT_ITEM_KEY ||
		    !btrfs_file_tree(id) || item->size <
		    offsetof(struct btrfs_root_item, generation_v2)) {
			error = EINVAL;
			goto out;
		}
		disk = (const void *)item->data;
		tree = log_tree_get(&trees, id);
		error = btrfs_get_root(bmp, id, &tree->root);
		if (error != 0)
			goto out;
		error = log_load_block(&root, &tree->items,
		    letoh64(disk->bytenr), letoh64(disk->generation),
		    disk->level, NULL, NULL);
		if (error == 0)
			error = log_validate(bmp, tree);
		if (error != 0)
			goto out;
	}
	/* Pin the entire forest before even the first COW allocation. */
	TAILQ_FOREACH(tree, &trees, entry) {
		RBT_FOREACH(item, log_items, &tree->items) {
			if (item->key.type == BTRFS_EXTENT_DATA_KEY) {
				error = log_claim_data(bmp, tree, item);
				if (error != 0)
					goto out;
			}
		}
	}
	error = btrfs_trans_recover_begin(bmp, &handle);
	if (error != 0)
		goto out;
	for (phase = LOG_REPLAY_INODES;
	    phase <= LOG_REPLAY_ACCOUNTING && error == 0; phase++) {
		TAILQ_FOREACH(tree, &trees, entry) {
			if (phase == LOG_REPLAY_ACCOUNTING)
				error = log_fix_inodes(handle, tree);
			else {
				RBT_FOREACH(item, log_items, &tree->items) {
					error = log_replay_item(handle, tree,
					    item, phase);
					if (error != 0)
						break;
				}
			}
			if (error != 0)
				break;
		}
	}
	if (error != 0)
		btrfs_trans_abort(handle, error);
	end_error = btrfs_trans_end(handle);
	handle = NULL;
	if (error == 0)
		error = end_error;
	if (error != 0) {
		(void)btrfs_trans_finish(bmp, trans, error);
		goto out;
	}
	error = btrfs_trans_commit_closed(trans, p);
out:
	btrfs_log_free_items(&roots);
	log_free(&trees);
	return (error);
}
