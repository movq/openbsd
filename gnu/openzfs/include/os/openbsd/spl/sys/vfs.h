// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS VFS compatibility for OpenBSD. */

#ifndef _SPL_SYS_VFS_H
#define	_SPL_SYS_VFS_H

#include <sys/param.h>
#include <sys/mount.h>
#include <sys/vnode.h>

typedef struct mount vfs_t;
typedef int umode_t;

#define	rootdir		rootvnode

#define	vfs_flag	mnt_flag
#define	vfs_data	mnt_data
#define	vfs_fsid	mnt_stat.f_fsid
#define	vfs_bsize	mnt_stat.f_bsize
#define	vfs_resource	mnt_stat.f_mntfromname

#define	v_vfsp		v_mount

#define	VFS_RDONLY	MNT_RDONLY
#define	VFS_NOSETUID	MNT_NOSUID
#define	VFS_NOEXEC	MNT_NOEXEC

typedef uint64_t vfs_feature_t;

#define	VFSFT_XVATTR			0x100000001ULL
#define	VFSFT_CASEINSENSITIVE		0x100000002ULL
#define	VFSFT_NOCASESENSITIVE		0x100000004ULL
#define	VFSFT_DIRENTFLAGS		0x100000008ULL
#define	VFSFT_ACLONCREATE		0x100000010ULL
#define	VFSFT_ACEMASKONACCESS		0x100000020ULL
#define	VFSFT_SYSATTR_VIEWS		0x100000040ULL
#define	VFSFT_ACCESS_FILTER		0x100000080ULL
#define	VFSFT_REPARSE			0x100000100ULL
#define	VFSFT_ZEROCOPY_SUPPORTED	0x100000200ULL

#endif /* _SPL_SYS_VFS_H */
