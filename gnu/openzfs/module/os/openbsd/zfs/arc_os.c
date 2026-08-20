// SPDX-License-Identifier: CDDL-1.0
/* OpenBSD memory accounting and pressure policy for the ARC. */

#include <sys/zfs_context.h>
#include <sys/arc.h>
#include <sys/arc_impl.h>

#include <uvm/uvm_extern.h>

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

static void
arc_lowmem(void *arg, int shortage)
{
	(void) arg;
	ASSERT3S(shortage, >, 0);

	/* Reclamation itself is performed by the common ARC worker threads. */
	arc_reclaim_async((uint64_t)shortage << PAGE_SHIFT);
}

void
arc_lowmem_init(void)
{
	uvm_reclaim_register(arc_lowmem, NULL);
}

void
arc_lowmem_fini(void)
{
	uvm_reclaim_unregister(arc_lowmem, NULL);
}

void
arc_register_hotplug(void)
{
}

void
arc_unregister_hotplug(void)
{
}
