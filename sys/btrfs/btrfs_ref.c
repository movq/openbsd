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
 * Reference decoding is shared with the disk reader, whose legacy and quota
 * format support is broader than write eligibility. Writers explicitly name
 * an implicit tree/file owner or a shared parent block; full-backreference
 * conversion is a separate metadata operation.
 *
 * Delayed metadata and data references have separate indexes keyed by extent
 * and complete ownership identity. Work queues keep adds and conversions ahead
 * of drops, including after a data delta changes sign. Cancellation removes
 * both index and queue entries, and coalescing rekeys the enlarged allocation.
 * Only a final drop pins space and updates free-space records; data first
 * removes its checksum range while preserving neighboring checksums. Hard
 * links change inode references, not data ownership (root, inode, file-base).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/malloc.h>

#include <lib/libkern/crc32c.h>

#include <btrfs/btrfs_var.h>

/*
 * An edit owns a private extent-item copy, never a tree path. Reference edits
 * can COW the extent tree and queue more metadata references themselves.
 */
struct btrfs_ref_edit {
	struct btrfs_root *root;
	struct btrfs_key key;
	uint8_t *payload;
	uint32_t size;
	uint32_t allocsize;
	uint64_t bytenr;
	uint64_t length;
	uint64_t flags;
	uint64_t refs;
};

static int
btrfs_ref_owner_compare(const struct btrfs_ref_owner *a,
    const struct btrfs_ref_owner *b)
{
	if (a->kind != b->kind)
		return (a->kind < b->kind ? -1 : 1);
	if (a->kind == BTRFS_REF_SHARED) {
		if (a->u.parent != b->u.parent)
			return (a->u.parent < b->u.parent ? -1 : 1);
		return (0);
	}
	if (a->u.implicit.root != b->u.implicit.root)
		return (a->u.implicit.root < b->u.implicit.root ? -1 : 1);
	if (a->u.implicit.objectid != b->u.implicit.objectid)
		return (a->u.implicit.objectid < b->u.implicit.objectid ? -1 : 1);
	if (a->u.implicit.offset != b->u.implicit.offset)
		return (a->u.implicit.offset < b->u.implicit.offset ? -1 : 1);
	return (0);
}

static inline int
btrfs_tree_ref_compare(const struct btrfs_delayed_tree_ref *a,
    const struct btrfs_delayed_tree_ref *b)
{
	int cmp;

	if (a->bdr_bytenr != b->bdr_bytenr)
		return (a->bdr_bytenr < b->bdr_bytenr ? -1 : 1);
	if (a->bdr_operation != b->bdr_operation)
		return (a->bdr_operation < b->bdr_operation ? -1 : 1);
	cmp = btrfs_ref_owner_compare(&a->bdr_owner, &b->bdr_owner);
	if (cmp != 0)
		return (cmp);
	return ((int)a->bdr_level - (int)b->bdr_level);
}

static inline int
btrfs_data_ref_compare(const struct btrfs_delayed_data_ref *a,
    const struct btrfs_delayed_data_ref *b)
{
	if (a->bdr_bytenr != b->bdr_bytenr)
		return (a->bdr_bytenr < b->bdr_bytenr ? -1 : 1);
	if (a->bdr_length != b->bdr_length)
		return (a->bdr_length < b->bdr_length ? -1 : 1);
	return (btrfs_ref_owner_compare(&a->bdr_owner, &b->bdr_owner));
}

RBT_GENERATE(btrfs_tree_ref_tree, btrfs_delayed_tree_ref, bdr_index,
    btrfs_tree_ref_compare);
RBT_GENERATE(btrfs_data_ref_tree, btrfs_delayed_data_ref, bdr_index,
    btrfs_data_ref_compare);

