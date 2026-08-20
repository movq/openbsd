// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS condition variables implemented with OpenBSD wait channels. */

#ifndef _SPL_SYS_CONDVAR_H
#define _SPL_SYS_CONDVAR_H

#include <sys/types.h>
#include <sys/mutex.h>

typedef struct kcondvar {
	const char	*cv_name;
} kcondvar_t;

typedef enum {
	CV_DEFAULT,
	CV_DRIVER
} kcv_type_t;

void	 zfs_cv_init(kcondvar_t *, const char *, kcv_type_t, void *);
void	 zfs_cv_destroy(kcondvar_t *);
void	 zfs_cv_wait(kcondvar_t *, kmutex_t *);
int	 zfs_cv_wait_sig(kcondvar_t *, kmutex_t *);
int	 zfs_cv_timedwait(kcondvar_t *, kmutex_t *, clock_t);
int	 zfs_cv_timedwait_sig(kcondvar_t *, kmutex_t *, clock_t);
int	 zfs_cv_timedwait_hires(kcondvar_t *, kmutex_t *, hrtime_t,
    hrtime_t, int);
int	 zfs_cv_timedwait_sig_hires(kcondvar_t *, kmutex_t *, hrtime_t,
    hrtime_t, int);
void	 zfs_cv_signal(kcondvar_t *);
void	 zfs_cv_broadcast(kcondvar_t *);

#define	cv_init(cv, name, type, arg) \
	zfs_cv_init((cv), #cv, (type), (arg))
#define	cv_destroy(cv)		zfs_cv_destroy((cv))
#define	cv_wait(cv, lock)	zfs_cv_wait((cv), (lock))
#define	cv_wait_sig(cv, lock)	zfs_cv_wait_sig((cv), (lock))
#define	cv_timedwait(cv, lock, at) \
	zfs_cv_timedwait((cv), (lock), (at))
#define	cv_timedwait_sig(cv, lock, at) \
	zfs_cv_timedwait_sig((cv), (lock), (at))
#define	cv_timedwait_hires(cv, lock, tim, res, flag) \
	zfs_cv_timedwait_hires((cv), (lock), (tim), (res), (flag))
#define	cv_timedwait_sig_hires(cv, lock, tim, res, flag) \
	zfs_cv_timedwait_sig_hires((cv), (lock), (tim), (res), (flag))
#define	cv_signal(cv)		zfs_cv_signal((cv))
#define	cv_broadcast(cv)	zfs_cv_broadcast((cv))

#define	cv_timedwait_io		cv_timedwait
#define	cv_timedwait_idle	cv_timedwait
#define	cv_timedwait_sig_io	cv_timedwait_sig
#define	cv_wait_io		cv_wait
#define	cv_wait_io_sig		cv_wait_sig
#define	cv_wait_idle		cv_wait
#define	cv_timedwait_io_hires	cv_timedwait_hires
#define	cv_timedwait_idle_hires	cv_timedwait_hires

#define	CALLOUT_FLAG_ABSOLUTE	1

#endif /* _SPL_SYS_CONDVAR_H */
