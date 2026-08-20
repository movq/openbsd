// SPDX-License-Identifier: CDDL-1.0

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/zfs_ioctl.h>
#include <sys/zfs_ioctl_compat.h>

#include <errno.h>

#include "libzfs_core_impl.h"

_Static_assert(ECKSUM == ZFS_ERR_CKSUM,
    "OpenBSD ECKSUM must match the ZFS ioctl ABI");
_Static_assert(ENOTACTIVE == ZFS_ERR_NOTACTIVE,
    "OpenBSD ENOTACTIVE must match the ZFS ioctl ABI");
_Static_assert(EREMOTEIO == ZFS_ERR_ACTIVE_POOL,
    "OpenBSD EREMOTEIO must match the ZFS ioctl ABI");

int
lzc_ioctl_fd_os(int fd, unsigned long request, zfs_cmd_t *zc)
{
	zfs_iocparm_t zp = {
		.zfs_ioctl_version = ZFS_IOCVER_OZFS,
		.zfs_cmd = (uint64_t)(uintptr_t)zc,
		.zfs_cmd_size = sizeof (*zc),
	};
	size_t oldsize = zc->zc_nvlist_dst_size;
	int ret;

	ret = ioctl(fd, _IOWR('Z', ZFS_IOCREQ(request), zfs_iocparm_t), &zp);
	if (ret == 0 && oldsize < zc->zc_nvlist_dst_size) {
		errno = ENOMEM;
		return (-1);
	}
	return (ret);
}
