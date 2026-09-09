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

#ifndef _BTRFS_LOG_H_
#define _BTRFS_LOG_H_

/*
 * Linux tree-log disk protocol:
 *
 * The superblock retains its committed generation and roots. log_root names
 * a tree owned by TREE_LOG_OBJECTID at generation super.generation + 1.
 * Its (TREE_LOG_OBJECTID, ROOT_ITEM, subvolume) items name per-subvolume
 * trees with the same owner and generation. These contain inode metadata,
 * directory index ranges, file extents, and checksums. Log blocks have no
 * extent-tree references. Data references are created only during replay.
 *
 * Writer and recovery share this owned, sorted item collection. The writer
 * collects one inode at a time; recovery retains the validated forest.
 * Neither collection survives its operation.
 */
struct log_item {
	RBT_ENTRY(log_item) entry;
	struct btrfs_key key;
	uint32_t size;
	uint8_t data[];
};
RBT_HEAD(log_items, log_item);
RBT_PROTOTYPE(log_items, log_item, entry, log_compare);

int	btrfs_log_key_compare(const struct btrfs_key *, const struct btrfs_key *);
struct log_item *btrfs_log_find(struct log_items *, uint64_t, uint8_t,
	    uint64_t);
int	btrfs_log_add(struct log_items *, const struct btrfs_key *,
	    const void *, uint32_t);
void	btrfs_log_free_items(struct log_items *);

#endif
