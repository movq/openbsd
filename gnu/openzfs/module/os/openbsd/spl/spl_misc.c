// SPDX-License-Identifier: BSD-2-Clause
/* Miscellaneous OpenZFS kernel compatibility for OpenBSD. */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/byteorder.h>
#include <sys/ctype.h>
#include <sys/inttypes.h>
#include <sys/isa_defs.h>
#include <sys/sysmacros.h>
#include <sys/random.h>
#include <sys/processor.h>
#include <sys/disp.h>
#include <sys/misc.h>
#include <sys/systeminfo.h>
#include <sys/callb.h>
#include <sys/timer.h>
#include <sys/types32.h>
#include <sys/vmem.h>
#include <uvm/uvm.h>

static int zfs_usleep_chan;
static int zfs_delay_chan;

static utsname_t zfs_utsname_data = {
	.sysname = ostype,
	.nodename = hostname,
	.release = osrelease,
	.version = osversion,
	.machine = MACHINE
};

utsname_t *
zfs_utsname(void)
{

	return (&zfs_utsname_data);
}

void
zfs_usleep_range(unsigned long min, unsigned long max)
{

	(void)max;
	if (min == 0)
		return;
	(void)tsleep_nsec(&zfs_usleep_chan, PWAIT, "zfsslpt", USEC2NSEC(min));
}

void
zfs_delay(clock_t ticks)
{
	uint64_t nsecs;

	if (ticks <= 0)
		return;
	KASSERT(tick_nsec > 0);
	if ((uint64_t)ticks > MAXTSLP / (uint64_t)tick_nsec)
		nsecs = MAXTSLP;
	else
		nsecs = TICKS_TO_NSEC((uint64_t)ticks);
	(void)tsleep_nsec(&zfs_delay_chan, PWAIT, "zfsdelay", nsecs);
}

int
current_is_reclaim_thread(void)
{

	return (curproc == uvm.pagedaemon_proc);
}
