// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 * You can obtain a copy of the License at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 *
 * CDDL HEADER END
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/vdev_impl.h>
#include <sys/fs/zfs.h>
#include <sys/zio.h>

#include <sys/buf.h>
#include <sys/disklabel.h>
#include <sys/dkio.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/namei.h>
#include <sys/specdev.h>
#include <sys/taskq.h>
#include <sys/vnode.h>

typedef struct vdev_disk {
	struct vnode	*vd_vp;
	cred_t		*vd_cred;
	int		vd_open_flags;
	dev_t		vd_dev;
} vdev_disk_t;

#define	VDEV_OPENBSD_MIN_PHYSICAL_ASHIFT	12

typedef struct vdev_disk_io vdev_disk_io_t;

typedef struct vdev_disk_buf {
	struct buf	 vdb_buf;
	vdev_disk_io_t	*vdb_io;
} vdev_disk_buf_t;

struct vdev_disk_io {
	zio_t		*vdi_zio;
	void		*vdi_data;
	size_t		 vdi_alloc_size;
	size_t		 vdi_nbufs;
	volatile uint32_t vdi_pending;
	volatile uint32_t vdi_error;
	vdev_disk_buf_t vdi_bufs[];
};

static int
vdev_disk_open_flags(spa_mode_t mode)
{
	int flags = 0;

	if (mode == SPA_MODE_UNINIT ||
	    (mode & ~(SPA_MODE_READ | SPA_MODE_WRITE)) != 0)
		return (0);
	if (mode & SPA_MODE_READ)
		flags |= FREAD;
	if (mode & SPA_MODE_WRITE)
		flags |= FWRITE;

	return (flags);
}

static int
vdev_disk_get_geometry(vdev_t *vd, vdev_disk_t *dvd, uint64_t *psize,
	uint64_t *max_psize, uint64_t *logical_ashift,
	uint64_t *physical_ashift)
{
	struct partinfo partinfo;
	uint64_t sectors;
	uint32_t secsize;
	int error;

	KERNEL_ASSERT_LOCKED();
	error = VOP_IOCTL(dvd->vd_vp, DIOCGPART, (caddr_t)&partinfo, FREAD,
	    NOCRED, curproc);
	if (error != 0)
		return (error);

	if (partinfo.disklab == NULL || partinfo.part == NULL)
		return (EINVAL);

	secsize = partinfo.disklab->d_secsize;
	if (secsize < SPA_MINBLOCKSIZE ||
	    (secsize & (secsize - 1)) != 0) {
		return (EINVAL);
	}

	sectors = DL_GETPSIZE(partinfo.part);
	if (sectors == 0 || sectors > UINT64_MAX / secsize)
		return (EOVERFLOW);

	*psize = sectors * secsize;
	*max_psize = *psize;
	*logical_ashift = highbit64(secsize) - 1;
	*physical_ashift = MAX(*logical_ashift,
	    VDEV_OPENBSD_MIN_PHYSICAL_ASHIFT);

	/* Retry cache flushes after every successful open. */
	vd->vdev_nowritecache = B_FALSE;
	/*
	 * DIOCDISCARD has no capability-query counterpart.  It is safe to
	 * advertise ordinary TRIM and let an unsupported device reject the
	 * request, as is done for file vdevs.
	 */
	vd->vdev_has_trim = B_TRUE;
	vd->vdev_has_securetrim = B_FALSE;

	return (0);
}

