// SPDX-License-Identifier: BSD-2-Clause
/* OpenBSD has no DTrace/SDT provider for OpenZFS yet. */

#ifndef _SPL_SYS_TRACE_H
#define	_SPL_SYS_TRACE_H

#define	DTRACE_PROBE(name)				((void)0)
#define	DTRACE_PROBE1(name, t1, a1)			((void)0)
#define	DTRACE_PROBE2(name, t1, a1, t2, a2)		((void)0)
#define	DTRACE_PROBE3(name, t1, a1, t2, a2, t3, a3)	((void)0)
#define	DTRACE_PROBE4(name, t1, a1, t2, a2, t3, a3, t4, a4) \
	((void)0)

/* Linux trace headers use these to declare or define probe functions. */
#define	DEFINE_DTRACE_PROBE(name)
#define	DEFINE_DTRACE_PROBE1(name)
#define	DEFINE_DTRACE_PROBE2(name)
#define	DEFINE_DTRACE_PROBE3(name)
#define	DEFINE_DTRACE_PROBE4(name)

#endif /* _SPL_SYS_TRACE_H */
