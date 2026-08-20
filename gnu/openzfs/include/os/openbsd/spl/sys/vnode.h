// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS vnode compatibility for OpenBSD. */

#ifndef _SPL_SYS_VNODE_H
#define	_SPL_SYS_VNODE_H

#include_next <sys/vnode.h>

typedef struct vnode vnode_t;
typedef struct vattr vattr_t;
typedef enum vtype vtype_t;
typedef struct flock flock64_t;
typedef const struct vops vnodeops_t;

#define	VN_HOLD(vp)	vref((vp))
#define	VN_RELE(vp)	vrele((vp))
#define	VN_URELE(vp)	vput((vp))

/* va_spare is reserved for the Solaris-style requested-attribute mask. */
#define	va_mask		va_spare
#define	va_nodeid	va_fileid
#define	va_nblocks	va_bytes
#define	va_blksize	va_blocksize

#define	FIGNORECASE	0

#ifndef AT_MODE
#define	AT_MODE		0x00002
#define	AT_UID		0x00004
#define	AT_GID		0x00008
#define	AT_FSID		0x00010
#define	AT_NODEID	0x00020
#define	AT_NLINK	0x00040
#define	AT_SIZE		0x00080
#define	AT_ATIME	0x00100
#define	AT_MTIME	0x00200
#define	AT_CTIME	0x00400
#define	AT_RDEV		0x00800
#define	AT_BLKSIZE	0x01000
#define	AT_NBLOCKS	0x02000
#define	AT_SEQ		0x08000
#define	AT_XVATTR	0x10000
#endif

/* OpenZFS uses both the Solaris AT_* and Linux ATTR_* spellings. */
#define	ATTR_MODE	AT_MODE
#define	ATTR_UID	AT_UID
#define	ATTR_GID	AT_GID
#define	ATTR_FSID	AT_FSID
#define	ATTR_NODEID	AT_NODEID
#define	ATTR_NLINK	AT_NLINK
#define	ATTR_SIZE	AT_SIZE
#define	ATTR_ATIME	AT_ATIME
#define	ATTR_MTIME	AT_MTIME
#define	ATTR_CTIME	AT_CTIME
#define	ATTR_RDEV	AT_RDEV
#define	ATTR_BLKSIZE	AT_BLKSIZE
#define	ATTR_NBLOCKS	AT_NBLOCKS
#define	ATTR_SEQ	AT_SEQ
#define	ATTR_XVATTR	AT_XVATTR

/* Internal callers may request that the ZFS ACL check be skipped. */
#define	ATTR_NOACLCHECK	0x20

#endif /* _SPL_SYS_VNODE_H */
