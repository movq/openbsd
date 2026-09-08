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
 * Free-space indexes subtract allocated extents and physical superblock
 * stripes from block-group bounds. Stripe exclusions have no extent items
 * and are separate from disk usage. Free, reserved, allocated, and pinned
 * space are distinct; typed reservations account for mixed groups.
 *
 * The commit metadata reserve is excluded from ordinary handles. A second
 * protected reserve covers unlink/rmdir, a minimum truncate/orphan-cleanup
 * batch, or chunk allocation/removal, including system space for chunk-tree
 * COW. These operations can borrow it at ordinary exhaustion; unused space
 * follows delayed work to commit and is replenished at publication. Larger
 * reclaim plans combine it with ordinary space, restoring the protected
 * promise if reservation fails. Partial EOF COW still needs ordinary data
 * and metadata space.
 *
 * Mount needs existing free space to establish these reserves. Failure to
 * replenish them after publication leaves that generation durable and the
 * filesystem read-only. A failed reservation can publish pending work and
 * retry, but concurrent operations may claim the next generation's space
 * first, so large namespace plans can fail under transient pressure.
 *
 * The mapping lock protects the group pointer index. Handles, commit
 * ownership, or the chunk-allocation lock protect borrowed group pointers;
 * publication drains handles before replacing indexes and freeing groups.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/mount.h>

#include <btrfs/btrfs_var.h>

struct btrfs_space_build {
	struct btrfs_fs		*bsb_mount;
	uint64_t		 bsb_bytes_used;
	struct btrfs_free_extent	*bsb_free;
	uint64_t		 bsb_cursor;
};

/*
 * Keep enough metadata for four full-height COW paths which each split at
 * every level, plus a small margin for root and accounting updates.  Normal
 * operations must still reserve their own worst case before mutation.
 */
#define BTRFS_COMMIT_METADATA_BLOCKS	(BTRFS_MAX_LEVEL * 8 + 8)

static int	btrfs_space_add_block_group(
		    const struct btrfs_block_group_record *, void *);
static int	btrfs_space_add_extent(const struct btrfs_extent_record *,
		    void *);
static int	btrfs_space_validate_free(
		    const struct btrfs_free_space_record *, void *);
static int	btrfs_space_add_free(struct btrfs_block_group *, uint64_t,
		    uint64_t);
static int	btrfs_space_exclude_supers(struct btrfs_fs *,
		    struct btrfs_block_group *, const struct btrfs_chunk_map *);
static int	btrfs_space_check_type(uint64_t);
static int	btrfs_space_group_matches(const struct btrfs_block_group *,
		    uint64_t, int);
static int	btrfs_space_reserve_type(struct btrfs_fs *,
		    struct btrfs_reserved_space_list *, uint64_t, uint64_t);
static void	btrfs_space_release_list(
		    struct btrfs_reserved_space_list *);
static int	btrfs_space_handle_error(struct btrfs_trans_handle *);
static int	btrfs_space_ranges_overlap(uint64_t, uint64_t, uint64_t,
		    uint64_t);
static int	btrfs_space_range_is_free(const struct btrfs_block_group *,
		    uint64_t, uint64_t);
static void	btrfs_space_destroy_groups(struct btrfs_fs *, int);
static unsigned int
		btrfs_space_insert_free_locked(struct btrfs_block_group *,
		    struct btrfs_free_extent *,
		    struct btrfs_free_extent **);
static void	btrfs_space_check_group(struct btrfs_block_group *);
static void	btrfs_space_check_commit_reserve(
		    struct btrfs_transaction *);
static struct btrfs_block_group *
		btrfs_space_find_group(struct btrfs_fs *, uint64_t,
		    uint64_t);

static unsigned int
btrfs_space_group_count(struct btrfs_fs *bmp)
{
	unsigned int count;

	rw_enter_read(&bmp->bm_mapping_lock);
	count = bmp->bm_nblock_groups;
	rw_exit_read(&bmp->bm_mapping_lock);
	return (count);
}

static struct btrfs_block_group *
btrfs_space_group_at(struct btrfs_fs *bmp, unsigned int index)
{
	struct btrfs_block_group *group;

	/*
	 * Callers hold a transaction handle, own commit, or serialize chunk
	 * changes with bm_chunk_alloc_lock. Publication drains handles before
	 * replacing indexes and freeing removed groups. Statfs instead holds
	 * bm_mapping_lock throughout its traversal.
	 */
	rw_enter_read(&bmp->bm_mapping_lock);
	KASSERT(index < bmp->bm_nblock_groups);
	group = bmp->bm_block_groups[index];
	rw_exit_read(&bmp->bm_mapping_lock);
	return (group);
}

static struct btrfs_block_group *
btrfs_space_find_group(struct btrfs_fs *bmp, uint64_t bytenr,
    uint64_t length)
{
	struct btrfs_block_group *group;
	uint64_t delta;
	unsigned int i, count;

	if (length == 0 || bytenr > UINT64_MAX - length)
		return (NULL);
	count = btrfs_space_group_count(bmp);
	for (i = 0; i < count; i++) {
		group = btrfs_space_group_at(bmp, i);
		if (group->bbg_length == 0)
			continue;
		if (bytenr < group->bbg_bytenr)
			break;
		delta = bytenr - group->bbg_bytenr;
		if (delta <= group->bbg_length &&
		    length <= group->bbg_length - delta)
			return (group);
	}
	return (NULL);
}

static int
btrfs_space_add_free(struct btrfs_block_group *group, uint64_t bytenr,
    uint64_t length)
{
	struct btrfs_free_extent *space;

	if (length == 0)
		return (0);
	if (group->bbg_free_bytes > UINT64_MAX - length)
		return (EINVAL);
	space = malloc(sizeof(*space), M_BTRFS, M_WAITOK | M_ZERO);
	space->bfe_bytenr = bytenr;
	space->bfe_length = length;
	TAILQ_INSERT_TAIL(&group->bbg_free_extents, space, bfe_entry);
	group->bbg_free_bytes += length;
	return (0);
}

static int
btrfs_space_check_type(uint64_t type)
{
	const uint64_t types = BTRFS_BLOCK_GROUP_DATA |
	    BTRFS_BLOCK_GROUP_METADATA | BTRFS_BLOCK_GROUP_SYSTEM;

	return ((type & types) == type && type != 0 &&
	    (type & (type - 1)) == 0);
}

/*
 * Superblock stripes have no extent items.  Reverse-map each physical mirror
 * into the chunk and remove the entire 64 KiB stripe from allocatable gaps.
 * Merge the exclusions first: different DUP mirrors can map to the same
 * logical range.  This runs while constructing the mount's free-space index.
 */
static int
btrfs_space_exclude_supers(struct btrfs_fs *bmp,
    struct btrfs_block_group *group, const struct btrfs_chunk_map *chunk)
{
	struct {
		uint64_t start, end;
	} ranges[BTRFS_MAX_MIRRORS * BTRFS_SUPER_MIRROR_MAX + 1], range;
	struct btrfs_free_extent *space, *next, *suffix;
	uint64_t start, end, cut_start, cut_end, removed, space_end;
	unsigned int i, j, mirror, count = 0, merged = 0;

	if (group->bbg_bytenr < superblock_addrs[0]) {
		ranges[count].start = group->bbg_bytenr;
		ranges[count++].end = MIN(superblock_addrs[0],
		    group->bbg_bytenr + group->bbg_length);
	}
	for (mirror = 0; mirror < chunk->nmirrors; mirror++) {
		for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
			if (superblock_addrs[i] < chunk->physical[mirror] ||
			    superblock_addrs[i] - chunk->physical[mirror] >=
			    chunk->length)
				continue;
			start = chunk->logical + superblock_addrs[i] -
			    chunk->physical[mirror];
			ranges[count].start = start;
			ranges[count++].end = start + MIN(65536,
			    group->bbg_bytenr + group->bbg_length - start);
		}
	}
	for (i = 1; i < count; i++) {
		range = ranges[i];
		for (j = i; j > 0 && ranges[j - 1].start > range.start; j--)
			ranges[j] = ranges[j - 1];
		ranges[j] = range;
	}
	for (i = 0; i < count; i++) {
		if (merged != 0 && ranges[i].start <= ranges[merged - 1].end)
			ranges[merged - 1].end =
			    MAX(ranges[merged - 1].end, ranges[i].end);
		else
			ranges[merged++] = ranges[i];
	}
	for (i = 0; i < merged; i++) {
		start = ranges[i].start;
		end = ranges[i].end;
		removed = 0;
		TAILQ_FOREACH_SAFE(space, &group->bbg_free_extents, bfe_entry,
		    next) {
			space_end = space->bfe_bytenr + space->bfe_length;
			if (space_end <= start)
				continue;
			if (space->bfe_bytenr >= end)
				break;
			cut_start = MAX(start, space->bfe_bytenr);
			cut_end = MIN(end, space_end);
			if (space->bfe_bytenr < cut_start && cut_end < space_end) {
				suffix = malloc(sizeof(*suffix), M_BTRFS,
				    M_WAITOK | M_ZERO);
				suffix->bfe_bytenr = cut_end;
				suffix->bfe_length = space_end - cut_end;
				TAILQ_INSERT_AFTER(&group->bbg_free_extents,
				    space, suffix, bfe_entry);
				space->bfe_length = cut_start - space->bfe_bytenr;
			} else if (space->bfe_bytenr < cut_start) {
				space->bfe_length = cut_start - space->bfe_bytenr;
			} else if (cut_end < space_end) {
				space->bfe_bytenr = cut_end;
				space->bfe_length = space_end - cut_end;
			} else {
				TAILQ_REMOVE(&group->bbg_free_extents, space,
				    bfe_entry);
				free(space, M_BTRFS, sizeof(*space));
			}
			removed += cut_end - cut_start;
		}
		group->bbg_free_bytes -= removed;
		group->bbg_excluded_bytes += removed;
		if (removed != end - start && !bmp->bm_readonly) {
			printf("btrfs: allocated extent overlaps superblock"
			    " stripe at %llu\n", (unsigned long long)start);
			return (EINVAL);
		}
	}
	return (0);
}

