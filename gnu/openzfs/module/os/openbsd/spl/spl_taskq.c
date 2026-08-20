// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS task queues backed by OpenBSD taskq workers. */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/task.h>
#include <sys/timeout.h>
#include <sys/kmem.h>
#include <sys/condvar.h>
#include <sys/debug.h>
#include <sys/taskq.h>

/* Use the native names for the worker-pool operations in this file. */
#undef taskq_create
#undef taskq_destroy

enum zfs_taskq_ent_state {
	ZFS_TQENT_IDLE,
	ZFS_TQENT_DELAYED,
	ZFS_TQENT_QUEUED,
	ZFS_TQENT_RUNNING,
	ZFS_TQENT_CANCELLED_DELAYED,
	ZFS_TQENT_CANCELLED_QUEUED
};

#define	ZFS_TQENT_PREALLOC	0x01

TAILQ_HEAD(zfs_taskq_entry_list, taskq_ent);

struct zfs_taskq {
	struct taskq		*tq_native;
	struct mutex		 tq_lock;
	struct zfs_taskq_entry_list tq_entries;
	LIST_ENTRY(zfs_taskq)	 tq_link;
	uint64_t		 tq_next_id;
	uint_t		 tq_nthreads;
	uint_t		 tq_active;
	uint_t		 tq_ndynamic;
	uint_t		 tq_ndelayed;
	uint_t		 tq_suspended;
	uint_t		 tq_destroying;
	char			 tq_name[TASKQ_NAMELEN + 1];
};

static struct mutex zfs_taskqs_lock = MUTEX_INITIALIZER_FLAGS(IPL_HIGH,
    "zfstqs", MTX_DUPOK);
static LIST_HEAD(, zfs_taskq) zfs_taskqs = LIST_HEAD_INITIALIZER(zfs_taskqs);

taskq_t *system_taskq;
taskq_t *system_delay_taskq;

static taskq_ent_t *
zfs_taskq_lookup_locked(taskq_t *tq, taskqid_t id)
{
	taskq_ent_t *ent;

	MUTEX_ASSERT_LOCKED(&tq->tq_lock);
	TAILQ_FOREACH(ent, &tq->tq_entries, tqent_link) {
		if (ent->tqent_id == id)
			return (ent);
	}
	return (NULL);
}

static taskqid_t
zfs_taskq_next_id_locked(taskq_t *tq)
{

	MUTEX_ASSERT_LOCKED(&tq->tq_lock);
	if (++tq->tq_next_id == TASKQID_INVALID)
		++tq->tq_next_id;
	return (tq->tq_next_id);
}

static void
zfs_taskq_remove_locked(taskq_t *tq, taskq_ent_t *ent)
{

	MUTEX_ASSERT_LOCKED(&tq->tq_lock);
	TAILQ_REMOVE(&tq->tq_entries, ent, tqent_link);
	VERIFY3U(tq->tq_ndynamic, >, 0);
	tq->tq_ndynamic--;
	if (ent->tqent_state == ZFS_TQENT_DELAYED ||
	    ent->tqent_state == ZFS_TQENT_CANCELLED_DELAYED) {
		VERIFY3U(tq->tq_ndelayed, >, 0);
		tq->tq_ndelayed--;
		wakeup(&tq->tq_ndelayed);
	}
	ent->tqent_state = ZFS_TQENT_IDLE;
	wakeup(tq);
}

static void
zfs_taskq_run(void *arg)
{
	taskq_ent_t *ent = arg;
	taskq_t *tq = ent->tqent_taskq;
	task_func_t *func;
	void *func_arg;

	mtx_enter(&tq->tq_lock);
	while (tq->tq_suspended &&
	    ent->tqent_state == ZFS_TQENT_QUEUED)
		msleep_nsec(&tq->tq_suspended, &tq->tq_lock, PWAIT,
		    "zfstqsp", INFSLP);

	if (ent->tqent_state == ZFS_TQENT_CANCELLED_QUEUED) {
		zfs_taskq_remove_locked(tq, ent);
		mtx_leave(&tq->tq_lock);
		kmem_free(ent, sizeof (*ent));
		return;
	}

	VERIFY3U(ent->tqent_state, ==, ZFS_TQENT_QUEUED);
	ent->tqent_state = ZFS_TQENT_RUNNING;
	tq->tq_active++;
	func = ent->tqent_func;
	func_arg = ent->tqent_arg;
	mtx_leave(&tq->tq_lock);

	func(func_arg);

	mtx_enter(&tq->tq_lock);
	VERIFY3U(tq->tq_active, >, 0);
	tq->tq_active--;
	wakeup(&tq->tq_active);
	zfs_taskq_remove_locked(tq, ent);
	mtx_leave(&tq->tq_lock);
	kmem_free(ent, sizeof (*ent));
}

