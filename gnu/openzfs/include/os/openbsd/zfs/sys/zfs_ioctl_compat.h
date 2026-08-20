// SPDX-License-Identifier: CDDL-1.0

#ifndef _OPENBSD_SYS_ZFS_IOCTL_COMPAT_H
#define	_OPENBSD_SYS_ZFS_IOCTL_COMPAT_H

#include <sys/types.h>

/* Current OpenZFS ioctl ABI. */
#define	ZFS_IOCVER_UNDEF	(-1)
#define	ZFS_IOCVER_NONE		0
#define	ZFS_IOCVER_OZFS		15

#define	ZFS_IOCREQ(ioreq)	((ioreq) & 0xff)

/*
 * OpenBSD's ioctl layer copies this fixed-size envelope.  zfs_cmd points to
 * the full, stable OpenZFS command structure, which the driver copies itself.
 * The explicit pad keeps this envelope identical for 32- and 64-bit callers.
 */
typedef struct zfs_iocparm {
	uint32_t	zfs_ioctl_version;
	uint32_t	zfs_ioctl_pad;
	uint64_t	zfs_cmd;
	uint64_t	zfs_cmd_size;
} zfs_iocparm_t;

_Static_assert(sizeof (zfs_iocparm_t) == 24,
    "zfs_iocparm_t must have a stable ABI layout");

#endif /* _OPENBSD_SYS_ZFS_IOCTL_COMPAT_H */