/* Decode a separate reference, or the body of an inline reference. */
int
btrfs_ref_decode(uint8_t type, uint64_t key_offset, const uint8_t *data,
    uint32_t size, struct btrfs_ref_decoded *ref)
{
	const struct btrfs_extent_data_ref *dr;
	const struct btrfs_shared_data_ref *sr;
	const struct btrfs_extent_ref_v0 *v0;

	memset(ref, 0, sizeof(*ref));
	ref->type = type;
	ref->size = size;
	switch (type) {
	case BTRFS_TREE_BLOCK_REF_KEY:
	case BTRFS_SHARED_BLOCK_REF_KEY:
		if (size != 0)
			return (EINVAL);
		ref->owner = type == BTRFS_TREE_BLOCK_REF_KEY ?
		    btrfs_ref_tree(key_offset) : btrfs_ref_shared(key_offset);
		ref->count = 1;
		break;
	case BTRFS_EXTENT_DATA_REF_KEY:
		if (size != sizeof(*dr))
			return (EINVAL);
		dr = (const struct btrfs_extent_data_ref *)data;
		ref->owner = btrfs_ref_data(letoh64(dr->root),
		    letoh64(dr->objectid), letoh64(dr->offset));
		ref->count = letoh32(dr->count);
		ref->count_offset = offsetof(struct btrfs_extent_data_ref, count);
		break;
	case BTRFS_SHARED_DATA_REF_KEY:
		if (size != sizeof(*sr))
			return (EINVAL);
		sr = (const struct btrfs_shared_data_ref *)data;
		ref->owner = btrfs_ref_shared(key_offset);
		ref->count = letoh32(sr->count);
		ref->count_offset = offsetof(struct btrfs_shared_data_ref, count);
		break;
	case BTRFS_EXTENT_REF_V0_KEY:
		if (size != sizeof(*v0))
			return (EINVAL);
		v0 = (const struct btrfs_extent_ref_v0 *)data;
		ref->owner = btrfs_ref_data(letoh64(v0->root),
		    letoh64(v0->objectid), 0);
		ref->generation = letoh64(v0->generation);
		ref->count = letoh32(v0->count);
		ref->count_offset = offsetof(struct btrfs_extent_ref_v0, count);
		break;
	default:
		return (EINVAL);
	}
	if (ref->count == 0 ||
	    (ref->owner.kind == BTRFS_REF_SHARED ?
	    ref->owner.u.parent == 0 : ref->owner.u.implicit.root == 0))
		return (EINVAL);
	return (0);
}

int
btrfs_ref_inline(const uint8_t *data, uint32_t size, uint32_t position,
    uint64_t flags, struct btrfs_ref_decoded *ref)
{
	const struct btrfs_extent_inline_ref *ir;
	uint64_t offset = 0;
	uint32_t header, body = 0, remain;
	int error;

	header = offsetof(struct btrfs_extent_inline_ref, offset);
	if (position > size || size - position < header)
		return (EINVAL);
	remain = size - position;
	ir = (const struct btrfs_extent_inline_ref *)(data + position);
	switch (ir->type) {
	case BTRFS_TREE_BLOCK_REF_KEY:
	case BTRFS_SHARED_BLOCK_REF_KEY:
		if (!(flags & BTRFS_EXTENT_FLAG_TREE_BLOCK))
			return (EINVAL);
		header = sizeof(*ir);
		break;
	case BTRFS_EXTENT_DATA_REF_KEY:
		if (!(flags & BTRFS_EXTENT_FLAG_DATA))
			return (EINVAL);
		body = sizeof(struct btrfs_extent_data_ref);
		break;
	case BTRFS_SHARED_DATA_REF_KEY:
		if (!(flags & BTRFS_EXTENT_FLAG_DATA))
			return (EINVAL);
		header = sizeof(*ir);
		body = sizeof(struct btrfs_shared_data_ref);
		break;
	default:
		return (EINVAL);
	}
	if (header > remain || body > remain - header)
		return (EINVAL);
	if (header == sizeof(*ir))
		offset = letoh64(ir->offset);
	error = btrfs_ref_decode(ir->type, offset, data + position + header,
	    body, ref);
	if (error != 0)
		return (error);
	ref->position = position;
	ref->size = header + body;
	ref->count_offset += position + header;
	return (0);
}

/* Writable extent headers: data or skinny metadata, with no legacy body. */
static int
btrfs_ref_header(const uint8_t *data, uint32_t size, uint64_t generation,
    int metadata, uint64_t *refs, uint64_t *flags)
{
	const struct btrfs_extent_item *extent = (const void *)data;
	uint64_t expected = metadata ? BTRFS_EXTENT_FLAG_TREE_BLOCK :
	    BTRFS_EXTENT_FLAG_DATA;

	if (size < sizeof(*extent))
		return (EINVAL);
	*refs = letoh64(extent->refs);
	*flags = letoh64(extent->flags);
	if (*refs == 0 || letoh64(extent->generation) == 0 ||
	    letoh64(extent->generation) > generation ||
	    (*flags & ~(metadata ? BTRFS_BLOCK_FLAG_FULL_BACKREF : 0)) !=
	    expected)
		return (EINVAL);
	return (0);
}