static void
zfs_taskq_run_ent(void *arg)
{
	taskq_ent_t *ent = arg;
	taskq_t *tq = ent->tqent_taskq;
	task_func_t *func = ent->tqent_func;
	void *func_arg = ent->tqent_arg;

	/* The callback may free the object containing ent. */
	mtx_enter(&tq->tq_lock);
	while (tq->tq_suspended)
		msleep_nsec(&tq->tq_suspended, &tq->tq_lock, PWAIT,
		    "zfstqsp", INFSLP);
	tq->tq_active++;
	mtx_leave(&tq->tq_lock);

	func(func_arg);

	mtx_enter(&tq->tq_lock);
	VERIFY3U(tq->tq_active, >, 0);
	tq->tq_active--;
	wakeup(&tq->tq_active);
	mtx_leave(&tq->tq_lock);
}

static void
zfs_taskq_expire(void *arg)
{
	taskq_ent_t *ent = arg;
	taskq_t *tq = ent->tqent_taskq;
	int queued;

	mtx_enter(&tq->tq_lock);
	if (ent->tqent_state == ZFS_TQENT_CANCELLED_DELAYED) {
		zfs_taskq_remove_locked(tq, ent);
		mtx_leave(&tq->tq_lock);
		kmem_free(ent, sizeof (*ent));
		return;
	}

	VERIFY3U(ent->tqent_state, ==, ZFS_TQENT_DELAYED);
	VERIFY3U(tq->tq_ndelayed, >, 0);
	tq->tq_ndelayed--;
	wakeup(&tq->tq_ndelayed);
	ent->tqent_state = ZFS_TQENT_QUEUED;
	if (ent->tqent_flags & TQ_FRONT)
		queued = task_add_front(tq->tq_native, &ent->tqent_task);
	else
		queued = task_add(tq->tq_native, &ent->tqent_task);
	VERIFY(queued);
	mtx_leave(&tq->tq_lock);
}

static taskqid_t
zfs_taskq_dispatch_common(taskq_t *tq, task_func_t func, void *arg,
    uint_t flags, clock_t expire_time)
{
	taskq_ent_t *ent;
	clock_t delay;
	taskqid_t id;
	int kmflags, queued;

	if (flags & (TQ_NOSLEEP | TQ_NOALLOC | TQ_NOQUEUE))
		kmflags = KM_NOSLEEP;
	else
		kmflags = KM_SLEEP;
	ent = kmem_zalloc(sizeof (*ent), kmflags);
	if (ent == NULL)
		return (TASKQID_INVALID);

	ent->tqent_func = func;
	ent->tqent_arg = arg;
	ent->tqent_taskq = tq;
	ent->tqent_flags = flags;
	task_set(&ent->tqent_task, zfs_taskq_run, ent);
	timeout_set_proc(&ent->tqent_timeout, zfs_taskq_expire, ent);

	mtx_enter(&tq->tq_lock);
	if (tq->tq_destroying ||
	    ((flags & TQ_NOQUEUE) && tq->tq_active >= tq->tq_nthreads)) {
		mtx_leave(&tq->tq_lock);
		kmem_free(ent, sizeof (*ent));
		return (TASKQID_INVALID);
	}

	id = ent->tqent_id = zfs_taskq_next_id_locked(tq);
	TAILQ_INSERT_TAIL(&tq->tq_entries, ent, tqent_link);
	tq->tq_ndynamic++;
	delay = expire_time - ddi_get_lbolt();
	if (expire_time != 0 && delay > 0) {
		ent->tqent_state = ZFS_TQENT_DELAYED;
		tq->tq_ndelayed++;
		timeout_add(&ent->tqent_timeout,
		    delay > INT_MAX ? INT_MAX : (int)delay);
	} else {
		ent->tqent_state = ZFS_TQENT_QUEUED;
		if (flags & TQ_FRONT)
			queued = task_add_front(tq->tq_native,
			    &ent->tqent_task);
		else
			queued = task_add(tq->tq_native, &ent->tqent_task);
		VERIFY(queued);
	}
	mtx_leave(&tq->tq_lock);
	return (id);
}

