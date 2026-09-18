// SPDX-License-Identifier: BSD-2-Clause
/* OpenBSD additions to the shared OpenZFS kernel context. */

#ifndef _ZFS_CONTEXT_OS_H
#define _ZFS_CONTEXT_OS_H

#include <sys/fcntl.h>
#include <sys/param.h>
#include <sys/sig.h>
#include <sys/thread.h>
#include <sys/tsd.h>

#define	noinline	__attribute__((__noinline__))
#define	fm_panic	panic

/* spa_thread() is disabled upstream on non-illumos platforms. */
#define	thread_join(tid)	((void)(tid))

/* OpenBSD has no Linux-style filesystem reclaim transaction context. */
typedef int fstrans_cookie_t;
#define	spl_fstrans_mark()	(0)
#define	spl_fstrans_unmark(cookie)	((void)(cookie))

#if USPACE >= 16384
#define HAVE_LARGE_STACKS 1
#endif

#endif /* _ZFS_CONTEXT_OS_H */
