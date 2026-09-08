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

/*
 * Roots are persistent filesystem-owned objects. Readers snapshot their
 * locations; writers retain root locks and COW paths from the root downward,
 * using private extent-buffer storage. Validate blocks against the path's
 * view generation, and never overwrite committed metadata.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/vnode.h>
#include <sys/btrfsio.h>

#include <btrfs/btrfs_var.h>

static void	btrfs_root_init(struct btrfs_fs *, struct btrfs_root *,
		    struct rwlock *, uint64_t,
		    const struct btrfs_root_location *);
static struct btrfs_root *
		btrfs_root_lookup(struct btrfs_fs *, uint64_t);
static void	btrfs_root_insert(struct btrfs_fs *, uint64_t,
		    const struct btrfs_root_location *);
static int	btrfs_root_dirty(struct btrfs_trans_handle *,
		    struct btrfs_root *);
static const struct btrfs_key *
		btrfs_block_key(const struct btrfs_header *, uint32_t);
static int	btrfs_read_child(struct btrfs_path *, uint8_t, uint32_t,
		    struct btrfs_extent_buffer **);
static int	btrfs_key_cmp(const struct btrfs_key *,
		    const struct btrfs_key *);
static int	btrfs_leaf_mutate(struct btrfs_path *,
		    const struct btrfs_key *, const void *, uint32_t, int);
static int	btrfs_leaf_delete_empty(struct btrfs_path *,
		    const struct btrfs_key *);
static int	btrfs_leaf_split_insert(struct btrfs_path *,
		    const struct btrfs_key *, const void *, uint32_t);
static int	btrfs_insert_split_pointers(struct btrfs_path *,
		    struct btrfs_extent_buffer **, const struct btrfs_key *,
		    uint32_t);
static int	btrfs_grow_root(struct btrfs_path *,
		    struct btrfs_extent_buffer **, const struct btrfs_key *,
		    uint32_t);
static int	btrfs_mutate_item(struct btrfs_trans_handle *,
		    struct btrfs_root *, const struct btrfs_key *, const void *,
		    uint32_t, int);

#define BTRFS_LEAF_INSERT	1
#define BTRFS_LEAF_REPLACE	2
#define BTRFS_LEAF_DELETE	3
#define BTRFS_SPLIT_MAX_RIGHTS	2

/*
 * Snapshot inspection uses immutable roots pinned by directory descriptors.
 * The control mount lock excludes deletion/finalization. No transaction,
 * vnode lock, or tree buffer survives an ioctl or a userspace write.
 */
struct btrfs_tree_reader {
	struct btrfs_ioctl_tree *args;
	struct btrfs_root *parent;
	struct btrfs_key min, max;
	uint8_t *buffer;
	uint32_t capacity;
};

static void
btrfs_tree_export_key(struct btrfs_tree_key *to, const struct btrfs_key *from)
{
	memset(to, 0, sizeof(*to));
	to->objectid = letoh64(from->objectid);
	to->type = from->type;
	to->offset = letoh64(from->offset);
}

/*
 * Find the corresponding parent pointer at this level, even when root
 * heights or child slot numbers differ. Compare addresses AND generations
 * before reading the shared child. Paths validate child key boundaries.
 */
static int
btrfs_tree_shared(struct btrfs_tree_reader *r, const struct btrfs_key *key,
    uint64_t bytenr, uint64_t gen, uint8_t level, int *shared)
{
	struct btrfs_root *root = r->parent;
	struct btrfs_path path = { 0 };
	const struct btrfs_header *header;
	const struct btrfs_key_ptr *ptr;
	uint32_t lo, hi, mid, slot;
	uint8_t l;
	int error = 0;

	*shared = 0;
	if (root == NULL || root->br_level < level)
		return (0);
	if (root->br_level == level) {
		*shared = root->br_bytenr == bytenr &&
		    root->br_generation == gen;
		return (0);
	}
	path.bp_root = root;
	path.bp_level = l = root->br_level;
	path.bp_view_generation = root->br_view_generation;
	r->args->blocks++;
	error = btrfs_extent_buffer_read(root, root->br_bytenr,
	    root->br_generation, root->br_view_generation, l, &path.bp_eb[l]);
	if (error != 0)
		goto out;
	while (l > level) {
		header = btrfs_extent_buffer_data(path.bp_eb[l]);
		ptr = (const struct btrfs_key_ptr *)(header + 1);
		lo = 0;
		hi = letoh32(header->nritems);
		if (hi == 0) {
			error = EINVAL;
			goto out;
		}
		while (lo < hi) {
			mid = lo + (hi - lo) / 2;
			if (btrfs_key_cmp(&ptr[mid].key, key) <= 0)
				lo = mid + 1;
			else
				hi = mid;
		}
		slot = lo ? lo - 1 : 0;
		path.bp_slot[l] = slot;
		if (l == level + 1) {
			*shared = letoh64(ptr[slot].blockptr) == bytenr &&
			    letoh64(ptr[slot].generation) == gen;
			break;
		}
		r->args->blocks++;
		error = btrfs_read_child(&path, l, slot, &path.bp_eb[l - 1]);
		if (error != 0)
			break;
		l--;
	}
out:
	btrfs_release_path(&path);
	return (error);
}

static int
btrfs_tree_read_block(struct btrfs_tree_reader *r, struct btrfs_path *path,
    uint8_t level)
{
	struct btrfs_ioctl_tree *args = r->args;
	struct btrfs_path old = { 0 };
	const struct btrfs_header *header;
	const struct btrfs_key_ptr *ptr;
	const struct btrfs_item *items;
	const struct btrfs_key *key;
	const uint8_t *data, *previous;
	struct btrfs_tree_item record;
	uint32_t i, n, size, oldsize, bytes;
	int error = 0, shared;

	header = btrfs_extent_buffer_data(path->bp_eb[level]);
	n = letoh32(header->nritems);
	ptr = (const struct btrfs_key_ptr *)(header + 1);
	items = (const struct btrfs_item *)(header + 1);
	for (i = 0; i < n; i++) {
		key = btrfs_block_key(header, i);
		if (btrfs_key_cmp(key, &r->max) > 0)
			break;
		if (level != 0) {
			if (i + 1 < n &&
			    btrfs_key_cmp(&ptr[i + 1].key, &r->min) <= 0)
				continue;
			error = btrfs_tree_shared(r, key,
			    letoh64(ptr[i].blockptr), letoh64(ptr[i].generation),
			    level - 1, &shared);
			if (error != 0)
				break;
			if (shared) {
				args->shared++;
				continue;
			}
			path->bp_slot[level] = i;
			args->blocks++;
			error = btrfs_read_child(path, level, i,
			    &path->bp_eb[level - 1]);
			if (error != 0)
				break;
			error = btrfs_tree_read_block(r, path, level - 1);
			btrfs_extent_buffer_put(path->bp_eb[level - 1]);
			path->bp_eb[level - 1] = NULL;
			if (error != 0)
				break;
			continue;
		}
		if (btrfs_key_cmp(key, &r->min) < 0)
			continue;
		args->items++;
		size = letoh32(items[i].size);
		data = (const uint8_t *)(header + 1) +
		    letoh32(items[i].offset);
		if (r->parent != NULL) {
			error = btrfs_search_slot(r->parent, key, &old);
			args->blocks += r->parent->br_level + 1;
			shared = 0;
			if (error == 0) {
				error = btrfs_path_item(&old, NULL, &previous,
				    &oldsize);
				if (error == 0)
					shared = size == oldsize &&
					    memcmp(data, previous, size) == 0;
			} else if (error == ENOENT)
				error = 0;
			btrfs_release_path(&old);
			if (error != 0)
				break;
			if (shared)
				continue;
		}
		memset(&record, 0, sizeof(record));
		btrfs_tree_export_key(&record.key, key);
		record.size = args->flags & BTRFS_TREE_KEYS ? 0 : size;
		bytes = roundup(sizeof(record) + record.size, 8);
		if (bytes > r->capacity - args->size) {
			args->min = record.key;
			args->done = 0;
			return (args->size == 0 ? ENOBUFS : EAGAIN);
		}
		memcpy(r->buffer + args->size, &record, sizeof(record));
		memcpy(r->buffer + args->size + sizeof(record), data, record.size);
		args->size += bytes;
	}
	return (error);
}

int
btrfs_read_tree_items(struct btrfs_root *root, struct btrfs_root *parent,
    struct btrfs_ioctl_tree *args, void *buffer)
{
	struct btrfs_tree_reader r = { .args = args, .parent = parent,
	    .buffer = buffer, .capacity = args->size };
	struct btrfs_path path = { 0 };
	int error;

	r.min.objectid = htole64(args->min.objectid);
	r.min.type = args->min.type;
	r.min.offset = htole64(args->min.offset);
	r.max.objectid = htole64(args->max.objectid);
	r.max.type = args->max.type;
	r.max.offset = htole64(args->max.offset);
	if (btrfs_key_cmp(&r.min, &r.max) > 0)
		return (EINVAL);
	args->size = 0;
	args->blocks = args->shared = args->items = 0;
	args->done = 1;
	args->sectorsize = letoh32(root->br_super->sectorsize);
	if (parent != NULL && root->br_level == parent->br_level &&
	    root->br_bytenr == parent->br_bytenr &&
	    root->br_generation == parent->br_generation) {
		args->shared++;
		return (0);
	}
	path.bp_root = root;
	path.bp_level = root->br_level;
	path.bp_view_generation = root->br_view_generation;
	args->blocks++;
	error = btrfs_extent_buffer_read(root, root->br_bytenr,
	    root->br_generation, root->br_view_generation, root->br_level,
	    &path.bp_eb[root->br_level]);
	if (error == 0)
		error = btrfs_tree_read_block(&r, &path, root->br_level);
	btrfs_release_path(&path);
	return (error == EAGAIN ? 0 : error);
}

static void
btrfs_root_init(struct btrfs_fs *bmp, struct btrfs_root *root,
    struct rwlock *lock, uint64_t owner,
    const struct btrfs_root_location *location)
{
	memset(root, 0, sizeof(*root));
	root->br_mount = bmp;
	root->br_devvp = bmp->bm_devvp;
	root->br_dev = NODEV;
	root->br_super = &bmp->bm_super;
	root->br_lock = lock;
	root->br_bytenr = location->brl_bytenr;
	root->br_generation = location->brl_generation;
	root->br_view_generation = letoh64(bmp->bm_super.generation);
	root->br_owner = owner;
	root->br_level = location->brl_level;
}