taskq_t *
zfs_taskq_create(const char *name, int nthreads, pri_t pri,
    int minalloc, int maxalloc, uint_t flags)
{
	taskq_t *tq;

	(void)pri;
	(void)minalloc;
	(void)maxalloc;
	if (flags & TASKQ_THREADS_CPU_PCT)
		nthreads = MAX((ncpus * nthreads) / 100, 1);
	nthreads = MAX(nthreads, 1);

	tq = kmem_zalloc(sizeof (*tq), KM_SLEEP);
	strlcpy(tq->tq_name, name, sizeof (tq->tq_name));
	tq->tq_nthreads = nthreads;
	TAILQ_INIT(&tq->tq_entries);
	mtx_init_flags(&tq->tq_lock, IPL_HIGH, tq->tq_name, MTX_DUPOK);
	tq->tq_native = taskq_create(tq->tq_name, nthreads, IPL_HIGH,
	    TASKQ_MPSAFE);
	if (tq->tq_native == NULL) {
		kmem_free(tq, sizeof (*tq));
		return (NULL);
	}

	mtx_enter(&zfs_taskqs_lock);
	LIST_INSERT_HEAD(&zfs_taskqs, tq, tq_link);
	mtx_leave(&zfs_taskqs_lock);
	return (tq);
}

taskq_t *
zfs_taskq_create_proc(const char *name, int nthreads, pri_t pri,
    int minalloc, int maxalloc, proc_t *proc, uint_t flags)
{

	(void)proc;
	return (zfs_taskq_create(name, nthreads, pri, minalloc, maxalloc,
	    flags));
}

taskq_t *
zfs_taskq_create_sysdc(const char *name, int nthreads, int minalloc,
    int maxalloc, proc_t *proc, uint_t dc, uint_t flags)
{

	(void)dc;
	return (zfs_taskq_create_proc(name, nthreads, maxclsyspri, minalloc,
	    maxalloc, proc, flags));
}

taskq_t *
zfs_taskq_create_instance(const char *name, int instance, int nthreads,
    pri_t pri, int minalloc, int maxalloc, uint_t flags)
{
	char fullname[TASKQ_NAMELEN + 1];

	(void)snprintf(fullname, sizeof (fullname), "%s_%d", name, instance);
	return (zfs_taskq_create(fullname, nthreads, pri, minalloc, maxalloc,
	    flags));
}

typedef struct zfs_taskq_sync_arg {
	kthread_t	*tqs_thread;
	kcondvar_t	 tqs_cv;
	kmutex_t	 tqs_lock;
	int		 tqs_ready;
} zfs_taskq_sync_arg_t;

static void
zfs_taskq_sync_assign(void *arg)
{
	zfs_taskq_sync_arg_t *sync = arg;

	mutex_enter(&sync->tqs_lock);
	sync->tqs_thread = curthread;
	sync->tqs_ready = 1;
	cv_signal(&sync->tqs_cv);
	while (sync->tqs_ready == 1)
		cv_wait(&sync->tqs_cv, &sync->tqs_lock);
	mutex_exit(&sync->tqs_lock);
}

taskq_t *
zfs_taskq_create_synced(const char *name, int nthreads, pri_t pri,
    int minalloc, int maxalloc, uint_t flags, kthread_t ***threadsp)
{
	zfs_taskq_sync_arg_t *sync;
	kthread_t **threads;
	taskq_t *tq;
	int i;

	flags &= ~(TASKQ_DYNAMIC | TASKQ_THREADS_CPU_PCT | TASKQ_DC_BATCH);
	tq = zfs_taskq_create(name, nthreads, pri, minalloc, maxalloc,
	    flags | TASKQ_PREPOPULATE);
	if (tq == NULL)
		return (NULL);

	sync = kmem_zalloc(sizeof (*sync) * nthreads, KM_SLEEP);
	threads = kmem_zalloc(sizeof (*threads) * nthreads, KM_SLEEP);
	for (i = 0; i < nthreads; i++) {
		cv_init(&sync[i].tqs_cv, NULL, CV_DEFAULT, NULL);
		mutex_init(&sync[i].tqs_lock, NULL, MUTEX_DEFAULT, NULL);
		VERIFY(zfs_taskq_dispatch(tq, zfs_taskq_sync_assign, &sync[i],
		    TQ_FRONT) != TASKQID_INVALID);
	}

	for (i = 0; i < nthreads; i++) {
		mutex_enter(&sync[i].tqs_lock);
		while (sync[i].tqs_ready == 0)
			cv_wait(&sync[i].tqs_cv, &sync[i].tqs_lock);
		mutex_exit(&sync[i].tqs_lock);
	}
	for (i = 0; i < nthreads; i++) {
		mutex_enter(&sync[i].tqs_lock);
		sync[i].tqs_ready = 2;
		cv_broadcast(&sync[i].tqs_cv);
		mutex_exit(&sync[i].tqs_lock);
	}
	zfs_taskq_wait(tq);

	for (i = 0; i < nthreads; i++) {
		threads[i] = sync[i].tqs_thread;
		mutex_destroy(&sync[i].tqs_lock);
		cv_destroy(&sync[i].tqs_cv);
	}
	kmem_free(sync, sizeof (*sync) * nthreads);
	*threadsp = threads;
	return (tq);
}

