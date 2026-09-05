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
#include <sys/stat.h>
#include <sys/vnode.h>

#include <btrfs/btrfs_var.h>
#include <lib/libkern/crc32c.h>

static int	btrfs_ref_name_valid(const uint8_t *, uint16_t);
static void	btrfs_decode_timespec(const struct btrfs_timespec *,
		    struct timespec *);
static void	btrfs_encode_timespec(const struct timespec *,
		    struct btrfs_timespec *);
static void	btrfs_decode_inode(const struct btrfs_inode_item *,
		    struct btrfs_inode *);
static int	btrfs_decode_file_extent(const struct btrfs_mount *,
		    uint64_t, const struct btrfs_key *, const uint8_t *,
		    uint32_t, struct btrfs_file_extent *);
static int	btrfs_prepare_append(struct btrfs_root *,
		    const struct btrfs_key *, uint8_t *, uint32_t, uint32_t *);
static int	btrfs_insert_append(struct btrfs_trans_handle *,
		    struct btrfs_root *, const struct btrfs_key *,
		    const uint8_t *, uint32_t, uint32_t);

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

static void
btrfs_decode_timespec(const struct btrfs_timespec *disk,
    struct timespec *host)
{
	host->tv_sec = letoh64(disk->sec);
	host->tv_nsec = letoh32(disk->nsec);
}

static void
btrfs_encode_timespec(const struct timespec *host,
    struct btrfs_timespec *disk)
{
	disk->sec = htole64(host->tv_sec);
	disk->nsec = htole32(host->tv_nsec);
}

static void
btrfs_decode_inode(const struct btrfs_inode_item *disk,
    struct btrfs_inode *inode)
{
	memset(inode, 0, sizeof(*inode));
	inode->bi_generation = letoh64(disk->generation);
	inode->bi_transid = letoh64(disk->transid);
	inode->bi_size = letoh64(disk->size);
	inode->bi_nbytes = letoh64(disk->nbytes);
	inode->bi_block_group = letoh64(disk->block_group);
	inode->bi_nlink = letoh32(disk->nlink);
	inode->bi_uid = letoh32(disk->uid);
	inode->bi_gid = letoh32(disk->gid);
	inode->bi_mode = letoh32(disk->mode);
	inode->bi_rdev = letoh64(disk->rdev);
	inode->bi_flags = letoh64(disk->flags);
	inode->bi_sequence = letoh64(disk->sequence);
	btrfs_decode_timespec(&disk->atime, &inode->bi_atime);
	btrfs_decode_timespec(&disk->ctime, &inode->bi_ctime);
	btrfs_decode_timespec(&disk->mtime, &inode->bi_mtime);
	btrfs_decode_timespec(&disk->otime, &inode->bi_otime);
}

int
btrfs_find_inode(struct btrfs_root *root, uint64_t objectid,
    struct btrfs_inode *result)
{
	const uint8_t *data;
	const struct btrfs_inode_item *inode_item;
	struct btrfs_path path = { 0 };
	struct btrfs_key target;
	uint32_t size;
	int error;

	memset(result, 0, sizeof(*result));
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
	btrfs_decode_inode(inode_item, result);
	if (result->bi_generation > root->br_view_generation ||
	    result->bi_transid > root->br_view_generation ||
	    IFTOVT(result->bi_mode) == VNON ||
	    IFTOVT(result->bi_mode) == VBAD || result->bi_nlink == 0 ||
	    result->bi_atime.tv_nsec < 0 ||
	    result->bi_atime.tv_nsec >= 1000000000 ||
	    result->bi_ctime.tv_nsec < 0 ||
	    result->bi_ctime.tv_nsec >= 1000000000 ||
	    result->bi_mtime.tv_nsec < 0 ||
	    result->bi_mtime.tv_nsec >= 1000000000 ||
	    result->bi_otime.tv_nsec < 0 ||
	    result->bi_otime.tv_nsec >= 1000000000)
		goto invalid;

	error = 0;
	goto out;
invalid:
	memset(result, 0, sizeof(*result));
	error = EINVAL;
out:
	btrfs_release_path(&path);
	return (error);
}

