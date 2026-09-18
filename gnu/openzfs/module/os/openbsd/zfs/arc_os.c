// SPDX-License-Identifier: CDDL-1.0
/* OpenBSD memory accounting and pressure policy for the ARC. */

#include <sys/zfs_context.h>
#include <sys/arc.h>
#include <sys/arc_impl.h>

#include <uvm/uvm_extern.h>

typedef struct arc_pressure_stats {
	kstat_named_t	notify;
	kstat_named_t	req_bytes;
	kstat_named_t	last_pages;
	kstat_named_t	max_pages;
	kstat_named_t	wake_ok;
	kstat_named_t	wake_miss;
	kstat_named_t	processed;
	kstat_named_t	pending_b;
	kstat_named_t	arc_size;
	kstat_named_t	arc_target;
	kstat_named_t	avail_bytes;
	kstat_named_t	free_bytes;
	kstat_named_t	evict_bytes;
	kstat_named_t	no_grow;
	kstat_named_t	first_notify;
	kstat_named_t	first_process;
	kstat_named_t	first_evict;
	kstat_named_t	evict_runs;
	kstat_named_t	evict_rounds;
	kstat_named_t	evict_last_b;
	kstat_named_t	evict_last_ns;
	kstat_named_t	evict_max_ns;
} arc_pressure_stats_t;

static arc_pressure_stats_t arc_pressure_stats = {
	{ "notify",		KSTAT_DATA_UINT64 },
	{ "req_bytes",		KSTAT_DATA_UINT64 },
	{ "last_pages",		KSTAT_DATA_UINT64 },
	{ "max_pages",		KSTAT_DATA_UINT64 },
	{ "wake_ok",		KSTAT_DATA_UINT64 },
	{ "wake_miss",		KSTAT_DATA_UINT64 },
	{ "processed",		KSTAT_DATA_UINT64 },
	{ "pending_b",		KSTAT_DATA_UINT64 },
	{ "arc_size",		KSTAT_DATA_UINT64 },
	{ "arc_target",		KSTAT_DATA_UINT64 },
	{ "avail_bytes",	KSTAT_DATA_INT64 },
	{ "free_bytes",		KSTAT_DATA_UINT64 },
	{ "evict_bytes",	KSTAT_DATA_UINT64 },
	{ "no_grow",		KSTAT_DATA_UINT64 },
	{ "first_notify",	KSTAT_DATA_UINT64 },
	{ "first_process",	KSTAT_DATA_UINT64 },
	{ "first_evict",	KSTAT_DATA_UINT64 },
	{ "evict_runs",		KSTAT_DATA_UINT64 },
	{ "evict_rounds",	KSTAT_DATA_UINT64 },
	{ "evict_last_b",	KSTAT_DATA_UINT64 },
	{ "evict_last_ns",	KSTAT_DATA_UINT64 },
	{ "evict_max_ns",	KSTAT_DATA_UINT64 },
};

static kstat_t *arc_pressure_ksp;
static volatile uint64_t arc_pressure_notify;
static volatile uint64_t arc_pressure_req_bytes;
static volatile uint64_t arc_pressure_last_pages;
static volatile uint64_t arc_pressure_max_pages;
static volatile uint64_t arc_pressure_wake_ok;
static volatile uint64_t arc_pressure_wake_miss;
static volatile uint64_t arc_pressure_process_count;
static volatile uint64_t arc_pressure_first_notify;
static volatile uint64_t arc_pressure_first_process;
static volatile uint64_t arc_pressure_first_evict;
static volatile uint64_t arc_pressure_evict_runs;
static volatile uint64_t arc_pressure_evict_rounds;
static volatile uint64_t arc_pressure_evict_last_bytes;
static volatile uint64_t arc_pressure_evict_last_ns;
static volatile uint64_t arc_pressure_evict_max_ns;

uint64_t
arc_default_max(uint64_t min, uint64_t allmem)
{
	return (MAX(min, allmem / 2));
}

uint64_t
arc_all_memory(void)
{
	return ((uint64_t)uvmexp.npages << PAGE_SHIFT);
}

uint64_t
arc_free_memory(void)
{
	return ((uint64_t)atomic_load_sint(&uvmexp.free) << PAGE_SHIFT);
}

int64_t
arc_available_memory(void)
{
	int64_t free_pages;

	free_pages = atomic_load_sint(&uvmexp.free);
	free_pages -= uvmexp.freetarg;
	return (free_pages * PAGE_SIZE);
}

int
arc_memory_throttle(spa_t *spa, uint64_t reserve, uint64_t txg)
{
	(void) spa;
	(void) reserve;
	(void) txg;

	return (0);
}

