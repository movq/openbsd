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

/*
 * Vnodes use native OpenBSD locking, advisory locks and kqueue facilities.
 * FIFOs use the shared pipe implementation, and device nodes use special
 * device operations and aliases, including nodev and securelevel policy.
 * Their I/O remains available on read-only mounts. Size changes to devices,
 * FIFOs and sockets are no-ops, as on FFS.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/btrfsio.h>
#include <sys/dirent.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/event.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/lock.h>
#include <sys/lockf.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/pool.h>
#include <sys/proc.h>
#include <sys/specdev.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <sys/vnode.h>

#include <miscfs/fifofs/fifo.h>

#include <btrfs/btrfs_var.h>
#include <btrfs/btrfs_data.h>

static int	btrfs_lookup(void *);
static int	btrfs_create(void *);
static int	btrfs_mkdir(void *);
static int	btrfs_mknod(void *);
static int	btrfs_symlink(void *);
static int	btrfs_link(void *);
static int	btrfs_remove(void *);
static int	btrfs_rmdir(void *);
static int	btrfs_rename(void *);
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
static int	btrfs_spec_fsync(void *);
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
static int	btrfs_advlock(void *);
static int	btrfs_kqfilter(void *);
static void	filt_btrfsdetach(struct knote *);
static int	filt_btrfsread(struct knote *, long);
static int	filt_btrfswrite(struct knote *, long);
static int	filt_btrfsvnode(struct knote *, long);
#ifdef FIFO
static int	btrfs_fifo_reclaim(void *);
#endif

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
	.vop_kqfilter	= btrfs_kqfilter,
	.vop_revoke	= vop_generic_revoke,
	.vop_fsync	= btrfs_fsync,
	.vop_remove	= btrfs_remove,
	.vop_link	= btrfs_link,
	.vop_rename	= btrfs_rename,
	.vop_mkdir	= btrfs_mkdir,
	.vop_rmdir	= btrfs_rmdir,
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
	.vop_advlock	= btrfs_advlock,
	.vop_bwrite	= vop_generic_bwrite,
};

const struct vops btrfs_spec_vops = {
	.vop_access	= btrfs_access,
	.vop_getattr	= btrfs_getattr,
	.vop_setattr	= btrfs_setattr,
	.vop_fsync	= btrfs_spec_fsync,
	.vop_inactive	= btrfs_inactive,
	.vop_reclaim	= btrfs_reclaim,
	.vop_lock	= btrfs_lock,
	.vop_unlock	= btrfs_unlock,
	.vop_print	= btrfs_print,
	.vop_islocked	= btrfs_islocked,

	/* Keep in sync with spec_vops. */
	.vop_lookup	= vop_generic_lookup,
	.vop_create	= vop_generic_badop,
	.vop_mknod	= vop_generic_badop,
	.vop_open	= spec_open,
	.vop_close	= spec_close,
	.vop_read	= spec_read,
	.vop_write	= spec_write,
	.vop_ioctl	= spec_ioctl,
	.vop_kqfilter	= spec_kqfilter,
	.vop_revoke	= vop_generic_revoke,
	.vop_remove	= vop_generic_badop,
	.vop_link	= vop_generic_badop,
	.vop_rename	= vop_generic_badop,
	.vop_mkdir	= vop_generic_badop,
	.vop_rmdir	= vop_generic_badop,
	.vop_symlink	= vop_generic_badop,
	.vop_readdir	= vop_generic_badop,
	.vop_readlink	= vop_generic_badop,
	.vop_abortop	= vop_generic_badop,
	.vop_bmap	= vop_generic_bmap,
	.vop_strategy	= spec_strategy,
	.vop_pathconf	= spec_pathconf,
	.vop_advlock	= spec_advlock,
	.vop_bwrite	= vop_generic_bwrite,
};

