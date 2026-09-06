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
	struct file *fp;
	struct vnode *vp;
	struct mount *mp;
	int error;

	if ((error = suser(p)) != 0)
		return (error);
	if (cmd != BTRFSIOC_LIST && cmd != BTRFSIOC_CREATE &&
	    cmd != BTRFSIOC_DELETE && cmd != BTRFSIOC_SNAPSHOT)
		return (ENOTTY);
	if (cmd != BTRFSIOC_LIST && !(flags & FWRITE))
		return (EBADF);
	if (memchr(args->path, '\0', sizeof(args->path)) == NULL ||
	    memchr(args->source, '\0', sizeof(args->source)) == NULL)
		return (ENAMETOOLONG);
	if ((args->flags & ~(cmd == BTRFSIOC_SNAPSHOT ?
	    BTRFS_CTL_RDONLY : 0)) != 0 || args->id != 0 ||
	    args->parent != 0 || (cmd != BTRFSIOC_LIST && args->cursor != 0))
		return (EINVAL);
	fp = fd_getfile(p->p_fd, args->fd);
	if (fp == NULL)
		return (EBADF);
	if (fp->f_type != DTYPE_VNODE) {
		FRELE(fp, p);
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
			error = btrfs_control(mp, cmd, args, p);
			vfs_unbusy(mp);
		}
	}
	FRELE(fp, p);
	return (error);
}
