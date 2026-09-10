/* Public domain. */
/*
 * Logical relocation uses ordinary COW, including the existing conversion of
 * shared snapshot blocks to full backreferences. Each transaction replaces one
 * file extent or COWs one metadata path. The source group is excluded from new
 * reservations throughout. There is no on-disk relocation tree: every durable
 * prefix is a normal filesystem, and interruption leaves the remaining source
 * extents reachable.
 *
 * Data allocations are copied once per source group. The in-memory map keeps
 * reflinks and snapshots sharing the destination allocation. Its entries only
 * name allocations already referenced by committed file items. Administration
 * excludes writers (and readers carrying old extent mappings), so those
 * references cannot disappear while the map is in use.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <btrfs/btrfs_var.h>

struct balance_extent {
	RBT_ENTRY(balance_extent) entry;
	uint64_t old;
	uint64_t new;
	uint64_t length;
};
RBT_HEAD(balance_extents, balance_extent);

static inline int
balance_extent_compare(const struct balance_extent *a,
    const struct balance_extent *b)
{
	return (a->old < b->old ? -1 : a->old != b->old);
}
RBT_PROTOTYPE(balance_extents, balance_extent, entry, balance_extent_compare);
RBT_GENERATE(balance_extents, balance_extent, entry, balance_extent_compare);

static int
balance_contains(const struct btrfs_block_group *group, uint64_t bytenr)
{
	return (bytenr >= group->bbg_bytenr &&
	    bytenr - group->bbg_bytenr < group->bbg_length);
}

/* Reserve before mutation; growth is explicit because we own the chunk lock. */
static int
balance_join(struct btrfs_fs *bmp, uint64_t data, uint64_t metadata,
    int system, struct btrfs_trans_handle **handlep)
{
	struct btrfs_trans_reservation reserve = {
	    .btr_chunk = 1, .btr_contiguous_data = 1
	}, probe;
	struct btrfs_trans_handle *handle;
	uint64_t nodesize = letoh32(bmp->bm_super.nodesize);
	int error, attempt;

	reserve.btr_data = data;
	reserve.btr_metadata = metadata;
	if (system)
		reserve.btr_system = nodesize * BTRFS_CHUNK_SYSTEM_BLOCKS;
	for (attempt = 0; ; attempt++) {
		error = btrfs_trans_join(bmp, &reserve, handlep);
		if (error != ENOSPC || attempt == 1)
			return (error);
		probe = reserve;
		probe.btr_data = 0;
		error = btrfs_trans_join(bmp, &probe, &handle);
		if (error == 0)
			error = btrfs_trans_end(handle);
		else if (error == ENOSPC)
			error = btrfs_balance_grow(bmp,
			    BTRFS_BLOCK_GROUP_METADATA,
			    metadata * 2 + nodesize * 512);
		if (error == 0 && data != 0)
			error = btrfs_balance_grow(bmp, BTRFS_BLOCK_GROUP_DATA,
			    data);
		if (error == 0 && system)
			error = btrfs_balance_grow(bmp, BTRFS_BLOCK_GROUP_SYSTEM,
			    reserve.btr_system * 2);
		if (error != 0)
			return (error);
	}
}

static int
balance_finish(struct btrfs_trans_handle *handle, int error)
{
	struct btrfs_fs *bmp = handle->bth_transaction->bt_mount;
	int enderror;

	if (error != 0)
		btrfs_trans_abort(handle, error);
	enderror = btrfs_trans_end(handle);
	if (error == 0)
		error = enderror;
	if (error == 0)
		error = btrfs_commit_current(bmp, curproc);
	return (error);
}

static int
balance_data_item(struct btrfs_root *root, const struct btrfs_key *key,
    struct btrfs_file_extent_item *item, struct balance_extents *extents)
{
	struct btrfs_fs *bmp = root->br_mount;
	struct balance_extent find = { .old = letoh64(item->disk_bytenr) };
	struct balance_extent *extent;
	struct btrfs_trans_handle *handle;
	struct btrfs_ref_owner owner;
	uint64_t length = letoh64(item->disk_num_bytes), bytenr, metadata;
	uint64_t nodesize = letoh32(bmp->bm_super.nodesize);
	uint64_t sector = letoh32(bmp->bm_super.sectorsize);
	int error, fresh;

