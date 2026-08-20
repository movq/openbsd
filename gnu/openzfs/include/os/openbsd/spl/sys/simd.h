// SPDX-License-Identifier: BSD-2-Clause
/* OpenBSD kernel SIMD compatibility. */

#ifndef _SPL_SYS_SIMD_H
#define _SPL_SYS_SIMD_H

/*
 * Start with the portable implementations.  Architecture-specific backends
 * can be enabled once their kernel FPU save/restore contract is implemented.
 */
#define ZFS_ASM_DISABLED 1

#define kfpu_allowed()         0
#define kfpu_initialize(tsk)   do { } while (0)
#define kfpu_begin()           do { } while (0)
#define kfpu_end()             do { } while (0)
#define kfpu_init()            0
#define kfpu_fini()            do { } while (0)
#define simd_stat_init()       do { } while (0)
#define simd_stat_fini()       do { } while (0)

#endif /* _SPL_SYS_SIMD_H */
