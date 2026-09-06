/* Public domain. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/btrfsio.h>
#include <btrfs/btrfs_var.h>

static int
btrfs_clone_ioctl(struct btrfs_ioctl_clone *args, struct proc *p)
{
	struct file *src, *dst;
	struct vnode *svp, *dvp;
	struct mount *smp, *dmp;
	int error;

	src = fd_getfile(p->p_fd, args->src_fd);
	if (src == NULL)
		return (EBADF);
	dst = fd_getfile(p->p_fd, args->dst_fd);
	if (dst == NULL) {
		FRELE(src, p);
		return (EBADF);
	}
	error = EBADF;
	if (!(src->f_flag & FREAD) || !(dst->f_flag & FWRITE))
		goto out;
	error = EINVAL;
	if (src->f_type != DTYPE_VNODE || dst->f_type != DTYPE_VNODE)
		goto out;
	svp = src->f_data;
	dvp = dst->f_data;
	smp = svp->v_mount;
	dmp = dvp->v_mount;
	if (svp->v_type != VREG || dvp->v_type != VREG)
		goto out;
	error = EXDEV;
	if (smp == NULL || dmp == NULL ||
	    strcmp(smp->mnt_vfc->vfc_name, "btrfs") != 0 ||
	    strcmp(dmp->mnt_vfc->vfc_name, "btrfs") != 0)
		goto out;
	error = vfs_busy(smp, VB_READ | VB_NOWAIT);
	if (error != 0)
		goto out;
	if (dmp != smp) {
		error = vfs_busy(dmp, VB_READ | VB_NOWAIT);
		if (error != 0) {
			vfs_unbusy(smp);
			goto out;
		}
	}
	if (VTOBTRFS(svp)->bn_mount != VTOBTRFS(dvp)->bn_mount)
		error = EXDEV;
	else
		error = btrfs_clone_range(svp, dvp, args, p);
	if (dmp != smp)
		vfs_unbusy(dmp);
	vfs_unbusy(smp);
out:
	FRELE(dst, p);
	FRELE(src, p);
	return (error);
}

int
btrfsopen(dev_t dev, int flags, int mode, struct proc *p)
{
	if (minor(dev) != 0)
		return (ENXIO);
	return (suser(p));
}

int
btrfsclose(dev_t dev, int flags, int mode, struct proc *p)
{
	return (0);
}

int
btrfsioctl(dev_t dev, u_long cmd, caddr_t data, int flags, struct proc *p)
{
	struct btrfs_ioctl_subvolume *args = (void *)data;
	struct btrfs_ioctl_identity *identity = (void *)data;
	struct btrfs_ioctl_xattr *xattr = (void *)data;
	struct btrfs_ioctl_tree *tree = (void *)data;
	struct file *fp, *parent = NULL;
	struct vnode *vp, *pvp = NULL;
	struct mount *mp;
	int error, isidentity, isxattr, readonly, fd;

	if ((error = suser(p)) != 0)
		return (error);
	if (cmd == BTRFSIOC_CLONE) {
		if (!(flags & FWRITE))
			return (EBADF);
		return (btrfs_clone_ioctl((void *)data, p));
	}
	isidentity = cmd == BTRFSIOC_INFO || cmd == BTRFSIOC_FINISH;
	isxattr = cmd == BTRFSIOC_GETXATTR || cmd == BTRFSIOC_SETXATTR ||
	    cmd == BTRFSIOC_RMXATTR;
	readonly = cmd == BTRFSIOC_LIST || cmd == BTRFSIOC_INFO ||
	    cmd == BTRFSIOC_GETXATTR || cmd == BTRFSIOC_TREE;
	if (!isidentity && !isxattr && cmd != BTRFSIOC_TREE &&
	    cmd != BTRFSIOC_LIST && cmd != BTRFSIOC_CREATE &&
	    cmd != BTRFSIOC_DELETE && cmd != BTRFSIOC_SNAPSHOT)
		return (ENOTTY);
	if (!readonly && !(flags & FWRITE))
		return (EBADF);
	if (cmd == BTRFSIOC_TREE) {
		fd = tree->fd;
		if (tree->parent_fd != -1) {
			parent = fd_getfile(p->p_fd, tree->parent_fd);
			if (parent == NULL)
				return (EBADF);
			if (parent->f_type != DTYPE_VNODE) {
				FRELE(parent, p);
				return (EINVAL);
			}
			pvp = parent->f_data;
		}
	} else if (isxattr) {
		if (memchr(xattr->name, '\0', sizeof(xattr->name)) == NULL)
			return (ENAMETOOLONG);
		fd = xattr->fd;
	} else if (isidentity) {
		if (memchr(identity->path, '\0', sizeof(identity->path)) == NULL)
			return (ENAMETOOLONG);
		if (identity->flags != 0 ||
		    (identity->id != 0 && identity->path[0] != '\0'))
			return (EINVAL);
		fd = identity->fd;
	} else {
		if (memchr(args->path, '\0', sizeof(args->path)) == NULL ||
		    memchr(args->source, '\0', sizeof(args->source)) == NULL)
			return (ENAMETOOLONG);
		if ((args->flags & ~(cmd == BTRFSIOC_SNAPSHOT ?
		    BTRFS_CTL_RDONLY : 0)) != 0 || args->id != 0 ||
		    args->parent != 0 ||
		    (cmd != BTRFSIOC_LIST && args->cursor != 0))
			return (EINVAL);
		fd = args->fd;
	}
	fp = fd_getfile(p->p_fd, fd);
	if (fp == NULL) {
		if (parent != NULL)
			FRELE(parent, p);
		return (EBADF);
	}
	if (fp->f_type != DTYPE_VNODE) {
		FRELE(fp, p);
		if (parent != NULL)
			FRELE(parent, p);
		return (EINVAL);
	}
	vp = fp->f_data;
	mp = vp->v_mount;
	if (vp->v_type != VDIR || mp == NULL ||
	    strcmp(mp->mnt_vfc->vfc_name, "btrfs") != 0)
		error = EINVAL;
	else {
		error = vfs_busy(mp, VB_READ | VB_NOWAIT);
		if (error == 0) {
			if (cmd == BTRFSIOC_TREE)
				error = btrfs_tree_control(vp, pvp, tree);
			else if (isxattr)
				error = btrfs_control_xattr(vp, cmd, xattr, p);
			else if (isidentity) {
				error = btrfs_identity_control(mp, cmd, identity, p);
				if (error == 0 && cmd == BTRFSIOC_INFO)
					identity->fd_treeid = VTOBTRFS(vp)->bn_treeid;
			} else
				error = btrfs_control(mp, cmd, args, p);
			vfs_unbusy(mp);
		}
	}
	FRELE(fp, p);
	if (parent != NULL)
		FRELE(parent, p);
	return (error);
}
