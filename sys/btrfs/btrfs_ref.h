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

#ifndef _BTRFS_REF_H_
#define _BTRFS_REF_H_

/* Shared ownership names the parent block, independent of its tree owner. */
enum btrfs_ref_ownership {
	BTRFS_REF_IMPLICIT,
	BTRFS_REF_SHARED
};

struct btrfs_ref_owner {
	enum btrfs_ref_ownership kind;
	union {
		struct {
			uint64_t root;
			uint64_t objectid;
			uint64_t offset;
		} implicit;
		uint64_t parent;
	} u;
};

static inline struct btrfs_ref_owner
btrfs_ref_data(uint64_t root, uint64_t objectid, uint64_t file_base)
{
	struct btrfs_ref_owner owner = { .kind = BTRFS_REF_IMPLICIT };

	owner.u.implicit.root = root;
	owner.u.implicit.objectid = objectid;
	owner.u.implicit.offset = file_base;
	return (owner);
}

static inline struct btrfs_ref_owner
btrfs_ref_tree(uint64_t root)
{
	return (btrfs_ref_data(root, 0, 0));
}

static inline struct btrfs_ref_owner
btrfs_ref_shared(uint64_t parent)
{
	struct btrfs_ref_owner owner = { .kind = BTRFS_REF_SHARED };

	owner.u.parent = parent;
	return (owner);
}

enum btrfs_tree_ref_operation {
	BTRFS_TREE_REF_DELTA,
	BTRFS_TREE_REF_CONVERT_FULL
};

/*
 * Decoded reference and byte locations in the supplied item. count_offset is
 * meaningful for counted references only; tree references always count as one.
 * The decoder accepts legacy separate refs. Write eligibility is a separate
 * decision, as are extent headers and simple-quota owner records.
 */
struct btrfs_ref_decoded {
	struct btrfs_ref_owner owner;
	uint64_t generation;
	uint32_t count;
	uint32_t position;
	uint32_t size;
	uint32_t count_offset;
	uint8_t type;
};

int	btrfs_ref_decode(uint8_t, uint64_t, const uint8_t *, uint32_t,
	    struct btrfs_ref_decoded *);
int	btrfs_ref_inline(const uint8_t *, uint32_t, uint32_t, uint64_t,
	    struct btrfs_ref_decoded *);

#endif /* _BTRFS_REF_H_ */
