// SPDX-License-Identifier: BSD-2-Clause
/*
 * OpenZFS thread-specific data for OpenBSD.
 *
 * Keep the data outside struct proc, as the Linux SPL does outside
 * task_struct.  Values are indexed by an allocated key and the address of
 * the owning struct proc.  The native exit path removes a thread's entries
 * before the proc can be recycled.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/queue.h>
#include <sys/proc.h>
#include <sys/rwlock.h>
#include <sys/kmem.h>
#include <sys/tsd.h>

#define	TSD_HASH_TABLE_SIZE	(1U << TSD_HASH_TABLE_BITS_DEFAULT)
#define	TSD_HASH_TABLE_MASK	(TSD_HASH_TABLE_SIZE - 1)

typedef struct tsd_entry {
	LIST_ENTRY(tsd_entry)	te_hash;
	LIST_ENTRY(tsd_entry)	te_work;
	uint_t			te_key;
	struct proc		*te_thread;
	dtor_func_t		te_dtor;
	void			*te_value;
} tsd_entry_t;

typedef struct tsd_key {
	LIST_ENTRY(tsd_key)	tk_link;
	uint_t			tk_key;
	dtor_func_t		tk_dtor;
} tsd_key_t;

LIST_HEAD(tsd_bucket, tsd_entry);
LIST_HEAD(tsd_key_head, tsd_key);
LIST_HEAD(tsd_work_head, tsd_entry);

static struct tsd_bucket tsd_hash_table[TSD_HASH_TABLE_SIZE];
static struct tsd_key_head tsd_keys = LIST_HEAD_INITIALIZER(tsd_keys);
static struct rwlock tsd_lock = RWLOCK_INITIALIZER("zfstsd");
static uint_t tsd_next_key = 1;

static uint_t
tsd_hash(uint_t key, const struct proc *thread)
{
	uintptr_t value = (uintptr_t)thread >> 3;

	value ^= value >> TSD_HASH_TABLE_BITS_DEFAULT;
	value ^= (uintptr_t)key * 2654435761U;
	return ((uint_t)value & TSD_HASH_TABLE_MASK);
}

static tsd_key_t *
tsd_key_find(uint_t key)
{
	tsd_key_t *tk;

	LIST_FOREACH(tk, &tsd_keys, tk_link) {
		if (tk->tk_key == key)
			return (tk);
	}
	return (NULL);
}

static tsd_entry_t *
tsd_entry_find(uint_t key, struct proc *thread)
{
	struct tsd_bucket *bucket;
	tsd_entry_t *entry;

	bucket = &tsd_hash_table[tsd_hash(key, thread)];
	LIST_FOREACH(entry, bucket, te_hash) {
		if (entry->te_key == key && entry->te_thread == thread)
			return (entry);
	}
	return (NULL);
}

static void
tsd_work_run(struct tsd_work_head *work, boolean_t destroy)
{
	tsd_entry_t *entry;

	while ((entry = LIST_FIRST(work)) != NULL) {
		LIST_REMOVE(entry, te_work);
		if (destroy && entry->te_dtor != NULL)
			entry->te_dtor(entry->te_value);
		kmem_free(entry, sizeof (*entry));
	}
}

int
tsd_set(uint_t key, void *value)
{
	struct tsd_work_head work = LIST_HEAD_INITIALIZER(work);
	tsd_entry_t *entry, *new_entry = NULL;
	tsd_key_t *tk;

	if (key == 0 || key > TSD_KEYS_MAX)
		return (EINVAL);

	rw_enter_write(&tsd_lock);
	tk = tsd_key_find(key);
	if (tk == NULL) {
		rw_exit_write(&tsd_lock);
		return (EINVAL);
	}

	entry = tsd_entry_find(key, curproc);
	if (entry != NULL) {
		if (value == NULL) {
			LIST_REMOVE(entry, te_hash);
			LIST_INSERT_HEAD(&work, entry, te_work);
		} else {
			entry->te_value = value;
		}
		rw_exit_write(&tsd_lock);
		tsd_work_run(&work, B_FALSE);
		return (0);
	}
	rw_exit_write(&tsd_lock);

	if (value == NULL)
		return (0);

	/* Allocate outside the lock; the kernel allocator may sleep. */
	new_entry = kmem_zalloc(sizeof (*new_entry), KM_SLEEP);
	rw_enter_write(&tsd_lock);
	tk = tsd_key_find(key);
	if (tk == NULL) {
		rw_exit_write(&tsd_lock);
		kmem_free(new_entry, sizeof (*new_entry));
		return (EINVAL);
	}
	entry = tsd_entry_find(key, curproc);
	if (entry != NULL) {
		entry->te_value = value;
	} else {
		new_entry->te_key = key;
		new_entry->te_thread = curproc;
		new_entry->te_dtor = tk->tk_dtor;
		new_entry->te_value = value;
		LIST_INSERT_HEAD(&tsd_hash_table[tsd_hash(key, curproc)],
		    new_entry, te_hash);
		new_entry = NULL;
	}
	rw_exit_write(&tsd_lock);
	if (new_entry != NULL)
		kmem_free(new_entry, sizeof (*new_entry));
	return (0);
}

