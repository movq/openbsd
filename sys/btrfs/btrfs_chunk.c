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
 * Grow data, metadata and system groups within recorded member device sizes,
 * preserving existing profiles. Reservation failure publishes pending work
 * before entering chunk growth without a handle. Low system space triggers
 * system growth first. Logical ranges append beyond a filesystem-lifetime
 * high-water mark; chunk size does not change extent size or the ordered-data
 * watermark.
 *
 * The physical planner starts at 32 MiB for data/metadata or 8 MiB for system
 * chunks. Data growth can reach 256 MiB, limiting growth above the base to a
 * tenth of remaining physical space including mirrors. Smaller gaps halve
 * the target down to 1 MiB.
 *
 * If growth fails, empty data/metadata groups can be returned for another
 * allocation type. Groups with disk usage, reservations, allocations or pins
 * are ineligible. Removal atomically deletes chunk, device-extent, block-group
 * and optional free-space records and reduces device usage; publication
 * returns the stripes, while abort restores eligibility. Evict physical
 * buffers before reassignment because data and metadata buffer sizes differ.
 * Imported bitmap records can exceed the available deletion reservation.
 * Device removal moves physical chunks without changing logical extent
 * addresses. A closed commit copies the final live extent set after metadata
 * writeout and before super publication. Both old storage and old mappings
 * remain usable until publication; failed copies never expose new mappings.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/proc.h>

#include <btrfs/btrfs_var.h>

enum btrfs_chunk_action {
	BTRFS_CHUNK_ADD,
	BTRFS_CHUNK_REMOVE,
	BTRFS_CHUNK_MOVE,
	BTRFS_DEVICE_ADD,
	BTRFS_DEVICE_REMOVE
};

/*
 * bm_chunk_alloc_lock serializes preparation through transaction completion.
 * Preparation owns replacement indexes and either a private new group or an
 * existing group's exclusion from reservations. Other indexed groups are
 * borrowed. Roots belong to the filesystem; no tree paths survive preparation.
 * A move borrows its existing group and stages both device items and one
 * mapping. An add owns its unpublished device; a device removal borrows its
 * member until the ioctl caller can erase and close it after detachment.
 *
 * After application, chunk_execute transfers the whole operation to the
 * transaction before dropping its handle. Only btrfs_chunk_publish or
 * btrfs_chunk_abort may then consume it, even if commit reports an error.
 * Until that transfer, chunk_discard unwinds every preparation/application
 * failure. New groups never serve allocations before durable publication.
 *
 * One reserved handle applies chunk/device, block-group and free-space edits.
 * btr_chunk prevents recursive growth during join. Chunk-tree COW uses system
 * space; system growth checks superblock-array capacity and prepares a
 * bootstrap mapping. Publication installs indexes before establishing the next
 * generation's reserves. Callers must relinquish ownership even if reserve
 * replenishment fails after durable publication.
 */
struct btrfs_chunk_operation {
	struct btrfs_fs			*fs;
	enum btrfs_chunk_action		 action;
	struct btrfs_chunk_map		 chunk;
	struct btrfs_chunk_map		*chunks;
	struct btrfs_block_group		**groups;
	unsigned int			 count;
	struct btrfs_block_group		*group;
	struct btrfs_root		*chunk_root;
	struct btrfs_root		*dev_root;
	struct btrfs_root		*group_root;
	struct btrfs_root		*free_root;
	struct btrfs_trans_reservation	 reservation;
	struct btrfs_dev_item		 device;
	struct btrfs_device		*member;
	struct btrfs_device		*source;
	struct btrfs_dev_item		 source_item;
	struct btrfs_chunk_map		 oldchunk;
	unsigned int			 index;
	unsigned int			 nfree;
	uint32_t			 itemsize;
	uint8_t				 record[offsetof(struct btrfs_chunk,
	    stripe) + BTRFS_MAX_MIRRORS * sizeof(struct btrfs_stripe)];
	uint32_t			 system_size;
	uint8_t				 system[sizeof(struct btrfs_key) +
	    offsetof(struct btrfs_chunk, stripe) +
	    BTRFS_MAX_MIRRORS * sizeof(struct btrfs_stripe)];
};

static void
chunk_free_group(struct btrfs_block_group *group)
{
	struct btrfs_free_extent *space;

	KASSERT(group->bbg_reserved_bytes == 0);
	KASSERT(group->bbg_allocated_bytes == 0);
	KASSERT(group->bbg_pinned_bytes == 0);
	while ((space = TAILQ_FIRST(&group->bbg_free_extents)) != NULL) {
		TAILQ_REMOVE(&group->bbg_free_extents, space, bfe_entry);
		free(space, M_BTRFS, sizeof(*space));
	}
	free(group, M_BTRFS, sizeof(*group));
}

static void
chunk_discard(struct btrfs_chunk_operation *op)
{
	if (op->action == BTRFS_DEVICE_ADD && op->member != NULL)
		btrfs_close_member(op->fs, op->member, curproc);
	if (op->group != NULL) {
		if (op->action == BTRFS_CHUNK_REMOVE) {
			mtx_enter(&op->group->bbg_lock);
			KASSERT(op->group->bbg_removing);
			op->group->bbg_removing = 0;
			mtx_leave(&op->group->bbg_lock);
		} else
			chunk_free_group(op->group);
	}
	free(op->chunks, M_BTRFS, op->count * sizeof(*op->chunks));
	free(op->groups, M_BTRFS, op->count * sizeof(*op->groups));
	free(op, M_BTRFS, sizeof(*op));
}

/*
 * Find a physical stripe without crossing any existing device extent,
 * previously selected DUP stripe, or superblock stripe. The caller holds
 * the chunk-allocation lock, so the mapping index is current.
 */
static int
btrfs_chunk_physical(const struct btrfs_chunk_map *chunks, unsigned int count,
    struct btrfs_chunk_map *chunk, unsigned int mirror, uint64_t device_size)
{
	uint64_t start = 1024 * 1024, end, next, occupied, length;
	unsigned int i, j;

	while (start <= device_size && chunk->length <= device_size - start) {
		end = start + chunk->length;
		next = start;
		for (i = 0; i <= count; i++) {
			const struct btrfs_chunk_map *c =
			    i == count ? chunk : &chunks[i];
			unsigned int mirrors = i == count ? mirror : c->nmirrors;

			for (j = 0; j < mirrors; j++) {
				if (c->device[j] != chunk->device[mirror])
					continue;
				occupied = c->physical[j];
				length = c->length;
				if (start < occupied + length && occupied < end)
					next = MAX(next, occupied + length);
			}
		}
		for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
			occupied = superblock_addrs[i];
			if (start < occupied + 65536 && occupied < end)
				next = MAX(next, occupied + 65536);
		}
		if (next == start) {
			chunk->physical[mirror] = start;
			return (0);
		}
		if (next > UINT64_MAX - 65535)
			break;
		start = roundup(next, 65536);
	}
	return (ENOSPC);
}

