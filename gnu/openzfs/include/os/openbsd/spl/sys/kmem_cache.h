// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS kernel object-cache compatibility for OpenBSD. */

#ifndef _SPL_SYS_KMEM_CACHE_H
#define _SPL_SYS_KMEM_CACHE_H

#include <sys/kmem.h>

/* kmem move callback return values */
typedef enum kmem_cbrc {
	KMEM_CBRC_YES		= 0,
	KMEM_CBRC_NO		= 1,
	KMEM_CBRC_LATER		= 2,
	KMEM_CBRC_DONT_NEED	= 3,
	KMEM_CBRC_DONT_KNOW	= 4,
} kmem_cbrc_t;

void spl_kmem_cache_set_move(kmem_cache_t *,
    kmem_cbrc_t (*)(void *, void *, size_t, void *));

#define	kmem_cache_set_move(cache, move)	\
	spl_kmem_cache_set_move((cache), (move))

#endif /* _SPL_SYS_KMEM_CACHE_H */
