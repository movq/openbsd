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
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/malloc.h>

#include <btrfs/btrfs_var.h>
#include <btrfs/btrfs_dir.h>
#include <lib/libkern/crc32c.h>

int
btrfs_name_valid(const uint8_t *name, size_t len)
{
	return (len != 0 && len <= BTRFS_NAME_MAX &&
	    memchr(name, '\0', len) == NULL &&
	    memchr(name, '/', len) == NULL &&
	    !(len == 1 && name[0] == '.') &&
	    !(len == 2 && name[0] == '.' && name[1] == '.'));
}

uint64_t
btrfs_name_hash(const void *name, size_t len)
{
	/* Raw CRC32C seeded with ~1, without final inversion. */
	return (crc32c(1, name, len) ^ 0xffffffffU);
}

/* Directory entries and xattrs share framing, but not name/value semantics. */
int
btrfs_decode_dir_record(const uint8_t *data, uint32_t remaining,
    struct btrfs_dir_record *record)
{
	const struct btrfs_dir_item *item;
	uint32_t size;

	if (remaining < sizeof(*item))
		return (EINVAL);
	item = (const struct btrfs_dir_item *)data;
	record->namelen = letoh16(item->name_len);
	record->datalen = letoh16(item->data_len);
	size = sizeof(*item) + record->namelen + record->datalen;
	if (size > remaining)
		return (EINVAL);
	record->item = item;
	record->name = data + sizeof(*item);
	record->value = record->name + record->namelen;
	record->size = size;
	return (0);
}

int
btrfs_validate_dir_record(const struct btrfs_key *key, uint64_t generation,
    uint32_t itemsize, const struct btrfs_dir_record *record)
{
	const struct btrfs_dir_item *item = record->item;

	if (record->datalen != 0 ||
	    !btrfs_name_valid(record->name, record->namelen) ||
	    letoh64(item->location.objectid) < BTRFS_FIRST_FREE_OBJECTID ||
	    letoh64(item->transid) > generation ||
	    item->type > BTRFS_FT_SYMLINK)
		return (EINVAL);
	if (key->type == BTRFS_DIR_INDEX_KEY) {
		if (record->size != itemsize || letoh64(key->offset) < 2)
			return (EINVAL);
	} else if (key->type != BTRFS_DIR_ITEM_KEY ||
	    letoh64(key->offset) !=
	    btrfs_name_hash(record->name, record->namelen))
		return (EINVAL);
	if (item->location.type == BTRFS_ROOT_ITEM_KEY) {
		if (item->type != BTRFS_FT_DIR)
			return (EINVAL);
	} else if (item->location.type != BTRFS_INODE_ITEM_KEY ||
	    item->location.offset != 0)
		return (EINVAL);
	return (0);
}

int
btrfs_next_dir_index(struct btrfs_root *root, uint64_t ino, uint64_t *index)
{
	struct btrfs_key target = { 0 };
	struct btrfs_path path = { 0 };
	const struct btrfs_key *key;
	uint64_t last;
	int error;

	*index = 2;
	target.objectid = htole64(ino);
	target.type = BTRFS_DIR_INDEX_KEY;
	target.offset = htole64(UINT64_MAX);
	error = btrfs_search_predecessor(root, &target, &path);
	if (error == 0) {
		error = btrfs_path_item(&path, &key, NULL, NULL);
		if (error == 0 && key->objectid == target.objectid &&
		    key->type == target.type) {
			last = letoh64(key->offset);
			/* The next readdir cookie must fit in a signed offset. */
			if (last < 2 || last >= INT64_MAX - 1)
				error = EOVERFLOW;
			else
				*index = last + 1;
		}
	} else if (error == ENOENT)
		error = 0;
	btrfs_release_path(&path);
	return (error);
}

struct btrfs_name_plan *
btrfs_name_plan_alloc(struct btrfs_root *root)
{
	struct btrfs_name_plan *plan;

	plan = malloc(sizeof(*plan), M_BTRFS, M_WAITOK | M_ZERO);
	plan->root = root;
	plan->nodesize = letoh32(root->br_super->nodesize);
	plan->capacity = plan->nodesize - sizeof(struct btrfs_header) -
	    sizeof(struct btrfs_item);
	return (plan);
}

