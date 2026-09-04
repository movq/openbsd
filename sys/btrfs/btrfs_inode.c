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
#include <sys/stat.h>
#include <sys/vnode.h>

#include <btrfs/btrfs_var.h>

static int	btrfs_ref_name_valid(const uint8_t *, uint16_t);
static int	btrfs_decode_file_extent(const struct btrfs_mount *,
		    const struct btrfs_key *, const uint8_t *, uint32_t,
		    struct btrfs_file_extent *);

static int
btrfs_ref_name_valid(const uint8_t *name, uint16_t namelen)
{
	if (namelen == 0 || namelen > BTRFS_NAME_MAX ||
	    memchr(name, '\0', namelen) != NULL ||
	    memchr(name, '/', namelen) != NULL ||
	    (namelen == 1 && name[0] == '.') ||
	    (namelen == 2 && name[0] == '.' && name[1] == '.'))
		return (0);
	return (1);
}

int
btrfs_find_inode_item(struct btrfs_root *root, uint64_t objectid,
    struct btrfs_inode_item *result)
{
	const uint8_t *data;
	const struct btrfs_inode_item *inode_item;
	const struct btrfs_super_block *sb = root->br_super;
	struct btrfs_path path = { 0 };
	struct btrfs_key target;
	uint64_t generation, transid;
	uint32_t mode, nlink, size;
	int error;

	memset(&target, 0, sizeof(target));
	target.objectid = htole64(objectid);
	target.type = BTRFS_INODE_ITEM_KEY;
	error = btrfs_search_slot(root, &target, &path);
	if (error != 0)
		goto out;
	error = btrfs_path_item(&path, NULL, &data, &size);
	if (error != 0)
		goto out;
	if (size != sizeof(*inode_item))
		goto invalid;
	inode_item = (const struct btrfs_inode_item *)data;

	generation = letoh64(inode_item->generation);
	transid = letoh64(inode_item->transid);
	mode = letoh32(inode_item->mode);
	nlink = letoh32(inode_item->nlink);
	if (generation == 0 || generation > letoh64(sb->generation) ||
	    transid > letoh64(sb->generation) ||
	    IFTOVT(mode) == VNON || IFTOVT(mode) == VBAD || nlink == 0 ||
	    letoh32(inode_item->atime.nsec) >= 1000000000 ||
	    letoh32(inode_item->ctime.nsec) >= 1000000000 ||
	    letoh32(inode_item->mtime.nsec) >= 1000000000 ||
	    letoh32(inode_item->otime.nsec) >= 1000000000)
		goto invalid;

	memcpy(result, inode_item, sizeof(*result));
	error = 0;
	goto out;
invalid:
	error = EINVAL;
out:
	btrfs_release_path(&path);
	return (error);
}

int
btrfs_find_dir_parent(struct btrfs_root *root, uint64_t objectid,
    uint64_t *parentp)
{
	const struct btrfs_inode_extref *extref;
	const struct btrfs_inode_ref *ref;
	const struct btrfs_key *key;
	const uint8_t *data, *name;
	struct btrfs_path path = { 0 };
	struct btrfs_key target;
	uint64_t parent;
	uint32_t item_size;
	uint16_t namelen;
	size_t record_size, remaining;
	unsigned int nrefs = 0;
	int error;

	if (objectid == BTRFS_FIRST_FREE_OBJECTID) {
		*parentp = objectid;
		return (0);
	}

	memset(&target, 0, sizeof(target));
	target.objectid = htole64(objectid);
	target.type = BTRFS_INODE_REF_KEY;
	error = btrfs_search_lower_bound(root, &target, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &key, &data, &item_size);
		if (error != 0)
			break;
		if (letoh64(key->objectid) != objectid ||
		    key->type > BTRFS_INODE_EXTREF_KEY)
			break;

		remaining = item_size;
		if (key->type == BTRFS_INODE_REF_KEY) {
			parent = letoh64(key->offset);
			while (remaining != 0) {
				if (remaining < sizeof(*ref))
					goto invalid;
				ref = (const struct btrfs_inode_ref *)data;
				namelen = letoh16(ref->name_len);
				if (namelen > remaining - sizeof(*ref))
					goto invalid;
				record_size = sizeof(*ref) + namelen;
				name = data + sizeof(*ref);
				if (!btrfs_ref_name_valid(name, namelen))
					goto invalid;
				if (parent < BTRFS_FIRST_FREE_OBJECTID ||
				    parent > BTRFS_LAST_FREE_OBJECTID ||
				    parent == objectid || ++nrefs != 1)
					goto invalid;
				*parentp = parent;
				data += record_size;
				remaining -= record_size;
			}
		} else if (key->type == BTRFS_INODE_EXTREF_KEY) {
			while (remaining != 0) {
				if (remaining < sizeof(*extref))
					goto invalid;
				extref = (const struct btrfs_inode_extref *)data;
				namelen = letoh16(extref->name_len);
				if (namelen > remaining - sizeof(*extref))
					goto invalid;
				record_size = sizeof(*extref) + namelen;
				name = data + sizeof(*extref);
				if (!btrfs_ref_name_valid(name, namelen))
					goto invalid;
				parent = letoh64(extref->parent_objectid);
				if (parent < BTRFS_FIRST_FREE_OBJECTID ||
				    parent > BTRFS_LAST_FREE_OBJECTID ||
				    parent == objectid || ++nrefs != 1)
					goto invalid;
				*parentp = parent;
				data += record_size;
				remaining -= record_size;
			}
		}
		error = btrfs_next_item(&path);
	}
	if (error == ENOENT)
		error = 0;
	if (error == 0 && nrefs != 1)
		error = EINVAL;
	goto out;