/* Select the existing profile and an append-only logical range. */
static int
chunk_plan(struct btrfs_fs *bmp, struct btrfs_chunk_operation *op,
    uint64_t type, uint64_t needed)
{
	struct btrfs_chunk_map *chunk = &op->chunk;
	uint64_t logical;
	unsigned int count = bmp->bm_nchunks, i, template = UINT_MAX;

	rw_assert_wrlock(&bmp->bm_chunk_alloc_lock);
	KASSERT(count == bmp->bm_nblock_groups);
	if (count == 0 || count == UINT_MAX)
		return (EOVERFLOW);
	for (i = 0; i < count; i++) {
		if ((bmp->bm_chunks[i].type & (BTRFS_BLOCK_GROUP_DATA |
		    BTRFS_BLOCK_GROUP_METADATA | BTRFS_BLOCK_GROUP_SYSTEM)) ==
		    type)
			template = i;
	}
	if (template == UINT_MAX) {
		/* Balance can remove the last empty data group. */
		i = type == BTRFS_BLOCK_GROUP_DATA ? 0 :
		    type == BTRFS_BLOCK_GROUP_METADATA ? 1 : 2;
		*chunk = bmp->bm_chunks[0];
		chunk->type = type | bmp->bm_chunk_profile[i];
		chunk->nmirrors = chunk->type & BTRFS_BLOCK_GROUP_DUP ? 2 : 1;
		chunk->sub_stripes = 0;
	} else
		*chunk = bmp->bm_chunks[template];
	logical = bmp->bm_chunk_logical_end;
	if (logical > UINT64_MAX - 65535)
		return (EOVERFLOW);
	chunk->logical = roundup(logical, 65536);
	chunk->length = MAX(32ULL * 1024 * 1024, roundup(needed, 65536));
	if (type == BTRFS_BLOCK_GROUP_SYSTEM)
		chunk->length = MAX(8ULL * 1024 * 1024, roundup(needed, 65536));
	return (0);
}

/*
 * Prefer the member with most unallocated physical space, then try every
 * other member before reclaiming. Both DUP stripes always use one member.
 * Each smaller trial reselects all equal-length, disjoint copies.
 */
static int
chunk_place(struct btrfs_fs *bmp, struct btrfs_chunk_operation *op,
    uint64_t needed)
{
	struct btrfs_chunk_map *chunk = &op->chunk;
	struct btrfs_device *device;
	uint64_t device_size, available, most = 0, base = chunk->length;
	unsigned int d, i, j, first = 0;
	int error;

	for (i = 0; i < bmp->bm_ndevices; i++) {
		device = bmp->bm_devices[i];
		if (device == op->source)
			continue;
		available = letoh64(device->bd_item.total_bytes) -
		    letoh64(device->bd_item.bytes_used);
		if (available > most) {
			first = i;
			most = available;
		}
	}
	for (d = 0; d < bmp->bm_ndevices; d++) {
		device = bmp->bm_devices[(first + d) % bmp->bm_ndevices];
		if (device == op->source)
			continue;
		device_size = letoh64(device->bd_item.total_bytes);
		available = device_size - letoh64(device->bd_item.bytes_used);
		if (needed > available / chunk->nmirrors)
			continue;
		chunk->length = base;
		/* Leave headroom for metadata and small growth trials. */
		if (op->action != BTRFS_CHUNK_MOVE &&
		    (chunk->type & BTRFS_BLOCK_GROUP_DATA))
			chunk->length = MAX(base, MIN(256ULL * 1024 * 1024,
			    (available / (10 * chunk->nmirrors)) & ~65535ULL));
		for (j = 0; j < chunk->nmirrors; j++) {
			chunk->device[j] = device;
			chunk->devid[j] = letoh64(device->bd_item.devid);
			memcpy(chunk->dev_uuid[j], device->bd_item.uuid,
			    BTRFS_UUID_SIZE);
		}
		for (;;) {
			if (chunk->length < needed ||
			    chunk->length > UINT64_MAX - chunk->logical)
				break;
			error = ENOSPC;
			if (chunk->length <= available / chunk->nmirrors) {
				for (j = 0; j < chunk->nmirrors; j++) {
					error = btrfs_chunk_physical(
					    bmp->bm_chunks, bmp->bm_nchunks,
					    chunk, j, device_size);
					if (error != 0)
						break;
				}
			}
			if (error == 0) {
				op->member = device;
				return (0);
			}
			if (op->action == BTRFS_CHUNK_MOVE ||
			    chunk->length <= 1024 * 1024)
				break;
			chunk->length = (chunk->length / 2) & ~65535ULL;
		}
	}
	return (ENOSPC);
}

/*
 * Count or remove all free-space records in an empty group. Bitmap imports
 * can need more than the two records used by a newly allocated group, so
 * reserve their deletion cost before joining.
 */
static int
btrfs_free_chunk_items(struct btrfs_trans_handle *handle,
    struct btrfs_root *root, struct btrfs_block_group *group,
    unsigned int *countp)
{
	struct btrfs_path path = { 0 };
	struct btrfs_key target = { 0 }, key;
	const struct btrfs_key *found;
	uint64_t end = group->bbg_bytenr + group->bbg_length;
	unsigned int count = 0;
	int error;

	if (root == NULL) {
		*countp = 0;
		return (0);
	}
	target.objectid = htole64(group->bbg_bytenr);
	target.type = BTRFS_FREE_SPACE_INFO_KEY;
	error = btrfs_search_lower_bound(root, &target, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &found, NULL, NULL);
		if (error != 0 || letoh64(found->objectid) >= end)
			break;
		key = *found;
		if ((count == 0 && (key.objectid != target.objectid ||
		    key.type != BTRFS_FREE_SPACE_INFO_KEY ||
		    letoh64(key.offset) != group->bbg_length)) ||
		    (count != 0 && key.type != BTRFS_FREE_SPACE_EXTENT_KEY &&
		    key.type != BTRFS_FREE_SPACE_BITMAP_KEY)) {
			error = EINVAL;
			break;
		}
		if (count == UINT_MAX || (handle != NULL && count >= *countp)) {
			error = EOVERFLOW;
			break;
		}
		count++;
		if (handle == NULL)
			error = btrfs_next_item(&path);
		else {
			btrfs_release_path(&path);
			error = btrfs_delete_item(handle, root, &key);
			if (error != 0)
				break;
			error = btrfs_search_lower_bound(root, &target, &path);
		}
	}
	btrfs_release_path(&path);
	if (error == ENOENT)
		error = 0;
	if (error == 0 && (count < 2 ||
	    (handle != NULL && count != *countp)))
		error = EINVAL;
	*countp = count;
	return (error);
}

