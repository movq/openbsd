// SPDX-License-Identifier: CDDL-1.0
/*
 * Copyright (c) 2014 by Chunwei Chen. All rights reserved.
 * Copyright (c) 2016, 2019 by Delphix. All rights reserved.
 */

/*
 * OpenBSD ARC buffer data implementation.
 *
 * Scattered ABDs use page-sized objects from a native pool-backed kmem cache.
 * The objects are permanently mapped kernel memory, so iteration needs no
 * temporary VM mappings.  Page-backed direct-I/O ABDs are intentionally left
 * for the vnode integration layer.
 */

#include <sys/abd_impl.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/zio.h>
#include <sys/zfs_context.h>

typedef struct abd_stats {
	kstat_named_t abdstat_struct_size;
	kstat_named_t abdstat_scatter_cnt;
	kstat_named_t abdstat_scatter_data_size;
	kstat_named_t abdstat_scatter_chunk_waste;
	kstat_named_t abdstat_linear_cnt;
	kstat_named_t abdstat_linear_data_size;
	kstat_named_t abdstat_chunk_pages;
	kstat_named_t abdstat_chunk_cached;
	kstat_named_t abdstat_reap_runs;
	kstat_named_t abdstat_reap_items;
	kstat_named_t abdstat_reap_pages;
	kstat_named_t abdstat_reap_last_ns;
	kstat_named_t abdstat_reap_max_ns;
} abd_stats_t;

static abd_stats_t abd_stats = {
	{ "struct_size", KSTAT_DATA_UINT64 },
	{ "scatter_cnt", KSTAT_DATA_UINT64 },
	{ "scatter_data_size", KSTAT_DATA_UINT64 },
	{ "scatter_chunk_waste", KSTAT_DATA_UINT64 },
	{ "linear_cnt", KSTAT_DATA_UINT64 },
	{ "linear_data_size", KSTAT_DATA_UINT64 },
	{ "chunk_pages", KSTAT_DATA_UINT64 },
	{ "chunk_cached", KSTAT_DATA_UINT64 },
	{ "reap_runs", KSTAT_DATA_UINT64 },
	{ "reap_items", KSTAT_DATA_UINT64 },
	{ "reap_pages", KSTAT_DATA_UINT64 },
	{ "reap_last_ns", KSTAT_DATA_UINT64 },
	{ "reap_max_ns", KSTAT_DATA_UINT64 },
};

struct {
	wmsum_t abdstat_struct_size;
	wmsum_t abdstat_scatter_cnt;
	wmsum_t abdstat_scatter_data_size;
	wmsum_t abdstat_scatter_chunk_waste;
	wmsum_t abdstat_linear_cnt;
	wmsum_t abdstat_linear_data_size;
} abd_sums;

static size_t zfs_abd_scatter_min_size = PAGE_SIZE + 1;
static kmem_cache_t *abd_chunk_cache;
static kstat_t *abd_ksp;
static void *abd_zero_chunk;

abd_t *abd_zero_scatter;

static uint_t
abd_chunkcnt_for_bytes(size_t size)
{
	return ((size + PAGE_MASK) >> PAGE_SHIFT);
}

static uint_t
abd_scatter_chunkcnt(abd_t *abd)
{
	ASSERT(!abd_is_linear(abd));
	return (abd_chunkcnt_for_bytes(ABD_SCATTER(abd).abd_offset +
	    abd->abd_size));
}

boolean_t
abd_size_alloc_linear(size_t size)
{
	return (!zfs_abd_scatter_enabled || size < zfs_abd_scatter_min_size);
}

void
abd_update_scatter_stats(abd_t *abd, abd_stats_op_t op)
{
	uint_t nchunks = abd_scatter_chunkcnt(abd);
	int waste = (nchunks << PAGE_SHIFT) - abd->abd_size;

	ASSERT(op == ABDSTAT_INCR || op == ABDSTAT_DECR);
	if (op == ABDSTAT_INCR) {
		ABDSTAT_BUMP(abdstat_scatter_cnt);
		ABDSTAT_INCR(abdstat_scatter_data_size, abd->abd_size);
		ABDSTAT_INCR(abdstat_scatter_chunk_waste, waste);
	} else {
		ABDSTAT_BUMPDOWN(abdstat_scatter_cnt);
		ABDSTAT_INCR(abdstat_scatter_data_size, -(int)abd->abd_size);
		ABDSTAT_INCR(abdstat_scatter_chunk_waste, -waste);
	}
}

