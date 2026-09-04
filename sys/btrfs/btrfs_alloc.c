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

static int	btrfs_space_add_block_group(
		    const struct btrfs_block_group_record *, void *);
static int	btrfs_space_add_extent(const struct btrfs_extent_record *,
		    void *);
static int	btrfs_space_add_free(struct btrfs_block_group *, uint64_t,
		    uint64_t);
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