static int
vdev_disk_open_locked(vdev_t *vd, uint64_t *psize, uint64_t *max_psize,
	uint64_t *logical_ashift, uint64_t *physical_ashift)
{
	struct nameidata nd;
	vdev_disk_t *dvd;
	cred_t *cred;
	int error, flags;

	KERNEL_ASSERT_LOCKED();
	if (vd->vdev_path == NULL || vd->vdev_path[0] != '/') {
		vd->vdev_stat.vs_aux = VDEV_AUX_BAD_LABEL;
		return (SET_ERROR(EINVAL));
	}

	/* A reopen retains the vnode and refreshes its current geometry. */
	if (vd->vdev_tsd != NULL) {
		ASSERT(vd->vdev_reopening);
		dvd = vd->vdev_tsd;
		return (vdev_disk_get_geometry(vd, dvd, psize, max_psize,
		    logical_ashift, physical_ashift));
	}

	flags = vdev_disk_open_flags(spa_mode(vd->vdev_spa));
	if (flags == 0) {
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
		return (SET_ERROR(EINVAL));
	}

	NDINIT(&nd, LOOKUP, KERNELPATH, UIO_SYSSPACE, vd->vdev_path,
	    curproc);
	error = vn_open(&nd, flags, 0);
	if (error != 0) {
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
		return (SET_ERROR(error));
	}

	cred = crhold(curproc->p_ucred);
	VOP_UNLOCK(nd.ni_vp);
	if (nd.ni_vp->v_type != VBLK) {
		(void) vn_close(nd.ni_vp, flags, cred, curproc);
		crfree(cred);
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
		return (SET_ERROR(ENOTBLK));
	}

	dvd = kmem_zalloc(sizeof (*dvd), KM_SLEEP);
	dvd->vd_vp = nd.ni_vp;
	dvd->vd_cred = cred;
	dvd->vd_open_flags = flags;
	dvd->vd_dev = nd.ni_vp->v_rdev;
	vd->vdev_tsd = dvd;

	error = vdev_disk_get_geometry(vd, dvd, psize, max_psize,
	    logical_ashift, physical_ashift);
	if (error != 0)
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;

	return (SET_ERROR(error));
}

static void
vdev_disk_close_locked(vdev_t *vd)
{
	vdev_disk_t *dvd = vd->vdev_tsd;

	KERNEL_ASSERT_LOCKED();
	if (vd->vdev_reopening || dvd == NULL)
		return;

	vd->vdev_tsd = NULL;
	if (dvd->vd_vp != NULL)
		(void) vn_close(dvd->vd_vp, dvd->vd_open_flags, dvd->vd_cred,
		    curproc);
	crfree(dvd->vd_cred);
	kmem_free(dvd, sizeof (*dvd));
	vd->vdev_delayed_close = B_FALSE;
}

static int
vdev_disk_open(vdev_t *vd, uint64_t *psize, uint64_t *max_psize,
	uint64_t *logical_ashift, uint64_t *physical_ashift)
{
	int error;

	KERNEL_LOCK();
	error = vdev_disk_open_locked(vd, psize, max_psize, logical_ashift,
	    physical_ashift);
	KERNEL_UNLOCK();
	return (error);
}

static void
vdev_disk_close(vdev_t *vd)
{

	KERNEL_LOCK();
	vdev_disk_close_locked(vd);
	KERNEL_UNLOCK();
}

static void
vdev_disk_io_complete(vdev_disk_io_t *dio)
{
	if (atomic_dec_32_nv(&dio->vdi_pending) != 0)
		return;

	dio->vdi_zio->io_error = atomic_load_32(&dio->vdi_error);
	zio_delay_interrupt(dio->vdi_zio);
}

static void
vdev_disk_io_intr(struct buf *bp)
{
	vdev_disk_buf_t *vdb = (vdev_disk_buf_t *)bp;
	vdev_disk_io_t *dio = vdb->vdb_io;
	uint32_t error = 0;

	if (ISSET(bp->b_flags, B_ERROR))
		error = bp->b_error != 0 ? bp->b_error : EIO;
	else if (bp->b_resid != 0)
		error = EIO;

	if (error != 0)
		(void) atomic_cas_32(&dio->vdi_error, 0, error);
	vdev_disk_io_complete(dio);
}

static void
vdev_disk_io_submit(void *arg)
{
	vdev_disk_io_t *dio = arg;
	int s;

	for (size_t i = 0; i < dio->vdi_nbufs; i++) {
		struct buf *bp = &dio->vdi_bufs[i].vdb_buf;
		int error;

		if (!ISSET(bp->b_flags, B_READ)) {
			s = splbio();
			bp->b_vp->v_numoutput++;
			splx(s);
		}

		/*
		 * Physical disks queue strategy requests, but vnd(4) performs
		 * synchronous vnode I/O on its backing file.  Submit from a task
		 * worker so that acquiring the backing vnode cannot happen while
		 * the ZIO caller holds an arbitrary OpenZFS lock.
		 */
		KERNEL_LOCK();
		error = VOP_STRATEGY(bp->b_vp, bp);
		KERNEL_UNLOCK();
		if (error != 0) {
			bp->b_error = error;
			SET(bp->b_flags, B_ERROR);
			s = splbio();
			biodone(bp);
			splx(s);
		}
	}

	/* Drop the submission reference after every child is in flight. */
	vdev_disk_io_complete(dio);
}