int
btrfs_write_inode(struct btrfs_trans_handle *handle,
    struct btrfs_node *node)
{
	struct btrfs_transaction *trans;
	struct btrfs_inode_item item;
	struct btrfs_inode *inode;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	struct btrfs_key key;
	const uint8_t *data;
	uint32_t size;
	int error;

	if (handle == NULL || handle->bth_transaction == NULL ||
	    node == NULL || node->bn_vnode == NULL)
		return (EINVAL);
	KASSERT(VOP_ISLOCKED(node->bn_vnode));
	trans = handle->bth_transaction;
	inode = &node->bn_inode;
	if (node->bn_mount != trans->bt_mount)
		return (EINVAL);
	if (inode->bi_dirty_fields == 0)
		return (0);
	if (inode->bi_last_dirty_transid != trans->bt_generation ||
	    (inode->bi_dirty_fields & ~BTRFS_INODE_DIRTY_ALL) != 0 ||
	    inode->bi_generation > trans->bt_generation ||
	    inode->bi_transid > trans->bt_generation ||
	    inode->bi_nlink == 0 || IFTOVT(inode->bi_mode) == VNON ||
	    IFTOVT(inode->bi_mode) == VBAD ||
	    inode->bi_atime.tv_nsec < 0 ||
	    inode->bi_atime.tv_nsec >= 1000000000 ||
	    inode->bi_ctime.tv_nsec < 0 ||
	    inode->bi_ctime.tv_nsec >= 1000000000 ||
	    inode->bi_mtime.tv_nsec < 0 ||
	    inode->bi_mtime.tv_nsec >= 1000000000 ||
	    inode->bi_otime.tv_nsec < 0 ||
	    inode->bi_otime.tv_nsec >= 1000000000)
		return (EINVAL);

	error = btrfs_get_root(node->bn_mount, node->bn_treeid, &root);
	if (error != 0)
		return (error);
	memset(&key, 0, sizeof(key));
	key.objectid = htole64(node->bn_ino);
	key.type = BTRFS_INODE_ITEM_KEY;
	error = btrfs_search_slot(root, &key, &path);
	if (error != 0)
		goto out;
	error = btrfs_path_item(&path, NULL, &data, &size);
	if (error != 0)
		goto out;
	if (size != sizeof(item)) {
		error = EINVAL;
		goto out;
	}
	memcpy(&item, data, sizeof(item));
	btrfs_release_path(&path);

	item.generation = htole64(inode->bi_generation);
	item.transid = htole64(trans->bt_generation);
	item.size = htole64(inode->bi_size);
	item.nbytes = htole64(inode->bi_nbytes);
	item.block_group = htole64(inode->bi_block_group);
	item.nlink = htole32(inode->bi_nlink);
	item.uid = htole32(inode->bi_uid);
	item.gid = htole32(inode->bi_gid);
	item.mode = htole32(inode->bi_mode);
	item.rdev = htole64(inode->bi_rdev);
	item.flags = htole64(inode->bi_flags);
	item.sequence = htole64(inode->bi_sequence);
	btrfs_encode_timespec(&inode->bi_atime, &item.atime);
	btrfs_encode_timespec(&inode->bi_ctime, &item.ctime);
	btrfs_encode_timespec(&inode->bi_mtime, &item.mtime);
	btrfs_encode_timespec(&inode->bi_otime, &item.otime);

	error = btrfs_replace_item(handle, root, &key, &item, sizeof(item));
	if (error != 0)
		return (error);
	inode->bi_transid = trans->bt_generation;
	inode->bi_dirty_fields = 0;
	return (0);
out:
	btrfs_release_path(&path);
	return (error);
}

/*
 * The parent vnode lock protects its index and hash buckets.  The namespace
 * lock also protects the highest object ID across creates in different
 * directories.  Namespace items, the optional symlink extent, and the parent
 * inode belong to one transaction; any error after the first mutation aborts
 * that transaction.
 */
