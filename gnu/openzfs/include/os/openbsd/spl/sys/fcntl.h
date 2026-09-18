// SPDX-License-Identifier: BSD-2-Clause
/* OpenBSD open flags plus compatibility spellings used by OpenZFS. */

#ifndef _SPL_SYS_FCNTL_H
#define	_SPL_SYS_FCNTL_H

#include_next <sys/fcntl.h>
#include <sys/unistd.h>

/* OpenBSD file offsets are always 64-bit in the kernel. */
#define	O_LARGEFILE	0

/* Direct I/O has no native vnode contract in the initial port. */
#define	O_DIRECT	0

/* Solaris-compatible space-management command used by ZIL replay. */
#define	F_FREESP	11

#endif /* _SPL_SYS_FCNTL_H */
