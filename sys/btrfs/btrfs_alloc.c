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

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/malloc.h>

#include <btrfs/btrfs_var.h>

struct btrfs_space_build {
	struct btrfs_mount	*bsb_mount;
	uint64_t		 bsb_bytes_used;
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
static int	btrfs_space_add_free(struct btrfs_block_group *, uint64_t,
		    uint64_t);
static int	btrfs_space_check_type(uint64_t);
static int	btrfs_space_group_matches(const struct btrfs_block_group *,
		    uint64_t, int);
static int	btrfs_space_reserve_type(struct btrfs_mount *,
		    struct btrfs_reserved_space_list *, uint64_t, uint64_t);
static void	btrfs_space_release_list(
		    struct btrfs_reserved_space_list *);
static int	btrfs_space_handle_error(struct btrfs_trans_handle *);
static int	btrfs_space_ranges_overlap(uint64_t, uint64_t, uint64_t,
		    uint64_t);
static int	btrfs_space_range_is_free(const struct btrfs_block_group *,
		    uint64_t, uint64_t);
static unsigned int
		btrfs_space_insert_free_locked(struct btrfs_block_group *,
		    struct btrfs_free_extent *,
		    struct btrfs_free_extent **);
static void	btrfs_space_check_group(struct btrfs_block_group *);
static void	btrfs_space_check_commit_reserve(
		    struct btrfs_transaction *);
static struct btrfs_block_group *
		btrfs_space_find_group(struct btrfs_mount *, uint64_t,
		    uint64_t);

static struct btrfs_block_group *
btrfs_space_find_group(struct btrfs_mount *bmp, uint64_t bytenr,
    uint64_t length)
{
	struct btrfs_block_group *group;
	uint64_t delta;
	unsigned int i;

	if (length == 0 || bytenr > UINT64_MAX - length)
		return (NULL);
	for (i = 0; i < bmp->bm_nblock_groups; i++) {
		group = &bmp->bm_block_groups[i];
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
	struct btrfs_mount *bmp = trans->bt_mount;
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
	KASSERT(list_bytes == group->bbg_length -
	    group->bbg_disk_used - group->bbg_allocated_bytes);
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
		KASSERT(reservation->brs_type == BTRFS_BLOCK_GROUP_METADATA);
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
	struct btrfs_mount *bmp = build->bsb_mount;
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
	group = &bmp->bm_block_groups[i];
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

int
btrfs_space_init(struct btrfs_mount *bmp)
{
	struct btrfs_space_build build;
	struct btrfs_block_group *group;
	uint64_t end;
	unsigned int i;
	int error;

	KASSERT(bmp->bm_block_groups == NULL);
	KASSERT(bmp->bm_nblock_groups == 0);

	bmp->bm_block_groups = mallocarray(bmp->bm_nchunks,
	    sizeof(*bmp->bm_block_groups), M_BTRFS, M_WAITOK | M_ZERO);
	bmp->bm_nblock_groups = bmp->bm_nchunks;
	for (i = 0; i < bmp->bm_nblock_groups; i++) {
		group = &bmp->bm_block_groups[i];
		TAILQ_INIT(&group->bbg_free_extents);
		mtx_init(&group->bbg_lock, IPL_NONE);
	}
	memset(&build, 0, sizeof(build));
	build.bsb_mount = bmp;

	error = btrfs_iterate_block_groups(bmp,
	    btrfs_space_add_block_group, &build);
	if (error != 0)
		goto fail;
	error = btrfs_iterate_extent_items(bmp, btrfs_space_add_extent,
	    NULL, &build);
	if (error != 0)
		goto fail;

	for (i = 0; i < bmp->bm_nblock_groups; i++) {
		group = &bmp->bm_block_groups[i];
		if (group->bbg_length == 0 ||
		    group->bbg_build_used != group->bbg_disk_used) {
			error = EINVAL;
			goto fail;
		}
		end = group->bbg_bytenr + group->bbg_length;
		error = btrfs_space_add_free(group, group->bbg_build_cursor,
		    end - group->bbg_build_cursor);
		if (error != 0)
			goto fail;
		if (group->bbg_free_bytes !=
		    group->bbg_length - group->bbg_disk_used) {
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
		error = EINVAL;
		goto fail;
	}
	return (0);

fail:
	btrfs_space_destroy(bmp);
	return (error);
}

void
btrfs_space_destroy(struct btrfs_mount *bmp)
{
	struct btrfs_block_group *group;
	struct btrfs_free_extent *space;
	unsigned int i;

	if (bmp->bm_block_groups == NULL)
		return;
	for (i = 0; i < bmp->bm_nblock_groups; i++) {
		group = &bmp->bm_block_groups[i];
		mtx_enter(&group->bbg_lock);
		btrfs_space_check_group(group);
		KASSERT(group->bbg_reserved_bytes == 0);
		KASSERT(group->bbg_allocated_bytes == 0);
		KASSERT(group->bbg_pinned_bytes == 0);
		mtx_leave(&group->bbg_lock);
		while ((space = TAILQ_FIRST(&group->bbg_free_extents)) != NULL) {
			TAILQ_REMOVE(&group->bbg_free_extents, space, bfe_entry);
			free(space, M_BTRFS, sizeof(*space));
		}
	}
	free(bmp->bm_block_groups, M_BTRFS,
	    bmp->bm_nblock_groups * sizeof(*bmp->bm_block_groups));
	bmp->bm_block_groups = NULL;
	bmp->bm_nblock_groups = 0;
}

static int
btrfs_space_reserve_type(struct btrfs_mount *bmp,
    struct btrfs_reserved_space_list *reservations, uint64_t type,
    uint64_t bytes)
{
	struct btrfs_reserved_space *reservation;
	struct btrfs_block_group *group;
	uint64_t take;
	unsigned int i;
	int mixed;

	for (mixed = 0; mixed <= 1 && bytes != 0; mixed++) {
		for (i = 0; i < bmp->bm_nblock_groups && bytes != 0; i++) {
			group = &bmp->bm_block_groups[i];
			if (!btrfs_space_group_matches(group, type, mixed))
				continue;
			reservation = malloc(sizeof(*reservation), M_BTRFS,
			    M_WAITOK | M_ZERO);
			mtx_enter(&group->bbg_lock);
			take = MIN(bytes, group->bbg_free_bytes);
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
				bytes -= take;
			}
			btrfs_space_check_group(group);
			mtx_leave(&group->bbg_lock);
			if (take == 0)
				free(reservation, M_BTRFS,
				    sizeof(*reservation));
		}
	}
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
	struct btrfs_mount *bmp = handle->bth_transaction->bt_mount;
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
	error = btrfs_space_reserve_type(bmp, &handle->bth_reservations,
	    BTRFS_BLOCK_GROUP_SYSTEM, request->btr_system);
	if (error == 0)
		error = btrfs_space_reserve_type(bmp,
		    &handle->bth_reservations, BTRFS_BLOCK_GROUP_METADATA,
		    request->btr_metadata);
	if (error == 0)
		error = btrfs_space_reserve_type(bmp,
		    &handle->bth_reservations, BTRFS_BLOCK_GROUP_DATA,
		    request->btr_data);
	if (error != 0)
		btrfs_space_release(handle);
	return (error);
}

int
btrfs_space_reserve_commit(struct btrfs_transaction *trans)
{
	struct btrfs_mount *bmp = trans->bt_mount;
	uint64_t bytes;
	uint32_t nodesize;
	int error;

	KASSERT(TAILQ_EMPTY(&trans->bt_commit_reservations));
	KASSERT(trans->bt_commit_reserve_target == 0);
	KASSERT(trans->bt_commit_reserved_bytes == 0);

	nodesize = letoh32(bmp->bm_super.nodesize);
	bytes = nodesize * BTRFS_COMMIT_METADATA_BLOCKS;
	error = btrfs_space_reserve_type(bmp,
	    &trans->bt_commit_reservations, BTRFS_BLOCK_GROUP_METADATA,
	    bytes);
	if (error != 0) {
		btrfs_space_release_list(&trans->bt_commit_reservations);
		return (error);
	}
	trans->bt_commit_reserve_target = bytes;
	trans->bt_commit_reserved_bytes = bytes;
	btrfs_space_check_commit_reserve(trans);
	return (0);
}

void
btrfs_space_release(struct btrfs_trans_handle *handle)
{
	btrfs_space_release_list(&handle->bth_reservations);
}

int
btrfs_space_alloc(struct btrfs_trans_handle *handle, uint64_t type,
    uint64_t length, uint64_t alignment, uint64_t *bytenrp)
{
	struct btrfs_transaction *trans = handle->bth_transaction;
	struct btrfs_mount *bmp = trans->bt_mount;
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
	if (reservation == NULL)
		return (EINVAL);

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

int
btrfs_update_space_items(struct btrfs_trans_handle *handle)
{
	struct btrfs_transaction *trans = handle->bth_transaction;
	struct btrfs_mount *bmp = trans->bt_mount;
	struct btrfs_block_group *group;
	struct btrfs_block_group_item item;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	struct btrfs_key key;
	const uint8_t *data;
	uint64_t allocated, pinned, total = 0, used;
	uint32_t size;
	unsigned int i;
	int error;

	if (handle == NULL || handle->bth_transaction == NULL ||
	    !handle->bth_commit)
		return (EINVAL);
	error = btrfs_get_root(bmp, BTRFS_EXTENT_TREE_OBJECTID, &root);
	if (error != 0)
		return (error);

	for (i = 0; i < bmp->bm_nblock_groups; i++) {
		group = &bmp->bm_block_groups[i];
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
	struct btrfs_mount *bmp = trans->bt_mount;
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
	trans->bt_commit_reserve_target = 0;
	trans->bt_commit_reserved_bytes = 0;
	btrfs_space_check_commit_reserve(trans);
}
