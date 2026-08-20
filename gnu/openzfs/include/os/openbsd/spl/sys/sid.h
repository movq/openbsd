// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS SID-domain metadata compatibility for OpenBSD. */

#ifndef _SPL_SYS_SID_H
#define	_SPL_SYS_SID_H

#include <sys/kmem.h>
#include <sys/string.h>

/*
 * OpenBSD credentials contain native numeric uid/gid values, not SIDs.
 * This structure is nevertheless needed to read and preserve the domain
 * table used by ZFS FUIDs on disk.  It deliberately provides no identity
 * mapping or authorization interface.
 */
typedef struct ksiddomain {
	char	*kd_name;
	size_t	kd_len;
} ksiddomain_t;

typedef void ksid_t;

static inline ksiddomain_t *
ksid_lookupdomain(const char *domain)
{
	ksiddomain_t *kd;
	size_t len;

	len = strlen(domain) + 1;
	kd = kmem_alloc(sizeof (*kd), KM_SLEEP);
	kd->kd_len = len;
	kd->kd_name = kmem_alloc(len, KM_SLEEP);
	memcpy(kd->kd_name, domain, len);
	return (kd);
}

static inline void
ksiddomain_rele(ksiddomain_t *kd)
{
	kmem_free(kd->kd_name, kd->kd_len);
	kmem_free(kd, sizeof (*kd));
}

#endif /* _SPL_SYS_SID_H */