static int
btrfs_space_group_matches(const struct btrfs_block_group *group,
    uint64_t type, int mixed)
{
	const uint64_t types = BTRFS_BLOCK_GROUP_DATA |
	    BTRFS_BLOCK_GROUP_METADATA | BTRFS_BLOCK_GROUP_SYSTEM;
	uint64_t group_type = group->bbg_flags & types;

	if ((group_type & type) == 0)
		return (0);
	return (mixed ? group_type != type : group_type == type);
}

static int
btrfs_space_handle_error(struct btrfs_trans_handle *handle)
{
	struct btrfs_transaction *trans = handle->bth_transaction;
	struct btrfs_fs *bmp = trans->bt_mount;
	int error;

	mtx_enter(&bmp->bm_trans_mtx);
	error = trans->bt_error;
	if (error == 0 && trans->bt_state == BTRFS_TRANS_ABORTED)
		error = EROFS;
	mtx_leave(&bmp->bm_trans_mtx);
	return (error);
}

static int
btrfs_space_ranges_overlap(uint64_t bytenr1, uint64_t length1,
    uint64_t bytenr2, uint64_t length2)
{
	return (bytenr1 < bytenr2 + length2 &&
	    bytenr2 < bytenr1 + length1);
}

static int
btrfs_space_range_is_free(const struct btrfs_block_group *group,
    uint64_t bytenr, uint64_t length)
{
	const struct btrfs_free_extent *space;

	TAILQ_FOREACH(space, &group->bbg_free_extents, bfe_entry) {
		if (space->bfe_bytenr >= bytenr + length)
			break;
		if (btrfs_space_ranges_overlap(bytenr, length,
		    space->bfe_bytenr, space->bfe_length))
			return (1);
	}
	return (0);
}

/*
 * Insert a returned extent in address order and merge its neighbours.
 * Removed list nodes are returned to the caller because free() may not be
 * called while the block-group mutex is held.
 */
static unsigned int
btrfs_space_insert_free_locked(struct btrfs_block_group *group,
    struct btrfs_free_extent *space, struct btrfs_free_extent **garbage)
{
	struct btrfs_free_extent *current, *previous = NULL, *merged;
	unsigned int ngarbage = 0;
	uint64_t end;

	MUTEX_ASSERT_LOCKED(&group->bbg_lock);
	TAILQ_FOREACH(current, &group->bbg_free_extents, bfe_entry) {
		if (current->bfe_bytenr >= space->bfe_bytenr)
			break;
		previous = current;
	}
	if (previous != NULL) {
		end = previous->bfe_bytenr + previous->bfe_length;
		KASSERT(end <= space->bfe_bytenr);
	}
	if (current != NULL)
		KASSERT(space->bfe_bytenr + space->bfe_length <=
		    current->bfe_bytenr);

	if (previous != NULL &&
	    previous->bfe_bytenr + previous->bfe_length ==
	    space->bfe_bytenr) {
		previous->bfe_length += space->bfe_length;
		merged = previous;
		garbage[ngarbage++] = space;
	} else {
		if (current != NULL)
			TAILQ_INSERT_BEFORE(current, space, bfe_entry);
		else
			TAILQ_INSERT_TAIL(&group->bbg_free_extents, space,
			    bfe_entry);
		merged = space;
	}
	if (current != NULL && current != merged &&
	    merged->bfe_bytenr + merged->bfe_length ==
	    current->bfe_bytenr) {
		merged->bfe_length += current->bfe_length;
		TAILQ_REMOVE(&group->bbg_free_extents, current, bfe_entry);
		garbage[ngarbage++] = current;
	}
	return (ngarbage);
}

static void
btrfs_space_check_group(struct btrfs_block_group *group)
{
#ifdef DIAGNOSTIC
	struct btrfs_free_extent *space;
	uint64_t end = group->bbg_bytenr, list_bytes = 0;

	MUTEX_ASSERT_LOCKED(&group->bbg_lock);
	TAILQ_FOREACH(space, &group->bbg_free_extents, bfe_entry) {
		KASSERT(space->bfe_length != 0);
		KASSERT(space->bfe_bytenr >= end);
		KASSERT(space->bfe_bytenr >= group->bbg_bytenr);
		KASSERT(space->bfe_bytenr - group->bbg_bytenr <=
		    group->bbg_length);
		KASSERT(space->bfe_length <= group->bbg_length -
		    (space->bfe_bytenr - group->bbg_bytenr));
		KASSERT(list_bytes <= UINT64_MAX - space->bfe_length);
		list_bytes += space->bfe_length;
		end = space->bfe_bytenr + space->bfe_length;
	}
	KASSERT(group->bbg_free_bytes <= UINT64_MAX -
	    group->bbg_reserved_bytes);
	KASSERT(list_bytes == group->bbg_free_bytes +
	    group->bbg_reserved_bytes);
	KASSERT(group->bbg_disk_used <= group->bbg_length);
	KASSERT(group->bbg_allocated_bytes <=
	    group->bbg_length - group->bbg_disk_used);
	KASSERT(group->bbg_excluded_bytes <= group->bbg_length -
	    group->bbg_disk_used - group->bbg_allocated_bytes);
	KASSERT(list_bytes == group->bbg_length -
	    group->bbg_disk_used - group->bbg_allocated_bytes -
	    group->bbg_excluded_bytes);
	KASSERT(group->bbg_pinned_bytes <= group->bbg_disk_used);
#else
	(void)group;
#endif
}

static void
btrfs_space_check_commit_reserve(struct btrfs_transaction *trans)
{
#ifdef DIAGNOSTIC
	struct btrfs_reserved_space *reservation;
	uint64_t bytes = 0;

	TAILQ_FOREACH(reservation, &trans->bt_commit_reservations,
	    brs_entry) {
		KASSERT(reservation->brs_type == BTRFS_BLOCK_GROUP_METADATA ||
		    reservation->brs_type == BTRFS_BLOCK_GROUP_SYSTEM);
		KASSERT(bytes <= UINT64_MAX - reservation->brs_bytes);
		bytes += reservation->brs_bytes;
	}
	KASSERT(bytes == trans->bt_commit_reserved_bytes);
	KASSERT(bytes <= trans->bt_commit_reserve_target);
	if (trans->bt_commit_reserve_target == 0)
		KASSERT(TAILQ_EMPTY(&trans->bt_commit_reservations));
#else
	(void)trans;
#endif
}

static int
btrfs_space_add_block_group(const struct btrfs_block_group_record *record,
    void *arg)
{
	struct btrfs_space_build *build = arg;
	struct btrfs_fs *bmp = build->bsb_mount;
	struct btrfs_block_group *group;
	const struct btrfs_chunk_map *chunk;
	unsigned int i;

	for (i = 0; i < bmp->bm_nchunks; i++) {
		chunk = &bmp->bm_chunks[i];
		if (record->bbg_bytenr == chunk->logical &&
		    record->bbg_length == chunk->length)
			break;
	}
	if (i == bmp->bm_nchunks)
		return (EINVAL);
	group = bmp->bm_block_groups[i];
	if (group->bbg_length != 0)
		return (EINVAL);

	group->bbg_bytenr = record->bbg_bytenr;
	group->bbg_length = record->bbg_length;
	group->bbg_disk_used = record->bbg_used;
	group->bbg_flags = record->bbg_flags;
	group->bbg_build_cursor = record->bbg_bytenr;
	return (0);
}

static int
btrfs_space_validate_free(const struct btrfs_free_space_record *record,
    void *arg)
{
	struct btrfs_space_build *build = arg;
	struct btrfs_block_group *group;
	struct btrfs_free_extent *space;
	uint64_t bit, bytenr;
	uint32_t sectorsize;
	int expected;

	if (record->bfs_type == BTRFS_FREE_SPACE_RECORD_INFO) {
		if (build->bsb_free != NULL)
			return (EINVAL);
		group = btrfs_space_find_group(build->bsb_mount,
		    record->bfs_bytenr, record->bfs_length);
		if (group == NULL)
			return (EINVAL);
		build->bsb_free = TAILQ_FIRST(&group->bbg_free_extents);
		build->bsb_cursor = group->bbg_bytenr;
		return (0);
	}
	if (record->bfs_type == BTRFS_FREE_SPACE_RECORD_BITMAP) {
		if (record->bfs_bytenr != build->bsb_cursor)
			return (EINVAL);
		sectorsize = letoh32(build->bsb_mount->bm_super.sectorsize);
		for (bit = 0; bit < record->bfs_length / sectorsize; bit++) {
			bytenr = record->bfs_bytenr + bit * sectorsize;
			space = build->bsb_free;
			expected = space != NULL && bytenr >= space->bfe_bytenr;
			if (((record->bfs_bitmap[bit / 8] >> (bit & 7)) & 1) !=
			    expected)
				return (EINVAL);
			if (expected && bytenr + sectorsize ==
			    space->bfe_bytenr + space->bfe_length)
				build->bsb_free = TAILQ_NEXT(space, bfe_entry);
		}
		build->bsb_cursor += record->bfs_length;
		return (0);
	}
	space = build->bsb_free;
	if (record->bfs_type != BTRFS_FREE_SPACE_RECORD_EXTENT ||
	    space == NULL || space->bfe_bytenr != record->bfs_bytenr ||
	    space->bfe_length != record->bfs_length)
		return (EINVAL);
	build->bsb_free = TAILQ_NEXT(space, bfe_entry);
	return (0);
}

static int
btrfs_space_add_extent(const struct btrfs_extent_record *record, void *arg)
{
	struct btrfs_space_build *build = arg;
	struct btrfs_block_group *group;
	uint64_t end, gap;
	int error;

	group = btrfs_space_find_group(build->bsb_mount, record->ber_bytenr,
	    record->ber_length);
	if (group == NULL || record->ber_bytenr < group->bbg_build_cursor)
		return (EINVAL);
	end = record->ber_bytenr + record->ber_length;
	gap = record->ber_bytenr - group->bbg_build_cursor;
	error = btrfs_space_add_free(group, group->bbg_build_cursor, gap);
	if (error != 0)
		return (error);
	if (group->bbg_build_used > UINT64_MAX - record->ber_length ||
	    build->bsb_bytes_used > UINT64_MAX - record->ber_length)
		return (EINVAL);
	group->bbg_build_used += record->ber_length;
	build->bsb_bytes_used += record->ber_length;
	group->bbg_build_cursor = end;
	return (0);
}