static int
vdev_disk_io_rw(zio_t *zio, vdev_disk_t *dvd)
{
	vdev_disk_io_t *dio;
	uint64_t offset, remaining;
	size_t alloc_size, nbufs;
	char *data;
	int rw;

	if (zio->io_size == 0 ||
	    !IS_P2ALIGNED(zio->io_offset, DEV_BSIZE) ||
	    !IS_P2ALIGNED(zio->io_size, DEV_BSIZE))
		return (EINVAL);
	if (zio->io_size > SIZE_MAX ||
	    zio->io_offset > UINT64_MAX - zio->io_size)
		return (EOVERFLOW);

	nbufs = howmany(zio->io_size, MAXPHYS);
	if (nbufs >= UINT32_MAX ||
	    nbufs > (SIZE_MAX - sizeof (*dio)) / sizeof (dio->vdi_bufs[0]))
		return (EOVERFLOW);
	alloc_size = sizeof (*dio) + nbufs * sizeof (dio->vdi_bufs[0]);
	dio = kmem_zalloc(alloc_size, KM_SLEEP);
	dio->vdi_zio = zio;
	dio->vdi_alloc_size = alloc_size;
	dio->vdi_nbufs = nbufs;
	dio->vdi_pending = nbufs + 1;

	if (zio->io_type == ZIO_TYPE_READ)
		data = abd_borrow_buf(zio->io_abd, zio->io_size);
	else
		data = abd_borrow_buf_copy(zio->io_abd, zio->io_size);
	dio->vdi_data = data;
	zio->io_bio = dio;

	rw = zio->io_type == ZIO_TYPE_READ ? B_READ : B_WRITE;
	offset = zio->io_offset;
	remaining = zio->io_size;
	for (size_t i = 0; i < nbufs; i++) {
		vdev_disk_buf_t *vdb = &dio->vdi_bufs[i];
		struct buf *bp = &vdb->vdb_buf;
		size_t size = MIN(remaining, (uint64_t)MAXPHYS);

		vdb->vdb_io = dio;
		bp->b_flags = B_PHYS | B_CALL | rw;
		bp->b_proc = NULL;
		bp->b_error = 0;
		bp->b_dev = dvd->vd_dev;
		bp->b_blkno = btodb(offset);
		bp->b_data = data;
		bp->b_bcount = size;
		bp->b_bufsize = size;
		bp->b_resid = size;
		bp->b_iodone = vdev_disk_io_intr;
		bp->b_vp = dvd->vd_vp;

		offset += size;
		remaining -= size;
		data += size;
	}

	VERIFY3U(taskq_dispatch(system_taskq, vdev_disk_io_submit, dio,
	    TQ_SLEEP), !=, TASKQID_INVALID);
	return (0);
}

static void
vdev_disk_io_flush(void *arg)
{
	zio_t *zio = arg;
	vdev_disk_t *dvd = zio->io_vd->vdev_tsd;
	int error, force = 1;

	if (dvd == NULL) {
		error = ENXIO;
	} else {
		KERNEL_LOCK();
		error = VOP_IOCTL(dvd->vd_vp, DIOCCACHESYNC,
		    (caddr_t)&force, dvd->vd_open_flags, NOCRED, curproc);
		KERNEL_UNLOCK();
		if (error == ENOTTY || error == EOPNOTSUPP)
			error = ENOTSUP;
	}

	zio->io_error = error;
	zio_interrupt(zio);
}

static void
vdev_disk_io_trim(void *arg)
{
	zio_t *zio = arg;
	vdev_disk_t *dvd = zio->io_vd->vdev_tsd;
	struct dk_discard *discard;
	int error;

	if (dvd == NULL) {
		error = ENXIO;
	} else if (zio->io_size == 0 ||
	    zio->io_offset > UINT64_MAX - zio->io_size) {
		error = EINVAL;
	} else {
		/* Avoid placing the fixed, multi-range ioctl structure on stack. */
		discard = kmem_zalloc(sizeof (*discard), KM_SLEEP);
		discard->nranges = 1;
		discard->ranges[0].offset = zio->io_offset;
		discard->ranges[0].length = zio->io_size;

		KERNEL_LOCK();
		error = VOP_IOCTL(dvd->vd_vp, DIOCDISCARD,
		    (caddr_t)discard, dvd->vd_open_flags, NOCRED, curproc);
		KERNEL_UNLOCK();
		kmem_free(discard, sizeof (*discard));
		if (error == ENOTTY || error == EOPNOTSUPP)
			error = ENOTSUP;
	}

	zio->io_error = error;
	zio_interrupt(zio);
}