static int
arc_pressure_kstat_update(kstat_t *ksp, int rw)
{
	arc_pressure_stats_t *stats = ksp->ks_data;

	if (rw == KSTAT_WRITE)
		return (EACCES);

	stats->notify.value.ui64 = atomic_load_64(&arc_pressure_notify);
	stats->req_bytes.value.ui64 =
	    atomic_load_64(&arc_pressure_req_bytes);
	stats->last_pages.value.ui64 =
	    atomic_load_64(&arc_pressure_last_pages);
	stats->max_pages.value.ui64 =
	    atomic_load_64(&arc_pressure_max_pages);
	stats->wake_ok.value.ui64 = atomic_load_64(&arc_pressure_wake_ok);
	stats->wake_miss.value.ui64 =
	    atomic_load_64(&arc_pressure_wake_miss);
	stats->processed.value.ui64 =
	    atomic_load_64(&arc_pressure_process_count);
	stats->pending_b.value.ui64 = arc_reclaim_pending();
	/*
	 * Keep this diagnostic safe to read during severe pressure.  The
	 * upper bound is approximate, but unlike aggsum_value() it takes no
	 * aggregate or per-CPU bucket locks.
	 */
	stats->arc_size.value.ui64 =
	    aggsum_upper_bound(&arc_sums.arcstat_size);
	stats->arc_target.value.ui64 = atomic_load_64(&arc_c);
	stats->avail_bytes.value.i64 = arc_available_memory();
	stats->free_bytes.value.ui64 = arc_free_memory();
	stats->evict_bytes.value.ui64 = arc_evict_bytes();
	stats->no_grow.value.ui64 = arc_no_grow;
	stats->first_notify.value.ui64 =
	    atomic_load_64(&arc_pressure_first_notify);
	stats->first_process.value.ui64 =
	    atomic_load_64(&arc_pressure_first_process);
	stats->first_evict.value.ui64 =
	    atomic_load_64(&arc_pressure_first_evict);
	stats->evict_runs.value.ui64 =
	    atomic_load_64(&arc_pressure_evict_runs);
	stats->evict_rounds.value.ui64 =
	    atomic_load_64(&arc_pressure_evict_rounds);
	stats->evict_last_b.value.ui64 =
	    atomic_load_64(&arc_pressure_evict_last_bytes);
	stats->evict_last_ns.value.ui64 =
	    atomic_load_64(&arc_pressure_evict_last_ns);
	stats->evict_max_ns.value.ui64 =
	    atomic_load_64(&arc_pressure_evict_max_ns);
	return (0);
}

static void
arc_lowmem(void *arg, int shortage)
{
	uint64_t max_pages, now, pages = shortage;

	(void) arg;
	ASSERT3S(shortage, >, 0);

	now = gethrtime();
	(void) atomic_cas_64(&arc_pressure_first_notify, 0, now);
	atomic_inc_64(&arc_pressure_notify);
	atomic_add_64(&arc_pressure_req_bytes, pages << PAGE_SHIFT);
	atomic_store_64(&arc_pressure_last_pages, pages);
	max_pages = atomic_load_64(&arc_pressure_max_pages);
	while (max_pages < pages) {
		uint64_t observed;

		observed = atomic_cas_64(&arc_pressure_max_pages, max_pages,
		    pages);
		if (observed == max_pages)
			break;
		max_pages = observed;
	}

	/* Reclamation itself is performed by the common ARC worker threads. */
	if (arc_reclaim_async(pages << PAGE_SHIFT))
		atomic_inc_64(&arc_pressure_wake_ok);
	else
		atomic_inc_64(&arc_pressure_wake_miss);
}

void
arc_lowmem_init(void)
{
	atomic_store_64(&arc_pressure_notify, 0);
	atomic_store_64(&arc_pressure_req_bytes, 0);
	atomic_store_64(&arc_pressure_last_pages, 0);
	atomic_store_64(&arc_pressure_max_pages, 0);
	atomic_store_64(&arc_pressure_wake_ok, 0);
	atomic_store_64(&arc_pressure_wake_miss, 0);
	atomic_store_64(&arc_pressure_process_count, 0);
	atomic_store_64(&arc_pressure_first_notify, 0);
	atomic_store_64(&arc_pressure_first_process, 0);
	atomic_store_64(&arc_pressure_first_evict, 0);
	atomic_store_64(&arc_pressure_evict_runs, 0);
	atomic_store_64(&arc_pressure_evict_rounds, 0);
	atomic_store_64(&arc_pressure_evict_last_bytes, 0);
	atomic_store_64(&arc_pressure_evict_last_ns, 0);
	atomic_store_64(&arc_pressure_evict_max_ns, 0);

	uvm_reclaim_register(arc_lowmem, NULL);
}

void
arc_lowmem_fini(void)
{
	uvm_reclaim_unregister(arc_lowmem, NULL);
}

void
arc_pressure_init(void)
{
	arc_pressure_ksp = kstat_create("zfs", 0, "arcpressure", "misc",
	    KSTAT_TYPE_NAMED,
	    sizeof (arc_pressure_stats) / sizeof (kstat_named_t),
	    KSTAT_FLAG_VIRTUAL);
	if (arc_pressure_ksp != NULL) {
		arc_pressure_ksp->ks_data = &arc_pressure_stats;
		arc_pressure_ksp->ks_update = arc_pressure_kstat_update;
		kstat_install(arc_pressure_ksp);
	}
}

void
arc_pressure_fini(void)
{
	if (arc_pressure_ksp != NULL) {
		kstat_delete(arc_pressure_ksp);
		arc_pressure_ksp = NULL;
	}
}

void
arc_pressure_processed(void)
{
	(void) atomic_cas_64(&arc_pressure_first_process, 0, gethrtime());
	atomic_inc_64(&arc_pressure_process_count);
}

void
arc_pressure_evicted(uint64_t bytes, hrtime_t elapsed)
{
	uint64_t maximum;

	atomic_inc_64(&arc_pressure_evict_runs);
	atomic_store_64(&arc_pressure_evict_last_bytes, bytes);
	atomic_store_64(&arc_pressure_evict_last_ns, elapsed);
	maximum = atomic_load_64(&arc_pressure_evict_max_ns);
	while (maximum < elapsed) {
		uint64_t observed;

		observed = atomic_cas_64(&arc_pressure_evict_max_ns, maximum,
		    elapsed);
		if (observed == maximum)
			break;
		maximum = observed;
	}
	if (bytes != 0 && atomic_load_64(&arc_pressure_first_notify) != 0) {
		(void) atomic_cas_64(&arc_pressure_first_evict, 0,
		    gethrtime());
	}
}

void
arc_pressure_evict_round(void)
{
	atomic_inc_64(&arc_pressure_evict_rounds);
}

void
arc_register_hotplug(void)
{
}

void
arc_unregister_hotplug(void)
{
}
