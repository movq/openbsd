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
#include <sys/fcntl.h>
#include <sys/malloc.h>
#include <sys/vnode.h>

#include <lib/libkern/crc32c.h>

#include <btrfs/btrfs_var.h>

static int	btrfs_set_data_csum(struct btrfs_trans_handle *, uint64_t,
		    uint32_t);
static int	btrfs_set_data_csum_locked(struct btrfs_trans_handle *, uint64_t,
		    uint32_t);
static int	btrfs_delete_data_csums(struct btrfs_trans_handle *, uint64_t,
		    uint64_t);
static int	btrfs_delete_data_csums_locked(struct btrfs_trans_handle *,
		    uint64_t, uint64_t);
static void	btrfs_encode_file_extent(struct btrfs_file_extent_item *,
		    const struct btrfs_file_extent *, uint64_t);
static int	btrfs_find_separate_data_ref(struct btrfs_root *, uint64_t,
		    uint64_t, uint64_t, uint64_t, struct btrfs_key *,
		    struct btrfs_extent_data_ref *);
static int	btrfs_materialize_data_ref(struct btrfs_trans_handle *,
		    const struct btrfs_delayed_data_ref *);
static inline int btrfs_ordered_compare(const struct btrfs_ordered_extent *,
		    const struct btrfs_ordered_extent *);

RBT_HEAD(btrfs_ordered_io, btrfs_ordered_extent);
RBT_PROTOTYPE(btrfs_ordered_io, btrfs_ordered_extent, boe_io_entry,
    btrfs_ordered_compare);
RBT_GENERATE(btrfs_ordered_io, btrfs_ordered_extent, boe_io_entry,
    btrfs_ordered_compare);

static inline int
btrfs_ordered_compare(const struct btrfs_ordered_extent *a,
    const struct btrfs_ordered_extent *b)
{
	if (a->boe_bytenr < b->boe_bytenr)
		return (-1);
	return (a->boe_bytenr > b->boe_bytenr);
}

static int
btrfs_set_data_csum(struct btrfs_trans_handle *handle, uint64_t logical,
    uint32_t csum)
{
	int error;

	/* Adjacent allocations from different vnodes can share one item. */
	rw_enter_write(&handle->bth_transaction->bt_csum_lock);
	error = btrfs_set_data_csum_locked(handle, logical, csum);
	rw_exit_write(&handle->bth_transaction->bt_csum_lock);
	return (error);
}

static int
btrfs_set_data_csum_locked(struct btrfs_trans_handle *handle, uint64_t logical,
    uint32_t csum)
{
	struct btrfs_fs *bmp = handle->bth_transaction->bt_mount;
	const struct btrfs_key *found_key;
	const uint8_t *data;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	struct btrfs_key key;
	uint8_t *payload = NULL;
	uint64_t item_end, span, start;
	uint32_t disk_csum, item_size, sectorsize, capacity;
	size_t index;
	int error;

	sectorsize = letoh32(bmp->bm_super.sectorsize);
	/* Bound copying and leave room for neighboring items in the leaf. */
	capacity = letoh32(bmp->bm_super.nodesize) / 4;
	if ((logical & (sectorsize - 1)) != 0)
		return (EINVAL);
	if (logical > UINT64_MAX - sectorsize)
		return (EINVAL);
	error = btrfs_get_root(bmp, BTRFS_CSUM_TREE_OBJECTID, &root);
	if (error != 0)
		return (error);

	memset(&key, 0, sizeof(key));
	key.objectid = htole64(BTRFS_EXTENT_CSUM_OBJECTID);
	key.type = BTRFS_EXTENT_CSUM_KEY;
	key.offset = htole64(logical);
	error = btrfs_search_predecessor(root, &key, &path);
	if (error == 0) {
		error = btrfs_path_item(&path, &found_key, &data, &item_size);
		if (error != 0)
			goto out;
		if (letoh64(found_key->objectid) !=
		    BTRFS_EXTENT_CSUM_OBJECTID ||
		    found_key->type != BTRFS_EXTENT_CSUM_KEY ||
		    item_size == 0 || item_size % sizeof(disk_csum) != 0) {
			error = EINVAL;
			goto out;
		}
		start = letoh64(found_key->offset);
		span = (uint64_t)(item_size / sizeof(disk_csum)) * sectorsize;
		if ((start & (sectorsize - 1)) != 0 ||
		    start > UINT64_MAX - span) {
			error = EINVAL;
			goto out;
		}
		item_end = start + span;
		if (logical >= start && logical < item_end) {
			payload = malloc(item_size, M_BTRFS, M_WAITOK);
			memcpy(payload, data, item_size);
			memcpy(&key, found_key, sizeof(key));
			index = (logical - start) / sectorsize;
			disk_csum = htole32(csum);
			memcpy(payload + index * sizeof(disk_csum), &disk_csum,
			    sizeof(disk_csum));
			btrfs_release_path(&path);
			error = btrfs_replace_item(handle, root, &key, payload,
			    item_size);
			free(payload, M_BTRFS, item_size);
			return (error);
		}
		if (logical == item_end &&
		    item_size <= capacity - sizeof(disk_csum)) {
			payload = malloc(item_size + sizeof(disk_csum),
			    M_BTRFS, M_WAITOK);
			memcpy(payload, data, item_size);
			disk_csum = htole32(csum);
			memcpy(payload + item_size, &disk_csum,
			    sizeof(disk_csum));
			memcpy(&key, found_key, sizeof(key));
			btrfs_release_path(&path);
			/*
			 * Keys and checksum ranges are sector aligned, so the
			 * next item cannot overlap this one-sector extension.
			 * Replacement cannot split a full leaf; reinsertion can.
			 */
			error = btrfs_replace_item(handle, root, &key, payload,
			    item_size + sizeof(disk_csum));
			if (error == ENOSPC) {
				error = btrfs_delete_item(handle, root, &key);
				if (error == 0)
					error = btrfs_insert_item(handle, root,
					    &key, payload,
					    item_size + sizeof(disk_csum));
			}
			free(payload, M_BTRFS, item_size + sizeof(disk_csum));
			return (error);
		}
		btrfs_release_path(&path);
	} else if (error != ENOENT) {
		goto out;
	}

	error = btrfs_search_lower_bound(root, &key, &path);
	if (error == 0) {
		error = btrfs_path_item(&path, &found_key, NULL, NULL);
		if (error != 0)
			goto out;
		if (letoh64(found_key->objectid) ==
		    BTRFS_EXTENT_CSUM_OBJECTID &&
		    found_key->type == BTRFS_EXTENT_CSUM_KEY &&
		    letoh64(found_key->offset) < logical + sectorsize) {
			error = EINVAL;
			goto out;
		}
	} else if (error != ENOENT) {
		goto out;
	}
	btrfs_release_path(&path);
	disk_csum = htole32(csum);
	return (btrfs_insert_item(handle, root, &key, &disk_csum,
	    sizeof(disk_csum)));

out:
	btrfs_release_path(&path);
	return (error);
}