/* Shared tree acquisition, device validation/accounting and reservation. */
static int
chunk_prepare(struct btrfs_fs *bmp, struct btrfs_chunk_operation *op)
{
	struct btrfs_path path = { 0 };
	struct btrfs_key key = { 0 };
	const uint8_t *data;
	uint64_t owner, bytes, used;
	uint32_t nodesize = letoh32(bmp->bm_super.nodesize), size;
	int error;

	error = btrfs_get_root(bmp, BTRFS_CHUNK_TREE_OBJECTID, &op->chunk_root);
	if (error == 0)
		error = btrfs_get_root(bmp, BTRFS_DEV_TREE_OBJECTID, &op->dev_root);
	owner = (letoh64(bmp->bm_super.compat_ro_flags) &
	    BTRFS_FEATURE_COMPAT_RO_BLOCK_GROUP_TREE) ?
	    BTRFS_BLOCK_GROUP_TREE_OBJECTID : BTRFS_EXTENT_TREE_OBJECTID;
	if (error == 0)
		error = btrfs_get_root(bmp, owner, &op->group_root);
	if (error == 0 && (letoh64(bmp->bm_super.compat_ro_flags) &
	    BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE))
		error = btrfs_get_root(bmp, BTRFS_FREE_SPACE_TREE_OBJECTID,
		    &op->free_root);
	if (error == 0 && op->action == BTRFS_CHUNK_REMOVE)
		error = btrfs_free_chunk_items(NULL, op->free_root, op->group,
		    &op->nfree);
	if (error != 0)
		return (error);
	key.objectid = htole64(BTRFS_DEV_ITEMS_OBJECTID);
	key.type = BTRFS_DEV_ITEM_KEY;
	key.offset = op->member->bd_item.devid;
	error = btrfs_search_slot(op->chunk_root, &key, &path);
	if (error == 0)
		error = btrfs_path_item(&path, NULL, &data, &size);
	if (error == 0 && size != sizeof(op->device))
		error = EINVAL;
	if (error == 0)
		memcpy(&op->device, data, sizeof(op->device));
	btrfs_release_path(&path);
	if (error != 0)
		return (error);
	used = letoh64(op->device.bytes_used);
	if (memcmp(&op->device, &op->member->bd_item,
	    sizeof(op->device)) != 0)
		return (EINVAL);
	bytes = op->chunk.length * op->chunk.nmirrors;
	if (op->action == BTRFS_CHUNK_ADD ||
	    op->action == BTRFS_CHUNK_MOVE) {
		if (used > letoh64(op->device.total_bytes) ||
		    bytes > letoh64(op->device.total_bytes) - used)
			return (EINVAL);
		op->device.bytes_used = htole64(used + bytes);
	} else {
		if (bytes > used)
			return (EINVAL);
		op->device.bytes_used = htole64(used - bytes);
	}
	op->reservation.btr_metadata = (uint64_t)nodesize *
	    (BTRFS_RECLAIM_METADATA_BLOCKS +
	    (uint64_t)64 * (op->nfree > 2 ? op->nfree - 2 : 0));
	op->reservation.btr_system =
	    (uint64_t)nodesize * BTRFS_CHUNK_SYSTEM_BLOCKS;
	op->reservation.btr_reclaim = 1;
	/*
	 * This join may borrow protected cleanup space and flush pending work.
	 * It must never recurse into growth while we own bm_chunk_alloc_lock.
	 */
	op->reservation.btr_chunk = 1;
	return (0);
}

static void
chunk_encode(struct btrfs_chunk_operation *op)
{
	struct btrfs_chunk_map *chunk = &op->chunk;
	struct btrfs_chunk *item = (struct btrfs_chunk *)op->record;
	unsigned int j;

	item->length = htole64(chunk->length);
	item->owner = htole64(chunk->owner);
	item->stripe_len = htole64(chunk->stripe_len);
	item->type = htole64(chunk->type);
	item->io_align = htole32(chunk->io_align);
	item->io_width = htole32(chunk->io_width);
	item->sector_size = htole32(chunk->sector_size);
	item->num_stripes = htole16(chunk->nmirrors);
	item->sub_stripes = htole16(chunk->sub_stripes);
	for (j = 0; j < chunk->nmirrors; j++) {
		item->stripe[j].devid = htole64(chunk->devid[j]);
		item->stripe[j].offset = htole64(chunk->physical[j]);
		memcpy(item->stripe[j].dev_uuid, chunk->dev_uuid[j],
		    BTRFS_UUID_SIZE);
	}
	op->itemsize = offsetof(struct btrfs_chunk, stripe) +
	    chunk->nmirrors * sizeof(struct btrfs_stripe);
}

static int
chunk_add_prepare(struct btrfs_fs *bmp, struct btrfs_chunk_operation *op)
{
	struct btrfs_chunk_map *chunk = &op->chunk;
	struct btrfs_block_group *group;
	struct btrfs_free_extent *space;
	struct btrfs_key key = { 0 };
	unsigned int count = bmp->bm_nchunks;

	op->count = count + 1;
	op->chunks = mallocarray(op->count, sizeof(*op->chunks),
	    M_BTRFS, M_WAITOK);
	op->groups = mallocarray(op->count, sizeof(*op->groups),
	    M_BTRFS, M_WAITOK);
	memcpy(op->chunks, bmp->bm_chunks, count * sizeof(*op->chunks));
	memcpy(op->groups, bmp->bm_block_groups, count * sizeof(*op->groups));
	op->chunks[count] = *chunk;
	op->group = group = malloc(sizeof(*group), M_BTRFS, M_WAITOK | M_ZERO);
	mtx_init(&group->bbg_lock, IPL_NONE);
	TAILQ_INIT(&group->bbg_free_extents);
	group->bbg_bytenr = chunk->logical;
	group->bbg_length = chunk->length;
	group->bbg_flags = chunk->type;
	/* Placement excludes super stripes, so the entire new group is free. */
	space = malloc(sizeof(*space), M_BTRFS, M_WAITOK | M_ZERO);
	space->bfe_bytenr = chunk->logical;
	space->bfe_length = chunk->length;
	TAILQ_INSERT_TAIL(&group->bbg_free_extents, space, bfe_entry);
	group->bbg_free_bytes = chunk->length;
	op->groups[count] = group;

	chunk_encode(op);
	if (chunk->type & BTRFS_BLOCK_GROUP_SYSTEM) {
		op->system_size = sizeof(key) + op->itemsize;
		if (letoh32(bmp->bm_super.sys_chunk_array_size) >
		    BTRFS_SYSTEM_CHUNK_ARRAY_SIZE - op->system_size)
			return (ENOSPC);
		key.objectid = htole64(BTRFS_FIRST_CHUNK_TREE_OBJECTID);
		key.type = BTRFS_CHUNK_ITEM_KEY;
		key.offset = htole64(chunk->logical);
		memcpy(op->system, &key, sizeof(key));
		memcpy(op->system + sizeof(key), op->record, op->itemsize);
	}
	return (chunk_prepare(bmp, op));
}

static int
chunk_remove_prepare(struct btrfs_fs *bmp, struct btrfs_chunk_operation *op,
    unsigned int index)
{
	struct btrfs_block_group *group = bmp->bm_block_groups[index];
	unsigned int count = bmp->bm_nchunks, i;
	int empty;

	rw_assert_wrlock(&bmp->bm_chunk_alloc_lock);
	op->action = BTRFS_CHUNK_REMOVE;
	mtx_enter(&group->bbg_lock);
	empty = !group->bbg_removing && group->bbg_disk_used == 0 &&
	    group->bbg_reserved_bytes == 0 &&
	    group->bbg_allocated_bytes == 0 && group->bbg_pinned_bytes == 0;
	if (empty) {
		group->bbg_removing = 1;
		op->group = group;
	}
	mtx_leave(&group->bbg_lock);
	if (!empty)
		return (EBUSY);
	/* Own the exclusion even if any subsequent preparation or join fails. */
	op->chunk = bmp->bm_chunks[index];
	op->member = op->chunk.device[0];
	op->count = count - 1;
	op->chunks = mallocarray(op->count, sizeof(*op->chunks),
	    M_BTRFS, M_WAITOK);
	op->groups = mallocarray(op->count, sizeof(*op->groups),
	    M_BTRFS, M_WAITOK);
	for (i = 0; i < count; i++) {
		if (i == index)
			continue;
		op->chunks[i - (i > index)] = bmp->bm_chunks[i];
		op->groups[i - (i > index)] = bmp->bm_block_groups[i];
	}
	return (chunk_prepare(bmp, op));
}

