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
 * Range reads, including VM-pager reads, consult ordered data before disk and
 * use temporary storage bounded by MAXBSIZE, without a second cache of sector
 * vnode buffers. Vnode-locked writes replace at most one mapping per handle,
 * with uncompressed COW payloads of at most MAXBSIZE. NODATACOW mappings are
 * still replaced by COW; NODATASUM is preserved by omitting data checksums.
 * Larger extents would need bounded payload segments, and repeated compressed
 * reads could benefit from decompression caching.
 *
 * Writes and growth convert uncompressed or Zstd inline data of at most one
 * decoded sector to regular extents. Larger inline files, other codecs and
 * encoded mappings remain unsupported. Uncompressed and Zstd regular mappings
 * and uncompressed preallocation can be split, retaining the allocation,
 * decoded size and offsets of compressed slices.
 *
 * Shrink can discard whole compressed mappings or inline files without
 * decoding; retaining part of a compressed regular mapping requires Zstd.
 * Growth converts supported inline data and COWs partial data sectors with
 * zero tails before exposing the size. It rejects other compressed/encoded
 * overlap and regular mappings beyond the old rounded EOF; preallocation
 * stays zero-filled. Small shrinks fit one handle; larger ones publish a
 * target and recovery marker for bounded cleanup in btrfs_inode.c.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/malloc.h>
#include <sys/vnode.h>

#include <btrfs/btrfs_var.h>
#include <btrfs/btrfs_data.h>

static int	btrfs_insert_data_csums(struct btrfs_trans_handle *, uint64_t,
		    const uint8_t *, uint32_t);
static void	btrfs_encode_file_extent(struct btrfs_file_extent_item *,
		    const struct btrfs_file_extent *, uint64_t);
static inline int btrfs_ordered_compare(const struct btrfs_ordered_extent *,
		    const struct btrfs_ordered_extent *);
static inline int btrfs_ordered_file_compare(
		    const struct btrfs_ordered_extent *,
		    const struct btrfs_ordered_extent *);

RBT_HEAD(btrfs_ordered_io, btrfs_ordered_extent);
RBT_PROTOTYPE(btrfs_ordered_io, btrfs_ordered_extent, boe_io_entry,
    btrfs_ordered_compare);
RBT_GENERATE(btrfs_ordered_io, btrfs_ordered_extent, boe_io_entry,
    btrfs_ordered_compare);
RBT_GENERATE(btrfs_ordered_tree, btrfs_ordered_extent, boe_entry,
    btrfs_ordered_file_compare);

static inline int
btrfs_ordered_compare(const struct btrfs_ordered_extent *a,
    const struct btrfs_ordered_extent *b)
{
	if (a->boe_bytenr < b->boe_bytenr)
		return (-1);
	return (a->boe_bytenr > b->boe_bytenr);
}

static inline int
btrfs_ordered_file_compare(const struct btrfs_ordered_extent *a,
    const struct btrfs_ordered_extent *b)
{
	if (a->boe_treeid != b->boe_treeid)
		return (a->boe_treeid < b->boe_treeid ? -1 : 1);
	if (a->boe_objectid != b->boe_objectid)
		return (a->boe_objectid < b->boe_objectid ? -1 : 1);
	if (a->boe_file_offset < b->boe_file_offset)
		return (-1);
	return (a->boe_file_offset > b->boe_file_offset);
}

static struct btrfs_ordered_extent *
btrfs_find_ordered_sector(struct btrfs_transaction *trans,
    const struct btrfs_node *node, uint64_t file_offset)
{
	struct btrfs_ordered_extent key = { 0 }, *ordered;

	MUTEX_ASSERT_LOCKED(&trans->bt_lock);
	key.boe_treeid = node->bn_treeid;
	key.boe_objectid = node->bn_ino;
	key.boe_file_offset = file_offset;
	ordered = RBT_NFIND(btrfs_ordered_tree, &trans->bt_ordered_extents,
	    &key);
	if (ordered == NULL)
		ordered = RBT_MAX(btrfs_ordered_tree, &trans->bt_ordered_extents);
	else if (ordered->boe_file_offset != file_offset ||
	    ordered->boe_treeid != node->bn_treeid ||
	    ordered->boe_objectid != node->bn_ino)
		ordered = RBT_PREV(btrfs_ordered_tree, ordered);
	if (ordered == NULL || ordered->boe_treeid != node->bn_treeid ||
	    ordered->boe_objectid != node->bn_ino ||
	    file_offset < ordered->boe_file_offset ||
	    file_offset - ordered->boe_file_offset >= ordered->boe_file_length)
		return (NULL);
	return (ordered);
}

/*
 * Commit inserts checksums for new allocations only, in disk byte order.
 * Writers have drained, so no checksum writer can race the overlap searches.
 * Insertion and final-drop deletion belong to commit; canceled pending
 * allocations never acquire checksum items. New ranges must not overlap
 * existing ranges, and packed checksum items are capped at a quarter node.
 */
static int
btrfs_insert_data_csums(struct btrfs_trans_handle *handle, uint64_t logical,
    const uint8_t *csums, uint32_t count)
{
	struct btrfs_fs *bmp = handle->bth_transaction->bt_mount;
	const struct btrfs_key *found_key;
	const uint8_t *data;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	struct btrfs_key key;
	uint8_t *payload = NULL;
	uint64_t item_end, span, start, end;
	uint32_t item_size, sectorsize, capacity, bytes;
	size_t csum_size = btrfs_csum_size(&bmp->bm_super);
	int error;

	sectorsize = letoh32(bmp->bm_super.sectorsize);
	KASSERT(handle->bth_commit);
	KASSERT(handle->bth_transaction->bt_writers == 0);
	/* Bound copying and leave room for neighboring items in the leaf. */
	capacity = letoh32(bmp->bm_super.nodesize) / 4;
	if (count == 0 || count > capacity / csum_size ||
	    (logical & (sectorsize - 1)) != 0)
		return (EINVAL);
	span = (uint64_t)count * sectorsize;
	if (logical > UINT64_MAX - span)
		return (EINVAL);
	end = logical + span;
	bytes = count * csum_size;
	error = btrfs_get_root(bmp, BTRFS_CSUM_TREE_OBJECTID, &root);
	if (error != 0)
		return (error);

	memset(&key, 0, sizeof(key));
	key.objectid = htole64(BTRFS_EXTENT_CSUM_OBJECTID);
	key.type = BTRFS_EXTENT_CSUM_KEY;
	key.offset = htole64(logical);
	/* Newly allocated data must not overlap any existing checksum range. */
	error = btrfs_search_lower_bound(root, &key, &path);
	if (error == 0) {
		error = btrfs_path_item(&path, &found_key, NULL, NULL);
		if (error != 0)
			goto out;
		if (letoh64(found_key->objectid) ==
		    BTRFS_EXTENT_CSUM_OBJECTID &&
		    found_key->type == BTRFS_EXTENT_CSUM_KEY &&
		    letoh64(found_key->offset) < end) {
			error = EINVAL;
			goto out;
		}
	} else if (error != ENOENT)
		goto out;
	btrfs_release_path(&path);
	error = btrfs_search_predecessor(root, &key, &path);
	if (error == 0) {
		error = btrfs_path_item(&path, &found_key, &data, &item_size);
		if (error != 0)
			goto out;
		if (letoh64(found_key->objectid) !=
		    BTRFS_EXTENT_CSUM_OBJECTID ||
		    found_key->type != BTRFS_EXTENT_CSUM_KEY ||
		    item_size == 0 || item_size % csum_size != 0) {
			error = EINVAL;
			goto out;
		}
		start = letoh64(found_key->offset);
		span = (uint64_t)(item_size / csum_size) * sectorsize;
		if ((start & (sectorsize - 1)) != 0 ||
		    start > UINT64_MAX - span) {
			error = EINVAL;
			goto out;
		}
		item_end = start + span;
		if (logical < item_end) {
			error = EINVAL;
			goto out;
		}
		if (logical == item_end &&
		    item_size <= capacity - bytes) {
			payload = malloc(item_size + bytes, M_BTRFS, M_WAITOK);
			memcpy(payload, data, item_size);
			memcpy(payload + item_size, csums, bytes);
			memcpy(&key, found_key, sizeof(key));
			btrfs_release_path(&path);
			/*
			 * The lower-bound search excluded overlapping items.
			 * Replacement cannot split a full leaf; reinsertion can.
			 */
			error = btrfs_replace_item(handle, root, &key, payload,
			    item_size + bytes);
			if (error == ENOSPC) {
				error = btrfs_delete_item(handle, root, &key);
				if (error == 0)
					error = btrfs_insert_item(handle, root,
					    &key, payload,
					    item_size + bytes);
			}
			free(payload, M_BTRFS, item_size + bytes);
			return (error);
		}
		btrfs_release_path(&path);
	} else if (error != ENOENT) {
		goto out;
	}

	btrfs_release_path(&path);
	return (btrfs_insert_item(handle, root, &key, csums, bytes));

out:
	btrfs_release_path(&path);
	return (error);
}

