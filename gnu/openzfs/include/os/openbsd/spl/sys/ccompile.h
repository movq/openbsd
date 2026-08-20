// SPDX-License-Identifier: CDDL-1.0
/*
 * Compiler and build-environment compatibility for the OpenBSD kernel.
 */

#ifndef _SPL_SYS_CCOMPILE_H
#define _SPL_SYS_CCOMPILE_H

#define	EXPORT_SYMBOL(x)
#define	BUILD_BUG_ON(cond)	_Static_assert(!(cond), #cond)

/* sys/dev/pci/arc.c exports these names in the OpenBSD kernel. */
#define	arc_read	zfs_arc_read
#define	arc_write	zfs_arc_write

#define	asm	__asm
#define	__init
#define	__exit
#define	__maybe_unused	__attribute__((__unused__))
#define	__must_check	__attribute__((__warn_unused_result__))
#define	__printf__	__kprintf__
#define	__printflike(a, b) \
	__attribute__((__format__(__kprintf__, a, b)))
#define	____cacheline_aligned	__attribute__((__aligned__(64)))
#define	zfs_fallthrough	__attribute__((__fallthrough__))

#ifndef likely
#define	likely(x)	__predict_true(x)
#endif
#ifndef unlikely
#define	unlikely(x)	__predict_false(x)
#endif

#if defined(DIAGNOSTIC) && !defined(ZFS_DEBUG)
#define	ZFS_DEBUG
#undef NDEBUG
#endif
#if !defined(ZFS_DEBUG) && !defined(NDEBUG)
#define	NDEBUG
#endif

#ifndef ECKSUM
#define	ECKSUM		1053	/* ZFS_ERR_CKSUM */
#endif
#ifndef EFRAGS
#define	EFRAGS		ENOSPC
#endif
#ifndef ENOTACTIVE
#define	ENOTACTIVE	1054	/* ZFS_ERR_NOTACTIVE */
#endif
#ifndef EREMOTEIO
#define	EREMOTEIO	1055	/* ZFS_ERR_ACTIVE_POOL */
#endif

#define	ARRAY_SIZE(a)	(sizeof (a) / sizeof ((a)[0]))

#endif /* _SPL_SYS_CCOMPILE_H */
