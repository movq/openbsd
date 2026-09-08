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

#ifndef _BTRFS_DATA_H_
#define _BTRFS_DATA_H_

/*
 * Replace a range in one decoded mapping. A NULL replacement removes mappings
 * beyond EOF; a hole replacement keeps the range inside the file. At most a
 * prefix, suffix and replacement survive. Inline mappings can only be removed
 * whole; callers decode any retained data.
 *
 * Preparation borrows no leaf storage and validates against current nbytes.
 * Callers serialize the mapping and accounting through prepare and apply.
 * Discard the plan on preparation error. Apply once under an already reserved
 * handle; any apply error requires abort. Allocation, ordered payloads and
 * inode publication stay with callers.
 *
 * Splits preserve allocation and file-base identity, including compressed
 * decoded offsets. Inline removal accounts for decoded bytes, and replacement
 * holes follow NO_HOLES. Writes, clone and truncate cleanup share this plan.
 */
struct btrfs_extent_plan {
	struct btrfs_root	*root;
	uint64_t		ino;
	uint64_t		offset;
	uint64_t		length;
	struct btrfs_file_extent	old;
	struct btrfs_file_extent	prefix;
	struct btrfs_file_extent	suffix;
	struct btrfs_file_extent	replacement;
	/* Unsigned add/remove amounts avoid a signed byte-delta limit. */
	uint64_t		removed;
	uint64_t		added;
	uint64_t		nbytes;
	unsigned int		nrefs;
	struct {
		uint64_t	bytenr;
		uint64_t	length;
		uint64_t	file_base;
		int		delta;
	} refs[2];
};

int	btrfs_extent_plan_prepare(struct btrfs_extent_plan *,
	    struct btrfs_root *, uint64_t, const struct btrfs_file_extent *,
	    uint64_t, uint64_t, const struct btrfs_file_extent *, uint64_t);
int	btrfs_extent_plan_apply(struct btrfs_trans_handle *,
	    const struct btrfs_extent_plan *);

/*
 * Resize a locked regular file. Keep the vnode locked through finish/release.
 * Prepare validates mappings and stages a zero-tailed COW sector without
 * changing the inode; its errors leave no resources held. A zeroed plan can
 * also be released. Join reserves the initial handle, falling back to
 * protected, bounded cleanup for a shrink under metadata pressure.
 *
 * The caller saves the inode and may stage attribute changes before apply.
 * Apply encodes those attributes and the initial resize in the supplied handle;
 * it aborts that handle on error. The caller ends the handle. An apply/end
 * error requires restoring the saved inode and releasing the plan.
 *
 * Successful handle end publishes the target: the caller updates its VM state,
 * then MUST call finish, even if a later caller operation fails. Finish releases
 * the plan and completes any durable cleanup. Its errors never roll back the
 * target; they leave recovery work on a read-only filesystem.
 * Release alone is for preparation/join/apply/end failure, not published work.
 *
 * Setattr owns attribute policy; setattr and clone own VM updates and
 * notifications. Clone uses this protocol for destination growth.
 */
struct btrfs_resize_plan {
	struct btrfs_node	*node;
	uint64_t		oldsize;
	uint64_t		size;
	uint64_t		tail_offset;	/* UINT64_MAX if no tail COW. */
	uint8_t			*tail;
	uint64_t		affected_items;	/* Shrink edits or growth hole inserts. */
	struct btrfs_trans_reservation reservation;
	int			cleanup;
};

int	btrfs_resize_prepare(struct btrfs_resize_plan *, struct btrfs_node *,
	    uint64_t);
int	btrfs_resize_join(struct btrfs_resize_plan *,
	    struct btrfs_trans_handle **);
int	btrfs_resize_apply(struct btrfs_trans_handle *,
	    const struct btrfs_resize_plan *);
int	btrfs_resize_finish(struct btrfs_resize_plan *);
void	btrfs_resize_release(struct btrfs_resize_plan *);

#endif
