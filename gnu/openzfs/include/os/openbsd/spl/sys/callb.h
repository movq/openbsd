// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS kernel-thread CPR compatibility for OpenBSD. */

#ifndef _SPL_SYS_CALLB_H
#define	_SPL_SYS_CALLB_H

#include <sys/debug.h>
#include <sys/mutex.h>

/*
 * OpenBSD does not checkpoint kernel threads through an illumos-style
 * callback table.  Keep the locking contract used around sleep points;
 * suspend and resume need no additional registration here.
 */
typedef struct callb_cpr {
	kmutex_t	*cc_lockp;
} callb_cpr_t;

#define	CALLB_CPR_ASSERT(cp)	ASSERT(MUTEX_HELD((cp)->cc_lockp))

#define	CALLB_CPR_INIT(cp, lockp, func, name)	{		\
	(cp)->cc_lockp = (lockp);				\
}

#define	CALLB_CPR_SAFE_BEGIN(cp)	{			\
	CALLB_CPR_ASSERT(cp);					\
}

#define	CALLB_CPR_SAFE_END(cp, lockp)	{			\
	CALLB_CPR_ASSERT(cp);					\
}

#define	CALLB_CPR_EXIT(cp)	{				\
	CALLB_CPR_ASSERT(cp);					\
	mutex_exit((cp)->cc_lockp);				\
}

#endif /* _SPL_SYS_CALLB_H */
