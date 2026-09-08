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

#endif
