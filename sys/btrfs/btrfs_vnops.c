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
static int	btrfs_ioctl(void *);
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
	.vop_read	= eopnotsupp,
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
	.vop_readdir	= eopnotsupp,
	.vop_readlink	= eopnotsupp,
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

static int
btrfs_lookup(void *v)
{
	struct vop_lookup_args *ap = v;
	struct componentname *cnp = ap->a_cnp;

	*ap->a_vpp = NULL;
	if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') {
		vref(ap->a_dvp);
		*ap->a_vpp = ap->a_dvp;
		return (0);
	}
	return (EOPNOTSUPP);
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

static int
btrfs_ioctl(void *v)
{
	return (ENOTTY);
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
