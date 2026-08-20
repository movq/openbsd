// SPDX-License-Identifier: BSD-2-Clause
/* Global-zone compatibility for OpenBSD. */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/zone.h>

int
zone_dataset_attach(cred_t *cr, const char *dataset, int zoneid)
{

	(void)cr;
	(void)dataset;
	(void)zoneid;
	return (EOPNOTSUPP);
}

int
zone_dataset_detach(cred_t *cr, const char *dataset, int zoneid)
{

	(void)cr;
	(void)dataset;
	(void)zoneid;
	return (EOPNOTSUPP);
}

int
zone_dataset_visible(const char *dataset, int *write)
{

	(void)dataset;
	if (write != NULL)
		*write = 1;
	return (1);
}

uint32_t
zone_get_hostid(void *zone)
{

	KASSERT(zone == NULL);
	return ((uint32_t)hostid);
}