void *
tsd_get_by_thread(uint_t key, struct proc *thread)
{
	tsd_entry_t *entry;
	void *value = NULL;

	if (key == 0 || key > TSD_KEYS_MAX || thread == NULL)
		return (NULL);

	rw_enter_read(&tsd_lock);
	entry = tsd_entry_find(key, thread);
	if (entry != NULL)
		value = entry->te_value;
	rw_exit_read(&tsd_lock);
	return (value);
}

void *
tsd_get(uint_t key)
{

	return (tsd_get_by_thread(key, curproc));
}

void
tsd_create(uint_t *keyp, dtor_func_t dtor)
{
	tsd_key_t *tk;
	uint_t key, checked;

	KASSERT(keyp != NULL);
	if (*keyp != 0)
		return;

	tk = kmem_zalloc(sizeof (*tk), KM_SLEEP);
	rw_enter_write(&tsd_lock);
	if (*keyp != 0) {
		rw_exit_write(&tsd_lock);
		kmem_free(tk, sizeof (*tk));
		return;
	}
	for (checked = 0; checked < TSD_KEYS_MAX; checked++) {
		key = tsd_next_key++;
		if (tsd_next_key > TSD_KEYS_MAX)
			tsd_next_key = 1;
		if (tsd_key_find(key) == NULL)
			break;
	}
	KASSERT(checked < TSD_KEYS_MAX);
	tk->tk_key = key;
	tk->tk_dtor = dtor;
	LIST_INSERT_HEAD(&tsd_keys, tk, tk_link);
	*keyp = key;
	rw_exit_write(&tsd_lock);
}

void
tsd_destroy(uint_t *keyp)
{
	struct tsd_work_head work = LIST_HEAD_INITIALIZER(work);
	tsd_entry_t *entry, *next;
	tsd_key_t *tk;
	uint_t i, key;

	KASSERT(keyp != NULL);
	key = *keyp;
	if (key == 0)
		return;

	rw_enter_write(&tsd_lock);
	tk = tsd_key_find(key);
	if (tk == NULL) {
		rw_exit_write(&tsd_lock);
		return;
	}
	for (i = 0; i < TSD_HASH_TABLE_SIZE; i++) {
		for (entry = LIST_FIRST(&tsd_hash_table[i]); entry != NULL;
		    entry = next) {
			next = LIST_NEXT(entry, te_hash);
			if (entry->te_key != key)
				continue;
			LIST_REMOVE(entry, te_hash);
			LIST_INSERT_HEAD(&work, entry, te_work);
		}
	}
	LIST_REMOVE(tk, tk_link);
	*keyp = 0;
	rw_exit_write(&tsd_lock);

	tsd_work_run(&work, B_TRUE);
	kmem_free(tk, sizeof (*tk));
}

void
zfs_tsd_exit(struct proc *thread)
{
	struct tsd_work_head work = LIST_HEAD_INITIALIZER(work);
	tsd_entry_t *entry, *next;
	uint_t i;

	KASSERT(thread != NULL);
	rw_enter_write(&tsd_lock);
	for (i = 0; i < TSD_HASH_TABLE_SIZE; i++) {
		for (entry = LIST_FIRST(&tsd_hash_table[i]); entry != NULL;
		    entry = next) {
			next = LIST_NEXT(entry, te_hash);
			if (entry->te_thread != thread)
				continue;
			LIST_REMOVE(entry, te_hash);
			LIST_INSERT_HEAD(&work, entry, te_work);
		}
	}
	rw_exit_write(&tsd_lock);

	tsd_work_run(&work, B_TRUE);
}

void
tsd_exit(void)
{

	zfs_tsd_exit(curproc);
}