static struct btrfs_root *
btrfs_root_lookup(struct btrfs_fs *bmp, uint64_t owner)
{
	struct btrfs_root_entry *entry;
	struct btrfs_root *root = NULL;

	mtx_enter(&bmp->bm_rootmtx);
	LIST_FOREACH(entry, &bmp->bm_roots, bre_entry) {
		if (entry->bre_root.br_owner == owner) {
			root = &entry->bre_root;
			break;
		}
	}
	mtx_leave(&bmp->bm_rootmtx);
	return (root);
}

static void
btrfs_root_insert(struct btrfs_fs *bmp, uint64_t owner,
    const struct btrfs_root_location *location)
{
	struct btrfs_root_entry *entry;

	if (location->brl_bytenr == 0)
		return;
	entry = malloc(sizeof(*entry), M_BTRFS, M_WAITOK | M_ZERO);
	rw_init_flags(&entry->bre_lock, "btrfsroot", RWL_DUPOK);
	btrfs_root_init(bmp, &entry->bre_root, &entry->bre_lock, owner,
	    location);
	LIST_INSERT_HEAD(&bmp->bm_roots, entry, bre_entry);
}

void
btrfs_init_roots(struct btrfs_fs *bmp,
    const struct btrfs_bootstrap *bootstrap)
{
	struct btrfs_root_location location;
	struct btrfs_root *root;

	LIST_INIT(&bmp->bm_roots);
	mtx_init(&bmp->bm_rootmtx, IPL_NONE);

	location.brl_bytenr = letoh64(bmp->bm_super.root);
	location.brl_generation = letoh64(bmp->bm_super.generation);
	location.brl_level = bmp->bm_super.root_level;
	btrfs_root_insert(bmp, BTRFS_ROOT_TREE_OBJECTID, &location);

	location.brl_bytenr = letoh64(bmp->bm_super.chunk_root);
	location.brl_generation =
	    letoh64(bmp->bm_super.chunk_root_generation);
	location.brl_level = bmp->bm_super.chunk_root_level;
	btrfs_root_insert(bmp, BTRFS_CHUNK_TREE_OBJECTID, &location);

	location.brl_bytenr = bootstrap->bb_fs_root;
	location.brl_generation = bootstrap->bb_fs_root_generation;
	location.brl_level = bootstrap->bb_fs_root_level;
	btrfs_root_insert(bmp, BTRFS_FS_TREE_OBJECTID, &location);
	root = btrfs_root_lookup(bmp, BTRFS_FS_TREE_OBJECTID);
	root->br_flags = bootstrap->bb_fs_root_flags;

	location.brl_bytenr = bootstrap->bb_csum_root;
	location.brl_generation = bootstrap->bb_csum_root_generation;
	location.brl_level = bootstrap->bb_csum_root_level;
	btrfs_root_insert(bmp, BTRFS_CSUM_TREE_OBJECTID, &location);

	btrfs_root_insert(bmp, BTRFS_EXTENT_TREE_OBJECTID,
	    &bootstrap->bb_extent_root);
	btrfs_root_insert(bmp, BTRFS_DEV_TREE_OBJECTID,
	    &bootstrap->bb_dev_root);
	btrfs_root_insert(bmp, BTRFS_FREE_SPACE_TREE_OBJECTID,
	    &bootstrap->bb_free_space_root);
	btrfs_root_insert(bmp, BTRFS_BLOCK_GROUP_TREE_OBJECTID,
	    &bootstrap->bb_block_group_root);
}

void
btrfs_free_roots(struct btrfs_fs *bmp)
{
	struct btrfs_root_entry *entry;

	while ((entry = LIST_FIRST(&bmp->bm_roots)) != NULL) {
		LIST_REMOVE(entry, bre_entry);
		rw_assert_unlocked(&entry->bre_lock);
		KASSERT(entry->bre_root.br_transaction == NULL);
		free(entry, M_BTRFS, sizeof(*entry));
	}
}

static int
btrfs_root_dirty(struct btrfs_trans_handle *handle, struct btrfs_root *root)
{
	struct btrfs_transaction *trans = handle->bth_transaction;
	struct btrfs_dirty_root *dirty;

	rw_assert_wrlock(root->br_lock);
	if (root->br_transaction == trans)
		return (0);
	if (root->br_transaction != NULL)
		return (EBUSY);

	dirty = malloc(sizeof(*dirty), M_BTRFS, M_WAITOK | M_ZERO);
	dirty->bdr_root = root;
	dirty->bdr_old_location.brl_bytenr = root->br_bytenr;
	dirty->bdr_old_location.brl_generation = root->br_generation;
	dirty->bdr_old_location.brl_level = root->br_level;
	dirty->bdr_old_view_generation = root->br_view_generation;

	mtx_enter(&trans->bt_lock);
	TAILQ_INSERT_TAIL(&trans->bt_dirty_roots, dirty, bdr_entry);
	root->br_transaction = trans;
	mtx_leave(&trans->bt_lock);
	return (0);
}

int
btrfs_roots_finish(struct btrfs_transaction *trans, int committed)
{
	struct btrfs_dirty_root *dirty;
	struct btrfs_root *root;

	KASSERT(trans->bt_writers == 0);
	KASSERT(!trans->bt_commit_handle);
	while ((dirty = TAILQ_FIRST(&trans->bt_dirty_roots)) != NULL) {
		TAILQ_REMOVE(&trans->bt_dirty_roots, dirty, bdr_entry);
		root = dirty->bdr_root;
		rw_enter_write(root->br_lock);
		KASSERT(root->br_transaction == trans);
		KASSERT(!committed ||
		    (root->br_generation == trans->bt_generation &&
		    root->br_view_generation == trans->bt_generation));
		if (!committed) {
			root->br_bytenr =
			    dirty->bdr_old_location.brl_bytenr;
			root->br_generation =
			    dirty->bdr_old_location.brl_generation;
			root->br_level = dirty->bdr_old_location.brl_level;
			root->br_view_generation =
			    dirty->bdr_old_view_generation;
		}
		root->br_transaction = NULL;
		rw_exit_write(root->br_lock);
		free(dirty, M_BTRFS, sizeof(*dirty));
	}
	return (0);
}

int
btrfs_update_dirty_root_items(struct btrfs_trans_handle *handle)
{
	struct btrfs_transaction *trans;
	struct btrfs_dirty_root *dirty;
	struct btrfs_root *root, *root_tree;
	struct btrfs_root_item *item;
	struct btrfs_path path = { 0 };
	struct btrfs_key key;
	const uint8_t *data;
	uint8_t *payload;
	uint32_t size;
	int error;

	if (handle == NULL || handle->bth_transaction == NULL ||
	    !handle->bth_commit)
		return (EINVAL);
	trans = handle->bth_transaction;
	error = btrfs_get_root(trans->bt_mount, BTRFS_ROOT_TREE_OBJECTID,
	    &root_tree);
	if (error != 0)
		return (error);
	TAILQ_FOREACH(dirty, &trans->bt_dirty_roots, bdr_entry) {
		root = dirty->bdr_root;
		if (root->br_owner == BTRFS_ROOT_TREE_OBJECTID ||
		    root->br_owner == BTRFS_CHUNK_TREE_OBJECTID)
			continue;
		memset(&key, 0, sizeof(key));
		key.objectid = htole64(root->br_owner);
		key.type = BTRFS_ROOT_ITEM_KEY;
		key.offset = htole64(root->br_root_offset);
		error = btrfs_search_slot(root_tree, &key, &path);
		if (error != 0)
			goto out;
		error = btrfs_path_item(&path, NULL, &data, &size);
		if (error != 0)
			goto out;
		if (size < offsetof(struct btrfs_root_item, generation_v2)) {
			error = EOPNOTSUPP;
			goto out;
		}
		payload = malloc(size, M_BTRFS, M_WAITOK);
		memcpy(payload, data, size);
		btrfs_release_path(&path);

		item = (struct btrfs_root_item *)payload;
		item->bytenr = htole64(root->br_bytenr);
		item->generation = htole64(root->br_generation);
		item->level = root->br_level;
		if (size >= offsetof(struct btrfs_root_item, generation_v2) +
		    sizeof(item->generation_v2))
			item->generation_v2 = htole64(root->br_generation);
		error = btrfs_replace_item(handle, root_tree, &key, payload,
		    size);
		free(payload, M_BTRFS, size);
		if (error != 0)
			return (error);
	}
	return (0);
out:
	btrfs_release_path(&path);
	return (error);
}

int
btrfs_get_root(struct btrfs_fs *bmp, uint64_t owner,
    struct btrfs_root **rootp)
{
	struct btrfs_root_item item;
	struct btrfs_root_location location;
	struct btrfs_root_entry *entry, *new;
	struct btrfs_root *root, *root_tree;
	uint64_t offset;
	int error;

	*rootp = NULL;
	root = btrfs_root_lookup(bmp, owner);
	if (root != NULL) {
		if (root->br_deleted)
			return (ENOENT);
		*rootp = root;
		return (0);
	}

	root_tree = btrfs_root_lookup(bmp, BTRFS_ROOT_TREE_OBJECTID);
	KASSERT(root_tree != NULL);
	error = btrfs_find_root_item(root_tree, owner,
	    btrfs_file_tree(owner) ? BTRFS_FIRST_FREE_OBJECTID : 0, &item, &offset);
	if (error != 0)
		return (error);
	location.brl_bytenr = letoh64(item.bytenr);
	location.brl_generation = letoh64(item.generation);
	location.brl_level = item.level;

	new = malloc(sizeof(*new), M_BTRFS, M_WAITOK | M_ZERO);
	rw_init_flags(&new->bre_lock, "btrfsroot", RWL_DUPOK);
	btrfs_root_init(bmp, &new->bre_root, &new->bre_lock, owner,
	    &location);
	new->bre_root.br_flags = letoh64(item.flags);
	new->bre_root.br_root_offset = offset;
	mtx_enter(&bmp->bm_rootmtx);
	LIST_FOREACH(entry, &bmp->bm_roots, bre_entry) {
		if (entry->bre_root.br_owner == owner)
			break;
	}
	if (entry == NULL) {
		LIST_INSERT_HEAD(&bmp->bm_roots, new, bre_entry);
		entry = new;
		new = NULL;
	}
	root = &entry->bre_root;
	mtx_leave(&bmp->bm_rootmtx);
	if (new != NULL)
		free(new, M_BTRFS, sizeof(*new));
	*rootp = root;
	return (0);
}