invalid:
	error = EINVAL;
out:
	btrfs_release_path(&path);
	return (error);
}

int
btrfs_find_subvol_parent(struct btrfs_mount *bmp, uint64_t treeid,
    uint64_t *parent_treeidp, uint64_t *parent_diridp)
{
	const struct btrfs_root_ref *ref;
	const struct btrfs_key *key;
	const uint8_t *data, *name;
	struct btrfs_path path = { 0 };
	struct btrfs_root root;
	struct btrfs_key target;
	uint64_t parent_treeid, parent_dirid;
	uint32_t size;
	uint16_t namelen;
	unsigned int nrefs = 0;
	int error;

	if (treeid == bmp->bm_treeid) {
		*parent_treeidp = treeid;
		*parent_diridp = bmp->bm_root_dirid;
		return (0);
	}

	memset(&target, 0, sizeof(target));
	target.objectid = htole64(treeid);
	target.type = BTRFS_ROOT_BACKREF_KEY;
	btrfs_init_root_tree(bmp, &root);
	error = btrfs_search_lower_bound(&root, &target, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &key, &data, &size);
		if (error != 0)
			break;
		if (letoh64(key->objectid) != treeid ||
		    key->type != BTRFS_ROOT_BACKREF_KEY)
			break;
		if (size < sizeof(*ref))
			goto invalid;
		ref = (const struct btrfs_root_ref *)data;
		namelen = letoh16(ref->name_len);
		if (namelen != size - sizeof(*ref))
			goto invalid;
		name = data + sizeof(*ref);
		if (!btrfs_ref_name_valid(name, namelen))
			goto invalid;

		parent_treeid = letoh64(key->offset);
		parent_dirid = letoh64(ref->dirid);
		if ((parent_treeid != bmp->bm_treeid &&
		    (parent_treeid < BTRFS_FIRST_FREE_OBJECTID ||
		    parent_treeid > BTRFS_LAST_FREE_OBJECTID)) ||
		    parent_dirid < BTRFS_FIRST_FREE_OBJECTID ||
		    parent_dirid > BTRFS_LAST_FREE_OBJECTID ||
		    ++nrefs != 1)
			goto invalid;
		*parent_treeidp = parent_treeid;
		*parent_diridp = parent_dirid;
		error = btrfs_next_item(&path);
	}
	if (error == ENOENT)
		error = 0;
	if (error == 0 && nrefs != 1)
		error = EINVAL;
	goto out;
invalid:
	error = EINVAL;
out:
	btrfs_release_path(&path);
	return (error);
}

