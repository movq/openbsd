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

struct btrfs_free_space_state {
	uint64_t	last_end;
	uint32_t	expected;
	uint64_t	found;
	uint32_t	flags;
	uint8_t		have_info;
	uint8_t		last_bit;
};

static int	btrfs_first_item(struct btrfs_root *, struct btrfs_path *);
static int	btrfs_find_chunk(const struct btrfs_fs *, uint64_t,
		    uint64_t, unsigned int *);
static int	btrfs_finish_extent(uint64_t, uint64_t, int);
static int	btrfs_emit_backref(uint64_t, uint8_t, uint64_t,
		    const uint8_t *, size_t, int, uint64_t *,
		    btrfs_backref_iter_fn, void *);
static int	btrfs_read_extent_item(struct btrfs_fs *,
		    const struct btrfs_key *, const uint8_t *, uint32_t,
		    struct btrfs_extent_record *, uint64_t *,
		    btrfs_extent_iter_fn, btrfs_backref_iter_fn, void *);

static int
btrfs_first_item(struct btrfs_root *root, struct btrfs_path *path)
{
	struct btrfs_key key;

	memset(&key, 0, sizeof(key));
	return (btrfs_search_lower_bound(root, &key, path));
}

static int
btrfs_find_chunk(const struct btrfs_fs *bmp, uint64_t bytenr,
    uint64_t length, unsigned int *indexp)
{
	const struct btrfs_chunk_map *chunk;
	unsigned int i;

	if (length == 0 || bytenr > UINT64_MAX - length)
		return (EINVAL);
	for (i = 0; i < bmp->bm_nchunks; i++) {
		chunk = &bmp->bm_chunks[i];
		if (bytenr < chunk->logical)
			break;
		if (bytenr >= chunk->logical &&
		    bytenr - chunk->logical <= chunk->length &&
		    length <= chunk->length - (bytenr - chunk->logical)) {
			*indexp = i;
			return (0);
		}
	}
	return (ENOENT);
}

static int
btrfs_finish_extent(uint64_t expected, uint64_t found, int active)
{
	if (active && expected != found)
		return (EINVAL);
	return (0);
}

static int
btrfs_emit_backref(uint64_t bytenr, uint8_t type, uint64_t key_offset,
    const uint8_t *data, size_t size, int is_inline, uint64_t *refsp,
    btrfs_backref_iter_fn callback, void *arg)
{
	const struct btrfs_extent_data_ref *data_ref;
	const struct btrfs_extent_ref_v0 *ref_v0;
	const struct btrfs_shared_data_ref *shared_data;
	struct btrfs_backref_record record;
	uint64_t count;
	size_t expected;

	memset(&record, 0, sizeof(record));
	record.bbr_bytenr = bytenr;
	record.bbr_type = type;
	record.bbr_inline = is_inline;
	record.bbr_key_offset = key_offset;
	expected = 0;
	switch (type) {
	case BTRFS_TREE_BLOCK_REF_KEY:
		if (size != 0)
			return (EINVAL);
		record.bbr_root = key_offset;
		record.bbr_count = 1;
		break;
	case BTRFS_SHARED_BLOCK_REF_KEY:
		if (size != 0)
			return (EINVAL);
		record.bbr_parent = key_offset;
		record.bbr_count = 1;
		break;
	case BTRFS_EXTENT_DATA_REF_KEY:
		expected = sizeof(*data_ref);
		if (size != expected)
			return (EINVAL);
		data_ref = (const struct btrfs_extent_data_ref *)data;
		record.bbr_root = letoh64(data_ref->root);
		record.bbr_objectid = letoh64(data_ref->objectid);
		record.bbr_offset = letoh64(data_ref->offset);
		record.bbr_count = letoh32(data_ref->count);
		break;
	case BTRFS_SHARED_DATA_REF_KEY:
		expected = sizeof(*shared_data);
		if (size != expected)
			return (EINVAL);
		shared_data = (const struct btrfs_shared_data_ref *)data;
		record.bbr_parent = key_offset;
		record.bbr_count = letoh32(shared_data->count);
		break;
	case BTRFS_EXTENT_REF_V0_KEY:
		expected = sizeof(*ref_v0);
		if (is_inline || size != expected)
			return (EINVAL);
		ref_v0 = (const struct btrfs_extent_ref_v0 *)data;
		record.bbr_root = letoh64(ref_v0->root);
		record.bbr_objectid = letoh64(ref_v0->objectid);
		record.bbr_generation = letoh64(ref_v0->generation);
		record.bbr_count = letoh32(ref_v0->count);
		break;
	default:
		return (EINVAL);
	}

	if (record.bbr_count == 0 ||
	    (record.bbr_root == 0 &&
	    type != BTRFS_SHARED_BLOCK_REF_KEY &&
	    type != BTRFS_SHARED_DATA_REF_KEY) ||
	    ((type == BTRFS_SHARED_BLOCK_REF_KEY ||
	    type == BTRFS_SHARED_DATA_REF_KEY) &&
	    record.bbr_parent == 0))
		return (EINVAL);
	count = record.bbr_count;
	if (*refsp > UINT64_MAX - count)
		return (EINVAL);
	*refsp += count;
	if (callback != NULL)
		return (callback(&record, arg));
	return (0);
}

