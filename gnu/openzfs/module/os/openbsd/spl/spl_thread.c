// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS kernel-thread compatibility for OpenBSD. */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kthread.h>
#include <sys/proc.h>
#include <sys/debug.h>

kthread_t *
zfs_thread_create(const char *name, caddr_t stack, size_t stacksize,
    void (*func)(void *), void *arg, size_t len, proc_t *parent, int state,
    pri_t pri)
{
	kthread_t *thread = NULL;
	int error;

	ASSERT3P(stack, ==, NULL);
	ASSERT3U(stacksize, ==, 0);
	ASSERT3U(len, ==, 0);
	ASSERT3S(state, ==, TS_RUN);
	(void)parent;
	(void)pri;

	error = kthread_create(func, arg, &thread, name);
	if (error != 0)
		return (NULL);
	return (thread);
}
