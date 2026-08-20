// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS write-mostly counters backed by OpenBSD per-CPU counters. */

#ifndef _SPL_SYS_WMSUM_H
#define _SPL_SYS_WMSUM_H

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/percpu.h>

typedef struct cpumem *wmsum_t;

static inline void
wmsum_init(wmsum_t *ws, uint64_t value)
{
	*ws = counters_alloc(1);
	if (value != 0)
		counters_add(*ws, 0, value);
}

static inline void
wmsum_fini(wmsum_t *ws)
{
	counters_free(*ws, 1);
	*ws = NULL;
}

static inline uint64_t
wmsum_value(wmsum_t *ws)
{
	uint64_t value;

	counters_read(*ws, &value, 1, NULL);
	return (value);
}

static inline void
wmsum_add(wmsum_t *ws, int64_t delta)
{
	counters_add(*ws, 0, (uint64_t)delta);
}

#endif /* _SPL_SYS_WMSUM_H */
