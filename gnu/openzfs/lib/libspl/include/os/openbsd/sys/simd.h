// SPDX-License-Identifier: BSD-2-Clause
/* OpenBSD userland SIMD compatibility. */

#ifndef _LIBSPL_OPENBSD_SYS_SIMD_H
#define _LIBSPL_OPENBSD_SYS_SIMD_H

/*
 * Keep the portable Fletcher implementations enabled until the OpenBSD
 * userspace build supplies architecture-specific compiler feature headers.
 */
#define ZFS_ASM_DISABLED 1

#define kfpu_allowed()		0
#define kfpu_begin()		do { } while (0)
#define kfpu_end()		do { } while (0)
#define kfpu_init()		0
#define kfpu_fini()		do { } while (0)
#define simd_stat_init()	do { } while (0)
#define simd_stat_fini()	do { } while (0)

#endif /* _LIBSPL_OPENBSD_SYS_SIMD_H */