static int
btrfs_ref_load(struct btrfs_trans_handle *handle, struct btrfs_ref_edit *edit,
    uint64_t bytenr, uint64_t length, uint8_t level, int metadata)
{
	struct btrfs_transaction *trans = handle->bth_transaction;
	struct btrfs_path path = { 0 };
	const struct btrfs_key *key;
	const uint8_t *data;
	uint32_t size;
	int error;

	memset(edit, 0, sizeof(*edit));
	edit->bytenr = bytenr;
	edit->length = length;
	edit->flags = metadata ? BTRFS_EXTENT_FLAG_TREE_BLOCK :
	    BTRFS_EXTENT_FLAG_DATA;
	edit->key.objectid = htole64(bytenr);
	edit->key.type = metadata ? BTRFS_METADATA_ITEM_KEY :
	    BTRFS_EXTENT_ITEM_KEY;
	edit->key.offset = htole64(metadata ? level : length);
	if (metadata && !(letoh64(trans->bt_mount->bm_super.incompat_flags) &
	    BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA))
		return (EOPNOTSUPP);
	error = btrfs_get_root(trans->bt_mount, BTRFS_EXTENT_TREE_OBJECTID,
	    &edit->root);
	if (error != 0)
		return (error == ENOENT ? EINVAL : error);
	error = btrfs_search_slot(edit->root, &edit->key, &path);
	if (error == 0)
		error = btrfs_path_item(&path, &key, &data, &size);
	if (error != 0)
		goto out;
	if (memcmp(key, &edit->key, sizeof(*key)) != 0) {
		error = EINVAL;
		goto out;
	}
	error = btrfs_ref_header(data, size, trans->bt_generation, metadata,
	    &edit->refs, &edit->flags);
	if (error != 0)
		goto out;
	edit->size = edit->allocsize = size;
	edit->payload = malloc(size, M_BTRFS, M_WAITOK);
	memcpy(edit->payload, data, size);
out:
	btrfs_release_path(&path);
	return (error);
}

static uint8_t
btrfs_ref_type(const struct btrfs_ref_owner *owner, int metadata)
{
	if (metadata)
		return (owner->kind == BTRFS_REF_SHARED ?
		    BTRFS_SHARED_BLOCK_REF_KEY : BTRFS_TREE_BLOCK_REF_KEY);
	return (owner->kind == BTRFS_REF_SHARED ?
	    BTRFS_SHARED_DATA_REF_KEY : BTRFS_EXTENT_DATA_REF_KEY);
}

static uint32_t
btrfs_ref_encode(uint8_t *data, uint8_t type,
    const struct btrfs_ref_owner *owner, uint32_t count)
{
	struct btrfs_extent_data_ref dr;
	struct btrfs_shared_data_ref sr;

	if (type == BTRFS_EXTENT_DATA_REF_KEY) {
		dr.root = htole64(owner->u.implicit.root);
		dr.objectid = htole64(owner->u.implicit.objectid);
		dr.offset = htole64(owner->u.implicit.offset);
		dr.count = htole32(count);
		memcpy(data, &dr, sizeof(dr));
		return (sizeof(dr));
	}
	if (type == BTRFS_SHARED_DATA_REF_KEY) {
		sr.count = htole32(count);
		memcpy(data, &sr, sizeof(sr));
		return (sizeof(sr));
	}
	return (0);
}

static int
btrfs_ref_create(struct btrfs_trans_handle *handle,
    struct btrfs_ref_edit *edit, const struct btrfs_ref_owner *owner,
    uint8_t type)
{
	uint8_t payload[sizeof(struct btrfs_extent_item) +
	    sizeof(struct btrfs_extent_inline_ref) +
	    sizeof(struct btrfs_extent_data_ref)] = { 0 };
	struct btrfs_extent_item *extent = (void *)payload;
	struct btrfs_extent_inline_ref *ir = (void *)(extent + 1);
	uint32_t size;
	int error;

	extent->refs = htole64(1);
	extent->generation = htole64(handle->bth_transaction->bt_generation);
	extent->flags = htole64(edit->flags);
	ir->type = type;
	size = sizeof(*extent);
	if (type == BTRFS_EXTENT_DATA_REF_KEY)
		size += offsetof(struct btrfs_extent_inline_ref, offset);
	else {
		ir->offset = htole64(owner->kind == BTRFS_REF_SHARED ?
		    owner->u.parent : owner->u.implicit.root);
		size += sizeof(*ir);
	}
	size += btrfs_ref_encode(payload + size, type, owner, 1);
	error = btrfs_insert_item(handle, edit->root, &edit->key, payload, size);
	if (error == 0)
		error = btrfs_update_free_space(handle, edit->bytenr,
		    edit->length, 0);
	return (error);
}