static int
chunk_add_apply(struct btrfs_trans_handle *handle,
    struct btrfs_chunk_operation *op)
{
	struct btrfs_fs *bmp = handle->bth_transaction->bt_mount;
	struct btrfs_chunk_map *chunk = &op->chunk;
	struct btrfs_key key = { 0 };
	struct btrfs_dev_extent extent = { 0 };
	struct btrfs_block_group_item bgitem = { 0 };
	struct btrfs_free_space_info info = { 0 };
	unsigned int j;
	int error;

	key.objectid = htole64(BTRFS_FIRST_CHUNK_TREE_OBJECTID);
	key.type = BTRFS_CHUNK_ITEM_KEY;
	key.offset = htole64(chunk->logical);
	error = btrfs_insert_item(handle, op->chunk_root, &key, op->record,
	    op->itemsize);
	if (error != 0)
		return (error);
	extent.chunk_tree = htole64(BTRFS_CHUNK_TREE_OBJECTID);
	extent.chunk_objectid = htole64(BTRFS_FIRST_CHUNK_TREE_OBJECTID);
	extent.chunk_offset = htole64(chunk->logical);
	extent.length = htole64(chunk->length);
	memcpy(extent.chunk_tree_uuid, bmp->bm_chunk_tree_uuid, BTRFS_UUID_SIZE);
	for (j = 0; j < chunk->nmirrors; j++) {
		key.objectid = htole64(chunk->devid[j]);
		key.type = BTRFS_DEV_EXTENT_KEY;
		key.offset = htole64(chunk->physical[j]);
		error = btrfs_insert_item(handle, op->dev_root, &key, &extent,
		    sizeof(extent));
		if (error != 0)
			return (error);
	}
	bgitem.chunk_objectid = htole64(BTRFS_FIRST_CHUNK_TREE_OBJECTID);
	bgitem.flags = htole64(chunk->type);
	key.objectid = htole64(chunk->logical);
	key.type = BTRFS_BLOCK_GROUP_ITEM_KEY;
	key.offset = htole64(chunk->length);
	error = btrfs_insert_item(handle, op->group_root, &key, &bgitem,
	    sizeof(bgitem));
	if (error != 0)
		return (error);
	if (op->free_root != NULL) {
		info.extent_count = htole32(1);
		key.type = BTRFS_FREE_SPACE_INFO_KEY;
		error = btrfs_insert_item(handle, op->free_root, &key, &info,
		    sizeof(info));
		if (error == 0) {
			key.type = BTRFS_FREE_SPACE_EXTENT_KEY;
			error = btrfs_insert_item(handle, op->free_root, &key,
			    NULL, 0);
		}
	}
	return (error);
}

static int
chunk_remove_apply(struct btrfs_trans_handle *handle,
    struct btrfs_chunk_operation *op)
{
	struct btrfs_chunk_map *chunk = &op->chunk;
	struct btrfs_key key = { 0 };
	unsigned int i;
	int error;

	key.objectid = htole64(BTRFS_FIRST_CHUNK_TREE_OBJECTID);
	key.type = BTRFS_CHUNK_ITEM_KEY;
	key.offset = htole64(chunk->logical);
	error = btrfs_delete_item(handle, op->chunk_root, &key);
	for (i = 0; error == 0 && i < chunk->nmirrors; i++) {
		key.objectid = htole64(chunk->devid[i]);
		key.type = BTRFS_DEV_EXTENT_KEY;
		key.offset = htole64(chunk->physical[i]);
		error = btrfs_delete_item(handle, op->dev_root, &key);
	}
	if (error != 0)
		return (error);
	key.objectid = htole64(chunk->logical);
	key.type = BTRFS_BLOCK_GROUP_ITEM_KEY;
	key.offset = htole64(chunk->length);
	error = btrfs_delete_item(handle, op->group_root, &key);
	if (error == 0)
		error = btrfs_free_chunk_items(handle, op->free_root, op->group,
		    &op->nfree);
	return (error);
}

static int
chunk_move_apply(struct btrfs_trans_handle *handle,
    struct btrfs_chunk_operation *op)
{
	struct btrfs_fs *bmp = handle->bth_transaction->bt_mount;
	struct btrfs_key key = { 0 };
	struct btrfs_dev_extent extent = { 0 };
	unsigned int i;
	int error;

	key.objectid = htole64(BTRFS_DEV_ITEMS_OBJECTID);
	key.type = BTRFS_DEV_ITEM_KEY;
	key.offset = op->source_item.devid;
	error = btrfs_replace_item(handle, op->chunk_root, &key,
	    &op->source_item, sizeof(op->source_item));
	key.objectid = htole64(BTRFS_FIRST_CHUNK_TREE_OBJECTID);
	key.type = BTRFS_CHUNK_ITEM_KEY;
	key.offset = htole64(op->chunk.logical);
	if (error == 0)
		error = btrfs_replace_item(handle, op->chunk_root, &key,
		    op->record, op->itemsize);
	extent.chunk_tree = htole64(BTRFS_CHUNK_TREE_OBJECTID);
	extent.chunk_objectid = htole64(BTRFS_FIRST_CHUNK_TREE_OBJECTID);
	extent.chunk_offset = htole64(op->chunk.logical);
	extent.length = htole64(op->chunk.length);
	memcpy(extent.chunk_tree_uuid, bmp->bm_chunk_tree_uuid, BTRFS_UUID_SIZE);
	for (i = 0; error == 0 && i < op->chunk.nmirrors; i++) {
		key.objectid = htole64(op->oldchunk.devid[i]);
		key.type = BTRFS_DEV_EXTENT_KEY;
		key.offset = htole64(op->oldchunk.physical[i]);
		error = btrfs_delete_item(handle, op->dev_root, &key);
		key.objectid = htole64(op->chunk.devid[i]);
		key.offset = htole64(op->chunk.physical[i]);
		if (error == 0)
			error = btrfs_insert_item(handle, op->dev_root, &key,
			    &extent, sizeof(extent));
	}
	return (error);
}

/*
 * Consumes a prepared operation on every return. Join failure is pre-mutation;
 * application failure aborts the transaction before releasing the operation.
 * Once attached, transaction completion alone owns publication or rollback.
 */
