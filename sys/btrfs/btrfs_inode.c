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
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/vnode.h>

#include <btrfs/btrfs_var.h>
#include <btrfs/btrfs_data.h>
#include <btrfs/btrfs_dir.h>
#include <lib/libkern/crc32c.h>

static void	btrfs_decode_timespec(const struct btrfs_timespec *,
		    struct timespec *);
static void	btrfs_encode_timespec(const struct timespec *,
		    struct btrfs_timespec *);
static void	btrfs_decode_inode(const struct btrfs_inode_item *,
		    struct btrfs_inode *);
static uint8_t	btrfs_dir_type(mode_t);

static uint8_t
btrfs_dir_type(mode_t mode)
{
	switch (IFTOVT(mode)) {
	case VREG: return (BTRFS_FT_REG_FILE);
	case VDIR: return (BTRFS_FT_DIR);
	case VLNK: return (BTRFS_FT_SYMLINK);
	case VCHR: return (BTRFS_FT_CHRDEV);
	case VBLK: return (BTRFS_FT_BLKDEV);
	case VFIFO: return (BTRFS_FT_FIFO);
	case VSOCK: return (BTRFS_FT_SOCK);
	default: return (BTRFS_FT_UNKNOWN);
	}
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

/* Mutable inode state is host-endian; encoding preserves unmodeled fields. */
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
	    IFTOVT(inode->bi_mode) == VNON ||
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
 * Disk inodes store Linux's in-kernel dev_t, (major << 20) | minor; send
 * streams use the separate userspace new_encode_dev encoding. Preserve the
 * major/minor values, although driver assignments are OS-specific. Creation
 * rejects minors above 20 bits; loading rejects majors outside OpenBSD's
 * 8-bit range.
 */
int
btrfs_decode_rdev(uint64_t disk, dev_t *dev)
{
	uint32_t maj, min;

	maj = disk >> 20;
	min = disk & 0xfffff;
	if (disk > UINT32_MAX || maj > 0xff)
		return (EOVERFLOW);
	*dev = makedev(maj, min);
	return (0);
}

/*
 * The parent vnode lock protects its index and hash buckets. The namespace
 * lock protects inode number selection through vnode publication. Preallocate
 * the vnode before mutation and register aliases after ending the handle.
 * Namespace items, the optional symlink extent, and the parent inode belong
 * to one transaction; errors after the first mutation abort it.
 *
 * Linux xattrs remain on the parent and do not prevent creation or propagate
 * to the child. New inodes inherit the parent group, compression flags and
 * NODATACOW; regular children of NODATACOW directories also get NODATASUM.
 * Writes nevertheless use uncompressed COW data.
 */
int
btrfs_create_inode(struct btrfs_node *dir, const char *name, size_t namelen,
    mode_t mode, uid_t uid, gid_t gid, dev_t dev, const char *link,
    struct vnode **vpp)
{
	struct btrfs_fs *bmp = dir->bn_mount;
	struct btrfs_trans_reservation reservation = { 0 };
	struct btrfs_trans_handle *handle = NULL;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	struct btrfs_key target;
	const struct btrfs_key *key;
	struct btrfs_inode_item inode;
	struct btrfs_file_extent_item *extent;
	struct btrfs_inode saved;
	struct btrfs_dir_item entry = { 0 };
	struct btrfs_name_plan *plan = NULL;
	struct timespec now;
	uint8_t *link_item = NULL;
	uint64_t ino, index, generation, rdev = 0, flags;
	uint32_t nodesize;
	size_t linklen = 0, link_size = 0;
	int error, end_error;

	KASSERT(VOP_ISLOCKED(dir->bn_vnode));
	*vpp = NULL;
	if (!S_ISREG(mode) && !S_ISDIR(mode) && !S_ISLNK(mode) &&
	    !S_ISFIFO(mode) && !S_ISSOCK(mode) &&
	    !S_ISCHR(mode) && !S_ISBLK(mode))
		return (EOPNOTSUPP);
	if (S_ISCHR(mode) || S_ISBLK(mode)) {
		if (minor(dev) > 0xfffff)
			return (EOVERFLOW);
		rdev = ((uint64_t)major(dev) << 20) | minor(dev);
	}
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
	if (!btrfs_name_valid((const uint8_t *)name, namelen))
		return (EINVAL);
	if (dir->bn_inode.bi_nlink == 0)
		return (ENOENT);
	if (dir->bn_inode.bi_size > UINT64_MAX - namelen * 2)
		return (EOVERFLOW);
	rw_enter_write(&bmp->bm_namespace_lock);
	error = btrfs_get_root(bmp, dir->bn_treeid, &root);
	if (error != 0)
		goto out;
	/* Linux xattrs are opaque metadata; creation does not inherit them. */
	memset(&target, 0xff, sizeof(target));
	target.objectid = htole64(BTRFS_LAST_FREE_OBJECTID);
	error = btrfs_search_predecessor(root, &target, &path);
	if (error == 0)
		error = btrfs_path_item(&path, &key, NULL, NULL);
	if (error != 0)
		goto out;
	ino = letoh64(key->objectid);
	btrfs_release_path(&path);
	ino = MAX(ino, root->br_last_ino);
	if (ino < BTRFS_FIRST_FREE_OBJECTID ||
	    ino >= BTRFS_LAST_FREE_OBJECTID) {
		error = ENOSPC;
		goto out;
	}
	ino++;
	root->br_last_ino = ino;

	error = btrfs_next_dir_index(root, dir->bn_ino, &index);
	if (error != 0)
		goto out;

	nodesize = letoh32(bmp->bm_super.nodesize);
	if (link_size > nodesize - sizeof(struct btrfs_header) -
	    sizeof(struct btrfs_item)) {
		error = ENAMETOOLONG;
		goto out;
	}
	if (link_size != 0)
		link_item = malloc(link_size, M_BTRFS, M_WAITOK | M_ZERO);
	plan = btrfs_name_plan_alloc(root);
	entry.location.objectid = htole64(ino);
	entry.location.type = BTRFS_INODE_ITEM_KEY;
	entry.type = btrfs_dir_type(mode);
	error = btrfs_plan_add(plan, dir->bn_ino, ino, name, namelen,
	    index, &entry);
	if (error != 0)
		goto out;

	/* Only type and device identity are needed before encoding the inode. */
	memset(&saved, 0, sizeof(saved));
	saved.bi_mode = mode;
	saved.bi_rdev = rdev;
	error = btrfs_alloc_node(dir->bn_vnode->v_mount, root, ino, &saved, vpp);
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
	inode.rdev = htole64(rdev);
	inode.size = inode.nbytes = htole64(linklen);
	inode.sequence = htole64(1);
	flags = dir->bn_inode.bi_flags & (BTRFS_INODE_NOCOMPRESS |
	    BTRFS_INODE_COMPRESS | BTRFS_INODE_NODATACOW);
	if ((flags & BTRFS_INODE_NODATACOW) && S_ISREG(mode))
		flags |= BTRFS_INODE_NODATASUM;
	inode.flags = htole64(flags);
	btrfs_encode_timespec(&now, &inode.atime);
	inode.ctime = inode.mtime = inode.otime = inode.atime;
	btrfs_decode_inode(&inode, &VTOBTRFS(*vpp)->bn_inode);
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

	error = btrfs_name_plan_apply(handle, plan);
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
	if (error == 0) {
		error = btrfs_init_node(vpp);
		if (error == EEXIST) {
			/* A lookup may have instantiated the completed inode. */
			vput(*vpp);
			vgone(*vpp);
			*vpp = NULL;
			error = btrfs_vget_tree(dir->bn_vnode->v_mount,
			    dir->bn_treeid, ino, vpp);
			if (error == 0)
				VTOBTRFS(*vpp)->bn_inode.bi_last_dirty_transid =
				    generation;
		}
	}
	if (error != 0 && *vpp != NULL) {
		vput(*vpp);
		vgone(*vpp);
		*vpp = NULL;
	}
	btrfs_name_plan_free(plan);
	if (link_item != NULL)
		free(link_item, M_BTRFS, link_size);
	rw_exit_write(&bmp->bm_namespace_lock);
	return (error);
}

/*
 * Add a name without changing the inode's data extent references: these are
 * keyed by inode, not by directory entry.  Parent and source vnode locks
 * serialize directory indexes and the packed per-parent inode references.
 * All capacity checks precede mutation; a later error aborts the transaction.
 * Packed inode references overflow into extended references only when
 * EXTENDED_IREF is enabled; otherwise overflow returns EMLINK. Each hash
 * bucket must still fit in one item.
 */
int
btrfs_link_inode(struct btrfs_node *dir, struct btrfs_node *node,
    const char *name, size_t namelen)
{
	struct btrfs_fs *bmp = dir->bn_mount;
	struct btrfs_trans_reservation reservation = { 0 };
	struct btrfs_trans_handle *handle = NULL;
	struct btrfs_root *root;
	struct btrfs_name_plan *plan = NULL;
	struct btrfs_dir_item entry = { 0 };
	struct btrfs_inode saved_dir, saved_node;
	struct timespec now;
	uint64_t index, generation;
	uint32_t nodesize;
	int error, end_error;

	KASSERT(VOP_ISLOCKED(dir->bn_vnode));
	KASSERT(VOP_ISLOCKED(node->bn_vnode));
	if (node->bn_mount != bmp || node->bn_treeid != dir->bn_treeid)
		return (EXDEV);
	if (dir->bn_inode.bi_nlink == 0)
		return (ENOENT);
	if (namelen > BTRFS_NAME_MAX ||
	    !btrfs_name_valid((const uint8_t *)name, namelen))
		return (EINVAL);
	if (dir->bn_inode.bi_size > UINT64_MAX - namelen * 2)
		return (EOVERFLOW);
	error = btrfs_get_root(bmp, dir->bn_treeid, &root);
	if (error != 0)
		return (error);
	entry.type = btrfs_dir_type(node->bn_inode.bi_mode);
	if (entry.type == BTRFS_FT_UNKNOWN || entry.type == BTRFS_FT_DIR)
		return (EOPNOTSUPP);
	error = btrfs_next_dir_index(root, dir->bn_ino, &index);
	if (error != 0)
		goto out;

	nodesize = letoh32(bmp->bm_super.nodesize);
	plan = btrfs_name_plan_alloc(root);
	entry.location.objectid = htole64(node->bn_ino);
	entry.location.type = BTRFS_INODE_ITEM_KEY;
	error = btrfs_plan_add(plan, dir->bn_ino, node->bn_ino,
	    name, namelen, index, &entry);
	if (error != 0)
		goto out;

	/* Two inode updates and two potentially growing packed items. */
	reservation.btr_metadata = (uint64_t)nodesize * 160;
	error = btrfs_trans_join(bmp, &reservation, &handle);
	if (error != 0)
		goto out;
	generation = handle->bth_transaction->bt_generation;
	error = btrfs_name_plan_apply(handle, plan);
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
	if (handle != NULL) {
		end_error = btrfs_trans_end(handle);
		if (error == 0)
			error = end_error;
	}
	btrfs_name_plan_free(plan);
	return (error);
}

/*
 * Remove one packed inode reference in a private copy, retaining its index
 * for the corresponding directory-index deletion. An inode may have both
 * ordinary and extended references to the same parent.
 */
static int
btrfs_remove_ref(struct btrfs_key *key,
    uint64_t parent, const char *name, size_t namelen, uint8_t *buffer,
    uint32_t *sizep, uint64_t *indexp)
{
	const struct btrfs_inode_ref *ref;
	const struct btrfs_inode_extref *extref;
	uint64_t record_parent, index;
	uint32_t offset, header, length, match = 0, match_size = 0;
	uint16_t name_len;
	header = key->type == BTRFS_INODE_REF_KEY ?
	    sizeof(*ref) : sizeof(*extref);
	for (offset = 0; offset < *sizep; offset += length) {
		if (*sizep - offset < header)
			return (EINVAL);
		if (key->type == BTRFS_INODE_REF_KEY) {
			ref = (const struct btrfs_inode_ref *)(buffer + offset);
			record_parent = letoh64(key->offset);
			index = letoh64(ref->index);
			name_len = letoh16(ref->name_len);
		} else {
			extref = (const struct btrfs_inode_extref *)
			    (buffer + offset);
			record_parent = letoh64(extref->parent_objectid);
			index = letoh64(extref->index);
			name_len = letoh16(extref->name_len);
		}
		length = header + name_len;
		if (length > *sizep - offset || index < 2 ||
		    !btrfs_name_valid(buffer + offset + header, name_len))
			return (EINVAL);
		if (record_parent == parent && name_len == namelen &&
		    memcmp(buffer + offset + header, name, namelen) == 0) {
			if (match_size != 0)
				return (EINVAL);
			match = offset;
			match_size = length;
			*indexp = index;
		}
	}
	if (match_size == 0)
		return (ENOENT);
	memmove(buffer + match, buffer + match + match_size,
	    *sizep - match - match_size);
	*sizep -= match_size;
	return (0);
}

static int
btrfs_empty_directory(struct btrfs_node *dir, struct btrfs_node *node)
{
	struct btrfs_key target = { 0 };
	struct btrfs_path path = { 0 };
	const struct btrfs_key *key;
	uint64_t parent;
	int error;

	if (node->bn_inode.bi_size != 0)
		return (ENOTEMPTY);
	if (node->bn_inode.bi_nlink != 1 || node->bn_inode.bi_nbytes != 0)
		return (EINVAL);
	error = btrfs_find_dir_parent(dir->bn_root, node->bn_ino, &parent);
	if (error != 0)
		return (error);
	if (parent != dir->bn_ino)
		return (EINVAL);
	/* Check both namespace key types, independently of inode size. */
	target.objectid = htole64(node->bn_ino);
	target.type = BTRFS_DIR_ITEM_KEY;
	error = btrfs_search_lower_bound(dir->bn_root, &target, &path);
	if (error == 0) {
		error = btrfs_path_item(&path, &key, NULL, NULL);
		if (error == 0 && key->objectid == target.objectid &&
		    (key->type == BTRFS_DIR_ITEM_KEY ||
		    key->type == BTRFS_DIR_INDEX_KEY))
			error = ENOTEMPTY;
	} else if (error == ENOENT)
		error = 0;
	btrfs_release_path(&path);
	return (error);
}

/*
 * Parent and target locks protect all three namespace records. Validate the
 * hash bucket, reference and index together before reserving or changing any
 * tree. Final-link removal persists a marker for restartable orphan recovery.
 */
int
btrfs_unlink_inode(struct btrfs_node *dir, struct btrfs_node *node,
    const char *name, size_t namelen)
{
	struct btrfs_fs *bmp = dir->bn_mount;
	struct btrfs_trans_reservation reservation = { 0 };
	struct btrfs_trans_handle *handle = NULL;
	struct btrfs_root *root = dir->bn_root;
	struct btrfs_name_plan *plan;
	struct btrfs_inode saved_dir, saved_node;
	struct timespec now;
	uint64_t index = 0, generation;
	uint32_t nodesize;
	int error, end_error;

	KASSERT(VOP_ISLOCKED(dir->bn_vnode));
	KASSERT(VOP_ISLOCKED(node->bn_vnode));
	if (node->bn_mount != bmp || node->bn_treeid != dir->bn_treeid)
		return (EXDEV);
	if (node->bn_inode.bi_nlink == 0)
		return (ENOENT);
	if (node->bn_vnode->v_type == VDIR) {
		error = btrfs_empty_directory(dir, node);
		if (error != 0)
			return (error);
	}
	if (namelen > BTRFS_NAME_MAX ||
	    !btrfs_name_valid((const uint8_t *)name, namelen) ||
	    dir->bn_inode.bi_size < namelen * 2)
		return (EINVAL);
	nodesize = letoh32(bmp->bm_super.nodesize);
	plan = btrfs_name_plan_alloc(root);
	error = btrfs_plan_remove(plan, dir->bn_ino, node->bn_ino,
	    name, namelen, &index,
	    NULL);
	if (error != 0)
		goto out;

	/* Three deletions/replacements and two inode updates, plus refs. */
	reservation.btr_metadata = (uint64_t)nodesize * 160;
	reservation.btr_reclaim = 1;
	error = btrfs_trans_join(bmp, &reservation, &handle);
	if (error != 0)
		goto out;
	if (node->bn_inode.bi_nlink == 1) {
		struct btrfs_key orphan = { 0 };

		orphan.objectid = htole64(BTRFS_ORPHAN_OBJECTID);
		orphan.type = BTRFS_ORPHAN_ITEM_KEY;
		orphan.offset = htole64(node->bn_ino);
		error = btrfs_insert_item(handle, root, &orphan, NULL, 0);
		if (error != 0)
			goto abort;
	}
	error = btrfs_name_plan_apply(handle, plan);
	if (error != 0)
		goto abort;
	saved_dir = dir->bn_inode;
	saved_node = node->bn_inode;
	generation = handle->bth_transaction->bt_generation;
	getnanotime(&now);
	dir->bn_inode.bi_size -= namelen * 2;
	dir->bn_inode.bi_mtime = dir->bn_inode.bi_ctime = now;
	dir->bn_inode.bi_sequence++;
	dir->bn_inode.bi_last_dirty_transid = generation;
	dir->bn_inode.bi_dirty_fields |= BTRFS_INODE_DIRTY_SIZE |
	    BTRFS_INODE_DIRTY_MTIME | BTRFS_INODE_DIRTY_CTIME |
	    BTRFS_INODE_DIRTY_SEQUENCE;
	node->bn_inode.bi_nlink--;
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
	if (handle != NULL) {
		end_error = btrfs_trans_end(handle);
		if (error == 0)
			error = end_error;
	}
	btrfs_name_plan_free(plan);
	return (error);
}

uint64_t
btrfs_extref_hash(uint64_t parent, const char *name, size_t len)
{
	/* Extended inode references seed raw CRC32C with the parent ID. */
	return (crc32c((uint32_t)parent ^ 0xffffffffU,
	    (const uint8_t *)name, len) ^ 0xffffffffU);
}

/* Inode references supply the index; directory preparation checks its pair. */
int
btrfs_plan_remove(struct btrfs_name_plan *plan, uint64_t parent,
    uint64_t ino, const char *name, size_t len, uint64_t *indexp,
    struct btrfs_dir_item *record)
{
	struct btrfs_name_edit *ref;
	int error;

	error = btrfs_name_edit(plan, ino, BTRFS_INODE_REF_KEY,
	    parent, &ref);
	if (error == 0)
		error = btrfs_remove_ref(&ref->key, parent, name, len,
		    ref->data, &ref->size, indexp);
	if (error == ENOENT &&
	    (letoh64(plan->root->br_super->incompat_flags) &
	    BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF)) {
		error = btrfs_name_edit(plan, ino,
		    BTRFS_INODE_EXTREF_KEY,
		    btrfs_extref_hash(parent, name, len), &ref);
		if (error == 0)
			error = btrfs_remove_ref(&ref->key, parent, name,
			    len, ref->data, &ref->size, indexp);
	}
	if (error != 0)
		return (error);
	ref->dirty = 1;
	return (btrfs_plan_dir_remove(plan, parent, name, len,
	    *indexp, ino, BTRFS_INODE_ITEM_KEY, record));
}

int
btrfs_plan_add(struct btrfs_name_plan *plan, uint64_t parent, uint64_t ino,
    const char *name, size_t len, uint64_t cookie,
    const struct btrfs_dir_item *record)
{
	struct btrfs_name_edit *ref;
	struct btrfs_inode_ref *iref;
	struct btrfs_inode_extref *extref;
	uint32_t extra = sizeof(*iref) + len;
	int error;

	error = btrfs_plan_dir_add(plan, parent, name, len, cookie, record);
	if (error != 0)
		return (error);
	error = btrfs_name_edit(plan, ino, BTRFS_INODE_REF_KEY, parent, &ref);
	if (error != 0)
		return (error);
	if (ref->size > plan->capacity - extra &&
	    (letoh64(plan->root->br_super->incompat_flags) &
	    BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF)) {
		extra = sizeof(*extref) + len;
		error = btrfs_name_edit(plan, ino,
		    BTRFS_INODE_EXTREF_KEY,
		    btrfs_extref_hash(parent, name, len), &ref);
		if (error != 0)
			return (error);
	}
	if (ref->size > plan->capacity - extra)
		return (EMLINK);
	if (ref->key.type == BTRFS_INODE_REF_KEY) {
		iref = (struct btrfs_inode_ref *)(ref->data + ref->size);
		iref->index = htole64(cookie);
		iref->name_len = htole16(len);
		memcpy(iref + 1, name, len);
	} else {
		extref = (struct btrfs_inode_extref *)(ref->data + ref->size);
		extref->parent_objectid = htole64(parent);
		extref->index = htole64(cookie);
		extref->name_len = htole16(len);
		memcpy(extref->name, name, len);
	}
	ref->size += extra;
	ref->dirty = 1;
	return (0);
}

int
btrfs_rename_inode(struct btrfs_node *fdir, struct btrfs_node *node,
    const char *fname, size_t flen, struct btrfs_node *tdir,
    struct btrfs_node *target, const char *tname, size_t tlen)
{
	struct btrfs_fs *bmp = fdir->bn_mount;
	struct btrfs_name_plan *plan;
	struct btrfs_trans_reservation reservation = { 0 };
	struct btrfs_trans_handle *handle = NULL;
	struct btrfs_key orphan = { 0 };
	struct btrfs_node *nodes[4] = { fdir, tdir, node, target };
	struct btrfs_inode saved[4];
	struct timespec now;
	struct btrfs_dir_item record;
	uint64_t cookie, tcookie = 2, generation, fsize, tsize;
	unsigned int i, j;
	int error, end_error;

	for (i = 0; i < nitems(nodes); i++) {
		if (nodes[i] == NULL)
			continue;
		KASSERT(VOP_ISLOCKED(nodes[i]->bn_vnode));
		if (nodes[i]->bn_mount != bmp ||
		    nodes[i]->bn_treeid != fdir->bn_treeid)
			return (EXDEV);
		if (nodes[i]->bn_inode.bi_nlink == 0)
			return (ENOENT);
	}
	KASSERT(node != target && node != fdir && node != tdir);
	if (flen > BTRFS_NAME_MAX || tlen > BTRFS_NAME_MAX ||
	    !btrfs_name_valid((const uint8_t *)fname, flen) ||
	    !btrfs_name_valid((const uint8_t *)tname, tlen))
		return (EINVAL);
	if (target != NULL && target->bn_vnode->v_type == VDIR) {
		error = btrfs_empty_directory(tdir, target);
		if (error != 0)
			return (error);
	}
	fsize = fdir->bn_inode.bi_size;
	tsize = tdir->bn_inode.bi_size;
	if (fsize < flen * 2 || (target != NULL && tsize < tlen * 2))
		return (EINVAL);
	fsize -= flen * 2;
	if (fdir == tdir)
		tsize = fsize;
	if (target != NULL) {
		if (tsize < tlen * 2)
			return (EINVAL);
		tsize -= tlen * 2;
	}
	if (tsize > UINT64_MAX - tlen * 2)
		return (EOVERFLOW);
	tsize += tlen * 2;
	plan = btrfs_name_plan_alloc(fdir->bn_root);
	error = btrfs_plan_remove(plan, fdir->bn_ino, node->bn_ino,
	    fname, flen, &cookie,
	    &record);
	if (error != 0)
		goto out;
	if (target != NULL) {
		error = btrfs_plan_remove(plan, tdir->bn_ino, target->bn_ino,
		    tname, tlen,
		    &tcookie, NULL);
		if (error != 0)
			goto out;
	} else if (fdir == tdir)
		tcookie = cookie;
	else {
		error = btrfs_next_dir_index(plan->root, tdir->bn_ino,
		    &tcookie);
		if (error != 0)
			goto out;
	}
	error = btrfs_plan_add(plan, tdir->bn_ino, node->bn_ino, tname,
	    tlen, tcookie, &record);
	if (error != 0)
		goto out;

	/* Up to nine namespace edits, four inodes, and a cleanup marker. */
	reservation.btr_metadata = (uint64_t)plan->nodesize * 384;
	error = btrfs_trans_join(bmp, &reservation, &handle);
	if (error != 0)
		goto out;
	generation = handle->bth_transaction->bt_generation;
	if (target != NULL && target->bn_inode.bi_nlink == 1) {
		orphan.objectid = htole64(BTRFS_ORPHAN_OBJECTID);
		orphan.type = BTRFS_ORPHAN_ITEM_KEY;
		orphan.offset = htole64(target->bn_ino);
		error = btrfs_insert_item(handle, plan->root, &orphan, NULL, 0);
		if (error != 0)
			goto abort;
	}
	error = btrfs_name_plan_apply(handle, plan);
	if (error != 0)
		goto abort;
	for (i = 0; i < nitems(nodes); i++)
		if (nodes[i] != NULL)
			saved[i] = nodes[i]->bn_inode;
	getnanotime(&now);
	fdir->bn_inode.bi_size = fsize;
	tdir->bn_inode.bi_size = tsize;
	if (target != NULL)
		target->bn_inode.bi_nlink--;
	for (i = 0; i < nitems(nodes); i++) {
		if (nodes[i] == NULL || (i == 1 && fdir == tdir))
			continue;
		nodes[i]->bn_inode.bi_ctime = now;
		nodes[i]->bn_inode.bi_last_dirty_transid = generation;
		nodes[i]->bn_inode.bi_dirty_fields |= BTRFS_INODE_DIRTY_CTIME;
		if (i < 2) {
			nodes[i]->bn_inode.bi_mtime = now;
			nodes[i]->bn_inode.bi_sequence++;
			nodes[i]->bn_inode.bi_dirty_fields |=
			    BTRFS_INODE_DIRTY_SIZE | BTRFS_INODE_DIRTY_MTIME |
			    BTRFS_INODE_DIRTY_SEQUENCE;
		}
		if (i == 3)
			nodes[i]->bn_inode.bi_dirty_fields |=
			    BTRFS_INODE_DIRTY_NLINK;
		error = btrfs_write_inode(handle, nodes[i]);
		if (error != 0) {
			for (j = 0; j < nitems(nodes); j++)
				if (nodes[j] != NULL)
					nodes[j]->bn_inode = saved[j];
			goto abort;
		}
	}
	goto out;
abort:
	btrfs_trans_abort(handle, error);
out:
	if (handle != NULL) {
		end_error = btrfs_trans_end(handle);
		if (error == 0)
			error = end_error;
	}
	btrfs_name_plan_free(plan);
	return (error);
}

static int
btrfs_read_orphan_inode(struct btrfs_root *root, uint64_t ino,
    struct btrfs_inode_item *item)
{
	struct btrfs_key key = { 0 };
	struct btrfs_path path = { 0 };
	const uint8_t *data;
	uint32_t size;
	int error;

	key.objectid = htole64(ino);
	key.type = BTRFS_INODE_ITEM_KEY;
	error = btrfs_search_slot(root, &key, &path);
	if (error == 0)
		error = btrfs_path_item(&path, NULL, &data, &size);
	if (error == 0) {
		if (size != sizeof(*item))
			error = EINVAL;
		else {
			memcpy(item, data, size);
			/* Verity is excluded by the writable feature mask. */
			if (item->nlink != 0 &&
			    IFTOVT(letoh32(item->mode)) != VREG)
				error = EOPNOTSUPP;
			else if (letoh64(item->generation) >
			    path.bp_view_generation ||
			    letoh64(item->transid) > path.bp_view_generation)
				error = EINVAL;
		}
	}
	btrfs_release_path(&path);
	return (error);
}

/*
 * Validate recovery before modifying a filesystem at mount. An unlinked
 * inode has only data mappings and xattrs. A linked regular inode instead
 * records an interrupted truncate; its names and retained prefix survive.
 */
static int
btrfs_validate_orphan(struct btrfs_root *root, uint64_t ino, uint64_t *itemsp)
{
	struct btrfs_inode_item item;
	struct btrfs_file_extent extent;
	struct btrfs_key target = { 0 };
	struct btrfs_path path = { 0 };
	const struct btrfs_key *key;
	const uint8_t *data;
	uint64_t nbytes = 0, items = 0, cut;
	uint32_t size, sectorsize;
	int error;

	/*
	 * A read-only snapshot can inherit orphan and truncate markers.
	 * Internal recovery may retire them; VFS mutation still enforces the
	 * root's read-only flag before reaching this cleanup engine.
	 */
	error = btrfs_read_orphan_inode(root, ino, &item);
	if (error != 0)
		return (error);
	if (IFTOVT(letoh32(item.mode)) == VNON ||
	    IFTOVT(letoh32(item.mode)) == VBAD)
		return (EINVAL);
	if (letoh64(item.size) > LLONG_MAX)
		return (EINVAL);
	sectorsize = letoh32(root->br_mount->bm_super.sectorsize);
	cut = roundup(letoh64(item.size), sectorsize);
	target.objectid = htole64(ino);
	target.type = BTRFS_INODE_ITEM_KEY + 1;
	error = btrfs_search_lower_bound(root, &target, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &key, &data, &size);
		if (error != 0 || key->objectid != target.objectid)
			break;
		if (key->type == BTRFS_EXTENT_DATA_KEY) {
			error = btrfs_decode_file_extent(root->br_mount,
			    path.bp_view_generation, key, data, size, &extent);
			if (error != 0)
				break;
			if (item.nlink != 0) {
				if (extent.bfe_type == BTRFS_FILE_EXTENT_INLINE) {
					if (extent.bfe_length != letoh64(item.size))
						error = EOPNOTSUPP;
				} else if (extent.bfe_logical +
				    extent.bfe_length > cut &&
				    (extent.bfe_encryption != 0 ||
				    extent.bfe_other_encoding != 0 ||
				    (extent.bfe_logical & (sectorsize - 1)) != 0 ||
				    (extent.bfe_length & (sectorsize - 1)) != 0 ||
				    (extent.bfe_logical < cut &&
				    extent.bfe_logical + extent.bfe_length > cut &&
				    extent.bfe_compression != BTRFS_COMPRESS_NONE &&
				    extent.bfe_compression != BTRFS_COMPRESS_ZSTD)))
					error = EOPNOTSUPP;
				if (error != 0)
					break;
			}
			if (extent.bfe_type != BTRFS_FILE_EXTENT_HOLE) {
				if (extent.bfe_length > UINT64_MAX - nbytes) {
					error = EINVAL;
					break;
				}
				nbytes += extent.bfe_length;
			}
		} else if (key->type != BTRFS_XATTR_ITEM_KEY &&
		    !(item.nlink != 0 && (key->type == BTRFS_INODE_REF_KEY ||
		    key->type == BTRFS_INODE_EXTREF_KEY))) {
			error = EOPNOTSUPP;
			break;
		}
		items++;
		error = btrfs_next_item(&path);
	}
	btrfs_release_path(&path);
	if (error == ENOENT)
		error = 0;
	if (error == 0 && nbytes != letoh64(item.nbytes))
		error = EINVAL;
	if (error == 0 && itemsp != NULL)
		*itemsp = items;
	return (error);
}