static int
btrfs_delete_data_csums(struct btrfs_trans_handle *handle, uint64_t logical,
    uint64_t length)
{
	int error;

	rw_enter_write(&handle->bth_transaction->bt_csum_lock);
	error = btrfs_delete_data_csums_locked(handle, logical, length);
	rw_exit_write(&handle->bth_transaction->bt_csum_lock);
	return (error);
}

static int
btrfs_delete_data_csums_locked(struct btrfs_trans_handle *handle,
    uint64_t logical, uint64_t length)
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
	int error;

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
			    item_size % sizeof(uint32_t) != 0)
				goto invalid;
			span = (uint64_t)(item_size / sizeof(uint32_t)) *
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
		if (item_size == 0 || item_size % sizeof(uint32_t) != 0 ||
		    (start & (sectorsize - 1)) != 0)
			goto invalid;
		span = (uint64_t)(item_size / sizeof(uint32_t)) * sectorsize;
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
			    sizeof(uint32_t);
		suffix_size = 0;
		if (item_end > end)
			suffix_size = (item_end - end) / sectorsize *
			    sizeof(uint32_t);
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

int
btrfs_delayed_data_ref_add(struct btrfs_trans_handle *handle,
    uint64_t bytenr, uint64_t length, uint64_t root, uint64_t objectid,
    uint64_t offset, int ref_mod)
{
	struct btrfs_delayed_data_ref *ref, *new;
	struct btrfs_transaction *trans;
	struct btrfs_fs *bmp;
	uint32_t sectorsize;

	if (handle == NULL || handle->bth_transaction == NULL ||
	    ref_mod == 0)
		return (EINVAL);
	trans = handle->bth_transaction;
	bmp = trans->bt_mount;
	sectorsize = letoh32(bmp->bm_super.sectorsize);
	if (bytenr == 0 || length == 0 ||
	    (bytenr & (sectorsize - 1)) != 0 ||
	    (length & (sectorsize - 1)) != 0 ||
	    bytenr > UINT64_MAX - length || root == 0 || objectid == 0)
		return (EINVAL);

	new = malloc(sizeof(*new), M_BTRFS, M_WAITOK | M_ZERO);
	new->bdr_bytenr = bytenr;
	new->bdr_length = length;
	new->bdr_root = root;
	new->bdr_objectid = objectid;
	new->bdr_offset = offset;
	new->bdr_ref_mod = ref_mod;

	mtx_enter(&trans->bt_lock);
	TAILQ_FOREACH(ref, &trans->bt_delayed_data_refs, bdr_entry) {
		if (ref->bdr_bytenr == bytenr &&
		    ref->bdr_length == length && ref->bdr_root == root &&
		    ref->bdr_objectid == objectid &&
		    ref->bdr_offset == offset)
			break;
	}
	if (ref == NULL) {
		TAILQ_INSERT_TAIL(&trans->bt_delayed_data_refs, new,
		    bdr_entry);
		new = NULL;
	} else if ((ref_mod > 0 &&
	    ref->bdr_ref_mod > INT64_MAX - ref_mod) ||
	    (ref_mod < 0 && ref->bdr_ref_mod < INT64_MIN - ref_mod)) {
		mtx_leave(&trans->bt_lock);
		free(new, M_BTRFS, sizeof(*new));
		return (EOVERFLOW);
	} else {
		ref->bdr_ref_mod += ref_mod;
		if (ref->bdr_ref_mod == 0) {
			TAILQ_REMOVE(&trans->bt_delayed_data_refs, ref,
			    bdr_entry);
			free(ref, M_BTRFS, sizeof(*ref));
		}
	}
	mtx_leave(&trans->bt_lock);
	if (new != NULL)
		free(new, M_BTRFS, sizeof(*new));
	handle->bth_delayed = 1;
	return (0);
}

