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

#ifndef _BTRFS_DIR_H_
#define _BTRFS_DIR_H_

/* Decoded views borrow storage from the supplied packed item. */
struct btrfs_dir_record {
	const struct btrfs_dir_item *item;
	const uint8_t	*name;
	const uint8_t	*value;
	uint32_t	size;
	uint16_t	namelen;
	uint16_t	datalen;
};

/*
 * A namespace operation keeps one private image per key. Callers serialize
 * the affected names, indexes and references through preparation and apply.
 * Prepare removals before additions, then apply once with a reserved handle.
 * Any preparation error discards the plan; any apply error requires abort.
 * Twelve items cover rename, including unsuccessful ordinary-ref probes.
 */
#define BTRFS_NAME_ITEMS	12
struct btrfs_name_edit {
	struct btrfs_key	key;
	uint8_t		*data;
	uint32_t	size;
	uint32_t	oldsize;
	int		dirty;
};

struct btrfs_name_plan {
	struct btrfs_root	*root;
	uint32_t		nodesize;
	uint32_t		capacity;
	unsigned int		count;
	unsigned int		nadds;
	struct btrfs_name_edit	edits[BTRFS_NAME_ITEMS];
	struct {
		struct btrfs_name_edit *hash;
		struct btrfs_name_edit *index;
		uint32_t offset;
	} adds[BTRFS_NAME_ITEMS];
};

int	btrfs_name_valid(const uint8_t *, size_t);
uint64_t btrfs_name_hash(const void *, size_t);
int	btrfs_decode_dir_record(const uint8_t *, uint32_t,
	    struct btrfs_dir_record *);
int	btrfs_validate_dir_record(const struct btrfs_key *, uint64_t,
	    uint32_t, const struct btrfs_dir_record *);
int	btrfs_next_dir_index(struct btrfs_root *, uint64_t, uint64_t *);
struct btrfs_name_plan *btrfs_name_plan_alloc(struct btrfs_root *);
void	btrfs_name_plan_free(struct btrfs_name_plan *);
int	btrfs_name_edit(struct btrfs_name_plan *, uint64_t, uint8_t,
	    uint64_t, struct btrfs_name_edit **);
int	btrfs_plan_dir_add(struct btrfs_name_plan *, uint64_t,
	    const char *, size_t, uint64_t, const struct btrfs_dir_item *);
int	btrfs_plan_dir_remove(struct btrfs_name_plan *, uint64_t,
	    const char *, size_t, uint64_t, uint64_t, uint8_t,
	    struct btrfs_dir_item *);
int	btrfs_name_plan_apply(struct btrfs_trans_handle *,
	    struct btrfs_name_plan *);

#endif