void
btrfs_forget_root(struct btrfs_fs *bmp, uint64_t owner)
{
	struct btrfs_root_entry *entry;

	mtx_enter(&bmp->bm_rootmtx);
	LIST_FOREACH(entry, &bmp->bm_roots, bre_entry)
		if (entry->bre_root.br_owner == owner)
			break;
	if (entry != NULL)
		entry->bre_root.br_deleted = 1;
	mtx_leave(&bmp->bm_rootmtx);
}

/* Change the references carried by one block, without traversing children. */
int
btrfs_block_children(struct btrfs_trans_handle *handle,
    const struct btrfs_extent_buffer *eb, uint64_t owner, int full, int delta)
{
	const struct btrfs_header *header = btrfs_extent_buffer_data(eb);
	const struct btrfs_key_ptr *ptr;
	const struct btrfs_item *items;
	const struct btrfs_file_extent_item *fi;
	const uint8_t *data;
	uint64_t disk, base;
	uint32_t i, n = letoh32(header->nritems), size;
	int error = 0;

	if (eb->eb_level != 0) {
		ptr = (const struct btrfs_key_ptr *)(header + 1);
		for (i = 0; i < n; i++) {
			error = btrfs_delayed_ref_add(handle,
			    letoh64(ptr[i].blockptr), full ?
			    btrfs_ref_shared(eb->eb_bytenr) :
			    btrfs_ref_tree(owner), eb->eb_level - 1, delta);
			if (error != 0)
				return (error);
		}
	} else {
		items = (const struct btrfs_item *)(header + 1);
		for (i = 0; i < n; i++) {
			size = letoh32(items[i].size);
			data = (const uint8_t *)(header + 1) +
			    letoh32(items[i].offset);
			if (items[i].key.type != BTRFS_EXTENT_DATA_KEY)
				continue;
			if (size < offsetof(struct btrfs_file_extent_item,
			    disk_bytenr)) {
				return (EINVAL);
			}
			fi = (const struct btrfs_file_extent_item *)data;
			if (fi->type == BTRFS_FILE_EXTENT_INLINE)
				continue;
			if (size != sizeof(*fi) ||
			    (fi->type != BTRFS_FILE_EXTENT_REG &&
			    fi->type != BTRFS_FILE_EXTENT_PREALLOC)) {
				return (EINVAL);
			}
			disk = letoh64(fi->disk_bytenr);
			if (disk == 0)
				continue;
			base = letoh64(items[i].key.offset) -
			    letoh64(fi->offset);
			error = btrfs_delayed_data_ref_add(handle, disk,
			    letoh64(fi->disk_num_bytes), full ?
			    btrfs_ref_shared(eb->eb_bytenr) :
			    btrfs_ref_data(owner, letoh64(items[i].key.objectid),
			    base), delta);
			if (error != 0)
				return (error);
		}
	}
	return (0);
}

/*
 * Deletion first converts outgoing references belonging to the disappearing
 * owner to full references. Then it drops only edges whose parent actually
 * dies. Shared subtrees retain their children and data references.
 */
static int
btrfs_walk_drop(struct btrfs_root *root, uint64_t bytenr, uint64_t gen,
    uint8_t level, struct btrfs_trans_handle *handle, uint64_t parent,
    int operation, uint64_t *blocks, uint64_t *nrefs)
{
	struct btrfs_extent_buffer *eb;
	const struct btrfs_header *header;
	const struct btrfs_key_ptr *ptr;
	const struct btrfs_item *items;
	const struct btrfs_file_extent_item *fi;
	uint64_t refs = 1, flags = 0;
	uint32_t i, n;
	int error, full;

	error = btrfs_extent_buffer_read(root, bytenr, gen,
	    root->br_view_generation, level, &eb);
	if (error != 0)
		return (error);
	header = btrfs_extent_buffer_data(eb);
	n = letoh32(header->nritems);
	(*blocks)++;
	if (level != 0)
		*nrefs += n;
	else {
		items = (const struct btrfs_item *)(header + 1);
		for (i = 0; i < n; i++) {
			if (items[i].key.type != BTRFS_EXTENT_DATA_KEY)
				continue;
			if (letoh32(items[i].size) <
			    offsetof(struct btrfs_file_extent_item, disk_bytenr)) {
				error = EINVAL;
				goto out;
			}
			fi = (const struct btrfs_file_extent_item *)
			    ((const uint8_t *)(header + 1) +
			    letoh32(items[i].offset));
			if (fi->type == BTRFS_FILE_EXTENT_INLINE)
				continue;
			if (letoh32(items[i].size) != sizeof(*fi)) {
				error = EINVAL;
				goto out;
			}
			if (fi->disk_bytenr != 0)
				(*nrefs)++;
		}
	}
	if (handle != NULL) {
		error = btrfs_block_refs(handle, eb, &refs, &flags);
		if (error != 0)
			goto out;
		full = (flags & BTRFS_BLOCK_FLAG_FULL_BACKREF) != 0;
		if (operation == 1 && eb->eb_owner == root->br_owner &&
		    !full) {
			error = btrfs_block_children(handle, eb, root->br_owner,
			    1, 1);
			if (error == 0)
				error = btrfs_block_children(handle, eb,
				    root->br_owner, 0, -1);
			if (error == 0)
				error = btrfs_ref_convert_full(handle, eb);
			if (error != 0)
				goto out;
			flags |= BTRFS_BLOCK_FLAG_FULL_BACKREF;
		}
		if (operation == -1 && refs > 1)
			goto drop;
	}
	if (level != 0) {
		ptr = (const struct btrfs_key_ptr *)(header + 1);
		for (i = 0; i < n; i++) {
			error = btrfs_walk_drop(root, letoh64(ptr[i].blockptr),
			    letoh64(ptr[i].generation), level - 1, handle,
			    flags & BTRFS_BLOCK_FLAG_FULL_BACKREF ? bytenr : 0,
			    operation, blocks, nrefs);
			if (error != 0)
				goto out;
		}
	} else if (operation == -1) {
		error = btrfs_block_children(handle, eb, root->br_owner,
		    (flags & BTRFS_BLOCK_FLAG_FULL_BACKREF) != 0, -1);
		if (error != 0)
			goto out;
	}
drop:
	if (operation == -1)
		error = btrfs_delayed_ref_add(handle, bytenr, parent != 0 ?
		    btrfs_ref_shared(parent) : btrfs_ref_tree(root->br_owner),
		    level, -1);
out:
	btrfs_extent_buffer_put(eb);
	return (error);
}

int
btrfs_count_tree(struct btrfs_root *root, uint64_t *blocks, uint64_t *refs)
{
	*blocks = *refs = 0;
	return (btrfs_walk_drop(root, root->br_bytenr, root->br_generation,
	    root->br_level, NULL, 0, 0, blocks, refs));
}

int
btrfs_drop_subvolume_tree(struct btrfs_trans_handle *handle,
    struct btrfs_root *root)
{
	uint64_t blocks = 0, refs = 0;
	int error;

	error = btrfs_walk_drop(root, root->br_bytenr, root->br_generation,
	    root->br_level, handle, 0, 1, &blocks, &refs);
	if (error == 0)
		error = btrfs_walk_drop(root, root->br_bytenr,
		    root->br_generation, root->br_level, handle, 0,
		    -1, &blocks, &refs);
	return (error);
}

/* As on Linux, a snapshot gets a new root block and shares everything below. */
int
btrfs_new_subvolume_root(struct btrfs_trans_handle *handle,
    struct btrfs_root *root, uint64_t owner, struct btrfs_root_item *item,
    int snapshot)
{
	struct btrfs_extent_buffer *eb, *copy;
	struct btrfs_header *header;
	struct btrfs_item *items;
	struct btrfs_inode_ref *ref;
	uint64_t bytenr;
	uint32_t end, nodesize = letoh32(root->br_super->nodesize);
	int error;

	error = btrfs_extent_buffer_read(root, root->br_bytenr,
	    root->br_generation, root->br_view_generation,
	    root->br_level, &eb);
	if (error != 0)
		return (error);
	rw_exit_read(&eb->eb_lock);
	rw_enter_write(&eb->eb_lock);
	error = btrfs_extent_buffer_clone(handle, eb, &copy);
	btrfs_extent_buffer_put(eb);
	if (error != 0)
		return (error);
	header = btrfs_extent_buffer_data_mutable(handle, copy);
	header->owner = htole64(owner);
	copy->eb_owner = owner;
	bytenr = copy->eb_bytenr;
	if (snapshot) {
		error = btrfs_block_children(handle, copy, owner, 0, 1);
		if (error == 0)
			error = btrfs_delayed_ref_add(handle, bytenr,
			    btrfs_ref_tree(owner), copy->eb_level, 1);
		item->level = copy->eb_level;
		btrfs_extent_buffer_put(copy);
		if (error != 0)
			return (error);
		goto done;
	}
	memset(header + 1, 0, nodesize - sizeof(*header));
	header->level = 0;
	header->nritems = htole32(2);
	copy->eb_level = 0;
	items = (struct btrfs_item *)(header + 1);
	end = nodesize - sizeof(*header) - sizeof(item->inode);
	items[0].key.objectid = htole64(BTRFS_FIRST_FREE_OBJECTID);
	items[0].key.type = BTRFS_INODE_ITEM_KEY;
	items[0].offset = htole32(end);
	items[0].size = htole32(sizeof(item->inode));
	memcpy((uint8_t *)(header + 1) + end, &item->inode,
	    sizeof(item->inode));
	end -= sizeof(*ref) + 2;
	items[1].key.objectid = items[0].key.objectid;
	items[1].key.type = BTRFS_INODE_REF_KEY;
	items[1].key.offset = items[0].key.objectid;
	items[1].offset = htole32(end);
	items[1].size = htole32(sizeof(*ref) + 2);
	ref = (struct btrfs_inode_ref *)((uint8_t *)(header + 1) + end);
	ref->name_len = htole16(2);
	memcpy(ref + 1, "..", 2);
	error = btrfs_delayed_ref_add(handle, bytenr, btrfs_ref_tree(owner),
	    0, 1);
	btrfs_extent_buffer_put(copy);
	if (error != 0)
		return (error);
	item->level = 0;
	item->bytes_used = htole64(nodesize);
done:
	item->bytenr = htole64(bytenr);
	item->generation = item->generation_v2 =
	    htole64(handle->bth_transaction->bt_generation);
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
	    path->bp_view_generation, parent_level - 1, ebp);
	if (error != 0)
		return (error);
	if (path->bp_write) {
		rw_exit_read(&(*ebp)->eb_lock);
		rw_enter_write(&(*ebp)->eb_lock);
		error = btrfs_cow_block(path->bp_handle, path->bp_root,
		    path->bp_eb[parent_level], slot, ebp);
		if (error != 0) {
			btrfs_extent_buffer_put(*ebp);
			*ebp = NULL;
			return (error);
		}
	}

	child = btrfs_extent_buffer_data(*ebp);
	nritems = letoh32(child->nritems);
	if (nritems == 0) {
		if (path->bp_write)
			btrfs_trans_abort(path->bp_handle, EINVAL);
		btrfs_extent_buffer_put(*ebp);
		*ebp = NULL;
		return (EINVAL);
	}
	first = btrfs_block_key(child, 0);
	last = btrfs_block_key(child, nritems - 1);
	if (slot + 1 < letoh32(parent->nritems))
		upper = &ptrs[slot + 1].key;
	for (level = parent_level + 1;
	    upper == NULL && level <= path->bp_level; level++) {
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
		if (path->bp_write)
			btrfs_trans_abort(path->bp_handle, EINVAL);
		btrfs_extent_buffer_put(*ebp);
		*ebp = NULL;
		return (EINVAL);
	}
	return (0);
}