static int
btrfs_find_separate_data_ref(struct btrfs_root *root, uint64_t bytenr,
    uint64_t ref_root, uint64_t objectid, uint64_t offset,
    struct btrfs_key *result_key, struct btrfs_extent_data_ref *result_ref)
{
	const struct btrfs_extent_data_ref *data_ref;
	const struct btrfs_key *key;
	const uint8_t *data;
	struct btrfs_path path = { 0 };
	struct btrfs_key target;
	uint32_t size;
	int error;

	memset(&target, 0, sizeof(target));
	target.objectid = htole64(bytenr);
	target.type = BTRFS_EXTENT_DATA_REF_KEY;
	error = btrfs_search_lower_bound(root, &target, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &key, &data, &size);
		if (error != 0)
			break;
		if (letoh64(key->objectid) != bytenr ||
		    key->type != BTRFS_EXTENT_DATA_REF_KEY) {
			error = ENOENT;
			break;
		}
		if (size != sizeof(*data_ref)) {
			error = EINVAL;
			break;
		}
		data_ref = (const struct btrfs_extent_data_ref *)data;
		if (letoh64(data_ref->root) == ref_root &&
		    letoh64(data_ref->objectid) == objectid &&
		    letoh64(data_ref->offset) == offset) {
			if (letoh32(data_ref->count) == 0) {
				error = EINVAL;
				break;
			}
			memcpy(result_key, key, sizeof(*result_key));
			memcpy(result_ref, data_ref, sizeof(*result_ref));
			error = 0;
			goto out;
		}
		error = btrfs_next_item(&path);
	}
	if (error == ENOENT)
		error = ENOENT;
out:
	btrfs_release_path(&path);
	return (error);
}