/*
 * Walk the entire inline body, including after a match. Return an item
 * location, so callers need not know any inline encoding sizes or offsets.
 * A NULL owner validates the body for full-backreference conversion.
 */
static int
btrfs_ref_find_inline(struct btrfs_ref_edit *edit,
    const struct btrfs_ref_owner *owner, struct btrfs_ref_decoded *match)
{
	struct btrfs_ref_decoded ref;
	uint64_t total = 0;
	uint32_t pos;
	int error;

	memset(match, 0, sizeof(*match));
	for (pos = sizeof(struct btrfs_extent_item); pos < edit->size;
	    pos += ref.size) {
		if (edit->payload[pos] == BTRFS_EXTENT_OWNER_REF_KEY)
			return (EOPNOTSUPP);
		error = btrfs_ref_inline(edit->payload, edit->size, pos,
		    edit->flags, &ref);
		if (error != 0)
			return (error);
		if (ref.count > edit->refs - total)
			return (EINVAL);
		total += ref.count;
		if (owner == NULL || btrfs_ref_owner_compare(owner, &ref.owner))
			continue;
		if (match->size != 0)
			return (EINVAL);
		*match = ref;
	}
	return (0);
}

/*
 * Implicit data keys are hashes, with linear collision probing on insertion.
 * Search the whole type range: imported references can have displaced keys.
 * Other owners have exact keys. Always check separate refs even for an inline
 * match, so the same identity cannot be edited in two locations.
 */
static int
btrfs_ref_find_separate(struct btrfs_ref_edit *edit,
    const struct btrfs_ref_owner *owner, uint8_t type,
    struct btrfs_key *result, uint32_t *count)
{
	struct btrfs_path path = { 0 };
	struct btrfs_key target = { 0 };
	struct btrfs_ref_decoded ref;
	const struct btrfs_key *key;
	const uint8_t *data;
	uint32_t size;
	int error, hashed = type == BTRFS_EXTENT_DATA_REF_KEY;

	*count = 0;
	target.objectid = edit->key.objectid;
	target.type = type;
	if (!hashed)
		target.offset = htole64(owner->kind == BTRFS_REF_SHARED ?
		    owner->u.parent : owner->u.implicit.root);
	*result = target;
	error = hashed ? btrfs_search_lower_bound(edit->root, &target, &path) :
	    btrfs_search_slot(edit->root, &target, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &key, &data, &size);
		if (error != 0)
			break;
		if (key->objectid != target.objectid || key->type != type) {
			error = ENOENT;
			break;
		}
		error = btrfs_ref_decode(type, letoh64(key->offset), data,
		    size, &ref);
		if (error != 0)
			break;
		if (btrfs_ref_owner_compare(owner, &ref.owner) == 0) {
			if (*count != 0) {
				error = EINVAL;
				break;
			}
			*result = *key;
			*count = ref.count;
		}
		if (!hashed)
			break;
		error = btrfs_next_item(&path);
	}
	btrfs_release_path(&path);
	return (error == ENOENT ? 0 : error);
}

static int
btrfs_ref_data_key(struct btrfs_root *root,
    const struct btrfs_ref_owner *owner, struct btrfs_key *key)
{
	struct btrfs_path path = { 0 };
	uint64_t hash, le_root, le_ino, le_offset;
	uint32_t high, low;
	int error;

	le_root = htole64(owner->u.implicit.root);
	le_ino = htole64(owner->u.implicit.objectid);
	le_offset = htole64(owner->u.implicit.offset);
	high = crc32c(0, (uint8_t *)&le_root, sizeof(le_root)) ^ 0xffffffffU;
	low = crc32c(0, (uint8_t *)&le_ino, sizeof(le_ino));
	low = crc32c(low, (uint8_t *)&le_offset, sizeof(le_offset)) ^ 0xffffffffU;
	hash = ((uint64_t)high << 31) ^ low;
	for (;;) {
		key->offset = htole64(hash);
		error = btrfs_search_slot(root, key, &path);
		btrfs_release_path(&path);
		if (error != 0)
			return (error == ENOENT ? 0 : error);
		if (++hash == 0)
			return (EOVERFLOW);
	}
}

