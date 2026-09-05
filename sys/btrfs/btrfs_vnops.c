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
#include <sys/pool.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <sys/vnode.h>

#include <btrfs/btrfs_var.h>

static int	btrfs_lookup(void *);
static int	btrfs_create(void *);
static int	btrfs_mkdir(void *);
static int	btrfs_mknod(void *);
static int	btrfs_symlink(void *);
static int	btrfs_link(void *);
static int	btrfs_makeinode(struct vnode *, struct vnode **,
		    struct componentname *, struct vattr *, const char *);
static int	btrfs_open(void *);
static int	btrfs_close(void *);
static int	btrfs_access(void *);
static int	btrfs_getattr(void *);
static int	btrfs_setattr(void *);
static int	btrfs_read(void *);
static int	btrfs_write(void *);
static int	btrfs_fsync(void *);
static int	btrfs_strategy(void *);
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
	.vop_create	= btrfs_create,
	.vop_mknod	= btrfs_mknod,
	.vop_open	= btrfs_open,
	.vop_close	= btrfs_close,
	.vop_access	= btrfs_access,
	.vop_getattr	= btrfs_getattr,
	.vop_setattr	= btrfs_setattr,
	.vop_read	= btrfs_read,
	.vop_write	= btrfs_write,
	.vop_ioctl	= btrfs_ioctl,
	.vop_kqfilter	= eopnotsupp,
	.vop_revoke	= vop_generic_revoke,
	.vop_fsync	= btrfs_fsync,
	.vop_remove	= eopnotsupp,
	.vop_link	= btrfs_link,
	.vop_rename	= eopnotsupp,
	.vop_mkdir	= btrfs_mkdir,
	.vop_rmdir	= eopnotsupp,
	.vop_symlink	= btrfs_symlink,
	.vop_readdir	= btrfs_readdir,
	.vop_readlink	= btrfs_readlink,
	.vop_abortop	= vop_generic_abortop,
	.vop_inactive	= btrfs_inactive,
	.vop_reclaim	= btrfs_reclaim,
	.vop_lock	= btrfs_lock,
	.vop_unlock	= btrfs_unlock,
	.vop_bmap	= eopnotsupp,
	.vop_strategy	= btrfs_strategy,
	.vop_print	= btrfs_print,
	.vop_islocked	= btrfs_islocked,
	.vop_pathconf	= btrfs_pathconf,
	.vop_advlock	= eopnotsupp,
	.vop_bwrite	= vop_generic_bwrite,
};