	if (length == 0 || length % sector)
		return (EINVAL);
	extent = RBT_FIND(balance_extents, extents, &find);
	fresh = extent == NULL;
	if (!fresh && extent->length != length)
		return (EINVAL);
	metadata = nodesize * BTRFS_RECLAIM_METADATA_BLOCKS;
	if (fresh) {
		/*
		 * Checksum insertion can split leaves. Budget full-height paths
		 * for each nodesize/4 payload, plus shared-leaf reference work.
		 */
		metadata += roundup((length / sector) *
		    btrfs_csum_size(&bmp->bm_super), nodesize / 4) *
		    (BTRFS_MAX_LEVEL * 8);
	}
	error = balance_join(bmp, fresh ? length : 0, metadata, 0, &handle);
	if (error != 0)
		return (error);
	if (fresh) {
		error = btrfs_space_alloc(handle, BTRFS_BLOCK_GROUP_DATA,
		    length, sector, &bytenr);
		if (error == ENOSPC) {
			/* No mutation yet: a fragmented reservation is recoverable. */
			(void)btrfs_trans_end(handle);
			error = btrfs_balance_grow(bmp, BTRFS_BLOCK_GROUP_DATA,
			    length);
			if (error != 0)
				return (error);
			error = balance_join(bmp, length, metadata, 0, &handle);
			if (error != 0)
				return (error);
			error = btrfs_space_alloc(handle, BTRFS_BLOCK_GROUP_DATA,
			    length, sector, &bytenr);
		}
		if (error != 0) {
			(void)btrfs_trans_end(handle);
			return (error);
		}
		extent = malloc(sizeof(*extent), M_BTRFS, M_WAITOK | M_ZERO);
		extent->old = find.old;
		extent->new = bytenr;
		extent->length = length;
		error = btrfs_relocate_data(handle, extent->old, extent->new,
		    length);
	}
	if (error == 0) {
		item->disk_bytenr = htole64(extent->new);
		error = btrfs_replace_item(handle, root, key, item, sizeof(*item));
	}
	/* File-base subtraction deliberately wraps for clones to lower offsets. */
	owner = btrfs_ref_data(root->br_owner, letoh64(key->objectid),
	    letoh64(key->offset) - letoh64(item->offset));
	if (error == 0)
		error = btrfs_delayed_data_ref_add(handle, extent->new, length,
		    owner, 1);
	if (error == 0)
		error = btrfs_delayed_data_ref_add(handle, extent->old, length,
		    owner, -1);
	error = balance_finish(handle, error);
	if (fresh) {
		if (error == 0)
			RBT_INSERT(balance_extents, extents, extent);
		else
			free(extent, M_BTRFS, sizeof(*extent));
	}
	return (error);
}

static int
balance_data_tree(struct btrfs_root *root, struct btrfs_block_group *group,
    struct balance_extents *extents)
{
	struct btrfs_path path = { 0 };
	struct btrfs_key key = { 0 };
	const struct btrfs_key *found;
	const uint8_t *data;
	struct btrfs_file_extent_item item;
	uint32_t size;
	int error;

	error = btrfs_search_lower_bound(root, &key, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &found, &data, &size);
		if (error != 0)
			break;
		key = *found;
		if (key.type == BTRFS_EXTENT_DATA_KEY &&
		    size >= offsetof(struct btrfs_file_extent_item, disk_bytenr) &&
		    ((const struct btrfs_file_extent_item *)data)->type !=
		    BTRFS_FILE_EXTENT_INLINE) {
			if (size != sizeof(item)) {
				error = EINVAL;
				break;
			}
			memcpy(&item, data, sizeof(item));
			if (item.type != BTRFS_FILE_EXTENT_REG &&
			    item.type != BTRFS_FILE_EXTENT_PREALLOC) {
				error = EINVAL;
				break;
			}
			if (balance_contains(group, letoh64(item.disk_bytenr))) {
				if (letoh64(item.disk_num_bytes) >
				    group->bbg_length - (letoh64(item.disk_bytenr) -
				    group->bbg_bytenr)) {
					error = EINVAL;
					break;
				}
				btrfs_release_path(&path);
				error = btrfs_balance_interrupted(root->br_mount);
				if (error == 0)
					error = balance_data_item(root, &key,
					    &item, extents);
				if (error != 0)
					break;
				error = btrfs_search_slot(root, &key, &path);
				if (error != 0)
					break;
			}
		}
		error = btrfs_next_item(&path);
	}
	btrfs_release_path(&path);
	return (error == ENOENT ? 0 : error);
}

