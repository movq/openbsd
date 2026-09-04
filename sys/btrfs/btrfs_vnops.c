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
#include <sys/buf.h>
#include <sys/dirent.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <sys/vnode.h>

#include <btrfs/btrfs_var.h>

static int	btrfs_lookup(void *);
static int	btrfs_open(void *);
static int	btrfs_close(void *);
static int	btrfs_access(void *);
static int	btrfs_getattr(void *);
static int	btrfs_read(void *);
static int	btrfs_ioctl(void *);
static int	btrfs_readdir(void *);
static int	btrfs_readlink(void *);
static int	btrfs_inactive(void *);
static int	btrfs_reclaim(void *);
static int	btrfs_lock(void *);
static int	btrfs_unlock(void *);
static int	btrfs_islocked(void *);
static int	btrfs_print(void *);
static int	btrfs_pathconf(void *);

const struct vops btrfs_vops = {
	.vop_lookup	= btrfs_lookup,
	.vop_create	= eopnotsupp,
	.vop_mknod	= eopnotsupp,
	.vop_open	= btrfs_open,
	.vop_close	= btrfs_close,
	.vop_access	= btrfs_access,
	.vop_getattr	= btrfs_getattr,
	.vop_setattr	= eopnotsupp,
	.vop_read	= btrfs_read,
	.vop_write	= eopnotsupp,
	.vop_ioctl	= btrfs_ioctl,
	.vop_kqfilter	= eopnotsupp,
	.vop_revoke	= vop_generic_revoke,
	.vop_fsync	= nullop,
	.vop_remove	= eopnotsupp,
	.vop_link	= eopnotsupp,
	.vop_rename	= eopnotsupp,
	.vop_mkdir	= eopnotsupp,
	.vop_rmdir	= eopnotsupp,
	.vop_symlink	= eopnotsupp,
	.vop_readdir	= btrfs_readdir,
	.vop_readlink	= btrfs_readlink,
	.vop_abortop	= vop_generic_abortop,
	.vop_inactive	= btrfs_inactive,
	.vop_reclaim	= btrfs_reclaim,
	.vop_lock	= btrfs_lock,
	.vop_unlock	= btrfs_unlock,
	.vop_bmap	= eopnotsupp,
	.vop_strategy	= vop_generic_badop,
	.vop_print	= btrfs_print,
	.vop_islocked	= btrfs_islocked,
	.vop_pathconf	= btrfs_pathconf,
	.vop_advlock	= eopnotsupp,
	.vop_bwrite	= vop_generic_bwrite,
};

#define BTRFS_LOOKUP_FOUND	(-1)

struct btrfs_lookup_ctx {
	const char	*blc_name;
	size_t		 blc_namelen;
	uint64_t	 blc_objectid;
	uint8_t		 blc_type;
	int		 blc_subvolume;
};

static int
btrfs_lookup_entry(const struct btrfs_dir_entry *entry, void *arg)
{
	struct btrfs_lookup_ctx *ctx = arg;

	if (entry->bde_namelen != ctx->blc_namelen ||
	    memcmp(entry->bde_name, ctx->blc_name, ctx->blc_namelen) != 0)
		return (0);

	ctx->blc_objectid = entry->bde_objectid;
	ctx->blc_type = entry->bde_type;
	ctx->blc_subvolume = entry->bde_subvolume;
	return (BTRFS_LOOKUP_FOUND);
}

static enum vtype
btrfs_dirent_vtype(uint8_t type)
{
	switch (type) {
	case BTRFS_FT_REG_FILE:
		return (VREG);
	case BTRFS_FT_DIR:
		return (VDIR);
	case BTRFS_FT_CHRDEV:
		return (VCHR);
	case BTRFS_FT_BLKDEV:
		return (VBLK);
	case BTRFS_FT_FIFO:
		return (VFIFO);
	case BTRFS_FT_SOCK:
		return (VSOCK);
	case BTRFS_FT_SYMLINK:
		return (VLNK);
	default:
		return (VNON);
	}
}