static int
btrfs_read_extent_item(struct btrfs_fs *bmp,
    const struct btrfs_key *key, const uint8_t *data, uint32_t size,
    struct btrfs_extent_record *record, uint64_t *refsp,
    btrfs_extent_iter_fn extent_callback,
    btrfs_backref_iter_fn backref_callback, void *arg)
{
	const struct btrfs_extent_inline_ref *inline_ref;
	const struct btrfs_extent_item *item;
	const struct btrfs_extent_item_v0 *item_v0;
	const struct btrfs_tree_block_info *tree_info;
	const struct btrfs_chunk_map *chunk;
	const uint8_t *p;
	uint64_t flags, offset;
	uint32_t sectorsize;
	size_t body_size, header_size, remain;
	unsigned int chunk_index;
	int error;

	memset(record, 0, sizeof(*record));
	*refsp = 0;
	record->ber_bytenr = letoh64(key->objectid);
	sectorsize = letoh32(bmp->bm_super.sectorsize);
	if (record->ber_bytenr == 0 ||
	    (record->ber_bytenr & (sectorsize - 1)) != 0)
		return (EINVAL);

	if (size == sizeof(*item_v0)) {
		if (key->type != BTRFS_EXTENT_ITEM_KEY)
			return (EINVAL);
		item_v0 = (const struct btrfs_extent_item_v0 *)data;
		record->ber_length = letoh64(key->offset);
		record->ber_refs = letoh32(item_v0->refs);
		record->ber_legacy = 1;
		if (record->ber_refs == 0 || record->ber_length == 0 ||
		    (record->ber_length & (sectorsize - 1)) != 0)
			return (EINVAL);
		error = btrfs_find_chunk(bmp, record->ber_bytenr,
		    record->ber_length, &chunk_index);
		if (error != 0)
			return (EINVAL);
		if (extent_callback != NULL)
			return (extent_callback(record, arg));
		return (0);
	}

	if (size < sizeof(*item))
		return (EINVAL);
	item = (const struct btrfs_extent_item *)data;
	record->ber_refs = letoh64(item->refs);
	record->ber_generation = letoh64(item->generation);
	record->ber_flags = flags = letoh64(item->flags);
	if (record->ber_refs == 0 || record->ber_generation == 0 ||
	    record->ber_generation > letoh64(bmp->bm_super.generation) ||
	    (flags & ~(BTRFS_EXTENT_FLAG_DATA |
	    BTRFS_EXTENT_FLAG_TREE_BLOCK)) != 0 ||
	    (flags & (BTRFS_EXTENT_FLAG_DATA |
	    BTRFS_EXTENT_FLAG_TREE_BLOCK)) == 0 ||
	    (flags & (BTRFS_EXTENT_FLAG_DATA |
	    BTRFS_EXTENT_FLAG_TREE_BLOCK)) ==
	    (BTRFS_EXTENT_FLAG_DATA | BTRFS_EXTENT_FLAG_TREE_BLOCK))
		return (EINVAL);

	body_size = sizeof(*item);
	if (key->type == BTRFS_METADATA_ITEM_KEY) {
		if ((letoh64(bmp->bm_super.incompat_flags) &
		    BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA) == 0 ||
		    (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK) == 0 ||
		    letoh64(key->offset) >= BTRFS_MAX_LEVEL)
			return (EINVAL);
		record->ber_length = letoh32(bmp->bm_super.nodesize);
		record->ber_level = letoh64(key->offset);
		record->ber_skinny = 1;
	} else if (key->type == BTRFS_EXTENT_ITEM_KEY) {
		record->ber_length = letoh64(key->offset);
		if (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK) {
			if (size < body_size + sizeof(*tree_info))
				return (EINVAL);
			tree_info = (const struct btrfs_tree_block_info *)
			    (data + body_size);
			record->ber_tree_objectid =
			    letoh64(tree_info->key.objectid);
			record->ber_tree_type = tree_info->key.type;
			record->ber_tree_offset =
			    letoh64(tree_info->key.offset);
			record->ber_level = tree_info->level;
			if (record->ber_level >= BTRFS_MAX_LEVEL)
				return (EINVAL);
			body_size += sizeof(*tree_info);
		}
	} else {
		return (EINVAL);
	}

	if (record->ber_length == 0 ||
	    (record->ber_length & (sectorsize - 1)) != 0 ||
	    ((flags & BTRFS_EXTENT_FLAG_TREE_BLOCK) != 0 &&
	    record->ber_length != letoh32(bmp->bm_super.nodesize)))
		return (EINVAL);
	error = btrfs_find_chunk(bmp, record->ber_bytenr,
	    record->ber_length, &chunk_index);
	if (error != 0)
		return (EINVAL);
	chunk = &bmp->bm_chunks[chunk_index];
	if (((flags & BTRFS_EXTENT_FLAG_DATA) != 0 &&
	    (chunk->type & BTRFS_BLOCK_GROUP_DATA) == 0) ||
	    ((flags & BTRFS_EXTENT_FLAG_TREE_BLOCK) != 0 &&
	    (chunk->type & (BTRFS_BLOCK_GROUP_METADATA |
	    BTRFS_BLOCK_GROUP_SYSTEM)) == 0))
		return (EINVAL);

	p = data + body_size;
	remain = size - body_size;
	if (remain >= sizeof(*inline_ref) &&
	    p[0] == BTRFS_EXTENT_OWNER_REF_KEY) {
		inline_ref = (const struct btrfs_extent_inline_ref *)p;
		record->ber_owner_root = letoh64(inline_ref->offset);
		record->ber_has_owner = 1;
		if ((letoh64(bmp->bm_super.incompat_flags) &
		    BTRFS_FEATURE_INCOMPAT_SIMPLE_QUOTA) == 0 ||
		    (flags & BTRFS_EXTENT_FLAG_DATA) == 0 ||
		    record->ber_owner_root == 0)
			return (EINVAL);
		p += sizeof(*inline_ref);
		remain -= sizeof(*inline_ref);
	}

	if (extent_callback != NULL) {
		error = extent_callback(record, arg);
		if (error != 0)
			return (error);
	}

	while (remain != 0) {
		header_size = offsetof(struct btrfs_extent_inline_ref, offset);
		if (remain < header_size)
			return (EINVAL);
		inline_ref = (const struct btrfs_extent_inline_ref *)p;
		body_size = 0;
		switch (inline_ref->type) {
		case BTRFS_TREE_BLOCK_REF_KEY:
		case BTRFS_SHARED_BLOCK_REF_KEY:
			if ((flags & BTRFS_EXTENT_FLAG_TREE_BLOCK) == 0)
				return (EINVAL);
			header_size = sizeof(*inline_ref);
			break;
		case BTRFS_EXTENT_DATA_REF_KEY:
			if ((flags & BTRFS_EXTENT_FLAG_DATA) == 0)
				return (EINVAL);
			body_size = sizeof(struct btrfs_extent_data_ref);
			break;
		case BTRFS_SHARED_DATA_REF_KEY:
			if ((flags & BTRFS_EXTENT_FLAG_DATA) == 0)
				return (EINVAL);
			header_size = sizeof(*inline_ref);
			body_size = sizeof(struct btrfs_shared_data_ref);
			break;
		default:
			return (EINVAL);
		}
		if (header_size > remain || body_size > remain - header_size)
			return (EINVAL);
		offset = header_size == sizeof(*inline_ref) ?
		    letoh64(inline_ref->offset) : 0;
		error = btrfs_emit_backref(record->ber_bytenr,
		    inline_ref->type, offset, p + header_size,
		    body_size, 1, refsp, backref_callback, arg);
		if (error != 0)
			return (error);
		body_size += header_size;
		p += body_size;
		remain -= body_size;
	}
	return (0);
}

