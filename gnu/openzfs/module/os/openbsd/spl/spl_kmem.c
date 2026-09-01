// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS kernel allocation compatibility for OpenBSD. */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/pool.h>
#include <sys/kmem.h>
#include <sys/kmem_cache.h>
#include <sys/debug.h>

void *
zfs_kmem_alloc(size_t size, int flags)
{

	return (malloc(MAX(size, 1), M_ZFS, flags));
}

void
zfs_kmem_free(void *buf, size_t size)
{

	/* OpenZFS callers rely on the native free(9) NULL-pointer no-op. */
	free(buf, M_ZFS, MAX(size, 1));
}

uint64_t
kmem_size(void)
{

	return ((uint64_t)physmem * PAGE_SIZE);
}

kmem_cache_t *
kmem_cache_create(const char *name, size_t size, size_t align,
    int (*constructor)(void *, void *, int),
    void (*destructor)(void *, void *), void (*reclaim)(void *),
    void *private, vmem_t *vmp, int flags)
{
	kmem_cache_t *cache;

	ASSERT3P(vmp, ==, NULL);
	(void)reclaim;
	(void)flags;

	cache = kmem_zalloc(sizeof (*cache), KM_SLEEP);
	strlcpy(cache->kc_name, name, sizeof (cache->kc_name));
	cache->kc_size = size;
	cache->kc_constructor = constructor;
	cache->kc_destructor = destructor;
	cache->kc_private = private;

	/*
	 * OpenZFS cache operations run in thread context and may sleep.  Mark
	 * the pool accordingly so objects larger than one eighth of a page use
	 * pool_allocator_multi_ni and kernel_map.  Without PR_WAITOK, OpenBSD
	 * uses pool_allocator_multi and charges those slabs to the small
	 * interrupt-safe kmem_map; an ARC full of page-sized ABD chunks can
	 * exhaust that map even while physical memory remains available.
	 */
	pool_init(&cache->kc_pool, size, align, IPL_NONE, PR_WAITOK,
	    cache->kc_name, NULL);
	/*
	 * Keep ZIO buffer frees synchronous in debug kernels.  pool_cache_put()
	 * otherwise defers the underlying pool_put() until the per-CPU cache is
	 * drained, which loses the caller responsible for a double or cross-cache
	 * free from the panic trace.
	 */
#if defined(ZFS_DEBUG) || defined(DIAGNOSTIC)
	if (strncmp(name, "zio_buf_", 8) != 0 &&
	    strncmp(name, "zio_data_buf_", 13) != 0)
#endif
	pool_cache_init(&cache->kc_pool);

	return (cache);
}

void
kmem_cache_destroy(kmem_cache_t *cache)
{

	ASSERT3U(cache->kc_pool.pr_nout, ==, 0);
	pool_destroy(&cache->kc_pool);
	kmem_free(cache, sizeof (*cache));
}

void *
kmem_cache_alloc(kmem_cache_t *cache, int flags)
{
	void *buf;

	buf = pool_get(&cache->kc_pool, flags);
	if (buf != NULL && cache->kc_constructor != NULL &&
	    cache->kc_constructor(buf, cache->kc_private, flags) != 0) {
		pool_put(&cache->kc_pool, buf);
		buf = NULL;
	}

	return (buf);
}

void
kmem_cache_free(kmem_cache_t *cache, void *buf)
{

	if (cache->kc_destructor != NULL)
		cache->kc_destructor(buf, cache->kc_private);
	pool_put(&cache->kc_pool, buf);
}

boolean_t
kmem_cache_reap_active(void)
{

	return (B_FALSE);
}

void
kmem_cache_reap_soon(kmem_cache_t *cache)
{

	(void)pool_reclaim(&cache->kc_pool);
}

void
kmem_reap(void)
{

	pool_reclaim_all();
}

int
kmem_debugging(void)
{

	return (0);
}

uint64_t
spl_kmem_cache_inuse(kmem_cache_t *cache)
{

	return (cache->kc_pool.pr_nout);
}

uint64_t
spl_kmem_cache_entry_size(kmem_cache_t *cache)
{

	return (cache->kc_size);
}

/* OpenBSD pools do not currently provide movable-object callbacks. */
void
spl_kmem_cache_set_move(kmem_cache_t *cache,
    kmem_cbrc_t (*move)(void *, void *, size_t, void *))
{

	(void)cache;
	ASSERT3P(move, !=, NULL);
}

char *
kmem_vasprintf(const char *fmt, va_list ap)
{
	char *buf;
	int len;
	va_list aq;

	va_copy(aq, ap);
	len = vsnprintf(NULL, 0, fmt, aq);
	va_end(aq);
	VERIFY3S(len, >=, 0);

	buf = kmem_alloc((size_t)len + 1, KM_SLEEP);
	(void)vsnprintf(buf, (size_t)len + 1, fmt, ap);
	return (buf);
}

char *
kmem_asprintf(const char *fmt, ...)
{
	char *buf;
	va_list ap;

	va_start(ap, fmt);
	buf = kmem_vasprintf(fmt, ap);
	va_end(ap);
	return (buf);
}

char *
kmem_strdup(const char *src)
{
	size_t len = strlen(src) + 1;
	char *dst;

	dst = kmem_alloc(len, KM_SLEEP);
	memcpy(dst, src, len);
	return (dst);
}

void
kmem_strfree(char *str)
{

	ASSERT3P(str, !=, NULL);
	kmem_free(str, strlen(str) + 1);
}

int
kmem_scnprintf(char *buf, size_t size, const char *fmt, ...)
{
	int len;
	va_list ap;

	if (size == 0)
		return (0);

	va_start(ap, fmt);
	len = vsnprintf(buf, size, fmt, ap);
	va_end(ap);

	if (len < 0)
		return (0);
	if ((size_t)len >= size)
		return (size - 1);
	return (len);
}
