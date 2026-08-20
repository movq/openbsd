// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS synchronization compatibility for OpenBSD. */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/rwlock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/time.h>
#include <sys/debug.h>

/* Use the native functions inside this implementation. */
#undef rw_init
#undef rw_enter
#undef rw_exit

void
zfs_mutex_init(kmutex_t *lock, const char *name, kmutex_type_t type, void *arg,
    const struct lock_type *lock_type)
{
	int flags = RWL_DUPOK;

	ASSERT(type == MUTEX_DEFAULT || type == MUTEX_NOLOCKDEP);
	ASSERT3P(arg, ==, NULL);
	if (type == MUTEX_NOLOCKDEP)
		flags |= RWL_NOWITNESS;
	_rw_init_flags(lock, name, flags, lock_type, 0);
}

void
zfs_mutex_destroy(kmutex_t *lock)
{

	rw_assert_unlocked(lock);
}

void
zfs_mutex_enter(kmutex_t *lock)
{

	rw_enter_write(lock);
}

int
zfs_mutex_enter_interruptible(kmutex_t *lock)
{

	return (rw_enter(lock, RW_WRITE | RW_INTR));
}

int
zfs_mutex_tryenter(kmutex_t *lock)
{

	return (rw_enter(lock, RW_WRITE | RW_NOSLEEP) == 0);
}

void
zfs_mutex_exit(kmutex_t *lock)
{

	rw_exit_write(lock);
}

int
zfs_mutex_owned(kmutex_t *lock)
{

	return (rw_write_held(lock));
}

struct proc *
zfs_mutex_owner(kmutex_t *lock)
{

	if ((lock->rwl_owner & RWLOCK_WRLOCK) == 0)
		return (NULL);
	return (RWLOCK_OWNER(lock));
}

void
zfs_rw_init(krwlock_t *lock, const char *name, krw_type_t type, void *arg,
    const struct lock_type *lock_type)
{
	int flags = RWL_DUPOK;

	ASSERT(type == 0 || type == RW_DEFAULT || type == RW_NOLOCKDEP);
	ASSERT3P(arg, ==, NULL);
	if (type == RW_NOLOCKDEP)
		flags |= RWL_NOWITNESS;
	_rw_init_flags(lock, name, flags, lock_type, 0);
}

void
zfs_rw_destroy(krwlock_t *lock)
{

	rw_assert_unlocked(lock);
}

void
zfs_rw_enter(krwlock_t *lock, krw_t how)
{

	if (how == RW_READER)
		rw_enter_read(lock);
	else {
		ASSERT3S(how, ==, RW_WRITER);
		rw_enter_write(lock);
	}
}

int
zfs_rw_tryenter(krwlock_t *lock, krw_t how)
{
	int flags;

	ASSERT(how == RW_READER || how == RW_WRITER);
	flags = how == RW_READER ? RW_READ : RW_WRITE;
	return (rw_enter(lock, flags | RW_NOSLEEP) == 0);
}

void
zfs_rw_exit(krwlock_t *lock)
{

	rw_exit(lock);
}

void
zfs_rw_downgrade(krwlock_t *lock)
{

	VERIFY0(rw_enter(lock, RW_DOWNGRADE));
}

int
zfs_rw_tryupgrade(krwlock_t *lock)
{

	return (rw_enter(lock, RW_UPGRADE | RW_NOSLEEP) == 0);
}

struct proc *
zfs_rw_owner(krwlock_t *lock)
{

	if ((lock->rwl_owner & RWLOCK_WRLOCK) == 0)
		return (NULL);
	return (RWLOCK_OWNER(lock));
}

void
zfs_cv_init(kcondvar_t *cv, const char *name, kcv_type_t type, void *arg)
{

	ASSERT3S(type, ==, CV_DEFAULT);
	ASSERT3P(arg, ==, NULL);
	cv->cv_name = name;
}

void
zfs_cv_destroy(kcondvar_t *cv)
{

	cv->cv_name = NULL;
}

void
zfs_cv_wait(kcondvar_t *cv, kmutex_t *lock)
{
	int error;

	error = rwsleep(cv, lock, PWAIT, cv->cv_name, 0);
	VERIFY0(error);
}

int
zfs_cv_wait_sig(kcondvar_t *cv, kmutex_t *lock)
{
	int error;

	error = rwsleep(cv, lock, PWAIT | PCATCH, cv->cv_name, 0);
	return (error == 0);
}

static int
zfs_cv_timedwait_common(kcondvar_t *cv, kmutex_t *lock, clock_t abstime,
    int catch)
{
	clock_t delta;
	int error;

	delta = abstime - ddi_get_lbolt();
	if (delta <= 0)
		return (-1);

	error = rwsleep(cv, lock, PWAIT | (catch ? PCATCH : 0), cv->cv_name,
	    delta);
	if (error == EWOULDBLOCK)
		return (-1);
	if (error == EINTR || error == ERESTART)
		return (0);
	VERIFY0(error);
	return (1);
}

int
zfs_cv_timedwait(kcondvar_t *cv, kmutex_t *lock, clock_t abstime)
{

	return (zfs_cv_timedwait_common(cv, lock, abstime, 0));
}

int
zfs_cv_timedwait_sig(kcondvar_t *cv, kmutex_t *lock, clock_t abstime)
{

	return (zfs_cv_timedwait_common(cv, lock, abstime, 1));
}

static int
zfs_cv_timedwait_hires_common(kcondvar_t *cv, kmutex_t *lock, hrtime_t tim,
    hrtime_t res, int flag, int catch)
{
	hrtime_t now;
	int error;

	ASSERT3S(tim, >=, res);
	now = gethrtime();
	if (flag == 0) {
		if (tim > INT64_MAX - now)
			tim = INT64_MAX;
		else
			tim += now;
	}
	if (tim <= now)
		return (-1);

	error = rwsleep_nsec(cv, lock, PWAIT | (catch ? PCATCH : 0),
	    cv->cv_name, tim - now);
	if (error == EWOULDBLOCK)
		return (-1);
	if (error == EINTR || error == ERESTART)
		return (0);
	VERIFY0(error);
	return (1);
}

int
zfs_cv_timedwait_hires(kcondvar_t *cv, kmutex_t *lock, hrtime_t tim,
    hrtime_t res, int flag)
{

	return (zfs_cv_timedwait_hires_common(cv, lock, tim, res, flag, 0));
}

int
zfs_cv_timedwait_sig_hires(kcondvar_t *cv, kmutex_t *lock, hrtime_t tim,
    hrtime_t res, int flag)
{

	return (zfs_cv_timedwait_hires_common(cv, lock, tim, res, flag, 1));
}

void
zfs_cv_signal(kcondvar_t *cv)
{

	wakeup_one(cv);
}

void
zfs_cv_broadcast(kcondvar_t *cv)
{

	wakeup(cv);
}