void
btrfs_name_plan_free(struct btrfs_name_plan *plan)
{
	unsigned int i;

	if (plan == NULL)
		return;
	for (i = 0; i < plan->count; i++)
		free(plan->edits[i].data, M_BTRFS, plan->nodesize);
	free(plan, M_BTRFS, sizeof(*plan));
}

int
btrfs_name_edit(struct btrfs_name_plan *plan, uint64_t ino, uint8_t type,
    uint64_t offset, struct btrfs_name_edit **editp)
{
	struct btrfs_name_edit *edit;
	struct btrfs_path path = { 0 };
	const uint8_t *data;
	unsigned int i;
	int error;

	for (i = 0; i < plan->count; i++) {
		edit = &plan->edits[i];
		if (letoh64(edit->key.objectid) == ino &&
		    edit->key.type == type &&
		    letoh64(edit->key.offset) == offset) {
			*editp = edit;
			return (0);
		}
	}
	if (plan->count == nitems(plan->edits))
		return (EOVERFLOW);
	edit = &plan->edits[plan->count++];
	edit->key.objectid = htole64(ino);
	edit->key.type = type;
	edit->key.offset = htole64(offset);
	edit->data = malloc(plan->nodesize, M_BTRFS, M_WAITOK);
	error = btrfs_search_slot(plan->root, &edit->key, &path);
	if (error == 0) {
		error = btrfs_path_item(&path, NULL, &data, &edit->size);
		if (error == 0 && (edit->size == 0 ||
		    edit->size > plan->capacity))
			error = EINVAL;
		if (error == 0) {
			memcpy(edit->data, data, edit->size);
			edit->oldsize = edit->size;
		}
	} else if (error == ENOENT)
		error = 0;
	btrfs_release_path(&path);
	*editp = edit;
	return (error);
}

/* Validate the entire bucket, including records beyond the requested name. */
static int
btrfs_plan_dir_find(struct btrfs_name_plan *plan, uint64_t parent,
    const char *name, size_t len, struct btrfs_name_edit **hashp,
    uint32_t *matchp, uint32_t *sizep)
{
	struct btrfs_name_edit *hash;
	struct btrfs_dir_record record;
	uint32_t off;
	int error;

	if (!btrfs_name_valid((const uint8_t *)name, len))
		return (EINVAL);
	error = btrfs_name_edit(plan, parent, BTRFS_DIR_ITEM_KEY,
	    btrfs_name_hash(name, len), &hash);
	if (error != 0)
		return (error);
	*hashp = hash;
	*sizep = 0;
	for (off = 0; off < hash->size; off += record.size) {
		error = btrfs_decode_dir_record(hash->data + off,
		    hash->size - off, &record);
		if (error == 0)
			error = btrfs_validate_dir_record(&hash->key,
			    plan->root->br_view_generation, hash->size, &record);
		if (error != 0)
			return (error);
		if (record.namelen != len || memcmp(record.name, name, len))
			continue;
		if (*sizep != 0)
			return (EINVAL);
		*matchp = off;
		*sizep = record.size;
	}
	return (0);
}

int
btrfs_plan_dir_remove(struct btrfs_name_plan *plan, uint64_t parent,
    const char *name, size_t len, uint64_t cookie, uint64_t objectid,
    uint8_t type, struct btrfs_dir_item *record)
{
	struct btrfs_name_edit *hash, *index;
	const struct btrfs_dir_item *entry;
	uint32_t match = 0, size;
	int error;

	KASSERT(plan->nadds == 0);
	error = btrfs_plan_dir_find(plan, parent, name, len, &hash,
	    &match, &size);
	if (error != 0)
		return (error);
	if (size == 0)
		return (ENOENT);
	entry = (const struct btrfs_dir_item *)(hash->data + match);
	if (letoh64(entry->location.objectid) != objectid ||
	    entry->location.type != type || cookie < 2)
		return (EINVAL);
	error = btrfs_name_edit(plan, parent, BTRFS_DIR_INDEX_KEY,
	    cookie, &index);
	if (error != 0)
		return (error);
	if (index->size != size ||
	    memcmp(index->data, hash->data + match, size) != 0)
		return (EINVAL);
	if (record != NULL)
		memcpy(record, entry, sizeof(*record));
	index->size = 0;
	index->dirty = 1;
	memmove(hash->data + match, hash->data + match + size,
	    hash->size - match - size);
	hash->size -= size;
	hash->dirty = 1;
	return (0);
}

