// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS uio wrapper around OpenBSD's native struct uio. */

#ifndef _SPL_SYS_UIO_H
#define _SPL_SYS_UIO_H

#include_next <sys/uio.h>
#include <sys/debug.h>
#include <sys/param.h>
#include <sys/string.h>

#define UIO_DIRECT 0x0001
#define UIO_PAGER  0x0002
#define UIO_ZIL_DEFER 0x0004	/* Defer and report a required ZIL commit. */

typedef struct iovec iovec_t;
typedef enum uio_seg zfs_uio_seg_t;
typedef enum uio_rw zfs_uio_rw_t;

typedef struct zfs_uio_dio {
	void	**pages;
	int	npages;
} zfs_uio_dio_t;

typedef struct zfs_uio {
	struct uio	*uio;
	offset_t	uio_soffset;
	uint16_t	uio_extflg;
	zfs_uio_dio_t	uio_dio;
} zfs_uio_t;

#define GET_UIO_STRUCT(u)	((u)->uio)
#define zfs_uio_segflg(u)	(GET_UIO_STRUCT(u)->uio_segflg)
#define zfs_uio_offset(u)	(GET_UIO_STRUCT(u)->uio_offset)
#define zfs_uio_resid(u)	(GET_UIO_STRUCT(u)->uio_resid)
#define zfs_uio_iovcnt(u)	(GET_UIO_STRUCT(u)->uio_iovcnt)
#define zfs_uio_iovlen(u, i)	(GET_UIO_STRUCT(u)->uio_iov[(i)].iov_len)
#define zfs_uio_iovbase(u, i)	(GET_UIO_STRUCT(u)->uio_iov[(i)].iov_base)
#define zfs_uio_rw(u)		(GET_UIO_STRUCT(u)->uio_rw)
#define zfs_uio_soffset(u)	((u)->uio_soffset)
#define zfs_uio_fault_disable(u, set) do { } while (0)
#define zfs_uio_prefaultpages(size, u) 0
#define zfs_uio_rlimit_fsize(z, u) 0
#define zfs_uio_fault_move(p, n, rw, u) zfs_uiomove((p), (n), (rw), (u))

#ifndef PAGESIZE
#define PAGESIZE PAGE_SIZE
#endif

static inline void
zfs_uio_setoffset(zfs_uio_t *uio, offset_t off)
{
	zfs_uio_offset(uio) = off;
}

static inline void
zfs_uio_setsoffset(zfs_uio_t *uio, offset_t off)
{
	ASSERT3U(zfs_uio_offset(uio), ==, off);
	zfs_uio_soffset(uio) = off;
}

static inline void
zfs_uio_advance(zfs_uio_t *uio, ssize_t size)
{
	zfs_uio_resid(uio) -= size;
	zfs_uio_offset(uio) += size;
}

static inline void
zfs_uio_init(zfs_uio_t *uio, struct uio *native_uio)
{
	memset(uio, 0, sizeof (*uio));
	uio->uio = native_uio;
	if (native_uio != NULL)
		uio->uio_soffset = native_uio->uio_offset;
}

#endif /* _SPL_SYS_UIO_H */