void
abd_update_linear_stats(abd_t *abd, abd_stats_op_t op)
{
	ASSERT(op == ABDSTAT_INCR || op == ABDSTAT_DECR);
	if (op == ABDSTAT_INCR) {
		ABDSTAT_BUMP(abdstat_linear_cnt);
		ABDSTAT_INCR(abdstat_linear_data_size, abd->abd_size);
	} else {
		ABDSTAT_BUMPDOWN(abdstat_linear_cnt);
		ABDSTAT_INCR(abdstat_linear_data_size, -(int)abd->abd_size);
	}
}

void
abd_verify_scatter(abd_t *abd)
{
	uint_t i, nchunks;

	ASSERT(!abd_is_linear_page(abd));
	ASSERT(!abd_is_from_pages(abd));
	ASSERT3U(ABD_SCATTER(abd).abd_offset, <, PAGE_SIZE);
	nchunks = abd_scatter_chunkcnt(abd);
	for (i = 0; i < nchunks; i++)
		ASSERT3P(ABD_SCATTER(abd).abd_chunks[i], !=, NULL);
}

void
abd_alloc_chunks(abd_t *abd, size_t size)
{
	uint_t i, nchunks = abd_chunkcnt_for_bytes(size);

	for (i = 0; i < nchunks; i++)
		ABD_SCATTER(abd).abd_chunks[i] =
		    kmem_cache_alloc(abd_chunk_cache, KM_PUSHPAGE);
}

void
abd_free_chunks(abd_t *abd)
{
	uint_t i, nchunks = abd_scatter_chunkcnt(abd);

	ASSERT(!abd_is_from_pages(abd));
	for (i = 0; i < nchunks; i++)
		kmem_cache_free(abd_chunk_cache,
		    ABD_SCATTER(abd).abd_chunks[i]);
}

abd_t *
abd_alloc_struct_impl(size_t size)
{
	uint_t nchunks = abd_chunkcnt_for_bytes(size);
	size_t struct_size = MAX(sizeof (abd_t),
	    offsetof(abd_t, abd_u.abd_scatter.abd_chunks[nchunks]));
	abd_t *abd = kmem_alloc(struct_size, KM_PUSHPAGE);

	ABDSTAT_INCR(abdstat_struct_size, struct_size);
	return (abd);
}

void
abd_free_struct_impl(abd_t *abd)
{
	uint_t nchunks = abd_is_linear(abd) || abd_is_gang(abd) ? 0 :
	    abd_scatter_chunkcnt(abd);
	size_t struct_size = MAX(sizeof (abd_t),
	    offsetof(abd_t, abd_u.abd_scatter.abd_chunks[nchunks]));

	kmem_free(abd, struct_size);
	ABDSTAT_INCR(abdstat_struct_size, -(int64_t)struct_size);
}

void *
abd_alloc_linear_buf(size_t size, boolean_t is_metadata)
{
	(void)is_metadata;
	return (kmem_alloc(size, KM_PUSHPAGE));
}

void
abd_free_linear_buf(void *buf, size_t size, boolean_t is_metadata)
{
	(void)is_metadata;
	kmem_free(buf, size);
}

static void
abd_alloc_zero_scatter(void)
{
	uint_t i, nchunks = abd_chunkcnt_for_bytes(SPA_MAXBLOCKSIZE);

	abd_zero_scatter = abd_alloc_struct(SPA_MAXBLOCKSIZE);
	abd_zero_scatter->abd_flags |= ABD_FLAG_OWNER;
	abd_zero_scatter->abd_size = SPA_MAXBLOCKSIZE;
	ABD_SCATTER(abd_zero_scatter).abd_offset = 0;

	abd_zero_chunk = kmem_cache_alloc(abd_chunk_cache, KM_PUSHPAGE);
	memset(abd_zero_chunk, 0, PAGE_SIZE);
	for (i = 0; i < nchunks; i++)
		ABD_SCATTER(abd_zero_scatter).abd_chunks[i] = abd_zero_chunk;

	ABDSTAT_BUMP(abdstat_scatter_cnt);
	ABDSTAT_INCR(abdstat_scatter_data_size, PAGE_SIZE);
}

static void
abd_free_zero_scatter(void)
{
	ABDSTAT_BUMPDOWN(abdstat_scatter_cnt);
	ABDSTAT_INCR(abdstat_scatter_data_size, -(int)PAGE_SIZE);
	abd_free_struct(abd_zero_scatter);
	abd_zero_scatter = NULL;
	kmem_cache_free(abd_chunk_cache, abd_zero_chunk);
	abd_zero_chunk = NULL;
}