static int
chunk_execute(struct btrfs_fs *bmp, struct btrfs_chunk_operation *op)
{
	struct btrfs_trans_handle *handle;
	struct btrfs_transaction *trans;
	struct btrfs_key key = { 0 };
	uint64_t generation;
	unsigned int i;
	int error;

	rw_assert_wrlock(&bmp->bm_chunk_alloc_lock);
	error = btrfs_trans_join(bmp, &op->reservation, &handle);
	if (error != 0) {
		chunk_discard(op);
		return (error);
	}
	trans = handle->bth_transaction;
	generation = trans->bt_generation;
	KASSERT(trans->bt_chunk_op == NULL);
	if (op->action == BTRFS_CHUNK_REMOVE) {
		/*
		 * Reassignment can change physical buffer sizes between data
		 * sectors and metadata nodes. Evict while reservations are
		 * excluded, before returning stripes to the physical planner.
		 */
		for (i = 0; i < op->chunk.nmirrors; i++)
			btrfs_invalidate_physical(bmp, op->chunk.device[i],
			    op->chunk.physical[i], op->chunk.length);
	}
	key.objectid = htole64(BTRFS_DEV_ITEMS_OBJECTID);
	key.type = BTRFS_DEV_ITEM_KEY;
	key.offset = op->member->bd_item.devid;
	if (op->action == BTRFS_DEVICE_ADD)
		error = btrfs_insert_item(handle, op->chunk_root, &key,
		    &op->device, sizeof(op->device));
	else if (op->action == BTRFS_DEVICE_REMOVE)
		error = btrfs_delete_item(handle, op->chunk_root, &key);
	else
		error = btrfs_replace_item(handle, op->chunk_root, &key,
		    &op->device, sizeof(op->device));
	if (error == 0) {
		if (op->action == BTRFS_CHUNK_ADD)
			error = chunk_add_apply(handle, op);
		else if (op->action == BTRFS_CHUNK_REMOVE)
			error = chunk_remove_apply(handle, op);
		else if (op->action == BTRFS_CHUNK_MOVE)
			error = chunk_move_apply(handle, op);
	}
	if (error != 0) {
		btrfs_trans_abort(handle, error);
		(void)btrfs_trans_end(handle);
		chunk_discard(op);
		return (error);
	}
	trans->bt_chunk_op = op;
	/* Do not touch op or its group after dropping this handle. */
	error = btrfs_trans_end(handle);
	if (error == 0)
		error = btrfs_trans_commit(bmp, generation, curproc);
	return (error);
}

/*
 * Caller owns the mount administration lock, chunk lock and writer gate.
 * One chunk per commit bounds reservations and makes interrupted evacuation
 * restartable without a separate relocation tree or persistent cursor.
 * ADD consumes the new member on every return.
 */
int
btrfs_device_change(struct btrfs_fs *bmp, struct btrfs_device *device, int add)
{
	struct btrfs_chunk_operation *op;
	uint64_t bytes;
	unsigned int i;
	int error;

	rw_assert_wrlock(&bmp->bm_chunk_alloc_lock);
	if (!add) {
		for (i = 0; i < bmp->bm_nchunks; i++) {
			if (bmp->bm_chunks[i].device[0] != device)
				continue;
			op = malloc(sizeof(*op), M_BTRFS, M_WAITOK | M_ZERO);
			op->action = BTRFS_CHUNK_MOVE;
			op->index = i;
			op->source = device;
			op->oldchunk = op->chunk = bmp->bm_chunks[i];
			error = chunk_place(bmp, op, op->chunk.length);
			if (error == 0)
				error = chunk_prepare(bmp, op);
			if (error != 0) {
				chunk_discard(op);
				return (error);
			}
			bytes = op->chunk.length * op->chunk.nmirrors;
			op->source_item = device->bd_item;
			KASSERT(letoh64(op->source_item.bytes_used) >= bytes);
			op->source_item.bytes_used = htole64(
			    letoh64(op->source_item.bytes_used) - bytes);
			chunk_encode(op);
			error = chunk_execute(bmp, op);
			if (error != 0)
				return (error);
		}
		KASSERT(device->bd_item.bytes_used == 0);
	}
	op = malloc(sizeof(*op), M_BTRFS, M_WAITOK | M_ZERO);
	op->fs = bmp;
	op->action = add ? BTRFS_DEVICE_ADD : BTRFS_DEVICE_REMOVE;
	op->member = device;
	op->device = device->bd_item;
	error = btrfs_get_root(bmp, BTRFS_CHUNK_TREE_OBJECTID, &op->chunk_root);
	if (error != 0) {
		chunk_discard(op);
		return (error);
	}
	op->reservation.btr_metadata =
	    (uint64_t)letoh32(bmp->bm_super.nodesize) *
	    BTRFS_RECLAIM_METADATA_BLOCKS;
	op->reservation.btr_system =
	    (uint64_t)letoh32(bmp->bm_super.nodesize) * BTRFS_CHUNK_SYSTEM_BLOCKS;
	op->reservation.btr_reclaim = op->reservation.btr_chunk = 1;
	return (chunk_execute(bmp, op));
}

/* Super publication includes a pending add, and omits a pending removal. */
struct btrfs_device *
btrfs_commit_device(struct btrfs_fs *bmp, unsigned int index)
{
	struct btrfs_chunk_operation *op = bmp->bm_transaction == NULL ?
	    NULL : bmp->bm_transaction->bt_chunk_op;
	unsigned int i;

	/*
	 * Publish the new member first. Once an old member advertises the new
	 * count, the new device must already have a discoverable UUID/super.
	 */
	if (op != NULL && op->action == BTRFS_DEVICE_ADD) {
		if (index == 0)
			return (op->member);
		index--;
	}
	for (i = 0; i < bmp->bm_ndevices; i++) {
		if (op != NULL && op->action == BTRFS_DEVICE_REMOVE &&
		    bmp->bm_devices[i] == op->member)
			continue;
		if (index-- == 0)
			return (bmp->bm_devices[i]);
	}
	return (NULL);
}

