// SPDX-License-Identifier: CDDL-1.0

/* Native OpenBSD /dev/zfs transport and ioctl hooks. */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/atomic.h>
#include <sys/conf.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/ioccom.h>
#include <sys/kmem.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/specdev.h>
#include <sys/vnode.h>

#include <uvm/uvm_extern.h>

#include <sys/zfs_context.h>
#include <sys/zfs_ctldir.h>
#include <sys/zfs_file.h>
#include <sys/zfs_ioctl.h>
#include <sys/zfs_ioctl_compat.h>
#include <sys/zfs_vfsops.h>
#include <sys/zfs_ioctl_impl.h>

#define	ZFSDEV_NCLONES	(CLONE_MAPSZ * NBBY)
#define	ZFSDEV_MINOR_INDEX(m)	((m) >> CLONE_SHIFT)

_Static_assert(ECKSUM == ZFS_ERR_CKSUM,
    "OpenBSD ECKSUM must match the ZFS ioctl ABI");
_Static_assert(ENOTACTIVE == ZFS_ERR_NOTACTIVE,
    "OpenBSD ENOTACTIVE must match the ZFS ioctl ABI");
_Static_assert(EREMOTEIO == ZFS_ERR_ACTIVE_POOL,
    "OpenBSD EREMOTEIO must match the ZFS ioctl ABI");

/* Indexed by the native D_CLONE instance encoded in the device minor. */
static zfsdev_state_t *zfsdev_states[ZFSDEV_NCLONES];

/*
 * Keep ZFS-private failures in the stable ioctl error namespace.  Having the
 * conversion at the cdev boundary also makes it explicit that these values
 * are not additions to OpenBSD's native errno namespace.
 */
static int
zfs_ioctl_error_to_user(int error)
{
	switch (error) {
	case ECKSUM:
		return (ZFS_ERR_CKSUM);
	case ENOTACTIVE:
		return (ZFS_ERR_NOTACTIVE);
	case EREMOTEIO:
		return (ZFS_ERR_ACTIVE_POOL);
	default:
		return (error);
	}
}

int
zfs_vfs_ref(zfsvfs_t **zfvp)
{
	zfsvfs_t *zfsvfs = *zfvp;

	if (zfsvfs == NULL || zfsvfs->z_vfs == NULL)
		return (SET_ERROR(ESRCH));
	if (vfs_busy(zfsvfs->z_vfs, VB_READ | VB_NOWAIT) != 0) {
		*zfvp = NULL;
		return (SET_ERROR(ESRCH));
	}
	return (0);
}

boolean_t
zfs_vfs_held(zfsvfs_t *zfsvfs)
{
	return (zfsvfs != NULL && zfsvfs->z_vfs != NULL);
}

void
zfs_vfs_rele(zfsvfs_t *zfsvfs)
{
	vfs_unbusy(zfsvfs->z_vfs);
}

void
zfs_ioctl_update_mount_cache(const char *dsname)
{
	zfsvfs_t *zfsvfs;

	if (getzfsvfs(dsname, &zfsvfs) == 0) {
		struct mount *mp = zfsvfs->z_vfs;

		(void) VFS_STATFS(mp, &mp->mnt_stat, curproc);
		zfs_vfs_rele(zfsvfs);
	}
}

uint64_t
zfs_max_nvlist_src_size_os(void)
{
	uint64_t size;

	if (zfs_max_nvlist_src_size != 0)
		return (zfs_max_nvlist_src_size);

	size = ptoa((uint64_t)uvmexp.npages) / 4;
	return (MIN(size, 128ULL * 1024 * 1024));
}

void
zfs_ioctl_init_os(void)
{
	/* OpenBSD has no platform-specific ZFS ioctl commands. */
}

int
zfsdev_attach(void)
{
	/* /dev/zfs is installed statically in the machine cdevsw. */
	return (0);
}

void
zfsdev_detach(void)
{
}

void
zfsdev_private_set_state(void *priv, zfsdev_state_t *zs)
{
	u_int idx = ZFSDEV_MINOR_INDEX((uintptr_t)priv);

	KERNEL_ASSERT_LOCKED();
	KASSERT(idx > 0 && idx < nitems(zfsdev_states));
	KASSERT(zfsdev_states[idx] == NULL);
	zfsdev_states[idx] = zs;
}

