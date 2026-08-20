// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS arithmetic and CPU compatibility macros for OpenBSD. */

#ifndef _SPL_SYS_SYSMACROS_H
#define _SPL_SYS_SYSMACROS_H

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <machine/cpu.h>

#define	dtob(x)		((x) << DEV_BSHIFT)
#define	btod(x)		(((x) + DEV_BSIZE - 1) >> DEV_BSHIFT)
#define	btodt(x)	((x) >> DEV_BSHIFT)
#define	lbtod(x)	(((offset_t)(x) + DEV_BSIZE - 1) >> DEV_BSHIFT)

#ifndef MIN
#define	MIN(a, b)	((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define	MAX(a, b)	((a) < (b) ? (b) : (a))
#endif
#ifndef ABS
#define	ABS(a)		((a) < 0 ? -(a) : (a))
#endif
#ifndef SIGNOF
#define	SIGNOF(a)	((a) < 0 ? -1 : (a) > 0)
#endif
#ifndef ARRAY_SIZE
#define	ARRAY_SIZE(a)	(sizeof (a) / sizeof ((a)[0]))
#endif
#ifndef DIV_ROUND_UP
#define	DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))
#endif

#define	CPU_SEQID		CPU_INFO_UNIT(curcpu())
#define	CPU_SEQID_UNSTABLE	CPU_INFO_UNIT(curcpu())
#define	is_system_labeled()	0
#define	SET_ERROR(error)	(error)
#define	MAXOFFSET_T		INT64_MAX

void	zfs_qsort(void *, size_t, size_t,
	    int (*)(const void *, const void *));
#define	qsort(base, num, size, cmp)	\
	zfs_qsort((base), (num), (size), (cmp))

static inline uint8_t
zfs_byte_to_bcd(uint8_t value)
{

	return ((value / 10) << 4 | value % 10);
}

static inline uint8_t
zfs_bcd_to_byte(uint8_t value)
{

	return ((value >> 4) * 10 + (value & 0xf));
}

#define	BYTE_TO_BCD(x)	zfs_byte_to_bcd((uint8_t)(x))
#define	BCD_TO_BYTE(x)	zfs_bcd_to_byte((uint8_t)(x))

#define	IS_P2ALIGNED(v, a) \
	((((uintptr_t)(v)) & ((uintptr_t)(a) - 1)) == 0)
#define	ISP2(x)		(((x) & ((x) - 1)) == 0)

#define	P2CROSS(x, y, align)	(((x) ^ (y)) > (align) - 1)
#define	P2PHASE(x, align)	((x) & ((align) - 1))
#define	P2NPHASE(x, align)	(-(x) & ((align) - 1))
#define	P2ROUNDUP(x, align)	(-(-(x) & -(align)))
#define	P2END(x, align)		(-(~(x) & -(align)))
#define	P2PHASEUP(x, align, phase) \
	((phase) - (((phase) - (x)) & -(align)))
#define	P2BOUNDARY(off, len, align) \
	(((off) ^ ((off) + (len) - 1)) > (align) - 1)
#define	P2SAMEHIGHBIT(x, y)	(((x) ^ (y)) < ((x) & (y)))

#define	P2ALIGN_TYPED(x, align, type)	\
	((type)(x) & -(type)(align))
#define	P2PHASE_TYPED(x, align, type)	\
	((type)(x) & ((type)(align) - 1))
#define	P2NPHASE_TYPED(x, align, type)	\
	(-(type)(x) & ((type)(align) - 1))
#define	P2ROUNDUP_TYPED(x, align, type)	\
	(-(-(type)(x) & -(type)(align)))
#define	P2END_TYPED(x, align, type)	\
	(-(~(type)(x) & -(type)(align)))
#define	P2PHASEUP_TYPED(x, align, phase, type)	\
	((type)(phase) - (((type)(phase) - (type)(x)) & -(type)(align)))
#define	P2CROSS_TYPED(x, y, align, type)	\
	(((type)(x) ^ (type)(y)) > (type)(align) - 1)
#define	P2SAMEHIGHBIT_TYPED(x, y, type)	\
	(((type)(x) ^ (type)(y)) < ((type)(x) & (type)(y)))

static inline int
highbit64(uint64_t value)
{
	uint32_t high;

	high = value >> 32;
	return (high != 0 ? 32 + fls(high) : fls((uint32_t)value));
}

static inline int
lowbit64(uint64_t value)
{
	uint32_t low;

	low = value;
	return (low != 0 ? ffs(low) :
	    (value != 0 ? 32 + ffs((uint32_t)(value >> 32)) : 0));
}

#define	highbit(x)	highbit64((uint64_t)(x))
#define	lowbit(x)	lowbit64((uint64_t)(x))

#define	INCR_COUNT(var, lock) do {	\
	mutex_enter(lock);		\
	(*(var))++;			\
	mutex_exit(lock);		\
} while (0)
#define	DECR_COUNT(var, lock) do {	\
	mutex_enter(lock);		\
	(*(var))--;			\
	mutex_exit(lock);		\
} while (0)

#endif /* _SPL_SYS_SYSMACROS_H */