/* Find one path containing a source block, including empty tree roots. */
static int
balance_metadata_key(struct btrfs_root *root, struct btrfs_block_group *group,
    struct btrfs_key *key)
{
	struct btrfs_path path = { 0 };
	const struct btrfs_key *found;
	const struct btrfs_header *header;
	unsigned int level;
	int error, match;

	error = btrfs_search_slot(root, key, &path);
	if (error == ENOENT && path.bp_eb[0] != NULL)
		error = 0;
	while (error == 0) {
		match = 0;
		for (level = 0; level <= path.bp_level; level++)
			if (balance_contains(group, path.bp_eb[level]->eb_bytenr))
				match = 1;
		if (match) {
			error = btrfs_path_item(&path, &found, NULL, NULL);
			if (error == 0)
				*key = *found;
			else if (error == ENOENT)
				error = 0;
			break;
		}
		header = btrfs_extent_buffer_data(path.bp_eb[0]);
		path.bp_slot[0] = letoh32(header->nritems);
		error = btrfs_next_item(&path);
	}
	btrfs_release_path(&path);
	return (error);
}

static int
balance_metadata_tree(struct btrfs_root *root, struct btrfs_block_group *group)
{
	struct btrfs_fs *bmp = root->br_mount;
	struct btrfs_trans_handle *handle;
	struct btrfs_path path = { 0 };
	struct btrfs_key key = { 0 };
	int error;

	for (;;) {
		/*
		 * Resume at the last COWed key. Exclusion guarantees that no
		 * commit can introduce source blocks behind this cursor.
		 */
		error = balance_metadata_key(root, group, &key);
		if (error != 0)
			return (error == ENOENT ? 0 : error);
		error = btrfs_balance_interrupted(bmp);
		if (error != 0)
			return (error);
		error = balance_join(bmp, 0,
		    (uint64_t)letoh32(bmp->bm_super.nodesize) *
		    BTRFS_RECLAIM_METADATA_BLOCKS,
		    root->br_owner == BTRFS_CHUNK_TREE_OBJECTID, &handle);
		if (error != 0)
			return (error);
		error = btrfs_search_slot_write(handle, root, &key, &path);
		btrfs_release_path(&path);
		if (error == ENOENT)
			error = 0;
		error = balance_finish(handle, error);
		if (error != 0)
			return (error);
	}
}

int
btrfs_balance_relocate(struct btrfs_fs *bmp, struct btrfs_block_group *group)
{
	struct balance_extents extents = RBT_INITIALIZER(&extents);
	struct balance_extent *extent;
	struct btrfs_root *roots, *root;
	struct btrfs_path path = { 0 };
	struct btrfs_key key = { 0 };
	const struct btrfs_key *found;
	uint64_t owner;
	int error, metadata = !(group->bbg_flags & BTRFS_BLOCK_GROUP_DATA);

	error = btrfs_get_root(bmp, BTRFS_ROOT_TREE_OBJECTID, &roots);
	if (error != 0)
		return (error);
	/*
	 * Root item keys remain stable during the run. Release every path
	 * before relocation, since a commit can COW the root tree itself.
	 */
	for (;;) {
		error = btrfs_search_lower_bound(roots, &key, &path);
		while (error == 0) {
			error = btrfs_path_item(&path, &found, NULL, NULL);
			if (error != 0 || found->type == BTRFS_ROOT_ITEM_KEY)
				break;
			error = btrfs_next_item(&path);
		}
		if (error != 0)
			break;
		owner = letoh64(found->objectid);
		btrfs_release_path(&path);
		if (metadata || btrfs_file_tree(owner)) {
			error = btrfs_get_root(bmp, owner, &root);
			if (error != 0)
				break;
			if (metadata)
				error = balance_metadata_tree(root, group);
			else
				error = balance_data_tree(root, group, &extents);
		}
		if (error != 0 || owner == UINT64_MAX)
			break;
		key.objectid = htole64(owner + 1);
	}
	btrfs_release_path(&path);
	if (error == ENOENT)
		error = 0;
	if (error == 0 && metadata)
		error = balance_metadata_tree(roots, group);
	if (error == 0 && metadata)
		error = btrfs_get_root(bmp, BTRFS_CHUNK_TREE_OBJECTID, &root);
	if (error == 0 && metadata)
		error = balance_metadata_tree(root, group);
	while ((extent = RBT_ROOT(balance_extents, &extents)) != NULL) {
		RBT_REMOVE(balance_extents, &extents, extent);
		free(extent, M_BTRFS, sizeof(*extent));
	}
	return (error);
}