int
btrfs_create_inode(struct btrfs_node *dir, const char *name, size_t namelen,
    mode_t mode, uid_t uid, gid_t gid, const char *link, struct vnode **vpp)
{
	struct btrfs_mount *bmp = dir->bn_mount;
	struct btrfs_trans_reservation reservation = { 0 };
	struct btrfs_trans_handle *handle = NULL;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	struct btrfs_key target, hashkey;
	const struct btrfs_key *key;
	struct btrfs_inode_item inode;
	struct btrfs_file_extent_item *extent;
	struct btrfs_inode saved;
	struct btrfs_dir_item *entry;
	struct btrfs_inode_ref *ref;
	struct timespec now;
	uint8_t *bucket = NULL;
	uint8_t *link_item = NULL;
	uint8_t record[sizeof(*entry) + BTRFS_NAME_MAX];
	uint8_t reference[sizeof(*ref) + BTRFS_NAME_MAX];
	uint64_t ino, index = 2, generation;
	uint32_t nodesize, bucket_size = 0, record_size;
	size_t linklen = 0, link_size = 0;
	int error, end_error;

	KASSERT(VOP_ISLOCKED(dir->bn_vnode));
	*vpp = NULL;
	if (!S_ISREG(mode) && !S_ISDIR(mode) && !S_ISLNK(mode))
		return (EOPNOTSUPP);
	if (S_ISLNK(mode) != (link != NULL))
		return (EINVAL);
	if (link != NULL) {
		linklen = strlen(link);
		if (linklen == 0)
			return (ENOENT);
		if (linklen >= MAXPATHLEN)
			return (ENAMETOOLONG);
		link_size = offsetof(struct btrfs_file_extent_item,
		    disk_bytenr) + linklen;
	}
	if (!btrfs_ref_name_valid((const uint8_t *)name, namelen))
		return (EINVAL);
	if (dir->bn_inode.bi_size > UINT64_MAX - namelen * 2)
		return (EOVERFLOW);
	/* Inheritance of ACLs and other directory xattrs is not implemented. */
	if (dir->bn_inode.bi_flags & BTRFS_INODE_NODATACOW)
		return (EOPNOTSUPP);
	rw_enter_write(&bmp->bm_namespace_lock);
	error = btrfs_get_root(bmp, dir->bn_treeid, &root);
	if (error != 0)
		goto out;
	memset(&target, 0, sizeof(target));
	target.objectid = htole64(dir->bn_ino);
	target.type = BTRFS_XATTR_ITEM_KEY;
	error = btrfs_search_lower_bound(root, &target, &path);
	if (error == 0) {
		error = btrfs_path_item(&path, &key, NULL, NULL);
		if (error == 0 && key->objectid == target.objectid &&
		    key->type == target.type)
			error = EOPNOTSUPP;
	}
	btrfs_release_path(&path);
	if (error != 0 && error != ENOENT)
		goto out;

	memset(&target, 0xff, sizeof(target));
	target.objectid = htole64(BTRFS_LAST_FREE_OBJECTID);
	error = btrfs_search_predecessor(root, &target, &path);
	if (error == 0)
		error = btrfs_path_item(&path, &key, NULL, NULL);
	if (error != 0)
		goto out;
	ino = letoh64(key->objectid);
	btrfs_release_path(&path);
	if (ino < BTRFS_FIRST_FREE_OBJECTID ||
	    ino >= BTRFS_LAST_FREE_OBJECTID) {
		error = ENOSPC;
		goto out;
	}
	ino++;

	target.objectid = htole64(dir->bn_ino);
	target.type = BTRFS_DIR_INDEX_KEY;
	target.offset = htole64(UINT64_MAX);
	error = btrfs_search_predecessor(root, &target, &path);
	if (error == 0) {
		error = btrfs_path_item(&path, &key, NULL, NULL);
		if (error != 0)
			goto out;
		if (key->objectid == target.objectid &&
		    key->type == target.type) {
			index = letoh64(key->offset);
			if (index < 2 || index >= INT64_MAX - 1) {
				error = EOVERFLOW;
				goto out;
			}
			index++;
		}
	} else if (error != ENOENT)
		goto out;
	btrfs_release_path(&path);

	nodesize = letoh32(bmp->bm_super.nodesize);
	if (link_size > nodesize - sizeof(struct btrfs_header) -
	    sizeof(struct btrfs_item)) {
		error = ENAMETOOLONG;
		goto out;
	}
	if (link_size != 0)
		link_item = malloc(link_size, M_BTRFS, M_WAITOK | M_ZERO);
	record_size = sizeof(*entry) + namelen;
	bucket = malloc(nodesize, M_BTRFS, M_WAITOK | M_ZERO);
	memset(&hashkey, 0, sizeof(hashkey));
	hashkey.objectid = htole64(dir->bn_ino);
	hashkey.type = BTRFS_DIR_ITEM_KEY;
	/* Btrfs uses raw CRC32C seeded with ~1, without final inversion. */
	hashkey.offset = htole64(crc32c(1, (const uint8_t *)name,
	    namelen) ^ 0xffffffffU);
	error = btrfs_prepare_append(root, &hashkey, bucket, record_size,
	    &bucket_size);
	if (error != 0)
		goto out;

	/* Include the inline target and possible hash bucket delete/reinsert. */
	reservation.btr_metadata = (uint64_t)nodesize *
	    (link != NULL ? 160 : 128);
	error = btrfs_trans_join(bmp, &reservation, &handle);
	if (error != 0)
		goto out;
	generation = handle->bth_transaction->bt_generation;
	getnanotime(&now);
	memset(&inode, 0, sizeof(inode));
	inode.generation = inode.transid = htole64(generation);
	inode.nlink = htole32(1);	/* Btrfs directories also have one link. */
	inode.uid = htole32(uid);
	inode.gid = htole32(gid);
	inode.mode = htole32(mode);
	inode.size = inode.nbytes = htole64(linklen);
	inode.sequence = htole64(1);
	inode.flags = htole64(dir->bn_inode.bi_flags &
	    (BTRFS_INODE_NOCOMPRESS | BTRFS_INODE_COMPRESS));
	btrfs_encode_timespec(&now, &inode.atime);
	inode.ctime = inode.mtime = inode.otime = inode.atime;
	memset(&target, 0, sizeof(target));
	target.objectid = htole64(ino);
	target.type = BTRFS_INODE_ITEM_KEY;
	error = btrfs_insert_item(handle, root, &target, &inode,
	    sizeof(inode));
	if (error != 0)
		goto abort;

	if (link != NULL) {
		extent = (struct btrfs_file_extent_item *)link_item;
		extent->generation = htole64(generation);
		extent->ram_bytes = htole64(linklen);
		extent->type = BTRFS_FILE_EXTENT_INLINE;
		memcpy(link_item + offsetof(struct btrfs_file_extent_item,
		    disk_bytenr), link, linklen);
		target.type = BTRFS_EXTENT_DATA_KEY;
		error = btrfs_insert_item(handle, root, &target, link_item,
		    link_size);
		if (error != 0)
			goto abort;
		target.type = BTRFS_INODE_ITEM_KEY;
	}

	memset(record, 0, sizeof(record));
	entry = (struct btrfs_dir_item *)record;
	entry->location = target;
	entry->transid = htole64(generation);
	entry->name_len = htole16(namelen);
	entry->type = S_ISDIR(mode) ? BTRFS_FT_DIR :
	    S_ISLNK(mode) ? BTRFS_FT_SYMLINK : BTRFS_FT_REG_FILE;
	memcpy(record + sizeof(*entry), name, namelen);
	memcpy(bucket + bucket_size, record, record_size);
	error = btrfs_insert_append(handle, root, &hashkey, bucket,
	    bucket_size, record_size);
	if (error != 0)
		goto abort;
	target.objectid = htole64(dir->bn_ino);
	target.type = BTRFS_DIR_INDEX_KEY;
	target.offset = htole64(index);
	error = btrfs_insert_item(handle, root, &target, record, record_size);
	if (error != 0)
		goto abort;
	ref = (struct btrfs_inode_ref *)reference;
	ref->index = htole64(index);
	ref->name_len = htole16(namelen);
	memcpy(reference + sizeof(*ref), name, namelen);
	target.objectid = htole64(ino);
	target.type = BTRFS_INODE_REF_KEY;
	target.offset = htole64(dir->bn_ino);
	error = btrfs_insert_item(handle, root, &target, reference,
	    sizeof(*ref) + namelen);
	if (error != 0)
		goto abort;

	saved = dir->bn_inode;
	dir->bn_inode.bi_size += namelen * 2;
	dir->bn_inode.bi_mtime = dir->bn_inode.bi_ctime = now;
	dir->bn_inode.bi_sequence++;
	dir->bn_inode.bi_last_dirty_transid = generation;
	dir->bn_inode.bi_dirty_fields |= BTRFS_INODE_DIRTY_SIZE |
	    BTRFS_INODE_DIRTY_MTIME | BTRFS_INODE_DIRTY_CTIME |
	    BTRFS_INODE_DIRTY_SEQUENCE;
	error = btrfs_write_inode(handle, dir);
	if (error == 0)
		error = btrfs_vget_tree(dir->bn_vnode->v_mount,
		    dir->bn_treeid, ino, vpp);
	if (error != 0) {
		dir->bn_inode = saved;
		goto abort;
	}
	VTOBTRFS(*vpp)->bn_inode.bi_last_dirty_transid = generation;
	goto out;
abort:
	btrfs_trans_abort(handle, error);
out:
	btrfs_release_path(&path);
	if (handle != NULL) {
		end_error = btrfs_trans_end(handle);
		if (error == 0)
			error = end_error;
	}
	if (error != 0 && *vpp != NULL) {
		vput(*vpp);
		*vpp = NULL;
	}
	if (bucket != NULL)
		free(bucket, M_BTRFS, nodesize);
	if (link_item != NULL)
		free(link_item, M_BTRFS, link_size);
	rw_exit_write(&bmp->bm_namespace_lock);
	return (error);
}

