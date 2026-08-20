// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS adaptive mutexes implemented by exclusive OpenBSD rwlocks. */

#ifndef _SPL_SYS_MUTEX_H
#define _SPL_SYS_MUTEX_H

#include_next <sys/mutex.h>
#include_next <sys/rwlock.h>

struct proc;
typedef struct rwlock kmutex_t;

typedef enum {
	MUTEX_DEFAULT = 0,
	MUTEX_NOLOCKDEP = 3
} kmutex_type_t;

void	 zfs_mutex_init(kmutex_t *, const char *, kmutex_type_t, void *,
	    const struct lock_type *);
void	 zfs_mutex_destroy(kmutex_t *);
void	 zfs_mutex_enter(kmutex_t *);
int	 zfs_mutex_enter_interruptible(kmutex_t *);
int	 zfs_mutex_tryenter(kmutex_t *);
void	 zfs_mutex_exit(kmutex_t *);
int	 zfs_mutex_owned(kmutex_t *);
struct proc *zfs_mutex_owner(kmutex_t *);

#ifdef WITNESS
#define	mutex_init(lock, desc, type, arg)	do { \
	static const struct lock_type __lock_type = { .lt_name = #lock }; \
	zfs_mutex_init((lock), #lock, (type), (arg), &__lock_type); \
} while (0)
#else
#define	mutex_init(lock, desc, type, arg) \
	zfs_mutex_init((lock), #lock, (type), (arg), NULL)
#endif
#define	mutex_destroy(lock)	zfs_mutex_destroy((lock))
#define	mutex_enter(lock)	zfs_mutex_enter((lock))
#define	mutex_enter_nested(lock, type)	zfs_mutex_enter((lock))
#define	mutex_enter_interruptible(lock) \
	zfs_mutex_enter_interruptible((lock))
#define	mutex_tryenter(lock)	zfs_mutex_tryenter((lock))
#define	mutex_exit(lock)	zfs_mutex_exit((lock))
#define	mutex_owned(lock)	zfs_mutex_owned((lock))
#define	mutex_owner(lock)	zfs_mutex_owner((lock))

#define	MUTEX_HELD(lock)	zfs_mutex_owned((lock))
#define	MUTEX_NOT_HELD(lock)	(!zfs_mutex_owned((lock)) || panicstr)

#endif /* _SPL_SYS_MUTEX_H */