void
btrfs_release_path(struct btrfs_path *path)
{
	struct btrfs_root *root = path->bp_root;
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
	path->bp_handle = NULL;
	path->bp_view_generation = 0;
	path->bp_level = 0;
	if (path->bp_write) {
		KASSERT(root != NULL);
		KASSERT(root->br_lock != NULL);
		rw_exit_write(root->br_lock);
		path->bp_write = 0;
	}
#ifdef DIAGNOSTIC
	for (level = 0; level < BTRFS_MAX_LEVEL; level++)
		KASSERT(path->bp_eb[level] == NULL);
#endif
}

int
btrfs_cow_block(struct btrfs_trans_handle *handle, struct btrfs_root *root,
    struct btrfs_extent_buffer *parent, uint32_t parent_slot,
    struct btrfs_extent_buffer **ebp)
{
	struct btrfs_transaction *trans;
	struct btrfs_extent_buffer *source, *cow;
	struct btrfs_header *parent_header;
	struct btrfs_key_ptr *ptrs;
	uint64_t refs, flags;
	uint32_t nritems;
	int error;

	if (handle == NULL || root == NULL || ebp == NULL || *ebp == NULL ||
	    root->br_lock == NULL)
		return (EINVAL);
	trans = handle->bth_transaction;
	source = *ebp;
	if (trans == NULL || root->br_mount != trans->bt_mount ||
	    source->eb_mount != trans->bt_mount ||
	    (source->eb_owner != root->br_owner &&
	    !(btrfs_file_tree(root->br_owner) &&
	    btrfs_file_tree(source->eb_owner))))
		return (EINVAL);
	rw_assert_wrlock(root->br_lock);
	rw_assert_wrlock(&source->eb_lock);

	if (parent == NULL) {
		if (source->eb_level != root->br_level ||
		    source->eb_bytenr != root->br_bytenr ||
		    source->eb_generation != root->br_generation)
			return (EINVAL);
	} else {
		rw_assert_wrlock(&parent->eb_lock);
		if (parent->eb_transaction != trans ||
		    parent->eb_generation != trans->bt_generation ||
		    parent->eb_level != source->eb_level + 1)
			return (EINVAL);
		parent_header = btrfs_extent_buffer_data_mutable(handle,
		    parent);
		if (parent_header == NULL)
			return (EINVAL);
		nritems = letoh32(parent_header->nritems);
		if (parent_slot >= nritems)
			return (EINVAL);
		ptrs = (struct btrfs_key_ptr *)(parent_header + 1);
		if (letoh64(ptrs[parent_slot].blockptr) !=
		    source->eb_bytenr ||
		    letoh64(ptrs[parent_slot].generation) !=
		    source->eb_generation)
			return (EINVAL);
	}

	if (source->eb_transaction == trans) {
		if (source->eb_generation != trans->bt_generation ||
		    source->eb_private == NULL || !source->eb_dirty ||
		    (parent == NULL && root->br_transaction != trans))
			return (EINVAL);
		return (0);
	}
	if (source->eb_transaction != NULL ||
	    source->eb_generation >= trans->bt_generation)
		return (EINVAL);

	error = btrfs_extent_buffer_clone(handle, source, &cow);
	if (error != 0)
		return (error);

	/*
	 * New blocks use implicit references owned by this root. When the
	 * original owner leaves a shared block, preserve its outgoing edges
	 * as full backreferences; its implicit edges now belong to the copy.
	 * Other owners add their own edges. A final full block transfers its
	 * outgoing edges back to the new owner's implicit representation.
	 */
	if (btrfs_file_tree(root->br_owner)) {
		error = btrfs_block_refs(handle, source, &refs, &flags);
		if (error == 0 && refs > 1 &&
		    source->eb_owner == root->br_owner &&
		    !(flags & BTRFS_BLOCK_FLAG_FULL_BACKREF)) {
			error = btrfs_block_children(handle, source,
			    root->br_owner, 1, 1);
			if (error == 0)
				error = btrfs_ref_convert_full(handle, source);
		} else if (error == 0 && (refs > 1 ||
		    (flags & BTRFS_BLOCK_FLAG_FULL_BACKREF))) {
			error = btrfs_block_children(handle, cow,
			    root->br_owner, 0, 1);
			if (error == 0 && refs == 1)
				error = btrfs_block_children(handle, source,
				    root->br_owner, 1, -1);
		}
	}
	if (error == 0) {
		struct btrfs_header *header =
		    btrfs_extent_buffer_data_mutable(handle, cow);
		header->owner = htole64(root->br_owner);
		cow->eb_owner = root->br_owner;
		error = btrfs_delayed_ref_add(handle, cow->eb_bytenr,
		    btrfs_ref_tree(root->br_owner), cow->eb_level, 1);
	}
	if (error == 0)
		error = btrfs_delayed_ref_add(handle, source->eb_bytenr,
		    btrfs_ref_tree(root->br_owner), source->eb_level, -1);
	if (error != 0) {
		btrfs_trans_abort(handle, error);
		btrfs_extent_buffer_put(cow);
		return (error);
	}

	if (parent == NULL) {
		error = btrfs_root_dirty(handle, root);
		if (error != 0) {
			btrfs_trans_abort(handle, error);
			btrfs_extent_buffer_put(cow);
			return (error);
		}
		root->br_bytenr = cow->eb_bytenr;
		root->br_generation = trans->bt_generation;
		root->br_view_generation = trans->bt_generation;
		root->br_level = cow->eb_level;
	} else {
		parent_header = btrfs_extent_buffer_data_mutable(handle,
		    parent);
		KASSERT(parent_header != NULL);
		ptrs = (struct btrfs_key_ptr *)(parent_header + 1);
		ptrs[parent_slot].blockptr = htole64(cow->eb_bytenr);
		ptrs[parent_slot].generation =
		    htole64(trans->bt_generation);
	}

	btrfs_extent_buffer_put(source);
	*ebp = cow;
	return (0);
}

int
btrfs_search_slot(struct btrfs_root *root, const struct btrfs_key *target,
    struct btrfs_path *path)
{
	const struct btrfs_header *header;
	const struct btrfs_key_ptr *ptrs;
	const struct btrfs_item *items;
	uint64_t bytenr, generation, view_generation;
	uint32_t high, low, mid, slot;
	uint8_t level;
	int cmp, error;

	if (path->bp_root != NULL)
		btrfs_release_path(path);

	if (root->br_lock != NULL)
		rw_enter_read(root->br_lock);
	bytenr = root->br_bytenr;
	generation = root->br_generation;
	view_generation = root->br_view_generation;
	level = root->br_level;
	if (root->br_lock != NULL)
		rw_exit_read(root->br_lock);
	if (level >= BTRFS_MAX_LEVEL)
		return (EINVAL);
	path->bp_root = root;
	path->bp_view_generation = view_generation;
	path->bp_level = level;
	error = btrfs_extent_buffer_read(root, bytenr, generation,
	    view_generation, level, &path->bp_eb[level]);
	if (error != 0)
		goto fail;