int
btrfs_delete_data_csums(struct btrfs_trans_handle *handle, uint64_t logical,
    uint64_t length)
{
	struct btrfs_fs *bmp = handle->bth_transaction->bt_mount;
	const struct btrfs_key *found_key;
	const uint8_t *data;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	struct btrfs_key key, suffix_key;
	uint8_t *payload = NULL;
	uint64_t cursor, end, item_end, span, start;
	uint32_t item_size, prefix_size, sectorsize, suffix_size;
	size_t csum_size = btrfs_csum_size(&bmp->bm_super);
	int error;

	KASSERT(handle->bth_commit);
	KASSERT(handle->bth_transaction->bt_writers == 0);
	sectorsize = letoh32(bmp->bm_super.sectorsize);
	if (length == 0 || (logical & (sectorsize - 1)) != 0 ||
	    (length & (sectorsize - 1)) != 0 ||
	    logical > UINT64_MAX - length)
		return (EINVAL);
	end = logical + length;
	error = btrfs_get_root(bmp, BTRFS_CSUM_TREE_OBJECTID, &root);
	if (error != 0)
		return (error);

	cursor = logical;
	while (cursor < end) {
		memset(&key, 0, sizeof(key));
		key.objectid = htole64(BTRFS_EXTENT_CSUM_OBJECTID);
		key.type = BTRFS_EXTENT_CSUM_KEY;
		key.offset = htole64(cursor);
		error = btrfs_search_predecessor(root, &key, &path);
		if (error == 0) {
			error = btrfs_path_item(&path, &found_key, &data,
			    &item_size);
			if (error != 0)
				goto out;
			start = letoh64(found_key->offset);
			if (letoh64(found_key->objectid) !=
			    BTRFS_EXTENT_CSUM_OBJECTID ||
			    found_key->type != BTRFS_EXTENT_CSUM_KEY ||
			    item_size == 0 ||
			    item_size % csum_size != 0)
				goto invalid;
			span = (uint64_t)(item_size / csum_size) *
			    sectorsize;
			if ((start & (sectorsize - 1)) != 0 ||
			    start > UINT64_MAX - span)
				goto invalid;
			item_end = start + span;
			if (item_end <= cursor) {
				btrfs_release_path(&path);
				error = btrfs_search_lower_bound(root, &key,
				    &path);
			}
		} else if (error == ENOENT) {
			error = btrfs_search_lower_bound(root, &key, &path);
		}
		if (error == ENOENT) {
			error = 0;
			goto out;
		}
		if (error != 0)
			goto out;
		error = btrfs_path_item(&path, &found_key, &data, &item_size);
		if (error != 0)
			goto out;
		if (letoh64(found_key->objectid) !=
		    BTRFS_EXTENT_CSUM_OBJECTID ||
		    found_key->type != BTRFS_EXTENT_CSUM_KEY) {
			error = 0;
			goto out;
		}
		start = letoh64(found_key->offset);
		if (item_size == 0 || item_size % csum_size != 0 ||
		    (start & (sectorsize - 1)) != 0)
			goto invalid;
		span = (uint64_t)(item_size / csum_size) * sectorsize;
		if (start > UINT64_MAX - span)
			goto invalid;
		item_end = start + span;
		if (start >= end) {
			error = 0;
			goto out;
		}
		if (item_end <= cursor)
			goto invalid;

		prefix_size = 0;
		if (start < logical)
			prefix_size = (logical - start) / sectorsize *
			    csum_size;
		suffix_size = 0;
		if (item_end > end)
			suffix_size = (item_end - end) / sectorsize *
			    csum_size;
		payload = malloc(item_size, M_BTRFS, M_WAITOK);
		memcpy(payload, data, item_size);
		memcpy(&key, found_key, sizeof(key));
		btrfs_release_path(&path);

		if (prefix_size != 0)
			error = btrfs_replace_item(handle, root, &key, payload,
			    prefix_size);
		else
			error = btrfs_delete_item(handle, root, &key);
		if (error == 0 && suffix_size != 0) {
			memcpy(&suffix_key, &key, sizeof(suffix_key));
			suffix_key.offset = htole64(end);
			error = btrfs_insert_item(handle, root, &suffix_key,
			    payload + item_size - suffix_size, suffix_size);
		}
		free(payload, M_BTRFS, item_size);
		payload = NULL;
		if (error != 0)
			return (error);
		cursor = MIN(item_end, end);
	}
	return (0);

invalid:
	error = EINVAL;
out:
	btrfs_release_path(&path);
	if (payload != NULL)
		free(payload, M_BTRFS, item_size);
	return (error);
}

static void
btrfs_encode_file_extent(struct btrfs_file_extent_item *item,
    const struct btrfs_file_extent *extent, uint64_t generation)
{
	memset(item, 0, sizeof(*item));
	item->generation = htole64(generation);
	if (extent->bfe_type == BTRFS_FILE_EXTENT_HOLE) {
		item->ram_bytes = htole64(extent->bfe_length);
		item->type = BTRFS_FILE_EXTENT_REG;
		item->num_bytes = htole64(extent->bfe_length);
		return;
	}
	item->ram_bytes = htole64(extent->bfe_ram_bytes);
	item->compression = extent->bfe_compression;
	item->encryption = extent->bfe_encryption;
	item->other_encoding = htole16(extent->bfe_other_encoding);
	item->type = extent->bfe_type;
	item->disk_bytenr = htole64(extent->bfe_disk_bytenr);
	item->disk_num_bytes = htole64(extent->bfe_disk_num_bytes);
	item->offset = htole64(extent->bfe_disk_offset);
	item->num_bytes = htole64(extent->bfe_length);
}

/* Mapping edits preserve encoding; any decoding policy belongs to the caller. */
static int
btrfs_extent_edit_valid(const struct btrfs_file_extent *extent,
    uint32_t sectorsize)
{
	if (extent->bfe_length == 0 ||
	    extent->bfe_logical > UINT64_MAX - extent->bfe_length)
		return (EINVAL);
	if (extent->bfe_encryption != 0 || extent->bfe_other_encoding != 0)
		return (EOPNOTSUPP);
	if (extent->bfe_type == BTRFS_FILE_EXTENT_INLINE)
		return (extent->bfe_logical == 0 &&
		    extent->bfe_item_present ? 0 : EINVAL);
	if ((extent->bfe_logical & (sectorsize - 1)) != 0 ||
	    (extent->bfe_length & (sectorsize - 1)) != 0)
		return (EOPNOTSUPP);
	if (extent->bfe_type == BTRFS_FILE_EXTENT_HOLE)
		return (0);
	if (extent->bfe_type != BTRFS_FILE_EXTENT_REG &&
	    extent->bfe_type != BTRFS_FILE_EXTENT_PREALLOC)
		return (EINVAL);
	if (extent->bfe_disk_bytenr == 0 ||
	    extent->bfe_disk_num_bytes == 0 ||
	    (extent->bfe_disk_bytenr & (sectorsize - 1)) != 0 ||
	    (extent->bfe_disk_num_bytes & (sectorsize - 1)) != 0 ||
	    (extent->bfe_disk_offset & (sectorsize - 1)) != 0 ||
	    extent->bfe_disk_bytenr >
	    UINT64_MAX - extent->bfe_disk_num_bytes ||
	    extent->bfe_disk_offset > extent->bfe_ram_bytes ||
	    extent->bfe_length >
	    extent->bfe_ram_bytes - extent->bfe_disk_offset)
		return (EINVAL);
	if (extent->bfe_compression == BTRFS_COMPRESS_NONE) {
		if (extent->bfe_disk_offset > extent->bfe_disk_num_bytes ||
		    extent->bfe_length >
		    extent->bfe_disk_num_bytes - extent->bfe_disk_offset)
			return (EINVAL);
	} else if (extent->bfe_type != BTRFS_FILE_EXTENT_REG ||
	    extent->bfe_compression > BTRFS_COMPRESS_ZSTD ||
	    extent->bfe_disk_num_bytes > BTRFS_MAX_COMPRESSED ||
	    extent->bfe_ram_bytes > BTRFS_MAX_UNCOMPRESSED)
		return (EINVAL);
	return (0);
}

static void
btrfs_extent_plan_ref(struct btrfs_extent_plan *plan,
    const struct btrfs_file_extent *extent, int delta)
{
	unsigned int i;

	if (delta == 0 || (extent->bfe_type != BTRFS_FILE_EXTENT_REG &&
	    extent->bfe_type != BTRFS_FILE_EXTENT_PREALLOC))
		return;
	KASSERT(plan->nrefs < nitems(plan->refs));
	i = plan->nrefs++;
	plan->refs[i].bytenr = extent->bfe_disk_bytenr;
	plan->refs[i].length = extent->bfe_disk_num_bytes;
	/* Unsigned file-base subtraction is the on-disk backref ABI. */
	plan->refs[i].file_base = extent->bfe_logical - extent->bfe_disk_offset;
	plan->refs[i].delta = delta;
}