static int
btrfs_materialize_data_ref(struct btrfs_trans_handle *handle,
    const struct btrfs_delayed_data_ref *ref)
{
	struct btrfs_transaction *trans = handle->bth_transaction;
	struct btrfs_fs *bmp = trans->bt_mount;
	const struct btrfs_extent_inline_ref *candidate;
	const struct btrfs_extent_data_ref *candidate_data;
	struct btrfs_extent_inline_ref *inline_ref, *matched_inline = NULL;
	struct btrfs_extent_data_ref *data_ref;
	struct btrfs_extent_item *extent;
	const struct btrfs_key *found_key;
	const uint8_t *data;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	struct btrfs_key extent_key, separate_key;
	struct btrfs_extent_data_ref separate_ref;
	uint8_t *payload = NULL;
	uint64_t delta, refs;
	uint32_t alloc_size = 0, count = 0, i, remain, size, step;
	int error, found_inline = 0, found_separate = 0;

	error = btrfs_get_root(bmp, BTRFS_EXTENT_TREE_OBJECTID, &root);
	if (error != 0)
		return (error);
	memset(&extent_key, 0, sizeof(extent_key));
	extent_key.objectid = htole64(ref->bdr_bytenr);
	extent_key.type = BTRFS_EXTENT_ITEM_KEY;
	extent_key.offset = htole64(ref->bdr_length);

	error = btrfs_search_slot(root, &extent_key, &path);
	if (error == ENOENT) {
		btrfs_release_path(&path);
		if (ref->bdr_ref_mod != 1)
			return (EINVAL);
		size = sizeof(*extent) +
		    offsetof(struct btrfs_extent_inline_ref, offset) +
		    sizeof(*data_ref);
		payload = malloc(size, M_BTRFS, M_WAITOK | M_ZERO);
		extent = (struct btrfs_extent_item *)payload;
		extent->refs = htole64(1);
		extent->generation = htole64(trans->bt_generation);
		extent->flags = htole64(BTRFS_EXTENT_FLAG_DATA);
		inline_ref = (struct btrfs_extent_inline_ref *)(extent + 1);
		inline_ref->type = BTRFS_EXTENT_DATA_REF_KEY;
		data_ref = (struct btrfs_extent_data_ref *)
		    ((uint8_t *)inline_ref +
		    offsetof(struct btrfs_extent_inline_ref, offset));
		data_ref->root = htole64(ref->bdr_root);
		data_ref->objectid = htole64(ref->bdr_objectid);
		data_ref->offset = htole64(ref->bdr_offset);
		data_ref->count = htole32(1);
		error = btrfs_insert_item(handle, root, &extent_key, payload,
		    size);
		free(payload, M_BTRFS, size);
		if (error == 0)
			error = btrfs_update_free_space(handle, ref->bdr_bytenr,
			    ref->bdr_length, 0);
		return (error);
	}
	if (error != 0)
		return (error);
	error = btrfs_path_item(&path, &found_key, &data, &size);
	if (error != 0)
		goto out;
	if (memcmp(found_key, &extent_key, sizeof(*found_key)) != 0 ||
	    size < sizeof(*extent)) {
		error = EINVAL;
		goto out;
	}
	payload = malloc(size, M_BTRFS, M_WAITOK);
	alloc_size = size;
	memcpy(payload, data, size);
	btrfs_release_path(&path);

	extent = (struct btrfs_extent_item *)payload;
	refs = letoh64(extent->refs);
	if (refs == 0 ||
	    letoh64(extent->flags) != BTRFS_EXTENT_FLAG_DATA ||
	    letoh64(extent->generation) > trans->bt_generation) {
		error = EINVAL;
		goto done;
	}
	remain = size - sizeof(*extent);
	inline_ref = (struct btrfs_extent_inline_ref *)(extent + 1);
	for (i = 0; i < remain; i += step) {
		candidate = (const struct btrfs_extent_inline_ref *)
		    ((const uint8_t *)inline_ref + i);
		if (remain - i <
		    offsetof(struct btrfs_extent_inline_ref, offset)) {
			error = EINVAL;
			goto done;
		}
		if (candidate->type == BTRFS_EXTENT_OWNER_REF_KEY ||
		    candidate->type == BTRFS_SHARED_DATA_REF_KEY) {
			error = EOPNOTSUPP;
			goto done;
		}
		if (candidate->type != BTRFS_EXTENT_DATA_REF_KEY ||
		    remain - i <
		    offsetof(struct btrfs_extent_inline_ref, offset) +
		    sizeof(*candidate_data)) {
			error = EINVAL;
			goto done;
		}
		step = offsetof(struct btrfs_extent_inline_ref, offset) +
		    sizeof(*candidate_data);
		candidate_data = (const struct btrfs_extent_data_ref *)
		    ((const uint8_t *)candidate +
		    offsetof(struct btrfs_extent_inline_ref, offset));
		if (letoh32(candidate_data->count) == 0) {
			error = EINVAL;
			goto done;
		}
		if (letoh64(candidate_data->root) == ref->bdr_root &&
		    letoh64(candidate_data->objectid) ==
		    ref->bdr_objectid &&
		    letoh64(candidate_data->offset) == ref->bdr_offset) {
			if (found_inline) {
				error = EINVAL;
				goto done;
			}
			found_inline = 1;
			count = letoh32(candidate_data->count);
			data_ref = (struct btrfs_extent_data_ref *)
			    ((uint8_t *)inline_ref + i +
			    offsetof(struct btrfs_extent_inline_ref, offset));
			matched_inline = (struct btrfs_extent_inline_ref *)
			    ((uint8_t *)inline_ref + i);
		}
	}

	error = btrfs_find_separate_data_ref(root, ref->bdr_bytenr,
	    ref->bdr_root, ref->bdr_objectid, ref->bdr_offset, &separate_key,
	    &separate_ref);
	if (error == 0) {
		found_separate = 1;
		if (found_inline) {
			error = EINVAL;
			goto done;
		}
		count = letoh32(separate_ref.count);
	} else if (error != ENOENT) {
		goto done;
	}
	if (!found_inline && !found_separate) {
		error = EINVAL;
		goto done;
	}
	delta = ref->bdr_ref_mod < 0 ?
	    (uint64_t)(-(ref->bdr_ref_mod + 1)) + 1 :
	    (uint64_t)ref->bdr_ref_mod;
	if ((ref->bdr_ref_mod < 0 && delta > count) ||
	    (ref->bdr_ref_mod > 0 &&
	    delta > UINT32_MAX - count) ||
	    (ref->bdr_ref_mod < 0 && delta > refs) ||
	    (ref->bdr_ref_mod > 0 &&
	    delta > UINT64_MAX - refs)) {
		error = EINVAL;
		goto done;
	}
	if (ref->bdr_ref_mod < 0) {
		count -= delta;
		refs -= delta;
	} else {
		count += delta;
		refs += delta;
	}

	if (found_inline) {
		if (count == 0) {
			step = offsetof(struct btrfs_extent_inline_ref, offset) +
			    sizeof(*data_ref);
			i = (uint8_t *)matched_inline - payload;
			memmove(payload + i, payload + i + step,
			    size - i - step);
			size -= step;
		} else {
			data_ref->count = htole32(count);
		}
	} else {
		if (count == 0)
			error = btrfs_delete_item(handle, root, &separate_key);
		else {
			separate_ref.count = htole32(count);
			error = btrfs_replace_item(handle, root, &separate_key,
			    &separate_ref, sizeof(separate_ref));
		}
		if (error != 0)
			goto done;
	}

	if (refs == 0 && count != 0) {
		error = EINVAL;
		goto done;
	}
	if (refs == 0) {
		error = btrfs_delete_item(handle, root, &extent_key);
		if (error == 0)
			error = btrfs_delete_data_csums(handle,
			    ref->bdr_bytenr, ref->bdr_length);
		if (error == 0)
			error = btrfs_space_pin(handle, ref->bdr_bytenr,
			    ref->bdr_length);
		if (error == 0)
			error = btrfs_update_free_space(handle, ref->bdr_bytenr,
			    ref->bdr_length, 1);
	} else {
		extent = (struct btrfs_extent_item *)payload;
		extent->refs = htole64(refs);
		error = btrfs_replace_item(handle, root, &extent_key, payload,
		    size);
	}

done:
	if (payload != NULL)
		free(payload, M_BTRFS, alloc_size);
	return (error);
out:
	btrfs_release_path(&path);
	goto done;
}