/*
 * Validate free-space records against the extent tree's complement before
 * excluding superblock stripes from the in-memory index.
 */
int
btrfs_space_init(struct btrfs_fs *bmp)
{
	struct btrfs_space_build build;
	struct btrfs_block_group *group;
	uint64_t end;
	unsigned int i;
	int error;

	KASSERT(bmp->bm_block_groups == NULL);
	KASSERT(bmp->bm_nblock_groups == 0);
	if (bmp->bm_nchunks == 0)
		return (EINVAL);

	bmp->bm_block_groups = mallocarray(bmp->bm_nchunks,
	    sizeof(*bmp->bm_block_groups), M_BTRFS, M_WAITOK | M_ZERO);
	bmp->bm_nblock_groups = bmp->bm_nchunks;
	bmp->bm_chunk_logical_end = bmp->bm_chunks[bmp->bm_nchunks - 1].logical +
	    bmp->bm_chunks[bmp->bm_nchunks - 1].length;
	for (i = 0; i < bmp->bm_nblock_groups; i++) {
		group = malloc(sizeof(*group), M_BTRFS, M_WAITOK | M_ZERO);
		bmp->bm_block_groups[i] = group;
		TAILQ_INIT(&group->bbg_free_extents);
		mtx_init(&group->bbg_lock, IPL_NONE);
	}
	memset(&build, 0, sizeof(build));
	build.bsb_mount = bmp;

	error = btrfs_iterate_block_groups(bmp,
	    btrfs_space_add_block_group, &build);
	if (error != 0) {
		printf("btrfs: cannot load block groups: error %d\n", error);
		goto fail;
	}
	error = btrfs_iterate_extent_items(bmp, btrfs_space_add_extent,
	    NULL, &build);
	if (error != 0) {
		printf("btrfs: cannot load allocated extents: error %d\n",
		    error);
		goto fail;
	}

	for (i = 0; i < bmp->bm_nblock_groups; i++) {
		group = bmp->bm_block_groups[i];
		if (group->bbg_length == 0 ||
		    group->bbg_build_used != group->bbg_disk_used) {
			printf("btrfs: block group %llu usage mismatch: "
			    "extent tree %llu, block group %llu\n",
			    (unsigned long long)group->bbg_bytenr,
			    (unsigned long long)group->bbg_build_used,
			    (unsigned long long)group->bbg_disk_used);
			error = EINVAL;
			goto fail;
		}
		end = group->bbg_bytenr + group->bbg_length;
		error = btrfs_space_add_free(group, group->bbg_build_cursor,
		    end - group->bbg_build_cursor);
		if (error != 0)
			goto fail;
	}
	/*
	 * The on-disk free-space tree describes the complement of extent
	 * items, including superblock stripes. Validate before excluding
	 * those stripes from our allocator's index.
	 */
	if (!bmp->bm_readonly && (letoh64(bmp->bm_super.compat_ro_flags) &
	    BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE)) {
		error = btrfs_iterate_free_space(bmp, btrfs_space_validate_free,
		    &build);
		if (error == 0 && build.bsb_free != NULL)
			error = EINVAL;
		if (error != 0)
			goto fail;
	}
	for (i = 0; i < bmp->bm_nblock_groups; i++) {
		group = bmp->bm_block_groups[i];
		error = btrfs_space_exclude_supers(bmp, group,
		    &bmp->bm_chunks[i]);
		if (error != 0)
			goto fail;
		if (group->bbg_free_bytes !=
		    group->bbg_length - group->bbg_disk_used -
		    group->bbg_excluded_bytes) {
			printf("btrfs: block group %llu free-space mismatch\n",
			    (unsigned long long)group->bbg_bytenr);
			error = EINVAL;
			goto fail;
		}
		group->bbg_build_cursor = 0;
		group->bbg_build_used = 0;
		mtx_enter(&group->bbg_lock);
		btrfs_space_check_group(group);
		mtx_leave(&group->bbg_lock);
	}
	if (build.bsb_bytes_used != letoh64(bmp->bm_super.bytes_used)) {
		printf("btrfs: filesystem usage mismatch: extent tree %llu, "
		    "superblock %llu\n",
		    (unsigned long long)build.bsb_bytes_used,
		    (unsigned long long)letoh64(bmp->bm_super.bytes_used));
		error = EINVAL;
		goto fail;
	}
	return (0);

fail:
	btrfs_space_destroy_groups(bmp, 0);
	return (error);
}

static void
btrfs_space_destroy_groups(struct btrfs_fs *bmp, int initialized)
{
	struct btrfs_block_group *group;
	struct btrfs_free_extent *space;
	unsigned int i;

	if (bmp->bm_block_groups == NULL)
		return;
	for (i = 0; i < bmp->bm_nblock_groups; i++) {
		group = bmp->bm_block_groups[i];
		mtx_enter(&group->bbg_lock);
		if (initialized) {
			btrfs_space_check_group(group);
			KASSERT(group->bbg_reserved_bytes == 0);
			KASSERT(group->bbg_allocated_bytes == 0);
			KASSERT(group->bbg_pinned_bytes == 0);
		}
		mtx_leave(&group->bbg_lock);
		while ((space = TAILQ_FIRST(&group->bbg_free_extents)) != NULL) {
			TAILQ_REMOVE(&group->bbg_free_extents, space, bfe_entry);
			free(space, M_BTRFS, sizeof(*space));
		}
		free(group, M_BTRFS, sizeof(*group));
	}
	free(bmp->bm_block_groups, M_BTRFS,
	    bmp->bm_nblock_groups * sizeof(*bmp->bm_block_groups));
	bmp->bm_block_groups = NULL;
	bmp->bm_nblock_groups = 0;
}

void
btrfs_space_destroy(struct btrfs_fs *bmp)
{
	btrfs_space_destroy_groups(bmp, 1);
}

/*
 * Log blocks have no extent items. Keep them out of the allocator without
 * changing on-disk block-group/free-space accounting. During recovery, new
 * data allocations instead enter the transaction's ordinary allocation list;
 * replay will supply their references. All callers own the closed transaction
 * or the as-yet unpublished mount.
 */
int
btrfs_space_log_claim(struct btrfs_fs *bmp, uint64_t bytenr,
    uint64_t length, int data)
{
	struct btrfs_transaction *trans = bmp->bm_transaction;
	struct btrfs_block_group *group;
	struct btrfs_free_extent *space, *split = NULL;
	struct btrfs_trans_extent *extent;
	uint64_t end, left, right;
	uint32_t sector = letoh32(bmp->bm_super.sectorsize);

	if (length == 0 || bytenr > UINT64_MAX - length ||
	    ((bytenr | length) & (sector - 1)) != 0)
		return (EINVAL);
	group = btrfs_space_find_group(bmp, bytenr, length);
	if (group == NULL ||
	    !(group->bbg_flags & (data ? BTRFS_BLOCK_GROUP_DATA :
	    BTRFS_BLOCK_GROUP_METADATA)))
		return (EINVAL);
	if (data) {
		TAILQ_FOREACH(extent, &trans->bt_allocated_extents, bte_entry)
			if (extent->bte_bytenr == bytenr &&
			    extent->bte_length == length &&
			    extent->bte_type == BTRFS_BLOCK_GROUP_DATA)
				return (0);
	}
	end = bytenr + length;
	extent = malloc(sizeof(*extent), M_BTRFS, M_WAITOK | M_ZERO);
	split = malloc(sizeof(*split), M_BTRFS, M_WAITOK | M_ZERO);
	mtx_enter(&group->bbg_lock);
	TAILQ_FOREACH(space, &group->bbg_free_extents, bfe_entry)
		if (space->bfe_bytenr <= bytenr &&
		    space->bfe_bytenr + space->bfe_length >= end)
			break;
	if (space == NULL || group->bbg_free_bytes < length) {
		mtx_leave(&group->bbg_lock);
		free(extent, M_BTRFS, sizeof(*extent));
		free(split, M_BTRFS, sizeof(*split));
		return (space == NULL ? EINVAL : ENOSPC);
	}
	left = bytenr - space->bfe_bytenr;
	right = space->bfe_bytenr + space->bfe_length - end;
	if (left != 0 && right != 0) {
		split->bfe_bytenr = end;
		split->bfe_length = right;
		TAILQ_INSERT_AFTER(&group->bbg_free_extents, space, split,
		    bfe_entry);
		split = NULL;
		space->bfe_length = left;
	} else if (left != 0)
		space->bfe_length = left;
	else if (right != 0) {
		space->bfe_bytenr = end;
		space->bfe_length = right;
	} else {
		TAILQ_REMOVE(&group->bbg_free_extents, space, bfe_entry);
		free(space, M_BTRFS, sizeof(*space));
	}
	group->bbg_free_bytes -= length;
	extent->bte_group = group;
	extent->bte_bytenr = bytenr;
	extent->bte_length = length;
	extent->bte_type = data ? BTRFS_BLOCK_GROUP_DATA :
	    BTRFS_BLOCK_GROUP_METADATA;
	if (data) {
		group->bbg_allocated_bytes += length;
		trans->bt_allocated_bytes += length;
		trans->bt_space_seq++;
		TAILQ_INSERT_TAIL(&trans->bt_allocated_extents, extent,
		    bte_entry);
	} else {
		group->bbg_excluded_bytes += length;
		TAILQ_INSERT_TAIL(&bmp->bm_log_extents, extent, bte_entry);
	}
	btrfs_space_check_group(group);
	mtx_leave(&group->bbg_lock);
	free(split, M_BTRFS, sizeof(*split));
	return (0);
}