static int
btrfs_lookup(void *v)
{
	struct vop_lookup_args *ap = v;
	struct vnode *dvp = ap->a_dvp;
	struct vnode **vpp = ap->a_vpp;
	struct componentname *cnp = ap->a_cnp;
	struct btrfs_node *node = VTOBTRFS(dvp);
	struct btrfs_mount *bmp = node->bn_mount;
	struct btrfs_lookup_ctx ctx;
	struct btrfs_root root;
	uint64_t parent, parent_treeid;
	enum vtype type;
	int error, lastcn, lockparent;

	KASSERT(VOP_ISLOCKED(dvp));
	cnp->cn_flags &= ~PDIRUNLOCK;
	*vpp = NULL;
	lastcn = (cnp->cn_flags & ISLASTCN) != 0;
	lockparent = (cnp->cn_flags & LOCKPARENT) != 0;

	if (dvp->v_type != VDIR)
		return (ENOTDIR);
	error = VOP_ACCESS(dvp, VEXEC, cnp->cn_cred, cnp->cn_proc);
	if (error != 0)
		return (error);
	if (lastcn && (cnp->cn_nameiop == DELETE ||
	    cnp->cn_nameiop == RENAME))
		return (EROFS);

	error = cache_lookup(dvp, vpp, cnp);
	if (error >= 0)
		return (error);
	error = 0;

	if (cnp->cn_flags & ISDOTDOT) {
		parent_treeid = node->bn_treeid;
		if (node->bn_ino == bmp->bm_root_dirid &&
		    node->bn_treeid == bmp->bm_treeid) {
			vref(dvp);
			*vpp = dvp;
			goto found;
		}
		if (node->bn_ino == BTRFS_FIRST_FREE_OBJECTID) {
			error = btrfs_find_subvol_parent(bmp, node->bn_treeid,
			    &parent_treeid, &parent);
		} else {
			error = btrfs_init_fs_root(bmp, node->bn_treeid, &root);
			if (error == 0)
				error = btrfs_find_dir_parent(&root,
				    node->bn_ino, &parent);
		}
		if (error != 0)
			goto out;
		VOP_UNLOCK(dvp);
		cnp->cn_flags |= PDIRUNLOCK;
		error = btrfs_vget_tree(dvp->v_mount, parent_treeid, parent,
		    vpp);
		if (error != 0) {
			if (vn_lock(dvp, LK_EXCLUSIVE | LK_RETRY) == 0)
				cnp->cn_flags &= ~PDIRUNLOCK;
			return (error);
		}
		if (lockparent && lastcn) {
			error = vn_lock(dvp, LK_EXCLUSIVE);
			if (error != 0) {
				vput(*vpp);
				*vpp = NULL;
				return (error);
			}
			cnp->cn_flags &= ~PDIRUNLOCK;
		}
		goto found;
	}
	if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') {
		vref(dvp);
		*vpp = dvp;
		goto found;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.blc_name = cnp->cn_nameptr;
	ctx.blc_namelen = cnp->cn_namelen;
	error = btrfs_init_fs_root(bmp, node->bn_treeid, &root);
	if (error != 0)
		goto out;
	error = btrfs_iterate_directory(&root, node->bn_ino,
	    btrfs_lookup_entry, &ctx);
	if (error == BTRFS_LOOKUP_FOUND)
		error = 0;
	else if (error == 0)
		error = ENOENT;
	if (error != 0) {
		if (error == ENOENT && lastcn &&
		    cnp->cn_nameiop == CREATE) {
			error = VOP_ACCESS(dvp, VWRITE, cnp->cn_cred,
			    cnp->cn_proc);
			if (error == 0)
				error = EJUSTRETURN;
		}
		if (error == ENOENT && (cnp->cn_flags & MAKEENTRY))
			cache_enter(dvp, NULL, cnp);
		goto out;
	}

	if (!lastcn && ctx.blc_type != BTRFS_FT_UNKNOWN &&
	    ctx.blc_type != BTRFS_FT_DIR &&
	    ctx.blc_type != BTRFS_FT_SYMLINK) {
		error = ENOTDIR;
		goto out;
	}
	if (!ctx.blc_subvolume && ctx.blc_objectid == node->bn_ino) {
		error = EINVAL;
		goto out;
	}

	if (ctx.blc_subvolume)
		error = btrfs_vget_tree(dvp->v_mount, ctx.blc_objectid,
		    BTRFS_FIRST_FREE_OBJECTID, vpp);
	else
		error = btrfs_vget_tree(dvp->v_mount, node->bn_treeid,
		    ctx.blc_objectid, vpp);
	if (error != 0)
		goto out;
	type = btrfs_dirent_vtype(ctx.blc_type);
	if (type != VNON && type != (*vpp)->v_type) {
		vput(*vpp);
		*vpp = NULL;
		error = EINVAL;
		goto out;
	}

found:
	if (cnp->cn_flags & MAKEENTRY)
		cache_enter(dvp, *vpp, cnp);
out:
	if (error == 0 && *vpp != dvp && (!lockparent || !lastcn) &&
	    (cnp->cn_flags & PDIRUNLOCK) == 0) {
		VOP_UNLOCK(dvp);
		cnp->cn_flags |= PDIRUNLOCK;
	}
	KASSERT((*vpp != NULL && VOP_ISLOCKED(*vpp)) || error != 0);
	return (error);
}