/*
 * Read a packed item before mutation, leaving room for an additional record.
 * The caller holds the vnode locks protecting the item's contents.
 */
static int
btrfs_prepare_append(struct btrfs_root *root, const struct btrfs_key *key,
    uint8_t *buffer, uint32_t extra, uint32_t *sizep)
{
	struct btrfs_path path = { 0 };
	const uint8_t *data;
	uint32_t size, capacity;
	int error;

	*sizep = 0;
	capacity = letoh32(root->br_super->nodesize) -
	    sizeof(struct btrfs_header) - sizeof(struct btrfs_item);
	error = btrfs_search_slot(root, key, &path);
	if (error == ENOENT)
		error = 0;
	else if (error == 0) {
		error = btrfs_path_item(&path, NULL, &data, &size);
		if (error == 0) {
			if (extra > capacity || size > capacity - extra)
				error = ENOSPC;
			else {
				memcpy(buffer, data, size);
				*sizep = size;
			}
		}
	}
	btrfs_release_path(&path);
	return (error);
}

static int
btrfs_insert_append(struct btrfs_trans_handle *handle, struct btrfs_root *root,
    const struct btrfs_key *key, const uint8_t *buffer, uint32_t oldsize,
    uint32_t extra)
{
	int error = 0;

	/* Delete/reinsert permits the expanded item to split its leaf. */
	if (oldsize != 0)
		error = btrfs_delete_item(handle, root, key);
	if (error == 0)
		error = btrfs_insert_item(handle, root, key, buffer,
		    oldsize + extra);
	return (error);
}