static int
abd_kstats_update(kstat_t *ksp, int rw)
{
	abd_stats_t *as = ksp->ks_data;

	if (rw == KSTAT_WRITE)
		return (EACCES);
	as->abdstat_struct_size.value.ui64 =
	    wmsum_value(&abd_sums.abdstat_struct_size);
	as->abdstat_scatter_cnt.value.ui64 =
	    wmsum_value(&abd_sums.abdstat_scatter_cnt);
	as->abdstat_scatter_data_size.value.ui64 =
	    wmsum_value(&abd_sums.abdstat_scatter_data_size);
	as->abdstat_scatter_chunk_waste.value.ui64 =
	    wmsum_value(&abd_sums.abdstat_scatter_chunk_waste);
	as->abdstat_linear_cnt.value.ui64 =
	    wmsum_value(&abd_sums.abdstat_linear_cnt);
	as->abdstat_linear_data_size.value.ui64 =
	    wmsum_value(&abd_sums.abdstat_linear_data_size);
	as->abdstat_chunk_pages.value.ui64 = abd_chunk_cache->kc_pool.pr_npages;
	as->abdstat_chunk_cached.value.ui64 =
	    abd_chunk_cache->kc_pool.pr_cache_nitems;
	as->abdstat_reap_runs.value.ui64 =
	    atomic_load_64(&abd_chunk_cache->kc_reap_runs);
	as->abdstat_reap_items.value.ui64 =
	    atomic_load_64(&abd_chunk_cache->kc_reap_items);
	as->abdstat_reap_pages.value.ui64 =
	    atomic_load_64(&abd_chunk_cache->kc_reap_pages);
	as->abdstat_reap_last_ns.value.ui64 =
	    atomic_load_64(&abd_chunk_cache->kc_reap_last_ns);
	as->abdstat_reap_max_ns.value.ui64 =
	    atomic_load_64(&abd_chunk_cache->kc_reap_max_ns);
	return (0);
}

void
abd_init(void)
{
	abd_chunk_cache = kmem_cache_create("abd_chunk", PAGE_SIZE, 0,
	    NULL, NULL, NULL, NULL, NULL, KMC_NODEBUG | KMC_RECLAIMABLE);

	wmsum_init(&abd_sums.abdstat_struct_size, 0);
	wmsum_init(&abd_sums.abdstat_scatter_cnt, 0);
	wmsum_init(&abd_sums.abdstat_scatter_data_size, 0);
	wmsum_init(&abd_sums.abdstat_scatter_chunk_waste, 0);
	wmsum_init(&abd_sums.abdstat_linear_cnt, 0);
	wmsum_init(&abd_sums.abdstat_linear_data_size, 0);

	abd_ksp = kstat_create("zfs", 0, "abdstats", "misc",
	    KSTAT_TYPE_NAMED, sizeof (abd_stats) / sizeof (kstat_named_t),
	    KSTAT_FLAG_VIRTUAL);
	if (abd_ksp != NULL) {
		abd_ksp->ks_data = &abd_stats;
		abd_ksp->ks_update = abd_kstats_update;
		kstat_install(abd_ksp);
	}

	abd_alloc_zero_scatter();
}

void
abd_fini(void)
{
	abd_free_zero_scatter();
	if (abd_ksp != NULL) {
		kstat_delete(abd_ksp);
		abd_ksp = NULL;
	}

	wmsum_fini(&abd_sums.abdstat_struct_size);
	wmsum_fini(&abd_sums.abdstat_scatter_cnt);
	wmsum_fini(&abd_sums.abdstat_scatter_data_size);
	wmsum_fini(&abd_sums.abdstat_scatter_chunk_waste);
	wmsum_fini(&abd_sums.abdstat_linear_cnt);
	wmsum_fini(&abd_sums.abdstat_linear_data_size);

	kmem_cache_destroy(abd_chunk_cache);
	abd_chunk_cache = NULL;
}

void
abd_free_linear_page(abd_t *abd)
{
	(void)abd;
	panic("page-backed ABDs are not implemented on OpenBSD");
}

abd_t *
abd_alloc_for_io(size_t size, boolean_t is_metadata)
{
	return (abd_alloc_linear(size, is_metadata));
}