static int
btrfs_open(void *v)
{
	struct vop_open_args *ap = v;

	if (ap->a_mode & FWRITE)
		return (EROFS);
	return (0);
}

static int
btrfs_close(void *v)
{
	return (0);
}

static int
btrfs_access(void *v)
{
	struct vop_access_args *ap = v;
	struct btrfs_node *node = VTOBTRFS(ap->a_vp);
	uint32_t mode;

	if (ap->a_mode & VWRITE)
		return (EROFS);
	mode = letoh32(node->bn_inode.mode);
	return (vaccess(ap->a_vp->v_type, mode & ALLPERMS,
	    letoh32(node->bn_inode.uid), letoh32(node->bn_inode.gid),
	    ap->a_mode, ap->a_cred));
}

static void
btrfs_timespec(const struct btrfs_timespec *disk, struct timespec *host)
{
	host->tv_sec = letoh64(disk->sec);
	host->tv_nsec = letoh32(disk->nsec);
}

static int
btrfs_getattr(void *v)
{
	struct vop_getattr_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct btrfs_node *node = VTOBTRFS(vp);
	const struct btrfs_inode_item *inode = &node->bn_inode;
	struct vattr *vap = ap->a_vap;

	vattr_null(vap);
	vap->va_fsid = node->bn_mount->bm_dev;
	vap->va_fileid = node->bn_ino;
	vap->va_mode = letoh32(inode->mode) & ALLPERMS;
	vap->va_nlink = letoh32(inode->nlink);
	vap->va_uid = letoh32(inode->uid);
	vap->va_gid = letoh32(inode->gid);
	btrfs_timespec(&inode->atime, &vap->va_atime);
	btrfs_timespec(&inode->mtime, &vap->va_mtime);
	btrfs_timespec(&inode->ctime, &vap->va_ctime);
	vap->va_rdev = letoh64(inode->rdev);
	vap->va_size = letoh64(inode->size);
	vap->va_flags = 0;
	vap->va_gen = letoh64(inode->generation);
	vap->va_blocksize = letoh32(node->bn_mount->bm_super.nodesize);
	vap->va_bytes = letoh64(inode->nbytes);
	vap->va_type = vp->v_type;
	vap->va_filerev = letoh64(inode->transid);
	vap->va_vaflags = 0;
	return (0);
}

static const uint8_t btrfs_zeros[DEV_BSIZE];

static int
btrfs_uiomove_zeros(size_t size, struct uio *uio)
{
	size_t chunk;
	int error;

	while (size != 0) {
		chunk = MIN(size, sizeof(btrfs_zeros));
		error = uiomove((void *)btrfs_zeros, chunk, uio);
		if (error != 0)
			return (error);
		size -= chunk;
	}
	return (0);
}

static int
btrfs_read_regular_extent(struct btrfs_node *node,
    const struct btrfs_file_extent *extent, size_t size, struct uio *uio)
{
	struct btrfs_mount *bmp = node->bn_mount;
	struct buf *bp = NULL;
	uint64_t block, logical, relative;
	uint64_t inode_flags;
	const uint32_t *expectedp;
	uint32_t expected;
	uint32_t sectorsize;
	size_t chunk, offset;
	int error = 0;

	sectorsize = letoh32(bmp->bm_super.sectorsize);
	inode_flags = letoh64(node->bn_inode.flags);
	relative = uio->uio_offset - extent->bfe_logical;
	logical = extent->bfe_disk_bytenr + extent->bfe_disk_offset + relative;

	while (size != 0) {
		block = logical & ~((uint64_t)sectorsize - 1);
		offset = logical - block;
		chunk = MIN(size, sectorsize - offset);
		expectedp = NULL;
		if ((inode_flags & BTRFS_INODE_NODATASUM) == 0) {
			error = btrfs_lookup_data_csum(bmp, block, &expected);
			if (error != 0) {
				if (error == ENOENT)
					error = EINVAL;
				break;
			}
			expectedp = &expected;
		}
		error = btrfs_read_data_block(bmp, block, expectedp, &bp);
		if (error != 0)
			break;
		error = uiomove((uint8_t *)bp->b_data + offset, chunk, uio);
		brelse(bp);
		bp = NULL;
		if (error != 0)
			break;
		logical += chunk;
		size -= chunk;
	}

	if (bp != NULL)
		brelse(bp);
	return (error);
}

