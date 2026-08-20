// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS current-thread helpers for OpenBSD. */

#ifndef _SPL_SYS_THREAD_H
#define _SPL_SYS_THREAD_H

#include <sys/proc.h>

#define	getcomm()	(curthread->p_p->ps_comm)
#define	getpid()	(curthread->p_tid)

#endif /* _SPL_SYS_THREAD_H */