	for (; level != 0; level--) {
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
btrfs_search_slot_write(struct btrfs_trans_handle *handle,
    struct btrfs_root *root, const struct btrfs_key *target,
    struct btrfs_path *path)
{
	const struct btrfs_header *header;
	const struct btrfs_key_ptr *ptrs;
	const struct btrfs_item *items;
	struct btrfs_transaction *trans;
	uint64_t bytenr, generation;
	uint32_t high, low, mid, slot;
	uint8_t level;
	int cmp, error;

	if (handle == NULL || handle->bth_transaction == NULL ||
	    root == NULL || root->br_lock == NULL)
		return (EINVAL);
	trans = handle->bth_transaction;
	if (root->br_mount != trans->bt_mount)
		return (EINVAL);
	if (path->bp_root != NULL)
		btrfs_release_path(path);

	rw_enter_write(root->br_lock);
	path->bp_root = root;
	path->bp_handle = handle;
	path->bp_write = 1;
	bytenr = root->br_bytenr;
	generation = root->br_generation;
	level = root->br_level;
	if (root->br_transaction != NULL &&
	    root->br_transaction != trans) {
		error = EBUSY;
		goto fail;
	}
	if (generation == 0 || generation > trans->bt_generation ||
	    level >= BTRFS_MAX_LEVEL) {
		error = EINVAL;
		goto fail;
	}
	path->bp_view_generation = trans->bt_generation;
	path->bp_level = level;
	error = btrfs_extent_buffer_read(root, bytenr, generation,
	    trans->bt_generation, level, &path->bp_eb[level]);
	if (error != 0)
		goto fail;
	rw_exit_read(&path->bp_eb[level]->eb_lock);
	rw_enter_write(&path->bp_eb[level]->eb_lock);
	error = btrfs_cow_block(handle, root, NULL, 0,
	    &path->bp_eb[level]);
	if (error != 0)
		goto fail;

	for (; level != 0; level--) {
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

/*
 * Removing the final item in a leaf can detach a whole COW path.  Retire its
 * allocations after delayed-reference processing, and keep the leaf as an
 * empty root when no sibling survives.
 */
static int
btrfs_leaf_delete_empty(struct btrfs_path *path,
    const struct btrfs_key *key)
{
	struct btrfs_trans_handle *handle = path->bp_handle;
	struct btrfs_root *root = path->bp_root;
	struct btrfs_extent_buffer *child, *leaf, *parent, *promoted = NULL;
	struct btrfs_header *header, *parent_header;
	struct btrfs_item *items;
	struct btrfs_key old_first;
	struct btrfs_key_ptr *ptrs;
	uint64_t bytenr, generation;
	uint32_t capacity, i, nritems, nodesize, slot;
	uint8_t level;
	int all_single = 1;
	int error;

	leaf = path->bp_eb[0];
	header = btrfs_extent_buffer_data_mutable(handle, leaf);
	if (header == NULL || header->level != 0 ||
	    letoh32(header->nritems) != 1 || path->bp_slot[0] != 0)
		return (EINVAL);
	items = (struct btrfs_item *)(header + 1);
	if (btrfs_key_cmp(&items[0].key, key) != 0)
		return (ENOENT);

	for (level = 1; level <= path->bp_level; level++) {
		parent_header = btrfs_extent_buffer_data_mutable(handle,
		    path->bp_eb[level]);
		if (parent_header == NULL ||
		    path->bp_slot[level] >= letoh32(parent_header->nritems))
			return (EINVAL);
		if (letoh32(parent_header->nritems) != 1 ||
		    path->bp_slot[level] != 0)
			all_single = 0;
	}

	nodesize = letoh32(root->br_super->nodesize);
	capacity = nodesize - sizeof(*header);
	if (all_single) {
		memset(header + 1, 0, capacity);
		header->nritems = htole32(0);
		for (level = 1; level <= path->bp_level; level++) {
			child = path->bp_eb[level];
			error = btrfs_delayed_ref_add(handle,
			    child->eb_bytenr, btrfs_ref_tree(root->br_owner),
			    child->eb_level, -1);
			if (error != 0)
				goto fail;
			error = btrfs_extent_buffer_discard(handle, child);
			if (error != 0)
				goto fail;
			path->bp_eb[level] = NULL;
		}
		root->br_bytenr = leaf->eb_bytenr;
		root->br_generation = leaf->eb_generation;
		root->br_view_generation =
		    handle->bth_transaction->bt_generation;
		root->br_level = 0;
		path->bp_level = 0;
		return (0);
	}

	for (level = 0; level < path->bp_level; level++) {
		child = path->bp_eb[level];
		parent = path->bp_eb[level + 1];
		parent_header = btrfs_extent_buffer_data_mutable(handle,
		    parent);
		nritems = letoh32(parent_header->nritems);
		slot = path->bp_slot[level + 1];
		ptrs = (struct btrfs_key_ptr *)(parent_header + 1);
		if (nritems == 0 || slot >= nritems ||
		    letoh64(ptrs[slot].blockptr) != child->eb_bytenr ||
		    letoh64(ptrs[slot].generation) != child->eb_generation) {
			error = EINVAL;
			goto fail;
		}
		memcpy(&old_first, &ptrs[0].key, sizeof(old_first));
		error = btrfs_delayed_ref_add(handle, child->eb_bytenr,
		    btrfs_ref_tree(root->br_owner), child->eb_level, -1);
		if (error != 0)
			goto fail;
		error = btrfs_extent_buffer_discard(handle, child);
		if (error != 0)
			goto fail;
		path->bp_eb[level] = NULL;

		memmove(&ptrs[slot], &ptrs[slot + 1],
		    (nritems - slot - 1) * sizeof(*ptrs));
		memset(&ptrs[nritems - 1], 0, sizeof(*ptrs));
		parent_header->nritems = htole32(nritems - 1);
		if (nritems == 1)
			continue;

		if (level + 1 == path->bp_level && nritems == 2) {
			bytenr = letoh64(ptrs[0].blockptr);
			generation = letoh64(ptrs[0].generation);
			error = btrfs_extent_buffer_read(root, bytenr,
			    generation, path->bp_view_generation, level,
			    &promoted);
			if (error != 0)
				goto fail;
			rw_exit_read(&promoted->eb_lock);
			rw_enter_write(&promoted->eb_lock);
			error = btrfs_cow_block(handle, root, parent, 0,
			    &promoted);
			if (error != 0)
				goto fail;
			bytenr = promoted->eb_bytenr;
			generation = promoted->eb_generation;
			error = btrfs_delayed_ref_add(handle,
			    parent->eb_bytenr, btrfs_ref_tree(root->br_owner),
			    parent->eb_level, -1);
			if (error != 0)
				goto fail;
			root->br_bytenr = bytenr;
			root->br_generation = generation;
			root->br_view_generation =
			    handle->bth_transaction->bt_generation;
			root->br_level = level;
			btrfs_extent_buffer_put(promoted);
			promoted = NULL;
			error = btrfs_extent_buffer_discard(handle, parent);
			if (error != 0)
				goto fail;
			path->bp_eb[level + 1] = NULL;
			path->bp_level = level;
			return (0);
		}

		if (slot == 0) {
			for (i = level + 2; i <= path->bp_level; i++) {
				header = btrfs_extent_buffer_data_mutable(handle,
				    path->bp_eb[i]);
				ptrs = (struct btrfs_key_ptr *)(header + 1);
				if (path->bp_slot[i] >=
				    letoh32(header->nritems) ||
				    btrfs_key_cmp(&ptrs[path->bp_slot[i]].key,
				    &old_first) != 0) {
					error = EINVAL;
					goto fail;
				}
				memcpy(&ptrs[path->bp_slot[i]].key,
				    btrfs_block_key(parent_header, 0),
				    sizeof(ptrs[path->bp_slot[i]].key));
				if (path->bp_slot[i] != 0)
					break;
			}
		}
		return (0);
	}

	error = EINVAL;
fail:
	if (promoted != NULL)
		btrfs_extent_buffer_put(promoted);
	btrfs_trans_abort(handle, error);
	return (error);
}

/*
 * Edit a packed, private leaf by moving only the payload below the edited
 * item and its following descriptors. The caller has validated the layout,
 * capacity and ancestor separators; key and data must not alias the leaf.
 */
static void
btrfs_leaf_edit_packed(struct btrfs_header *header,
    const struct btrfs_key *key, const void *data, uint32_t size,
    uint32_t capacity, uint32_t slot, int operation)
{
	struct btrfs_item *items = (struct btrfs_item *)(header + 1);
	uint8_t *base = (uint8_t *)(header + 1);
	uint32_t nritems = letoh32(header->nritems);
	uint32_t low, end, oldsize, i, first;
	int32_t delta;

	low = nritems == 0 ? capacity : letoh32(items[nritems - 1].offset);
	end = slot == 0 ? capacity : letoh32(items[slot - 1].offset);
	oldsize = operation == BTRFS_LEAF_INSERT ? 0 :
	    letoh32(items[slot].size);
	if (operation == BTRFS_LEAF_DELETE)
		size = 0;
	delta = (int32_t)size - (int32_t)oldsize;
	memmove(base + low - delta, base + low, end - oldsize - low);

	if (operation == BTRFS_LEAF_INSERT) {
		memmove(&items[slot + 1], &items[slot],
		    (nritems - slot) * sizeof(*items));
		nritems++;
	} else if (operation == BTRFS_LEAF_DELETE) {
		memmove(&items[slot], &items[slot + 1],
		    (nritems - slot - 1) * sizeof(*items));
		nritems--;
	}
	first = slot;
	if (operation != BTRFS_LEAF_DELETE) {
		memcpy(&items[slot].key, key, sizeof(*key));
		items[slot].offset = htole32(end - size);
		items[slot].size = htole32(size);
		if (size != 0)
			memcpy(base + end - size, data, size);
		first++;
	}
	for (i = first; i < nritems; i++)
		items[i].offset = htole32(letoh32(items[i].offset) - delta);
	header->nritems = htole32(nritems);
	low -= delta;
	KASSERT(low >= nritems * sizeof(*items));
	memset(&items[nritems], 0, low - nritems * sizeof(*items));
}

/*
 * Equal-size replacements update only the private COW payload. Size changes
 * move packed data in place; imported leaves with gaps, or aliased inputs,
 * use a scratch rebuild. Both paths compact and zero unused bytes.
 */
static int
btrfs_leaf_mutate(struct btrfs_path *path, const struct btrfs_key *key,
    const void *data, uint32_t size, int operation)
{
	struct btrfs_extent_buffer *child, *leaf, *parent;
	struct btrfs_header *header, *new_header, *parent_header;
	struct btrfs_item *items, *new_items;
	struct btrfs_key old_first;
	struct btrfs_key_ptr *ptrs;
	uint8_t *base, *new_base, *scratch;
	size_t payload_bytes;
	uint32_t capacity, destination_slot, i, new_nritems, nodesize;
	uint32_t nritems, offset, payload_end, slot, source_size, source_slot;
	int first_changed = 0, packed = 1;

	if (path->bp_root == NULL || !path->bp_write ||
	    path->bp_handle == NULL || key == NULL ||
	    path->bp_eb[0] == NULL)
		return (EINVAL);
	leaf = path->bp_eb[0];
	rw_assert_wrlock(path->bp_root->br_lock);
	rw_assert_wrlock(&leaf->eb_lock);
	if (leaf->eb_level != 0 ||
	    leaf->eb_transaction != path->bp_handle->bth_transaction ||
	    leaf->eb_generation !=
	    path->bp_handle->bth_transaction->bt_generation ||
	    leaf->eb_private == NULL || !leaf->eb_dirty)
		return (EINVAL);

	header = btrfs_extent_buffer_data_mutable(path->bp_handle, leaf);
	if (header == NULL || header->level != 0)
		return (EINVAL);
	nodesize = letoh32(path->bp_root->br_super->nodesize);
	if (nodesize <= sizeof(*header))
		return (EINVAL);
	capacity = nodesize - sizeof(*header);
	nritems = letoh32(header->nritems);
	slot = path->bp_slot[0];
	if (nritems > capacity / sizeof(*items) || slot > nritems)
		return (EINVAL);

	items = (struct btrfs_item *)(header + 1);
	base = (uint8_t *)(header + 1);
	if (operation == BTRFS_LEAF_REPLACE && slot < nritems &&
	    letoh32(items[slot].size) == size) {
		if (btrfs_key_cmp(&items[slot].key, key) != 0)
			return (ENOENT);
		offset = letoh32(items[slot].offset);
		if (offset < nritems * sizeof(*items) ||
		    offset > capacity || size > capacity - offset)
			return (EINVAL);
		if (size != 0)
			memmove(base + offset, data, size);
		return (0);
	}
	payload_bytes = 0;
	payload_end = capacity;
	for (i = 0; i < nritems; i++) {
		offset = letoh32(items[i].offset);
		source_size = letoh32(items[i].size);
		if (offset < nritems * sizeof(*items) ||
		    offset > payload_end || source_size > payload_end - offset ||
		    (i != 0 &&
		    btrfs_key_cmp(&items[i - 1].key, &items[i].key) >= 0))
			return (EINVAL);
		if (source_size != payload_end - offset)
			packed = 0;
		payload_end = offset;
		payload_bytes += source_size;
		if (payload_bytes > capacity)
			return (EINVAL);
	}

	switch (operation) {
	case BTRFS_LEAF_INSERT:
		if (slot != nritems &&
		    btrfs_key_cmp(&items[slot].key, key) == 0)
			return (EEXIST);
		if ((slot != 0 &&
		    btrfs_key_cmp(&items[slot - 1].key, key) >= 0) ||
		    (slot != nritems &&
		    btrfs_key_cmp(key, &items[slot].key) >= 0))
			return (EINVAL);
		if (nritems == UINT32_MAX)
			return (ENOSPC);
		new_nritems = nritems + 1;
		if (payload_bytes > SIZE_MAX - size)
			return (ENOSPC);
		payload_bytes += size;
		first_changed = slot == 0 && nritems != 0;
		break;
	case BTRFS_LEAF_REPLACE:
		if (slot >= nritems ||
		    btrfs_key_cmp(&items[slot].key, key) != 0)
			return (ENOENT);
		source_size = letoh32(items[slot].size);
		payload_bytes -= source_size;
		if (payload_bytes > SIZE_MAX - size)
			return (ENOSPC);
		payload_bytes += size;
		new_nritems = nritems;
		break;
	case BTRFS_LEAF_DELETE:
		if (slot >= nritems ||
		    btrfs_key_cmp(&items[slot].key, key) != 0)
			return (ENOENT);
		if (nritems == 1 && path->bp_level != 0)
			return (btrfs_leaf_delete_empty(path, key));
		payload_bytes -= letoh32(items[slot].size);
		new_nritems = nritems - 1;
		first_changed = slot == 0 && new_nritems != 0;
		break;
	default:
		return (EINVAL);
	}

	if (new_nritems > capacity / sizeof(*new_items) ||
	    payload_bytes > capacity -
	    (size_t)new_nritems * sizeof(*new_items))
		return (ENOSPC);

	/*
	 * A changed first key must match each separator on the all-slot-zero
	 * ancestor chain.  Check the chain before changing the leaf.
	 */
	if (first_changed) {
		memcpy(&old_first, &items[0].key, sizeof(old_first));
		child = leaf;
		for (i = 1; i <= path->bp_level; i++) {
			parent = path->bp_eb[i];
			if (parent == NULL)
				return (EINVAL);
			rw_assert_wrlock(&parent->eb_lock);
			parent_header = btrfs_extent_buffer_data_mutable(
			    path->bp_handle, parent);
			if (parent_header == NULL ||
			    parent_header->level != i ||
			    path->bp_slot[i] >=
			    letoh32(parent_header->nritems))
				return (EINVAL);
			ptrs = (struct btrfs_key_ptr *)(parent_header + 1);
			if (letoh64(ptrs[path->bp_slot[i]].blockptr) !=
			    child->eb_bytenr ||
			    letoh64(ptrs[path->bp_slot[i]].generation) !=
			    child->eb_generation ||
			    btrfs_key_cmp(&ptrs[path->bp_slot[i]].key,
			    &old_first) != 0)
				return (EINVAL);
			if (path->bp_slot[i] != 0)
				break;
			child = parent;
		}
	}

	if (packed &&
	    ((uintptr_t)key < (uintptr_t)header ||
	    (uintptr_t)key >= (uintptr_t)header + nodesize) &&
	    (size == 0 || (uintptr_t)data < (uintptr_t)header ||
	    (uintptr_t)data >= (uintptr_t)header + nodesize)) {
		btrfs_leaf_edit_packed(header, key, data, size, capacity,
		    slot, operation);
		goto separators;
	}

	scratch = malloc(nodesize, M_BTRFS, M_WAITOK | M_ZERO);
	new_header = (struct btrfs_header *)scratch;
	memcpy(new_header, header, sizeof(*new_header));
	new_header->nritems = htole32(new_nritems);
	new_items = (struct btrfs_item *)(new_header + 1);
	new_base = (uint8_t *)(new_header + 1);
	payload_end = capacity;

	for (destination_slot = 0; destination_slot < new_nritems;
	    destination_slot++) {
		if (operation == BTRFS_LEAF_INSERT &&
		    destination_slot == slot) {
			memcpy(&new_items[destination_slot].key, key,
			    sizeof(*key));
			source_size = size;
			payload_end -= source_size;
			if (source_size != 0)
				memcpy(new_base + payload_end, data, source_size);
		} else if (operation == BTRFS_LEAF_REPLACE &&
		    destination_slot == slot) {
			memcpy(&new_items[destination_slot].key, key,
			    sizeof(*key));
			source_size = size;
			payload_end -= source_size;
			if (source_size != 0)
				memcpy(new_base + payload_end, data, source_size);
		} else {
			source_slot = destination_slot;
			if (operation == BTRFS_LEAF_INSERT &&
			    destination_slot > slot)
				source_slot--;
			else if (operation == BTRFS_LEAF_DELETE &&
			    destination_slot >= slot)
				source_slot++;
			memcpy(&new_items[destination_slot].key,
			    &items[source_slot].key, sizeof(*key));
			source_size = letoh32(items[source_slot].size);
			offset = letoh32(items[source_slot].offset);
			payload_end -= source_size;
			if (source_size != 0)
				memcpy(new_base + payload_end, base + offset,
				    source_size);
		}
		new_items[destination_slot].offset = htole32(payload_end);
		new_items[destination_slot].size = htole32(source_size);
	}
	KASSERT(payload_end >= new_nritems * sizeof(*new_items));
	memcpy(header, scratch, nodesize);
	free(scratch, M_BTRFS, nodesize);

separators:
	if (first_changed) {
		items = (struct btrfs_item *)(header + 1);
		for (i = 1; i <= path->bp_level; i++) {
			parent_header = btrfs_extent_buffer_data_mutable(
			    path->bp_handle, path->bp_eb[i]);
			KASSERT(parent_header != NULL);
			ptrs = (struct btrfs_key_ptr *)(parent_header + 1);
			memcpy(&ptrs[path->bp_slot[i]].key, &items[0].key,
			    sizeof(items[0].key));
			if (path->bp_slot[i] != 0)
				break;
		}
	}
	return (0);
}

static int
btrfs_leaf_insert_entry(const struct btrfs_header *source, uint32_t slot,
    const struct btrfs_key *key, const void *data, uint32_t size,
    uint32_t index, const struct btrfs_key **keyp, const void **datap,
    uint32_t *sizep)
{
	const struct btrfs_item *items;
	uint32_t source_slot;

	if (index == slot) {
		*keyp = key;
		*datap = data;
		*sizep = size;
		return (0);
	}
	source_slot = index - (index > slot);
	if (source_slot >= letoh32(source->nritems))
		return (EINVAL);
	items = (const struct btrfs_item *)(source + 1);
	*keyp = &items[source_slot].key;
	*sizep = letoh32(items[source_slot].size);
	*datap = (const uint8_t *)(source + 1) +
	    letoh32(items[source_slot].offset);
	return (0);
}

static int
btrfs_leaf_build_insert(struct btrfs_path *path,
    struct btrfs_extent_buffer *eb, const struct btrfs_header *source,
    uint32_t slot, const struct btrfs_key *key, const void *data,
    uint32_t size, uint32_t first, uint32_t count)
{
	struct btrfs_header *header;
	struct btrfs_item *items;
	const struct btrfs_key *item_key;
	const void *item_data;
	uint8_t *base;
	uint32_t capacity, i, item_size, nodesize, offset;
	int error;

	header = btrfs_extent_buffer_data_mutable(path->bp_handle, eb);
	if (header == NULL || header->level != 0)
		return (EINVAL);
	nodesize = letoh32(path->bp_root->br_super->nodesize);
	capacity = nodesize - sizeof(*header);
	memset(header + 1, 0, capacity);
	header->nritems = htole32(count);
	items = (struct btrfs_item *)(header + 1);
	base = (uint8_t *)(header + 1);
	offset = capacity;
	for (i = 0; i < count; i++) {
		error = btrfs_leaf_insert_entry(source, slot, key, data, size,
		    first + i, &item_key, &item_data, &item_size);
		if (error != 0 || item_size > offset)
			return (EINVAL);
		offset -= item_size;
		if (item_size != 0)
			memcpy(base + offset, item_data, item_size);
		memcpy(&items[i].key, item_key, sizeof(items[i].key));
		items[i].offset = htole32(offset);
		items[i].size = htole32(item_size);
	}
	if (offset < count * sizeof(*items))
		return (EINVAL);
	return (0);
}

/*
 * Insert pointers for adjacent newly split right siblings.  A three-way leaf
 * split starts with two pointers; after a full parent is split, propagation
 * carries its one new right sibling upward as usual.
 */
static int
btrfs_insert_split_pointers(struct btrfs_path *path,
    struct btrfs_extent_buffer **rights, const struct btrfs_key *right_keys,
    uint32_t nrights)
{
	struct btrfs_trans_handle *handle = path->bp_handle;
	struct btrfs_root *root = path->bp_root;
	struct btrfs_extent_buffer *carry[BTRFS_SPLIT_MAX_RIGHTS] = { NULL };
	struct btrfs_extent_buffer *parent, *new_right;
	struct btrfs_header *header, *new_header;
	struct btrfs_key carry_keys[BTRFS_SPLIT_MAX_RIGHTS];
	struct btrfs_key_ptr *combined, *ptrs, *new_ptrs;
	uint32_t capacity, insert_slot, left_count, nritems, nodesize;
	uint32_t carry_count, i, source_slot, total;
	uint8_t level;
	int error;

	if (nrights == 0 || nrights > BTRFS_SPLIT_MAX_RIGHTS)
		return (EINVAL);
	carry_count = nrights;
	for (i = 0; i < carry_count; i++) {
		if (rights[i] == NULL ||
		    rights[i]->eb_level != rights[0]->eb_level) {
			error = EINVAL;
			goto fail;
		}
		carry[i] = rights[i];
		memcpy(&carry_keys[i], &right_keys[i],
		    sizeof(carry_keys[i]));
	}
	nodesize = letoh32(root->br_super->nodesize);
	capacity = (nodesize - sizeof(struct btrfs_header)) /
	    sizeof(struct btrfs_key_ptr);

	for (level = carry[0]->eb_level + 1; level <= path->bp_level;
	    level++) {
		parent = path->bp_eb[level];
		if (parent == NULL || parent->eb_level != level ||
		    parent->eb_transaction != handle->bth_transaction) {
			error = EINVAL;
			goto fail;
		}
		rw_assert_wrlock(&parent->eb_lock);
		header = btrfs_extent_buffer_data_mutable(handle, parent);
		if (header == NULL || header->level != level) {
			error = EINVAL;
			goto fail;
		}
		nritems = letoh32(header->nritems);
		insert_slot = path->bp_slot[level] + 1;
		if (nritems == 0 || nritems > capacity ||
		    insert_slot == 0 || insert_slot > nritems) {
			error = EINVAL;
			goto fail;
		}
		ptrs = (struct btrfs_key_ptr *)(header + 1);
		if (btrfs_key_cmp(&ptrs[insert_slot - 1].key,
		    &carry_keys[0]) >= 0 ||
		    (carry_count == 2 &&
		    btrfs_key_cmp(&carry_keys[0], &carry_keys[1]) >= 0) ||
		    (insert_slot < nritems &&
		    btrfs_key_cmp(&carry_keys[carry_count - 1],
		    &ptrs[insert_slot].key) >= 0)) {
			error = EINVAL;
			goto fail;
		}

		if (carry_count <= capacity - nritems) {
			memmove(&ptrs[insert_slot + carry_count],
			    &ptrs[insert_slot],
			    (nritems - insert_slot) * sizeof(*ptrs));
			for (i = 0; i < carry_count; i++) {
				memcpy(&ptrs[insert_slot + i].key,
				    &carry_keys[i], sizeof(carry_keys[i]));
				ptrs[insert_slot + i].blockptr =
				    htole64(carry[i]->eb_bytenr);
				ptrs[insert_slot + i].generation =
				    htole64(carry[i]->eb_generation);
			}
			header->nritems =
			    htole32(nritems + carry_count);
			for (i = 0; i < carry_count; i++) {
				error = btrfs_delayed_ref_add(handle,
				    carry[i]->eb_bytenr,
				    btrfs_ref_tree(root->br_owner),
				    carry[i]->eb_level, 1);
				if (error != 0)
					goto fail;
			}
			for (i = 0; i < carry_count; i++)
				btrfs_extent_buffer_put(carry[i]);
			return (0);
		}

		total = nritems + carry_count;
		combined = mallocarray(total, sizeof(*combined), M_BTRFS,
		    M_WAITOK | M_ZERO);
		for (i = 0; i < total; i++) {
			if (i >= insert_slot &&
			    i < insert_slot + carry_count) {
				source_slot = i - insert_slot;
				memcpy(&combined[i].key,
				    &carry_keys[source_slot],
				    sizeof(carry_keys[source_slot]));
				combined[i].blockptr =
				    htole64(carry[source_slot]->eb_bytenr);
				combined[i].generation =
				    htole64(carry[source_slot]->eb_generation);
			} else {
				source_slot = i;
				if (i >= insert_slot + carry_count)
					source_slot -= carry_count;
				memcpy(&combined[i], &ptrs[source_slot],
				    sizeof(combined[i]));
			}
		}

		error = btrfs_extent_buffer_alloc(handle, parent, level,
		    &new_right);
		if (error != 0) {
			free(combined, M_BTRFS,
			    total * sizeof(*combined));
			goto fail;
		}
		new_header = btrfs_extent_buffer_data_mutable(handle,
		    new_right);
		if (new_header == NULL) {
			error = EINVAL;
			btrfs_extent_buffer_put(new_right);
			free(combined, M_BTRFS,
			    total * sizeof(*combined));
			goto fail;
		}
		left_count = total / 2;
		memset(ptrs, 0, capacity * sizeof(*ptrs));
		memcpy(ptrs, combined, left_count * sizeof(*ptrs));
		header->nritems = htole32(left_count);
		new_ptrs = (struct btrfs_key_ptr *)(new_header + 1);
		memcpy(new_ptrs, &combined[left_count],
		    (total - left_count) * sizeof(*new_ptrs));
		new_header->nritems = htole32(total - left_count);

		/* Existing children retain their implicit root references. */
		for (i = 0; error == 0 && i < carry_count; i++)
			error = btrfs_delayed_ref_add(handle,
			    carry[i]->eb_bytenr, btrfs_ref_tree(root->br_owner),
			    level - 1, 1);
		memcpy(&carry_keys[0], &combined[left_count].key,
		    sizeof(carry_keys[0]));
		free(combined, M_BTRFS, total * sizeof(*combined));
		for (i = 0; i < carry_count; i++)
			btrfs_extent_buffer_put(carry[i]);
		if (error != 0) {
			btrfs_extent_buffer_put(new_right);
			goto fail_no_carry;
		}
		carry[0] = new_right;
		carry_count = 1;
	}

	return (btrfs_grow_root(path, carry, carry_keys, carry_count));

fail:
	for (i = 0; i < carry_count; i++) {
		if (carry[i] != NULL)
			btrfs_extent_buffer_put(carry[i]);
	}
fail_no_carry:
	btrfs_trans_abort(handle, error);
	return (error);
}

static int
btrfs_grow_root(struct btrfs_path *path,
    struct btrfs_extent_buffer **rights, const struct btrfs_key *right_keys,
    uint32_t nrights)
{
	struct btrfs_trans_handle *handle = path->bp_handle;
	struct btrfs_root *root = path->bp_root;
	struct btrfs_extent_buffer *left, *new_root;
	struct btrfs_header *header;
	struct btrfs_key_ptr *ptrs;
	const struct btrfs_header *left_header, *previous_header;
	const struct btrfs_header *right_header;
	uint32_t capacity, i, left_nritems, nodesize, previous_nritems;
	uint32_t right_nritems;
	uint8_t level;
	int error;

	level = path->bp_level;
	left = path->bp_eb[level];
	if (left == NULL || left->eb_level != level || nrights == 0 ||
	    nrights > BTRFS_SPLIT_MAX_RIGHTS ||
	    level + 1 >= BTRFS_MAX_LEVEL) {
		error = level + 1 >= BTRFS_MAX_LEVEL ? EFBIG : EINVAL;
		goto fail;
	}
	left_header = btrfs_extent_buffer_data(left);
	left_nritems = letoh32(left_header->nritems);
	nodesize = letoh32(root->br_super->nodesize);
	capacity = (nodesize - sizeof(*header)) / sizeof(*ptrs);
	if (left_nritems == 0 || nrights + 1 > capacity) {
		error = EINVAL;
		goto fail;
	}
	previous_header = left_header;
	previous_nritems = left_nritems;
	for (i = 0; i < nrights; i++) {
		if (rights[i] == NULL || rights[i]->eb_level != level) {
			error = EINVAL;
			goto fail;
		}
		right_header = btrfs_extent_buffer_data(rights[i]);
		right_nritems = letoh32(right_header->nritems);
		if (right_nritems == 0 ||
		    btrfs_key_cmp(btrfs_block_key(right_header, 0),
		    &right_keys[i]) != 0 ||
		    btrfs_key_cmp(btrfs_block_key(previous_header,
		    previous_nritems - 1), &right_keys[i]) >= 0) {
			error = EINVAL;
			goto fail;
		}
		previous_header = right_header;
		previous_nritems = right_nritems;
	}
	error = btrfs_extent_buffer_alloc(handle, left, level + 1,
	    &new_root);
	if (error != 0)
		goto fail;
	header = btrfs_extent_buffer_data_mutable(handle, new_root);
	if (header == NULL) {
		error = EINVAL;
		goto fail_new;
	}
	ptrs = (struct btrfs_key_ptr *)(header + 1);
	memcpy(&ptrs[0].key, btrfs_block_key(left_header, 0),
	    sizeof(ptrs[0].key));
	ptrs[0].blockptr = htole64(left->eb_bytenr);
	ptrs[0].generation = htole64(left->eb_generation);
	for (i = 0; i < nrights; i++) {
		memcpy(&ptrs[i + 1].key, &right_keys[i],
		    sizeof(ptrs[i + 1].key));
		ptrs[i + 1].blockptr = htole64(rights[i]->eb_bytenr);
		ptrs[i + 1].generation =
		    htole64(rights[i]->eb_generation);
	}
	header->nritems = htole32(nrights + 1);

	/* The old root remains referenced by the same tree as a child. */
	error = 0;
	for (i = 0; error == 0 && i < nrights; i++)
		error = btrfs_delayed_ref_add(handle, rights[i]->eb_bytenr,
		    btrfs_ref_tree(root->br_owner), rights[i]->eb_level, 1);
	if (error == 0)
		error = btrfs_delayed_ref_add(handle, new_root->eb_bytenr,
		    btrfs_ref_tree(root->br_owner), new_root->eb_level, 1);
	if (error != 0)
		goto fail_new;

	KASSERT(root->br_transaction == handle->bth_transaction);
	root->br_bytenr = new_root->eb_bytenr;
	root->br_generation = new_root->eb_generation;
	root->br_view_generation = new_root->eb_generation;
	root->br_level = new_root->eb_level;
	btrfs_extent_buffer_put(new_root);
	for (i = 0; i < nrights; i++)
		btrfs_extent_buffer_put(rights[i]);
	return (0);

fail_new:
	btrfs_extent_buffer_put(new_root);
fail:
	for (i = 0; i < nrights; i++) {
		if (rights[i] != NULL)
			btrfs_extent_buffer_put(rights[i]);
	}
	btrfs_trans_abort(handle, error);
	return (error);
}

static int
btrfs_leaf_split_insert(struct btrfs_path *path,
    const struct btrfs_key *key, const void *data, uint32_t size)
{
	struct btrfs_extent_buffer *child, *leaf, *parent;
	struct btrfs_extent_buffer *rights[BTRFS_SPLIT_MAX_RIGHTS] = { NULL };
	struct btrfs_header *header, *parent_header;
	const struct btrfs_header *check_header, *source;
	const struct btrfs_item *items;
	struct btrfs_key old_first;
	struct btrfs_key right_keys[BTRFS_SPLIT_MAX_RIGHTS];
	struct btrfs_key_ptr *ptrs;
	const void *item_data;
	const struct btrfs_key *item_key;
	void *insert_data = NULL;
	uint8_t *scratch;
	uint64_t best_delta = UINT64_MAX, delta;
	uint32_t best = 0, capacity, carry_count, count, i, item_size;
	uint32_t nritems, nodesize, parent_capacity, payload, right_count;
	uint32_t right_payload, slot, total;
	int error;

	leaf = path->bp_eb[0];
	header = btrfs_extent_buffer_data_mutable(path->bp_handle, leaf);
	if (header == NULL || header->level != 0)
		return (EINVAL);
	nodesize = letoh32(path->bp_root->br_super->nodesize);
	capacity = nodesize - sizeof(*header);
	nritems = letoh32(header->nritems);
	slot = path->bp_slot[0];
	if (nritems == 0 || nritems == UINT32_MAX || slot > nritems ||
	    nritems > capacity / sizeof(*items))
		return (EINVAL);
	items = (const struct btrfs_item *)(header + 1);
	if ((slot != 0 && btrfs_key_cmp(&items[slot - 1].key, key) >= 0) ||
	    (slot != nritems &&
	    btrfs_key_cmp(key, &items[slot].key) >= 0))
		return (EINVAL);
	if (size > capacity - sizeof(*items))
		return (ENOSPC);

	/*
	 * Choose a split by used bytes, while requiring both resulting leaves
	 * to satisfy the packed item-array/payload boundary.
	 */
	total = nritems + 1;
	payload = 0;
	for (i = 0; i < total; i++) {
		error = btrfs_leaf_insert_entry(header, slot, key, data, size,
		    i, &item_key, &item_data, &item_size);
		if (error != 0 || payload > UINT32_MAX - item_size)
			return (EINVAL);
		payload += item_size;
	}
	right_payload = payload;
	payload = 0;
	for (count = 1; count < total; count++) {
		error = btrfs_leaf_insert_entry(header, slot, key, data, size,
		    count - 1, &item_key, &item_data, &item_size);
		if (error != 0 || payload > UINT32_MAX - item_size ||
		    right_payload < item_size)
			return (EINVAL);
		payload += item_size;
		right_payload -= item_size;
		if (payload > capacity - count * sizeof(*items) ||
		    right_payload >
		    capacity - (total - count) * sizeof(*items))
			continue;
		delta = payload + count * sizeof(*items);
		if (delta > right_payload +
		    (total - count) * sizeof(*items))
			delta -= right_payload +
			    (total - count) * sizeof(*items);
		else
			delta = right_payload +
			    (total - count) * sizeof(*items) - delta;
		if (delta < best_delta) {
			best = count;
			best_delta = delta;
		}
	}
	/*
	 * If no two-way partition fits, the new item must be in the middle:
	 * the old prefix and suffix already fit together in the source leaf,
	 * while the new item was checked above to fit by itself.
	 */
	right_count = best == 0 ? 2 : 1;
	if (right_count == 2 && (slot == 0 || slot == nritems))
		return (EINVAL);
	if (path->bp_level + 1 >= BTRFS_MAX_LEVEL) {
		parent_capacity =
		    (nodesize - sizeof(*check_header)) / sizeof(*ptrs);
		if (parent_capacity < right_count)
			return (EFBIG);
		carry_count = right_count;
		for (i = 1; i <= path->bp_level; i++) {
			check_header =
			    btrfs_extent_buffer_data(path->bp_eb[i]);
			if (letoh32(check_header->nritems) <=
			    parent_capacity - carry_count)
				break;
			carry_count = 1;
		}
		if (i > path->bp_level)
			return (EFBIG);
	}

	if (size != 0) {
		insert_data = malloc(size, M_BTRFS, M_WAITOK);
		memcpy(insert_data, data, size);
	}
	scratch = malloc(nodesize, M_BTRFS, M_WAITOK);
	memcpy(scratch, header, nodesize);
	source = (const struct btrfs_header *)scratch;
	memcpy(&old_first, &items[0].key, sizeof(old_first));

	/*
	 * Verify the separator chain before allocating or changing either leaf.
	 */
	if (slot == 0) {
		child = leaf;
		for (i = 1; i <= path->bp_level; i++) {
			parent = path->bp_eb[i];
			parent_header =
			    btrfs_extent_buffer_data_mutable(path->bp_handle,
			    parent);
			if (parent_header == NULL ||
			    path->bp_slot[i] >=
			    letoh32(parent_header->nritems)) {
				error = EINVAL;
				goto out;
			}
			ptrs = (struct btrfs_key_ptr *)(parent_header + 1);
			if (letoh64(ptrs[path->bp_slot[i]].blockptr) !=
			    child->eb_bytenr ||
			    letoh64(ptrs[path->bp_slot[i]].generation) !=
			    child->eb_generation ||
			    btrfs_key_cmp(&ptrs[path->bp_slot[i]].key,
			    &old_first) != 0) {
				error = EINVAL;
				goto out;
			}
			if (path->bp_slot[i] != 0)
				break;
			child = parent;
		}
	}

	error = btrfs_extent_buffer_alloc(path->bp_handle, leaf, 0,
	    &rights[0]);
	if (error != 0)
		goto out;
	if (right_count == 2)
		error = btrfs_extent_buffer_alloc(path->bp_handle, leaf, 0,
		    &rights[1]);
	if (right_count == 1) {
		if (error == 0)
			error = btrfs_leaf_build_insert(path, leaf, source,
			    slot, key, insert_data, size, 0, best);
		if (error == 0)
			error = btrfs_leaf_build_insert(path, rights[0],
			    source, slot, key, insert_data, size, best,
			    total - best);
	} else {
		if (error == 0)
			error = btrfs_leaf_build_insert(path, leaf, source,
			    slot, key, insert_data, size, 0, slot);
		if (error == 0)
			error = btrfs_leaf_build_insert(path, rights[0],
			    source, slot, key, insert_data, size, slot, 1);
		if (error == 0)
			error = btrfs_leaf_build_insert(path, rights[1],
			    source, slot, key, insert_data, size, slot + 1,
			    total - slot - 1);
	}
	if (error != 0) {
		for (i = 0; i < right_count; i++) {
			if (rights[i] != NULL)
				btrfs_extent_buffer_put(rights[i]);
		}
		btrfs_trans_abort(path->bp_handle, error);
		goto out;
	}

	header = btrfs_extent_buffer_data_mutable(path->bp_handle, leaf);
	if (slot == 0) {
		items = (const struct btrfs_item *)(header + 1);
		for (i = 1; i <= path->bp_level; i++) {
			parent_header = btrfs_extent_buffer_data_mutable(
			    path->bp_handle, path->bp_eb[i]);
			KASSERT(parent_header != NULL);
			ptrs = (struct btrfs_key_ptr *)(parent_header + 1);
			memcpy(&ptrs[path->bp_slot[i]].key, &items[0].key,
			    sizeof(items[0].key));
			if (path->bp_slot[i] != 0)
				break;
		}
	}
	for (i = 0; i < right_count; i++) {
		header = btrfs_extent_buffer_data_mutable(path->bp_handle,
		    rights[i]);
		items = (const struct btrfs_item *)(header + 1);
		memcpy(&right_keys[i], &items[0].key,
		    sizeof(right_keys[i]));
	}
	error = btrfs_insert_split_pointers(path, rights, right_keys,
	    right_count);
out:
	free(scratch, M_BTRFS, nodesize);
	if (insert_data != NULL)
		free(insert_data, M_BTRFS, size);
	return (error);
}

static int
btrfs_mutate_item(struct btrfs_trans_handle *handle, struct btrfs_root *root,
    const struct btrfs_key *key, const void *data, uint32_t size, int operation)
{
	struct btrfs_path path = { 0 };
	int error;

	if (handle == NULL || handle->bth_transaction == NULL || root == NULL ||
	    key == NULL || root->br_mount !=
	    handle->bth_transaction->bt_mount ||
	    (operation != BTRFS_LEAF_DELETE && size != 0 && data == NULL))
		return (EINVAL);

	error = btrfs_search_slot_write(handle, root, key, &path);
	if (operation == BTRFS_LEAF_INSERT) {
		if (error == 0) {
			error = EEXIST;
			goto out;
		}
		if (error != ENOENT)
			goto out;
	} else if (error != 0) {
		goto out;
	}

	error = btrfs_leaf_mutate(&path, key, data, size, operation);
	if (error == ENOSPC && operation == BTRFS_LEAF_INSERT)
		error = btrfs_leaf_split_insert(&path, key, data, size);
	if (error == EINVAL)
		btrfs_trans_abort(handle, error);
out:
	btrfs_release_path(&path);
	return (error);
}

int
btrfs_insert_item(struct btrfs_trans_handle *handle, struct btrfs_root *root,
    const struct btrfs_key *key, const void *data, uint32_t size)
{
	return (btrfs_mutate_item(handle, root, key, data, size,
	    BTRFS_LEAF_INSERT));
}

int
btrfs_replace_item(struct btrfs_trans_handle *handle, struct btrfs_root *root,
    const struct btrfs_key *key, const void *data, uint32_t size)
{
	return (btrfs_mutate_item(handle, root, key, data, size,
	    BTRFS_LEAF_REPLACE));
}

int
btrfs_delete_item(struct btrfs_trans_handle *handle, struct btrfs_root *root,
    const struct btrfs_key *key)
{
	return (btrfs_mutate_item(handle, root, key, NULL, 0,
	    BTRFS_LEAF_DELETE));
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

	for (level = 1; level <= path->bp_level; level++) {
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

	for (level = 1; level <= path->bp_level; level++) {
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
btrfs_read_data_csums(struct btrfs_fs *bmp, uint64_t logical,
    uint64_t length, uint8_t *csums)
{
	const struct btrfs_key *key;
	const uint8_t *data;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	struct btrfs_key target;
	uint64_t cursor, end, item_end, span, start;
	uint32_t item_size, sectorsize;
	size_t csum_index, csum_offset, navailable, ncopy;
	size_t csum_size = btrfs_csum_size(&bmp->bm_super);
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
	error = btrfs_get_root(bmp, BTRFS_CSUM_TREE_OBJECTID, &root);
	if (error != 0)
		return (error);
	error = btrfs_search_predecessor(root, &target, &path);
	if (error == ENOENT)
		error = btrfs_search_lower_bound(root, &target, &path);
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
		    item_size % csum_size != 0) {
			error = EINVAL;
			goto out;
		}
		span = (uint64_t)(item_size / csum_size) * sectorsize;
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
		navailable = item_size / csum_size - csum_offset;
		ncopy = MIN(navailable, (size_t)((end - cursor) / sectorsize));
		memcpy(csums + csum_index * csum_size,
		    data + csum_offset * csum_size, ncopy * csum_size);
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
btrfs_lookup_data_csum(struct btrfs_fs *bmp, uint64_t logical,
    uint8_t *csump)
{
	return (btrfs_read_data_csums(bmp, logical,
	    letoh32(bmp->bm_super.sectorsize), csump));
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
