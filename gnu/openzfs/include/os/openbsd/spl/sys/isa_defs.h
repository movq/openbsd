// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS ABI, architecture, and byte-order definitions for OpenBSD. */

#ifndef _SPL_SYS_ISA_DEFS_H
#define _SPL_SYS_ISA_DEFS_H

#include <sys/endian.h>

#if defined(__LP64__) || defined(_LP64)
#ifndef _LP64
#define	_LP64
#endif
#else
#ifndef _ILP32
#define	_ILP32
#endif
#endif

#if defined(_ILP32) && defined(_LP64)
#error "Both _ILP32 and _LP64 are defined"
#endif

#if defined(__amd64__) || defined(__i386__)
#ifndef __x86
#define	__x86
#endif
#endif

#if defined(__amd64__) && !defined(__amd64)
#define	__amd64
#endif
#if defined(__i386__) && !defined(__i386)
#define	__i386
#endif
#if defined(__powerpc__) && !defined(__powerpc)
#define	__powerpc
#endif
#if defined(__sparc__) && !defined(__sparc)
#define	__sparc
#endif

#if BYTE_ORDER == LITTLE_ENDIAN
#define	_ZFS_LITTLE_ENDIAN
#elif BYTE_ORDER == BIG_ENDIAN
#define	_ZFS_BIG_ENDIAN
#else
#error "Unsupported OpenBSD byte order"
#endif

#endif /* _SPL_SYS_ISA_DEFS_H */