static int
btrfs_ref_counts(uint64_t *refs, uint32_t *count, int64_t delta,
    int metadata)
{
	uint64_t magnitude = delta < 0 ? (uint64_t)(-(delta + 1)) + 1 : delta;
	uint32_t limit = metadata ? 1 : UINT32_MAX;

	if (*count > *refs || *count > limit ||
	    (delta < 0 && (magnitude > *count || magnitude > *refs)) ||
	    (delta > 0 && (magnitude > limit - *count ||
	    magnitude > UINT64_MAX - *refs)))
		return (EINVAL);
	if (delta < 0) {
		*count -= magnitude;
		*refs -= magnitude;
	} else {
		*count += magnitude;
		*refs += magnitude;
	}
	return (0);
}

/* Only the final reference retires the allocation and its data checksums. */
static int
btrfs_ref_save(struct btrfs_trans_handle *handle, struct btrfs_ref_edit *edit)
{
	struct btrfs_extent_item *extent = (void *)edit->payload;
	int error;

	if (edit->refs != 0) {
		extent->refs = htole64(edit->refs);
		extent->flags = htole64(edit->flags);
		return (btrfs_replace_item(handle, edit->root, &edit->key,
		    edit->payload, edit->size));
	}
	if (edit->size != sizeof(*extent))
		return (EINVAL);
	error = btrfs_delete_item(handle, edit->root, &edit->key);
	if (error == 0 && (edit->flags & BTRFS_EXTENT_FLAG_DATA))
		error = btrfs_delete_data_csums(handle, edit->bytenr, edit->length);
	if (error == 0)
		error = btrfs_space_pin(handle, edit->bytenr, edit->length);
	if (error == 0)
		error = btrfs_update_free_space(handle, edit->bytenr,
		    edit->length, 1);
	return (error);
}

static int
btrfs_ref_change(struct btrfs_trans_handle *handle,
    struct btrfs_ref_edit *edit, const struct btrfs_ref_owner *owner,
    int64_t delta, int metadata)
{
	struct btrfs_ref_decoded match;
	struct btrfs_key key;
	uint8_t body[sizeof(struct btrfs_extent_data_ref)];
	uint8_t type = btrfs_ref_type(owner, metadata);
	uint32_t count, separate, size, le_count;
	int error;

	error = btrfs_ref_find_inline(edit, owner, &match);
	if (error != 0)
		return (error);
	error = btrfs_ref_find_separate(edit, owner, type, &key, &separate);
	if (error != 0)
		return (error);
	if (match.size != 0 && separate != 0)
		return (EINVAL);
	count = match.size != 0 ? match.count : separate;
	error = btrfs_ref_counts(&edit->refs, &count, delta, metadata);
	if (error != 0)
		return (error);
	if (match.size != 0) {
		if (count == 0) {
			memmove(edit->payload + match.position,
			    edit->payload + match.position + match.size,
			    edit->size - match.position - match.size);
			edit->size -= match.size;
		} else {
			KASSERT(!metadata);
			le_count = htole32(count);
			memcpy(edit->payload + match.count_offset, &le_count,
			    sizeof(le_count));
		}
	} else {
		if (separate == 0 && type == BTRFS_EXTENT_DATA_REF_KEY) {
			error = btrfs_ref_data_key(edit->root, owner, &key);
			if (error != 0)
				return (error);
		}
		size = btrfs_ref_encode(body, type, owner, count);
		if (count == 0)
			error = btrfs_delete_item(handle, edit->root, &key);
		else if (separate != 0)
			error = btrfs_replace_item(handle, edit->root, &key,
			    body, size);
		else
			error = btrfs_insert_item(handle, edit->root, &key,
			    body, size);
		if (error != 0)
			return (error);
	}
	return (btrfs_ref_save(handle, edit));
}

static int
btrfs_materialize_data_ref(struct btrfs_trans_handle *handle,
    const struct btrfs_delayed_data_ref *ref)
{
	struct btrfs_ref_edit edit;
	int error;

	error = btrfs_ref_load(handle, &edit, ref->bdr_bytenr,
	    ref->bdr_length, 0, 0);
	if (error == ENOENT) {
		/* A new allocation starts with one implicit file owner. */
		if (ref->bdr_ref_mod < 1 ||
		    ref->bdr_owner.kind != BTRFS_REF_IMPLICIT)
			return (EINVAL);
		error = btrfs_ref_create(handle, &edit, &ref->bdr_owner,
		    BTRFS_EXTENT_DATA_REF_KEY);
		if (error == 0 && ref->bdr_ref_mod > 1) {
			error = btrfs_ref_load(handle, &edit, ref->bdr_bytenr,
			    ref->bdr_length, 0, 0);
			if (error == 0)
				error = btrfs_ref_change(handle, &edit,
				    &ref->bdr_owner, ref->bdr_ref_mod - 1, 0);
			free(edit.payload, M_BTRFS, edit.allocsize);
		}
		return (error);
	}
	if (error == 0)
		error = btrfs_ref_change(handle, &edit, &ref->bdr_owner,
		    ref->bdr_ref_mod, 0);
	free(edit.payload, M_BTRFS, edit.allocsize);
	return (error);
}

