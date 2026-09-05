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
static int	btrfs_delete_data_csums(struct btrfs_trans_handle *, uint64_t,
		    uint64_t);
static void	btrfs_encode_file_extent(struct btrfs_file_extent_item *,
		    const struct btrfs_file_extent *, uint64_t);
static int	btrfs_find_separate_data_ref(struct btrfs_root *, uint64_t,
		    uint64_t, uint64_t, uint64_t, struct btrfs_key *,
		    struct btrfs_extent_data_ref *);
static int	btrfs_materialize_data_ref(struct btrfs_trans_handle *,
		    const struct btrfs_delayed_data_ref *);

static int
btrfs_set_data_csum(struct btrfs_trans_handle *handle, uint64_t logical,
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
	uint32_t disk_csum, item_size, sectorsize;
	size_t index;
	int error;

	sectorsize = letoh32(bmp->bm_super.sectorsize);
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
	item->ram_bytes = htole64(extent->bfe_ram_bytes);
	item->compression = extent->bfe_compression;
	item->encryption = extent->bfe_encryption;
	item->other_encoding = htole16(extent->bfe_other_encoding);
	item->type = extent->bfe_type == BTRFS_FILE_EXTENT_HOLE ?
	    BTRFS_FILE_EXTENT_REG : extent->bfe_type;
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
	struct btrfs_ordered_extent *ordered;
	struct btrfs_fs *bmp;
	uint32_t sectorsize;
	int error;

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
		    (ordered->boe_file_offset & (sectorsize - 1)) != 0 ||
		    ordered->boe_treeid == 0 || ordered->boe_objectid == 0)
			return (EINVAL);
		error = btrfs_write_logical(bmp->bm_devvp, bmp->bm_chunks,
		    bmp->bm_nchunks, ordered->boe_bytenr,
		    ordered->boe_length, BTRFS_BLOCK_GROUP_DATA,
		    ordered->boe_data, NULL);
		if (error != 0)
			return (error);
		ordered->boe_written = 1;
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
	int error, old_ref_mod, old_refs;

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
	if ((node->bn_inode.bi_flags & BTRFS_INODE_NODATASUM) != 0)
		return (EOPNOTSUPP);
	end = file_offset + sectorsize;
	csum = crc32c(0, data, sectorsize);

	mtx_enter(&trans->bt_lock);
	TAILQ_FOREACH(ordered, &trans->bt_ordered_extents, boe_entry) {
		if (ordered->boe_treeid == node->bn_treeid &&
		    ordered->boe_objectid == node->bn_ino &&
		    ordered->boe_file_offset == file_offset)
			break;
	}
	mtx_leave(&trans->bt_lock);
	if (ordered != NULL) {
		error = btrfs_set_data_csum(handle, ordered->boe_bytenr,
		    csum);
		if (error != 0)
			goto abort;
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

	lookup_size = MAX(node->bn_inode.bi_size, end);
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
			if (first.bfe_compression != BTRFS_COMPRESS_NONE ||
			    first.bfe_encryption != 0 ||
			    first.bfe_other_encoding != 0 ||
			    first.bfe_length > sectorsize) {
				error = EOPNOTSUPP;
				goto out;
			}
			if (first.bfe_inline_size != first.bfe_length ||
			    first.bfe_length != node->bn_inode.bi_size ||
			    first.bfe_length != node->bn_inode.bi_nbytes) {
				error = EINVAL;
				goto out;
			}
			inline_data = malloc(sectorsize, M_BTRFS,
			    M_WAITOK | M_ZERO);
			memcpy(inline_data, first.bfe_inline_data,
			    first.bfe_inline_size);
			btrfs_release_path(&path);
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
		if (old.bfe_compression != BTRFS_COMPRESS_NONE ||
		    old.bfe_encryption != 0 || old.bfe_other_encoding != 0 ||
		    old.bfe_length > sectorsize)
			return (EOPNOTSUPP);
		if (old.bfe_inline_size != old.bfe_length ||
		    old.bfe_length != node->bn_inode.bi_size ||
		    old.bfe_length != node->bn_inode.bi_nbytes)
			return (EINVAL);
		inline_bytes = old.bfe_length;
		/* The caller's zero-filled sector includes all inline data. */
		old.bfe_length = sectorsize;
	}
	old_end = old.bfe_logical + old.bfe_length;
	if (old.bfe_logical > file_offset || old_end < end ||
	    (old.bfe_logical & (sectorsize - 1)) != 0 ||
	    (old.bfe_length & (sectorsize - 1)) != 0 ||
	    old.bfe_encryption != 0 || old.bfe_other_encoding != 0 ||
	    old.bfe_compression != BTRFS_COMPRESS_NONE)
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

	error = btrfs_set_data_csum(handle, bytenr, csum);
	if (error != 0)
		goto abort;

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