int
btrfs_chunk_copy(struct btrfs_transaction *trans)
{
	struct btrfs_chunk_operation *op = trans->bt_chunk_op;
	struct btrfs_fs *bmp = trans->bt_mount;
	struct btrfs_root *root;
	struct btrfs_path path = { 0 };
	struct btrfs_key key = { 0 };
	const struct btrfs_key *found;
	const struct btrfs_extent_item *item;
	const struct btrfs_tree_block_info *info;
	const uint8_t *data;
	uint64_t logical, length, flags, end, generation;
	uint32_t size;
	int error, metadata, level;

	if (op == NULL || op->action != BTRFS_CHUNK_MOVE)
		return (0);
	KASSERT(trans->bt_writers == 0 && !trans->bt_commit_handle);
	error = btrfs_get_root(bmp, BTRFS_EXTENT_TREE_OBJECTID, &root);
	if (error != 0)
		return (error);
	end = op->chunk.logical + op->chunk.length;
	key.objectid = htole64(op->chunk.logical);
	error = btrfs_search_lower_bound(root, &key, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &found, &data, &size);
		if (error != 0 || letoh64(found->objectid) >= end)
			break;
		if (found->type != BTRFS_EXTENT_ITEM_KEY &&
		    found->type != BTRFS_METADATA_ITEM_KEY) {
			error = btrfs_next_item(&path);
			continue;
		}
		if (size < sizeof(*item)) {
			error = EINVAL;
			break;
		}
		item = (const void *)data;
		flags = letoh64(item->flags);
		metadata = (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK) != 0;
		generation = letoh64(item->generation);
		logical = letoh64(found->objectid);
		length = found->type == BTRFS_METADATA_ITEM_KEY ?
		    letoh32(bmp->bm_super.nodesize) : letoh64(found->offset);
		if (letoh64(item->refs) == 0 || generation == 0 ||
		    generation > trans->bt_generation ||
		    (flags & (BTRFS_EXTENT_FLAG_TREE_BLOCK |
		    BTRFS_EXTENT_FLAG_DATA)) != (metadata ?
		    BTRFS_EXTENT_FLAG_TREE_BLOCK : BTRFS_EXTENT_FLAG_DATA) ||
		    length > end - logical ||
		    (metadata && length != letoh32(bmp->bm_super.nodesize))) {
			error = EINVAL;
			break;
		}
		level = -1;
		if (metadata) {
			if (found->type == BTRFS_METADATA_ITEM_KEY)
				level = letoh64(found->offset);
			else {
				if (size < sizeof(*item) + sizeof(*info)) {
					error = EINVAL;
					break;
				}
				info = (const void *)(item + 1);
				level = info->level;
			}
			if (level < 0 || level >= BTRFS_MAX_LEVEL) {
				error = EINVAL;
				break;
			}
		} else if (found->type == BTRFS_METADATA_ITEM_KEY) {
			error = EINVAL;
			break;
		}
		error = btrfs_copy_extent(bmp, &op->chunk, logical, length,
		    level, generation);
		if (error != 0)
			break;
		error = btrfs_next_item(&path);
	}
	btrfs_release_path(&path);
	return (error == ENOENT ? 0 : error);
}

/*
 * Return one empty group of another allocation type after physical growth
 * fails. Keep one group per profile as a growth template, and retain system
 * groups whose mappings are also part of the bootstrap array.
 */
static int
chunk_reclaim(struct btrfs_fs *bmp, uint64_t type)
{
	struct btrfs_chunk_operation *op;
	unsigned int count = bmp->bm_nchunks, i, j;
	int error;

	rw_assert_wrlock(&bmp->bm_chunk_alloc_lock);
	/* Prefer later growth over the initial, often smaller, group. */
	for (i = count; i-- != 0;) {
		if (bmp->bm_chunks[i].type & (type | BTRFS_BLOCK_GROUP_SYSTEM))
			continue;
		for (j = 0; j < count; j++)
			if (j != i && bmp->bm_chunks[j].type ==
			    bmp->bm_chunks[i].type)
				break;
		if (j == count)
			continue;
		op = malloc(sizeof(*op), M_BTRFS, M_WAITOK | M_ZERO);
		error = chunk_remove_prepare(bmp, op, i);
		if (error != 0) {
			chunk_discard(op);
			if (error == EBUSY || error == ENOSPC)
				continue;
			return (error);
		}
		error = chunk_execute(bmp, op);
		/* Replenishment can fail after publication has freed the group. */
		if (error != ENOSPC || bmp->bm_readonly)
			return (error);
	}
	return (ENOSPC);
}

static uint64_t
chunk_available(struct btrfs_fs *bmp, uint64_t type)
{
	struct btrfs_block_group *group;
	uint64_t available = 0;
	unsigned int i;

	rw_assert_wrlock(&bmp->bm_chunk_alloc_lock);
	for (i = 0; i < bmp->bm_nblock_groups; i++) {
		group = bmp->bm_block_groups[i];
		mtx_enter(&group->bbg_lock);
		if ((group->bbg_flags & type) && !group->bbg_removing)
			available += group->bbg_free_bytes;
		mtx_leave(&group->bbg_lock);
	}
	return (available);
}

/* Balance already owns the chunk lock. Force a destination group when an
 * allocation has enough total free bytes but no sufficiently large gap. */
int
btrfs_balance_grow(struct btrfs_fs *bmp, uint64_t type, uint64_t needed)
{
	struct btrfs_chunk_operation *op;
	int error;

	rw_assert_wrlock(&bmp->bm_chunk_alloc_lock);
	op = malloc(sizeof(*op), M_BTRFS, M_WAITOK | M_ZERO);
	op->action = BTRFS_CHUNK_ADD;
	error = chunk_plan(bmp, op, type, needed);
	if (error == 0)
		error = chunk_place(bmp, op, needed);
	if (error == 0)
		error = chunk_add_prepare(bmp, op);
	if (error != 0)
		chunk_discard(op);
	else
		error = chunk_execute(bmp, op);
	return (error);
}

static int
balance_matches(struct btrfs_block_group *group,
    const struct btrfs_balance_filter *filter)
{
	uint64_t used = group->bbg_disk_used, length = group->bbg_length;
	uint64_t lower, upper;

	/* Percent arithmetic without multiplying a filesystem-sized value. */
	lower = (length / 100) * filter->min +
	    ((length % 100) * filter->min + 99) / 100;
	upper = (length / 100) * filter->max +
	    ((length % 100) * filter->max + 99) / 100;
	return (used >= lower && (filter->max == 100 ||
	    (filter->max == 0 ? used == 0 : used < upper)));
}

/*
 * Snapshot the selection once. Destinations created by this run must never
 * become fresh work. Visit high logical addresses first, allowing the usual
 * allocator's preference for earlier groups to compact sparse allocations.
 */