int
btrfs_plan_dir_add(struct btrfs_name_plan *plan, uint64_t parent,
    const char *name, size_t len, uint64_t cookie,
    const struct btrfs_dir_item *record)
{
	struct btrfs_name_edit *hash, *index;
	struct btrfs_dir_item *entry;
	struct btrfs_dir_record decoded;
	uint32_t match, size, extra = sizeof(*record) + len;
	int error;

	if (cookie < 2 || cookie >= INT64_MAX)
		return (EOVERFLOW);
	if (plan->nadds == nitems(plan->adds))
		return (EOVERFLOW);
	error = btrfs_plan_dir_find(plan, parent, name, len, &hash,
	    &match, &size);
	if (error != 0)
		return (error);
	if (size != 0)
		return (EEXIST);
	if (extra > plan->capacity || hash->size > plan->capacity - extra)
		return (ENOSPC);
	error = btrfs_name_edit(plan, parent, BTRFS_DIR_INDEX_KEY,
	    cookie, &index);
	if (error != 0)
		return (error);
	if (index->size != 0)
		return (EINVAL);
	entry = (struct btrfs_dir_item *)(hash->data + hash->size);
	memcpy(entry, record, sizeof(*entry));
	entry->transid = 0;	/* Filled from the caller's handle at apply. */
	entry->name_len = htole16(len);
	memcpy(entry + 1, name, len);
	error = btrfs_decode_dir_record((const uint8_t *)entry, extra,
	    &decoded);
	if (error == 0)
		error = btrfs_validate_dir_record(&hash->key,
		    plan->root->br_view_generation, extra, &decoded);
	if (error != 0)
		return (error);
	plan->adds[plan->nadds].hash = hash;
	plan->adds[plan->nadds].index = index;
	plan->adds[plan->nadds++].offset = hash->size;
	memcpy(index->data, entry, extra);
	index->size = extra;
	index->dirty = 1;
	hash->size += extra;
	hash->dirty = 1;
	return (0);
}

int
btrfs_name_plan_apply(struct btrfs_trans_handle *handle,
    struct btrfs_name_plan *plan)
{
	struct btrfs_name_edit *edit;
	struct btrfs_dir_item *entry;
	uint64_t generation = handle->bth_transaction->bt_generation;
	unsigned int i;
	int error = 0;

	for (i = 0; i < plan->nadds; i++) {
		entry = (struct btrfs_dir_item *)(plan->adds[i].hash->data +
		    plan->adds[i].offset);
		entry->transid = htole64(generation);
		entry = (struct btrfs_dir_item *)plan->adds[i].index->data;
		entry->transid = htole64(generation);
	}
	for (i = 0; i < plan->count; i++) {
		edit = &plan->edits[i];
		if (!edit->dirty)
			continue;
		if (edit->size == 0) {
			if (edit->oldsize != 0)
				error = btrfs_delete_item(handle, plan->root,
				    &edit->key);
		} else if (edit->oldsize != 0 && edit->size <= edit->oldsize)
			error = btrfs_replace_item(handle, plan->root, &edit->key,
			    edit->data, edit->size);
		else {
			/* Only insertion can split a leaf for a growing item. */
			if (edit->oldsize != 0)
				error = btrfs_delete_item(handle, plan->root,
				    &edit->key);
			if (error == 0)
				error = btrfs_insert_item(handle, plan->root,
				    &edit->key, edit->data, edit->size);
		}
		if (error != 0)
			break;
	}
	return (error);
}