int
btrfs_check_orphan(struct btrfs_root *root, uint64_t ino)
{
	return (btrfs_validate_orphan(root, ino, NULL));
}

/*
 * The inode's durable size is the deletion cursor's lower bound. Re-search
 * it in every batch, shortening a crossing mapping once and deleting whole
 * items thereafter. Ordered data must already have been committed.
 */
static int
btrfs_truncate_batch(struct btrfs_trans_handle *handle, struct btrfs_root *root,
    uint64_t ino, unsigned int limit, int *finished, uint64_t *remaining)
{
	struct btrfs_inode_item inode;
	struct btrfs_file_extent extent;
	struct btrfs_extent_plan plan;
	struct btrfs_path path = { 0 };
	struct btrfs_key key = { 0 };
	uint64_t cursor, cut, end, left, nbytes;
	uint32_t sectorsize;
	unsigned int count = 0;
	int error;

	*finished = 0;
	error = btrfs_read_orphan_inode(root, ino, &inode);
	if (error != 0)
		return (error);
	sectorsize = letoh32(root->br_mount->bm_super.sectorsize);
	cut = roundup(letoh64(inode.size), sectorsize);
	cursor = cut;
	nbytes = letoh64(inode.nbytes);
	while (count < limit && cursor < UINT64_MAX) {
		error = btrfs_find_file_extent(root->br_mount, root, &path,
		    ino, cursor, UINT64_MAX, &extent);
		if (error != 0)
			goto out;
		end = extent.bfe_logical + extent.bfe_length;
		if (end <= cursor) {
			error = EINVAL;
			goto out;
		}
		cursor = end;
		if (!extent.bfe_item_present) {
			btrfs_release_path(&path);
			continue;
		}
		if (extent.bfe_type == BTRFS_FILE_EXTENT_INLINE) {
			error = EOPNOTSUPP;
			goto out;
		}
		left = cut > extent.bfe_logical ? cut - extent.bfe_logical : 0;
		btrfs_release_path(&path);
		error = btrfs_extent_plan_prepare(&plan, root, ino, &extent,
		    extent.bfe_logical + left, extent.bfe_length - left,
		    NULL, nbytes);
		if (error != 0)
			return (error);
		error = btrfs_extent_plan_apply(handle, &plan);
		if (error != 0)
			return (error);
		nbytes = plan.nbytes;
		count++;
	}
	*finished = cursor == UINT64_MAX;
	inode.nbytes = htole64(nbytes);
	inode.transid = htole64(handle->bth_transaction->bt_generation);
	key.objectid = htole64(ino);
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;
	error = btrfs_replace_item(handle, root, &key, &inode, sizeof(inode));
	if (error == 0 && *finished && inode.nlink != 0) {
		key.objectid = htole64(BTRFS_ORPHAN_OBJECTID);
		key.type = BTRFS_ORPHAN_ITEM_KEY;
		key.offset = htole64(ino);
		error = btrfs_delete_item(handle, root, &key);
	}
	*remaining = nbytes;
	return (error);
out:
	btrfs_release_path(&path);
	return (error);
}