#define BTRFS_LOOKUP_FOUND	(-1)
#define BTRFS_VOP_METADATA_BLOCKS	64

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
	struct btrfs_root *root;
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
			error = btrfs_get_root(bmp, node->bn_treeid, &root);
			if (error == 0)
				error = btrfs_find_dir_parent(root,
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
	error = btrfs_get_root(bmp, node->bn_treeid, &root);
	if (error != 0)
		goto out;
	error = btrfs_iterate_directory(root, node->bn_ino,
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
			if (error == 0) {
				cnp->cn_flags |= SAVENAME;
				error = EJUSTRETURN;
			}
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
	if ((error == 0 || error == EJUSTRETURN) &&
	    *vpp != dvp && (!lockparent || !lastcn) &&
	    (cnp->cn_flags & PDIRUNLOCK) == 0) {
		VOP_UNLOCK(dvp);
		cnp->cn_flags |= PDIRUNLOCK;
	}
	KASSERT((*vpp != NULL && VOP_ISLOCKED(*vpp)) || error != 0);
	return (error);
}

static int
btrfs_makeinode(struct vnode *dvp, struct vnode **vpp,
    struct componentname *cnp, struct vattr *vap, const char *link)
{
	struct btrfs_node *dir = VTOBTRFS(dvp);
	mode_t mode;
	int error;

	KASSERT(VOP_ISLOCKED(dvp));
	KASSERT(cnp->cn_flags & HASBUF);
	*vpp = NULL;
	if (vap->va_type != VREG && vap->va_type != VDIR &&
	    vap->va_type != VLNK) {
		error = EOPNOTSUPP;
		goto out;
	}
	error = VOP_ACCESS(dvp, VWRITE | VEXEC, cnp->cn_cred, cnp->cn_proc);
	if (error != 0)
		goto out;
	mode = MAKEIMODE(vap->va_type, vap->va_mode);
	if ((mode & S_ISGID) &&
	    !groupmember(dir->bn_inode.bi_gid, cnp->cn_cred) &&
	    !vnoperm(dvp) && suser_ucred(cnp->cn_cred))
		mode &= ~S_ISGID;
	error = btrfs_create_inode(dir, cnp->cn_nameptr, cnp->cn_namelen,
	    mode, cnp->cn_cred->cr_uid, dir->bn_inode.bi_gid, link, vpp);
	if (error != 0)
		goto out;
	cache_purge(dvp);
	if (cnp->cn_flags & MAKEENTRY)
		cache_enter(dvp, *vpp, cnp);
	VN_KNOTE(dvp, NOTE_WRITE);
	if ((dvp->v_mount->mnt_flag & MNT_SYNCHRONOUS) ||
	    (dir->bn_inode.bi_flags &
	    (BTRFS_INODE_SYNC | BTRFS_INODE_DIRSYNC))) {
		error = btrfs_trans_commit(dir->bn_mount,
		    dir->bn_inode.bi_last_dirty_transid, cnp->cn_proc);
		if (error != 0) {
			vput(*vpp);
			*vpp = NULL;
		}
	}
out:
	if (error != 0 || (cnp->cn_flags & SAVESTART) == 0)
		pool_put(&namei_pool, cnp->cn_pnbuf);
	return (error);
}

static int
btrfs_create(void *v)
{
	struct vop_create_args *ap = v;

	return (btrfs_makeinode(ap->a_dvp, ap->a_vpp, ap->a_cnp,
	    ap->a_vap, NULL));
}

static int
btrfs_mkdir(void *v)
{
	struct vop_mkdir_args *ap = v;
	int error;

	error = btrfs_makeinode(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap,
	    NULL);
	vput(ap->a_dvp);
	return (error);
}

static int
btrfs_mknod(void *v)
{
	struct vop_mknod_args *ap = v;

	*ap->a_vpp = NULL;
	VOP_ABORTOP(ap->a_dvp, ap->a_cnp);
	return (EOPNOTSUPP);
}

static int
btrfs_symlink(void *v)
{
	struct vop_symlink_args *ap = v;
	struct vattr attr = *ap->a_vap;
	int error;

	attr.va_type = VLNK;
	error = btrfs_makeinode(ap->a_dvp, ap->a_vpp, ap->a_cnp, &attr,
	    ap->a_target);
	vput(ap->a_dvp);
	if (error == 0)
		vput(*ap->a_vpp);
	return (error);
}

static int
btrfs_link(void *v)
{
	struct vop_link_args *ap = v;

	VOP_ABORTOP(ap->a_dvp, ap->a_cnp);
	vput(ap->a_dvp);
	return (EOPNOTSUPP);
}

static int
btrfs_open(void *v)
{
	struct vop_open_args *ap = v;
	struct btrfs_node *node = VTOBTRFS(ap->a_vp);

	if (ap->a_mode & FWRITE) {
		if ((ap->a_vp->v_mount->mnt_flag & MNT_RDONLY) ||
		    node->bn_mount->bm_subvol_readonly ||
		    node->bn_treeid != node->bn_mount->bm_treeid ||
		    (node->bn_inode.bi_flags & BTRFS_INODE_READONLY))
			return (EROFS);
		if (node->bn_inode.bi_flags & BTRFS_INODE_IMMUTABLE)
			return (EPERM);
	}
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

	if (ap->a_mode & VWRITE) {
		if ((ap->a_vp->v_mount->mnt_flag & MNT_RDONLY) ||
		    node->bn_mount->bm_subvol_readonly ||
		    node->bn_treeid != node->bn_mount->bm_treeid ||
		    (node->bn_inode.bi_flags & BTRFS_INODE_READONLY))
			return (EROFS);
		if (node->bn_inode.bi_flags & BTRFS_INODE_IMMUTABLE)
			return (EPERM);
	}
	mode = node->bn_inode.bi_mode;
	return (vaccess(ap->a_vp->v_type, mode & ALLPERMS,
	    node->bn_inode.bi_uid, node->bn_inode.bi_gid,
	    ap->a_mode, ap->a_cred));
}

static int
btrfs_getattr(void *v)
{
	struct vop_getattr_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct btrfs_node *node = VTOBTRFS(vp);
	const struct btrfs_inode *inode = &node->bn_inode;
	struct vattr *vap = ap->a_vap;

	vattr_null(vap);
	vap->va_fsid = node->bn_mount->bm_dev;
	vap->va_fileid = node->bn_ino;
	vap->va_mode = inode->bi_mode & ALLPERMS;
	vap->va_nlink = inode->bi_nlink;
	vap->va_uid = inode->bi_uid;
	vap->va_gid = inode->bi_gid;
	vap->va_atime = inode->bi_atime;
	vap->va_mtime = inode->bi_mtime;
	vap->va_ctime = inode->bi_ctime;
	vap->va_rdev = inode->bi_rdev;
	vap->va_size = inode->bi_size;
	vap->va_flags = 0;
	vap->va_gen = inode->bi_generation;
	vap->va_blocksize = letoh32(node->bn_mount->bm_super.nodesize);
	vap->va_bytes = inode->bi_nbytes;
	vap->va_type = vp->v_type;
	vap->va_filerev = inode->bi_transid;
	vap->va_vaflags = 0;
	return (0);
}

static int
btrfs_setattr(void *v)
{
	struct vop_setattr_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct btrfs_node *node = VTOBTRFS(vp);
	struct btrfs_mount *bmp = node->bn_mount;
	struct btrfs_inode saved;
	struct btrfs_trans_handle *handle = NULL;
	struct btrfs_trans_reservation reservation = { 0 };
	struct vattr *vap = ap->a_vap;
	struct ucred *cred = ap->a_cred;
	struct timespec now;
	uid_t uid;
	gid_t gid;
	uint32_t dirty = 0;
	int end_error, error;

	KASSERT(VOP_ISLOCKED(vp));
	if (vap->va_type != VNON || vap->va_nlink != VNOVAL ||
	    vap->va_fsid != VNOVAL || vap->va_fileid != VNOVAL ||
	    vap->va_blocksize != VNOVAL || vap->va_rdev != VNOVAL ||
	    (int)vap->va_bytes != VNOVAL || vap->va_gen != VNOVAL)
		return (EINVAL);
	if (vap->va_size != VNOVAL || vap->va_flags != VNOVAL)
		return (EOPNOTSUPP);
	if ((vap->va_atime.tv_nsec != VNOVAL &&
	    (vap->va_atime.tv_nsec < 0 ||
	    vap->va_atime.tv_nsec >= 1000000000)) ||
	    (vap->va_mtime.tv_nsec != VNOVAL &&
	    (vap->va_mtime.tv_nsec < 0 ||
	    vap->va_mtime.tv_nsec >= 1000000000)))
		return (EINVAL);
	if ((vp->v_mount->mnt_flag & MNT_RDONLY) ||
	    bmp->bm_subvol_readonly ||
	    node->bn_treeid != bmp->bm_treeid ||
	    (node->bn_inode.bi_flags & BTRFS_INODE_READONLY))
		return (EROFS);
	if (node->bn_inode.bi_flags &
	    (BTRFS_INODE_IMMUTABLE | BTRFS_INODE_APPEND))
		return (EPERM);

	uid = vap->va_uid == (uid_t)VNOVAL ?
	    node->bn_inode.bi_uid : vap->va_uid;
	gid = vap->va_gid == (gid_t)VNOVAL ?
	    node->bn_inode.bi_gid : vap->va_gid;
	if (vap->va_uid != (uid_t)VNOVAL ||
	    vap->va_gid != (gid_t)VNOVAL) {
		if ((cred->cr_uid != node->bn_inode.bi_uid ||
		    uid != node->bn_inode.bi_uid ||
		    (gid != node->bn_inode.bi_gid &&
		    !groupmember(gid, cred))) &&
		    !vnoperm(vp) && (error = suser_ucred(cred)) != 0)
			return (error);
	}
	if (vap->va_mode != (mode_t)VNOVAL) {
		if (cred->cr_uid != node->bn_inode.bi_uid &&
		    !vnoperm(vp) && (error = suser_ucred(cred)) != 0)
			return (error);
		if (cred->cr_uid != 0 && !vnoperm(vp)) {
			if (vp->v_type != VDIR && (vap->va_mode & S_ISTXT))
				return (EFTYPE);
			if (!groupmember(node->bn_inode.bi_gid, cred) &&
			    (vap->va_mode & S_ISGID))
				return (EPERM);
		}
	}
	if ((vap->va_vaflags & VA_UTIMES_CHANGE) ||
	    vap->va_atime.tv_nsec != VNOVAL ||
	    vap->va_mtime.tv_nsec != VNOVAL) {
		if (cred->cr_uid != node->bn_inode.bi_uid &&
		    !vnoperm(vp) && (error = suser_ucred(cred)) != 0 &&
		    ((vap->va_vaflags & VA_UTIMES_NULL) == 0 ||
		    (error = VOP_ACCESS(vp, VWRITE, cred, ap->a_p)) != 0))
			return (error);
	}

	if (uid != node->bn_inode.bi_uid)
		dirty |= BTRFS_INODE_DIRTY_UID;
	if (gid != node->bn_inode.bi_gid)
		dirty |= BTRFS_INODE_DIRTY_GID;
	if (vap->va_mode != (mode_t)VNOVAL &&
	    (vap->va_mode & ALLPERMS) !=
	    (node->bn_inode.bi_mode & ALLPERMS))
		dirty |= BTRFS_INODE_DIRTY_MODE;
	if (vap->va_atime.tv_nsec != VNOVAL)
		dirty |= BTRFS_INODE_DIRTY_ATIME;
	if (vap->va_mtime.tv_nsec != VNOVAL)
		dirty |= BTRFS_INODE_DIRTY_MTIME;
	if (vap->va_vaflags & VA_UTIMES_CHANGE)
		dirty |= BTRFS_INODE_DIRTY_CTIME;
	if (dirty == 0)
		return (0);

	reservation.btr_metadata =
	    (uint64_t)letoh32(bmp->bm_super.nodesize) *
	    BTRFS_VOP_METADATA_BLOCKS;
	error = btrfs_trans_join(bmp, &reservation, &handle);
	if (error != 0)
		return (error);
	memcpy(&saved, &node->bn_inode, sizeof(saved));
	getnanotime(&now);
	node->bn_inode.bi_uid = uid;
	node->bn_inode.bi_gid = gid;
	if (vap->va_mode != (mode_t)VNOVAL)
		node->bn_inode.bi_mode = (node->bn_inode.bi_mode & S_IFMT) |
		    (vap->va_mode & ALLPERMS);
	if (vap->va_atime.tv_nsec != VNOVAL)
		node->bn_inode.bi_atime = vap->va_atime;
	if (vap->va_mtime.tv_nsec != VNOVAL)
		node->bn_inode.bi_mtime = vap->va_mtime;
	if ((uid != saved.bi_uid || gid != saved.bi_gid) &&
	    cred->cr_uid != 0 && !vnoperm(vp)) {
		node->bn_inode.bi_mode &= ~(S_ISUID | S_ISGID);
		dirty |= BTRFS_INODE_DIRTY_MODE;
	}
	node->bn_inode.bi_ctime = now;
	dirty |= BTRFS_INODE_DIRTY_CTIME;
	node->bn_inode.bi_dirty_fields |= dirty;
	node->bn_inode.bi_last_dirty_transid =
	    handle->bth_transaction->bt_generation;
	error = btrfs_write_inode(handle, node);
	if (error != 0)
		btrfs_trans_abort(handle, error);
	end_error = btrfs_trans_end(handle);
	if (error == 0)
		error = end_error;
	if (error != 0) {
		memcpy(&node->bn_inode, &saved, sizeof(saved));
		return (error);
	}
	VN_KNOTE(vp, NOTE_ATTRIB);
	return (0);
}

static int
btrfs_read_regular_extent(struct btrfs_node *node,
    const struct btrfs_file_extent *extent, uint64_t file_offset, size_t size,
    uint8_t *destination)
{
	struct btrfs_mount *bmp = node->bn_mount;
	struct buf *bp = NULL;
	uint32_t *csums = NULL;
	uint64_t block, logical, relative;
	uint64_t csum_length, csum_start;
	uint64_t inode_flags;
	const uint32_t *expectedp;
	uint32_t sectorsize;
	size_t chunk, nsectors, offset;
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
		csums = mallocarray(nsectors, sizeof(*csums), M_BTRFS,
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
			expectedp = &csums[(block - csum_start) / sectorsize];
		error = btrfs_read_data_block(bmp, block, expectedp, &bp);
		if (error != 0)
			break;
		memcpy(destination, (uint8_t *)bp->b_data + offset, chunk);
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
		free(csums, M_BTRFS, nsectors * sizeof(*csums));
	return (error);
}

static int
btrfs_read_file_range(struct btrfs_node *node, uint64_t offset, size_t length,
    uint8_t *destination)
{
	struct btrfs_mount *bmp = node->bn_mount;
	struct btrfs_file_extent extent;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	uint64_t available, end, file_size;
	size_t size;
	int error = 0;

	file_size = node->bn_inode.bi_size;
	if (destination == NULL || offset > file_size ||
	    length > file_size - offset)
		return (EINVAL);
	if (length == 0)
		return (0);
	end = offset + length;

	error = btrfs_get_root(bmp, node->bn_treeid, &root);
	if (error != 0)
		return (error);

	while (error == 0 && offset < end) {
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
btrfs_strategy(void *v)
{
	struct vop_strategy_args *ap = v;
	struct buf *bp = ap->a_bp;
	struct btrfs_node *node = VTOBTRFS(bp->b_vp);
	uint64_t file_offset, file_size;
	uint32_t sectorsize;
	size_t length;
	int error, s;

	sectorsize = letoh32(node->bn_mount->bm_super.sectorsize);
	error = 0;
	if ((bp->b_flags & B_READ) == 0)
		error = EROFS;
	else if (bp->b_lblkno < 0 ||
	    (uint64_t)bp->b_lblkno > UINT64_MAX / sectorsize ||
	    bp->b_bcount != sectorsize)
		error = EINVAL;

	if (error == 0) {
		file_offset = (uint64_t)bp->b_lblkno * sectorsize;
		file_size = node->bn_inode.bi_size;
		clrbuf(bp);
		if (file_offset < file_size) {
			length = MIN((uint64_t)bp->b_bcount,
			    file_size - file_offset);
			error = btrfs_read_ordered_sector(node, file_offset,
			    bp->b_data);
			if (error == ENOENT)
				error = btrfs_read_file_range(node, file_offset,
				    length, bp->b_data);
		}
	}
	if (error != 0) {
		bp->b_error = error;
		bp->b_flags |= B_ERROR;
	}

	s = splbio();
	biodone(bp);
	splx(s);
	return (error);
}

static int
btrfs_read(void *v)
{
	struct vop_read_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct btrfs_node *node = VTOBTRFS(vp);
	struct btrfs_mount *bmp = node->bn_mount;
	struct buf *bp = NULL;
	struct uio *uio = ap->a_uio;
	uint64_t file_size;
	uint32_t sectorsize;
	daddr_t block;
	size_t offset, size;
	int error = 0;

	KASSERT(VOP_ISLOCKED(vp));
	if (vp->v_type != VREG)
		return (EISDIR);
	if (uio->uio_rw != UIO_READ || uio->uio_offset < 0)
		return (EINVAL);
	if (uio->uio_resid == 0)
		return (0);

	file_size = node->bn_inode.bi_size;
	sectorsize = letoh32(bmp->bm_super.sectorsize);
	while (uio->uio_resid != 0 &&
	    (uint64_t)uio->uio_offset < file_size) {
		block = (uint64_t)uio->uio_offset / sectorsize;
		offset = (uint64_t)uio->uio_offset & (sectorsize - 1);
		size = MIN((size_t)(sectorsize - offset), uio->uio_resid);
		if (size > file_size - (uint64_t)uio->uio_offset)
			size = file_size - (uint64_t)uio->uio_offset;

		error = bread(vp, block, sectorsize, &bp);
		if (error != 0)
			break;
		error = uiomove((uint8_t *)bp->b_data + offset, size, uio);
		brelse(bp);
		bp = NULL;
		if (error != 0)
			break;
	}
	if (bp != NULL)
		brelse(bp);
	return (error);
}

static int
btrfs_write(void *v)
{
	struct vop_write_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct btrfs_node *node = VTOBTRFS(vp);
	struct btrfs_mount *bmp = node->bn_mount;
	struct btrfs_inode saved;
	struct btrfs_trans_handle *handle = NULL;
	struct btrfs_trans_reservation reservation = { 0 };
	struct buf *bp = NULL;
	struct uio *uio = ap->a_uio;
	struct timespec now;
	uint8_t *data = NULL;
	uint64_t file_offset, file_size;
	uint32_t sectorsize;
	daddr_t block;
	off_t move_offset, unit_offset;
	size_t move_resid, moved, offset, resid, size;
	ssize_t overrun;
	int end_error, error = 0, extended = 0, move_error, wrote = 0;

	KASSERT(VOP_ISLOCKED(vp));
	if (vp->v_type != VREG)
		return (EISDIR);
	if (uio->uio_rw != UIO_WRITE)
		return (EINVAL);
	if (uio->uio_resid == 0)
		return (0);
	if ((vp->v_mount->mnt_flag & MNT_RDONLY) ||
	    bmp->bm_subvol_readonly ||
	    node->bn_treeid != bmp->bm_treeid ||
	    (node->bn_inode.bi_flags & BTRFS_INODE_READONLY))
		return (EROFS);
	if (node->bn_inode.bi_flags & BTRFS_INODE_IMMUTABLE)
		return (EPERM);
	if ((ap->a_ioflag & IO_APPEND) &&
	    node->bn_inode.bi_size > LLONG_MAX)
		return (EFBIG);
	if (ap->a_ioflag & IO_APPEND)
		uio->uio_offset = node->bn_inode.bi_size;
	if ((node->bn_inode.bi_flags & BTRFS_INODE_APPEND) &&
	    (uint64_t)uio->uio_offset != node->bn_inode.bi_size)
		return (EPERM);
	if (uio->uio_offset < 0 ||
	    (uint64_t)uio->uio_offset > LLONG_MAX ||
	    uio->uio_resid > LLONG_MAX - (uint64_t)uio->uio_offset)
		return (EFBIG);
	if ((node->bn_inode.bi_flags & BTRFS_INODE_NODATASUM) != 0)
		return (EOPNOTSUPP);
	error = vn_fsizechk(vp, uio, ap->a_ioflag, &overrun);
	if (error != 0)
		return (error);
	unit_offset = uio->uio_offset;
	resid = uio->uio_resid;

	sectorsize = letoh32(bmp->bm_super.sectorsize);
	reservation.btr_data = sectorsize;
	reservation.btr_metadata =
	    (uint64_t)letoh32(bmp->bm_super.nodesize) *
	    BTRFS_VOP_METADATA_BLOCKS;
	data = malloc(sectorsize, M_BTRFS, M_WAITOK);
	while (uio->uio_resid != 0) {
		block = (uint64_t)uio->uio_offset / sectorsize;
		offset = (uint64_t)uio->uio_offset & (sectorsize - 1);
		size = MIN((size_t)(sectorsize - offset), uio->uio_resid);
		error = bread(vp, block, sectorsize, &bp);
		if (error != 0)
			break;
		memcpy(data, bp->b_data, sectorsize);

		move_offset = uio->uio_offset;
		move_resid = uio->uio_resid;
		move_error = uiomove(data + offset, size, uio);
		moved = move_resid - uio->uio_resid;
		if (moved == 0) {
			error = move_error;
			brelse(bp);
			bp = NULL;
			break;
		}
		file_offset = (uint64_t)block * sectorsize;
		file_size = MAX(node->bn_inode.bi_size,
		    (uint64_t)move_offset + moved);
		error = btrfs_trans_join(bmp, &reservation, &handle);
		if (error == 0) {
			memcpy(&saved, &node->bn_inode, sizeof(saved));
			getnanotime(&now);
			node->bn_inode.bi_mtime = now;
			node->bn_inode.bi_ctime = now;
			node->bn_inode.bi_dirty_fields |=
			    BTRFS_INODE_DIRTY_MTIME |
			    BTRFS_INODE_DIRTY_CTIME;
			if (ap->a_cred != NOCRED &&
			    ap->a_cred->cr_uid != 0 && !vnoperm(vp) &&
			    (node->bn_inode.bi_mode &
			    (S_ISUID | S_ISGID))) {
				node->bn_inode.bi_mode &=
				    ~(S_ISUID | S_ISGID);
				node->bn_inode.bi_dirty_fields |=
				    BTRFS_INODE_DIRTY_MODE;
			}
			node->bn_inode.bi_last_dirty_transid =
			    handle->bth_transaction->bt_generation;
			error = btrfs_write_file_sector(handle, node,
			    file_offset, data, file_size);
			end_error = btrfs_trans_end(handle);
			handle = NULL;
			if (error == 0)
				error = end_error;
			if (error != 0)
				memcpy(&node->bn_inode, &saved, sizeof(saved));
		}
		if (error != 0) {
			uio->uio_offset = move_offset;
			uio->uio_resid = move_resid;
			brelse(bp);
			bp = NULL;
			break;
		}

		(void)uvm_vnp_uncache(vp);
		memcpy(bp->b_data, data, sectorsize);
		if (ap->a_ioflag & IO_NOCACHE)
			bp->b_flags |= B_NOCACHE;
		brelse(bp);
		bp = NULL;
		if (file_size > saved.bi_size) {
			uvm_vnp_setsize(vp, file_size);
			extended = 1;
		}
		wrote = 1;
		if (move_error != 0) {
			error = move_error;
			break;
		}
	}
	if (bp != NULL)
		brelse(bp);
	free(data, M_BTRFS, sectorsize);
	if (error != 0 && (ap->a_ioflag & IO_UNIT)) {
		uio->uio_offset = unit_offset;
		uio->uio_resid = resid;
	}
	uio->uio_resid += overrun;
	if (wrote)
		VN_KNOTE(vp, NOTE_WRITE | (extended ? NOTE_EXTEND : 0));
	if (wrote && (ap->a_ioflag & IO_SYNC)) {
		end_error = btrfs_trans_commit(bmp,
		    node->bn_inode.bi_last_dirty_transid,
		    uio->uio_procp != NULL ? uio->uio_procp : curproc);
		if (error == 0)
			error = end_error;
	}
	return (error);
}

static int
btrfs_fsync(void *v)
{
	struct vop_fsync_args *ap = v;
	struct btrfs_node *node = VTOBTRFS(ap->a_vp);
	struct btrfs_mount *bmp = node->bn_mount;
	struct btrfs_transaction *trans;
	uint64_t generation;
	int error = 0;

	KASSERT(VOP_ISLOCKED(ap->a_vp));
	if ((bmp->bm_open_flags & FWRITE) == 0)
		return (0);
	mtx_enter(&bmp->bm_trans_mtx);
	trans = bmp->bm_transaction;
	if (trans->bt_state == BTRFS_TRANS_ABORTED)
		error = trans->bt_error != 0 ? trans->bt_error : EIO;
	generation = node->bn_inode.bi_last_dirty_transid;
	mtx_leave(&bmp->bm_trans_mtx);
	if (error != 0 || generation == 0)
		return (error);
	return (btrfs_trans_commit(bmp, generation, ap->a_p));
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
	struct btrfs_root *root;
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

	file_size = node->bn_inode.bi_size;
	if (file_size == 0)
		return (EINVAL);
	error = btrfs_get_root(bmp, node->bn_treeid, &root);
	if (error != 0)
		return (error);
	error = btrfs_find_file_extent(bmp, root, &path, node->bn_ino, 0,
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
	struct btrfs_root *root;
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
			error = btrfs_get_root(bmp, node->bn_treeid, &root);
			if (error == 0)
				error = btrfs_find_dir_parent(root,
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
	error = btrfs_get_root(bmp, node->bn_treeid, &root);
	if (error != 0)
		goto out;
	error = btrfs_iterate_directory(root, node->bn_ino,
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