int
btrfs_space_log_alloc(struct btrfs_fs *bmp, uint64_t *bytenr)
{
	struct btrfs_block_group *group;
	struct btrfs_free_extent *space;
	uint64_t size = letoh32(bmp->bm_super.nodesize), found = 0;
	unsigned int i;

	for (i = 0; i < bmp->bm_nblock_groups && found == 0; i++) {
		group = bmp->bm_block_groups[i];
		if (!(group->bbg_flags & BTRFS_BLOCK_GROUP_METADATA) ||
		    group->bbg_removing)
			continue;
		mtx_enter(&group->bbg_lock);
		if (group->bbg_free_bytes >= size) {
			TAILQ_FOREACH(space, &group->bbg_free_extents, bfe_entry)
				if (space->bfe_length >= size) {
					found = space->bfe_bytenr;
					break;
				}
		}
		mtx_leave(&group->bbg_lock);
	}
	if (found == 0)
		return (ENOSPC);
	*bytenr = found;
	return (btrfs_space_log_claim(bmp, found, size, 0));
}

void
btrfs_space_log_release(struct btrfs_fs *bmp)
{
	struct btrfs_trans_extent *extent;
	struct btrfs_block_group *group;
	struct btrfs_free_extent *space, *garbage[2];
	unsigned int i, count;

	while ((extent = TAILQ_FIRST(&bmp->bm_log_extents)) != NULL) {
		TAILQ_REMOVE(&bmp->bm_log_extents, extent, bte_entry);
		group = extent->bte_group;
		space = malloc(sizeof(*space), M_BTRFS, M_WAITOK | M_ZERO);
		space->bfe_bytenr = extent->bte_bytenr;
		space->bfe_length = extent->bte_length;
		mtx_enter(&group->bbg_lock);
		group->bbg_excluded_bytes -= extent->bte_length;
		group->bbg_free_bytes += extent->bte_length;
		count = btrfs_space_insert_free_locked(group, space, garbage);
		btrfs_space_check_group(group);
		mtx_leave(&group->bbg_lock);
		for (i = 0; i < count; i++)
			free(garbage[i], M_BTRFS, sizeof(*garbage[i]));
		free(extent, M_BTRFS, sizeof(*extent));
	}
}

/*
 * Report logical capacity of allocated groups, counting DUP once. Free blocks
 * include reservations but exclude pending allocations, pins and superblock
 * stripes. Available blocks include only unreserved data-capable space.
 * Capacity changes as chunks are allocated or returned; metadata exhaustion
 * and fragmentation can still limit writes. Hold the mapping lock throughout
 * traversal so chunk publication cannot retire a group underneath us.
 */
int
btrfs_space_statfs(struct btrfs_fs *bmp, struct statfs *sbp)
{
	struct btrfs_block_group *group;
	uint64_t total = 0, free = 0, available = 0;
	uint64_t length, group_free, group_available, sectorsize;
	unsigned int i, count;

	/*
	 * Only existing chunks can be allocated. Report logical capacity,
	 * counting DUP space once, and retain superblock stripes as overhead.
	 * Reservations are free storage but unavailable to new operations;
	 * pinned extents remain used until durable publication.
	 *
	 * Keep removed group objects alive throughout this read-only traversal.
	 */
	rw_enter_read(&bmp->bm_mapping_lock);
	count = bmp->bm_nblock_groups;
	for (i = 0; i < count; i++) {
		group = bmp->bm_block_groups[i];
		mtx_enter(&group->bbg_lock);
		length = group->bbg_length;
		group_free = group->bbg_free_bytes + group->bbg_reserved_bytes;
		group_available = !group->bbg_removing &&
		    (group->bbg_flags & BTRFS_BLOCK_GROUP_DATA) ?
		    group->bbg_free_bytes : 0;
		mtx_leave(&group->bbg_lock);
		if (length > UINT64_MAX - total ||
		    group_free > UINT64_MAX - free ||
		    group_available > UINT64_MAX - available) {
			rw_exit_read(&bmp->bm_mapping_lock);
			return (EOVERFLOW);
		}
		total += length;
		free += group_free;
		available += group_available;
	}
	rw_exit_read(&bmp->bm_mapping_lock);
	sectorsize = letoh32(bmp->bm_super.sectorsize);
	sbp->f_blocks = total / sectorsize;
	sbp->f_bfree = free / sectorsize;
	sbp->f_bavail = available / sectorsize;
	return (0);
}

static int
btrfs_space_reserve_type(struct btrfs_fs *bmp,
    struct btrfs_reserved_space_list *reservations, uint64_t type,
    uint64_t bytes)
{
	struct btrfs_reserved_space *reservation = NULL;
	struct btrfs_block_group *group;
	uint64_t take;
	unsigned int i, count;
	int mixed;

	count = btrfs_space_group_count(bmp);
	for (mixed = 0; mixed <= 1 && bytes != 0; mixed++) {
		for (i = 0; i < count && bytes != 0; i++) {
			group = btrfs_space_group_at(bmp, i);
			if (!btrfs_space_group_matches(group, type, mixed))
				continue;
			if (reservation == NULL)
				reservation = malloc(sizeof(*reservation),
				    M_BTRFS, M_WAITOK | M_ZERO);
			mtx_enter(&group->bbg_lock);
			take = group->bbg_removing ? 0 :
			    MIN(bytes, group->bbg_free_bytes);
			if (take != 0) {
				group->bbg_free_bytes -= take;
				KASSERT(group->bbg_reserved_bytes <=
				    UINT64_MAX - take);
				group->bbg_reserved_bytes += take;
				reservation->brs_group = group;
				reservation->brs_bytes = take;
				reservation->brs_type = type;
				TAILQ_INSERT_TAIL(reservations,
				    reservation, brs_entry);
				reservation = NULL;
				bytes -= take;
			}
			btrfs_space_check_group(group);
			mtx_leave(&group->bbg_lock);
		}
	}
	if (reservation != NULL)
		free(reservation, M_BTRFS, sizeof(*reservation));
	return (bytes == 0 ? 0 : ENOSPC);
}

static void
btrfs_space_release_list(struct btrfs_reserved_space_list *reservations)
{
	struct btrfs_reserved_space *reservation;
	struct btrfs_block_group *group;

	while ((reservation = TAILQ_FIRST(reservations)) != NULL) {
		TAILQ_REMOVE(reservations, reservation, brs_entry);
		group = reservation->brs_group;
		mtx_enter(&group->bbg_lock);
		KASSERT(group->bbg_reserved_bytes >=
		    reservation->brs_bytes);
		KASSERT(group->bbg_free_bytes <= UINT64_MAX -
		    reservation->brs_bytes);
		group->bbg_reserved_bytes -= reservation->brs_bytes;
		group->bbg_free_bytes += reservation->brs_bytes;
		btrfs_space_check_group(group);
		mtx_leave(&group->bbg_lock);
		free(reservation, M_BTRFS, sizeof(*reservation));
	}
}

int
btrfs_space_reserve(struct btrfs_trans_handle *handle,
    const struct btrfs_trans_reservation *request)
{
	struct btrfs_fs *bmp = handle->bth_transaction->bt_mount;
	struct btrfs_transaction *trans = handle->bth_transaction;
	struct btrfs_reserved_space *reservation;
	struct btrfs_reserved_space_list borrowed;
	uint64_t metadata, reclaim_metadata = 0, reclaim_system = 0;
	uint32_t sectorsize;
	int error;

	if (handle->bth_commit)
		return (EINVAL);
	if (request == NULL)
		return (0);
	sectorsize = letoh32(bmp->bm_super.sectorsize);
	if ((request->btr_data & (sectorsize - 1)) != 0 ||
	    (request->btr_metadata & (sectorsize - 1)) != 0 ||
	    (request->btr_system & (sectorsize - 1)) != 0)
		return (EINVAL);
	error = btrfs_space_handle_error(handle);
	if (error != 0)
		return (error);

	/*
	 * Protect system and metadata promises before data can consume mixed
	 * block groups.
	 */
	metadata = request->btr_metadata;
	/* Free-space COW paths accompany the delayed extent-tree work. */
	if (letoh64(bmp->bm_super.compat_ro_flags) &
	    BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE) {
		if (metadata > UINT64_MAX / 2)
			return (EOVERFLOW);
		metadata *= 2;
	}
	error = btrfs_space_reserve_type(bmp, &handle->bth_reservations,
	    BTRFS_BLOCK_GROUP_SYSTEM, request->btr_system);
	handle->bth_failed_type = BTRFS_BLOCK_GROUP_SYSTEM;
	if (error == 0) {
		handle->bth_failed_type = BTRFS_BLOCK_GROUP_METADATA;
		error = btrfs_space_reserve_type(bmp,
		    &handle->bth_reservations, BTRFS_BLOCK_GROUP_METADATA,
		    metadata);
	}
	if (error == 0) {
		handle->bth_failed_type = BTRFS_BLOCK_GROUP_DATA;
		error = btrfs_space_reserve_type(bmp,
		    &handle->bth_reservations, BTRFS_BLOCK_GROUP_DATA,
		    request->btr_data);
	}
	if (error == 0)
		handle->bth_failed_type = 0;
	if (error != 0)
		btrfs_space_release(handle);
	if (error == ENOSPC && request->btr_reclaim &&
	    request->btr_data == 0) {
		/*
		 * Ordinary writers cannot spend the protected reclaim promise.
		 * Combine the protected promise with ordinary space for larger
		 * reclaim plans. Keep it separate until the remainder succeeds,
		 * so failure restores the promise instead of exposing it to
		 * ordinary writers.
		 */
		TAILQ_INIT(&borrowed);
		mtx_enter(&trans->bt_lock);
		TAILQ_FOREACH(reservation, &trans->bt_reclaim_reservations,
		    brs_entry) {
			if (reservation->brs_type == BTRFS_BLOCK_GROUP_METADATA)
				reclaim_metadata += reservation->brs_bytes;
			if (reservation->brs_type == BTRFS_BLOCK_GROUP_SYSTEM)
				reclaim_system += reservation->brs_bytes;
		}
		TAILQ_CONCAT(&borrowed, &trans->bt_reclaim_reservations,
		    brs_entry);
		mtx_leave(&trans->bt_lock);
		if (!TAILQ_EMPTY(&borrowed)) {
			handle->bth_failed_type = BTRFS_BLOCK_GROUP_SYSTEM;
			error = btrfs_space_reserve_type(bmp,
			    &handle->bth_reservations, BTRFS_BLOCK_GROUP_SYSTEM,
			    request->btr_system > reclaim_system ?
			    request->btr_system - reclaim_system : 0);
			if (error == 0) {
				handle->bth_failed_type = BTRFS_BLOCK_GROUP_METADATA;
				error = btrfs_space_reserve_type(bmp,
				    &handle->bth_reservations,
				    BTRFS_BLOCK_GROUP_METADATA,
				    metadata > reclaim_metadata ?
				    metadata - reclaim_metadata : 0);
			}
			if (error == 0) {
				TAILQ_CONCAT(&handle->bth_reservations,
				    &borrowed, brs_entry);
				handle->bth_delayed = 1;
				handle->bth_failed_type = 0;
			} else {
				btrfs_space_release(handle);
				mtx_enter(&trans->bt_lock);
				TAILQ_CONCAT(&trans->bt_reclaim_reservations,
				    &borrowed, brs_entry);
				mtx_leave(&trans->bt_lock);
			}
		}
	}
	return (error);
}