static int
btrfs_materialize_tree_ref(struct btrfs_trans_handle *handle,
    const struct btrfs_delayed_tree_ref *ref)
{
	struct btrfs_ref_edit edit;
	struct btrfs_ref_decoded match;
	int error;

	if (ref->bdr_ref_mod < -1 || ref->bdr_ref_mod > 1)
		return (EOPNOTSUPP);
	error = btrfs_ref_load(handle, &edit, ref->bdr_bytenr,
	    letoh32(handle->bth_transaction->bt_mount->bm_super.nodesize),
	    ref->bdr_level, 1);
	if (error == ENOENT) {
		if (ref->bdr_operation != BTRFS_TREE_REF_DELTA ||
		    ref->bdr_ref_mod != 1)
			return (EINVAL);
		return (btrfs_ref_create(handle, &edit, &ref->bdr_owner,
		    btrfs_ref_type(&ref->bdr_owner, 1)));
	}
	if (error == 0) {
		if (ref->bdr_operation == BTRFS_TREE_REF_CONVERT_FULL) {
			error = btrfs_ref_find_inline(&edit, NULL, &match);
			if (error == 0) {
				edit.flags |= BTRFS_BLOCK_FLAG_FULL_BACKREF;
				error = btrfs_ref_save(handle, &edit);
			}
		} else
			error = btrfs_ref_change(handle, &edit, &ref->bdr_owner,
			    ref->bdr_ref_mod, 1);
	}
	free(edit.payload, M_BTRFS, edit.allocsize);
	return (error);
}

static int
btrfs_ref_owner_valid(const struct btrfs_ref_owner *owner, uint32_t sectorsize,
    int metadata)
{
	if (owner->kind == BTRFS_REF_SHARED)
		return (owner->u.parent != 0 &&
		    (owner->u.parent & (sectorsize - 1)) == 0);
	if (owner->kind != BTRFS_REF_IMPLICIT || owner->u.implicit.root == 0)
		return (0);
	if (metadata)
		return (owner->u.implicit.objectid == 0 &&
		    owner->u.implicit.offset == 0);
	return (owner->u.implicit.objectid != 0);
}

static int
btrfs_queue_tree_ref(struct btrfs_trans_handle *handle, uint64_t bytenr,
    struct btrfs_ref_owner owner, uint8_t level, int ref_mod,
    enum btrfs_tree_ref_operation operation)
{
	struct btrfs_delayed_tree_ref *ref, *new;
	struct btrfs_transaction *trans;
	uint32_t sectorsize;

	if (handle == NULL || handle->bth_transaction == NULL)
		return (EINVAL);
	trans = handle->bth_transaction;
	sectorsize = letoh32(trans->bt_mount->bm_super.sectorsize);
	if (bytenr == 0 || (bytenr & (sectorsize - 1)) != 0 ||
	    level >= BTRFS_MAX_LEVEL)
		return (EINVAL);
	if (operation == BTRFS_TREE_REF_DELTA &&
	    ((ref_mod != -1 && ref_mod != 1) ||
	    !btrfs_ref_owner_valid(&owner, sectorsize, 1)))
		return (EINVAL);

	new = malloc(sizeof(*new), M_BTRFS, M_WAITOK | M_ZERO);
	new->bdr_bytenr = bytenr;
	new->bdr_owner = owner;
	new->bdr_operation = operation;
	new->bdr_level = level;
	new->bdr_ref_mod = ref_mod;