int
btrfs_iterate_extent_items(struct btrfs_fs *bmp,
    btrfs_extent_iter_fn extent_callback,
    btrfs_backref_iter_fn backref_callback, void *arg)
{
	const struct btrfs_key *key;
	const uint8_t *data;
	struct btrfs_extent_record extent;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	uint64_t expected = 0, found = 0;
	uint32_t size;
	int active = 0;
	int error;

	error = btrfs_get_root(bmp, BTRFS_EXTENT_TREE_OBJECTID, &root);
	if (error != 0)
		return (error);
	error = btrfs_first_item(root, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &key, &data, &size);
		if (error != 0)
			break;
		if (active && letoh64(key->objectid) != extent.ber_bytenr) {
			error = btrfs_finish_extent(expected, found, active);
			if (error != 0)
				break;
			active = 0;
		}

		switch (key->type) {
		case BTRFS_EXTENT_ITEM_KEY:
		case BTRFS_METADATA_ITEM_KEY:
			if (active) {
				error = EINVAL;
				break;
			}
			error = btrfs_read_extent_item(bmp, key, data, size,
			    &extent, &found, extent_callback,
			    backref_callback, arg);
			if (error == 0) {
				expected = extent.ber_refs;
				active = 1;
			}
			break;
		case BTRFS_TREE_BLOCK_REF_KEY:
		case BTRFS_EXTENT_DATA_REF_KEY:
		case BTRFS_EXTENT_REF_V0_KEY:
		case BTRFS_SHARED_BLOCK_REF_KEY:
		case BTRFS_SHARED_DATA_REF_KEY:
			if (!active ||
			    letoh64(key->objectid) != extent.ber_bytenr) {
				error = EINVAL;
				break;
			}
			if ((key->type == BTRFS_EXTENT_REF_V0_KEY) !=
			    extent.ber_legacy) {
				error = EINVAL;
				break;
			}
			if (((key->type == BTRFS_TREE_BLOCK_REF_KEY ||
			    key->type == BTRFS_SHARED_BLOCK_REF_KEY) &&
			    !extent.ber_legacy &&
			    (extent.ber_flags &
			    BTRFS_EXTENT_FLAG_TREE_BLOCK) == 0) ||
			    ((key->type == BTRFS_EXTENT_DATA_REF_KEY ||
			    key->type == BTRFS_SHARED_DATA_REF_KEY) &&
			    !extent.ber_legacy &&
			    (extent.ber_flags &
			    BTRFS_EXTENT_FLAG_DATA) == 0)) {
				error = EINVAL;
				break;
			}
			error = btrfs_emit_backref(extent.ber_bytenr,
			    key->type, letoh64(key->offset), data, size, 0,
			    &found, backref_callback, arg);
			break;
		case BTRFS_BLOCK_GROUP_ITEM_KEY:
			error = btrfs_finish_extent(expected, found, active);
			active = 0;
			break;
		default:
			error = EINVAL;
			break;
		}
		if (error != 0)
			break;
		error = btrfs_next_item(&path);
	}
	if (error == ENOENT)
		error = btrfs_finish_extent(expected, found, active);
	btrfs_release_path(&path);
	return (error);
}

