// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS credentials mapped directly to OpenBSD ucreds. */

#ifndef _SPL_SYS_CRED_H
#define	_SPL_SYS_CRED_H

#include <sys/types.h>
#include <sys/ucred.h>
#include <sys/proc.h>

typedef struct ucred cred_t;

/* Numeric identity of OpenBSD's unprivileged nobody account. */
#define	UID_NOBODY	((uid_t)32767)
#define	GID_NOBODY	((gid_t)32767)

#define	CRED()			(curproc->p_ucred)
#define	kcred			(proc0.p_ucred)

#define	KUID_TO_SUID(id)	(id)
#define	KGID_TO_SGID(id)	(id)

#define	crgetuid(cr)		((cr)->cr_uid)
#define	crgetruid(cr)		((cr)->cr_ruid)
#define	crgetgid(cr)		((cr)->cr_gid)
#define	crgetrgid(cr)		((cr)->cr_rgid)
#define	crgetgroups(cr)		((cr)->cr_groups)
#define	crgetngroups(cr)	((cr)->cr_ngroups)
#define	crgetzoneid(cr)		((zoneid_t)0)

#endif /* _SPL_SYS_CRED_H */
