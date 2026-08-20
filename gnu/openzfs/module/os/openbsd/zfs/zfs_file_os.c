// SPDX-License-Identifier: CDDL-1.0

/* OpenZFS kernel-file operations implemented on OpenBSD vnodes. */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/lock.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <sys/zfs_file.h>

int
zfs_file_open(const char *path, int oflags, int mode, zfs_file_t **fpp)
{
	struct nameidata nd;
	struct file *fp;
	struct vnode *vp;
	int error, flags;

	*fpp = NULL;
	flags = FFLAGS(oflags);
	if ((flags & (FREAD | FWRITE)) == 0)
		return (EINVAL);

	fp = fnew(curproc);
	if (fp == NULL)
		return (ENFILE);

	NDINIT(&nd, 0, KERNELPATH, UIO_SYSSPACE, path, curproc);
	KERNEL_LOCK();
	error = vn_open(&nd, flags, mode);
	if (error != 0) {
		KERNEL_UNLOCK();
		FRELE(fp, curproc);
		return (error);
	}

	vp = nd.ni_vp;
	fp->f_flag = flags & FMASK;
	fp->f_type = DTYPE_VNODE;
	fp->f_ops = &vnops;
	fp->f_data = vp;
	VOP_UNLOCK(vp);
	KERNEL_UNLOCK();

	if (vp->v_type != VREG) {
		FRELE(fp, curproc);
		return (EACCES);
	}

	*fpp = fp;
	return (0);
}

void
zfs_file_close(zfs_file_t *fp)
{
	FRELE(fp, curproc);
}

static int
zfs_file_write_impl(zfs_file_t *fp, const void *buf, size_t len,
    loff_t off, int ioflag, ssize_t *resid)
{
	struct iovec iov;
	struct uio uio;
	int error;

	if ((fp->f_flag & FWRITE) == 0)
		return (EBADF);

	iov.iov_base = (void *)(uintptr_t)buf;
	iov.iov_len = len;
	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_offset = off;
	uio.uio_resid = len;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_rw = UIO_WRITE;
	uio.uio_procp = curproc;

	error = fp->f_ops->fo_write(fp, &uio, ioflag);
	if (resid != NULL)
		*resid = uio.uio_resid;
	else if (error == 0 && uio.uio_resid != 0)
		error = EIO;
	return (error);
}

int
zfs_file_write(zfs_file_t *fp, const void *buf, size_t len, ssize_t *resid)
{
	return (zfs_file_write_impl(fp, buf, len, 0, 0, resid));
}

int
zfs_file_pwrite(zfs_file_t *fp, const void *buf, size_t len, loff_t off,
    uint8_t ashift, ssize_t *resid)
{
	(void)ashift;
	return (zfs_file_write_impl(fp, buf, len, off, FO_POSITION, resid));
}

static int
zfs_file_read_impl(zfs_file_t *fp, void *buf, size_t len, loff_t off,
    int ioflag, ssize_t *resid)
{
	struct iovec iov;
	struct uio uio;
	int error;

	if ((fp->f_flag & FREAD) == 0)
		return (EBADF);

	iov.iov_base = buf;
	iov.iov_len = len;
	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_offset = off;
	uio.uio_resid = len;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_rw = UIO_READ;
	uio.uio_procp = curproc;

	error = fp->f_ops->fo_read(fp, &uio, ioflag);
	if (resid != NULL)
		*resid = uio.uio_resid;
	return (error);
}

int
zfs_file_read(zfs_file_t *fp, void *buf, size_t len, ssize_t *resid)
{
	return (zfs_file_read_impl(fp, buf, len, 0, 0, resid));
}

int
zfs_file_pread(zfs_file_t *fp, void *buf, size_t len, loff_t off,
    ssize_t *resid)
{
	return (zfs_file_read_impl(fp, buf, len, off, FO_POSITION, resid));
}

int
zfs_file_seek(zfs_file_t *fp, loff_t *offp, int whence)
{
	off_t off = *offp;
	int error;

	error = fp->f_ops->fo_seek(fp, &off, whence, curproc);
	if (error == 0)
		*offp = off;
	return (error);
}

int
zfs_file_getattr(zfs_file_t *fp, zfs_file_attr_t *zfattr)
{
	struct stat st;
	int error;

	error = fp->f_ops->fo_stat(fp, &st, curproc);
	if (error == 0) {
		zfattr->zfa_size = st.st_size;
		zfattr->zfa_mode = st.st_mode;
	}
	return (error);
}

int
zfs_file_fsync(zfs_file_t *fp, int flags)
{
	struct vnode *vp = fp->f_data;
	int error;

	(void)flags;
	KERNEL_LOCK();
	vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
	error = VOP_FSYNC(vp, fp->f_cred, MNT_WAIT, curproc);
	VOP_UNLOCK(vp);
	KERNEL_UNLOCK();
	return (error);
}

int
zfs_file_deallocate(zfs_file_t *fp, loff_t offset, loff_t len)
{
	(void)fp;
	(void)offset;
	(void)len;
	return (EOPNOTSUPP);
}

loff_t
zfs_file_off(zfs_file_t *fp)
{
	return (foffset(fp));
}

int
zfs_file_unlink(const char *path)
{
	(void)path;
	return (EOPNOTSUPP);
}