int
btrfs_iterate_directory(struct btrfs_root *root, uint64_t objectid,
    btrfs_dir_iter_fn callback, void *arg)
{
	const struct btrfs_dir_item *dir_item;
	const struct btrfs_key *key;
	const struct btrfs_super_block *sb = root->br_super;
	struct btrfs_path path = { 0 };
	struct btrfs_key target;
	struct btrfs_dir_entry entry;
	const uint8_t *data, *name;
	uint64_t location, transid;
	uint32_t size;
	uint16_t data_len, name_len;
	size_t record_size, remaining;
	int error;

	memset(&target, 0, sizeof(target));
	target.objectid = htole64(objectid);
	target.type = BTRFS_DIR_INDEX_KEY;
	error = btrfs_search_lower_bound(root, &target, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &key, &data, &size);
		if (error != 0)
			break;
		if (letoh64(key->objectid) > objectid ||
		    key->type > BTRFS_DIR_INDEX_KEY)
			break;
		remaining = size;
		while (remaining != 0) {
			if (remaining < sizeof(*dir_item))
				goto invalid;
			dir_item = (const struct btrfs_dir_item *)data;
			data_len = letoh16(dir_item->data_len);
			name_len = letoh16(dir_item->name_len);
			if (name_len == 0 || name_len > BTRFS_NAME_MAX ||
			    data_len != 0 ||
			    name_len > remaining - sizeof(*dir_item))
				goto invalid;
			record_size = sizeof(*dir_item) + name_len;
			name = data + sizeof(*dir_item);
			if (!btrfs_ref_name_valid(name, name_len))
				goto invalid;

			location = letoh64(dir_item->location.objectid);
			transid = letoh64(dir_item->transid);
			if (location < BTRFS_FIRST_FREE_OBJECTID ||
			    transid > letoh64(sb->generation) ||
			    dir_item->type > BTRFS_FT_SYMLINK)
				goto invalid;
			if (dir_item->location.type == BTRFS_ROOT_ITEM_KEY) {
				if (dir_item->type != BTRFS_FT_DIR)
					goto invalid;
			} else if (dir_item->location.type !=
			    BTRFS_INODE_ITEM_KEY ||
			    letoh64(dir_item->location.offset) != 0)
				goto invalid;

			if (callback != NULL) {
				entry.bde_name = name;
				entry.bde_objectid = location;
				entry.bde_index = letoh64(key->offset);
				entry.bde_namelen = name_len;
				entry.bde_type = dir_item->type;
				entry.bde_subvolume =
				    dir_item->location.type ==
				    BTRFS_ROOT_ITEM_KEY;
				error = callback(&entry, arg);
				if (error != 0)
					goto out;
			}

			data += record_size;
			remaining -= record_size;
		}
		error = btrfs_next_item(&path);
	}

	if (error == ENOENT)
		error = 0;
	goto out;
invalid:
	error = EINVAL;
out:
	btrfs_release_path(&path);
	return (error);
}

static int
btrfs_decode_file_extent(const struct btrfs_mount *bmp,
    const struct btrfs_key *key, const uint8_t *data, uint32_t item_size,
    struct btrfs_file_extent *decoded)
{
	const struct btrfs_file_extent_item *extent;
	uint64_t disk_end, extent_end, generation, ram_bytes;
	uint32_t sectorsize;
	size_t prefix_size;

	memset(decoded, 0, sizeof(*decoded));
	prefix_size = offsetof(struct btrfs_file_extent_item, disk_bytenr);
	if (item_size < prefix_size)
		return (EINVAL);

	extent = (const struct btrfs_file_extent_item *)data;
	generation = letoh64(extent->generation);
	ram_bytes = letoh64(extent->ram_bytes);
	if (generation == 0 ||
	    generation > letoh64(bmp->bm_super.generation) ||
	    extent->compression > BTRFS_COMPRESS_ZSTD ||
	    extent->type > BTRFS_FILE_EXTENT_PREALLOC)
		return (EINVAL);

	decoded->bfe_logical = letoh64(key->offset);
	decoded->bfe_compression = extent->compression;
	decoded->bfe_encryption = extent->encryption;
	decoded->bfe_other_encoding = letoh16(extent->other_encoding);
	decoded->bfe_type = extent->type;
	decoded->bfe_ram_bytes = ram_bytes;

	if (extent->type == BTRFS_FILE_EXTENT_INLINE) {
		if (decoded->bfe_logical != 0 || ram_bytes == 0 ||
		    (decoded->bfe_compression != BTRFS_COMPRESS_NONE &&
		    ram_bytes > BTRFS_MAX_UNCOMPRESSED))
			return (EINVAL);
		decoded->bfe_length = ram_bytes;
		decoded->bfe_inline_data = data + prefix_size;
		decoded->bfe_inline_size = item_size - prefix_size;
		return (0);
	}
	if (item_size != sizeof(*extent))
		return (EINVAL);

	decoded->bfe_length = letoh64(extent->num_bytes);
	decoded->bfe_disk_bytenr = letoh64(extent->disk_bytenr);
	decoded->bfe_disk_num_bytes = letoh64(extent->disk_num_bytes);
	decoded->bfe_disk_offset = letoh64(extent->offset);
	if (decoded->bfe_length == 0 ||
	    decoded->bfe_logical > UINT64_MAX - decoded->bfe_length)
		return (EINVAL);