int
btrfs_extent_plan_prepare(struct btrfs_extent_plan *plan,
    struct btrfs_root *root, uint64_t ino,
    const struct btrfs_file_extent *old, uint64_t offset, uint64_t length,
    const struct btrfs_file_extent *replacement, uint64_t nbytes)
{
	uint64_t end, old_end, left, right;
	uint32_t sectorsize = letoh32(root->br_mount->bm_super.sectorsize);
	int error;

	memset(plan, 0, sizeof(*plan));
	if (length == 0 || offset > UINT64_MAX - length ||
	    old->bfe_logical > offset ||
	    old->bfe_logical > UINT64_MAX - old->bfe_length ||
	    (!old->bfe_item_present &&
	    old->bfe_type != BTRFS_FILE_EXTENT_HOLE))
		return (EINVAL);
	end = offset + length;
	old_end = old->bfe_logical + old->bfe_length;
	left = offset - old->bfe_logical;
	if (old->bfe_type == BTRFS_FILE_EXTENT_INLINE) {
		/* A sector replacement may extend beyond decoded inline EOF. */
		if (left != 0 || end < old_end ||
		    (replacement == NULL && end != old_end))
			return (EINVAL);
		right = 0;
	} else {
		if (end > old_end ||
		    (offset & (sectorsize - 1)) != 0 ||
		    (length & (sectorsize - 1)) != 0)
			return (EINVAL);
		right = old_end - end;
	}
	error = btrfs_extent_edit_valid(old, sectorsize);
	if (error != 0)
		return (error);
	plan->root = root;
	plan->ino = ino;
	plan->offset = offset;
	plan->length = length;
	plan->old = *old;
	plan->old.bfe_inline_data = NULL;
	if (old->bfe_item_present && left != 0) {
		plan->prefix = *old;
		plan->prefix.bfe_length = left;
	}
	if (old->bfe_item_present && right != 0) {
		plan->suffix = *old;
		plan->suffix.bfe_logical = end;
		plan->suffix.bfe_length = right;
		/* Holes have no decoded offset. Data slices keep file-base. */
		if (old->bfe_type != BTRFS_FILE_EXTENT_HOLE)
			plan->suffix.bfe_disk_offset += end - old->bfe_logical;
	}
	if (replacement != NULL) {
		if (replacement->bfe_logical != offset ||
		    replacement->bfe_length != length ||
		    replacement->bfe_type == BTRFS_FILE_EXTENT_INLINE)
			return (EINVAL);
		error = btrfs_extent_edit_valid(replacement, sectorsize);
		if (error != 0)
			return (error);
		plan->replacement = *replacement;
		plan->replacement.bfe_inline_data = NULL;
		plan->replacement.bfe_item_present =
		    replacement->bfe_type != BTRFS_FILE_EXTENT_HOLE ||
		    !(letoh64(root->br_mount->bm_super.incompat_flags) &
		    BTRFS_FEATURE_INCOMPAT_NO_HOLES);
		if (replacement->bfe_type != BTRFS_FILE_EXTENT_HOLE)
			plan->added = length;
		/* Queue the new owner before dropping the old one. */
		btrfs_extent_plan_ref(plan, replacement, 1);
	}
	if (old->bfe_type != BTRFS_FILE_EXTENT_HOLE)
		plan->removed = old->bfe_type == BTRFS_FILE_EXTENT_INLINE ?
		    old->bfe_length : length;
	if (plan->removed > nbytes)
		return (EINVAL);
	if (plan->added > UINT64_MAX - (nbytes - plan->removed))
		return (EOVERFLOW);
	plan->nbytes = nbytes - plan->removed + plan->added;
	/* Two retained slices still own the same allocation and file-base. */
	btrfs_extent_plan_ref(plan, old, (left != 0) + (right != 0) - 1);
	return (0);
}

int
btrfs_extent_plan_apply(struct btrfs_trans_handle *handle,
    const struct btrfs_extent_plan *plan)
{
	struct btrfs_file_extent_item item;
	struct btrfs_key key = { 0 };
	const struct btrfs_file_extent *pieces[] = {
		&plan->suffix, &plan->replacement
	};
	uint64_t generation = handle->bth_transaction->bt_generation;
	unsigned int i;
	int error;

	KASSERT(handle->bth_transaction->bt_mount == plan->root->br_mount);
	key.objectid = htole64(plan->ino);
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = htole64(plan->old.bfe_logical);
	if (plan->old.bfe_item_present) {
		if (plan->prefix.bfe_length != 0) {
			btrfs_encode_file_extent(&item, &plan->prefix, generation);
			error = btrfs_replace_item(handle, plan->root, &key,
			    &item, sizeof(item));
		} else
			error = btrfs_delete_item(handle, plan->root, &key);
		if (error != 0)
			return (error);
	}
	for (i = 0; i < nitems(pieces); i++) {
		if (!pieces[i]->bfe_item_present)
			continue;
		key.offset = htole64(pieces[i]->bfe_logical);
		btrfs_encode_file_extent(&item, pieces[i], generation);
		error = btrfs_insert_item(handle, plan->root, &key, &item,
		    sizeof(item));
		if (error != 0)
			return (error);
	}
	for (i = 0; i < plan->nrefs; i++) {
		error = btrfs_delayed_data_ref_add(handle, plan->refs[i].bytenr,
		    plan->refs[i].length,
		    btrfs_ref_data(plan->root->br_owner, plan->ino,
		    plan->refs[i].file_base), plan->refs[i].delta);
		if (error != 0)
			return (error);
	}
	return (0);
}

int
btrfs_write_ordered_extents(struct btrfs_trans_handle *handle)
{
	struct btrfs_transaction *trans;
	struct btrfs_ordered_extent *ordered, *next, *first;
	struct btrfs_ordered_io io = RBT_INITIALIZER(&io);
	struct btrfs_io_map map;
	struct btrfs_write_batch batch;
	struct btrfs_fs *bmp;
	uint8_t *scratch = NULL;
	const void *data;
	uint64_t bytenr;
	uint32_t length, sectorsize, pos;
	uint8_t csums[MAXBSIZE / BTRFS_MIN_SECTORSIZE *
	    BTRFS_SUPPORTED_CSUM_MAX];
	size_t csum_size;
	int error = 0, end_error;

	if (handle == NULL || !handle->bth_commit)
		return (EINVAL);
	trans = handle->bth_transaction;
	bmp = trans->bt_mount;
	csum_size = btrfs_csum_size(&bmp->bm_super);
	mtx_enter(&bmp->bm_trans_mtx);
	if (bmp->bm_transaction != trans || !bmp->bm_committer ||
	    trans->bt_state != BTRFS_TRANS_COMMITTING ||
	    trans->bt_writers != 0 || !trans->bt_commit_handle) {
		error = trans->bt_error != 0 ? trans->bt_error : EINVAL;
		mtx_leave(&bmp->bm_trans_mtx);
		return (error);
	}
	mtx_leave(&bmp->bm_trans_mtx);

	sectorsize = letoh32(bmp->bm_super.sectorsize);
	RBT_FOREACH(ordered, btrfs_ordered_tree, &trans->bt_ordered_extents) {
		if (ordered->boe_written)
			continue;
		if (ordered->boe_data == NULL ||
		    ordered->boe_length == 0 ||
		    ordered->boe_length > MAXBSIZE ||
		    (ordered->boe_length & (sectorsize - 1)) != 0 ||
		    (ordered->boe_bytenr & (sectorsize - 1)) != 0 ||
		    ordered->boe_bytenr > UINT64_MAX - ordered->boe_length ||
		    (ordered->boe_file_offset & (sectorsize - 1)) != 0 ||
		    ordered->boe_treeid == 0 || ordered->boe_objectid == 0)
			return (EINVAL);
		if (RBT_INSERT(btrfs_ordered_io, &io, ordered) != NULL)
			return (EINVAL);
	}
	if (RBT_EMPTY(btrfs_ordered_io, &io))
		return (0);

	/*
	 * Handles have drained: payloads and mappings remain stable through
	 * publication. Sort by allocation address, including across vnodes,
	 * and combine bounded runs without changing allocation ownership.
	 * A single payload can be submitted directly; staging is needed only
	 * when a run combines multiple allocations.
	 */
	btrfs_write_batch_init(&batch);
	ordered = RBT_MIN(btrfs_ordered_io, &io);
	while (ordered != NULL) {
		first = ordered;
		bytenr = ordered->boe_bytenr;
		data = first->boe_data;
		length = 0;
		do {
			if (ordered != first) {
				if (scratch == NULL)
					scratch = pool_get(&bmp->bm_scratch_pool,
					    PR_WAITOK);
				if (length == first->boe_length) {
					memcpy(scratch, first->boe_data, length);
					data = scratch;
				}
				memcpy(scratch + length, ordered->boe_data,
				    ordered->boe_length);
			}
			for (pos = 0; !first->boe_nodatasum &&
			    pos < ordered->boe_length; pos += sectorsize)
				btrfs_csum(&bmp->bm_super,
				    (uint8_t *)ordered->boe_data + pos,
				    sectorsize, csums +
				    (length + pos) / sectorsize * csum_size);
			length += ordered->boe_length;
			next = RBT_NEXT(btrfs_ordered_io, ordered);
			if (next == NULL ||
			    length > MAXBSIZE - next->boe_length ||
			    next->boe_bytenr != bytenr + length ||
			    next->boe_nodatasum != first->boe_nodatasum)
				break;
			/* Adjacent logical chunks need not share physical runs. */
			error = btrfs_lookup_fs_logical(bmp, bytenr,
			    length + next->boe_length, &map);
			if (error == ENOENT) {
				error = 0;
				break;
			}
			if (error != 0)
				goto out;
			ordered = next;
		} while (1);
		error = btrfs_write_logical(bmp, bytenr, length,
		    BTRFS_BLOCK_GROUP_DATA, data, &batch);
		if (error == 0 && !first->boe_nodatasum)
			error = btrfs_insert_data_csums(handle, bytenr, csums,
			    length / sectorsize);
		if (error != 0)
			break;
		/* Submission is final only if the whole phase drains successfully. */
		for (ordered = first; ordered != next;
		    ordered = RBT_NEXT(btrfs_ordered_io, ordered))
			ordered->boe_written = 1;
	}
out:
	end_error = btrfs_write_batch_wait(&batch);
	if (error == 0)
		error = end_error;
	if (scratch != NULL)
		pool_put(&bmp->bm_scratch_pool, scratch);
	return (error);
}

/*
 * Replace a run of private sector mappings and their unmaterialized adds with
 * one durable allocation. The allocator may retain separate sector accounting
 * records: their disjoint union is unchanged, and commit releases them.
 * No writer or cancellation can run after this transformation.
 */
