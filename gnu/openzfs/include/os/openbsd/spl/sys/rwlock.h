// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS reader/writer lock compatibility for OpenBSD. */

#ifndef _SPL_SYS_RWLOCK_H
#define _SPL_SYS_RWLOCK_H

#include_next <sys/rwlock.h>

struct proc;
typedef struct rwlock krwlock_t;

typedef enum {
	RW_DEFAULT = 4,
	RW_NOLOCKDEP = 5
} krw_type_t;

typedef enum {
	RW_NONE = 0,
	RW_WRITER = 1,
	RW_READER = 2
} krw_t;

void	 zfs_rw_init(krwlock_t *, const char *, krw_type_t, void *,
	    const struct lock_type *);
void	 zfs_rw_destroy(krwlock_t *);
void	 zfs_rw_enter(krwlock_t *, krw_t);
int	 zfs_rw_tryenter(krwlock_t *, krw_t);
void	 zfs_rw_exit(krwlock_t *);
void	 zfs_rw_downgrade(krwlock_t *);
int	 zfs_rw_tryupgrade(krwlock_t *);
struct proc *zfs_rw_owner(krwlock_t *);

#undef rw_init
#ifdef WITNESS
#define	rw_init(lock, desc, type, arg)	do { \
	static const struct lock_type __lock_type = { .lt_name = #lock }; \
	zfs_rw_init((lock), #lock, (type), (arg), &__lock_type); \
} while (0)
#else
#define	rw_init(lock, desc, type, arg) \
	zfs_rw_init((lock), #lock, (type), (arg), NULL)
#endif
#define	rw_destroy(lock)	zfs_rw_destroy((lock))
#define	rw_enter(lock, how)	zfs_rw_enter((lock), (how))
#define	rw_tryenter(lock, how)	zfs_rw_tryenter((lock), (how))
#define	rw_exit(lock)		zfs_rw_exit((lock))
#define	rw_downgrade(lock)	zfs_rw_downgrade((lock))
#define	rw_tryupgrade(lock)	zfs_rw_tryupgrade((lock))
#define	rw_owner(lock)		zfs_rw_owner((lock))

#define	RW_READ_HELD(lock)	rw_read_held((lock))
#define	RW_WRITE_HELD(lock)	rw_write_held((lock))
#define	RW_LOCK_HELD(lock)	rw_lock_held((lock))
#define	RW_ISWRITER(lock)	rw_write_held((lock))

#endif /* _SPL_SYS_RWLOCK_H */