#ifdef FIFO
const struct vops btrfs_fifo_vops = {
	.vop_access	= btrfs_access,
	.vop_getattr	= btrfs_getattr,
	.vop_setattr	= btrfs_setattr,
	.vop_fsync	= btrfs_fsync,
	.vop_inactive	= btrfs_inactive,
	.vop_reclaim	= btrfs_fifo_reclaim,
	.vop_lock	= btrfs_lock,
	.vop_unlock	= btrfs_unlock,
	.vop_print	= btrfs_print,
	.vop_islocked	= btrfs_islocked,
	.vop_bwrite	= vop_generic_bwrite,

	/* Keep in sync with fifo_vops. */
	.vop_lookup	= vop_generic_lookup,
	.vop_create	= vop_generic_badop,
	.vop_mknod	= vop_generic_badop,
	.vop_open	= fifo_open,
	.vop_close	= fifo_close,
	.vop_read	= fifo_read,
	.vop_write	= fifo_write,
	.vop_ioctl	= fifo_ioctl,
	.vop_kqfilter	= fifo_kqfilter,
	.vop_revoke	= vop_generic_revoke,
	.vop_remove	= vop_generic_badop,
	.vop_link	= vop_generic_badop,
	.vop_rename	= vop_generic_badop,
	.vop_mkdir	= vop_generic_badop,
	.vop_rmdir	= vop_generic_badop,
	.vop_symlink	= vop_generic_badop,
	.vop_readdir	= vop_generic_badop,
	.vop_readlink	= vop_generic_badop,
	.vop_abortop	= vop_generic_badop,
	.vop_bmap	= vop_generic_bmap,
	.vop_strategy	= vop_generic_badop,
	.vop_pathconf	= fifo_pathconf,
	.vop_advlock	= fifo_advlock,
};
#endif

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
	struct btrfs_fs *bmp = node->bn_mount;
	struct btrfs_mount *view = VFSTOBTRFSVIEW(dvp->v_mount);
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
	if (node->bn_inode.bi_nlink == 0)
		return (ENOENT);
	error = VOP_ACCESS(dvp, VEXEC, cnp->cn_cred, cnp->cn_proc);
	if (error != 0)
		return (error);
	if (lastcn && (cnp->cn_nameiop == DELETE ||
	    cnp->cn_nameiop == RENAME)) {
		error = VOP_ACCESS(dvp, VWRITE, cnp->cn_cred, cnp->cn_proc);
		if (error != 0)
			return (error);
		cnp->cn_flags |= SAVENAME;
	}

	error = cache_lookup(dvp, vpp, cnp);
	if (error >= 0)
		return (error);
	error = 0;

	if (cnp->cn_flags & ISDOTDOT) {
		parent_treeid = node->bn_treeid;
		if (node->bn_ino == view->bmv_root_dirid &&
		    node->bn_treeid == view->bmv_treeid) {
			vref(dvp);
			*vpp = dvp;
			goto found;
		}
		if (node->bn_stub_parent != 0) {
			parent = node->bn_stub_parent;
		} else if (node->bn_ino == BTRFS_FIRST_FREE_OBJECTID) {
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
	if (node->bn_stub_parent != 0) {
		error = ENOENT;
		goto out;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.blc_name = cnp->cn_nameptr;
	ctx.blc_namelen = cnp->cn_namelen;
	error = btrfs_get_root(bmp, node->bn_treeid, &root);
	if (error != 0)
		goto out;
	error = btrfs_lookup_directory(root, node->bn_ino,
	    cnp->cn_nameptr, cnp->cn_namelen,
	    btrfs_lookup_entry, &ctx);
	if (error == BTRFS_LOOKUP_FOUND)
		error = 0;
	else if (error == 0)
		error = ENOENT;
	if (error != 0) {
		if (error == ENOENT && lastcn &&
		    (cnp->cn_nameiop == CREATE || cnp->cn_nameiop == RENAME)) {
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

	if (ctx.blc_subvolume) {
		/* Directory entries must agree with the ancestry used at mount. */
		if (ctx.blc_objectid == BTRFS_FS_TREE_OBJECTID) {
			error = EINVAL;
			goto out;
		}
		error = btrfs_check_subvol_link(root, node->bn_ino,
		    ctx.blc_objectid, cnp->cn_nameptr, cnp->cn_namelen);
		if (error == ENOENT) {
			error = btrfs_vget_stub(node, ctx.blc_objectid, vpp);
			if (error == 0)
				goto found;
			goto out;
		}
		if (error != 0)
			goto out;
		error = btrfs_vget_tree(dvp->v_mount, ctx.blc_objectid,
		    BTRFS_FIRST_FREE_OBJECTID, vpp);
	} else
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
	    vap->va_type != VLNK && vap->va_type != VSOCK &&
	    vap->va_type != VCHR && vap->va_type != VBLK
#ifdef FIFO
	    && vap->va_type != VFIFO
#endif
	    ) {
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
	    mode, cnp->cn_cred->cr_uid, dir->bn_inode.bi_gid, vap->va_rdev,
	    link, vpp);
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
	int error;

	error = btrfs_makeinode(ap->a_dvp, ap->a_vpp, ap->a_cnp,
	    ap->a_vap, NULL);
	/* mknod's caller releases the parent only. */
	if (error == 0) {
		vput(*ap->a_vpp);
		*ap->a_vpp = NULL;
	}
	return (error);
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
	struct vnode *dvp = ap->a_dvp, *vp = ap->a_vp;
	struct componentname *cnp = ap->a_cnp;
	struct btrfs_node *dir = VTOBTRFS(dvp), *node;
	int error;

	KASSERT(VOP_ISLOCKED(dvp));
	KASSERT(cnp->cn_flags & HASBUF);
	if (vp->v_type == VDIR) {
		error = EPERM;
		goto out;
	}
	error = vn_lock(vp, LK_EXCLUSIVE);
	if (error != 0)
		goto out;
	node = VTOBTRFS(vp);
	if (vp->v_mount != dvp->v_mount ||
	    node->bn_treeid != dir->bn_treeid) {
		error = EXDEV;
		goto unlock;
	}
	error = VOP_ACCESS(dvp, VWRITE | VEXEC, cnp->cn_cred, cnp->cn_proc);
	if (error != 0)
		goto unlock;
	if (node->bn_inode.bi_flags & BTRFS_INODE_READONLY) {
		error = EROFS;
		goto unlock;
	}
	if (node->bn_inode.bi_flags &
	    (BTRFS_INODE_IMMUTABLE | BTRFS_INODE_APPEND)) {
		error = EPERM;
		goto unlock;
	}
	if (node->bn_inode.bi_nlink >= LINK_MAX) {
		error = EMLINK;
		goto unlock;
	}
	if (node->bn_inode.bi_nlink == 0) {
		error = ENOENT;
		goto unlock;
	}
	error = btrfs_link_inode(dir, node, cnp->cn_nameptr, cnp->cn_namelen);
	if (error != 0)
		goto unlock;
	cache_purge(dvp);
	if (cnp->cn_flags & MAKEENTRY)
		cache_enter(dvp, vp, cnp);
	VN_KNOTE(vp, NOTE_LINK);
	VN_KNOTE(dvp, NOTE_WRITE);
	if ((dvp->v_mount->mnt_flag & MNT_SYNCHRONOUS) ||
	    (dir->bn_inode.bi_flags &
	    (BTRFS_INODE_SYNC | BTRFS_INODE_DIRSYNC)) ||
	    (node->bn_inode.bi_flags & BTRFS_INODE_SYNC))
		error = btrfs_trans_commit(dir->bn_mount,
		    node->bn_inode.bi_last_dirty_transid, cnp->cn_proc);
unlock:
	VOP_UNLOCK(vp);
out:
	VOP_ABORTOP(dvp, cnp);
	vput(dvp);
	return (error);
}

static int
btrfs_node_readonly(struct btrfs_node *node)
{
	struct btrfs_mount *view =
	    VFSTOBTRFSVIEW(node->bn_vnode->v_mount);

	return (node->bn_mount->bm_readonly ||
	    (view->bmv_mount->mnt_flag & MNT_RDONLY) ||
	    (node->bn_root->br_flags & BTRFS_ROOT_SUBVOL_RDONLY) ||
	    (node->bn_inode.bi_flags & BTRFS_INODE_READONLY));
}

static int
btrfs_remove_name(struct vnode *dvp, struct vnode *vp,
    struct componentname *cnp, int directory)
{
	struct btrfs_node *dir = VTOBTRFS(dvp), *node = VTOBTRFS(vp);
	int error;

	KASSERT(VOP_ISLOCKED(dvp));
	KASSERT(VOP_ISLOCKED(vp));
	if (!directory && vp->v_type == VDIR) {
		error = EPERM;
		goto out;
	}
	if (vp->v_mount != dvp->v_mount ||
	    node->bn_treeid != dir->bn_treeid) {
		error = directory ? EBUSY : EXDEV;
		goto out;
	}
	if (directory && node->bn_ino == BTRFS_FIRST_FREE_OBJECTID) {
		error = EBUSY;
		goto out;
	}
	if (btrfs_node_readonly(node)) {
		error = EROFS;
		goto out;
	}
	error = VOP_ACCESS(dvp, VWRITE | VEXEC, cnp->cn_cred, cnp->cn_proc);
	if (error != 0)
		goto out;
	if ((node->bn_inode.bi_flags &
	    (BTRFS_INODE_IMMUTABLE | BTRFS_INODE_APPEND)) ||
	    (dir->bn_inode.bi_flags & BTRFS_INODE_APPEND)) {
		error = EPERM;
		goto out;
	}
	if ((dir->bn_inode.bi_mode & S_ISTXT) &&
	    cnp->cn_cred->cr_uid != 0 &&
	    cnp->cn_cred->cr_uid != dir->bn_inode.bi_uid &&
	    cnp->cn_cred->cr_uid != node->bn_inode.bi_uid && !vnoperm(dvp)) {
		error = EPERM;
		goto out;
	}
	error = btrfs_unlink_inode(dir, node, cnp->cn_nameptr, cnp->cn_namelen);
	if (error != 0)
		goto out;
	cache_purge(dvp);
	cache_purge(vp);
	VN_KNOTE(vp, NOTE_DELETE);
	VN_KNOTE(dvp, NOTE_WRITE);
	if ((dvp->v_mount->mnt_flag & MNT_SYNCHRONOUS) ||
	    (dir->bn_inode.bi_flags &
	    (BTRFS_INODE_SYNC | BTRFS_INODE_DIRSYNC)) ||
	    (node->bn_inode.bi_flags & BTRFS_INODE_SYNC))
		error = btrfs_trans_commit(dir->bn_mount,
		    node->bn_inode.bi_last_dirty_transid, cnp->cn_proc);
out:
	VOP_ABORTOP(dvp, cnp);
	return (error);
}

static int
btrfs_remove(void *v)
{
	struct vop_remove_args *ap = v;

	return (btrfs_remove_name(ap->a_dvp, ap->a_vp, ap->a_cnp, 0));
}

static int
btrfs_rmdir(void *v)
{
	struct vop_rmdir_args *ap = v;
	int error;

	error = btrfs_remove_name(ap->a_dvp, ap->a_vp, ap->a_cnp, 1);
	vput(ap->a_vp);
	vput(ap->a_dvp);
	return (error);
}

/*
 * Revalidate directly under the parent lock, without the name cache or
 * acquiring a child lock. A rename may have dropped all incoming locks.
 */
static int
btrfs_rename_lookup(struct vnode *dvp, struct componentname *cnp,
    uint64_t *inop)
{
	struct btrfs_node *dir = VTOBTRFS(dvp);
	struct btrfs_lookup_ctx ctx = { 0 };
	int error;

	*inop = 0;
	if (dir->bn_inode.bi_nlink == 0)
		return (ENOENT);
	error = VOP_ACCESS(dvp, VEXEC | VWRITE, cnp->cn_cred, cnp->cn_proc);
	if (error != 0)
		return (error);
	ctx.blc_name = cnp->cn_nameptr;
	ctx.blc_namelen = cnp->cn_namelen;
	error = btrfs_lookup_directory(dir->bn_root, dir->bn_ino,
	    ctx.blc_name, ctx.blc_namelen, btrfs_lookup_entry, &ctx);
	if (error == BTRFS_LOOKUP_FOUND) {
		if (ctx.blc_subvolume)
			return (EBUSY);
		*inop = ctx.blc_objectid;
		return (0);
	}
	return (error == 0 ? ENOENT : error);
}

static int
btrfs_rename_permitted(struct vnode *dvp, struct vnode *vp,
    struct componentname *cnp)
{
	struct btrfs_node *dir = VTOBTRFS(dvp), *node = VTOBTRFS(vp);

	if (btrfs_node_readonly(node) || btrfs_node_readonly(dir))
		return (EROFS);
	if ((node->bn_inode.bi_flags &
	    (BTRFS_INODE_IMMUTABLE | BTRFS_INODE_APPEND)) ||
	    (dir->bn_inode.bi_flags &
	    (BTRFS_INODE_IMMUTABLE | BTRFS_INODE_APPEND)))
		return (EPERM);
	if ((dir->bn_inode.bi_mode & S_ISTXT) &&
	    cnp->cn_cred->cr_uid != 0 &&
	    cnp->cn_cred->cr_uid != dir->bn_inode.bi_uid &&
	    cnp->cn_cred->cr_uid != node->bn_inode.bi_uid && !vnoperm(dvp))
		return (EPERM);
	return (0);
}

/*
 * Only rename can move a linked directory's ancestry. The rename lock and
 * locked destination therefore stabilize this walk. Detect corrupt cycles
 * with a second cursor rather than imposing a filesystem depth limit.
 */
static int
btrfs_rename_ancestry(struct btrfs_node *node, struct btrfs_node *dir)
{
	uint64_t slow = dir->bn_ino, fast = slow;
	int error, i;

	while (slow != BTRFS_FIRST_FREE_OBJECTID) {
		if (slow == node->bn_ino)
			return (EINVAL);
		error = btrfs_find_dir_parent(dir->bn_root, slow, &slow);
		if (error != 0)
			return (error);
		for (i = 0; i < 2 && fast != BTRFS_FIRST_FREE_OBJECTID; i++) {
			error = btrfs_find_dir_parent(dir->bn_root, fast, &fast);
			if (error != 0)
				return (error);
		}
		if (slow == fast && slow != BTRFS_FIRST_FREE_OBJECTID)
			return (EINVAL);
	}
	return (0);
}

/*
 * Release incoming vnode locks before taking the filesystem rename lock.
 * Acquire involved vnodes without blocking, dropping all locks before waiting
 * on a contended vnode alone, then revalidate names directly. The rename lock
 * stabilizes ancestry for cycle checks. One reserved handle preserves source
 * inode identity and atomically replaces the destination using unlink's
 * orphan lifecycle, so open destination descriptors survive.
 */
static int
btrfs_rename(void *v)
{
	struct vop_rename_args *ap = v;
	struct vnode *fdvp = ap->a_fdvp, *fvp = ap->a_fvp;
	struct vnode *tdvp = ap->a_tdvp, *tvp = ap->a_tvp;
	struct componentname *fcnp = ap->a_fcnp, *tcnp = ap->a_tcnp;
	struct vnode *vps[4], *locked[4], *newvp, *waitvp;
	struct btrfs_fs *bmp;
	struct btrfs_node *fdir, *tdir, *node, *target;
	uint64_t fino, tino;
	unsigned int i, j, nlocked = 0;
	int error, sync = 0;

	/* VFS passes referenced sources and locked destinations. */
	VOP_UNLOCK(tdvp);
	if (tvp != NULL && tvp != tdvp)
		VOP_UNLOCK(tvp);
	if (fdvp->v_mount != tdvp->v_mount ||
	    fvp->v_mount != fdvp->v_mount ||
	    (tvp != NULL && tvp->v_mount != fdvp->v_mount)) {
		error = EXDEV;
		goto out;
	}
	if ((fcnp->cn_flags & ISDOTDOT) || (tcnp->cn_flags & ISDOTDOT) ||
	    (fcnp->cn_namelen == 1 && fcnp->cn_nameptr[0] == '.') ||
	    (tcnp->cn_namelen == 1 && tcnp->cn_nameptr[0] == '.')) {
		error = EINVAL;
		goto out;
	}
	fdir = VTOBTRFS(fdvp);
	tdir = VTOBTRFS(tdvp);
	bmp = fdir->bn_mount;
	if (fdir->bn_treeid != tdir->bn_treeid) {
		error = EXDEV;
		goto out;
	}
	rw_enter_write(&bmp->bm_rename_lock);
retry:
	/*
	 * Never wait for a vnode while holding any other vnode lock.
	 * Ordinary lookup locks parents before children, including "..";
	 * inode-number ordering alone cannot prevent those inversions.
	 */
	vps[0] = fdvp;
	vps[1] = tdvp;
	vps[2] = fvp;
	vps[3] = tvp;
	for (i = 0; i < nitems(vps); i++) {
		if (vps[i] == NULL)
			continue;
		for (j = 0; j < nlocked; j++)
			if (locked[j] == vps[i])
				break;
		if (j != nlocked)
			continue;
		error = vn_lock(vps[i], LK_EXCLUSIVE | LK_NOWAIT);
		if (error != 0) {
			waitvp = vps[i];
			while (nlocked != 0)
				VOP_UNLOCK(locked[--nlocked]);
			error = vn_lock(waitvp, LK_EXCLUSIVE | LK_RETRY);
			if (error != 0)
				goto done;
			VOP_UNLOCK(waitvp);
			goto retry;
		}
		locked[nlocked++] = vps[i];
	}
	error = btrfs_rename_lookup(fdvp, fcnp, &fino);
	if (error != 0)
		goto done;
	error = btrfs_rename_lookup(tdvp, tcnp, &tino);
	if (error == ENOENT && tdir->bn_inode.bi_nlink != 0)
		error = 0;
	if (error != 0)
		goto done;
	if (fino != VTOBTRFS(fvp)->bn_ino ||
	    VTOBTRFS(fvp)->bn_treeid != fdir->bn_treeid ||
	    (tvp == NULL ? tino != 0 :
	    tino != VTOBTRFS(tvp)->bn_ino ||
	    VTOBTRFS(tvp)->bn_treeid != tdir->bn_treeid)) {
		while (nlocked != 0)
			VOP_UNLOCK(locked[--nlocked]);
		/* Obtain refreshed children alone, then recheck both names. */
		error = btrfs_vget_tree(fdvp->v_mount, fdir->bn_treeid,
		    fino, &newvp);
		if (error == ENOENT)
			goto retry;
		if (error != 0)
			goto done;
		VOP_UNLOCK(newvp);
		vrele(fvp);
		fvp = newvp;
		if (tino != 0) {
			error = btrfs_vget_tree(tdvp->v_mount, tdir->bn_treeid,
			    tino, &newvp);
			if (error == ENOENT)
				goto retry;
			if (error != 0)
				goto done;
			VOP_UNLOCK(newvp);
		} else
			newvp = NULL;
		if (tvp != NULL)
			vrele(tvp);
		tvp = newvp;
		goto retry;
	}
	if (fvp == tvp) {
		error = 0;
		goto done;
	}
	if (tvp != NULL && (fvp->v_type == VDIR) != (tvp->v_type == VDIR)) {
		error = fvp->v_type == VDIR ? ENOTDIR : EISDIR;
		goto done;
	}
	if (tvp == fdvp) {
		/* The destination contains the source and cannot be empty. */
		error = ENOTEMPTY;
		goto done;
	}
	if (fvp == fdvp || fvp == tdvp || tvp == tdvp) {
		error = EINVAL;
		goto done;
	}
	node = VTOBTRFS(fvp);
	target = tvp != NULL ? VTOBTRFS(tvp) : NULL;
	if (node->bn_ino == BTRFS_FIRST_FREE_OBJECTID ||
	    (target != NULL && target->bn_ino == BTRFS_FIRST_FREE_OBJECTID)) {
		error = EBUSY;
		goto done;
	}
	error = btrfs_rename_permitted(fdvp, fvp, fcnp);
	if (error == 0 && tvp != NULL)
		error = btrfs_rename_permitted(tdvp, tvp, tcnp);
	if (error != 0)
		goto done;
	if (fvp->v_type == VDIR) {
		error = VOP_ACCESS(fvp, VWRITE, fcnp->cn_cred, fcnp->cn_proc);
		if (error == 0 && tvp != NULL)
			error = VOP_ACCESS(tvp, VWRITE, tcnp->cn_cred, tcnp->cn_proc);
		if (error != 0) {
			error = EACCES;
			goto done;
		}
		if (fdvp != tdvp)
			error = btrfs_rename_ancestry(node, tdir);
		if (error != 0)
			goto done;
	}
	error = btrfs_rename_inode(fdir, node, fcnp->cn_nameptr,
	    fcnp->cn_namelen, tdir, target, tcnp->cn_nameptr, tcnp->cn_namelen);
	if (error != 0)
		goto done;
	for (i = 0; i < nlocked; i++) {
		struct btrfs_node *n = VTOBTRFS(locked[i]);

		cache_purge(locked[i]);
		if (n->bn_inode.bi_flags & (BTRFS_INODE_SYNC |
		    (locked[i]->v_type == VDIR ? BTRFS_INODE_DIRSYNC : 0)))
			sync = 1;
	}
	VN_KNOTE(fdvp, NOTE_WRITE);
	if (tdvp != fdvp)
		VN_KNOTE(tdvp, NOTE_WRITE);
	VN_KNOTE(fvp, NOTE_RENAME);
	if (tvp != NULL)
		VN_KNOTE(tvp, NOTE_DELETE);
	if (sync || (fdvp->v_mount->mnt_flag & MNT_SYNCHRONOUS))
		error = btrfs_trans_commit(bmp,
		    node->bn_inode.bi_last_dirty_transid, fcnp->cn_proc);
done:
	while (nlocked != 0)
		VOP_UNLOCK(locked[--nlocked]);
	rw_exit_write(&bmp->bm_rename_lock);
out:
	VOP_ABORTOP(fdvp, fcnp);
	VOP_ABORTOP(tdvp, tcnp);
	vrele(fdvp);
	vrele(tdvp);
	vrele(fvp);
	if (tvp != NULL)
		vrele(tvp);
	return (error);
}

static int
btrfs_open(void *v)
{
	struct vop_open_args *ap = v;
	struct btrfs_node *node = VTOBTRFS(ap->a_vp);

	if (ap->a_mode & FWRITE) {
		if (btrfs_node_readonly(node))
			return (EROFS);
		if (node->bn_inode.bi_flags & BTRFS_INODE_IMMUTABLE)
			return (EPERM);
		if ((node->bn_inode.bi_flags & BTRFS_INODE_APPEND) &&
		    (ap->a_mode & O_APPEND) == 0)
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
		if (ap->a_vp->v_type != VFIFO &&
		    ap->a_vp->v_type != VSOCK &&
		    ap->a_vp->v_type != VCHR &&
		    ap->a_vp->v_type != VBLK &&
		    btrfs_node_readonly(node))
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
	vap->va_fsid = node->bn_root->br_dev;
	vap->va_fileid = node->bn_ino;
	vap->va_mode = inode->bi_mode & ALLPERMS;
	vap->va_nlink = inode->bi_nlink;
	vap->va_uid = inode->bi_uid;
	vap->va_gid = inode->bi_gid;
	vap->va_atime = inode->bi_atime;
	vap->va_mtime = inode->bi_mtime;
	vap->va_ctime = inode->bi_ctime;
	vap->va_rdev = (vp->v_type == VCHR || vp->v_type == VBLK) ?
	    vp->v_rdev : 0;
	vap->va_size = inode->bi_size;
	vap->va_flags = 0;
	if (inode->bi_flags & BTRFS_INODE_NODUMP)
		vap->va_flags |= UF_NODUMP;
	if (inode->bi_flags & BTRFS_INODE_IMMUTABLE)
		vap->va_flags |= UF_IMMUTABLE;
	if (inode->bi_flags & BTRFS_INODE_APPEND)
		vap->va_flags |= UF_APPEND;
	vap->va_gen = inode->bi_generation;
	vap->va_blocksize = letoh32(node->bn_mount->bm_super.nodesize);
	vap->va_bytes = inode->bi_nbytes;
	vap->va_type = vp->v_type;
	vap->va_filerev = inode->bi_transid;
	vap->va_vaflags = 0;
	return (0);
}

/*
 * Expose nodump, immutable and append as user flags; owners may clear even
 * immutable/append, and unmodeled btrfs flags survive encoding. System flags
 * and opaque directories are unsupported: btrfs has no separate system
 * immutable/append state for OpenBSD securelevel semantics. New writable
 * append-only opens require O_APPEND, while existing descriptors must write
 * at EOF, as on FFS.
 */
static int
btrfs_setattr(void *v)
{
	struct vop_setattr_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct btrfs_node *node = VTOBTRFS(vp);
	struct btrfs_fs *bmp = node->bn_mount;
	struct btrfs_inode saved;
	struct btrfs_trans_handle *handle = NULL;
	struct btrfs_trans_reservation reservation = { 0 };
	struct btrfs_resize_plan resize = { 0 };
	struct vattr attr = *ap->a_vap;
	struct vattr *vap = &attr;
	struct ucred *cred = ap->a_cred;
	struct timespec now;
	uid_t uid;
	gid_t gid;
	uint64_t flags;
	uint32_t dirty = 0;
	long hint = NOTE_ATTRIB;
	int end_error, error;

	KASSERT(VOP_ISLOCKED(vp));
	if (vap->va_type != VNON || vap->va_nlink != VNOVAL ||
	    vap->va_fsid != VNOVAL || vap->va_fileid != VNOVAL ||
	    vap->va_blocksize != VNOVAL || vap->va_rdev != VNOVAL ||
	    (int)vap->va_bytes != VNOVAL || vap->va_gen != VNOVAL)
		return (EINVAL);
	if (vap->va_size != VNOVAL) {
		if (vp->v_type == VDIR)
			return (EISDIR);
		if (vp->v_type == VCHR || vp->v_type == VBLK ||
		    vp->v_type == VFIFO || vp->v_type == VSOCK) {
			/*
			 * Special files have no file data to truncate. A
			 * size-only request needs no writable transaction.
			 */
			if (node->bn_inode.bi_flags &
			    (BTRFS_INODE_IMMUTABLE | BTRFS_INODE_APPEND))
				return (EPERM);
			vap->va_size = VNOVAL;
			if (vap->va_flags == VNOVAL &&
			    vap->va_uid == (uid_t)VNOVAL &&
			    vap->va_gid == (gid_t)VNOVAL &&
			    vap->va_mode == (mode_t)VNOVAL &&
			    vap->va_atime.tv_nsec == VNOVAL &&
			    vap->va_mtime.tv_nsec == VNOVAL &&
			    (vap->va_vaflags & VA_UTIMES_CHANGE) == 0)
				return (0);
		} else if (vp->v_type != VREG)
			return (EOPNOTSUPP);
		else if (vap->va_size > LLONG_MAX)
			return (EFBIG);
	}
	if ((vap->va_atime.tv_nsec != VNOVAL &&
	    (vap->va_atime.tv_nsec < 0 ||
	    vap->va_atime.tv_nsec >= 1000000000)) ||
	    (vap->va_mtime.tv_nsec != VNOVAL &&
	    (vap->va_mtime.tv_nsec < 0 ||
	    vap->va_mtime.tv_nsec >= 1000000000)))
		return (EINVAL);
	if (btrfs_node_readonly(node))
		return (EROFS);
	flags = node->bn_inode.bi_flags;
	if (vap->va_flags != VNOVAL) {
		if (cred->cr_uid != node->bn_inode.bi_uid &&
		    !vnoperm(vp) && (error = suser_ucred(cred)) != 0)
			return (error);
		/*
		 * Btrfs has one immutable/append pair.  Expose it as user
		 * flags; system flags need distinct persistent state.
		 * Preserve all unrelated format and policy bits.
		 */
		if (vap->va_flags & ~(UF_NODUMP | UF_IMMUTABLE | UF_APPEND))
			return (EOPNOTSUPP);
		flags &= ~(BTRFS_INODE_NODUMP | BTRFS_INODE_IMMUTABLE |
		    BTRFS_INODE_APPEND);
		if (vap->va_flags & UF_NODUMP)
			flags |= BTRFS_INODE_NODUMP;
		if (vap->va_flags & UF_IMMUTABLE)
			flags |= BTRFS_INODE_IMMUTABLE;
		if (vap->va_flags & UF_APPEND)
			flags |= BTRFS_INODE_APPEND;
		dirty |= BTRFS_INODE_DIRTY_FLAGS;
	}
	if ((flags & (BTRFS_INODE_IMMUTABLE | BTRFS_INODE_APPEND)) &&
	    (vap->va_flags == VNOVAL ||
	    vap->va_size != VNOVAL ||
	    vap->va_uid != (uid_t)VNOVAL || vap->va_gid != (gid_t)VNOVAL ||
	    vap->va_mode != (mode_t)VNOVAL ||
	    vap->va_atime.tv_nsec != VNOVAL ||
	    vap->va_mtime.tv_nsec != VNOVAL ||
	    (vap->va_vaflags & VA_UTIMES_CHANGE)))
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
	if (vap->va_size != VNOVAL) {
		if (vap->va_size > node->bn_inode.bi_size)
			hint |= NOTE_EXTEND;
		dirty |= BTRFS_INODE_DIRTY_SIZE | BTRFS_INODE_DIRTY_MTIME;
	}
	if (dirty == 0)
		return (0);

	if (vap->va_size != VNOVAL) {
		error = btrfs_resize_prepare(&resize, node, vap->va_size);
		if (error != 0)
			goto out;
		error = btrfs_resize_join(&resize, &handle);
	} else {
		reservation.btr_metadata =
		    (uint64_t)letoh32(bmp->bm_super.nodesize) *
		    BTRFS_VOP_METADATA_BLOCKS;
		error = btrfs_trans_join(bmp, &reservation, &handle);
	}
	if (error != 0)
		goto out;
	memcpy(&saved, &node->bn_inode, sizeof(saved));
	getnanotime(&now);
	node->bn_inode.bi_uid = uid;
	node->bn_inode.bi_gid = gid;
	node->bn_inode.bi_flags = flags;
	if (vap->va_mode != (mode_t)VNOVAL)
		node->bn_inode.bi_mode = (node->bn_inode.bi_mode & S_IFMT) |
		    (vap->va_mode & ALLPERMS);
	if (vap->va_atime.tv_nsec != VNOVAL)
		node->bn_inode.bi_atime = vap->va_atime;
	if (vap->va_mtime.tv_nsec != VNOVAL)
		node->bn_inode.bi_mtime = vap->va_mtime;
	else if (vap->va_size != VNOVAL)
		node->bn_inode.bi_mtime = now;
	if ((uid != saved.bi_uid || gid != saved.bi_gid ||
	    vap->va_size != VNOVAL) &&
	    cred->cr_uid != 0 && !vnoperm(vp)) {
		node->bn_inode.bi_mode &= ~(S_ISUID | S_ISGID);
		dirty |= BTRFS_INODE_DIRTY_MODE;
	}
	node->bn_inode.bi_ctime = now;
	dirty |= BTRFS_INODE_DIRTY_CTIME;
	node->bn_inode.bi_dirty_fields |= dirty;
	node->bn_inode.bi_last_dirty_transid =
	    handle->bth_transaction->bt_generation;
	if (vap->va_size != VNOVAL)
		error = btrfs_resize_apply(handle, &resize);
	else {
		error = btrfs_write_inode(handle, node);
		if (error != 0)
			btrfs_trans_abort(handle, error);
	}
	end_error = btrfs_trans_end(handle);
	if (error == 0)
		error = end_error;
	if (error != 0) {
		memcpy(&node->bn_inode, &saved, sizeof(saved));
		goto out;
	}
	if (vap->va_size != VNOVAL && vap->va_size != saved.bi_size) {
		(void)uvm_vnp_uncache(vp);
		if (vap->va_size < saved.bi_size) {
			/* Buffers are clean; ordered data owns pending writes. */
			(void)vinvalbuf(vp, 0, cred, ap->a_p, 0, INFSLP);
		}
		uvm_vnp_setsize(vp, node->bn_inode.bi_size);
	}
	VN_KNOTE(vp, hint);
	if (vap->va_size != VNOVAL) {
		/* The target is published; cleanup errors cannot restore saved. */
		error = btrfs_resize_finish(&resize);
		if (error != 0)
			goto out;
	}
	if ((vp->v_mount->mnt_flag & MNT_SYNCHRONOUS) ||
	    (node->bn_inode.bi_flags & BTRFS_INODE_SYNC))
		error = btrfs_trans_commit(bmp,
		    node->bn_inode.bi_last_dirty_transid, ap->a_p);
out:
	btrfs_resize_release(&resize);
	return (error);
}

/*
 * Compatibility and pager reads use the same range reader and mark buffers
 * noncacheable. Strategy writeback lacks the vnode lock required for tree
 * and inode mutation, so it is disabled.
 */
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
	/* Current file data lives in ordered payloads or physical buffers. */
	bp->b_flags |= B_NOCACHE;
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
	struct uio *uio = ap->a_uio;
	uint8_t *data;
	uint64_t file_size;
	size_t capacity, size;
	int error = 0;

	KASSERT(VOP_ISLOCKED(vp));
	if (vp->v_type != VREG)
		return (EISDIR);
	if (uio->uio_rw != UIO_READ || uio->uio_offset < 0)
		return (EINVAL);
	if (uio->uio_resid == 0)
		return (0);

	file_size = node->bn_inode.bi_size;
	if ((uint64_t)uio->uio_offset >= file_size)
		return (0);
	capacity = MIN((uint64_t)MIN(uio->uio_resid, MAXBSIZE),
	    file_size - (uint64_t)uio->uio_offset);
	data = pool_get(&node->bn_mount->bm_scratch_pool, PR_WAITOK);
	while (uio->uio_resid != 0 &&
	    (uint64_t)uio->uio_offset < file_size) {
		size = MIN(capacity, uio->uio_resid);
		if (size > file_size - (uint64_t)uio->uio_offset)
			size = file_size - (uint64_t)uio->uio_offset;

		error = btrfs_read_file_range(node, uio->uio_offset, size, data);
		if (error != 0)
			break;
		error = uiomove(data, size, uio);
		if (error != 0)
			break;
	}
	pool_put(&node->bn_mount->bm_scratch_pool, data);
	return (error);
}

/*
 * The vnode and handle keep the in-memory inode stable until it is encoded.
 * Even a later copy/read failure must publish the successfully modified prefix
 * into this transaction before the handle lets commit proceed.
 */
static int
btrfs_end_write(struct btrfs_trans_handle *handle, struct btrfs_node *node)
{
	int error, end_error;

	error = btrfs_write_inode(handle, node);
	if (error != 0)
		btrfs_trans_abort(handle, error);
	end_error = btrfs_trans_end(handle);
	return (error != 0 ? error : end_error);
}

static void
btrfs_cancel_write_alloc(struct btrfs_trans_handle *handle, uint64_t bytenr,
    uint32_t length)
{
	int error;

	error = btrfs_space_cancel_alloc(handle, bytenr, length);
	if (error != 0)
		btrfs_trans_abort(handle, error);
}

static int
btrfs_write(void *v)
{
	struct vop_write_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct btrfs_node *node = VTOBTRFS(vp);
	struct btrfs_fs *bmp = node->bn_mount;
	struct btrfs_inode saved;
	struct btrfs_trans_handle *handle = NULL;
	struct btrfs_trans_reservation reservation = { 0 };
	struct btrfs_file_extent first;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	struct uio *uio = ap->a_uio;
	struct timespec now;
	uint8_t *data = NULL;
	uint64_t file_offset, file_size, holes, metadata_reserve, metadata_unit;
	uint64_t data_reserve, planned_size, bytenr;
	uint32_t sectorsize, length, allocated_length;
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
	if (btrfs_node_readonly(node))
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
	sectorsize = letoh32(bmp->bm_super.sectorsize);
	reservation.btr_data = sectorsize;
	reservation.btr_metadata =
	    (uint64_t)letoh32(bmp->bm_super.nodesize) *
	    BTRFS_VOP_METADATA_BLOCKS;
	metadata_unit = reservation.btr_metadata;
	/*
	 * A sparse write to an inline file also materializes sector zero.
	 * Reserve both replacements before either becomes visible.
	 */
	if ((uint64_t)uio->uio_offset >= sectorsize) {
		error = btrfs_get_root(bmp, node->bn_treeid, &root);
		if (error == 0)
			error = btrfs_find_file_extent(bmp, root, &path,
			    node->bn_ino, 0, node->bn_inode.bi_size, &first);
		btrfs_release_path(&path);
		if (error != 0)
			return (error);
		if (first.bfe_type == BTRFS_FILE_EXTENT_INLINE) {
			reservation.btr_data *= 2;
			reservation.btr_metadata *= 2;
		}
	}
	metadata_reserve = reservation.btr_metadata;
	data_reserve = reservation.btr_data;
	error = vn_fsizechk(vp, uio, ap->a_ioflag, &overrun);
	if (error != 0)
		return (error);
	unit_offset = uio->uio_offset;
	resid = uio->uio_resid;

	data = pool_get(&bmp->bm_scratch_pool, PR_WAITOK);
	while (uio->uio_resid != 0) {
		block = (uint64_t)uio->uio_offset / sectorsize;
		offset = (uint64_t)uio->uio_offset & (sectorsize - 1);
		file_offset = (uint64_t)block * sectorsize;
		length = roundup(MIN(MAXBSIZE,
		    offset + uio->uio_resid), sectorsize);
		error = btrfs_file_write_length(node, file_offset, &length);
		if (error != 0)
			break;
		/*
		 * One old mapping, at most two retained pieces, one new mapping,
		 * an inode, a checksum run and their delayed references. Keep
		 * the existing worst-case single-mapping tree budget, independent
		 * of payload sector count. Inline conversion and holes are extra.
		 * Allocate before copying so fragmented space can shrink the
		 * operation without consuming the caller's uio.
		 */
		for (;;) {
			planned_size = MAX(node->bn_inode.bi_size,
			    file_offset + MIN(length, offset + uio->uio_resid));
			error = btrfs_count_file_holes(node, planned_size, &holes);
			if (error != 0)
				break;
			if (holes > (UINT64_MAX - metadata_reserve) /
			    metadata_unit) {
				error = EOVERFLOW;
				break;
			}
			reservation.btr_metadata = metadata_reserve +
			    holes * metadata_unit;
			reservation.btr_data = length + data_reserve - sectorsize;
			error = btrfs_trans_join(bmp, &reservation, &handle);
			if (error == 0) {
				error = btrfs_space_alloc(handle,
				    BTRFS_BLOCK_GROUP_DATA, length, sectorsize,
				    &bytenr);
				if (error != 0) {
					end_error = btrfs_trans_end(handle);
					handle = NULL;
					if (end_error != 0)
						error = end_error;
				}
			}
			if (error != ENOSPC || length == sectorsize)
				break;
			length = roundup(length / 2, sectorsize);
		}
		if (error != 0)
			break;
		allocated_length = length;
		size = MIN((size_t)(length - offset), uio->uio_resid);
		memset(data, 0, length);
		if (file_offset < node->bn_inode.bi_size) {
			error = btrfs_read_file_range(node, file_offset,
			    MIN(length, node->bn_inode.bi_size -
			    file_offset), data);
			if (error != 0) {
				btrfs_cancel_write_alloc(handle, bytenr,
				    allocated_length);
				break;
			}
		}

		move_offset = uio->uio_offset;
		move_resid = uio->uio_resid;
		move_error = uiomove(data + offset, size, uio);
		moved = move_resid - uio->uio_resid;
		if (moved == 0) {
			btrfs_cancel_write_alloc(handle, bytenr,
			    allocated_length);
			error = move_error;
			break;
		}
		length = roundup(offset + moved, sectorsize);
		file_size = MAX(node->bn_inode.bi_size,
		    (uint64_t)move_offset + moved);
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
			error = btrfs_write_file_range(handle, node,
			    file_offset, data, length, bytenr, allocated_length,
			    file_size, BTRFS_WRITE_DEFER_INODE |
			    (wrote ? BTRFS_WRITE_NO_INLINE : 0));
			if (error == 0) {
				error = btrfs_fill_file_holes(handle, node,
				    saved.bi_size, file_size);
				if (error != 0)
					btrfs_trans_abort(handle, error);
			}
			if (error != 0) {
				memcpy(&node->bn_inode, &saved, sizeof(saved));
				if (handle->bth_transaction->bt_state !=
				    BTRFS_TRANS_ABORTED)
					btrfs_cancel_write_alloc(handle,
					    bytenr, allocated_length);
			}
			end_error = btrfs_end_write(handle, node);
			handle = NULL;
			if (error == 0) {
				error = end_error;
				if (error != 0)
					memcpy(&node->bn_inode, &saved,
					    sizeof(saved));
			}
		}
		if (error != 0) {
			uio->uio_offset = move_offset;
			uio->uio_resid = move_resid;
			break;
		}

		(void)uvm_vnp_uncache(vp);
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
	if (handle != NULL) {
		/* A read/copy error before mutation left only a reservation. */
		end_error = btrfs_trans_end(handle);
		if (error == 0)
			error = end_error;
	}
	pool_put(&bmp->bm_scratch_pool, data);
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

/*
 * CLONE holds both regular-file vnodes throughout replay. Never wait on the
 * second while holding the first: rename can hold either as a target.
 * Open descriptors and the caller's busy mounts pin both identities.
 *
 * Commit their ordered writes before sharing: pending payloads could still
 * change in place or mask a replaced mapping. Source and destination must
 * share a filesystem and checksum policy. Offsets are sector aligned; a
 * partial final sector must end at source EOF and at or beyond destination
 * EOF. Reject same-inode overlaps.
 *
 * Each reserved handle replaces at most one destination mapping with a source
 * slice, preserving prefix/suffix owners and adding a destination file-base
 * reference to the whole allocation. Compressed slices retain decoded offsets;
 * preallocation becomes holes, and inline source data uses bounded COW copies.
 * Regular mappings require no data or checksum copy. Invalidate destination
 * vnode buffers before unlocking.
 *
 * The range is not atomic as a whole: an error can leave a completed prefix
 * and sparse growth to the destination offset. Growth past an old partial EOF
 * or inline prefix may require data COW and ordinary space.
 */
int
btrfs_clone_range(struct vnode *svp, struct vnode *dvp,
    struct btrfs_ioctl_clone *args, struct proc *p)
{
	struct btrfs_node *src = VTOBTRFS(svp), *dst = VTOBTRFS(dvp);
	struct btrfs_fs *bmp = dst->bn_mount;
	struct btrfs_file_extent source, old;
	struct btrfs_path path = { 0 };
	struct btrfs_trans_reservation reservation = { 0 };
	struct btrfs_trans_handle *handle;
	struct btrfs_inode saved;
	struct btrfs_resize_plan resize = { 0 };
	struct uio uio = { 0 };
	struct iovec iov;
	uint8_t *data = NULL;
	struct timespec now;
	uint64_t from = args->src_offset, to = args->dst_offset;
	uint64_t length = args->length, end, count, size, original_size;
	uint32_t sector = letoh32(bmp->bm_super.sectorsize);
	ssize_t overrun;
	int error, end_error, changed = 0;

retry:
	error = vn_lock(svp, LK_EXCLUSIVE | LK_RETRY);
	if (error != 0)
		return (error);
	if (dvp != svp) {
		error = vn_lock(dvp, LK_EXCLUSIVE | LK_NOWAIT);
		if (error != 0) {
			VOP_UNLOCK(svp);
			error = vn_lock(dvp, LK_EXCLUSIVE | LK_RETRY);
			if (error != 0)
				return (error);
			VOP_UNLOCK(dvp);
			goto retry;
		}
	}
	original_size = dst->bn_inode.bi_size;
	error = EINVAL;
	if (from > LLONG_MAX || length > LLONG_MAX - from ||
	    from > src->bn_inode.bi_size ||
	    length > src->bn_inode.bi_size - from ||
	    to > LLONG_MAX || length > LLONG_MAX - to ||
	    ((from | to) & (sector - 1)) != 0)
		goto out;
	end = to + length;
	if ((length & (sector - 1)) != 0 &&
	    (from + length != src->bn_inode.bi_size ||
	    end < dst->bn_inode.bi_size))
		goto out;
	if (src->bn_treeid == dst->bn_treeid &&
	    src->bn_ino == dst->bn_ino && length != 0 &&
	    to < roundup(from + length, sector) &&
	    from < roundup(end, sector))
		goto out;
	error = EROFS;
	if (btrfs_node_readonly(dst))
		goto out;
	error = EPERM;
	if (dst->bn_inode.bi_flags &
	    (BTRFS_INODE_IMMUTABLE | BTRFS_INODE_APPEND))
		goto out;
	error = EINVAL;
	/* A shared allocation must have one checksum policy for all owners. */
	if ((src->bn_inode.bi_flags ^ dst->bn_inode.bi_flags) &
	    BTRFS_INODE_NODATASUM)
		goto out;
	error = vn_writechk(dvp);
	if (error != 0 || length == 0)
		goto out;
	uio.uio_offset = to;
	uio.uio_resid = length;
	uio.uio_procp = p;
	error = vn_fsizechk(dvp, &uio, 0, &overrun);
	if (error != 0 || overrun != 0) {
		error = EFBIG;
		goto out;
	}

	/*
	 * Ordered writes may update their private allocation in place until
	 * commit. Finish them before sharing, and before replacing destination
	 * items whose pending payloads would otherwise mask the new mappings.
	 * No tree paths or handles survive this commit.
	 */
	error = btrfs_trans_commit(bmp,
	    MAX(src->bn_inode.bi_last_dirty_transid,
	    dst->bn_inode.bi_last_dirty_transid), p);
	if (error != 0)
		goto out;
	if (to > dst->bn_inode.bi_size) {
		/* Preserve/zero the old partial EOF and convert an inline prefix. */
		error = btrfs_resize_prepare(&resize, dst, to);
		if (error != 0)
			goto out;
		error = btrfs_resize_join(&resize, &handle);
		if (error != 0)
			goto out;
		saved = dst->bn_inode;
		getnanotime(&now);
		dst->bn_inode.bi_mtime = dst->bn_inode.bi_ctime = now;
		dst->bn_inode.bi_dirty_fields |= BTRFS_INODE_DIRTY_MTIME |
		    BTRFS_INODE_DIRTY_CTIME;
		if (p->p_ucred->cr_uid != 0 && !vnoperm(dvp)) {
			dst->bn_inode.bi_mode &= ~(S_ISUID | S_ISGID);
			dst->bn_inode.bi_dirty_fields |= BTRFS_INODE_DIRTY_MODE;
		}
		error = btrfs_resize_apply(handle, &resize);
		end_error = btrfs_trans_end(handle);
		if (error == 0)
			error = end_error;
		if (error != 0) {
			dst->bn_inode = saved;
			goto out;
		}
		changed = 1;
		(void)uvm_vnp_uncache(dvp);
		uvm_vnp_setsize(dvp, dst->bn_inode.bi_size);
		VN_KNOTE(dvp, NOTE_ATTRIB | NOTE_EXTEND);
		error = btrfs_resize_finish(&resize);
		if (error != 0)
			goto out;
		error = btrfs_trans_commit(bmp,
		    dst->bn_inode.bi_last_dirty_transid, p);
		if (error != 0)
			goto out;
	}
	reservation.btr_metadata =
	    (uint64_t)letoh32(bmp->bm_super.nodesize) *
	    BTRFS_VOP_METADATA_BLOCKS;
	while (length != 0) {
		error = btrfs_find_file_extent(bmp, src->bn_root, &path,
		    src->bn_ino, from, roundup(src->bn_inode.bi_size, sector),
		    &source);
		btrfs_release_path(&path);
		if (error != 0)
			break;
		if (source.bfe_type == BTRFS_FILE_EXTENT_INLINE) {
			/*
			 * Inline bytes have no shareable data allocation.
			 * Read/copy only this bounded inline payload, using the
			 * ordinary writer for its checksum and EOF handling.
			 */
			count = MIN(length, sector);
			data = malloc(sector, M_BTRFS, M_WAITOK);
			error = btrfs_read_file_range(src, from, count, data);
			if (error != 0)
				break;
			iov.iov_base = data;
			iov.iov_len = count;
			memset(&uio, 0, sizeof(uio));
			uio.uio_iov = &iov;
			uio.uio_iovcnt = 1;
			uio.uio_offset = to;
			uio.uio_resid = count;
			uio.uio_segflg = UIO_SYSSPACE;
			uio.uio_rw = UIO_WRITE;
			uio.uio_procp = p;
			error = VOP_WRITE(dvp, &uio, 0, p->p_ucred);
			free(data, M_BTRFS, sector);
			data = NULL;
			if (error != 0)
				break;
			changed = 1;
			from += count;
			to += count;
			length -= count;
			continue;
		}
		error = btrfs_find_file_extent(bmp, dst->bn_root, &path,
		    dst->bn_ino, to, roundup(MAX(end,
		    dst->bn_inode.bi_size), sector), &old);
		btrfs_release_path(&path);
		if (error != 0)
			break;
		count = MIN(roundup(length, sector),
		    source.bfe_logical + source.bfe_length - from);
		if (old.bfe_type == BTRFS_FILE_EXTENT_INLINE) {
			if (to != 0 || old.bfe_length > count) {
				error = EOPNOTSUPP;
				break;
			}
		} else
			count = MIN(count,
			    old.bfe_logical + old.bfe_length - to);
		if (count == 0 || (count & (sector - 1)) != 0 ||
		    (source.bfe_logical & (sector - 1)) != 0 ||
		    (old.bfe_logical & (sector - 1)) != 0 ||
		    (old.bfe_type != BTRFS_FILE_EXTENT_INLINE &&
		    (old.bfe_length & (sector - 1)) != 0) ||
		    source.bfe_encryption != 0 ||
		    source.bfe_other_encoding != 0 ||
		    old.bfe_encryption != 0 || old.bfe_other_encoding != 0) {
			error = EOPNOTSUPP;
			break;
		}
		/* Preallocation reads as zero and need not be duplicated. */
		if (source.bfe_type == BTRFS_FILE_EXTENT_PREALLOC)
			source.bfe_type = BTRFS_FILE_EXTENT_HOLE;
		source.bfe_disk_offset += from - source.bfe_logical;
		source.bfe_logical = to;
		source.bfe_length = count;
		size = MAX(dst->bn_inode.bi_size, to + MIN(length, count));
		error = btrfs_trans_join(bmp, &reservation, &handle);
		if (error != 0)
			break;
		saved = dst->bn_inode;
		getnanotime(&now);
		dst->bn_inode.bi_mtime = dst->bn_inode.bi_ctime = now;
		dst->bn_inode.bi_dirty_fields |= BTRFS_INODE_DIRTY_MTIME |
		    BTRFS_INODE_DIRTY_CTIME;
		error = btrfs_clone_file_extent(handle, dst, &source, &old,
		    to, count, size);
		end_error = btrfs_trans_end(handle);
		if (error == 0)
			error = end_error;
		if (error != 0) {
			dst->bn_inode = saved;
			break;
		}
		changed = 1;
		count = MIN(length, count);
		from += count;
		to += count;
		length -= count;
	}
out:
	btrfs_release_path(&path);
	btrfs_resize_release(&resize);
	free(data, M_BTRFS, sector);
	if (changed) {
		(void)uvm_vnp_uncache(dvp);
		/* All file buffers are clean; pending writes live in ordered data. */
		(void)vinvalbuf(dvp, 0, p->p_ucred, p, 0, INFSLP);
		uvm_vnp_setsize(dvp, dst->bn_inode.bi_size);
		VN_KNOTE(dvp, NOTE_WRITE |
		    (dst->bn_inode.bi_size > original_size ? NOTE_EXTEND : 0));
		if (error == 0 &&
		    ((dvp->v_mount->mnt_flag & MNT_SYNCHRONOUS) ||
		    (dst->bn_inode.bi_flags & BTRFS_INODE_SYNC)))
			error = btrfs_trans_commit(bmp,
			    dst->bn_inode.bi_last_dirty_transid, p);
	}
	if (dvp != svp)
		VOP_UNLOCK(dvp);
	VOP_UNLOCK(svp);
	return (error);
}

static int
btrfs_fsync(void *v)
{
	struct vop_fsync_args *ap = v;
	struct btrfs_node *node = VTOBTRFS(ap->a_vp);
	struct btrfs_fs *bmp = node->bn_mount;
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
btrfs_spec_fsync(void *v)
{
	int error;

	error = spec_fsync(v);
	if (error != 0)
		return (error);
	return (btrfs_fsync(v));
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
	struct btrfs_fs *bmp = node->bn_mount;
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
	    extent.bfe_logical != 0 ||
	    extent.bfe_length != extent.bfe_inline_size ||
	    extent.bfe_inline_size < file_size ||
	    extent.bfe_inline_size - file_size > 1) {
		error = EINVAL;
		goto out;
	}
	/* mkfs.btrfs may store a trailing NUL beyond the inode's size. */
	if ((extent.bfe_inline_size != file_size &&
	    extent.bfe_inline_data[file_size] != '\0') ||
	    memchr(extent.bfe_inline_data, '\0', file_size) != NULL) {
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
	struct btrfs_node	*brc_node;
	struct uio	*brc_uio;
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

	/* Leave room for the next cookie in the signed VFS offset. */
	if (entry->bde_index >= INT64_MAX)
		return (EOVERFLOW);

	/*
	 * A subvolume directory item names a root item; its visible inode is
	 * the root directory in that tree, not the root item's object ID.
	 */
	if (entry->bde_subvolume) {
		error = btrfs_check_subvol_link(ctx->brc_node->bn_root,
		    ctx->brc_node->bn_ino, entry->bde_objectid,
		    (const char *)entry->bde_name, entry->bde_namelen);
		if (error != 0 && error != ENOENT)
			return (error);
		fileno = error == ENOENT ? 2 : BTRFS_FIRST_FREE_OBJECTID;
	} else
		fileno = entry->bde_objectid;
	error = btrfs_emit_dirent(ctx, fileno,
	    btrfs_dirent_type(entry->bde_type), entry->bde_name,
	    entry->bde_namelen, entry->bde_index + 1);
	return (error);
}

/*
 * Persistent directory indexes are seek cookies; resume with a tree search,
 * allowing gaps. An index whose next cookie cannot fit a signed VFS offset
 * returns EOVERFLOW. Removed directory descriptors report zero links and EOF;
 * child lookup and creation no longer succeed.
 */
static int
btrfs_readdir(void *v)
{
	struct vop_readdir_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct btrfs_node *node = VTOBTRFS(vp);
	struct btrfs_fs *bmp = node->bn_mount;
	struct btrfs_mount *view = VFSTOBTRFSVIEW(vp->v_mount);
	struct btrfs_readdir_ctx ctx;
	struct btrfs_root *root;
	struct uio *uio = ap->a_uio;
	uint64_t parent, parent_treeid;
	size_t resid;
	int error = 0;

	KASSERT(VOP_ISLOCKED(vp));
	if (uio->uio_rw != UIO_READ || uio->uio_offset < 0)
		return (EINVAL);
	if (vp->v_type != VDIR)
		return (ENOTDIR);
	if (node->bn_inode.bi_nlink == 0) {
		if (ap->a_eofflag != NULL)
			*ap->a_eofflag = 1;
		return (0);
	}
	memset(&ctx, 0, sizeof(ctx));
	ctx.brc_node = node;
	ctx.brc_uio = uio;
	ctx.brc_offset = uio->uio_offset;
	resid = uio->uio_resid;

	if (ctx.brc_offset == BTRFS_DIR_OFFSET_DOT) {
		error = btrfs_emit_dirent(&ctx, node->bn_ino, DT_DIR,
		    (const uint8_t *)".", 1, BTRFS_DIR_OFFSET_DOTDOT);
		if (error != 0)
			goto out;
	}
	if (ctx.brc_offset == BTRFS_DIR_OFFSET_DOTDOT) {
		parent_treeid = node->bn_treeid;
		if (node->bn_stub_parent != 0)
			parent = node->bn_stub_parent;
		else if (node->bn_ino == view->bmv_root_dirid &&
		    node->bn_treeid == view->bmv_treeid)
			parent = node->bn_ino;
		else if (node->bn_ino == BTRFS_FIRST_FREE_OBJECTID &&
		    node->bn_treeid != view->bmv_treeid)
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

	if (node->bn_stub_parent != 0)
		goto out;
	error = btrfs_get_root(bmp, node->bn_treeid, &root);
	if (error != 0)
		goto out;
	error = btrfs_iterate_directory(root, node->bn_ino,
	    ctx.brc_offset, btrfs_readdir_entry, &ctx);
out:
	if (error == BTRFS_READDIR_FULL)
		error = uio->uio_resid == resid ? EINVAL : 0;
	uio->uio_offset = ctx.brc_offset;
	if (ap->a_eofflag != NULL)
		*ap->a_eofflag = error == 0 && !ctx.brc_full;
	return (error);
}

static int
btrfs_inactive(void *v)
{
	struct vop_inactive_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct btrfs_node *node = VTOBTRFS(vp);
	int error = 0, deleted = node->bn_inode.bi_nlink == 0;

	if (deleted && !btrfs_node_readonly(node)) {
		/*
		 * Retire ordered writes before raw, restartable range deletion.
		 * The orphan remains durable if cleanup cannot reserve space.
		 */
		error = btrfs_commit_inode_data(node, ap->a_p);
		if (error == 0)
			error = btrfs_reap_inode(node->bn_root, node->bn_ino);
	}
	VOP_UNLOCK(vp);
	if (deleted)
		vrecycle(vp, ap->a_p);
	return (error);
}

static int
btrfs_reclaim(void *v)
{
	struct vop_reclaim_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct btrfs_node *node = VTOBTRFS(vp);
	struct btrfs_fs *bmp = node->bn_mount;

	if (node->bn_hashed) {
		mtx_enter(&bmp->bm_nodemtx);
		RBT_REMOVE(btrfs_node_tree, &bmp->bm_node_tree, node);
		LIST_REMOVE(node, bn_entry);
		node->bn_hashed = 0;
		mtx_leave(&bmp->bm_nodemtx);
	}
	cache_purge(vp);
	lf_purgelocks(&node->bn_lockf);
	free(node, M_BTRFS, sizeof(*node));
	vp->v_data = NULL;
	return (0);
}

#ifdef FIFO
static int
btrfs_fifo_reclaim(void *v)
{
	fifo_reclaim(v);
	return (btrfs_reclaim(v));
}
#endif

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
btrfs_kqfilter(void *v)
{
	static const struct filterops read_filtops = {
		.f_flags = FILTEROP_ISFD,
		.f_detach = filt_btrfsdetach,
		.f_event = filt_btrfsread,
	};
	static const struct filterops write_filtops = {
		.f_flags = FILTEROP_ISFD,
		.f_detach = filt_btrfsdetach,
		.f_event = filt_btrfswrite,
	};
	static const struct filterops vnode_filtops = {
		.f_flags = FILTEROP_ISFD,
		.f_detach = filt_btrfsdetach,
		.f_event = filt_btrfsvnode,
	};
	struct vop_kqfilter_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct knote *kn = ap->a_kn;

	switch (kn->kn_filter) {
	case EVFILT_READ:
		kn->kn_fop = &read_filtops;
		break;
	case EVFILT_WRITE:
		kn->kn_fop = &write_filtops;
		break;
	case EVFILT_VNODE:
		kn->kn_fop = &vnode_filtops;
		break;
	default:
		return (EINVAL);
	}
	kn->kn_hook = (caddr_t)vp;
	klist_insert_locked(&vp->v_klist, kn);
	return (0);
}

static void
filt_btrfsdetach(struct knote *kn)
{
	struct vnode *vp = (struct vnode *)kn->kn_hook;

	klist_remove_locked(&vp->v_klist, kn);
}

static int
filt_btrfsread(struct knote *kn, long hint)
{
	struct vnode *vp = (struct vnode *)kn->kn_hook;

	/* Reclaim has already freed the inode when VFS sends NOTE_REVOKE. */
	if (hint == NOTE_REVOKE) {
		kn->kn_flags |= EV_EOF | EV_ONESHOT;
		return (1);
	}
	kn->kn_data = VTOBTRFS(vp)->bn_inode.bi_size - foffset(kn->kn_fp);
	if (kn->kn_data == 0 && (kn->kn_sfflags & NOTE_EOF)) {
		kn->kn_fflags |= NOTE_EOF;
		return (1);
	}
	if (kn->kn_flags & (__EV_POLL | __EV_SELECT))
		return (1);
	return (kn->kn_data != 0);
}

static int
filt_btrfswrite(struct knote *kn, long hint)
{
	if (hint == NOTE_REVOKE) {
		kn->kn_flags |= EV_EOF | EV_ONESHOT;
		return (1);
	}
	kn->kn_data = 0;
	return (1);
}

static int
filt_btrfsvnode(struct knote *kn, long hint)
{
	kn->kn_fflags |= kn->kn_sfflags & hint;
	if (hint == NOTE_REVOKE) {
		kn->kn_flags |= EV_EOF;
		return (1);
	}
	return (kn->kn_fflags != 0);
}

static int
btrfs_advlock(void *v)
{
	struct vop_advlock_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct btrfs_node *node;
	int reclaiming;

	/*
	 * vclean sets VXLOCK before draining the vnode. Advisory locking
	 * bypasses vn_lock, so reject new requests during that drain too.
	 * In particular, an interrupted waiter may retry while reclaim is
	 * still purging locks and has not installed dead_vops yet.
	 */
	mtx_enter(&vnode_mtx);
	reclaiming = (vp->v_lflag & VXLOCK) != 0;
	mtx_leave(&vnode_mtx);
	if (reclaiming)
		return (EBADF);
	node = VTOBTRFS(vp);

	/* The shared lock engine serializes state and sleeps without bn_lock. */
	return (lf_advlock(&node->bn_lockf, node->bn_inode.bi_size,
	    ap->a_id, ap->a_op, ap->a_fl, ap->a_flags));
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
	case _PC_ALLOC_SIZE_MIN:
		*ap->a_retval = ap->a_vp->v_mount->mnt_stat.f_bsize;
		break;
	case _PC_FILESIZEBITS:
		*ap->a_retval = 64;
		break;
	case _PC_REC_INCR_XFER_SIZE:
	case _PC_REC_MIN_XFER_SIZE:
		*ap->a_retval = ap->a_vp->v_mount->mnt_stat.f_iosize;
		break;
	case _PC_REC_MAX_XFER_SIZE:
		*ap->a_retval = -1;
		break;
	case _PC_REC_XFER_ALIGN:
		*ap->a_retval = PAGE_SIZE;
		break;
	case _PC_SYMLINK_MAX:
		*ap->a_retval = MAXPATHLEN - 1;
		break;
	case _PC_2_SYMLINKS:
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