static int
btrfs_coalesce_ordered_run(struct btrfs_trans_handle *handle,
    struct btrfs_ordered_extent *first, struct btrfs_ordered_extent *end,
    uint32_t length)
{
	struct btrfs_transaction *trans = handle->bth_transaction;
	struct btrfs_ordered_extent *ordered;
	struct btrfs_delayed_data_ref *ref;
	struct btrfs_file_extent_item item = { 0 };
	struct btrfs_root *root;
	struct btrfs_path path = { 0 };
	struct btrfs_key key = { 0 };
	const uint8_t *data;
	uint32_t size, sectorsize = first->boe_length;
	int error;

	error = btrfs_get_root(trans->bt_mount, first->boe_treeid, &root);
	if (error != 0)
		return (error);
	key.objectid = htole64(first->boe_objectid);
	key.type = BTRFS_EXTENT_DATA_KEY;
	item.generation = htole64(trans->bt_generation);
	item.ram_bytes = item.disk_num_bytes = item.num_bytes =
	    htole64(sectorsize);
	item.type = BTRFS_FILE_EXTENT_REG;
	/* Validate every private mapping before changing any of them. */
	for (ordered = first; ordered != end;
	    ordered = RBT_NEXT(btrfs_ordered_io, ordered)) {
		key.offset = htole64(ordered->boe_file_offset);
		item.disk_bytenr = htole64(ordered->boe_bytenr);
		error = btrfs_search_slot(root, &key, &path);
		if (error == 0)
			error = btrfs_path_item(&path, NULL, &data, &size);
		if (error == 0 && (size != sizeof(item) ||
		    memcmp(data, &item, sizeof(item)) != 0))
			error = EINVAL;
		btrfs_release_path(&path);
		if (error != 0)
			return (error);
	}

	key.offset = htole64(first->boe_file_offset);
	item.disk_bytenr = htole64(first->boe_bytenr);
	item.ram_bytes = item.disk_num_bytes = item.num_bytes = htole64(length);
	error = btrfs_replace_item(handle, root, &key, &item, sizeof(item));
	for (ordered = RBT_NEXT(btrfs_ordered_io, first);
	    error == 0 && ordered != end;
	    ordered = RBT_NEXT(btrfs_ordered_io, ordered)) {
		key.offset = htole64(ordered->boe_file_offset);
		error = btrfs_delete_item(handle, root, &key);
	}
	if (error != 0)
		return (error);

	RBT_REMOVE(btrfs_data_ref_tree, &trans->bt_data_ref_index,
	    first->boe_ref);
	first->boe_ref->bdr_length = length;
	RBT_INSERT(btrfs_data_ref_tree, &trans->bt_data_ref_index,
	    first->boe_ref);
	first->boe_ref = NULL;
	for (ordered = RBT_NEXT(btrfs_ordered_io, first); ordered != end;
	    ordered = RBT_NEXT(btrfs_ordered_io, ordered)) {
		ref = ordered->boe_ref;
		RBT_REMOVE(btrfs_data_ref_tree, &trans->bt_data_ref_index, ref);
		TAILQ_REMOVE(&trans->bt_delayed_data_refs, ref, bdr_entry);
		ordered->boe_ref = NULL;
		free(ref, M_BTRFS, sizeof(*ref));
	}
	return (0);
}

int
btrfs_coalesce_ordered_extents(struct btrfs_trans_handle *handle)
{
	struct btrfs_transaction *trans = handle->bth_transaction;
	struct btrfs_ordered_io io = RBT_INITIALIZER(&io);
	struct btrfs_ordered_extent probe = { 0 }, *ordered, *first, *next;
	struct btrfs_delayed_data_ref *ref;
	struct btrfs_io_map map;
	struct btrfs_fs *bmp = trans->bt_mount;
	uint32_t length, sectorsize = letoh32(bmp->bm_super.sectorsize);
	int error;

	KASSERT(handle->bth_commit);
	KASSERT(trans->bt_writers == 0);
	RBT_FOREACH(ordered, btrfs_ordered_tree, &trans->bt_ordered_extents) {
		KASSERT(ordered->boe_written);
		ordered->boe_ref = NULL;
		/* Direct ranges already own one mapping and allocation. */
		if (ordered->boe_length != sectorsize)
			continue;
		if (RBT_INSERT(btrfs_ordered_io, &io, ordered) != NULL)
			return (EINVAL);
	}
	/*
	 * Match in one pass; repeatedly searching the delayed-reference list
	 * would make a sequential transaction quadratic in its sector count.
	 * Old allocation drops cannot overlap these still-private allocations.
	 */
	TAILQ_FOREACH(ref, &trans->bt_delayed_data_refs, bdr_entry) {
		probe.boe_bytenr = ref->bdr_bytenr;
		ordered = RBT_FIND(btrfs_ordered_io, &io, &probe);
		if (ordered == NULL)
			continue;
		if (ordered->boe_ref != NULL || ref->bdr_length != sectorsize ||
		    ref->bdr_owner.kind != BTRFS_REF_IMPLICIT ||
		    ref->bdr_owner.u.implicit.root != ordered->boe_treeid ||
		    ref->bdr_owner.u.implicit.objectid != ordered->boe_objectid ||
		    ref->bdr_owner.u.implicit.offset != ordered->boe_file_offset ||
		    ref->bdr_ref_mod != 1)
			return (EINVAL);
		ordered->boe_ref = ref;
	}
	RBT_FOREACH(ordered, btrfs_ordered_tree, &trans->bt_ordered_extents)
		if (ordered->boe_length == sectorsize && ordered->boe_ref == NULL)
			return (EINVAL);

	for (ordered = RBT_MIN(btrfs_ordered_io, &io); ordered != NULL;
	    ordered = next) {
		first = ordered;
		length = sectorsize;
		for (;;) {
			next = RBT_NEXT(btrfs_ordered_io, ordered);
			if (next == NULL || length > MAXBSIZE - sectorsize ||
			    next->boe_bytenr != first->boe_bytenr + length ||
			    next->boe_treeid != first->boe_treeid ||
			    next->boe_objectid != first->boe_objectid ||
			    next->boe_file_offset != first->boe_file_offset +
			    length)
				break;
			error = btrfs_lookup_fs_logical(bmp, first->boe_bytenr,
			    length + sectorsize, &map);
			if (error == ENOENT)
				break;
			if (error != 0)
				return (error);
			length += sectorsize;
			ordered = next;
		}
		if (length == sectorsize)
			continue;
		error = btrfs_coalesce_ordered_run(handle, first, next, length);
		if (error != 0)
			return (error);
	}
	return (0);
}

int
btrfs_ordered_extents_finish(struct btrfs_transaction *trans, int committed)
{
	struct btrfs_ordered_extent *ordered;

	KASSERT(trans->bt_writers == 0);
	KASSERT(!trans->bt_commit_handle);
	mtx_enter(&trans->bt_lock);
	if (committed) {
		RBT_FOREACH(ordered, btrfs_ordered_tree,
		    &trans->bt_ordered_extents) {
			if (!ordered->boe_written) {
				mtx_leave(&trans->bt_lock);
				return (EBUSY);
			}
		}
	}
	while ((ordered = RBT_ROOT(btrfs_ordered_tree,
	    &trans->bt_ordered_extents)) != NULL) {
		RBT_REMOVE(btrfs_ordered_tree, &trans->bt_ordered_extents,
		    ordered);
		KASSERT(trans->bt_ordered_bytes >= ordered->boe_length);
		trans->bt_ordered_bytes -= ordered->boe_length;
		free(ordered->boe_data, M_BTRFS, ordered->boe_length);
		free(ordered, M_BTRFS, sizeof(*ordered));
	}
	KASSERT(trans->bt_ordered_bytes == 0);
	mtx_leave(&trans->bt_lock);
	return (0);
}

/*
 * Raw inode cleanup must not race ordered extent coalescing or leave payloads
 * referring to deleted mappings. The locked vnode excludes new writes to this
 * inode; unrelated ordered data does not require a commit before cleanup.
 */
int
btrfs_commit_inode_data(struct btrfs_node *node, struct proc *p)
{
	struct btrfs_fs *bmp = node->bn_mount;
	struct btrfs_transaction *trans;
	struct btrfs_ordered_extent key = { 0 }, *ordered;
	uint64_t generation = 0;

	KASSERT(VOP_ISLOCKED(node->bn_vnode));
	key.boe_treeid = node->bn_treeid;
	key.boe_objectid = node->bn_ino;
	mtx_enter(&bmp->bm_trans_mtx);
	trans = bmp->bm_transaction;
	if (trans != NULL) {
		/*
		 * A committer may already have retired the payloads, but its
		 * metadata changes and publication must finish before cleanup.
		 * Commit also reports an aborted transaction's error.
		 */
		if (trans->bt_state != BTRFS_TRANS_OPEN)
			generation = trans->bt_generation;
		else {
			mtx_enter(&trans->bt_lock);
			ordered = RBT_NFIND(btrfs_ordered_tree,
			    &trans->bt_ordered_extents, &key);
			if (ordered != NULL &&
			    ordered->boe_treeid == key.boe_treeid &&
			    ordered->boe_objectid == key.boe_objectid)
				generation = trans->bt_generation;
			mtx_leave(&trans->bt_lock);
		}
	}
	mtx_leave(&bmp->bm_trans_mtx);
	if (generation == 0)
		return (0);
	return (btrfs_trans_commit(bmp, generation, p));
}

/*
 * Reads and repeated writes find a containing live range under bt_lock.
 * Replacement changes its payload without splitting the private allocation.
 * A short truncate or copy fault may trim the live prefix: keep the complete
 * allocation payload for writeback/checksums, but exclude the trimmed suffix
 * from lookup. Range keys stay fixed until cancellation or teardown; final
 * mapping deletion cancels the delayed add, payload and allocation together.
 */
