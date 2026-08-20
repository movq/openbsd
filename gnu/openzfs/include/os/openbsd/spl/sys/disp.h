// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS scheduler compatibility for OpenBSD. */

#ifndef _SPL_SYS_DISP_H
#define _SPL_SYS_DISP_H

#include <sys/systm.h>

#define	KPREEMPT_SYNC		(-1)
#define	kpreempt(arg)		yield()
#define	kpreempt_disable()	((void)0)
#define	kpreempt_enable()	((void)0)

#endif /* _SPL_SYS_DISP_H */
