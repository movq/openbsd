// SPDX-License-Identifier: CDDL-1.0

#ifndef _OPENBSD_ZFS_SYS_ZFS_CTLDIR_H
#define _OPENBSD_ZFS_SYS_ZFS_CTLDIR_H

#include <sys/mount.h>

#define ZFS_CTLDIR_NAME ".zfs"

int zfsctl_snapshot_unmount(const char *, int);

#endif /* _OPENBSD_ZFS_SYS_ZFS_CTLDIR_H */