int
btrfs_read_ordered_range(struct btrfs_node *node, uint64_t file_offset,
    size_t length, void *data)
{
	struct btrfs_fs *bmp;
	struct btrfs_ordered_extent *ordered;
	struct btrfs_transaction *trans;
	uint32_t offset, sectorsize;
	int error = ENOENT;

	if (node == NULL || data == NULL)
		return (EINVAL);
	bmp = node->bn_mount;
	sectorsize = letoh32(bmp->bm_super.sectorsize);
	offset = file_offset & (sectorsize - 1);
	if (length == 0 || length > sectorsize - offset)
		return (EINVAL);
	file_offset -= offset;
	if ((bmp->bm_open_flags & FWRITE) == 0)
		return (ENOENT);

	mtx_enter(&bmp->bm_trans_mtx);
	trans = bmp->bm_transaction;
	if (trans == NULL) {
		mtx_leave(&bmp->bm_trans_mtx);
		return (ENOENT);
	}
	mtx_enter(&trans->bt_lock);
	if (trans->bt_state == BTRFS_TRANS_ABORTED) {
		error = trans->bt_error != 0 ? trans->bt_error : EIO;
		goto out;
	}
	ordered = btrfs_find_ordered_sector(trans, node, file_offset);
	if (ordered != NULL) {
		if (ordered->boe_data == NULL ||
		    file_offset - ordered->boe_file_offset + offset + length >
		    ordered->boe_file_length) {
			error = EINVAL;
			goto out;
		}
		memcpy(data, (uint8_t *)ordered->boe_data +
		    file_offset - ordered->boe_file_offset + offset, length);
		error = 0;
	}
out:
	mtx_leave(&trans->bt_lock);
	mtx_leave(&bmp->bm_trans_mtx);
	return (error);
}

/*
 * Older filesystems require explicit items for every hole below rounded EOF.
 * Count missing items before joining, then fill them after the data mutation
 * has succeeded.  Both passes run with the vnode locked.  Existing mappings
 * (including preallocation beyond EOF) must survive unchanged.
 */
static int
btrfs_file_holes(struct btrfs_trans_handle *handle, struct btrfs_node *node,
    uint64_t oldsize, uint64_t size, uint64_t *count)
{
	struct btrfs_fs *bmp = node->bn_mount;
	struct btrfs_file_extent extent;
	struct btrfs_file_extent_item item;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	struct btrfs_key key;
	uint64_t cursor, end, extent_end, holes = 0;
	uint32_t sectorsize;
	int error;

	KASSERT(VOP_ISLOCKED(node->bn_vnode));
	if (count != NULL)
		*count = 0;
	if (size <= oldsize || (letoh64(bmp->bm_super.incompat_flags) &
	    BTRFS_FEATURE_INCOMPAT_NO_HOLES))
		return (0);
	if (size > LLONG_MAX)
		return (EFBIG);
	sectorsize = letoh32(bmp->bm_super.sectorsize);
	cursor = roundup(oldsize, sectorsize);
	end = roundup(size, sectorsize);
	if (cursor == end)
		return (0);
	error = btrfs_get_root(bmp, node->bn_treeid, &root);
	if (error != 0)
		return (error);
	while (cursor < end) {
		error = btrfs_find_file_extent(bmp, root, &path,
		    node->bn_ino, cursor, end, &extent);
		if (error != 0)
			goto out;
		extent_end = extent.bfe_logical + extent.bfe_length;
		if (extent_end <= cursor ||
		    (extent.bfe_logical & (sectorsize - 1)) != 0 ||
		    (extent.bfe_length & (sectorsize - 1)) != 0) {
			error = EINVAL;
			goto out;
		}
		btrfs_release_path(&path);
		if (!extent.bfe_item_present) {
			KASSERT(extent.bfe_type == BTRFS_FILE_EXTENT_HOLE);
			holes++;
			if (handle != NULL) {
				memset(&key, 0, sizeof(key));
				key.objectid = htole64(node->bn_ino);
				key.type = BTRFS_EXTENT_DATA_KEY;
				key.offset = htole64(extent.bfe_logical);
				btrfs_encode_file_extent(&item, &extent,
				    handle->bth_transaction->bt_generation);
				error = btrfs_insert_item(handle, root, &key,
				    &item, sizeof(item));
				if (error != 0)
					goto out;
			}
		}
		cursor = extent_end;
	}
	if (count != NULL)
		*count = holes;
	error = 0;
out:
	btrfs_release_path(&path);
	return (error);
}

int
btrfs_count_file_holes(struct btrfs_node *node, uint64_t size, uint64_t *count)
{
	return (btrfs_file_holes(NULL, node, node->bn_inode.bi_size, size,
	    count));
}

int
btrfs_fill_file_holes(struct btrfs_trans_handle *handle,
    struct btrfs_node *node, uint64_t oldsize, uint64_t size)
{
	return (btrfs_file_holes(handle, node, oldsize, size, NULL));
}

static int
btrfs_read_regular_extent(struct btrfs_node *node,
    const struct btrfs_file_extent *extent, uint64_t file_offset, size_t size,
    uint8_t *destination)
{
	struct btrfs_fs *bmp = node->bn_mount;
	struct buf *bp = NULL;
	uint8_t *csums = NULL;
	uint64_t block, logical, relative;
	uint64_t csum_length, csum_start;
	uint64_t inode_flags;
	const uint8_t *expectedp;
	uint32_t buffer_offset, sectorsize;
	size_t chunk, nsectors, offset;
	size_t csum_size = btrfs_csum_size(&bmp->bm_super);
	int error = 0;

	sectorsize = letoh32(bmp->bm_super.sectorsize);
	inode_flags = node->bn_inode.bi_flags;
	if (file_offset < extent->bfe_logical || destination == NULL)
		return (EINVAL);
	relative = file_offset - extent->bfe_logical;
	if (relative > extent->bfe_length ||
	    size > extent->bfe_length - relative)
		return (EINVAL);
	logical = extent->bfe_disk_bytenr + extent->bfe_disk_offset + relative;
	csum_start = logical & ~((uint64_t)sectorsize - 1);
	if (size > UINT64_MAX - (logical - csum_start))
		return (EINVAL);
	csum_length = logical - csum_start + size;
	if (csum_length > UINT64_MAX - (sectorsize - 1))
		return (EINVAL);
	csum_length = roundup(csum_length, sectorsize);
	nsectors = csum_length / sectorsize;
	if ((inode_flags & BTRFS_INODE_NODATASUM) == 0) {
		csums = mallocarray(nsectors, csum_size, M_BTRFS,
		    M_WAITOK);
		error = btrfs_read_data_csums(bmp, csum_start, csum_length,
		    csums);
		if (error != 0) {
			if (error == ENOENT)
				error = EINVAL;
			goto out;
		}
	}

	while (size != 0) {
		block = logical & ~((uint64_t)sectorsize - 1);
		offset = logical - block;
		chunk = MIN(size, sectorsize - offset);
		expectedp = NULL;
		if (csums != NULL)
			expectedp = csums +
			    (block - csum_start) / sectorsize * csum_size;
		error = btrfs_read_data_sector(bmp, extent, block, expectedp,
		    &bp, &buffer_offset);
		if (error != 0)
			break;
		memcpy(destination, (uint8_t *)bp->b_data + buffer_offset +
		    offset, chunk);
		brelse(bp);
		bp = NULL;
		destination += chunk;
		logical += chunk;
		size -= chunk;
	}

out:
	if (bp != NULL)
		brelse(bp);
	if (csums != NULL)
		free(csums, M_BTRFS, nsectors * csum_size);
	return (error);
}

int
btrfs_read_file_range(struct btrfs_node *node, uint64_t offset, size_t length,
    uint8_t *destination)
{
	struct btrfs_fs *bmp = node->bn_mount;
	struct btrfs_file_extent extent;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	uint64_t available, end, file_size;
	uint32_t sectorsize;
	size_t size;
	int error = 0;

	file_size = node->bn_inode.bi_size;
	if (destination == NULL || offset > file_size ||
	    length > file_size - offset)
		return (EINVAL);
	if (length == 0)
		return (0);
	end = offset + length;
	sectorsize = letoh32(bmp->bm_super.sectorsize);

	error = btrfs_get_root(bmp, node->bn_treeid, &root);
	if (error != 0)
		return (error);

	while (error == 0 && offset < end) {
		/*
		 * Pending mappings are separate sectors until commit, which
		 * writes their data before merging them. A disk mapping found
		 * below therefore cannot span an unwritten pending sector.
		 */
		size = MIN(end - offset,
		    sectorsize - (offset & (sectorsize - 1)));
		error = btrfs_read_ordered_range(node, offset, size,
		    destination);
		if (error == 0) {
			offset += size;
			destination += size;
			continue;
		}
		if (error != ENOENT)
			break;
		error = btrfs_find_file_extent(bmp, root, &path,
		    node->bn_ino, offset, file_size, &extent);
		if (error != 0)
			break;
		if (extent.bfe_encryption != 0 ||
		    extent.bfe_other_encoding != 0) {
			error = EOPNOTSUPP;
			break;
		}

		available = extent.bfe_logical + extent.bfe_length - offset;
		size = end - offset;
		if (size > available)
			size = available;

		switch (extent.bfe_type) {
		case BTRFS_FILE_EXTENT_INLINE:
			if (extent.bfe_compression == BTRFS_COMPRESS_NONE) {
				if (extent.bfe_inline_size != extent.bfe_length) {
					error = EINVAL;
					break;
				}
				memcpy(destination, extent.bfe_inline_data +
				    offset - extent.bfe_logical, size);
			} else {
				error = btrfs_read_compressed_extent(node, &extent,
				    offset, size, destination);
			}
			break;
		case BTRFS_FILE_EXTENT_REG:
			if (extent.bfe_compression == BTRFS_COMPRESS_NONE)
				error = btrfs_read_regular_extent(node, &extent,
				    offset, size, destination);
			else
				error = btrfs_read_compressed_extent(node, &extent,
				    offset, size, destination);
			break;
		case BTRFS_FILE_EXTENT_PREALLOC:
		case BTRFS_FILE_EXTENT_HOLE:
			memset(destination, 0, size);
			break;
		default:
			error = EINVAL;
			break;
		}
		if (error == 0) {
			offset += size;
			destination += size;
		}
	}

	btrfs_release_path(&path);
	return (error);
}