int
btrfs_space_reserve_commit(struct btrfs_transaction *trans)
{
	struct btrfs_fs *bmp = trans->bt_mount;
	uint64_t bytes, reclaim;
	uint32_t nodesize;
	int error;

	KASSERT(TAILQ_EMPTY(&trans->bt_commit_reservations));
	KASSERT(trans->bt_commit_reserve_target == 0);
	KASSERT(trans->bt_commit_reserved_bytes == 0);

	nodesize = letoh32(bmp->bm_super.nodesize);
	bytes = nodesize * BTRFS_COMMIT_METADATA_BLOCKS;
	reclaim = nodesize * BTRFS_RECLAIM_METADATA_BLOCKS;
	if (letoh64(bmp->bm_super.compat_ro_flags) &
	    BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE) {
		bytes *= 2;
		reclaim *= 2;
	}
	error = btrfs_space_reserve_type(bmp,
	    &trans->bt_commit_reservations, BTRFS_BLOCK_GROUP_METADATA,
	    bytes);
	if (error == 0)
		error = btrfs_space_reserve_type(bmp,
		    &trans->bt_reclaim_reservations, BTRFS_BLOCK_GROUP_METADATA,
		    reclaim);
	if (error == 0)
		error = btrfs_space_reserve_type(bmp,
		    &trans->bt_reclaim_reservations, BTRFS_BLOCK_GROUP_SYSTEM,
		    nodesize * BTRFS_CHUNK_SYSTEM_BLOCKS);
	if (error != 0) {
		btrfs_space_release_list(&trans->bt_commit_reservations);
		btrfs_space_release_list(&trans->bt_reclaim_reservations);
		return (error);
	}
	trans->bt_commit_reserve_target = bytes;
	trans->bt_commit_reserved_bytes = bytes;
	btrfs_space_check_commit_reserve(trans);
	return (0);
}

/* Recovery cannot publish partial progress to replenish its reservation. */
int
btrfs_space_replay_reserve(struct btrfs_trans_handle *handle)
{
	struct btrfs_transaction *trans = handle->bth_transaction;
	struct btrfs_reserved_space_list extra;
	struct btrfs_reserved_space *space;
	uint64_t bytes, target;
	int error;

	KASSERT(handle->bth_commit);
	target = (uint64_t)letoh32(trans->bt_mount->bm_super.nodesize) *
	    BTRFS_RECLAIM_METADATA_BLOCKS;
	if (trans->bt_commit_reserved_bytes >= target)
		return (0);
	bytes = target - trans->bt_commit_reserved_bytes;
	TAILQ_INIT(&extra);
	error = btrfs_space_reserve_type(trans->bt_mount, &extra,
	    BTRFS_BLOCK_GROUP_METADATA, bytes);
	if (error != 0) {
		btrfs_space_release_list(&extra);
		return (error);
	}
	while ((space = TAILQ_FIRST(&extra)) != NULL) {
		TAILQ_REMOVE(&extra, space, brs_entry);
		TAILQ_INSERT_TAIL(&trans->bt_commit_reservations, space,
		    brs_entry);
	}
	trans->bt_commit_reserved_bytes += bytes;
	trans->bt_commit_reserve_target += bytes;
	return (0);
}

void
btrfs_space_release(struct btrfs_trans_handle *handle)
{
	btrfs_space_release_list(&handle->bth_reservations);
}

/*
 * The operation's metadata estimate includes its delayed work.  Preserve
 * the unused part until commit has materialized those references, rather
 * than returning it to new writers and relying on the emergency reserve.
 * Coalesce promises by block group so a batched transaction does not retain
 * one reservation object per operation.  No group accounting changes: the
 * bytes stay reserved throughout the ownership transfer.
 */
void
btrfs_space_keep_delayed(struct btrfs_trans_handle *handle)
{
	struct btrfs_transaction *trans = handle->bth_transaction;
	struct btrfs_reserved_space *reservation, *next, *existing;

	if (handle->bth_commit || !handle->bth_delayed)
		return;
	TAILQ_FOREACH_SAFE(reservation, &handle->bth_reservations, brs_entry,
	    next) {
		if ((reservation->brs_type != BTRFS_BLOCK_GROUP_METADATA &&
		    reservation->brs_type != BTRFS_BLOCK_GROUP_SYSTEM) ||
		    reservation->brs_bytes == 0)
			continue;
		TAILQ_REMOVE(&handle->bth_reservations, reservation, brs_entry);
		mtx_enter(&trans->bt_lock);
		TAILQ_FOREACH(existing, &trans->bt_commit_reservations,
		    brs_entry) {
			if (existing->brs_group == reservation->brs_group &&
			    existing->brs_type == reservation->brs_type)
				break;
		}
		KASSERT(trans->bt_commit_reserve_target <=
		    UINT64_MAX - reservation->brs_bytes);
		trans->bt_commit_reserve_target += reservation->brs_bytes;
		trans->bt_commit_reserved_bytes += reservation->brs_bytes;
		if (existing != NULL)
			existing->brs_bytes += reservation->brs_bytes;
		else
			TAILQ_INSERT_TAIL(&trans->bt_commit_reservations,
			    reservation, brs_entry);
		btrfs_space_check_commit_reserve(trans);
		mtx_leave(&trans->bt_lock);
		if (existing != NULL)
			free(reservation, M_BTRFS, sizeof(*reservation));
	}
}

int
btrfs_space_alloc(struct btrfs_trans_handle *handle, uint64_t type,
    uint64_t length, uint64_t alignment, uint64_t *bytenrp)
{
	struct btrfs_transaction *trans = handle->bth_transaction;
	struct btrfs_fs *bmp = trans->bt_mount;
	struct btrfs_reserved_space_list *reservations;
	struct btrfs_reserved_space *reservation;
	struct btrfs_trans_extent *allocated;
	struct btrfs_free_extent *space, *split, *removed = NULL;
	struct btrfs_block_group *group;
	uint64_t aligned, delta, end, suffix;
	uint32_t sectorsize;
	int error;

	if (bytenrp == NULL)
		return (EINVAL);
	*bytenrp = 0;
	sectorsize = letoh32(bmp->bm_super.sectorsize);
	if (!btrfs_space_check_type(type) || length == 0 ||
	    alignment == 0 || !powerof2(alignment) ||
	    alignment < sectorsize ||
	    (length & (sectorsize - 1)) != 0)
		return (EINVAL);
	error = btrfs_space_handle_error(handle);
	if (error != 0)
		return (error);

	allocated = malloc(sizeof(*allocated), M_BTRFS, M_WAITOK | M_ZERO);
	split = malloc(sizeof(*split), M_BTRFS, M_WAITOK | M_ZERO);
	if (handle->bth_commit)
		reservations = &trans->bt_commit_reservations;
	else
		reservations = &handle->bth_reservations;
	TAILQ_FOREACH(reservation, reservations, brs_entry) {
		if (reservation->brs_type != type ||
		    reservation->brs_bytes < length)
			continue;
		group = reservation->brs_group;
		mtx_enter(&group->bbg_lock);
		TAILQ_FOREACH(space, &group->bbg_free_extents, bfe_entry) {
			if (space->bfe_bytenr > UINT64_MAX - (alignment - 1))
				continue;
			aligned = (space->bfe_bytenr + alignment - 1) &
			    ~(alignment - 1);
			delta = aligned - space->bfe_bytenr;
			if (delta > space->bfe_length ||
			    length > space->bfe_length - delta)
				continue;
			end = aligned + length;
			suffix = space->bfe_bytenr + space->bfe_length - end;
			if (delta != 0 && suffix != 0) {
				split->bfe_bytenr = end;
				split->bfe_length = suffix;
				TAILQ_INSERT_AFTER(&group->bbg_free_extents,
				    space, split, bfe_entry);
				space->bfe_length = delta;
				split = NULL;
			} else if (delta != 0) {
				space->bfe_length = delta;
			} else if (suffix != 0) {
				space->bfe_bytenr = end;
				space->bfe_length = suffix;
			} else {
				TAILQ_REMOVE(&group->bbg_free_extents, space,
				    bfe_entry);
				removed = space;
			}
			KASSERT(group->bbg_reserved_bytes >= length);
			KASSERT(group->bbg_allocated_bytes <=
			    UINT64_MAX - length);
			group->bbg_reserved_bytes -= length;
			group->bbg_allocated_bytes += length;
			reservation->brs_bytes -= length;

			allocated->bte_group = group;
			allocated->bte_bytenr = aligned;
			allocated->bte_length = length;
			allocated->bte_type = type;
			allocated->bte_commit = handle->bth_commit;
			mtx_enter(&trans->bt_lock);
			KASSERT(trans->bt_allocated_bytes <=
			    UINT64_MAX - length);
			if (handle->bth_commit) {
				KASSERT(trans->bt_commit_reserved_bytes >=
				    length);
				trans->bt_commit_reserved_bytes -= length;
			}
			trans->bt_allocated_bytes += length;
			trans->bt_space_seq++;
			TAILQ_INSERT_TAIL(&trans->bt_allocated_extents,
			    allocated, bte_entry);
			btrfs_space_check_commit_reserve(trans);
			mtx_leave(&trans->bt_lock);
			btrfs_space_check_group(group);
			mtx_leave(&group->bbg_lock);

			if (removed != NULL)
				free(removed, M_BTRFS, sizeof(*removed));
			if (split != NULL)
				free(split, M_BTRFS, sizeof(*split));
			*bytenrp = aligned;
			return (0);
		}
		mtx_leave(&group->bbg_lock);
	}
	free(split, M_BTRFS, sizeof(*split));
	free(allocated, M_BTRFS, sizeof(*allocated));
	return (ENOSPC);
}

