// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS time compatibility for the OpenBSD kernel. */

#ifndef _SPL_SYS_TIME_H
#define _SPL_SYS_TIME_H

#include_next <sys/time.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/types.h>

extern volatile unsigned long jiffies;

#define	SEC		1
#define	MILLISEC	1000UL
#define	MICROSEC	1000000UL
#define	NANOSEC		1000000000UL

#define	MSEC2NSEC(m)	((hrtime_t)(m) * (NANOSEC / MILLISEC))
#define	NSEC2MSEC(n)	((n) / (NANOSEC / MILLISEC))
#define	USEC2NSEC(u)	((hrtime_t)(u) * (NANOSEC / MICROSEC))
#define	NSEC2USEC(n)	((n) / (NANOSEC / MICROSEC))
#define	NSEC2SEC(n)	((n) / NANOSEC)
#define	SEC2NSEC(s)	((hrtime_t)(s) * NANOSEC)
#define	SEC_TO_TICK(s)	((s) * hz)
#define	MSEC_TO_TICK(m)	(howmany((hrtime_t)(m) * hz, MILLISEC))
#define	USEC_TO_TICK(u)	(howmany((hrtime_t)(u) * hz, MICROSEC))
#define	NSEC_TO_TICK(n)	(howmany((hrtime_t)(n) * hz, NANOSEC))

static inline hrtime_t
gethrtime(void)
{

	return ((hrtime_t)nsecuptime());
}

static inline time_t
gethrestime_sec(void)
{

	return (gettime());
}

#define	gethrestime(ts)		getnanotime(ts)
#define	getlrtime()		gethrtime()
#define	gethrtime_waitfree()	gethrtime()
#define	ddi_get_lbolt()		((clock_t)jiffies)
#define	ddi_get_lbolt64()	((int64_t)jiffies)

#endif /* _SPL_SYS_TIME_H */