static int
btrfs_check_inline_conversion(struct btrfs_node *node,
    const struct btrfs_file_extent *extent)
{
	uint32_t sectorsize = letoh32(node->bn_mount->bm_super.sectorsize);

	KASSERT(extent->bfe_type == BTRFS_FILE_EXTENT_INLINE);
	if ((extent->bfe_compression != BTRFS_COMPRESS_NONE &&
	    extent->bfe_compression != BTRFS_COMPRESS_ZSTD) ||
	    extent->bfe_encryption != 0 || extent->bfe_other_encoding != 0 ||
	    extent->bfe_length > sectorsize)
		return (EOPNOTSUPP);
	if (extent->bfe_length != node->bn_inode.bi_size ||
	    extent->bfe_length != node->bn_inode.bi_nbytes ||
	    (extent->bfe_compression == BTRFS_COMPRESS_NONE &&
	    extent->bfe_inline_size != extent->bfe_length))
		return (EINVAL);
	return (0);
}

/*
 * Preflight growth before joining a transaction.  Return the sector needing
 * COW to preserve the old prefix and zero its tail, or UINT64_MAX for purely
 * sparse growth.  The caller retains the vnode lock through mutation.
 */
static int
btrfs_check_file_extend(struct btrfs_node *node, uint64_t size,
    uint64_t *tail_offset)
{
	struct btrfs_fs *bmp = node->bn_mount;
	struct btrfs_file_extent extent;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	uint64_t cursor, end, oldsize, sector_end;
	uint32_t sectorsize;
	int error;

	KASSERT(VOP_ISLOCKED(node->bn_vnode));
	oldsize = node->bn_inode.bi_size;
	*tail_offset = UINT64_MAX;
	if (size <= oldsize || size > LLONG_MAX)
		return (EINVAL);
	sectorsize = letoh32(bmp->bm_super.sectorsize);
	end = roundup(size, sectorsize);
	error = btrfs_get_root(bmp, node->bn_treeid, &root);
	if (error != 0)
		return (error);
	error = btrfs_find_file_extent(bmp, root, &path, node->bn_ino,
	    0, end, &extent);
	if (error != 0)
		goto out;
	if (extent.bfe_type == BTRFS_FILE_EXTENT_INLINE) {
		error = btrfs_check_inline_conversion(node, &extent);
		if (error != 0)
			goto out;
		*tail_offset = 0;
		cursor = sectorsize;
	} else {
		cursor = oldsize & ~((uint64_t)sectorsize - 1);
	}
	btrfs_release_path(&path);
	while (cursor < end) {
		error = btrfs_find_file_extent(bmp, root, &path,
		    node->bn_ino, cursor, end, &extent);
		if (error != 0)
			goto out;
		sector_end = extent.bfe_logical + extent.bfe_length;
		if ((extent.bfe_compression != BTRFS_COMPRESS_NONE &&
		    extent.bfe_compression != BTRFS_COMPRESS_ZSTD) ||
		    extent.bfe_encryption != 0 || extent.bfe_other_encoding != 0 ||
		    (extent.bfe_logical & (sectorsize - 1)) != 0 ||
		    (extent.bfe_length & (sectorsize - 1)) != 0) {
			error = EOPNOTSUPP;
			goto out;
		}
		if (extent.bfe_type == BTRFS_FILE_EXTENT_REG) {
			/*
			 * Only the partial EOF sector may contain data.
			 * Preallocation beyond EOF reads as zero; a regular
			 * mapping there would require range replacement.
			 */
			if (cursor >= oldsize ||
			    sector_end > roundup(oldsize, sectorsize)) {
				error = EOPNOTSUPP;
				goto out;
			}
			*tail_offset = cursor;
		} else if (extent.bfe_type != BTRFS_FILE_EXTENT_HOLE &&
		    extent.bfe_type != BTRFS_FILE_EXTENT_PREALLOC) {
			error = EOPNOTSUPP;
			goto out;
		}
		cursor = sector_end;
		btrfs_release_path(&path);
	}
	error = 0;
out:
	btrfs_release_path(&path);
	return (error);
}

/*
 * An ordered sector has never been published and its delayed add cancels
 * the drop. Remove its payload before returning the allocation
 * to the reservation pool; a later write must not find the old payload.
 */
static int
btrfs_cancel_ordered_sector(struct btrfs_trans_handle *handle,
    struct btrfs_node *node, const struct btrfs_file_extent *extent)
{
	struct btrfs_transaction *trans = handle->bth_transaction;
	struct btrfs_ordered_extent *ordered;
	int error;

	mtx_enter(&trans->bt_lock);
	ordered = btrfs_find_ordered_sector(trans, node, extent->bfe_logical);
	if (ordered == NULL) {
		mtx_leave(&trans->bt_lock);
		return (0);
	}
	if (ordered->boe_bytenr != extent->bfe_disk_bytenr ||
	    ordered->boe_length != extent->bfe_disk_num_bytes ||
	    extent->bfe_disk_offset != 0 ||
	    extent->bfe_length != ordered->boe_file_length) {
		mtx_leave(&trans->bt_lock);
		return (EINVAL);
	}
	RBT_REMOVE(btrfs_ordered_tree, &trans->bt_ordered_extents, ordered);
	KASSERT(trans->bt_ordered_bytes >= ordered->boe_length);
	trans->bt_ordered_bytes -= ordered->boe_length;
	mtx_leave(&trans->bt_lock);
	/* Its checksums have not been inserted yet. */
	error = btrfs_space_cancel_alloc(handle, ordered->boe_bytenr,
	    ordered->boe_length);
	free(ordered->boe_data, M_BTRFS, ordered->boe_length);
	free(ordered, M_BTRFS, sizeof(*ordered));
	return (error);
}

/*
 * Walk through the final extent, including preallocation past EOF. The same
 * walk preflights format support and reserves each affected item's worst-case
 * mutation cost. The vnode lock keeps this plan stable across transaction join.
 * Resize application COWs partial EOF data before the mutation walk.
 */
static int
btrfs_file_shrink(struct btrfs_trans_handle *handle, struct btrfs_node *node,
    uint64_t size, uint64_t *count, uint64_t *tail_offset)
{
	struct btrfs_fs *bmp = node->bn_mount;
	struct btrfs_file_extent extent;
	struct btrfs_extent_plan plan;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	uint64_t cursor, cut, end, left, nbytes, items = 0;
	uint32_t sectorsize;
	int error;

	KASSERT(VOP_ISLOCKED(node->bn_vnode));
	KASSERT(size < node->bn_inode.bi_size);
	sectorsize = letoh32(bmp->bm_super.sectorsize);
	cut = roundup(size, sectorsize);
	cursor = size & ~((uint64_t)sectorsize - 1);
	nbytes = node->bn_inode.bi_nbytes;
	if (tail_offset != NULL)
		*tail_offset = UINT64_MAX;
	error = btrfs_get_root(bmp, node->bn_treeid, &root);
	if (error != 0)
		return (error);
	while (cursor < UINT64_MAX) {
		error = btrfs_find_file_extent(bmp, root, &path,
		    node->bn_ino, cursor, UINT64_MAX, &extent);
		if (error != 0)
			goto out;
		end = extent.bfe_logical + extent.bfe_length;
		btrfs_release_path(&path);
		if (end <= cursor) {
			error = EINVAL;
			goto out;
		}
		cursor = end;
		if (!extent.bfe_item_present)
			continue;
		if (extent.bfe_type == BTRFS_FILE_EXTENT_INLINE) {
			if (extent.bfe_encryption != 0 ||
			    extent.bfe_other_encoding != 0) {
				error = EOPNOTSUPP;
				goto out;
			}
			if (extent.bfe_length != node->bn_inode.bi_size ||
			    extent.bfe_length != node->bn_inode.bi_nbytes) {
				error = EINVAL;
				goto out;
			}
			items++;
			if (size != 0) {
				KASSERT(handle == NULL);
				error = btrfs_check_inline_conversion(node,
				    &extent);
				if (error != 0)
					goto out;
				*tail_offset = 0;
				continue;
			}
			left = 0;
		} else {
			/*
			 * Whole compressed mappings need only a reference drop.
			 * Zstd mappings can also retain a prefix: the caller
			 * COWs partial EOF data before we shorten the mapping.
			 */
			if ((extent.bfe_compression != BTRFS_COMPRESS_NONE &&
			    extent.bfe_compression != BTRFS_COMPRESS_ZSTD &&
			    extent.bfe_logical < size) ||
			    extent.bfe_encryption != 0 ||
			    extent.bfe_other_encoding != 0 ||
			    (extent.bfe_logical & (sectorsize - 1)) != 0 ||
			    (extent.bfe_length & (sectorsize - 1)) != 0) {
				error = EOPNOTSUPP;
				goto out;
			}
			if (tail_offset != NULL && size % sectorsize != 0 &&
			    extent.bfe_logical < size && end >= cut &&
			    extent.bfe_type == BTRFS_FILE_EXTENT_REG)
				*tail_offset = size & ~((uint64_t)sectorsize - 1);
			if (end <= cut)
				continue;
			items++;
			left = cut > extent.bfe_logical ?
			    cut - extent.bfe_logical : 0;
		}
		error = btrfs_extent_plan_prepare(&plan, root, node->bn_ino,
		    &extent, extent.bfe_logical + left, extent.bfe_length - left,
		    NULL, nbytes);
		if (error != 0)
			goto out;
		nbytes = plan.nbytes;
		if (handle == NULL)
			continue;
		error = btrfs_extent_plan_apply(handle, &plan);
		if (error != 0)
			goto out;
		if (left != 0) {
			struct btrfs_ordered_extent *ordered;

			mtx_enter(&handle->bth_transaction->bt_lock);
			ordered = btrfs_find_ordered_sector(
			    handle->bth_transaction, node, extent.bfe_logical);
			if (ordered != NULL)
				ordered->boe_file_length = left;
			mtx_leave(&handle->bth_transaction->bt_lock);
		} else if (extent.bfe_disk_bytenr != 0)
			error = btrfs_cancel_ordered_sector(handle, node, &extent);
		if (error != 0)
			goto out;
	}
	if (count != NULL)
		*count = items;
	if (handle != NULL) {
		node->bn_inode.bi_nbytes = nbytes;
		node->bn_inode.bi_dirty_fields |= BTRFS_INODE_DIRTY_NBYTES;
	}
	error = 0;
out:
	btrfs_release_path(&path);
	return (error);
}

