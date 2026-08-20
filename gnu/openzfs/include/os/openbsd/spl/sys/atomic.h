// SPDX-License-Identifier: BSD-2-Clause
/*
 * Solaris atomic-operation compatibility using compiler atomics.
 */

#ifndef _SPL_SYS_ATOMIC_H
#define _SPL_SYS_ATOMIC_H

#include <sys/types.h>

#define	SPL_ATOMIC_OPS(bits, type) \
static inline void \
atomic_add_##bits(volatile type *p, type v) \
{ \
	(void)__atomic_add_fetch(p, v, __ATOMIC_SEQ_CST); \
} \
static inline type \
atomic_add_##bits##_nv(volatile type *p, type v) \
{ \
	return (__atomic_add_fetch(p, v, __ATOMIC_SEQ_CST)); \
} \
static inline void \
atomic_sub_##bits(volatile type *p, type v) \
{ \
	(void)__atomic_sub_fetch(p, v, __ATOMIC_SEQ_CST); \
} \
static inline type \
atomic_sub_##bits##_nv(volatile type *p, type v) \
{ \
	return (__atomic_sub_fetch(p, v, __ATOMIC_SEQ_CST)); \
} \
static inline void \
atomic_inc_##bits(volatile type *p) \
{ \
	atomic_add_##bits(p, 1); \
} \
static inline type \
atomic_inc_##bits##_nv(volatile type *p) \
{ \
	return (atomic_add_##bits##_nv(p, 1)); \
} \
static inline void \
atomic_dec_##bits(volatile type *p) \
{ \
	atomic_sub_##bits(p, 1); \
} \
static inline type \
atomic_dec_##bits##_nv(volatile type *p) \
{ \
	return (atomic_sub_##bits##_nv(p, 1)); \
} \
static inline type \
atomic_cas_##bits(volatile type *p, type old, type new) \
{ \
	(void)__atomic_compare_exchange_n(p, &old, new, 0, \
	    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
	return (old); \
} \
static inline type \
atomic_swap_##bits(volatile type *p, type new) \
{ \
	return (__atomic_exchange_n(p, new, __ATOMIC_SEQ_CST)); \
} \
static inline type \
atomic_load_##bits(volatile const type *p) \
{ \
	return (__atomic_load_n(p, __ATOMIC_SEQ_CST)); \
} \
static inline void \
atomic_store_##bits(volatile type *p, type v) \
{ \
	__atomic_store_n(p, v, __ATOMIC_SEQ_CST); \
}

SPL_ATOMIC_OPS(32, uint32_t)
SPL_ATOMIC_OPS(64, uint64_t)

#undef SPL_ATOMIC_OPS

static inline uint_t
atomic_add_int_nv(volatile uint_t *p, int v)
{
	return (atomic_add_32_nv(p, v));
}

/* Names used by native OpenBSD kernel interfaces such as FRELE(). */
static inline uint_t
atomic_dec_int_nv(volatile uint_t *p)
{
	return (atomic_dec_32_nv(p));
}

static inline void *
atomic_cas_ptr(volatile void *p, void *old, void *new)
{
	void *expected = old;

	(void)__atomic_compare_exchange_n((void *volatile *)p, &expected, new,
	    0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return (expected);
}

static inline void *
atomic_swap_ptr(volatile void *p, void *new)
{
	return (__atomic_exchange_n((void *volatile *)p, new,
	    __ATOMIC_SEQ_CST));
}

static inline void *
atomic_load_ptr(volatile const void *p)
{
	return (__atomic_load_n((void *volatile const *)p, __ATOMIC_SEQ_CST));
}

static inline void
atomic_store_ptr(volatile void *p, void *v)
{
	__atomic_store_n((void *volatile *)p, v, __ATOMIC_SEQ_CST);
}

#define	atomic_read_64(p)	atomic_load_64(p)
#define	atomic_load_consume_ptr(p)	\
	__atomic_load_n((void *volatile const *)(p), __ATOMIC_ACQUIRE)
#define	atomic_store_rel_ptr(p, v)	\
	__atomic_store_n((void *volatile *)(p), (v), __ATOMIC_RELEASE)

#define	membar_consumer()	__atomic_thread_fence(__ATOMIC_ACQUIRE)
#define	membar_producer()	__atomic_thread_fence(__ATOMIC_RELEASE)
#define	membar_sync()		__atomic_thread_fence(__ATOMIC_SEQ_CST)

#endif /* _SPL_SYS_ATOMIC_H */