/*
 * Reap at most limit items in a reserved handle. The inode and orphan marker
 * survive every intermediate commit, and nbytes tracks remaining mappings.
 * The final handle removes both together. No vnode is needed during recovery.
 */
static int
btrfs_reap_batch(struct btrfs_trans_handle *handle, struct btrfs_root *root,
    uint64_t ino, unsigned int limit, int *finished)
{
	struct btrfs_inode_item item;
	struct btrfs_file_extent extent;
	struct btrfs_path path = { 0 };
	struct btrfs_key target = { 0 }, key;
	const struct btrfs_key *found;
	const uint8_t *data;
	uint64_t nbytes;
	uint32_t size;
	unsigned int i;
	int error;

	*finished = 0;
	error = btrfs_read_orphan_inode(root, ino, &item);
	if (error != 0)
		return (error);
	nbytes = letoh64(item.nbytes);
	target.objectid = htole64(ino);
	target.type = BTRFS_INODE_ITEM_KEY + 1;
	for (i = 0; ; i++) {
		error = btrfs_search_lower_bound(root, &target, &path);
		if (error == 0)
			error = btrfs_path_item(&path, &found, &data, &size);
		if (error == ENOENT ||
		    (error == 0 && found->objectid != target.objectid)) {
			*finished = 1;
			error = 0;
			btrfs_release_path(&path);
			break;
		}
		if (error != 0)
			goto out;
		/* Look past a full batch before deciding to retain the marker. */
		if (i == limit) {
			btrfs_release_path(&path);
			break;
		}
		key = *found;
		memset(&extent, 0, sizeof(extent));
		if (key.type == BTRFS_EXTENT_DATA_KEY) {
			error = btrfs_decode_file_extent(root->br_mount,
			    path.bp_view_generation, &key, data, size, &extent);
			if (error != 0)
				goto out;
			if (extent.bfe_type != BTRFS_FILE_EXTENT_HOLE) {
				if (extent.bfe_length > nbytes) {
					error = EINVAL;
					goto out;
				}
				nbytes -= extent.bfe_length;
			}
		} else if (key.type != BTRFS_XATTR_ITEM_KEY) {
			error = EOPNOTSUPP;
			goto out;
		}
		btrfs_release_path(&path);
		error = btrfs_delete_item(handle, root, &key);
		if (error == 0 && extent.bfe_disk_bytenr != 0)
			error = btrfs_delayed_data_ref_add(handle,
			    extent.bfe_disk_bytenr, extent.bfe_disk_num_bytes,
			    btrfs_ref_data(root->br_owner, ino,
			    extent.bfe_logical - extent.bfe_disk_offset), -1);
		if (error != 0)
			return (error);
	}
	key = target;
	key.type = BTRFS_INODE_ITEM_KEY;
	if (*finished) {
		if (nbytes != 0)
			return (EINVAL);
		error = btrfs_delete_item(handle, root, &key);
		if (error == 0) {
			key.objectid = htole64(BTRFS_ORPHAN_OBJECTID);
			key.type = BTRFS_ORPHAN_ITEM_KEY;
			key.offset = htole64(ino);
			error = btrfs_delete_item(handle, root, &key);
		}
	} else {
		item.nbytes = htole64(nbytes);
		item.transid = htole64(handle->bth_transaction->bt_generation);
		error = btrfs_replace_item(handle, root, &key, &item, sizeof(item));
	}
	return (error);
out:
	btrfs_release_path(&path);
	return (error);
}

