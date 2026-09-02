// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS timer compatibility for the OpenBSD kernel. */

#ifndef _SPL_SYS_TIMER_H
#define	_SPL_SYS_TIMER_H

#include <sys/time.h>

#define	lbolt			ddi_get_lbolt()
#define	lbolt64			ddi_get_lbolt64()

#define	ddi_time_before(a, b)		((a) < (b))
#define	ddi_time_after(a, b)		ddi_time_before((b), (a))
#define	ddi_time_before_eq(a, b)	(!ddi_time_after((a), (b)))
#define	ddi_time_after_eq(a, b)		ddi_time_before_eq((b), (a))

#define	ddi_time_before64(a, b)		((a) < (b))
#define	ddi_time_after64(a, b)		ddi_time_before64((b), (a))
#define	ddi_time_before_eq64(a, b)	(!ddi_time_after64((a), (b)))
#define	ddi_time_after_eq64(a, b)	ddi_time_before_eq64((b), (a))

void	zfs_usleep_range(unsigned long, unsigned long);
void	zfs_delay(clock_t);

#define	usleep_range(min, max)	zfs_usleep_range((min), (max))
/* Solaris delay() sleeps in ticks; OpenBSD delay() busy-waits in usecs. */
#ifdef delay
#undef delay
#endif
#define	delay(ticks)		zfs_delay((ticks))

#endif /* _SPL_SYS_TIMER_H */
