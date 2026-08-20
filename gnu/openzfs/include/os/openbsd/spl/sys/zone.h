// SPDX-License-Identifier: BSD-2-Clause
/* OpenBSD has one global ZFS visibility domain. */

#ifndef _SPL_SYS_ZONE_H
#define	_SPL_SYS_ZONE_H

#include <sys/types.h>
#include <sys/cred.h>

#define	GLOBAL_ZONEID		((zoneid_t)0)
#define	INGLOBALZONE(proc)	(1)

int	 zone_dataset_attach(cred_t *, const char *, int);
int	 zone_dataset_detach(cred_t *, const char *, int);
int	 zone_dataset_visible(const char *, int *);
uint32_t zone_get_hostid(void *);

#endif /* _SPL_SYS_ZONE_H */