/*
 * Final-link removal records a zero-link inode and orphan marker with the
 * namespace removal. Open descriptors and mappings survive until the last
 * vnode reference, whose cleanup commits any pending data before calling us.
 * Rmdir uses the same lifecycle. A fragmented shrink instead publishes its
 * target size and marker first, then commits ordered data before cleanup;
 * the locked vnode must remain locked through all batches.
 *
 * Each truncate batch re-searches from rounded target EOF, including
 * preallocation beyond the old EOF, and keeps remaining byte accounting
 * durable. The final batch removes a linked inode's marker; an open unlinked
 * inode keeps its marker for last close. Failure after target publication cannot
 * restore the old size: leave the marker recoverable and the filesystem
 * read-only. Mount recovery uses this same vnode-independent engine before
 * exposing any view.
 *
 * Keep an inode-number high-water mark until filesystem teardown so deleted
 * vnodes and file handles cannot alias new inodes.
 */
static int
btrfs_cleanup_inode(struct btrfs_root *root, uint64_t ino,
    struct btrfs_node *node)
{
	struct btrfs_fs *bmp = root->br_mount;
	struct btrfs_inode_item inode;
	struct btrfs_trans_reservation reservation = { 0 };
	struct btrfs_trans_handle *handle;
	uint64_t generation, remaining = 0, items;
	unsigned int limit = 8, batch;
	int error, end_error, finished = 0, truncate;