	if (decoded->bfe_disk_bytenr == 0) {
		if (extent->type != BTRFS_FILE_EXTENT_REG ||
		    decoded->bfe_disk_num_bytes != 0 ||
		    decoded->bfe_disk_offset != 0 ||
		    decoded->bfe_compression != BTRFS_COMPRESS_NONE ||
		    decoded->bfe_encryption != 0 ||
		    decoded->bfe_other_encoding != 0)
			return (EINVAL);
		decoded->bfe_type = BTRFS_FILE_EXTENT_HOLE;
		return (0);
	}

	sectorsize = letoh32(bmp->bm_super.sectorsize);
	if ((decoded->bfe_disk_bytenr & (sectorsize - 1)) != 0 ||
	    decoded->bfe_disk_num_bytes == 0 ||
	    (decoded->bfe_disk_num_bytes & (sectorsize - 1)) != 0 ||
	    (decoded->bfe_disk_offset & (sectorsize - 1)) != 0 ||
	    decoded->bfe_disk_bytenr >
	    UINT64_MAX - decoded->bfe_disk_num_bytes ||
	    decoded->bfe_disk_offset > ram_bytes ||
	    decoded->bfe_length > ram_bytes - decoded->bfe_disk_offset)
		return (EINVAL);

	if (decoded->bfe_compression == BTRFS_COMPRESS_NONE) {
		disk_end = decoded->bfe_disk_num_bytes;
		extent_end = decoded->bfe_disk_offset + decoded->bfe_length;
		if (extent_end > disk_end)
			return (EINVAL);
	} else if (extent->type != BTRFS_FILE_EXTENT_REG ||
	    decoded->bfe_disk_num_bytes > BTRFS_MAX_COMPRESSED ||
	    ram_bytes > BTRFS_MAX_UNCOMPRESSED) {
		return (EINVAL);
	}
	return (0);
}

int
btrfs_find_file_extent(const struct btrfs_mount *bmp,
    struct btrfs_root *root, struct btrfs_path *path, uint64_t objectid,
    uint64_t position, uint64_t file_size, struct btrfs_file_extent *extent)
{
	const struct btrfs_key *key;
	const uint8_t *data;
	struct btrfs_key target;
	struct btrfs_file_extent decoded;
	uint64_t end, previous_end = 0;
	uint32_t item_size;
	int error;

	memset(extent, 0, sizeof(*extent));
	memset(&target, 0, sizeof(target));
	target.objectid = htole64(objectid);
	target.type = BTRFS_EXTENT_DATA_KEY;
	target.offset = htole64(position);

	error = btrfs_search_predecessor(root, &target, path);
	if (error == 0) {
		error = btrfs_path_item(path, &key, &data, &item_size);
		if (error != 0)
			return (error);
		if (letoh64(key->objectid) == objectid &&
		    key->type == BTRFS_EXTENT_DATA_KEY) {
			error = btrfs_decode_file_extent(bmp, key, data,
			    item_size, &decoded);
			if (error != 0)
				return (error);
			end = decoded.bfe_logical + decoded.bfe_length;
			if (position < end) {
				*extent = decoded;
				return (0);
			}
			previous_end = end;
			error = btrfs_next_item(path);
		} else {
			target.offset = 0;
			error = btrfs_search_lower_bound(root, &target, path);
		}
	} else if (error == ENOENT) {
		target.offset = 0;
		error = btrfs_search_lower_bound(root, &target, path);
	}
	if (error != 0 && error != ENOENT)
		return (error);

	if (error == 0) {
		error = btrfs_path_item(path, &key, &data, &item_size);
		if (error != 0)
			return (error);
		if (letoh64(key->objectid) == objectid &&
		    key->type == BTRFS_EXTENT_DATA_KEY) {
			error = btrfs_decode_file_extent(bmp, key, data,
			    item_size, &decoded);
			if (error != 0)
				return (error);
			if (decoded.bfe_logical < previous_end)
				return (EINVAL);
			end = decoded.bfe_logical + decoded.bfe_length;
			if (position < decoded.bfe_logical) {
				extent->bfe_logical = position;
				extent->bfe_length =
				    MIN(decoded.bfe_logical, file_size) -
				    position;
				extent->bfe_type = BTRFS_FILE_EXTENT_HOLE;
				return (0);
			}
			if (position < end) {
				*extent = decoded;
				return (0);
			}
		}
	}

	extent->bfe_logical = position;
	extent->bfe_length = file_size - position;
	extent->bfe_type = BTRFS_FILE_EXTENT_HOLE;
	return (0);
}