int
btrfs_run_delayed_data_refs(struct btrfs_trans_handle *handle)
{
	struct btrfs_transaction *trans;
	struct btrfs_delayed_data_ref *ref;
	int error;

	if (handle == NULL || !handle->bth_commit)
		return (EINVAL);
	trans = handle->bth_transaction;
	for (;;) {
		mtx_enter(&trans->bt_lock);
		ref = TAILQ_FIRST(&trans->bt_delayed_data_refs);
		if (ref != NULL)
			TAILQ_REMOVE(&trans->bt_delayed_data_refs, ref,
			    bdr_entry);
		mtx_leave(&trans->bt_lock);
		if (ref == NULL)
			return (0);
		error = btrfs_materialize_data_ref(handle, ref);
		free(ref, M_BTRFS, sizeof(*ref));
		if (error != 0) {
			btrfs_trans_abort(handle, error);
			return (error);
		}
	}
}

int
btrfs_write_ordered_extents(struct btrfs_transaction *trans)
{
	struct btrfs_ordered_extent *ordered, *next, *first;
	struct btrfs_ordered_io io = RBT_INITIALIZER(&io);
	struct btrfs_io_map map;
	struct btrfs_fs *bmp;
	uint8_t *data;
	uint64_t bytenr;
	uint32_t length, sectorsize;
	int error = 0;

	if (trans == NULL)
		return (EINVAL);
	bmp = trans->bt_mount;
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
	TAILQ_FOREACH(ordered, &trans->bt_ordered_extents, boe_entry) {
		if (ordered->boe_written)
			continue;
		if (ordered->boe_data == NULL ||
		    ordered->boe_length != sectorsize ||
		    (ordered->boe_bytenr & (sectorsize - 1)) != 0 ||
		    ordered->boe_bytenr > UINT64_MAX - sectorsize ||
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
	 * and stage bounded runs without changing sector ownership.
	 */
	data = malloc(MAXBSIZE, M_BTRFS, M_WAITOK);
	ordered = RBT_MIN(btrfs_ordered_io, &io);
	while (ordered != NULL) {
		first = ordered;
		bytenr = ordered->boe_bytenr;
		length = 0;
		do {
			memcpy(data + length, ordered->boe_data, sectorsize);
			length += sectorsize;
			next = RBT_NEXT(btrfs_ordered_io, ordered);
			if (next == NULL || length > MAXBSIZE - sectorsize ||
			    next->boe_bytenr != bytenr + length)
				break;
			/* Adjacent logical chunks need not share physical runs. */
			error = btrfs_lookup_fs_logical(bmp, bytenr,
			    length + sectorsize, &map);
			if (error == ENOENT) {
				error = 0;
				break;
			}
			if (error != 0)
				goto out;
			ordered = next;
		} while (1);
		error = btrfs_write_logical(bmp, bytenr, length,
		    BTRFS_BLOCK_GROUP_DATA, data, NULL);
		if (error != 0)
			break;
		for (ordered = first; ordered != next;
		    ordered = RBT_NEXT(btrfs_ordered_io, ordered))
			ordered->boe_written = 1;
	}
out:
	free(data, M_BTRFS, MAXBSIZE);
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

	first->boe_ref->bdr_length = length;
	first->boe_ref = NULL;
	for (ordered = RBT_NEXT(btrfs_ordered_io, first); ordered != end;
	    ordered = RBT_NEXT(btrfs_ordered_io, ordered)) {
		ref = ordered->boe_ref;
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
	TAILQ_FOREACH(ordered, &trans->bt_ordered_extents, boe_entry) {
		KASSERT(ordered->boe_written);
		KASSERT(ordered->boe_length == sectorsize);
		ordered->boe_ref = NULL;
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
		    ref->bdr_root != ordered->boe_treeid ||
		    ref->bdr_objectid != ordered->boe_objectid ||
		    ref->bdr_offset != ordered->boe_file_offset ||
		    ref->bdr_ref_mod != 1)
			return (EINVAL);
		ordered->boe_ref = ref;
	}
	TAILQ_FOREACH(ordered, &trans->bt_ordered_extents, boe_entry)
		if (ordered->boe_ref == NULL)
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
btrfs_delayed_data_refs_finish(struct btrfs_transaction *trans, int committed)
{
	struct btrfs_delayed_data_ref *ref;

	KASSERT(trans->bt_writers == 0);
	KASSERT(!trans->bt_commit_handle);
	if (committed && !TAILQ_EMPTY(&trans->bt_delayed_data_refs))
		return (EBUSY);
	while ((ref = TAILQ_FIRST(&trans->bt_delayed_data_refs)) != NULL) {
		TAILQ_REMOVE(&trans->bt_delayed_data_refs, ref, bdr_entry);
		free(ref, M_BTRFS, sizeof(*ref));
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
		TAILQ_FOREACH(ordered, &trans->bt_ordered_extents, boe_entry) {
			if (!ordered->boe_written) {
				mtx_leave(&trans->bt_lock);
				return (EBUSY);
			}
		}
	}
	while ((ordered = TAILQ_FIRST(&trans->bt_ordered_extents)) != NULL) {
		TAILQ_REMOVE(&trans->bt_ordered_extents, ordered, boe_entry);
		free(ordered->boe_data, M_BTRFS, ordered->boe_length);
		free(ordered, M_BTRFS, sizeof(*ordered));
	}
	mtx_leave(&trans->bt_lock);
	return (0);
}

int
btrfs_read_ordered_sector(struct btrfs_node *node, uint64_t file_offset,
    void *data)
{
	struct btrfs_fs *bmp;
	struct btrfs_ordered_extent *ordered;
	struct btrfs_transaction *trans;
	uint32_t sectorsize;
	int error = ENOENT;

	if (node == NULL || data == NULL)
		return (EINVAL);
	bmp = node->bn_mount;
	sectorsize = letoh32(bmp->bm_super.sectorsize);
	if ((file_offset & (sectorsize - 1)) != 0)
		return (EINVAL);
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
	TAILQ_FOREACH(ordered, &trans->bt_ordered_extents, boe_entry) {
		if (ordered->boe_treeid != node->bn_treeid ||
		    ordered->boe_objectid != node->bn_ino ||
		    ordered->boe_file_offset != file_offset)
			continue;
		if (ordered->boe_data == NULL ||
		    ordered->boe_length != sectorsize) {
			error = EINVAL;
			break;
		}
		memcpy(data, ordered->boe_data, sectorsize);
		error = 0;
		break;
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
int
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
 * the drop. Remove its checksum and payload before returning the allocation
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
	TAILQ_FOREACH(ordered, &trans->bt_ordered_extents, boe_entry) {
		if (ordered->boe_treeid == node->bn_treeid &&
		    ordered->boe_objectid == node->bn_ino &&
		    ordered->boe_file_offset == extent->bfe_logical)
			break;
	}
	if (ordered == NULL) {
		mtx_leave(&trans->bt_lock);
		return (0);
	}
	if (ordered->boe_bytenr != extent->bfe_disk_bytenr ||
	    ordered->boe_length != extent->bfe_disk_num_bytes ||
	    extent->bfe_disk_offset != 0 ||
	    extent->bfe_length != ordered->boe_length) {
		mtx_leave(&trans->bt_lock);
		return (EINVAL);
	}
	TAILQ_REMOVE(&trans->bt_ordered_extents, ordered, boe_entry);
	mtx_leave(&trans->bt_lock);
	error = btrfs_delete_data_csums(handle, ordered->boe_bytenr,
	    ordered->boe_length);
	if (error == 0)
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
 * Partial EOF data is COWed by setattr before the mutation walk.
 */
static int
btrfs_file_shrink(struct btrfs_trans_handle *handle, struct btrfs_node *node,
    uint64_t size, uint64_t *count, uint64_t *tail_offset)
{
	struct btrfs_fs *bmp = node->bn_mount;
	struct btrfs_file_extent extent;
	struct btrfs_file_extent_item item;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	struct btrfs_key key;
	uint64_t cursor, cut, end, left, removed = 0, items = 0;
	uint32_t sectorsize;
	int error;

	KASSERT(VOP_ISLOCKED(node->bn_vnode));
	KASSERT(size < node->bn_inode.bi_size);
	sectorsize = letoh32(bmp->bm_super.sectorsize);
	cut = roundup(size, sectorsize);
	cursor = size & ~((uint64_t)sectorsize - 1);
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
		if (extent.bfe_type != BTRFS_FILE_EXTENT_HOLE) {
			if (extent.bfe_length - left >
			    node->bn_inode.bi_nbytes - removed) {
				error = EINVAL;
				goto out;
			}
			removed += extent.bfe_length - left;
		}
		if (handle == NULL)
			continue;
		memset(&key, 0, sizeof(key));
		key.objectid = htole64(node->bn_ino);
		key.type = BTRFS_EXTENT_DATA_KEY;
		key.offset = htole64(extent.bfe_logical);
		if (left != 0) {
			extent.bfe_length = left;
			btrfs_encode_file_extent(&item, &extent,
			    handle->bth_transaction->bt_generation);
			error = btrfs_replace_item(handle, root, &key, &item,
			    sizeof(item));
		} else {
			error = btrfs_delete_item(handle, root, &key);
			if (error == 0 && extent.bfe_disk_bytenr != 0)
				error = btrfs_delayed_data_ref_add(handle,
				    extent.bfe_disk_bytenr,
				    extent.bfe_disk_num_bytes, node->bn_treeid,
				    node->bn_ino, extent.bfe_logical -
				    extent.bfe_disk_offset, -1);
			if (error == 0 && extent.bfe_disk_bytenr != 0)
				error = btrfs_cancel_ordered_sector(handle,
				    node, &extent);
		}
		if (error != 0)
			goto out;
	}
	if (count != NULL)
		*count = items;
	if (handle != NULL) {
		node->bn_inode.bi_nbytes -= removed;
		node->bn_inode.bi_dirty_fields |= BTRFS_INODE_DIRTY_NBYTES;
	}
	error = 0;
out:
	btrfs_release_path(&path);
	return (error);
}

int
btrfs_check_file_shrink(struct btrfs_node *node, uint64_t size,
    uint64_t *count, uint64_t *tail_offset)
{
	return (btrfs_file_shrink(NULL, node, size, count, tail_offset));
}

int
btrfs_shrink_file(struct btrfs_trans_handle *handle, struct btrfs_node *node,
    uint64_t size)
{
	return (btrfs_file_shrink(handle, node, size, NULL, NULL));
}

int
btrfs_write_file_sector(struct btrfs_trans_handle *handle,
    struct btrfs_node *node, uint64_t file_offset, const void *data,
    uint64_t file_size)
{
	struct btrfs_transaction *trans;
	struct btrfs_ordered_extent *ordered, *new_ordered = NULL;
	struct btrfs_file_extent first, old, piece, replacement;
	struct btrfs_file_extent_item item;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	struct btrfs_key key;
	uint8_t *inline_data;
	uint64_t bytenr, end, inline_bytes = 0, left, lookup_size, old_end;
	uint64_t ref_offset, right;
	uint32_t csum, sectorsize;
	int error, nodatasum, old_ref_mod, old_refs;

	if (handle == NULL || handle->bth_transaction == NULL ||
	    node == NULL || node->bn_vnode == NULL || data == NULL)
		return (EINVAL);
	KASSERT(VOP_ISLOCKED(node->bn_vnode));
	trans = handle->bth_transaction;
	sectorsize = letoh32(trans->bt_mount->bm_super.sectorsize);
	if (node->bn_mount != trans->bt_mount ||
	    node->bn_inode.bi_last_dirty_transid > trans->bt_generation ||
	    (file_offset & (sectorsize - 1)) != 0 ||
	    file_offset > UINT64_MAX - sectorsize ||
	    file_size <= file_offset || file_size < node->bn_inode.bi_size ||
	    file_size > MAX(node->bn_inode.bi_size,
	    file_offset + sectorsize))
		return (EINVAL);
	nodatasum = (node->bn_inode.bi_flags & BTRFS_INODE_NODATASUM) != 0;
	end = file_offset + sectorsize;
	csum = nodatasum ? 0 : crc32c(0, data, sectorsize);

	mtx_enter(&trans->bt_lock);
	TAILQ_FOREACH(ordered, &trans->bt_ordered_extents, boe_entry) {
		if (ordered->boe_treeid == node->bn_treeid &&
		    ordered->boe_objectid == node->bn_ino &&
		    ordered->boe_file_offset == file_offset)
			break;
	}
	mtx_leave(&trans->bt_lock);
	if (ordered != NULL) {
		if (!nodatasum) {
			error = btrfs_set_data_csum(handle, ordered->boe_bytenr,
			    csum);
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
		if (node->bn_inode.bi_dirty_fields != 0) {
			node->bn_inode.bi_last_dirty_transid =
			    trans->bt_generation;
			error = btrfs_write_inode(handle, node);
			if (error != 0)
				goto abort;
		}
		mtx_enter(&trans->bt_lock);
		memcpy(ordered->boe_data, data, sectorsize);
		mtx_leave(&trans->bt_lock);
		return (0);
	}

	/* Synthetic holes must cover complete sectors for range replacement. */
	lookup_size = roundup(MAX(node->bn_inode.bi_size, end), sectorsize);
	error = btrfs_get_root(node->bn_mount, node->bn_treeid, &root);
	if (error != 0)
		return (error);
	if (file_offset != 0) {
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
				    inline_data, node->bn_inode.bi_size);
			free(inline_data, M_BTRFS, sectorsize);
			if (error != 0)
				goto out;
			error = btrfs_write_file_sector(handle, node,
			    file_offset, data, file_size);
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
		error = btrfs_check_inline_conversion(node, &old);
		if (error != 0)
			return (error);
		inline_bytes = old.bfe_length;
		/* The caller's sector includes all decoded inline data. */
		old.bfe_length = sectorsize;
		old.bfe_compression = BTRFS_COMPRESS_NONE;
	}
	old_end = old.bfe_logical + old.bfe_length;
	if (old.bfe_logical > file_offset || old_end < end ||
	    (old.bfe_logical & (sectorsize - 1)) != 0 ||
	    (old.bfe_length & (sectorsize - 1)) != 0 ||
	    old.bfe_encryption != 0 || old.bfe_other_encoding != 0 ||
	    (old.bfe_compression != BTRFS_COMPRESS_NONE &&
	    old.bfe_compression != BTRFS_COMPRESS_ZSTD))
		return (EOPNOTSUPP);
	if (old.bfe_type != BTRFS_FILE_EXTENT_REG &&
	    old.bfe_type != BTRFS_FILE_EXTENT_PREALLOC &&
	    old.bfe_type != BTRFS_FILE_EXTENT_HOLE && inline_bytes == 0)
		return (EINVAL);
	if (old.bfe_type != BTRFS_FILE_EXTENT_HOLE && inline_bytes == 0 &&
	    (old.bfe_disk_bytenr == 0 ||
	    old.bfe_disk_num_bytes == 0))
		return (EINVAL);
	if (old.bfe_type == BTRFS_FILE_EXTENT_HOLE &&
	    node->bn_inode.bi_nbytes > UINT64_MAX - sectorsize)
		return (EOVERFLOW);

	new_ordered = malloc(sizeof(*new_ordered), M_BTRFS,
	    M_WAITOK | M_ZERO);
	new_ordered->boe_data = malloc(sectorsize, M_BTRFS, M_WAITOK);
	memcpy(new_ordered->boe_data, data, sectorsize);
	error = btrfs_space_alloc(handle, BTRFS_BLOCK_GROUP_DATA,
	    sectorsize, sectorsize, &bytenr);
	if (error != 0)
		goto out;
	new_ordered->boe_treeid = node->bn_treeid;
	new_ordered->boe_objectid = node->bn_ino;
	new_ordered->boe_file_offset = file_offset;
	new_ordered->boe_bytenr = bytenr;
	new_ordered->boe_length = sectorsize;

	if (!nodatasum) {
		error = btrfs_set_data_csum(handle, bytenr, csum);
		if (error != 0)
			goto abort;
	}

	left = file_offset - old.bfe_logical;
	right = old_end - end;
	if (old.bfe_item_present) {
		memset(&key, 0, sizeof(key));
		key.objectid = htole64(node->bn_ino);
		key.type = BTRFS_EXTENT_DATA_KEY;
		key.offset = htole64(old.bfe_logical);
		if (left != 0) {
			memcpy(&piece, &old, sizeof(piece));
			piece.bfe_length = left;
			btrfs_encode_file_extent(&item, &piece,
			    trans->bt_generation);
			error = btrfs_replace_item(handle, root, &key, &item,
			    sizeof(item));
		} else {
			error = btrfs_delete_item(handle, root, &key);
		}
		if (error != 0)
			goto abort;
		if (right != 0) {
			memcpy(&piece, &old, sizeof(piece));
			piece.bfe_logical = end;
			piece.bfe_disk_offset += end - old.bfe_logical;
			piece.bfe_length = right;
			key.offset = htole64(end);
			btrfs_encode_file_extent(&item, &piece,
			    trans->bt_generation);
			error = btrfs_insert_item(handle, root, &key, &item,
			    sizeof(item));
			if (error != 0)
				goto abort;
		}
	}

	memset(&replacement, 0, sizeof(replacement));
	replacement.bfe_logical = file_offset;
	replacement.bfe_length = sectorsize;
	replacement.bfe_disk_bytenr = bytenr;
	replacement.bfe_disk_num_bytes = sectorsize;
	replacement.bfe_ram_bytes = sectorsize;
	replacement.bfe_type = BTRFS_FILE_EXTENT_REG;
	memset(&key, 0, sizeof(key));
	key.objectid = htole64(node->bn_ino);
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = htole64(file_offset);
	btrfs_encode_file_extent(&item, &replacement, trans->bt_generation);
	error = btrfs_insert_item(handle, root, &key, &item, sizeof(item));
	if (error != 0)
		goto abort;

	error = btrfs_delayed_data_ref_add(handle, bytenr, sectorsize,
	    node->bn_treeid, node->bn_ino, file_offset, 1);
	if (error != 0)
		goto abort;
	if (old.bfe_type != BTRFS_FILE_EXTENT_HOLE && inline_bytes == 0) {
		old_refs = (left != 0) + (right != 0);
		old_ref_mod = old_refs - 1;
		ref_offset = old.bfe_logical - old.bfe_disk_offset;
		if (old_ref_mod != 0) {
			error = btrfs_delayed_data_ref_add(handle,
			    old.bfe_disk_bytenr, old.bfe_disk_num_bytes,
			    node->bn_treeid, node->bn_ino, ref_offset,
			    old_ref_mod);
			if (error != 0)
				goto abort;
		}
	}

	mtx_enter(&trans->bt_lock);
	TAILQ_INSERT_TAIL(&trans->bt_ordered_extents, new_ordered,
	    boe_entry);
	mtx_leave(&trans->bt_lock);
	new_ordered = NULL;
	if (old.bfe_type == BTRFS_FILE_EXTENT_HOLE || inline_bytes != 0) {
		node->bn_inode.bi_nbytes += sectorsize - inline_bytes;
		node->bn_inode.bi_dirty_fields |= BTRFS_INODE_DIRTY_NBYTES;
	}
	if (file_size > node->bn_inode.bi_size) {
		node->bn_inode.bi_size = file_size;
		node->bn_inode.bi_dirty_fields |= BTRFS_INODE_DIRTY_SIZE;
	}
	node->bn_inode.bi_last_dirty_transid = trans->bt_generation;
	error = btrfs_write_inode(handle, node);
	if (error != 0)
		goto abort;
	return (0);

abort:
	btrfs_trans_abort(handle, error);
out:
	btrfs_release_path(&path);
	if (new_ordered != NULL) {
		free(new_ordered->boe_data, M_BTRFS, sectorsize);
		free(new_ordered, M_BTRFS, sizeof(*new_ordered));
	}
	return (error);
}