int
btrfs_iterate_block_groups(struct btrfs_fs *bmp,
    btrfs_block_group_iter_fn callback, void *arg)
{
	const struct btrfs_block_group_item *item;
	const struct btrfs_chunk_map *chunk;
	const struct btrfs_key *key;
	const uint8_t *data;
	struct btrfs_block_group_record record;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	uint8_t *seen = NULL;
	uint64_t owner;
	uint32_t size;
	unsigned int i, index;
	int error;

	owner = BTRFS_EXTENT_TREE_OBJECTID;
	if (letoh64(bmp->bm_super.compat_ro_flags) &
	    BTRFS_FEATURE_COMPAT_RO_BLOCK_GROUP_TREE)
		owner = BTRFS_BLOCK_GROUP_TREE_OBJECTID;
	error = btrfs_get_root(bmp, owner, &root);
	if (error != 0)
		return (error);
	seen = mallocarray(bmp->bm_nchunks, sizeof(*seen), M_BTRFS,
	    M_WAITOK | M_ZERO);
	error = btrfs_first_item(root, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &key, &data, &size);
		if (error != 0)
			break;
		if (key->type != BTRFS_BLOCK_GROUP_ITEM_KEY) {
			error = btrfs_next_item(&path);
			continue;
		}
		if (size != sizeof(*item)) {
			error = EINVAL;
			break;
		}
		item = (const struct btrfs_block_group_item *)data;
		memset(&record, 0, sizeof(record));
		record.bbg_bytenr = letoh64(key->objectid);
		record.bbg_length = letoh64(key->offset);
		record.bbg_used = letoh64(item->used);
		record.bbg_chunk_objectid = letoh64(item->chunk_objectid);
		record.bbg_flags = letoh64(item->flags);
		error = btrfs_find_chunk(bmp, record.bbg_bytenr,
		    record.bbg_length, &index);
		if (error != 0) {
			error = EINVAL;
			break;
		}
		chunk = &bmp->bm_chunks[index];
		if (record.bbg_bytenr != chunk->logical ||
		    record.bbg_length != chunk->length ||
		    record.bbg_used > record.bbg_length ||
		    record.bbg_chunk_objectid !=
		    BTRFS_FIRST_CHUNK_TREE_OBJECTID ||
		    record.bbg_flags != chunk->type || seen[index]) {
			error = EINVAL;
			break;
		}
		seen[index] = 1;
		if (callback != NULL) {
			error = callback(&record, arg);
			if (error != 0)
				break;
		}
		error = btrfs_next_item(&path);
	}
	if (error == ENOENT) {
		error = 0;
		for (i = 0; i < bmp->bm_nchunks; i++) {
			if (!seen[i]) {
				error = EINVAL;
				break;
			}
		}
	}
	btrfs_release_path(&path);
	free(seen, M_BTRFS, bmp->bm_nchunks * sizeof(*seen));
	return (error);
}