static int
btrfs_read(void *v)
{
	struct vop_read_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct btrfs_node *node = VTOBTRFS(vp);
	struct btrfs_mount *bmp = node->bn_mount;
	struct btrfs_file_extent extent;
	struct btrfs_path path = { 0 };
	struct btrfs_root root;
	struct uio *uio = ap->a_uio;
	uint64_t available, file_size, offset;
	size_t size;
	int error = 0;

	KASSERT(VOP_ISLOCKED(vp));
	if (vp->v_type != VREG)
		return (EISDIR);
	if (uio->uio_rw != UIO_READ || uio->uio_offset < 0)
		return (EINVAL);
	if (uio->uio_resid == 0)
		return (0);

	file_size = letoh64(node->bn_inode.size);
	offset = uio->uio_offset;
	if (offset >= file_size)
		return (0);

	error = btrfs_init_fs_root(bmp, node->bn_treeid, &root);
	if (error != 0)
		return (error);

	while (error == 0 && uio->uio_resid != 0 &&
	    (uint64_t)uio->uio_offset < file_size) {
		offset = uio->uio_offset;
		error = btrfs_find_file_extent(bmp, &root, &path,
		    node->bn_ino, offset, file_size, &extent);
		if (error != 0)
			break;
		if (extent.bfe_encryption != 0 ||
		    extent.bfe_other_encoding != 0) {
			error = EOPNOTSUPP;
			break;
		}

		available = extent.bfe_logical + extent.bfe_length - offset;
		size = uio->uio_resid;
		if (size > file_size - offset)
			size = file_size - offset;
		if (size > available)
			size = available;

		switch (extent.bfe_type) {
		case BTRFS_FILE_EXTENT_INLINE:
			if (extent.bfe_compression == BTRFS_COMPRESS_NONE) {
				if (extent.bfe_inline_size != extent.bfe_length) {
					error = EINVAL;
					break;
				}
				error = uiomove((void *)(extent.bfe_inline_data +
				    offset - extent.bfe_logical), size, uio);
			} else {
				error = btrfs_read_compressed_extent(node, &extent,
				    size, uio);
			}
			break;
		case BTRFS_FILE_EXTENT_REG:
			if (extent.bfe_compression == BTRFS_COMPRESS_NONE)
				error = btrfs_read_regular_extent(node, &extent,
				    size, uio);
			else
				error = btrfs_read_compressed_extent(node, &extent,
				    size, uio);
			break;
		case BTRFS_FILE_EXTENT_PREALLOC:
		case BTRFS_FILE_EXTENT_HOLE:
			error = btrfs_uiomove_zeros(size, uio);
			break;
		default:
			error = EINVAL;
			break;
		}
	}

	btrfs_release_path(&path);
	return (error);
}

static int
btrfs_ioctl(void *v)
{
	return (ENOTTY);
}

static int
btrfs_readlink(void *v)
{
	struct vop_readlink_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct btrfs_node *node = VTOBTRFS(vp);
	struct btrfs_mount *bmp = node->bn_mount;
	struct btrfs_file_extent extent;
	struct btrfs_path path = { 0 };
	struct btrfs_root root;
	struct uio *uio = ap->a_uio;
	uint64_t file_size;
	size_t size;
	int error;

	KASSERT(VOP_ISLOCKED(vp));
	KASSERT(uio->uio_offset == 0);
	if (vp->v_type != VLNK)
		return (EINVAL);
	if (uio->uio_rw != UIO_READ || uio->uio_offset != 0)
		return (EINVAL);

	file_size = letoh64(node->bn_inode.size);
	if (file_size == 0)
		return (EINVAL);
	error = btrfs_init_fs_root(bmp, node->bn_treeid, &root);
	if (error != 0)
		return (error);
	error = btrfs_find_file_extent(bmp, &root, &path, node->bn_ino, 0,
	    file_size, &extent);
	if (error != 0)
		goto out;
	if (extent.bfe_compression != BTRFS_COMPRESS_NONE ||
	    extent.bfe_encryption != 0 || extent.bfe_other_encoding != 0) {
		error = EOPNOTSUPP;
		goto out;
	}
	if (extent.bfe_type != BTRFS_FILE_EXTENT_INLINE ||
	    extent.bfe_logical != 0 || extent.bfe_length != file_size ||
	    extent.bfe_inline_size != file_size) {
		error = EINVAL;
		goto out;
	}

	size = uio->uio_resid;
	if (size > file_size)
		size = file_size;
	error = uiomove((void *)extent.bfe_inline_data, size, uio);
out:
	btrfs_release_path(&path);
	return (error);
}