void
zfs_taskq_destroy(taskq_t *tq)
{

	mtx_enter(&tq->tq_lock);
	tq->tq_destroying = 1;
	tq->tq_suspended = 0;
	wakeup(&tq->tq_suspended);
	mtx_leave(&tq->tq_lock);
	zfs_taskq_wait(tq);

	mtx_enter(&zfs_taskqs_lock);
	LIST_REMOVE(tq, tq_link);
	mtx_leave(&zfs_taskqs_lock);
	taskq_destroy(tq->tq_native);
	kmem_free(tq, sizeof (*tq));
}

taskqid_t
zfs_taskq_dispatch(taskq_t *tq, task_func_t func, void *arg, uint_t flags)
{

	return (zfs_taskq_dispatch_common(tq, func, arg, flags, 0));
}

taskqid_t
zfs_taskq_dispatch_delay(taskq_t *tq, task_func_t func, void *arg,
    uint_t flags, clock_t expire_time)
{

	return (zfs_taskq_dispatch_common(tq, func, arg, flags, expire_time));
}

void
zfs_taskq_init_ent(taskq_ent_t *ent)
{

	memset(ent, 0, sizeof (*ent));
	ent->tqent_flags = ZFS_TQENT_PREALLOC;
	task_set(&ent->tqent_task, zfs_taskq_run_ent, ent);
}

void
zfs_taskq_dispatch_ent(taskq_t *tq, task_func_t func, void *arg,
    uint_t flags, taskq_ent_t *ent)
{
	int queued;

	VERIFY(ent->tqent_flags & ZFS_TQENT_PREALLOC);
	VERIFY(zfs_taskq_empty_ent(ent));
	ent->tqent_func = func;
	ent->tqent_arg = arg;
	ent->tqent_taskq = tq;
	task_set(&ent->tqent_task, zfs_taskq_run_ent, ent);
	if (flags & TQ_FRONT)
		queued = task_add_front(tq->tq_native, &ent->tqent_task);
	else
		queued = task_add(tq->tq_native, &ent->tqent_task);
	VERIFY(queued);
}

int
zfs_taskq_empty_ent(taskq_ent_t *ent)
{

	return (!task_pending(&ent->tqent_task));
}

void
zfs_taskq_wait(taskq_t *tq)
{

	mtx_enter(&tq->tq_lock);
	while (tq->tq_ndelayed != 0)
		msleep_nsec(&tq->tq_ndelayed, &tq->tq_lock, PWAIT,
		    "zfstqwt", INFSLP);
	mtx_leave(&tq->tq_lock);
	taskq_barrier(tq->tq_native);
}

void
zfs_taskq_wait_id(taskq_t *tq, taskqid_t id)
{

	mtx_enter(&tq->tq_lock);
	while (zfs_taskq_lookup_locked(tq, id) != NULL)
		msleep_nsec(tq, &tq->tq_lock, PWAIT, "zfstqid", INFSLP);
	mtx_leave(&tq->tq_lock);
}

void
zfs_taskq_wait_outstanding(taskq_t *tq, taskqid_t id)
{
	taskq_ent_t *ent;
	int found;

	if (id == 0) {
		zfs_taskq_wait(tq);
		return;
	}

	mtx_enter(&tq->tq_lock);
	do {
		found = 0;
		TAILQ_FOREACH(ent, &tq->tq_entries, tqent_link) {
			if (ent->tqent_id <= id) {
				found = 1;
				break;
			}
		}
		if (found)
			msleep_nsec(tq, &tq->tq_lock, PWAIT, "zfstqos",
			    INFSLP);
	} while (found);
	mtx_leave(&tq->tq_lock);
}