int
btrfs_iterate_chunk_items(struct btrfs_fs *bmp,
    btrfs_chunk_iter_fn callback, void *arg)
{
	const struct btrfs_key *key;
	const uint8_t *data;
	struct btrfs_chunk_map chunk;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	uint32_t size;
	unsigned int index = 0;
	int error;

	error = btrfs_get_root(bmp, BTRFS_CHUNK_TREE_OBJECTID, &root);
	if (error != 0)
		return (error);
	error = btrfs_first_item(root, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &key, &data, &size);
		if (error != 0)
			break;
		if (key->type != BTRFS_CHUNK_ITEM_KEY) {
			error = btrfs_next_item(&path);
			continue;
		}
		if (index >= bmp->bm_nchunks) {
			error = EINVAL;
			break;
		}
		error = btrfs_decode_chunk_item(&bmp->bm_super, key,
		    (const struct btrfs_chunk *)data, size, &chunk);
		if (error != 0)
			break;
		if (memcmp(&chunk, &bmp->bm_chunks[index], sizeof(chunk)) != 0) {
			error = EINVAL;
			break;
		}
		if (callback != NULL) {
			error = callback(&chunk, arg);
			if (error != 0)
				break;
		}
		index++;
		error = btrfs_next_item(&path);
	}
	if (error == ENOENT)
		error = index == bmp->bm_nchunks ? 0 : EINVAL;
	btrfs_release_path(&path);
	return (error);
}