/*
 * Add a name without changing the inode's data extent references: these are
 * keyed by inode, not by directory entry.  Parent and source vnode locks
 * serialize directory indexes and the packed per-parent inode references.
 * All capacity checks precede mutation; a later error aborts the transaction.
 */
int
btrfs_link_inode(struct btrfs_node *dir, struct btrfs_node *node,
    const char *name, size_t namelen)
{
	struct btrfs_mount *bmp = dir->bn_mount;
	struct btrfs_trans_reservation reservation = { 0 };
	struct btrfs_trans_handle *handle = NULL;
	struct btrfs_root *root;
	struct btrfs_path path = { 0 };
	struct btrfs_key hashkey, refkey, indexkey;
	const struct btrfs_key *key;
	struct btrfs_dir_item *entry;
	struct btrfs_inode_ref *ref;
	struct btrfs_inode saved_dir, saved_node;
	struct timespec now;
	uint8_t *bucket = NULL, *reference = NULL;
	uint8_t record[sizeof(*entry) + BTRFS_NAME_MAX];
	uint64_t index = 2, generation;
	uint32_t nodesize, bucket_size, ref_size, record_size, ref_extra;
	int error, end_error;

	KASSERT(VOP_ISLOCKED(dir->bn_vnode));
	KASSERT(VOP_ISLOCKED(node->bn_vnode));
	if (node->bn_mount != bmp || node->bn_treeid != dir->bn_treeid)
		return (EXDEV);
	if (namelen > BTRFS_NAME_MAX ||
	    !btrfs_ref_name_valid((const uint8_t *)name, namelen))
		return (EINVAL);
	if (dir->bn_inode.bi_size > UINT64_MAX - namelen * 2)
		return (EOVERFLOW);
	error = btrfs_get_root(bmp, dir->bn_treeid, &root);
	if (error != 0)
		return (error);
	memset(&indexkey, 0, sizeof(indexkey));
	indexkey.objectid = htole64(dir->bn_ino);
	indexkey.type = BTRFS_DIR_INDEX_KEY;
	indexkey.offset = htole64(UINT64_MAX);
	error = btrfs_search_predecessor(root, &indexkey, &path);
	if (error == 0) {
		error = btrfs_path_item(&path, &key, NULL, NULL);
		if (error != 0)
			goto out;
		if (key->objectid == indexkey.objectid &&
		    key->type == indexkey.type) {
			index = letoh64(key->offset);
			if (index < 2 || index >= INT64_MAX - 1) {
				error = EOVERFLOW;
				goto out;
			}
			index++;
		}
	} else if (error != ENOENT)
		goto out;
	btrfs_release_path(&path);
	indexkey.offset = htole64(index);

	nodesize = letoh32(bmp->bm_super.nodesize);
	bucket = malloc(nodesize, M_BTRFS, M_WAITOK | M_ZERO);
	reference = malloc(nodesize, M_BTRFS, M_WAITOK | M_ZERO);
	record_size = sizeof(*entry) + namelen;
	hashkey = indexkey;
	hashkey.type = BTRFS_DIR_ITEM_KEY;
	hashkey.offset = htole64(crc32c(1, (const uint8_t *)name,
	    namelen) ^ 0xffffffffU);
	error = btrfs_prepare_append(root, &hashkey, bucket, record_size,
	    &bucket_size);
	if (error != 0)
		goto out;
	memset(&refkey, 0, sizeof(refkey));
	refkey.objectid = htole64(node->bn_ino);
	refkey.type = BTRFS_INODE_REF_KEY;
	refkey.offset = htole64(dir->bn_ino);
	ref_extra = sizeof(*ref) + namelen;
	error = btrfs_prepare_append(root, &refkey, reference, ref_extra,
	    &ref_size);
	if (error == ENOSPC)
		error = EMLINK;
	if (error != 0)
		goto out;
	ref = (struct btrfs_inode_ref *)(reference + ref_size);
	ref->index = htole64(index);
	ref->name_len = htole16(namelen);
	memcpy(reference + ref_size + sizeof(*ref), name, namelen);

	/* Two inode updates and two potentially growing packed items. */
	reservation.btr_metadata = (uint64_t)nodesize * 160;
	error = btrfs_trans_join(bmp, &reservation, &handle);
	if (error != 0)
		goto out;
	generation = handle->bth_transaction->bt_generation;
	memset(record, 0, sizeof(record));
	entry = (struct btrfs_dir_item *)record;
	entry->location.objectid = htole64(node->bn_ino);
	entry->location.type = BTRFS_INODE_ITEM_KEY;
	entry->transid = htole64(generation);
	entry->name_len = htole16(namelen);
	switch (node->bn_vnode->v_type) {
	case VREG: entry->type = BTRFS_FT_REG_FILE; break;
	case VLNK: entry->type = BTRFS_FT_SYMLINK; break;
	case VCHR: entry->type = BTRFS_FT_CHRDEV; break;
	case VBLK: entry->type = BTRFS_FT_BLKDEV; break;
	case VFIFO: entry->type = BTRFS_FT_FIFO; break;
	case VSOCK: entry->type = BTRFS_FT_SOCK; break;
	default:
		error = EOPNOTSUPP;
		goto out;
	}
	memcpy(record + sizeof(*entry), name, namelen);
	memcpy(bucket + bucket_size, record, record_size);
	error = btrfs_insert_append(handle, root, &hashkey, bucket,
	    bucket_size, record_size);
	if (error == 0)
		error = btrfs_insert_item(handle, root, &indexkey, record,
		    record_size);
	if (error == 0)
		error = btrfs_insert_append(handle, root, &refkey, reference,
		    ref_size, ref_extra);
	if (error != 0)
		goto abort;

	saved_dir = dir->bn_inode;
	saved_node = node->bn_inode;
	getnanotime(&now);
	dir->bn_inode.bi_size += namelen * 2;
	dir->bn_inode.bi_mtime = dir->bn_inode.bi_ctime = now;
	dir->bn_inode.bi_sequence++;
	dir->bn_inode.bi_last_dirty_transid = generation;
	dir->bn_inode.bi_dirty_fields |= BTRFS_INODE_DIRTY_SIZE |
	    BTRFS_INODE_DIRTY_MTIME | BTRFS_INODE_DIRTY_CTIME |
	    BTRFS_INODE_DIRTY_SEQUENCE;
	node->bn_inode.bi_nlink++;
	node->bn_inode.bi_ctime = now;
	node->bn_inode.bi_last_dirty_transid = generation;
	node->bn_inode.bi_dirty_fields |= BTRFS_INODE_DIRTY_NLINK |
	    BTRFS_INODE_DIRTY_CTIME;
	error = btrfs_write_inode(handle, node);
	if (error == 0)
		error = btrfs_write_inode(handle, dir);
	if (error != 0) {
		dir->bn_inode = saved_dir;
		node->bn_inode = saved_node;
		goto abort;
	}
	goto out;
abort:
	btrfs_trans_abort(handle, error);
out:
	btrfs_release_path(&path);
	if (handle != NULL) {
		end_error = btrfs_trans_end(handle);
		if (error == 0)
			error = end_error;
	}
	if (bucket != NULL)
		free(bucket, M_BTRFS, nodesize);
	if (reference != NULL)
		free(reference, M_BTRFS, nodesize);
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
	struct btrfs_root *root;
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
	error = btrfs_get_root(bmp, BTRFS_ROOT_TREE_OBJECTID, &root);
	if (error != 0)
		return (error);
	error = btrfs_search_lower_bound(root, &target, &path);
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
			    transid > path.bp_view_generation ||
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
    uint64_t view_generation, const struct btrfs_key *key,
    const uint8_t *data, uint32_t item_size, struct btrfs_file_extent *decoded)
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
	    generation > view_generation ||
	    extent->compression > BTRFS_COMPRESS_ZSTD ||
	    extent->type > BTRFS_FILE_EXTENT_PREALLOC)
		return (EINVAL);

	decoded->bfe_logical = letoh64(key->offset);
	decoded->bfe_compression = extent->compression;
	decoded->bfe_encryption = extent->encryption;
	decoded->bfe_other_encoding = letoh16(extent->other_encoding);
	decoded->bfe_type = extent->type;
	decoded->bfe_ram_bytes = ram_bytes;
	decoded->bfe_item_present = 1;

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
			error = btrfs_decode_file_extent(bmp,
			    path->bp_view_generation, key, data, item_size,
			    &decoded);
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
			error = btrfs_decode_file_extent(bmp,
			    path->bp_view_generation, key, data, item_size,
			    &decoded);
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