zfsdev_state_t *
zfsdev_private_get_state(void *priv)
{
	u_int idx = ZFSDEV_MINOR_INDEX((uintptr_t)priv);

	KERNEL_ASSERT_LOCKED();
	if (idx == 0 || idx >= nitems(zfsdev_states))
		return (NULL);
	return (zfsdev_states[idx]);
}

int
zfsopen(dev_t dev, int flags, int mode, struct proc *p)
{
	u_int idx = ZFSDEV_MINOR_INDEX(minor(dev));
	int error;

	(void)flags;
	(void)mode;
	(void)p;
	KERNEL_ASSERT_LOCKED();
	if ((minor(dev) & ((1U << CLONE_SHIFT) - 1)) != 0 ||
	    idx == 0 || idx >= nitems(zfsdev_states))
		return (ENXIO);

	mutex_enter(&zfsdev_state_lock);
	if (zfsdev_states[idx] != NULL)
		error = EBUSY;
	else
		error = zfsdev_state_init((void *)(uintptr_t)minor(dev));
	mutex_exit(&zfsdev_state_lock);
	return (error);
}

int
zfsclose(dev_t dev, int flags, int mode, struct proc *p)
{
	u_int idx = ZFSDEV_MINOR_INDEX(minor(dev));
	void *priv = (void *)(uintptr_t)minor(dev);

	(void)flags;
	(void)mode;
	(void)p;
	KERNEL_ASSERT_LOCKED();
	if (idx == 0 || idx >= nitems(zfsdev_states) ||
	    zfsdev_states[idx] == NULL)
		return (ENXIO);

	zfsdev_state_destroy(priv);
	zfsdev_states[idx] = NULL;
	return (0);
}

int
zfsioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	zfs_iocparm_t *zp = (zfs_iocparm_t *)data;
	zfs_cmd_t *zc;
	uint_t vecnum;
	int error, rc;

	(void)dev;
	(void)p;
	KERNEL_ASSERT_LOCKED();
	if (IOCGROUP(cmd) != 'Z' || IOCPARM_LEN(cmd) != sizeof (*zp))
		return (ENOTTY);
	if (zp->zfs_ioctl_version != ZFS_IOCVER_OZFS ||
	    zp->zfs_cmd_size != sizeof (*zc))
		return (EINVAL);

	vecnum = ZFS_IOCREQ(cmd);
	zc = kmem_zalloc(sizeof (*zc), KM_SLEEP);
	error = copyin((void *)(uintptr_t)zp->zfs_cmd, zc, sizeof (*zc));
	if (error != 0)
		goto out;

	error = zfsdev_ioctl_common(vecnum, zc, flag);
	error = zfs_ioctl_error_to_user(error);
	rc = copyout(zc, (void *)(uintptr_t)zp->zfs_cmd, sizeof (*zc));
	if (error == 0 && rc != 0)
		error = EFAULT;
out:
	kmem_free(zc, sizeof (*zc));
	KASSERT(tsd_get(rrw_tsd_key) == NULL);
	return (error);
}

zfs_file_t *
zfs_file_get(int fd)
{
	return (fd_getfile(curproc->p_fd, fd));
}

void
zfs_file_put(zfs_file_t *fp)
{
	(void) FRELE(fp, curproc);
}

void *
zfs_file_private(zfs_file_t *fp)
{
	struct vnode *vp;
	dev_t dev;

	KERNEL_ASSERT_LOCKED();
	if (fp == NULL || fp->f_type != DTYPE_VNODE)
		return (NULL);
	vp = fp->f_data;
	if (vp == NULL || vp->v_type != VCHR)
		return (NULL);
	dev = vp->v_rdev;
	if ((u_int)major(dev) >= nchrdev ||
	    cdevsw[major(dev)].d_open != zfsopen)
		return (NULL);
	return (zfsdev_private_get_state(
	    (void *)(uintptr_t)minor(dev)));
}

int
zfsctl_snapshot_unmount(const char *snapname, int flags)
{
	(void)snapname;
	(void)flags;
	return (EOPNOTSUPP);
}