int
btrfs_iterate_device_extents(struct btrfs_fs *bmp,
    btrfs_dev_extent_iter_fn callback, void *arg)
{
	const struct btrfs_dev_extent *item;
	const struct btrfs_chunk_map *chunk;
	const struct btrfs_key *key;
	const uint8_t *data;
	struct btrfs_dev_extent_record record;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	uint8_t *seen = NULL;
	uint64_t end, last_end = 0, used = 0;
	uint32_t size;
	unsigned int i, index, stripe;
	int error;

	error = btrfs_get_root(bmp, BTRFS_DEV_TREE_OBJECTID, &root);
	if (error != 0)
		return (error);
	seen = mallocarray(bmp->bm_nchunks, sizeof(*seen), M_BTRFS,
	    M_WAITOK | M_ZERO);
	error = btrfs_first_item(root, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &key, &data, &size);
		if (error != 0)
			break;
		if (key->type != BTRFS_DEV_EXTENT_KEY) {
			error = btrfs_next_item(&path);
			continue;
		}
		if (size != sizeof(*item)) {
			error = EINVAL;
			break;
		}
		item = (const struct btrfs_dev_extent *)data;
		memset(&record, 0, sizeof(record));
		record.bde_devid = letoh64(key->objectid);
		record.bde_physical = letoh64(key->offset);
		record.bde_length = letoh64(item->length);
		record.bde_chunk_tree = letoh64(item->chunk_tree);
		record.bde_chunk_objectid = letoh64(item->chunk_objectid);
		record.bde_chunk_offset = letoh64(item->chunk_offset);
		if (record.bde_devid !=
		    letoh64(bmp->bm_super.dev_item.devid) ||
		    record.bde_length == 0 ||
		    record.bde_physical > UINT64_MAX - record.bde_length ||
		    record.bde_physical < last_end ||
		    record.bde_chunk_tree != BTRFS_CHUNK_TREE_OBJECTID ||
		    record.bde_chunk_objectid !=
		    BTRFS_FIRST_CHUNK_TREE_OBJECTID ||
		    memcmp(item->chunk_tree_uuid, bmp->bm_chunk_tree_uuid,
		    BTRFS_UUID_SIZE) != 0) {
			error = EINVAL;
			break;
		}
		end = record.bde_physical + record.bde_length;
		error = btrfs_find_chunk(bmp, record.bde_chunk_offset,
		    record.bde_length, &index);
		if (error != 0) {
			error = EINVAL;
			break;
		}
		chunk = &bmp->bm_chunks[index];
		if (record.bde_chunk_offset != chunk->logical ||
		    record.bde_length != chunk->length) {
			error = EINVAL;
			break;
		}
		for (stripe = 0; stripe < chunk->nmirrors; stripe++) {
			if (chunk->devid[stripe] == record.bde_devid &&
			    chunk->physical[stripe] == record.bde_physical)
				break;
		}
		if (stripe == chunk->nmirrors || (seen[index] & (1U << stripe))) {
			error = EINVAL;
			break;
		}
		seen[index] |= 1U << stripe;
		last_end = end;
		if (used > UINT64_MAX - record.bde_length) {
			error = EINVAL;
			break;
		}
		used += record.bde_length;
		if (callback != NULL) {
			error = callback(&record, arg);
			if (error != 0)
				break;
		}
		error = btrfs_next_item(&path);
	}
	if (error == ENOENT) {
		error = used == letoh64(bmp->bm_super.dev_item.bytes_used) ?
		    0 : EINVAL;
		for (i = 0; i < bmp->bm_nchunks; i++) {
			if (seen[i] != (1U << bmp->bm_chunks[i].nmirrors) - 1) {
				error = EINVAL;
				break;
			}
		}
	}
	btrfs_release_path(&path);
	free(seen, M_BTRFS, bmp->bm_nchunks * sizeof(*seen));
	return (error);
}

