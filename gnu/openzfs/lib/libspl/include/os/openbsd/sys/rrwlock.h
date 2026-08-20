// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS re-entrant reader/writer locks for OpenBSD userland. */

#ifndef _LIBSPL_OPENBSD_SYS_RRWLOCK_H
#define _LIBSPL_OPENBSD_SYS_RRWLOCK_H

/*
 * OpenBSD and OpenZFS both expose a struct rrwlock and rrw_* routines,
 * but the two interfaces are unrelated.  Load the native declarations
 * before applying the OpenZFS namespace, and keep the structure-tag alias
 * scoped to the shared OpenZFS header.  This mirrors the kernel SPL wrapper.
 */
#include <sys/types.h>
#include <sys/rwlock.h>

#undef rrw_init

#define	rrwlock		zfs_rrwlock
#define	rrw_tsd_key	zfs_rrw_tsd_key
#define	rrw_init	zfs_rrw_init
#define	rrw_destroy	zfs_rrw_destroy
#define	rrw_enter	zfs_rrw_enter
#define	rrw_enter_read	zfs_rrw_enter_read
#define	rrw_enter_read_prio	zfs_rrw_enter_read_prio
#define	rrw_enter_write	zfs_rrw_enter_write
#define	rrw_exit	zfs_rrw_exit
#define	rrw_held	zfs_rrw_held
#define	rrw_tsd_destroy	zfs_rrw_tsd_destroy

#define	rrm_init	zfs_rrm_init
#define	rrm_destroy	zfs_rrm_destroy
#define	rrm_enter	zfs_rrm_enter
#define	rrm_enter_read	zfs_rrm_enter_read
#define	rrm_enter_write	zfs_rrm_enter_write
#define	rrm_exit	zfs_rrm_exit
#define	rrm_held	zfs_rrm_held

#include_next <sys/rrwlock.h>

#undef rrwlock

#endif /* _LIBSPL_OPENBSD_SYS_RRWLOCK_H */