int
btrfs_balance(struct btrfs_fs *bmp, struct btrfs_ioctl_balance *args)
{
	const struct btrfs_balance_filter *filters[] = {
	    &args->data, &args->metadata, &args->system
	};
	const uint64_t types[] = { BTRFS_BLOCK_GROUP_DATA,
	    BTRFS_BLOCK_GROUP_METADATA, BTRFS_BLOCK_GROUP_SYSTEM };
	struct btrfs_block_group *group;
	struct btrfs_chunk_operation *op;
	struct btrfs_trans_reservation reserve = { .btr_chunk = 1 };
	struct btrfs_trans_handle *handle;
	struct btrfs_root *root;
	struct btrfs_path path = { 0 };
	struct btrfs_key key = { 0 };
	uint64_t *selected, limits[3] = { 0 }, needed, type;
	unsigned int count = bmp->bm_nchunks, n = 0, i, j, k;
	int error = 0, nospace = 0, enderror;

	rw_assert_wrlock(&bmp->bm_chunk_alloc_lock);
	selected = mallocarray(count, sizeof(*selected), M_BTRFS, M_WAITOK);
	for (i = count; i-- != 0;) {
		group = bmp->bm_block_groups[i];
		/* Mixed block groups require one common data/metadata filter. */
		if ((group->bbg_flags & (BTRFS_BLOCK_GROUP_DATA |
		    BTRFS_BLOCK_GROUP_METADATA)) == (BTRFS_BLOCK_GROUP_DATA |
		    BTRFS_BLOCK_GROUP_METADATA)) {
			error = EOPNOTSUPP;
			goto out;
		}
		for (j = 0; j < nitems(types); j++)
			if (group->bbg_flags & types[j])
				break;
		if (j == nitems(types) || !(args->flags & (1U << j)) ||
		    limits[j] >= filters[j]->limit ||
		    !balance_matches(group, filters[j]))
			continue;
		selected[n++] = group->bbg_bytenr;
		limits[j]++;
	}
	mtx_enter(&bmp->bm_trans_mtx);
	bmp->bm_balance.expected = n;
	mtx_leave(&bmp->bm_trans_mtx);
	for (k = 0; k < n; k++) {
		error = btrfs_balance_interrupted(bmp);
		if (error != 0)
			break;
		mtx_enter(&bmp->bm_trans_mtx);
		bmp->bm_balance.considered++;
		mtx_leave(&bmp->bm_trans_mtx);
		for (i = 0; i < bmp->bm_nchunks; i++)
			if (bmp->bm_chunks[i].logical == selected[k])
				break;
		KASSERT(i < bmp->bm_nchunks);
		group = bmp->bm_block_groups[i];
		type = group->bbg_flags & (BTRFS_BLOCK_GROUP_DATA |
		    BTRFS_BLOCK_GROUP_METADATA | BTRFS_BLOCK_GROUP_SYSTEM);
		mtx_enter(&group->bbg_lock);
		group->bbg_removing = 1;
		mtx_leave(&group->bbg_lock);

		/*
		 * A clean transaction still owns protected promises in this
		 * group. Supply enough external workspace, then publish a COW
		 * so the next generation's promises exclude the source.
		 */
		if (group->bbg_disk_used != 0 || group->bbg_reserved_bytes != 0) {
			needed = (uint64_t)letoh32(bmp->bm_super.nodesize) * 1024;
			if (type != BTRFS_BLOCK_GROUP_DATA &&
			    chunk_available(bmp, type) < needed)
				error = btrfs_balance_grow(bmp, type, needed);
			if (error == 0 && group->bbg_reserved_bytes != 0) {
				reserve.btr_metadata = needed / 4;
				error = btrfs_trans_join(bmp, &reserve, &handle);
				if (error == 0) {
					error = btrfs_get_root(bmp,
					    BTRFS_ROOT_TREE_OBJECTID, &root);
					if (error == 0)
						error = btrfs_search_slot_write(
						    handle, root, &key, &path);
					btrfs_release_path(&path);
					if (error == ENOENT)
						error = 0;
					if (error != 0)
						btrfs_trans_abort(handle, error);
					enderror = btrfs_trans_end(handle);
					if (error == 0)
						error = enderror;
					if (error == 0)
						error = btrfs_commit_current(bmp,
						    curproc);
				}
			}
			if (error == 0)
				error = btrfs_balance_relocate(bmp, group);
		}
		mtx_enter(&group->bbg_lock);
		group->bbg_removing = 0;
		mtx_leave(&group->bbg_lock);
		if (error == 0) {
			/* Growth may have replaced the index, but not this group. */
			for (i = 0; bmp->bm_block_groups[i] != group; i++)
				KASSERT(i + 1 < bmp->bm_nblock_groups);
			op = malloc(sizeof(*op), M_BTRFS, M_WAITOK | M_ZERO);
			error = chunk_remove_prepare(bmp, op, i);
			if (error != 0)
				chunk_discard(op);
			else
				error = chunk_execute(bmp, op);
		}
		if (error == ENOSPC && !bmp->bm_readonly) {
			nospace = 1;
			error = 0;
			continue;
		}
		if (error != 0)
			break;
		mtx_enter(&bmp->bm_trans_mtx);
		bmp->bm_balance.completed++;
		mtx_leave(&bmp->bm_trans_mtx);
	}
out:
	free(selected, M_BTRFS, count * sizeof(*selected));
	return (error != 0 ? error : nospace ? ENOSPC : 0);
}

/* Enter without a handle; the protected reserve pays for this operation. */
int
btrfs_chunk_grow(struct btrfs_fs *bmp, uint64_t type, uint64_t needed)
{
	const uint64_t types = BTRFS_BLOCK_GROUP_DATA |
	    BTRFS_BLOCK_GROUP_METADATA | BTRFS_BLOCK_GROUP_SYSTEM;
	struct btrfs_chunk_operation *op;
	uint64_t system_needed;
	int error;

	if ((type & types) != type || type == 0 || (type & (type - 1)) != 0 ||
	    needed == 0 || needed > UINT64_MAX - 65535)
		return (EINVAL);
	rw_enter_write(&bmp->bm_chunk_alloc_lock);
	for (;;) {
		error = btrfs_commit_current(bmp, curproc);
		if (error != 0)
			break;
		if (bmp->bm_readonly) {
			error = EROFS;
			break;
		}
		/* Chunk-tree COW must have system space before other growth. */
		system_needed = (uint64_t)letoh32(bmp->bm_super.nodesize) *
		    BTRFS_CHUNK_SYSTEM_BLOCKS;
		if (chunk_available(bmp, BTRFS_BLOCK_GROUP_SYSTEM) <
		    system_needed) {
			type = BTRFS_BLOCK_GROUP_SYSTEM;
			needed = system_needed;
		}
		if (chunk_available(bmp, type) >= needed) {
			error = 0;
			break;
		}
		op = malloc(sizeof(*op), M_BTRFS, M_WAITOK | M_ZERO);
		op->action = BTRFS_CHUNK_ADD;
		error = chunk_plan(bmp, op, type, needed);
		if (error != 0) {
			chunk_discard(op);
			break;
		}
		error = chunk_place(bmp, op, needed);
		if (error != 0) {
			chunk_discard(op);
			if (error == ENOSPC) {
				error = chunk_reclaim(bmp, type);
				if (error == 0)
					continue;
			}
			break;
		}
		error = chunk_add_prepare(bmp, op);
		if (error != 0)
			chunk_discard(op);
		else
			error = chunk_execute(bmp, op);
		break;
	}
	rw_exit_write(&bmp->bm_chunk_alloc_lock);
	return (error);
}

/* Commit accounting must skip the group whose tree records we removed. */
int
btrfs_chunk_removed(struct btrfs_transaction *trans,
    const struct btrfs_block_group *group)
{
	struct btrfs_chunk_operation *op = trans->bt_chunk_op;

	return (op != NULL && op->action == BTRFS_CHUNK_REMOVE &&
	    op->group == group);
}

/* Stage accounting and bootstrap mappings in the commit's superblock image. */
int
btrfs_chunk_update_super(struct btrfs_transaction *trans,
    struct btrfs_super_block *sb)
{
	struct btrfs_chunk_operation *op = trans->bt_chunk_op;
	struct btrfs_key *key;
	struct btrfs_chunk *chunk;
	uint32_t size, offset, length;