int
zfs_taskq_cancel_id(taskq_t *tq, taskqid_t id, boolean_t wait)
{
	taskq_ent_t *ent;
	int removed = 0;

	mtx_enter(&tq->tq_lock);
	ent = zfs_taskq_lookup_locked(tq, id);
	if (ent == NULL) {
		mtx_leave(&tq->tq_lock);
		return (ENOENT);
	}

	switch (ent->tqent_state) {
	case ZFS_TQENT_DELAYED:
		if (timeout_del(&ent->tqent_timeout)) {
			zfs_taskq_remove_locked(tq, ent);
			removed = 1;
		} else {
			ent->tqent_state = ZFS_TQENT_CANCELLED_DELAYED;
		}
		break;
	case ZFS_TQENT_QUEUED:
		if (task_del(tq->tq_native, &ent->tqent_task)) {
			zfs_taskq_remove_locked(tq, ent);
			removed = 1;
		} else {
			ent->tqent_state = ZFS_TQENT_CANCELLED_QUEUED;
			wakeup(&tq->tq_suspended);
		}
		break;
	case ZFS_TQENT_CANCELLED_DELAYED:
	case ZFS_TQENT_CANCELLED_QUEUED:
		break;
	case ZFS_TQENT_RUNNING:
		if (!wait) {
			mtx_leave(&tq->tq_lock);
			return (EBUSY);
		}
		while (zfs_taskq_lookup_locked(tq, id) != NULL)
			msleep_nsec(tq, &tq->tq_lock, PWAIT, "zfstqcn",
			    INFSLP);
		mtx_leave(&tq->tq_lock);
		return (ENOENT);
	default:
		panic("invalid taskq entry state %u", ent->tqent_state);
	}

	if (!removed && wait) {
		while (zfs_taskq_lookup_locked(tq, id) != NULL)
			msleep_nsec(tq, &tq->tq_lock, PWAIT, "zfstqcn",
			    INFSLP);
	}
	mtx_leave(&tq->tq_lock);
	if (removed)
		kmem_free(ent, sizeof (*ent));
	return (0);
}

int
zfs_taskq_member(taskq_t *tq, kthread_t *thread)
{

	return (taskq_is_member(tq->tq_native, thread));
}

taskq_t *
zfs_taskq_of_curthread(void)
{
	taskq_t *tq, *found = NULL;

	mtx_enter(&zfs_taskqs_lock);
	LIST_FOREACH(tq, &zfs_taskqs, tq_link) {
		if (taskq_is_member(tq->tq_native, curthread)) {
			found = tq;
			break;
		}
	}
	mtx_leave(&zfs_taskqs_lock);
	return (found);
}

void
zfs_taskq_suspend(taskq_t *tq)
{

	mtx_enter(&tq->tq_lock);
	tq->tq_suspended = 1;
	while (tq->tq_active != 0)
		msleep_nsec(&tq->tq_active, &tq->tq_lock, PWAIT,
		    "zfstqsu", INFSLP);
	mtx_leave(&tq->tq_lock);
}

int
zfs_taskq_suspended(taskq_t *tq)
{
	int suspended;

	mtx_enter(&tq->tq_lock);
	suspended = tq->tq_suspended;
	mtx_leave(&tq->tq_lock);
	return (suspended);
}

void
zfs_taskq_resume(taskq_t *tq)
{

	mtx_enter(&tq->tq_lock);
	tq->tq_suspended = 0;
	wakeup(&tq->tq_suspended);
	mtx_leave(&tq->tq_lock);
}

void
nulltask(void *arg)
{

	(void)arg;
}

void
system_taskq_init(void)
{

	VERIFY3P(system_taskq, ==, NULL);
	VERIFY3P(system_delay_taskq, ==, NULL);
	system_taskq = zfs_taskq_create("system_taskq", MAX(ncpus, 1),
	    minclsyspri, 0, 0, 0);
	system_delay_taskq = zfs_taskq_create("system_delay_taskq",
	    MAX(ncpus, 1), minclsyspri, 0, 0, 0);
	VERIFY3P(system_taskq, !=, NULL);
	VERIFY3P(system_delay_taskq, !=, NULL);
}

void
system_taskq_fini(void)
{

	if (system_delay_taskq != NULL) {
		zfs_taskq_destroy(system_delay_taskq);
		system_delay_taskq = NULL;
	}
	if (system_taskq != NULL) {
		zfs_taskq_destroy(system_taskq);
		system_taskq = NULL;
	}
}