static void
vdev_disk_io_start(zio_t *zio)
{
	vdev_t *vd = zio->io_vd;
	vdev_disk_t *dvd = vd->vdev_tsd;
	int error;

	if (dvd == NULL) {
		zio->io_error = SET_ERROR(ENXIO);
		zio_interrupt(zio);
		return;
	}

	switch (zio->io_type) {
	case ZIO_TYPE_READ:
	case ZIO_TYPE_WRITE:
		zio->io_target_timestamp = zio_handle_io_delay(zio);
		error = vdev_disk_io_rw(zio, dvd);
		if (error != 0) {
			zio->io_error = error;
			zio_interrupt(zio);
		}
		return;

	case ZIO_TYPE_FLUSH:
		if (!vdev_readable(vd)) {
			zio->io_error = SET_ERROR(ENXIO);
			zio_interrupt(zio);
			return;
		}
		if (zfs_nocacheflush) {
			zio_execute(zio);
			return;
		}
		if (vd->vdev_nowritecache) {
			zio->io_error = SET_ERROR(ENOTSUP);
			zio_execute(zio);
			return;
		}
		VERIFY3U(taskq_dispatch(system_taskq, vdev_disk_io_flush, zio,
		    TQ_SLEEP), !=, TASKQID_INVALID);
		return;

	case ZIO_TYPE_TRIM:
		if (zio->io_trim_flags & ZIO_TRIM_SECURE) {
			zio->io_error = SET_ERROR(ENOTSUP);
			zio_execute(zio);
			return;
		}
		VERIFY3U(taskq_dispatch(system_taskq, vdev_disk_io_trim, zio,
		    TQ_SLEEP), !=, TASKQID_INVALID);
		return;

	default:
		zio->io_error = SET_ERROR(ENOTSUP);
		zio_execute(zio);
		return;
	}
}

static void
vdev_disk_io_done(zio_t *zio)
{
	vdev_disk_io_t *dio = zio->io_bio;

	if (dio == NULL)
		return;

	zio->io_bio = NULL;
	if (zio->io_type == ZIO_TYPE_READ)
		abd_return_buf_copy(zio->io_abd, dio->vdi_data, zio->io_size);
	else
		abd_return_buf(zio->io_abd, dio->vdi_data, zio->io_size);
	kmem_free(dio, dio->vdi_alloc_size);
}

static void
vdev_disk_hold(vdev_t *vd)
{
	ASSERT(vd->vdev_path != NULL);
}

static void
vdev_disk_rele(vdev_t *vd)
{
	ASSERT(vd->vdev_path != NULL);
}

vdev_ops_t vdev_disk_ops = {
	.vdev_op_init = NULL,
	.vdev_op_fini = NULL,
	.vdev_op_open = vdev_disk_open,
	.vdev_op_close = vdev_disk_close,
	.vdev_op_psize_to_asize = vdev_default_asize,
	.vdev_op_asize_to_psize = vdev_default_psize,
	.vdev_op_min_asize = vdev_default_min_asize,
	.vdev_op_min_alloc = NULL,
	.vdev_op_io_start = vdev_disk_io_start,
	.vdev_op_io_done = vdev_disk_io_done,
	.vdev_op_state_change = NULL,
	.vdev_op_need_resilver = NULL,
	.vdev_op_hold = vdev_disk_hold,
	.vdev_op_rele = vdev_disk_rele,
	.vdev_op_remap = NULL,
	.vdev_op_xlate = vdev_default_xlate,
	.vdev_op_rebuild_asize = NULL,
	.vdev_op_metaslab_init = NULL,
	.vdev_op_config_generate = NULL,
	.vdev_op_nparity = NULL,
	.vdev_op_ndisks = NULL,
	.vdev_op_kobj_evt_post = NULL,
	.vdev_op_type = VDEV_TYPE_DISK,
	.vdev_op_leaf = B_TRUE
};