	mtx_enter(&trans->bt_lock);
	ref = RBT_FIND(btrfs_tree_ref_tree, &trans->bt_tree_ref_index, new);
	if (ref == NULL) {
		RBT_INSERT(btrfs_tree_ref_tree, &trans->bt_tree_ref_index, new);
		if (operation == BTRFS_TREE_REF_CONVERT_FULL || ref_mod > 0)
			TAILQ_INSERT_HEAD(&trans->bt_delayed_tree_refs, new,
			    bdr_entry);
		else
			TAILQ_INSERT_TAIL(&trans->bt_delayed_tree_refs, new,
			    bdr_entry);
		new = NULL;
	} else if (operation == BTRFS_TREE_REF_DELTA) {
		KASSERT(ref->bdr_ref_mod != 0);
		ref->bdr_ref_mod += ref_mod;
		if (ref->bdr_ref_mod == 0) {
			RBT_REMOVE(btrfs_tree_ref_tree,
			    &trans->bt_tree_ref_index, ref);
			TAILQ_REMOVE(&trans->bt_delayed_tree_refs, ref,
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

int
btrfs_delayed_ref_add(struct btrfs_trans_handle *handle, uint64_t bytenr,
    struct btrfs_ref_owner owner, uint8_t level, int ref_mod)
{
	return (btrfs_queue_tree_ref(handle, bytenr, owner, level, ref_mod,
	    BTRFS_TREE_REF_DELTA));
}

/*
 * Callers queue the child ownership changes before marking their parent full.
 * Conversion has its own index identity and cannot cancel an ownership delta.
 */
int
btrfs_ref_convert_full(struct btrfs_trans_handle *handle,
    const struct btrfs_extent_buffer *eb)
{
	struct btrfs_ref_owner unused = { 0 };

	return (btrfs_queue_tree_ref(handle, eb->eb_bytenr, unused,
	    eb->eb_level, 0, BTRFS_TREE_REF_CONVERT_FULL));
}

int
btrfs_block_refs(struct btrfs_trans_handle *handle,
    const struct btrfs_extent_buffer *eb, uint64_t *refs, uint64_t *flags)
{
	struct btrfs_root *root;
	struct btrfs_path path = { 0 };
	struct btrfs_key key = { 0 };
	const uint8_t *data;
	struct btrfs_delayed_tree_ref *ref, probe = { 0 };
	struct btrfs_transaction *trans = handle->bth_transaction;
	uint32_t size;
	int error;

	*refs = *flags = 0;
	error = btrfs_get_root(trans->bt_mount, BTRFS_EXTENT_TREE_OBJECTID,
	    &root);
	if (error != 0)
		return (error);
	key.objectid = htole64(eb->eb_bytenr);
	key.type = BTRFS_METADATA_ITEM_KEY;
	key.offset = htole64(eb->eb_level);
	error = btrfs_search_slot(root, &key, &path);
	if (error == 0) {
		error = btrfs_path_item(&path, NULL, &data, &size);
		if (error == 0)
			error = btrfs_ref_header(data, size, trans->bt_generation,
			    1, refs, flags);
	}
	btrfs_release_path(&path);
	if (error != 0 && error != ENOENT)
		return (error);
	mtx_enter(&trans->bt_lock);
	probe.bdr_bytenr = eb->eb_bytenr;
	for (ref = RBT_NFIND(btrfs_tree_ref_tree, &trans->bt_tree_ref_index,
	    &probe); ref != NULL; ref = RBT_NEXT(btrfs_tree_ref_tree, ref)) {
		if (ref->bdr_bytenr != eb->eb_bytenr)
			break;
		if (ref->bdr_operation == BTRFS_TREE_REF_CONVERT_FULL)
			*flags |= BTRFS_BLOCK_FLAG_FULL_BACKREF;
		else
			*refs += ref->bdr_ref_mod;
	}
	mtx_leave(&trans->bt_lock);
	return (*refs == 0 ? EINVAL : 0);
}

int
btrfs_delayed_data_ref_add(struct btrfs_trans_handle *handle,
    uint64_t bytenr, uint64_t length, struct btrfs_ref_owner owner, int ref_mod)
{
	struct btrfs_delayed_data_ref *ref, *new;
	struct btrfs_transaction *trans;
	uint32_t sectorsize;

	if (handle == NULL || handle->bth_transaction == NULL ||
	    ref_mod == 0)
		return (EINVAL);
	trans = handle->bth_transaction;
	sectorsize = letoh32(trans->bt_mount->bm_super.sectorsize);
	if (bytenr == 0 || length == 0 ||
	    (bytenr & (sectorsize - 1)) != 0 ||
	    (length & (sectorsize - 1)) != 0 ||
	    bytenr > UINT64_MAX - length ||
	    !btrfs_ref_owner_valid(&owner, sectorsize, 0))
		return (EINVAL);

	new = malloc(sizeof(*new), M_BTRFS, M_WAITOK | M_ZERO);
	new->bdr_bytenr = bytenr;
	new->bdr_length = length;
	new->bdr_owner = owner;
	new->bdr_ref_mod = ref_mod;

	mtx_enter(&trans->bt_lock);
	ref = RBT_FIND(btrfs_data_ref_tree, &trans->bt_data_ref_index, new);
	if (ref == NULL) {
		RBT_INSERT(btrfs_data_ref_tree, &trans->bt_data_ref_index, new);
		if (ref_mod > 0)
			TAILQ_INSERT_HEAD(&trans->bt_delayed_data_refs, new,
			    bdr_entry);
		else
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
		TAILQ_REMOVE(&trans->bt_delayed_data_refs, ref, bdr_entry);
		ref->bdr_ref_mod += ref_mod;
		if (ref->bdr_ref_mod == 0) {
			RBT_REMOVE(btrfs_data_ref_tree,
			    &trans->bt_data_ref_index, ref);
			free(ref, M_BTRFS, sizeof(*ref));
		} else if (ref->bdr_ref_mod > 0)
			TAILQ_INSERT_HEAD(&trans->bt_delayed_data_refs, ref,
			    bdr_entry);
		else
			TAILQ_INSERT_TAIL(&trans->bt_delayed_data_refs, ref,
			    bdr_entry);
	}
	mtx_leave(&trans->bt_lock);
	if (new != NULL)
		free(new, M_BTRFS, sizeof(*new));
	handle->bth_delayed = 1;
	return (0);
}

static int
btrfs_run_delayed_data_refs(struct btrfs_trans_handle *handle)
{
	struct btrfs_transaction *trans = handle->bth_transaction;
	struct btrfs_delayed_data_ref *ref;
	int error;

	for (;;) {
		mtx_enter(&trans->bt_lock);
		ref = TAILQ_FIRST(&trans->bt_delayed_data_refs);
		if (ref != NULL) {
			RBT_REMOVE(btrfs_data_ref_tree,
			    &trans->bt_data_ref_index, ref);
			TAILQ_REMOVE(&trans->bt_delayed_data_refs, ref, bdr_entry);
		}
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
btrfs_run_delayed_refs(struct btrfs_trans_handle *handle)
{
	struct btrfs_transaction *trans;
	struct btrfs_delayed_tree_ref *ref;
	int error;

	if (handle == NULL || !handle->bth_commit)
		return (EINVAL);
	trans = handle->bth_transaction;
	for (;;) {
		mtx_enter(&trans->bt_lock);
		/* Materialize adds and conversions before any final drop. */
		ref = TAILQ_FIRST(&trans->bt_delayed_tree_refs);
		if (ref != NULL) {
			RBT_REMOVE(btrfs_tree_ref_tree,
			    &trans->bt_tree_ref_index, ref);
			TAILQ_REMOVE(&trans->bt_delayed_tree_refs, ref, bdr_entry);
		}
		mtx_leave(&trans->bt_lock);
		if (ref == NULL)
			return (btrfs_run_delayed_data_refs(handle));
		error = btrfs_materialize_tree_ref(handle, ref);
		free(ref, M_BTRFS, sizeof(*ref));
		if (error != 0) {
			btrfs_trans_abort(handle, error);
			return (error);
		}
	}
}

int
btrfs_delayed_refs_finish(struct btrfs_transaction *trans, int committed)
{
	struct btrfs_delayed_tree_ref *tree;
	struct btrfs_delayed_data_ref *data;

	KASSERT(trans->bt_writers == 0);
	KASSERT(!trans->bt_commit_handle);
	if (committed && (!TAILQ_EMPTY(&trans->bt_delayed_tree_refs) ||
	    !TAILQ_EMPTY(&trans->bt_delayed_data_refs)))
		return (EBUSY);
	while ((tree = TAILQ_FIRST(&trans->bt_delayed_tree_refs)) != NULL) {
		RBT_REMOVE(btrfs_tree_ref_tree, &trans->bt_tree_ref_index, tree);
		TAILQ_REMOVE(&trans->bt_delayed_tree_refs, tree, bdr_entry);
		free(tree, M_BTRFS, sizeof(*tree));
	}
	while ((data = TAILQ_FIRST(&trans->bt_delayed_data_refs)) != NULL) {
		RBT_REMOVE(btrfs_data_ref_tree, &trans->bt_data_ref_index, data);
		TAILQ_REMOVE(&trans->bt_delayed_data_refs, data, bdr_entry);
		free(data, M_BTRFS, sizeof(*data));
	}
	return (0);
}
