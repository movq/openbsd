// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS task queues backed by OpenBSD taskq workers. */

#ifndef _SPL_SYS_TASKQ_H
#define	_SPL_SYS_TASKQ_H

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/task.h>
#include <sys/timeout.h>
#include <sys/proc.h>

#ifdef _KERNEL

#define	TASKQ_NAMELEN	31

struct zfs_taskq;
typedef struct zfs_taskq taskq_t;
typedef uintptr_t taskqid_t;
typedef void (task_func_t)(void *);

typedef struct taskq_ent {
	struct task	 tqent_task;
	struct timeout	 tqent_timeout;
	task_func_t	*tqent_func;
	void		*tqent_arg;
	taskq_t		*tqent_taskq;
	taskqid_t	 tqent_id;
	TAILQ_ENTRY(taskq_ent) tqent_link;
	volatile uint_t tqent_state;
	uint_t		 tqent_flags;
} taskq_ent_t;

#define	TASKQ_PREPOPULATE	0x0001
#define	TASKQ_CPR_SAFE		0x0002
#define	TASKQ_DYNAMIC		0x0004
#define	TASKQ_THREADS_CPU_PCT	0x0008
#define	TASKQ_DC_BATCH		0x0010

#define	TQ_SLEEP	0x00
#define	TQ_NOSLEEP	0x01
#define	TQ_NOQUEUE	0x02
#define	TQ_NOALLOC	0x04
#define	TQ_FRONT	0x08

#define	TASKQID_INVALID	((taskqid_t)0)

extern taskq_t *system_taskq;
extern taskq_t *system_delay_taskq;

taskq_t *zfs_taskq_create(const char *, int, pri_t, int, int, uint_t);
taskq_t *zfs_taskq_create_synced(const char *, int, pri_t, int, int,
    uint_t, kthread_t ***);
taskq_t *zfs_taskq_create_instance(const char *, int, int, pri_t, int, int,
    uint_t);
taskq_t *zfs_taskq_create_proc(const char *, int, pri_t, int, int,
    proc_t *, uint_t);
taskq_t *zfs_taskq_create_sysdc(const char *, int, int, int, proc_t *,
    uint_t, uint_t);
void	 zfs_taskq_destroy(taskq_t *);
taskqid_t zfs_taskq_dispatch(taskq_t *, task_func_t, void *, uint_t);
taskqid_t zfs_taskq_dispatch_delay(taskq_t *, task_func_t, void *, uint_t,
    clock_t);
void	 zfs_taskq_dispatch_ent(taskq_t *, task_func_t, void *, uint_t,
    taskq_ent_t *);
void	 zfs_taskq_init_ent(taskq_ent_t *);
int	 zfs_taskq_empty_ent(taskq_ent_t *);
void	 zfs_taskq_wait(taskq_t *);
void	 zfs_taskq_wait_id(taskq_t *, taskqid_t);
void	 zfs_taskq_wait_outstanding(taskq_t *, taskqid_t);
int	 zfs_taskq_cancel_id(taskq_t *, taskqid_t, boolean_t);
int	 zfs_taskq_member(taskq_t *, kthread_t *);
taskq_t *zfs_taskq_of_curthread(void);
void	 zfs_taskq_suspend(taskq_t *);
int	 zfs_taskq_suspended(taskq_t *);
void	 zfs_taskq_resume(taskq_t *);
void	 nulltask(void *);
void	 system_taskq_init(void);
void	 system_taskq_fini(void);

#define	taskq_create			zfs_taskq_create
#define	taskq_create_synced		zfs_taskq_create_synced
#define	taskq_create_instance		zfs_taskq_create_instance
#define	taskq_create_proc		zfs_taskq_create_proc
#define	taskq_create_sysdc		zfs_taskq_create_sysdc
#define	taskq_destroy			zfs_taskq_destroy
#define	taskq_dispatch			zfs_taskq_dispatch
#define	taskq_dispatch_delay		zfs_taskq_dispatch_delay
#define	taskq_dispatch_ent		zfs_taskq_dispatch_ent
#define	taskq_init_ent			zfs_taskq_init_ent
#define	taskq_empty_ent			zfs_taskq_empty_ent
#define	taskq_wait			zfs_taskq_wait
#define	taskq_wait_id			zfs_taskq_wait_id
#define	taskq_wait_outstanding		zfs_taskq_wait_outstanding
#define	taskq_cancel_id			zfs_taskq_cancel_id
#define	taskq_member			zfs_taskq_member
#define	taskq_of_curthread		zfs_taskq_of_curthread
#define	taskq_suspend			zfs_taskq_suspend
#define	taskq_suspended			zfs_taskq_suspended
#define	taskq_resume			zfs_taskq_resume

#endif /* _KERNEL */

#ifdef _STANDALONE
typedef void taskq_t;
typedef int taskq_ent_t;
#define	taskq_init_ent(ent)
#endif

#endif /* _SPL_SYS_TASKQ_H */