int
btrfs_iterate_free_space(struct btrfs_fs *bmp,
    btrfs_free_space_iter_fn callback, void *arg)
{
	const struct btrfs_free_space_info *info;
	const struct btrfs_key *key;
	const uint8_t *data;
	struct btrfs_free_space_record record;
	struct btrfs_free_space_state *states = NULL;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	uint64_t bit, compat_ro, end, nbits;
	uint32_t sectorsize, size;
	size_t expected_size;
	unsigned int i, index;
	int error;

	compat_ro = letoh64(bmp->bm_super.compat_ro_flags);
	if ((compat_ro & BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE) == 0)
		return (ENOENT);
	if ((compat_ro & BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID) == 0)
		return (ESTALE);
	error = btrfs_get_root(bmp, BTRFS_FREE_SPACE_TREE_OBJECTID, &root);
	if (error != 0)
		return (error);
	states = mallocarray(bmp->bm_nchunks, sizeof(*states), M_BTRFS,
	    M_WAITOK | M_ZERO);
	sectorsize = letoh32(bmp->bm_super.sectorsize);
	error = btrfs_first_item(root, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &key, &data, &size);
		if (error != 0)
			break;
		memset(&record, 0, sizeof(record));
		record.bfs_bytenr = letoh64(key->objectid);
		record.bfs_length = letoh64(key->offset);
		switch (key->type) {
		case BTRFS_FREE_SPACE_INFO_KEY:
			if (size != sizeof(*info)) {
				error = EINVAL;
				break;
			}
			error = btrfs_find_chunk(bmp, record.bfs_bytenr,
			    record.bfs_length, &index);
			if (error != 0 ||
			    record.bfs_bytenr != bmp->bm_chunks[index].logical ||
			    record.bfs_length != bmp->bm_chunks[index].length ||
			    states[index].have_info) {
				error = EINVAL;
				break;
			}
			info = (const struct btrfs_free_space_info *)data;
			record.bfs_type = BTRFS_FREE_SPACE_RECORD_INFO;
			record.bfs_extent_count = letoh32(info->extent_count);
			record.bfs_flags = letoh32(info->flags);
			if (record.bfs_flags &
			    ~BTRFS_FREE_SPACE_USING_BITMAPS) {
				error = EINVAL;
				break;
			}
			states[index].expected = record.bfs_extent_count;
			states[index].flags = record.bfs_flags;
			states[index].last_end = record.bfs_bytenr;
			states[index].have_info = 1;
			break;
		case BTRFS_FREE_SPACE_EXTENT_KEY:
		case BTRFS_FREE_SPACE_BITMAP_KEY:
			error = btrfs_find_chunk(bmp, record.bfs_bytenr,
			    record.bfs_length, &index);
			if (error != 0 || !states[index].have_info ||
			    record.bfs_length == 0 ||
			    (record.bfs_bytenr & (sectorsize - 1)) != 0 ||
			    (record.bfs_length & (sectorsize - 1)) != 0 ||
			    record.bfs_bytenr < states[index].last_end) {
				error = EINVAL;
				break;
			}
			end = record.bfs_bytenr + record.bfs_length;
			if (key->type == BTRFS_FREE_SPACE_EXTENT_KEY) {
				if (size != 0 ||
				    (states[index].flags &
				    BTRFS_FREE_SPACE_USING_BITMAPS) != 0) {
					error = EINVAL;
					break;
				}
				record.bfs_type =
				    BTRFS_FREE_SPACE_RECORD_EXTENT;
				states[index].found++;
			} else {
				nbits = record.bfs_length / sectorsize;
				expected_size = (nbits + 7) / 8;
				if (expected_size != size ||
				    record.bfs_bytenr != states[index].last_end ||
				    (states[index].flags &
				    BTRFS_FREE_SPACE_USING_BITMAPS) == 0) {
					error = EINVAL;
					break;
				}
				if ((nbits & 7) != 0 &&
				    (data[size - 1] &
				    (0xffU << (nbits & 7))) != 0) {
					error = EINVAL;
					break;
				}
				record.bfs_type =
				    BTRFS_FREE_SPACE_RECORD_BITMAP;
				record.bfs_bitmap = data;
				record.bfs_bitmap_size = size;
				/* Count free runs, including bitmap boundaries. */
				for (bit = 0; bit < nbits; bit++) {
					int set = (data[bit / 8] >>
					    (bit & 7)) & 1;
					if (set && !states[index].last_bit)
						states[index].found++;
					states[index].last_bit = set;
				}
			}
			states[index].last_end = end;
			break;
		default:
			error = EINVAL;
			break;
		}
		if (error != 0)
			break;
		if (callback != NULL) {
			error = callback(&record, arg);
			if (error != 0)
				break;
		}
		error = btrfs_next_item(&path);
	}
	if (error == ENOENT) {
		error = 0;
		for (i = 0; i < bmp->bm_nchunks; i++) {
			if (!states[i].have_info ||
			    states[i].found != states[i].expected ||
			    ((states[i].flags & BTRFS_FREE_SPACE_USING_BITMAPS) &&
			    states[i].last_end != bmp->bm_chunks[i].logical +
			    bmp->bm_chunks[i].length)) {
				error = EINVAL;
				break;
			}
		}
	}
	btrfs_release_path(&path);
	free(states, M_BTRFS, bmp->bm_nchunks * sizeof(*states));
	return (error);
}