#define BTRFS_DIR_OFFSET_DOT		0
#define BTRFS_DIR_OFFSET_DOTDOT		1
#define BTRFS_DIR_OFFSET_FIRST		2
#define BTRFS_READDIR_FULL		(-1)

struct btrfs_readdir_ctx {
	struct uio	*brc_uio;
	off_t		 brc_skip;
	off_t		 brc_position;
	off_t		 brc_offset;
	int		 brc_full;
};

static uint8_t
btrfs_dirent_type(uint8_t type)
{
	switch (type) {
	case BTRFS_FT_REG_FILE:
		return (DT_REG);
	case BTRFS_FT_DIR:
		return (DT_DIR);
	case BTRFS_FT_CHRDEV:
		return (DT_CHR);
	case BTRFS_FT_BLKDEV:
		return (DT_BLK);
	case BTRFS_FT_FIFO:
		return (DT_FIFO);
	case BTRFS_FT_SOCK:
		return (DT_SOCK);
	case BTRFS_FT_SYMLINK:
		return (DT_LNK);
	default:
		return (DT_UNKNOWN);
	}
}

static int
btrfs_emit_dirent(struct btrfs_readdir_ctx *ctx, ino_t fileno,
    uint8_t type, const uint8_t *name, uint16_t namelen, off_t next)
{
	union {
		struct dirent	dirent;
		uint8_t		padding[roundup(sizeof(struct dirent), 8)];
	} entry;
	struct dirent *dirent = &entry.dirent;
	int error;

	memset(&entry, 0, sizeof(entry));
	dirent->d_fileno = fileno;
	dirent->d_off = next;
	dirent->d_type = type;
	dirent->d_namlen = namelen;
	dirent->d_reclen = DIRENT_SIZE(dirent);
	if (ctx->brc_uio->uio_resid < dirent->d_reclen) {
		ctx->brc_full = 1;
		return (BTRFS_READDIR_FULL);
	}
	memcpy(dirent->d_name, name, namelen);

	error = uiomove(dirent, dirent->d_reclen, ctx->brc_uio);
	if (error == 0)
		ctx->brc_offset = next;
	return (error);
}

static int
btrfs_readdir_entry(const struct btrfs_dir_entry *entry, void *arg)
{
	struct btrfs_readdir_ctx *ctx = arg;
	ino_t fileno;
	int error;

	if (ctx->brc_position < ctx->brc_skip) {
		ctx->brc_position++;
		return (0);
	}

	/*
	 * A subvolume directory item names a root item; its visible inode is
	 * the root directory in that tree, not the root item's object ID.
	 */
	if (entry->bde_subvolume)
		fileno = BTRFS_FIRST_FREE_OBJECTID;
	else
		fileno = entry->bde_objectid;
	error = btrfs_emit_dirent(ctx, fileno,
	    btrfs_dirent_type(entry->bde_type), entry->bde_name,
	    entry->bde_namelen, ctx->brc_position + 1);
	if (error == 0)
		ctx->brc_position++;
	return (error);
}