	if (op == NULL)
		return (0);
	if (sb->dev_item.devid == op->device.devid)
		sb->dev_item = op->device;
	if (op->action == BTRFS_CHUNK_REMOVE &&
	    (op->chunk.type & BTRFS_BLOCK_GROUP_SYSTEM)) {
		size = letoh32(sb->sys_chunk_array_size);
		for (offset = 0; offset < size; offset += length) {
			if (size - offset < sizeof(*key) +
			    offsetof(struct btrfs_chunk, stripe))
				return (EINVAL);
			key = (void *)(sb->sys_chunk_array + offset);
			chunk = (void *)(key + 1);
			length = sizeof(*key) +
			    offsetof(struct btrfs_chunk, stripe) +
			    letoh16(chunk->num_stripes) *
			    sizeof(struct btrfs_stripe);
			if (length > size - offset)
				return (EINVAL);
			if (letoh64(key->offset) != op->chunk.logical)
				continue;
			memmove(key, (uint8_t *)key + length,
			    size - offset - length);
			memset(sb->sys_chunk_array + size - length, 0, length);
			sb->sys_chunk_array_size = htole32(size - length);
			return (0);
		}
		return (EINVAL);
	}
	if (op->action == BTRFS_DEVICE_ADD) {
		sb->num_devices = htole64(letoh64(sb->num_devices) + 1);
		sb->total_bytes = htole64(letoh64(sb->total_bytes) +
		    letoh64(op->device.total_bytes));
	} else if (op->action == BTRFS_DEVICE_REMOVE) {
		sb->num_devices = htole64(letoh64(sb->num_devices) - 1);
		sb->total_bytes = htole64(letoh64(sb->total_bytes) -
		    letoh64(op->device.total_bytes));
		/* The template identity must itself still be a live member. */
		if (sb->dev_item.devid == op->device.devid)
			sb->dev_item = btrfs_commit_device(trans->bt_mount, 0)->
			    bd_item;
	} else if (op->action == BTRFS_CHUNK_MOVE) {
		if (sb->dev_item.devid == op->source_item.devid)
			sb->dev_item = op->source_item;
		if (!(op->chunk.type & BTRFS_BLOCK_GROUP_SYSTEM))
			return (0);
		size = letoh32(sb->sys_chunk_array_size);
		for (offset = 0; offset < size; offset += length) {
			if (size - offset < sizeof(*key) +
			    offsetof(struct btrfs_chunk, stripe))
				return (EINVAL);
			key = (void *)(sb->sys_chunk_array + offset);
			chunk = (void *)(key + 1);
			length = sizeof(*key) +
			    offsetof(struct btrfs_chunk, stripe) +
			    letoh16(chunk->num_stripes) *
			    sizeof(struct btrfs_stripe);
			if (length > size - offset)
				return (EINVAL);
			if (letoh64(key->offset) != op->chunk.logical)
				continue;
			if (length != sizeof(*key) + op->itemsize)
				return (EINVAL);
			memcpy(chunk, op->record, op->itemsize);
			return (0);
		}
		return (EINVAL);
	}
	if (op->system_size != 0) {
		size = letoh32(sb->sys_chunk_array_size);
		if (size > BTRFS_SYSTEM_CHUNK_ARRAY_SIZE ||
		    op->system_size > BTRFS_SYSTEM_CHUNK_ARRAY_SIZE - size)
			return (ENOSPC);
		memcpy(sb->sys_chunk_array + size, op->system, op->system_size);
		sb->sys_chunk_array_size = htole32(size + op->system_size);
	}
	return (0);
}

/* A commit stages each member super without publishing device accounting. */
void
btrfs_chunk_device_item(struct btrfs_transaction *trans,
    struct btrfs_device *device, struct btrfs_dev_item *item)
{
	struct btrfs_chunk_operation *op = trans->bt_chunk_op;

	*item = op != NULL && op->member == device ?
	    op->device : device->bd_item;
	if (op != NULL && op->source == device)
		*item = op->source_item;
}

/* Called only after durable commit, before the next generation's reserves. */
void
btrfs_chunk_publish(struct btrfs_transaction *trans)
{
	struct btrfs_fs *bmp = trans->bt_mount;
	struct btrfs_chunk_operation *op = trans->bt_chunk_op;
	struct btrfs_chunk_map *oldchunks;
	struct btrfs_block_group **oldgroups;
	unsigned int count;

	if (op == NULL)
		return;
	KASSERT(trans->bt_writers == 0);
	KASSERT(!trans->bt_commit_handle);
	/*
	 * Old-map reads finish before a stripe can be reused or a member closed.
	 * Busy returned buffers are drained by invalidate/close below.
	 */
	rw_enter_write(&bmp->bm_io_lock);
	if (op->action == BTRFS_DEVICE_ADD) {
		bmp->bm_devices[bmp->bm_ndevices++] = op->member;
		op->member = NULL;
		goto done;
	}
	if (op->action == BTRFS_DEVICE_REMOVE) {
		for (count = 0; bmp->bm_devices[count] != op->member; count++)
			KASSERT(count + 1 < bmp->bm_ndevices);
		bmp->bm_ndevices--;
		memmove(&bmp->bm_devices[count], &bmp->bm_devices[count + 1],
		    (bmp->bm_ndevices - count) * sizeof(*bmp->bm_devices));
		bmp->bm_devices[bmp->bm_ndevices] = NULL;
		/* The ioctl caller now owns erasure and closure of this member. */
		op->member = NULL;
		goto done;
	}
	if (op->action == BTRFS_CHUNK_MOVE) {
		rw_enter_write(&bmp->bm_mapping_lock);
		bmp->bm_chunks[op->index] = op->chunk;
		op->member->bd_item = op->device;
		op->source->bd_item = op->source_item;
		rw_exit_write(&bmp->bm_mapping_lock);
		btrfs_space_moved(bmp, bmp->bm_block_groups[op->index],
		    &op->oldchunk);
		for (count = 0; count < op->oldchunk.nmirrors; count++)
			btrfs_invalidate_physical(bmp, op->source,
			    op->oldchunk.physical[count], op->oldchunk.length);
		goto done;
	}
	rw_enter_write(&bmp->bm_mapping_lock);
	count = bmp->bm_nchunks;
	KASSERT(bmp->bm_nblock_groups == count);
	KASSERT(op->count == (op->action == BTRFS_CHUNK_REMOVE ?
	    count - 1 : count + 1));
	oldchunks = bmp->bm_chunks;
	oldgroups = bmp->bm_block_groups;
	bmp->bm_chunks = op->chunks;
	bmp->bm_block_groups = op->groups;
	bmp->bm_nchunks = bmp->bm_nblock_groups = op->count;
	op->member->bd_item = op->device;
	if (op->action == BTRFS_CHUNK_ADD)
		bmp->bm_chunk_logical_end = op->chunk.logical + op->chunk.length;
	/* The filesystem now owns both indexes and, for growth, the new group. */
	op->chunks = NULL;
	op->groups = NULL;
	rw_exit_write(&bmp->bm_mapping_lock);
	if (op->action == BTRFS_CHUNK_REMOVE)
		chunk_free_group(op->group);
	op->group = NULL;
	free(oldchunks, M_BTRFS, count * sizeof(*oldchunks));
	free(oldgroups, M_BTRFS, count * sizeof(*oldgroups));
done:
	rw_exit_write(&bmp->bm_io_lock);
	trans->bt_chunk_op = NULL;
	chunk_discard(op);
}

void
btrfs_chunk_abort(struct btrfs_transaction *trans)
{
	struct btrfs_chunk_operation *op = trans->bt_chunk_op;

	KASSERT(trans->bt_writers == 0);
	KASSERT(!trans->bt_commit_handle);
	trans->bt_chunk_op = NULL;
	if (op != NULL)
		chunk_discard(op);
}
