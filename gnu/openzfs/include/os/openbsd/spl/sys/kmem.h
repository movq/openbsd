// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS kernel allocation compatibility for OpenBSD. */

#ifndef _SPL_SYS_KMEM_H
#define _SPL_SYS_KMEM_H

#include <sys/types.h>
#include <sys/malloc.h>
#include <sys/refcnt.h>
#include <sys/pool.h>

#define	KM_SLEEP	M_WAITOK
#define	KM_PUSHPAGE	M_WAITOK
#define	KM_NOSLEEP	M_NOWAIT
#define	KM_NORMALPRI	0

#define	KMC_NODEBUG	0
#define	KMC_RECLAIMABLE	0

#define	POINTER_IS_VALID(p)	(!((uintptr_t)(p) & 0x3))
#define	POINTER_INVALIDATE(pp) \
	(*(pp) = (void *)((uintptr_t)(*(pp)) | 0x1))

struct vmem;
typedef struct vmem vmem_t;

typedef struct kmem_cache {
	struct pool	kc_pool;
	char		kc_name[32];
	size_t		kc_size;
	int		(*kc_constructor)(void *, void *, int);
	void		(*kc_destructor)(void *, void *);
	void		*kc_private;
	volatile uint32_t kc_reaping;
	volatile uint64_t kc_reap_runs;
	volatile uint64_t kc_reap_items;
	volatile uint64_t kc_reap_pages;
	volatile uint64_t kc_reap_last_ns;
	volatile uint64_t kc_reap_max_ns;
} kmem_cache_t;

void	*zfs_kmem_alloc(size_t, int)
    __attribute__((__malloc__, __alloc_size__(1)));
void	 zfs_kmem_free(void *, size_t);
uint64_t kmem_size(void);

kmem_cache_t *kmem_cache_create(const char *, size_t, size_t,
    int (*)(void *, void *, int), void (*)(void *, void *),
    void (*)(void *), void *, vmem_t *, int);
void	 kmem_cache_destroy(kmem_cache_t *);
void	*kmem_cache_alloc(kmem_cache_t *, int) __attribute__((__malloc__));
void	 kmem_cache_free(kmem_cache_t *, void *);
boolean_t kmem_cache_reap_active(void);
void	 kmem_cache_reap_soon(kmem_cache_t *);
void	 kmem_reap(void);
int	 kmem_debugging(void);
uint64_t spl_kmem_cache_inuse(kmem_cache_t *);
uint64_t spl_kmem_cache_entry_size(kmem_cache_t *);

char	*kmem_vasprintf(const char *, va_list)
    __attribute__((__format__(__kprintf__, 1, 0)));
char	*kmem_asprintf(const char *, ...)
    __attribute__((__format__(__kprintf__, 1, 2)));
char	*kmem_strdup(const char *);
void	 kmem_strfree(char *);
int	 kmem_scnprintf(char *, size_t, const char *, ...)
    __attribute__((__format__(__kprintf__, 3, 4)));

#define	kmem_alloc(size, flags)	zfs_kmem_alloc((size), (flags))
#define	kmem_zalloc(size, flags) \
	zfs_kmem_alloc((size), (flags) | M_ZERO)
#define	kmem_free(buf, size)	zfs_kmem_free((buf), (size))
#define	kmem_cache_reap_now	kmem_cache_reap_soon

#endif /* _SPL_SYS_KMEM_H */