int
btrfs_space_cancel_alloc(struct btrfs_trans_handle *handle, uint64_t bytenr,
    uint64_t length)
{
	struct btrfs_transaction *trans = handle->bth_transaction;
	struct btrfs_reserved_space_list *reservations;
	struct btrfs_reserved_space *reservation;
	struct btrfs_trans_extent *extent;
	struct btrfs_free_extent *space, *garbage[2];
	struct btrfs_block_group *group = NULL;
	uint64_t type = 0;
	unsigned int i, ngarbage;
	int error;

	error = btrfs_space_handle_error(handle);
	if (error != 0)
		return (error);

	mtx_enter(&trans->bt_lock);
	TAILQ_FOREACH(extent, &trans->bt_allocated_extents, bte_entry) {
		if (extent->bte_bytenr == bytenr &&
		    extent->bte_length == length) {
			group = extent->bte_group;
			type = extent->bte_type;
			break;
		}
	}
	mtx_leave(&trans->bt_lock);
	if (group == NULL)
		return (ENOENT);

	if (handle->bth_commit)
		reservations = &trans->bt_commit_reservations;
	else
		reservations = &handle->bth_reservations;
	TAILQ_FOREACH(reservation, reservations, brs_entry) {
		if (reservation->brs_group == group &&
		    reservation->brs_type == type)
			break;
	}
	if (reservation == NULL) {
		/* The allocation may belong to an earlier operation's group. */
		reservation = malloc(sizeof(*reservation), M_BTRFS,
		    M_WAITOK | M_ZERO);
		reservation->brs_group = group;
		reservation->brs_type = type;
		TAILQ_INSERT_TAIL(reservations, reservation, brs_entry);
	}

	space = malloc(sizeof(*space), M_BTRFS, M_WAITOK | M_ZERO);
	space->bfe_bytenr = bytenr;
	space->bfe_length = length;

	mtx_enter(&group->bbg_lock);
	mtx_enter(&trans->bt_lock);
	TAILQ_FOREACH(extent, &trans->bt_allocated_extents, bte_entry) {
		if (extent->bte_bytenr == bytenr &&
		    extent->bte_length == length &&
		    extent->bte_group == group)
			break;
	}
	if (extent == NULL) {
		error = ENOENT;
		goto unlock;
	}
	KASSERT(group->bbg_allocated_bytes >= length);
	KASSERT(group->bbg_reserved_bytes <= UINT64_MAX - length);
	KASSERT(reservation->brs_bytes <= UINT64_MAX - length);
	KASSERT(trans->bt_allocated_bytes >= length);
	group->bbg_allocated_bytes -= length;
	group->bbg_reserved_bytes += length;
	reservation->brs_bytes += length;
	trans->bt_allocated_bytes -= length;
	trans->bt_space_seq++;
	if (handle->bth_commit) {
		KASSERT(trans->bt_commit_reserved_bytes <=
		    UINT64_MAX - length);
		trans->bt_commit_reserved_bytes += length;
		/* An ordinary allocation was never charged to this pool. */
		if (!extent->bte_commit)
			trans->bt_commit_reserve_target += length;
	}
	TAILQ_REMOVE(&trans->bt_allocated_extents, extent, bte_entry);
	ngarbage = btrfs_space_insert_free_locked(group, space, garbage);
	space = NULL;
	error = 0;
unlock:
	btrfs_space_check_commit_reserve(trans);
	mtx_leave(&trans->bt_lock);
	btrfs_space_check_group(group);
	mtx_leave(&group->bbg_lock);
	if (space == NULL) {
		for (i = 0; i < ngarbage; i++)
			free(garbage[i], M_BTRFS, sizeof(*garbage[i]));
		free(extent, M_BTRFS, sizeof(*extent));
	} else {
		free(space, M_BTRFS, sizeof(*space));
	}
	return (error);
}

/*
 * A detached COW block may already have an extent item: commit itself can
 * empty an extent-tree leaf after materializing its delayed add.  Keep the
 * allocation unavailable until the matching drop has been materialized.
 */
int
btrfs_space_discard_alloc(struct btrfs_trans_handle *handle, uint64_t bytenr,
    uint64_t length)
{
	struct btrfs_transaction *trans = handle->bth_transaction;
	struct btrfs_trans_extent *extent;
	int error = ENOENT;

	mtx_enter(&trans->bt_lock);
	TAILQ_FOREACH(extent, &trans->bt_allocated_extents, bte_entry) {
		if (extent->bte_bytenr != bytenr ||
		    extent->bte_length != length)
			continue;
		if ((extent->bte_type != BTRFS_BLOCK_GROUP_METADATA &&
		    extent->bte_type != BTRFS_BLOCK_GROUP_SYSTEM) ||
		    extent->bte_discarded) {
			error = EINVAL;
			break;
		}
		extent->bte_discarded = 1;
		error = 0;
		break;
	}
	mtx_leave(&trans->bt_lock);
	return (error);
}

int
btrfs_space_release_discarded(struct btrfs_trans_handle *handle)
{
	struct btrfs_transaction *trans = handle->bth_transaction;
	struct btrfs_trans_extent *extent;
	struct btrfs_root *root;
	struct btrfs_path path = { 0 };
	struct btrfs_key target;
	const struct btrfs_key *key;
	uint64_t bytenr, length;
	int error;

	KASSERT(handle->bth_commit);
	KASSERT(trans->bt_writers == 0);
	/*
	 * Data-reference processing can queue more tree references.  Wait for
	 * the next preparation pass in that case, including canceled adds for
	 * blocks which never acquired an extent item.
	 */
	if (!TAILQ_EMPTY(&trans->bt_delayed_tree_refs) ||
	    !TAILQ_EMPTY(&trans->bt_delayed_data_refs))
		return (0);
	error = btrfs_get_root(trans->bt_mount, BTRFS_EXTENT_TREE_OBJECTID,
	    &root);
	if (error != 0)
		return (error);
	for (;;) {
		TAILQ_FOREACH(extent, &trans->bt_allocated_extents, bte_entry) {
			if (extent->bte_discarded)
				break;
		}
		if (extent == NULL)
			return (0);
		bytenr = extent->bte_bytenr;
		length = extent->bte_length;
		memset(&target, 0, sizeof(target));
		target.objectid = htole64(bytenr);
		error = btrfs_search_lower_bound(root, &target, &path);
		if (error == 0) {
			error = btrfs_path_item(&path, &key, NULL, NULL);
			/* A block group can begin at this same logical address. */
			if (error == 0 && key->objectid == target.objectid &&
			    key->type != BTRFS_BLOCK_GROUP_ITEM_KEY)
				error = EINVAL;
		} else if (error == ENOENT)
			error = 0;
		btrfs_release_path(&path);
		if (error != 0)
			return (error);
		error = btrfs_space_cancel_alloc(handle, bytenr, length);
		if (error != 0)
			return (error);
	}
}

/*
 * Find a neighboring free extent without retaining a path across mutation.
 * An INFO item is a block-group boundary, not a free extent.
 */
static int
btrfs_free_space_neighbor(struct btrfs_root *root,
    struct btrfs_block_group *group, uint64_t bytenr, int previous,
    struct btrfs_key *result, int *found)
{
	struct btrfs_path path = { 0 };
	struct btrfs_key target = { 0 };
	const struct btrfs_key *key;
	uint32_t size;
	uint64_t start, length;
	int error;

	*found = 0;
	target.objectid = htole64(bytenr);
	target.type = BTRFS_FREE_SPACE_EXTENT_KEY;
	target.offset = previous ? htole64(UINT64_MAX) : 0;
	if (previous)
		error = btrfs_search_predecessor(root, &target, &path);
	else
		error = btrfs_search_lower_bound(root, &target, &path);
	if (error == 0) {
		error = btrfs_path_item(&path, &key, NULL, &size);
		if (error == 0 && key->type == BTRFS_FREE_SPACE_EXTENT_KEY) {
			start = letoh64(key->objectid);
			length = letoh64(key->offset);
			if (start >= group->bbg_bytenr &&
			    start < group->bbg_bytenr + group->bbg_length) {
				if (size != 0 || length == 0 ||
				    length > group->bbg_bytenr +
				    group->bbg_length - start)
					error = EINVAL;
				else {
					*result = *key;
					*found = 1;
				}
			}
		}
	} else if (error == ENOENT)
		error = 0;
	btrfs_release_path(&path);
	return (error);
}

static int
btrfs_free_space_bitmap(struct btrfs_root *root, uint64_t bytenr,
    struct btrfs_key *key, uint8_t **payload, uint32_t *size)
{
	struct btrfs_path path = { 0 };
	struct btrfs_key target = { 0 };
	const struct btrfs_key *found;
	const uint8_t *data;
	uint64_t start, length, nbits;
	uint32_t sectorsize = letoh32(root->br_mount->bm_super.sectorsize);
	int error;

