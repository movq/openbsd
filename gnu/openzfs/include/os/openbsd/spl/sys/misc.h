// SPDX-License-Identifier: BSD-2-Clause
/* Miscellaneous OpenZFS kernel compatibility for OpenBSD. */

#ifndef _SPL_SYS_MISC_H
#define _SPL_SYS_MISC_H

#include <sys/limits.h>

#define	MAXUID	UID_MAX

struct opensolaris_utsname {
	const char *sysname;
	const char *nodename;
	const char *release;
	const char *version;
	const char *machine;
};
typedef struct opensolaris_utsname utsname_t;

utsname_t *zfs_utsname(void);
#define	utsname	zfs_utsname

#define	task_io_account_read(n)	((void)(n))
#define	task_io_account_write(n)	((void)(n))

int current_is_reclaim_thread(void);

#endif /* _SPL_SYS_MISC_H */