static int
btrfs_readdir(void *v)
{
	struct vop_readdir_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct btrfs_node *node = VTOBTRFS(vp);
	struct btrfs_mount *bmp = node->bn_mount;
	struct btrfs_readdir_ctx ctx;
	struct btrfs_root root;
	struct uio *uio = ap->a_uio;
	uint64_t parent, parent_treeid;
	int error = 0;

	KASSERT(VOP_ISLOCKED(vp));
	if (uio->uio_rw != UIO_READ || uio->uio_offset < 0)
		return (EINVAL);
	if (vp->v_type != VDIR)
		return (ENOTDIR);
	memset(&ctx, 0, sizeof(ctx));
	ctx.brc_uio = uio;
	ctx.brc_offset = uio->uio_offset;

	if (ctx.brc_offset == BTRFS_DIR_OFFSET_DOT) {
		error = btrfs_emit_dirent(&ctx, node->bn_ino, DT_DIR,
		    (const uint8_t *)".", 1, BTRFS_DIR_OFFSET_DOTDOT);
		if (error != 0)
			goto out;
	}
	if (ctx.brc_offset == BTRFS_DIR_OFFSET_DOTDOT) {
		parent_treeid = node->bn_treeid;
		if (node->bn_ino == BTRFS_FIRST_FREE_OBJECTID &&
		    node->bn_treeid != bmp->bm_treeid)
			error = btrfs_find_subvol_parent(bmp, node->bn_treeid,
			    &parent_treeid, &parent);
		else {
			error = btrfs_init_fs_root(bmp, node->bn_treeid, &root);
			if (error == 0)
				error = btrfs_find_dir_parent(&root,
				    node->bn_ino, &parent);
		}
		if (error != 0)
			goto out;
		error = btrfs_emit_dirent(&ctx, parent, DT_DIR,
		    (const uint8_t *)"..", 2, BTRFS_DIR_OFFSET_FIRST);
		if (error != 0)
			goto out;
	}

	ctx.brc_skip = ctx.brc_offset;
	ctx.brc_position = BTRFS_DIR_OFFSET_FIRST;
	error = btrfs_init_fs_root(bmp, node->bn_treeid, &root);
	if (error != 0)
		goto out;
	error = btrfs_iterate_directory(&root, node->bn_ino,
	    btrfs_readdir_entry, &ctx);
out:
	if (error == BTRFS_READDIR_FULL)
		error = 0;
	uio->uio_offset = ctx.brc_offset;
	if (ap->a_eofflag != NULL)
		*ap->a_eofflag = error == 0 && !ctx.brc_full;
	return (error);
}

static int
btrfs_inactive(void *v)
{
	struct vop_inactive_args *ap = v;

	VOP_UNLOCK(ap->a_vp);
	return (0);
}

static int
btrfs_reclaim(void *v)
{
	struct vop_reclaim_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct btrfs_node *node = VTOBTRFS(vp);
	struct btrfs_mount *bmp = node->bn_mount;

	if (node->bn_hashed) {
		mtx_enter(&bmp->bm_nodemtx);
		LIST_REMOVE(node, bn_entry);
		node->bn_hashed = 0;
		mtx_leave(&bmp->bm_nodemtx);
	}
	cache_purge(vp);
	free(node, M_BTRFS, sizeof(*node));
	vp->v_data = NULL;
	return (0);
}

static int
btrfs_lock(void *v)
{
	struct vop_lock_args *ap = v;

	return (rrw_enter(&VTOBTRFS(ap->a_vp)->bn_lock,
	    ap->a_flags & LK_RWFLAGS));
}

static int
btrfs_unlock(void *v)
{
	struct vop_unlock_args *ap = v;

	rrw_exit(&VTOBTRFS(ap->a_vp)->bn_lock);
	return (0);
}

static int
btrfs_islocked(void *v)
{
	struct vop_islocked_args *ap = v;

	return (rrw_status(&VTOBTRFS(ap->a_vp)->bn_lock));
}

static int
btrfs_print(void *v)
{
#if defined(DEBUG) || defined(DIAGNOSTIC) || defined(VFSLCKDEBUG)
	struct vop_print_args *ap = v;
	struct btrfs_node *node = VTOBTRFS(ap->a_vp);

	printf("tag VT_BTRFS, tree %llu, inode %llu\n",
	    (unsigned long long)node->bn_treeid,
	    (unsigned long long)node->bn_ino);
#endif
	return (0);
}

static int
btrfs_pathconf(void *v)
{
	struct vop_pathconf_args *ap = v;

	switch (ap->a_name) {
	case _PC_LINK_MAX:
		*ap->a_retval = LINK_MAX;
		break;
	case _PC_NAME_MAX:
		*ap->a_retval = BTRFS_NAME_MAX;
		break;
	case _PC_CHOWN_RESTRICTED:
		*ap->a_retval = 1;
		break;
	case _PC_NO_TRUNC:
		*ap->a_retval = 1;
		break;
	case _PC_TIMESTAMP_RESOLUTION:
		*ap->a_retval = 1;
		break;
	default:
		return (EINVAL);
	}
	return (0);
}
