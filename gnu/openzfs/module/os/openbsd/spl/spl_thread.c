// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS kernel-thread compatibility for OpenBSD. */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kthread.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/debug.h>

typedef struct zfs_thread_start {
	void	(*zts_func)(void *);
	void	 *zts_arg;
} zfs_thread_start_t;

static void
zfs_thread_start(void *arg)
{
	zfs_thread_start_t *start = arg;
	void (*func)(void *) = start->zts_func;
	void *func_arg = start->zts_arg;

	/* OpenBSD starts kernel threads under the giant lock. */
	KERNEL_ASSERT_LOCKED();
	free(start, M_TEMP, sizeof (*start));
	KERNEL_UNLOCK();

	func(func_arg);

	KERNEL_LOCK();
	kthread_exit(0);
}

kthread_t *
zfs_thread_create(const char *name, caddr_t stack, size_t stacksize,
    void (*func)(void *), void *arg, size_t len, proc_t *parent, int state,
    pri_t pri)
{
	zfs_thread_start_t *start;
	kthread_t *thread = NULL;
	int error;

	ASSERT3P(stack, ==, NULL);
	ASSERT3U(stacksize, ==, 0);
	ASSERT3U(len, ==, 0);
	ASSERT3S(state, ==, TS_RUN);
	(void)parent;
	(void)pri;

	start = malloc(sizeof (*start), M_TEMP, M_WAITOK);
	start->zts_func = func;
	start->zts_arg = arg;

	error = kthread_create(zfs_thread_start, start, &thread, name);
	if (error != 0) {
		free(start, M_TEMP, sizeof (*start));
		return (NULL);
	}
	return (thread);
}

void
zfs_thread_exit(void)
{

	/* Restore the lock state expected by OpenBSD's exit path. */
	KERNEL_ASSERT_UNLOCKED();
	KERNEL_LOCK();
	kthread_exit(0);
}