/*
 * The inode/tail budget covers an unchanged size or sparse growth. Each
 * affected mapping (or explicit hole to insert) adds one extent-edit budget.
 * Batched shrink only stages the tail and marker in this first handle.
 */
#define BTRFS_RESIZE_METADATA_BLOCKS	64
#define BTRFS_RESIZE_BATCH_ITEMS		8

int
btrfs_resize_prepare(struct btrfs_resize_plan *plan, struct btrfs_node *node,
    uint64_t size)
{
	struct btrfs_fs *bmp = node->bn_mount;
	uint64_t items, metadata;
	uint32_t sectorsize = letoh32(bmp->bm_super.sectorsize);
	int error;

	KASSERT(VOP_ISLOCKED(node->bn_vnode));
	memset(plan, 0, sizeof(*plan));
	plan->node = node;
	plan->oldsize = node->bn_inode.bi_size;
	plan->size = size;
	plan->tail_offset = UINT64_MAX;
	if (size > LLONG_MAX)
		return (EFBIG);
	if (size < plan->oldsize) {
		error = btrfs_file_shrink(NULL, node, size,
		    &plan->affected_items, &plan->tail_offset);
		if (error != 0)
			return (error);
		plan->cleanup = plan->affected_items > BTRFS_RESIZE_BATCH_ITEMS;
	} else if (size > plan->oldsize) {
		error = btrfs_check_file_extend(node, size, &plan->tail_offset);
		if (error != 0)
			return (error);
		error = btrfs_count_file_holes(node, size, &plan->affected_items);
		if (error != 0)
			return (error);
	}
	metadata = (uint64_t)letoh32(bmp->bm_super.nodesize) *
	    BTRFS_RESIZE_METADATA_BLOCKS;
	items = plan->cleanup ? 1 : plan->affected_items;
	if (items > UINT64_MAX / metadata - 1)
		return (EOVERFLOW);
	plan->reservation.btr_metadata = metadata * (1 + items);
	plan->reservation.btr_reclaim = plan->cleanup;
	if (plan->tail_offset != UINT64_MAX) {
		plan->tail = malloc(sectorsize, M_BTRFS, M_WAITOK | M_ZERO);
		error = btrfs_read_file_range(node, plan->tail_offset,
		    MIN(plan->oldsize, size) - plan->tail_offset, plan->tail);
		if (error != 0) {
			btrfs_resize_release(plan);
			return (error);
		}
		plan->reservation.btr_data = sectorsize;
	}
	return (0);
}

int
btrfs_resize_join(struct btrfs_resize_plan *plan,
    struct btrfs_trans_handle **handle)
{
	struct btrfs_fs *bmp = plan->node->bn_mount;
	int error;

	KASSERT(VOP_ISLOCKED(plan->node->bn_vnode));
	error = btrfs_trans_join(bmp, &plan->reservation, handle);
	if (error == ENOSPC && plan->size < plan->oldsize && !plan->cleanup) {
		/* One mapping, including inline data, fits the protected handle. */
		plan->cleanup = plan->affected_items > 1;
		plan->reservation.btr_metadata =
		    (uint64_t)letoh32(bmp->bm_super.nodesize) *
		    BTRFS_RECLAIM_METADATA_BLOCKS;
		plan->reservation.btr_reclaim = 1;
		error = btrfs_trans_join(bmp, &plan->reservation, handle);
	}
	return (error);
}

int
btrfs_resize_apply(struct btrfs_trans_handle *handle,
    const struct btrfs_resize_plan *plan)
{
	struct btrfs_node *node = plan->node;
	int error = 0;

	KASSERT(VOP_ISLOCKED(node->bn_vnode));
	KASSERT(node->bn_inode.bi_size == plan->oldsize);
	if (plan->tail != NULL)
		error = btrfs_write_file_sector(handle, node, plan->tail_offset,
		    plan->tail, plan->oldsize, 0);
	if (error == 0) {
		if (plan->cleanup)
			error = btrfs_start_truncate(handle, node);
		else if (plan->size < plan->oldsize)
			error = btrfs_file_shrink(handle, node, plan->size,
			    NULL, NULL);
		else
			error = btrfs_fill_file_holes(handle, node,
			    plan->oldsize, plan->size);
	}
	if (error == 0) {
		node->bn_inode.bi_size = plan->size;
		node->bn_inode.bi_dirty_fields |= BTRFS_INODE_DIRTY_SIZE;
		node->bn_inode.bi_last_dirty_transid =
		    handle->bth_transaction->bt_generation;
		error = btrfs_write_inode(handle, node);
	}
	if (error != 0)
		btrfs_trans_abort(handle, error);
	return (error);
}

int
btrfs_resize_finish(struct btrfs_resize_plan *plan)
{
	int error = 0;

	KASSERT(VOP_ISLOCKED(plan->node->bn_vnode));
	KASSERT(plan->node->bn_inode.bi_size == plan->size);
	if (plan->cleanup)
		error = btrfs_finish_truncate(plan->node);
	btrfs_resize_release(plan);
	return (error);
}

void
btrfs_resize_release(struct btrfs_resize_plan *plan)
{
	if (plan->tail != NULL) {
		free(plan->tail, M_BTRFS,
		    letoh32(plan->node->bn_mount->bm_super.sectorsize));
		plan->tail = NULL;
	}
}

/*
 * Replace a range contained in one destination item (or synthetic hole).
 * The caller has flushed ordered data, locked both vnodes, checked alignment
 * and reserved the split, insertion, inode and reference work. Source is a
 * private decoded mapping, already sliced to the destination range. No data
 * allocation or checksum change is needed: the new owner references the
 * entire source allocation, even for a slice of a compressed extent.
 */
int
btrfs_clone_file_extent(struct btrfs_trans_handle *handle,
    struct btrfs_node *node, const struct btrfs_file_extent *source,
    const struct btrfs_file_extent *old, uint64_t offset, uint64_t length,
    uint64_t size)
{
	struct btrfs_transaction *trans = handle->bth_transaction;
	struct btrfs_extent_plan plan;
	int error;

	KASSERT(VOP_ISLOCKED(node->bn_vnode));
	error = btrfs_extent_plan_prepare(&plan, node->bn_root, node->bn_ino,
	    old, offset, length, source, node->bn_inode.bi_nbytes);
	if (error != 0)
		return (error);
	error = btrfs_extent_plan_apply(handle, &plan);
	if (error != 0)
		goto abort;
	node->bn_inode.bi_nbytes = plan.nbytes;
	node->bn_inode.bi_size = size;
	node->bn_inode.bi_dirty_fields |= BTRFS_INODE_DIRTY_NBYTES |
	    BTRFS_INODE_DIRTY_SIZE;
	node->bn_inode.bi_last_dirty_transid = trans->bt_generation;
	error = btrfs_write_inode(handle, node);
	if (error == 0)
		return (0);
abort:
	btrfs_trans_abort(handle, error);
	return (error);
}

/* Bound one replacement by its old mapping; inline conversion stays sector-sized. */
int
btrfs_file_write_length(struct btrfs_node *node, uint64_t offset,
    uint32_t *length)
{
	struct btrfs_file_extent extent;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	uint32_t sectorsize = letoh32(node->bn_mount->bm_super.sectorsize);
	uint64_t size = roundup(MAX(node->bn_inode.bi_size,
	    offset + *length), sectorsize);
	int error;

	KASSERT(VOP_ISLOCKED(node->bn_vnode));
	error = btrfs_get_root(node->bn_mount, node->bn_treeid, &root);
	if (error == 0)
		error = btrfs_find_file_extent(node->bn_mount, root, &path,
		    node->bn_ino, 0, size, &extent);
	if (error == 0 && extent.bfe_type == BTRFS_FILE_EXTENT_INLINE) {
		*length = sectorsize;
		btrfs_release_path(&path);
		return (0);
	}
	btrfs_release_path(&path);
	if (error == 0)
		error = btrfs_find_file_extent(node->bn_mount, root, &path,
		    node->bn_ino, offset, size, &extent);
	if (error == 0)
		*length = MIN(*length, extent.bfe_logical +
		    extent.bfe_length - offset);
	btrfs_release_path(&path);
	if (error == 0 && (*length == 0 ||
	    (*length & (sectorsize - 1)) != 0))
		error = EOPNOTSUPP;
	return (error);
}