	error = btrfs_validate_orphan(root, ino, &items);
	if (error != 0)
		return (error);
	error = btrfs_read_orphan_inode(root, ino, &inode);
	if (error != 0)
		return (error);
	truncate = node != NULL || inode.nlink != 0;
	/* A still-cached deleted vnode must never alias a new creation. */
	rw_enter_write(&bmp->bm_namespace_lock);
	root->br_last_ino = MAX(root->br_last_ino, ino);
	rw_exit_write(&bmp->bm_namespace_lock);
	while (!finished) {
		/*
		 * Validation counted every mapping and xattr of an unlinked
		 * inode. Its locked vnode (or private mount recovery) keeps
		 * that count stable. Reserve only the items this batch can
		 * remove, retaining the minimum inode/marker budget even
		 * for an empty inode. Truncate keeps its existing bound
		 * because it preserves some of those items.
		 */
		batch = truncate ? limit : MIN(items, limit);
		reservation.btr_metadata =
		    (uint64_t)letoh32(bmp->bm_super.nodesize) *
		    64 * (MAX(1, batch) + 1);
		/* A small inode alone must not force a protected-reserve commit. */
		reservation.btr_reclaim = limit == 1;
		error = btrfs_trans_join(bmp, &reservation, &handle);
		if (error == ENOSPC && limit > 1) {
			limit = MAX(1, batch / 2);
			continue;
		}
		if (error != 0)
			return (error);
		generation = handle->bth_transaction->bt_generation;
		if (truncate)
			error = btrfs_truncate_batch(handle, root, ino, limit,
			    &finished, &remaining);
		else {
			error = btrfs_reap_batch(handle, root, ino, batch,
			    &finished);
			if (error == 0) {
				items -= batch;
				if (finished != (items == 0))
					error = EINVAL;
			}
		}
		if (error != 0)
			btrfs_trans_abort(handle, error);
		end_error = btrfs_trans_end(handle);
		if (error == 0)
			error = end_error;
		/*
		 * Publish intermediate batches to bound reservations and
		 * release pinned space. A completed unlink can share the open
		 * transaction with later namespace operations; the inode and
		 * marker disappear atomically when that transaction commits.
		 * Publish protected-reserve cleanup promptly to replenish it.
		 */
		if (error == 0 && (!finished || truncate ||
		    reservation.btr_reclaim))
			error = btrfs_trans_commit(bmp, generation, curproc);
		if (error != 0)
			return (error);
		if (node != NULL) {
			node->bn_inode.bi_nbytes = remaining;
			node->bn_inode.bi_transid = generation;
			node->bn_inode.bi_last_dirty_transid = generation;
		}
	}
	return (0);
}