	*payload = NULL;
	target.objectid = htole64(bytenr);
	target.type = BTRFS_FREE_SPACE_BITMAP_KEY;
	target.offset = htole64(UINT64_MAX);
	error = btrfs_search_predecessor(root, &target, &path);
	if (error == 0)
		error = btrfs_path_item(&path, &found, &data, size);
	if (error == 0) {
		start = letoh64(found->objectid);
		length = letoh64(found->offset);
		nbits = length / sectorsize;
		if (found->type != BTRFS_FREE_SPACE_BITMAP_KEY ||
		    start > bytenr || bytenr - start >= length ||
		    length > UINT64_MAX - start ||
		    (length & (sectorsize - 1)) != 0 ||
		    (nbits + 7) / 8 != *size)
			error = EINVAL;
		else {
			*key = *found;
			*payload = malloc(*size, M_BTRFS, M_WAITOK);
			memcpy(*payload, data, *size);
		}
	}
	btrfs_release_path(&path);
	return (error);
}

static int
btrfs_free_space_bit(struct btrfs_root *root, uint64_t bytenr, int *set)
{
	struct btrfs_key key;
	uint8_t *payload;
	uint64_t bit;
	uint32_t size;
	int error;

	error = btrfs_free_space_bitmap(root, bytenr, &key, &payload, &size);
	if (error != 0)
		return (error);
	bit = (bytenr - letoh64(key.objectid)) /
	    letoh32(root->br_mount->bm_super.sectorsize);
	*set = (payload[bit / 8] >> (bit & 7)) & 1;
	free(payload, M_BTRFS, size);
	return (0);
}

static int
btrfs_modify_free_space_bitmap(struct btrfs_trans_handle *handle,
    struct btrfs_root *root, struct btrfs_block_group *group, uint64_t start,
    uint64_t end, int add, int *delta)
{
	struct btrfs_key key;
	uint8_t *payload;
	uint64_t bit, first, last, limit;
	uint32_t size, sectorsize;
	int error = 0, previous = 0, next = 0;

	sectorsize = letoh32(root->br_mount->bm_super.sectorsize);
	if (start > group->bbg_bytenr)
		error = btrfs_free_space_bit(root, start - sectorsize,
		    &previous);
	if (error == 0 && end < group->bbg_bytenr + group->bbg_length)
		error = btrfs_free_space_bit(root, end, &next);
	if (error != 0)
		return (error);
	*delta = add ? 1 - previous - next : -1 + previous + next;
	while (start < end) {
		error = btrfs_free_space_bitmap(root, start, &key, &payload,
		    &size);
		if (error != 0)
			return (error);
		limit = MIN(end, letoh64(key.objectid) + letoh64(key.offset));
		first = (start - letoh64(key.objectid)) / sectorsize;
		last = (limit - letoh64(key.objectid)) / sectorsize;
		for (bit = first; bit < last; bit++) {
			if (((payload[bit / 8] >> (bit & 7)) & 1) == add) {
				error = EINVAL;
				break;
			}
			payload[bit / 8] ^= 1U << (bit & 7);
		}
		if (error == 0)
			error = btrfs_replace_item(handle, root, &key, payload,
			    size);
		free(payload, M_BTRFS, size);
		if (error != 0)
			return (error);
		start = limit;
	}
	return (0);
}

/*
 * Extent ownership and free-space records change together at delayed-ref
 * materialization. This can COW the free-space tree and queue more refs,
 * but never recurses into the extent tree. Pinned space is free on disk in
 * the new generation and remains unavailable in memory until publication.
 * Bitmap groups retain their representation; their counts describe contiguous
 * free runs, including runs that cross bitmap boundaries. Usage is recorded
 * in the block group tree when enabled, otherwise in the extent tree.
 */
int
btrfs_update_free_space(struct btrfs_trans_handle *handle, uint64_t bytenr,
    uint64_t length, int add)
{
	struct btrfs_fs *bmp = handle->bth_transaction->bt_mount;
	struct btrfs_block_group *group;
	struct btrfs_root *root;
	struct btrfs_path path = { 0 };
	struct btrfs_key info_key = { 0 }, left, right, key = { 0 };
	struct btrfs_free_space_info info;
	const uint8_t *data;
	uint64_t start, end, count, left_start, left_end;
	uint32_t size;
	int error, have_left, have_right, delta;

	KASSERT(handle->bth_commit);
	if (!(letoh64(bmp->bm_super.compat_ro_flags) &
	    BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE))
		return (0);
	group = btrfs_space_find_group(bmp, bytenr, length);
	if (group == NULL)
		return (EINVAL);
	error = btrfs_get_root(bmp, BTRFS_FREE_SPACE_TREE_OBJECTID, &root);
	if (error != 0)
		return (error);
	info_key.objectid = htole64(group->bbg_bytenr);
	info_key.type = BTRFS_FREE_SPACE_INFO_KEY;
	info_key.offset = htole64(group->bbg_length);
	error = btrfs_search_slot(root, &info_key, &path);
	if (error == 0)
		error = btrfs_path_item(&path, NULL, &data, &size);
	if (error == 0) {
		if (size != sizeof(info))
			error = EINVAL;
		else
			memcpy(&info, data, sizeof(info));
	}
	btrfs_release_path(&path);
	if (error != 0)
		return (error);
	if (letoh32(info.flags) & ~BTRFS_FREE_SPACE_USING_BITMAPS)
		return (EOPNOTSUPP);
	count = letoh32(info.extent_count);
	start = bytenr;
	end = bytenr + length;
	if (letoh32(info.flags) & BTRFS_FREE_SPACE_USING_BITMAPS) {
		error = btrfs_modify_free_space_bitmap(handle, root, group,
		    start, end, add, &delta);
		goto update_count;
	}
	error = btrfs_free_space_neighbor(root, group, start, 1,
	    &left, &have_left);
	if (error != 0)
		return (error);
	left_start = have_left ? letoh64(left.objectid) : 0;
	left_end = have_left ? left_start + letoh64(left.offset) : 0;
	key.type = BTRFS_FREE_SPACE_EXTENT_KEY;
	if (!add) {
		if (!have_left || left_start > start || left_end < end)
			return (EINVAL);
		delta = -1 + (left_start < start) + (end < left_end);
		error = btrfs_delete_item(handle, root, &left);
		if (error == 0 && left_start < start) {
			key.objectid = htole64(left_start);
			key.offset = htole64(start - left_start);
			error = btrfs_insert_item(handle, root, &key, NULL, 0);
		}
		if (error == 0 && end < left_end) {
			key.objectid = htole64(end);
			key.offset = htole64(left_end - end);
			error = btrfs_insert_item(handle, root, &key, NULL, 0);
		}
	} else {
		if (have_left && left_end > start)
			return (EINVAL);
		error = btrfs_free_space_neighbor(root, group, start, 0,
		    &right, &have_right);
		if (error != 0)
			return (error);
		if (have_right && letoh64(right.objectid) < end)
			return (EINVAL);
		delta = 1;
		if (have_left && left_end == start) {
			start = left_start;
			delta--;
			error = btrfs_delete_item(handle, root, &left);
		}
		if (error == 0 && have_right &&
		    letoh64(right.objectid) == end) {
			end += letoh64(right.offset);
			delta--;
			error = btrfs_delete_item(handle, root, &right);
		}
		if (error == 0) {
			key.objectid = htole64(start);
			key.offset = htole64(end - start);
			error = btrfs_insert_item(handle, root, &key, NULL, 0);
		}
	}
update_count:
	if (error != 0)
		return (error);
	if ((delta < 0 && count == 0) ||
	    (delta > 0 && count == UINT32_MAX))
		return (EINVAL);
	if (delta == 0)
		return (0);
	info.extent_count = htole32(count + delta);
	return (btrfs_replace_item(handle, root, &info_key, &info,
	    sizeof(info)));
}

int
btrfs_update_space_items(struct btrfs_trans_handle *handle)
{
	struct btrfs_transaction *trans = handle->bth_transaction;
	struct btrfs_fs *bmp = trans->bt_mount;
	struct btrfs_block_group *group;
	struct btrfs_block_group_item item;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	struct btrfs_key key;
	const uint8_t *data;
	uint64_t allocated, owner, pinned, total = 0, used;
	uint32_t size;
	unsigned int i, count;
	int error;

	if (handle == NULL || handle->bth_transaction == NULL ||
	    !handle->bth_commit)
		return (EINVAL);
	owner = BTRFS_EXTENT_TREE_OBJECTID;
	if (letoh64(bmp->bm_super.compat_ro_flags) &
	    BTRFS_FEATURE_COMPAT_RO_BLOCK_GROUP_TREE)
		owner = BTRFS_BLOCK_GROUP_TREE_OBJECTID;
	error = btrfs_get_root(bmp, owner, &root);
	if (error != 0)
		return (error);

	count = btrfs_space_group_count(bmp);
	for (i = 0; i < count; i++) {
		group = btrfs_space_group_at(bmp, i);
		if (btrfs_chunk_removed(trans, group))
			continue;
		mtx_enter(&group->bbg_lock);
		allocated = group->bbg_allocated_bytes;
		pinned = group->bbg_pinned_bytes;
		if (group->bbg_disk_used > group->bbg_length ||
		    allocated > group->bbg_length - group->bbg_disk_used ||
		    pinned > group->bbg_disk_used) {
			mtx_leave(&group->bbg_lock);
			return (EINVAL);
		}
		used = group->bbg_disk_used + allocated - pinned;
		mtx_leave(&group->bbg_lock);
		if (total > UINT64_MAX - used)
			return (EOVERFLOW);
		total += used;

		memset(&key, 0, sizeof(key));
		key.objectid = htole64(group->bbg_bytenr);
		key.type = BTRFS_BLOCK_GROUP_ITEM_KEY;
		key.offset = htole64(group->bbg_length);
		error = btrfs_search_slot(root, &key, &path);
		if (error != 0)
			goto out;
		error = btrfs_path_item(&path, NULL, &data, &size);
		if (error != 0)
			goto out;
		if (size != sizeof(item)) {
			error = EINVAL;
			goto out;
		}
		memcpy(&item, data, sizeof(item));
		btrfs_release_path(&path);
		if (letoh64(item.chunk_objectid) !=
		    BTRFS_FIRST_CHUNK_TREE_OBJECTID ||
		    letoh64(item.flags) != group->bbg_flags) {
			error = EINVAL;
			goto out;
		}
		/*
		 * Rewriting unchanged usage would COW the extent tree even
		 * for an idle sync, creating unreserved delayed-reference
		 * work.  Compare against the transaction's current item so
		 * later preparation passes still publish accounting changes.
		 */
		if (letoh64(item.used) == used)
			continue;
		item.used = htole64(used);
		error = btrfs_replace_item(handle, root, &key, &item,
		    sizeof(item));
		if (error != 0)
			return (error);
	}
	trans->bt_bytes_used = total;
	return (0);
out:
	btrfs_release_path(&path);
	return (error);
}