int
btrfs_write_file_sector(struct btrfs_trans_handle *handle,
    struct btrfs_node *node, uint64_t file_offset, const void *data,
    uint64_t file_size, int flags)
{
	uint32_t sectorsize;

	if (node == NULL)
		return (EINVAL);
	sectorsize = letoh32(node->bn_mount->bm_super.sectorsize);
	return (btrfs_write_file_range(handle, node, file_offset, data,
	    sectorsize, 0, sectorsize, file_size, flags));
}

/*
 * Replace at most one mapping. A preallocated range may exceed the copied
 * prefix after uiomove faults: retain its complete payload/checksum ownership
 * while exposing only the rounded prefix as a file mapping.
 * A DEFER_INODE caller must encode the inode before releasing its handle.
 * NO_INLINE is valid after a successful write under the same vnode lock:
 * inline conversion has completed and no other writer can change the layout.
 */
int
btrfs_write_file_range(struct btrfs_trans_handle *handle,
    struct btrfs_node *node, uint64_t file_offset, const void *data,
    uint32_t length, uint64_t allocated_bytenr, uint32_t allocated_length,
    uint64_t file_size, int flags)
{
	struct btrfs_transaction *trans;
	struct btrfs_ordered_extent *ordered, *new_ordered = NULL;
	struct btrfs_file_extent first, old, replacement;
	struct btrfs_extent_plan plan;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	uint8_t *inline_data;
	uint64_t bytenr, end, lookup_size;
	uint32_t sectorsize;
	int error, cancel_error, nodatasum;

	if (handle == NULL || handle->bth_transaction == NULL ||
	    node == NULL || node->bn_vnode == NULL || data == NULL)
		return (EINVAL);
	KASSERT(VOP_ISLOCKED(node->bn_vnode));
	trans = handle->bth_transaction;
	sectorsize = letoh32(trans->bt_mount->bm_super.sectorsize);
	if (node->bn_mount != trans->bt_mount ||
	    node->bn_inode.bi_last_dirty_transid > trans->bt_generation ||
	    (file_offset & (sectorsize - 1)) != 0 ||
	    length == 0 || length > allocated_length ||
	    allocated_length > MAXBSIZE ||
	    (length & (sectorsize - 1)) != 0 ||
	    (allocated_length & (sectorsize - 1)) != 0 ||
	    file_offset > UINT64_MAX - length ||
	    file_size <= file_offset || file_size < node->bn_inode.bi_size ||
	    file_size > MAX(node->bn_inode.bi_size,
	    file_offset + length))
		return (EINVAL);
	nodatasum = (node->bn_inode.bi_flags & BTRFS_INODE_NODATASUM) != 0;
	end = file_offset + length;

	mtx_enter(&trans->bt_lock);
	ordered = btrfs_find_ordered_sector(trans, node, file_offset);
	mtx_leave(&trans->bt_lock);
	if (ordered != NULL) {
		KASSERT(ordered->boe_nodatasum == nodatasum);
		if (end > ordered->boe_file_offset + ordered->boe_file_length)
			return (EINVAL);
		if (allocated_bytenr != 0) {
			error = btrfs_space_cancel_alloc(handle, allocated_bytenr,
			    allocated_length);
			if (error != 0)
				goto abort;
		}
		if (file_size > node->bn_inode.bi_size) {
			node->bn_inode.bi_size = file_size;
			node->bn_inode.bi_dirty_fields |=
			    BTRFS_INODE_DIRTY_SIZE;
			node->bn_inode.bi_last_dirty_transid =
			    trans->bt_generation;
		}
		if (!(flags & BTRFS_WRITE_DEFER_INODE) &&
		    node->bn_inode.bi_dirty_fields != 0) {
			node->bn_inode.bi_last_dirty_transid =
			    trans->bt_generation;
			error = btrfs_write_inode(handle, node);
			if (error != 0)
				goto abort;
		}
		mtx_enter(&trans->bt_lock);
		memcpy((uint8_t *)ordered->boe_data +
		    file_offset - ordered->boe_file_offset, data, length);
		mtx_leave(&trans->bt_lock);
		return (0);
	}

	/* Synthetic holes must cover complete sectors for range replacement. */
	lookup_size = roundup(MAX(node->bn_inode.bi_size, end), sectorsize);
	error = btrfs_get_root(node->bn_mount, node->bn_treeid, &root);
	if (error != 0)
		return (error);
	if (file_offset != 0 && !(flags & BTRFS_WRITE_NO_INLINE)) {
		error = btrfs_find_file_extent(node->bn_mount, root, &path,
		    node->bn_ino, 0, lookup_size, &first);
		if (error != 0)
			goto out;
		if (first.bfe_type == BTRFS_FILE_EXTENT_INLINE) {
			/*
			 * Inline files cannot coexist with regular extents.
			 * Preserve sector zero in the same reserved handle
			 * before installing a write beyond it.
			 */
			error = btrfs_check_inline_conversion(node, &first);
			if (error != 0)
				goto out;
			inline_data = malloc(sectorsize, M_BTRFS,
			    M_WAITOK | M_ZERO);
			if (first.bfe_compression == BTRFS_COMPRESS_NONE)
				memcpy(inline_data, first.bfe_inline_data,
				    first.bfe_inline_size);
			else
				error = btrfs_read_compressed_extent(node,
				    &first, 0, first.bfe_length, inline_data);
			btrfs_release_path(&path);
			if (error == 0)
				error = btrfs_write_file_sector(handle, node, 0,
				    inline_data, node->bn_inode.bi_size,
				    flags);
			free(inline_data, M_BTRFS, sectorsize);
			if (error != 0)
				goto out;
			error = btrfs_write_file_range(handle, node,
			    file_offset, data, length, allocated_bytenr,
			    allocated_length, file_size,
			    flags | BTRFS_WRITE_NO_INLINE);
			if (error != 0)
				goto abort;
			return (0);
		}
		btrfs_release_path(&path);
	}
	error = btrfs_find_file_extent(node->bn_mount, root, &path,
	    node->bn_ino, file_offset, lookup_size, &old);
	if (error != 0)
		goto out;
	btrfs_release_path(&path);

	if (old.bfe_type == BTRFS_FILE_EXTENT_INLINE) {
		KASSERT(length == sectorsize);
		/* The caller's sector includes all decoded inline data. */
		error = btrfs_check_inline_conversion(node, &old);
		if (error != 0)
			return (error);
	}
	if (old.bfe_compression != BTRFS_COMPRESS_NONE &&
	    old.bfe_compression != BTRFS_COMPRESS_ZSTD)
		return (EOPNOTSUPP);

	new_ordered = malloc(sizeof(*new_ordered), M_BTRFS,
	    M_WAITOK | M_ZERO);
	new_ordered->boe_data = malloc(allocated_length, M_BTRFS, M_WAITOK);
	memcpy(new_ordered->boe_data, data, allocated_length);
	bytenr = allocated_bytenr;
	if (bytenr == 0)
		error = btrfs_space_alloc(handle, BTRFS_BLOCK_GROUP_DATA,
		    allocated_length, sectorsize, &bytenr);
	if (error != 0)
		goto out;
	new_ordered->boe_treeid = node->bn_treeid;
	new_ordered->boe_objectid = node->bn_ino;
	new_ordered->boe_file_offset = file_offset;
	new_ordered->boe_bytenr = bytenr;
	new_ordered->boe_length = allocated_length;
	new_ordered->boe_file_length = length;
	new_ordered->boe_nodatasum = nodatasum;

	memset(&replacement, 0, sizeof(replacement));
	replacement.bfe_logical = file_offset;
	replacement.bfe_length = length;
	replacement.bfe_disk_bytenr = bytenr;
	replacement.bfe_disk_num_bytes = allocated_length;
	replacement.bfe_ram_bytes = allocated_length;
	replacement.bfe_type = BTRFS_FILE_EXTENT_REG;
	error = btrfs_extent_plan_prepare(&plan, root, node->bn_ino, &old,
	    file_offset, length, &replacement, node->bn_inode.bi_nbytes);
	if (error != 0) {
		/* The caller cancels supplied allocations on preparation failure. */
		if (allocated_bytenr == 0) {
			cancel_error = btrfs_space_cancel_alloc(handle, bytenr,
			    allocated_length);
			if (cancel_error != 0) {
				error = cancel_error;
				goto abort;
			}
		}
		goto out;
	}
	error = btrfs_extent_plan_apply(handle, &plan);
	if (error != 0)
		goto abort;

	mtx_enter(&trans->bt_lock);
	ordered = RBT_INSERT(btrfs_ordered_tree, &trans->bt_ordered_extents,
	    new_ordered);
	if (ordered == NULL)
		trans->bt_ordered_bytes += new_ordered->boe_length;
	mtx_leave(&trans->bt_lock);
	if (ordered != NULL) {
		error = EINVAL;
		goto abort;
	}
	new_ordered = NULL;
	if (node->bn_inode.bi_nbytes != plan.nbytes) {
		node->bn_inode.bi_nbytes = plan.nbytes;
		node->bn_inode.bi_dirty_fields |= BTRFS_INODE_DIRTY_NBYTES;
	}
	if (file_size > node->bn_inode.bi_size) {
		node->bn_inode.bi_size = file_size;
		node->bn_inode.bi_dirty_fields |= BTRFS_INODE_DIRTY_SIZE;
	}
	node->bn_inode.bi_last_dirty_transid = trans->bt_generation;
	if (!(flags & BTRFS_WRITE_DEFER_INODE)) {
		error = btrfs_write_inode(handle, node);
		if (error != 0)
			goto abort;
	}
	return (0);

abort:
	btrfs_trans_abort(handle, error);
out:
	btrfs_release_path(&path);
	if (new_ordered != NULL) {
		free(new_ordered->boe_data, M_BTRFS, allocated_length);
		free(new_ordered, M_BTRFS, sizeof(*new_ordered));
	}
	return (error);
}