int
btrfs_reap_inode(struct btrfs_root *root, uint64_t ino)
{
	return (btrfs_cleanup_inode(root, ino, NULL));
}

int
btrfs_start_truncate(struct btrfs_trans_handle *handle, struct btrfs_node *node)
{
	struct btrfs_key key = { 0 };

	/* Open, unlinked inodes already have a marker for last-close cleanup. */
	if (node->bn_inode.bi_nlink == 0)
		return (0);
	key.objectid = htole64(BTRFS_ORPHAN_OBJECTID);
	key.type = BTRFS_ORPHAN_ITEM_KEY;
	key.offset = htole64(node->bn_ino);
	return (btrfs_insert_item(handle, node->bn_root, &key, NULL, 0));
}

int
btrfs_finish_truncate(struct btrfs_node *node)
{
	struct btrfs_fs *bmp = node->bn_mount;
	int error;

	KASSERT(VOP_ISLOCKED(node->bn_vnode));
	error = btrfs_trans_commit(bmp, node->bn_inode.bi_last_dirty_transid,
	    curproc);
	if (error == 0)
		error = btrfs_cleanup_inode(node->bn_root, node->bn_ino, node);
	if (error != 0) {
		/* Further writes must not expose the unfinished deletion range. */
		mtx_enter(&bmp->bm_trans_mtx);
		btrfs_fs_set_readonly(bmp);
		mtx_leave(&bmp->bm_trans_mtx);
	}
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
				if (!btrfs_name_valid(name, namelen))
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
				if (!btrfs_name_valid(name, namelen))
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

/*
 * Missing backreferences are snapshot boundaries: the copied directory item
 * is retained, but the nested subvolume itself was not snapshotted.
 */
int
btrfs_check_subvol_link(struct btrfs_root *root, uint64_t dirid,
    uint64_t id, const char *name, size_t len)
{
	struct btrfs_root *roots;
	struct btrfs_path path = { 0 };
	struct btrfs_key key = { 0 };
	const struct btrfs_root_ref *ref;
	const uint8_t *data;
	uint32_t size;
	int error;

	error = btrfs_get_root(root->br_mount, BTRFS_ROOT_TREE_OBJECTID, &roots);
	if (error != 0)
		return (error);
	key.objectid = htole64(id);
	key.type = BTRFS_ROOT_BACKREF_KEY;
	key.offset = htole64(root->br_owner);
	error = btrfs_search_slot(roots, &key, &path);
	if (error == 0)
		error = btrfs_path_item(&path, NULL, &data, &size);
	if (error == 0) {
		ref = (const struct btrfs_root_ref *)data;
		if (size != sizeof(*ref) + len ||
		    letoh64(ref->dirid) != dirid ||
		    letoh16(ref->name_len) != len ||
		    memcmp(ref + 1, name, len) != 0)
			error = EINVAL;
	}
	btrfs_release_path(&path);
	return (error);
}

int
btrfs_find_subvol_parent(struct btrfs_fs *bmp, uint64_t treeid,
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

	if (treeid == BTRFS_FS_TREE_OBJECTID) {
		*parent_treeidp = treeid;
		*parent_diridp = BTRFS_FIRST_FREE_OBJECTID;
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
		if (!btrfs_name_valid(name, namelen))
			goto invalid;

		parent_treeid = letoh64(key->offset);
		parent_dirid = letoh64(ref->dirid);
		if ((parent_treeid != BTRFS_FS_TREE_OBJECTID &&
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

static int
btrfs_iterate_dir_item(struct btrfs_path *path,
    btrfs_dir_iter_fn callback, void *arg)
{
	const struct btrfs_key *key;
	struct btrfs_dir_entry entry;
	struct btrfs_dir_record record;
	const uint8_t *data;
	uint32_t size, remaining;
	int error;

	error = btrfs_path_item(path, &key, &data, &size);
	if (error != 0)
		return (error);
	remaining = size;
	if (remaining == 0)
		return (EINVAL);
	while (remaining != 0) {
		error = btrfs_decode_dir_record(data, remaining, &record);
		if (error == 0)
			error = btrfs_validate_dir_record(key,
			    path->bp_view_generation, size, &record);
		if (error != 0)
			return (error);

		if (callback != NULL) {
			entry.bde_name = record.name;
			entry.bde_objectid = letoh64(record.item->location.objectid);
			/* Only DIR_INDEX items supply a readdir position. */
			entry.bde_index = key->type == BTRFS_DIR_INDEX_KEY ?
			    letoh64(key->offset) : 0;
			entry.bde_namelen = record.namelen;
			entry.bde_type = record.item->type;
			entry.bde_subvolume =
			    record.item->location.type == BTRFS_ROOT_ITEM_KEY;
			error = callback(&entry, arg);
			if (error != 0)
				return (error);
		}

		data += record.size;
		remaining -= record.size;
	}
	return (0);
}

int
btrfs_iterate_directory(struct btrfs_root *root, uint64_t objectid,
    uint64_t start, btrfs_dir_iter_fn callback, void *arg)
{
	const struct btrfs_key *key;
	struct btrfs_path path = { 0 };
	struct btrfs_key target;
	int error;

	memset(&target, 0, sizeof(target));
	target.objectid = htole64(objectid);
	target.type = BTRFS_DIR_INDEX_KEY;
	target.offset = htole64(start);
	error = btrfs_search_lower_bound(root, &target, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &key, NULL, NULL);
		if (error != 0 || key->objectid != target.objectid ||
		    key->type != target.type)
			break;
		error = btrfs_iterate_dir_item(&path, callback, arg);
		if (error != 0)
			goto out;
		error = btrfs_next_item(&path);
	}
	if (error == ENOENT)
		error = 0;
out:
	btrfs_release_path(&path);
	return (error);
}

int
btrfs_lookup_directory(struct btrfs_root *root, uint64_t objectid,
    const char *name, size_t namelen, btrfs_dir_iter_fn callback, void *arg)
{
	struct btrfs_path path = { 0 };
	struct btrfs_key target;
	int error;

	if (namelen > BTRFS_NAME_MAX ||
	    !btrfs_name_valid((const uint8_t *)name, namelen))
		return (EINVAL);
	memset(&target, 0, sizeof(target));
	target.objectid = htole64(objectid);
	target.type = BTRFS_DIR_ITEM_KEY;
	target.offset = htole64(btrfs_name_hash(name, namelen));
	error = btrfs_search_slot(root, &target, &path);
	if (error == 0)
		error = btrfs_iterate_dir_item(&path, callback, arg);
	else if (error == ENOENT)
		error = 0;
	btrfs_release_path(&path);
	return (error);
}

int
btrfs_decode_file_extent(const struct btrfs_fs *bmp,
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
btrfs_find_file_extent(const struct btrfs_fs *bmp,
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
