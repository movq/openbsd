// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS random-byte compatibility for OpenBSD. */

#ifndef _SPL_SYS_RANDOM_H
#define _SPL_SYS_RANDOM_H

#include <sys/types.h>
#include <sys/systm.h>
#include <sys/debug.h>

static inline int
random_get_bytes(uint8_t *buf, size_t len)
{

	arc4random_buf(buf, len);
	return (0);
}

static inline int
random_get_pseudo_bytes(uint8_t *buf, size_t len)
{

	arc4random_buf(buf, len);
	return (0);
}

static inline uint32_t
random_in_range(uint32_t range)
{

	ASSERT3U(range, !=, 0);
	return (arc4random_uniform(range));
}

#endif /* _SPL_SYS_RANDOM_H */
