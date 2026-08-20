// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS process and kernel-thread compatibility for OpenBSD. */

#ifndef _SPL_SYS_PROC_H
#define _SPL_SYS_PROC_H

#include <sys/param.h>
#include <sys/kthread.h>
#include_next <sys/proc.h>
#include <sys/systm.h>

#ifdef _KERNEL

#define	p0		proc0
#define	curthread	curproc
#define	t_tid		p_tid

#define	TS_RUN		0

#define	minclsyspri	PRIBIO
#define	defclsyspri	PWAIT
#define	wtqclsyspri	((PVM + PRIBIO) / 2)
#define	maxclsyspri	PVM

#define	max_ncpus	ncpus
#define	boot_max_ncpus	ncpus
#define	boot_ncpus	ncpus

typedef short pri_t;
typedef struct proc kthread_t;
typedef struct proc *kthread_id_t;
typedef struct proc proc_t;

kthread_t *zfs_thread_create(const char *, caddr_t, size_t,
    void (*)(void *), void *, size_t, proc_t *, int, pri_t);

#define	thread_create_named(name, stk, stksize, func, arg, len, pp, state, \
    pri)	zfs_thread_create((name), (stk), (stksize), (func), (arg), \
    (len), (pp), (state), (pri))
#define	thread_create(stk, stksize, func, arg, len, pp, state, pri) \
	zfs_thread_create(#func, (stk), (stksize), (func), (arg), (len), \
    (pp), (state), (pri))
#define	thread_exit()	kthread_exit(0)

static inline boolean_t
zfs_proc_is_caller(proc_t *p)
{

	return (p == curproc);
}

#endif /* _KERNEL */
#endif /* _SPL_SYS_PROC_H */