abd_t *
abd_get_offset_scatter(abd_t *abd, abd_t *sabd, size_t off, size_t size)
{
	size_t new_offset;
	uint_t nchunks;

	abd_verify(sabd);
	ASSERT3U(off, <=, sabd->abd_size);
	ASSERT(!abd_is_from_pages(sabd));

	new_offset = ABD_SCATTER(sabd).abd_offset + off;
	nchunks = abd_chunkcnt_for_bytes((new_offset & PAGE_MASK) + size);
	ASSERT3U(nchunks, <=, abd_scatter_chunkcnt(sabd));

	if (abd != NULL &&
	    offsetof(abd_t, abd_u.abd_scatter.abd_chunks[nchunks]) >
	    sizeof (abd_t))
		abd = NULL;
	if (abd == NULL)
		abd = abd_alloc_struct(nchunks << PAGE_SHIFT);

	ABD_SCATTER(abd).abd_offset = new_offset & PAGE_MASK;
	memcpy(ABD_SCATTER(abd).abd_chunks,
	    &ABD_SCATTER(sabd).abd_chunks[new_offset >> PAGE_SHIFT],
	    nchunks * sizeof (void *));
	return (abd);
}

void
abd_iter_init(struct abd_iter *aiter, abd_t *abd)
{
	ASSERT(!abd_is_gang(abd));
	abd_verify(abd);
	memset(aiter, 0, sizeof (*aiter));
	aiter->iter_abd = abd;
}

boolean_t
abd_iter_at_end(struct abd_iter *aiter)
{
	return (aiter->iter_pos == aiter->iter_abd->abd_size);
}

void
abd_iter_advance(struct abd_iter *aiter, size_t amount)
{
	ASSERT0P(aiter->iter_mapaddr);
	ASSERT0(aiter->iter_mapsize);
	if (!abd_iter_at_end(aiter))
		aiter->iter_pos += amount;
}

void
abd_iter_map(struct abd_iter *aiter)
{
	abd_t *abd;
	size_t offset;
	void *base;

	ASSERT0P(aiter->iter_mapaddr);
	ASSERT0(aiter->iter_mapsize);
	if (abd_iter_at_end(aiter))
		return;

	abd = aiter->iter_abd;
	offset = aiter->iter_pos;
	if (abd_is_linear(abd)) {
		base = ABD_LINEAR_BUF(abd);
		aiter->iter_mapsize = abd->abd_size - offset;
	} else {
		offset += ABD_SCATTER(abd).abd_offset;
		base = ABD_SCATTER(abd).abd_chunks[offset >> PAGE_SHIFT];
		offset &= PAGE_MASK;
		aiter->iter_mapsize = MIN(PAGE_SIZE - offset,
		    abd->abd_size - aiter->iter_pos);
	}
	aiter->iter_mapaddr = (char *)base + offset;
}

void
abd_iter_unmap(struct abd_iter *aiter)
{
	if (!abd_iter_at_end(aiter)) {
		ASSERT3P(aiter->iter_mapaddr, !=, NULL);
		ASSERT3U(aiter->iter_mapsize, >, 0);
	}
	aiter->iter_mapaddr = NULL;
	aiter->iter_mapsize = 0;
}

void
abd_cache_reap_now(void)
{
	kmem_cache_reap_soon(abd_chunk_cache);
}

void *
abd_borrow_buf(abd_t *abd, size_t n)
{
	void *buf;

	abd_verify(abd);
	ASSERT3U(abd->abd_size, >=, n);
	buf = abd_is_linear(abd) ? ABD_LINEAR_BUF(abd) :
	    abd_alloc_linear_buf(n, B_TRUE);
#ifdef ZFS_DEBUG
	(void)zfs_refcount_add_many(&abd->abd_children, n, buf);
#endif
	return (buf);
}

void *
abd_borrow_buf_copy(abd_t *abd, size_t n)
{
	void *buf = abd_borrow_buf(abd, n);

	if (!abd_is_linear(abd))
		abd_copy_to_buf(buf, abd, n);
	return (buf);
}

void
abd_return_buf(abd_t *abd, void *buf, size_t n)
{
	abd_verify(abd);
	ASSERT3U(abd->abd_size, >=, n);
#ifdef ZFS_DEBUG
	(void)zfs_refcount_remove_many(&abd->abd_children, n, buf);
#endif
	if (abd_is_linear(abd)) {
		ASSERT3P(buf, ==, ABD_LINEAR_BUF(abd));
	} else {
		ASSERT0(abd_cmp_buf(abd, buf, n));
		abd_free_linear_buf(buf, n, B_TRUE);
	}
}

void
abd_return_buf_copy(abd_t *abd, void *buf, size_t n)
{
	if (!abd_is_linear(abd))
		abd_copy_from_buf(abd, buf, n);
	abd_return_buf(abd, buf, n);
}
