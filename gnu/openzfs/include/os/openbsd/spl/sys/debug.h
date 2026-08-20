// SPDX-License-Identifier: BSD-2-Clause
/*
 * OpenZFS assertion compatibility for the OpenBSD kernel.
 */

#ifndef _SPL_SYS_DEBUG_H
#define _SPL_SYS_DEBUG_H

#include <sys/types.h>
#include <sys/systm.h>

#define	PANIC(fmt, ...)	panic("ZFS: " fmt, ##__VA_ARGS__)

#define	VERIFY(cond) \
	((void)(likely(cond) || \
	    (panic("ZFS: VERIFY(%s) failed at %s:%d", #cond, \
	    __FILE__, __LINE__), 0)))

#define	VERIFYF(cond, fmt, ...)	do { \
	if (unlikely(!(cond))) \
		panic("ZFS: VERIFY(%s) failed at %s:%d: " fmt, #cond, \
		    __FILE__, __LINE__, ##__VA_ARGS__); \
} while (0)

#define	VERIFY3S(left, op, right)	do { \
	int64_t _left = (int64_t)(left); \
	int64_t _right = (int64_t)(right); \
	if (unlikely(!(_left op _right))) \
		panic("ZFS: VERIFY3S(%s %s %s) failed (%lld %s %lld) " \
		    "at %s:%d", #left, #op, #right, (long long)_left, #op, \
		    (long long)_right, __FILE__, __LINE__); \
} while (0)

#define	VERIFY3U(left, op, right)	do { \
	uint64_t _left = (uint64_t)(left); \
	uint64_t _right = (uint64_t)(right); \
	if (unlikely(!(_left op _right))) \
		panic("ZFS: VERIFY3U(%s %s %s) failed (%llu %s %llu) " \
		    "at %s:%d", #left, #op, #right, \
		    (unsigned long long)_left, #op, \
		    (unsigned long long)_right, __FILE__, __LINE__); \
} while (0)

#define	VERIFY3P(left, op, right)	do { \
	uintptr_t _left = (uintptr_t)(left); \
	uintptr_t _right = (uintptr_t)(right); \
	if (unlikely(!(_left op _right))) \
		panic("ZFS: VERIFY3P(%s %s %s) failed (%p %s %p) " \
		    "at %s:%d", #left, #op, #right, (void *)_left, #op, \
		    (void *)_right, __FILE__, __LINE__); \
} while (0)

#define	VERIFY3B(left, op, right)	VERIFY3U(!!(left), op, !!(right))
#define	VERIFY0(right)		VERIFY3S(0, ==, right)
#define	VERIFY0P(right)		VERIFY3P(NULL, ==, right)
#define	VERIFY3BF(left, op, right, fmt, ...)	VERIFY3B(left, op, right)
#define	VERIFY3SF(left, op, right, fmt, ...)	VERIFY3S(left, op, right)
#define	VERIFY3UF(left, op, right, fmt, ...)	VERIFY3U(left, op, right)
#define	VERIFY3PF(left, op, right, fmt, ...)	VERIFY3P(left, op, right)
#define	VERIFY0F(right, fmt, ...)		VERIFY0(right)
#define	VERIFY0PF(right, fmt, ...)		VERIFY0P(right)
#define	VERIFY_IMPLY(a, b)	VERIFY(!(a) || (b))
#define	VERIFY_EQUIV(a, b)	VERIFY3B(a, ==, b)

#ifdef NDEBUG
#define	ASSERT(cond)		((void)sizeof (cond))
#define	ASSERT3S(l, o, r)	((void)sizeof (l), (void)sizeof (r))
#define	ASSERT3U(l, o, r)	((void)sizeof (l), (void)sizeof (r))
#define	ASSERT3P(l, o, r)	((void)sizeof (l), (void)sizeof (r))
#define	ASSERT3B(l, o, r)	((void)sizeof (l), (void)sizeof (r))
#define	ASSERT0(r)		((void)sizeof (r))
#define	ASSERT0P(r)		((void)sizeof (r))
#define	ASSERT3BF(l, o, r, fmt, ...)	ASSERT3B(l, o, r)
#define	ASSERT3SF(l, o, r, fmt, ...)	ASSERT3S(l, o, r)
#define	ASSERT3UF(l, o, r, fmt, ...)	ASSERT3U(l, o, r)
#define	ASSERT3PF(l, o, r, fmt, ...)	ASSERT3P(l, o, r)
#define	ASSERT0F(r, fmt, ...)		ASSERT0(r)
#define	ASSERT0PF(r, fmt, ...)		ASSERT0P(r)
#define	ASSERTF(c, fmt, ...)		ASSERT(c)
#define	IMPLY(a, b)		((void)sizeof (a), (void)sizeof (b))
#define	EQUIV(a, b)		((void)sizeof (a), (void)sizeof (b))
#else
#define	ASSERT			VERIFY
#define	ASSERT3S		VERIFY3S
#define	ASSERT3U		VERIFY3U
#define	ASSERT3P		VERIFY3P
#define	ASSERT3B		VERIFY3B
#define	ASSERT0			VERIFY0
#define	ASSERT0P		VERIFY0P
#define	ASSERT3BF		VERIFY3BF
#define	ASSERT3SF		VERIFY3SF
#define	ASSERT3UF		VERIFY3UF
#define	ASSERT3PF		VERIFY3PF
#define	ASSERT0F		VERIFY0F
#define	ASSERT0PF		VERIFY0PF
#define	ASSERTF			VERIFYF
#define	IMPLY			VERIFY_IMPLY
#define	EQUIV			VERIFY_EQUIV
#endif

#endif /* _SPL_SYS_DEBUG_H */
