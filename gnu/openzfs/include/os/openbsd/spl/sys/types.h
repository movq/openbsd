// SPDX-License-Identifier: BSD-2-Clause
/*
 * Solaris-derived types used by the OpenZFS common code.
 */

#ifndef _SPL_SYS_TYPES_H
#define _SPL_SYS_TYPES_H

#include_next <sys/types.h>
#include <sys/stdarg.h>
#include <sys/stdint.h>

typedef unsigned int		uint_t;
typedef unsigned char		uchar_t;
typedef unsigned short		ushort_t;
typedef unsigned long		ulong_t;
typedef unsigned long long	u_longlong_t;
typedef long long		longlong_t;
typedef off_t			loff_t;
typedef off_t			off64_t;
typedef off_t			offset_t;
typedef rlim_t			rlim64_t;
typedef u_longlong_t		u_offset_t;
typedef int64_t			hrlong_t;
typedef int64_t			hrtime_t;
typedef __ptrdiff_t		ptrdiff_t;
typedef int			minor_t;
typedef id_t			taskid_t;
typedef id_t			projid_t;
typedef id_t			poolid_t;
typedef uint_t			zoneid_t;
typedef int			boolean_t;

#ifndef MAXNAMELEN
#define	MAXNAMELEN	256
#endif

#define	B_FALSE	0
#define	B_TRUE	1

typedef struct timespec		timestruc_t;
typedef struct timespec		timespec_t;
typedef struct timespec		inode_timespec_t;

/* OpenBSD has no mount idmapping namespace. */
typedef void			zidmap_t;

#endif /* _SPL_SYS_TYPES_H */