int
btrfs_space_pin(struct btrfs_trans_handle *handle, uint64_t bytenr,
    uint64_t length)
{
	struct btrfs_transaction *trans = handle->bth_transaction;
	struct btrfs_fs *bmp = trans->bt_mount;
	struct btrfs_trans_extent *extent, *pinned;
	struct btrfs_block_group *group;
	uint32_t sectorsize;
	int error = 0;

	sectorsize = letoh32(bmp->bm_super.sectorsize);
	if (length == 0 || bytenr > UINT64_MAX - length ||
	    (bytenr & (sectorsize - 1)) != 0 ||
	    (length & (sectorsize - 1)) != 0)
		return (EINVAL);
	error = btrfs_space_handle_error(handle);
	if (error != 0)
		return (error);
	group = btrfs_space_find_group(bmp, bytenr, length);
	if (group == NULL)
		return (EINVAL);
	pinned = malloc(sizeof(*pinned), M_BTRFS, M_WAITOK | M_ZERO);

	mtx_enter(&group->bbg_lock);
	if (btrfs_space_range_is_free(group, bytenr, length)) {
		error = EINVAL;
		goto out;
	}
	mtx_enter(&trans->bt_lock);
	TAILQ_FOREACH(extent, &trans->bt_allocated_extents, bte_entry) {
		if (btrfs_space_ranges_overlap(bytenr, length,
		    extent->bte_bytenr, extent->bte_length)) {
			/* New, detached metadata has no committed owner. */
			if (extent->bte_discarded &&
			    (extent->bte_type == BTRFS_BLOCK_GROUP_METADATA ||
			    extent->bte_type == BTRFS_BLOCK_GROUP_SYSTEM) &&
			    extent->bte_bytenr == bytenr &&
			    extent->bte_length == length)
				error = 0;
			else
				error = EINVAL;
			goto unlock;
		}
	}
	TAILQ_FOREACH(extent, &trans->bt_pinned_extents, bte_entry) {
		if (btrfs_space_ranges_overlap(bytenr, length,
		    extent->bte_bytenr, extent->bte_length)) {
			error = EINVAL;
			goto unlock;
		}
	}
	if (group->bbg_pinned_bytes > UINT64_MAX - length ||
	    trans->bt_pinned_bytes > UINT64_MAX - length) {
		error = EOVERFLOW;
		goto unlock;
	}
	pinned->bte_group = group;
	pinned->bte_bytenr = bytenr;
	pinned->bte_length = length;
	group->bbg_pinned_bytes += length;
	trans->bt_pinned_bytes += length;
	trans->bt_space_seq++;
	TAILQ_INSERT_TAIL(&trans->bt_pinned_extents, pinned, bte_entry);
	pinned = NULL;
unlock:
	mtx_leave(&trans->bt_lock);
out:
	btrfs_space_check_group(group);
	mtx_leave(&group->bbg_lock);
	if (pinned != NULL)
		free(pinned, M_BTRFS, sizeof(*pinned));
	return (error);
}

void
btrfs_space_commit(struct btrfs_transaction *trans)
{
	struct btrfs_trans_extent *extent;
	struct btrfs_free_extent *space, *garbage[2];
	struct btrfs_block_group *group;
	unsigned int i, ngarbage;

	KASSERT(trans->bt_writers == 0);
	KASSERT(!trans->bt_commit_handle);
	KASSERT(TAILQ_EMPTY(&trans->bt_dirty_roots));
	KASSERT(TAILQ_EMPTY(&trans->bt_delayed_tree_refs));
	KASSERT(TAILQ_EMPTY(&trans->bt_dirty_extent_buffers));
	btrfs_space_check_commit_reserve(trans);
	while ((extent = TAILQ_FIRST(&trans->bt_allocated_extents)) != NULL) {
		KASSERT(!extent->bte_discarded);
		TAILQ_REMOVE(&trans->bt_allocated_extents, extent, bte_entry);
		group = extent->bte_group;
		mtx_enter(&group->bbg_lock);
		KASSERT(group->bbg_allocated_bytes >= extent->bte_length);
		KASSERT(group->bbg_disk_used <=
		    UINT64_MAX - extent->bte_length);
		group->bbg_allocated_bytes -= extent->bte_length;
		group->bbg_disk_used += extent->bte_length;
		btrfs_space_check_group(group);
		mtx_leave(&group->bbg_lock);
		KASSERT(trans->bt_allocated_bytes >= extent->bte_length);
		trans->bt_allocated_bytes -= extent->bte_length;
		free(extent, M_BTRFS, sizeof(*extent));
	}
	while ((extent = TAILQ_FIRST(&trans->bt_pinned_extents)) != NULL) {
		TAILQ_REMOVE(&trans->bt_pinned_extents, extent, bte_entry);
		group = extent->bte_group;
		space = malloc(sizeof(*space), M_BTRFS, M_WAITOK | M_ZERO);
		space->bfe_bytenr = extent->bte_bytenr;
		space->bfe_length = extent->bte_length;
		mtx_enter(&group->bbg_lock);
		KASSERT(group->bbg_disk_used >= extent->bte_length);
		KASSERT(group->bbg_pinned_bytes >= extent->bte_length);
		KASSERT(group->bbg_free_bytes <=
		    UINT64_MAX - extent->bte_length);
		group->bbg_disk_used -= extent->bte_length;
		group->bbg_pinned_bytes -= extent->bte_length;
		group->bbg_free_bytes += extent->bte_length;
		ngarbage = btrfs_space_insert_free_locked(group, space,
		    garbage);
		btrfs_space_check_group(group);
		mtx_leave(&group->bbg_lock);
		for (i = 0; i < ngarbage; i++)
			free(garbage[i], M_BTRFS, sizeof(*garbage[i]));
		KASSERT(trans->bt_pinned_bytes >= extent->bte_length);
		trans->bt_pinned_bytes -= extent->bte_length;
		free(extent, M_BTRFS, sizeof(*extent));
	}
	KASSERT(trans->bt_allocated_bytes == 0);
	KASSERT(trans->bt_pinned_bytes == 0);
	btrfs_space_release_list(&trans->bt_commit_reservations);
	btrfs_space_release_list(&trans->bt_reclaim_reservations);
	trans->bt_commit_reserve_target = 0;
	trans->bt_commit_reserved_bytes = 0;
	btrfs_space_check_commit_reserve(trans);
}

void
btrfs_space_abort(struct btrfs_transaction *trans)
{
	struct btrfs_trans_extent *extent;
	struct btrfs_free_extent *space, *garbage[2];
	struct btrfs_block_group *group;
	unsigned int i, ngarbage;

	KASSERT(trans->bt_writers == 0);
	KASSERT(!trans->bt_commit_handle);
	KASSERT(TAILQ_EMPTY(&trans->bt_dirty_roots));
	KASSERT(TAILQ_EMPTY(&trans->bt_delayed_tree_refs));
	KASSERT(TAILQ_EMPTY(&trans->bt_dirty_extent_buffers));
	btrfs_space_check_commit_reserve(trans);
	while ((extent = TAILQ_FIRST(&trans->bt_allocated_extents)) != NULL) {
		TAILQ_REMOVE(&trans->bt_allocated_extents, extent, bte_entry);
		group = extent->bte_group;
		space = malloc(sizeof(*space), M_BTRFS, M_WAITOK | M_ZERO);
		space->bfe_bytenr = extent->bte_bytenr;
		space->bfe_length = extent->bte_length;
		mtx_enter(&group->bbg_lock);
		KASSERT(group->bbg_allocated_bytes >= extent->bte_length);
		KASSERT(group->bbg_free_bytes <=
		    UINT64_MAX - extent->bte_length);
		group->bbg_allocated_bytes -= extent->bte_length;
		group->bbg_free_bytes += extent->bte_length;
		ngarbage = btrfs_space_insert_free_locked(group, space,
		    garbage);
		btrfs_space_check_group(group);
		mtx_leave(&group->bbg_lock);
		for (i = 0; i < ngarbage; i++)
			free(garbage[i], M_BTRFS, sizeof(*garbage[i]));
		KASSERT(trans->bt_allocated_bytes >= extent->bte_length);
		trans->bt_allocated_bytes -= extent->bte_length;
		free(extent, M_BTRFS, sizeof(*extent));
	}
	while ((extent = TAILQ_FIRST(&trans->bt_pinned_extents)) != NULL) {
		TAILQ_REMOVE(&trans->bt_pinned_extents, extent, bte_entry);
		group = extent->bte_group;
		mtx_enter(&group->bbg_lock);
		KASSERT(group->bbg_pinned_bytes >= extent->bte_length);
		group->bbg_pinned_bytes -= extent->bte_length;
		btrfs_space_check_group(group);
		mtx_leave(&group->bbg_lock);
		KASSERT(trans->bt_pinned_bytes >= extent->bte_length);
		trans->bt_pinned_bytes -= extent->bte_length;
		free(extent, M_BTRFS, sizeof(*extent));
	}
	KASSERT(trans->bt_allocated_bytes == 0);
	KASSERT(trans->bt_pinned_bytes == 0);
	btrfs_space_release_list(&trans->bt_commit_reservations);
	btrfs_space_release_list(&trans->bt_reclaim_reservations);
	trans->bt_commit_reserve_target = 0;
	trans->bt_commit_reserved_bytes = 0;
	btrfs_space_check_commit_reserve(trans);
}
