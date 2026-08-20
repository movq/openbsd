// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS thread-specific data for OpenBSD. */

#ifndef _SPL_SYS_TSD_H
#define _SPL_SYS_TSD_H

#include <sys/types.h>

struct proc;

#define	TSD_HASH_TABLE_BITS_DEFAULT	9
#define	TSD_KEYS_MAX			32768

typedef void (*dtor_func_t)(void *);

int	 tsd_set(uint_t, void *);
void	*tsd_get(uint_t);
void	*tsd_get_by_thread(uint_t, struct proc *);
void	 tsd_create(uint_t *, dtor_func_t);
void	 tsd_destroy(uint_t *);
void	 tsd_exit(void);

/* Called from the native thread-exit path, including non-ZFS threads. */
void	 zfs_tsd_exit(struct proc *);

#endif /* _SPL_SYS_TSD_H */
