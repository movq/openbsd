// SPDX-License-Identifier: BSD-2-Clause
/* Core OpenZFS privilege checks using OpenBSD credentials. */

#include <sys/types.h>
#include <sys/ucred.h>
#include <sys/cred.h>
#include <sys/policy.h>

int
secpolicy_nfs(cred_t *cr)
{

	return (suser_ucred(cr));
}

int
secpolicy_zfs(cred_t *cr)
{

	return (suser_ucred(cr));
}

int
secpolicy_sys_config(cred_t *cr, int checkonly)
{

	(void)checkonly;
	return (suser_ucred(cr));
}

int
secpolicy_zinject(cred_t *cr)
{

	return (suser_ucred(cr));
}

int
secpolicy_vnode_setid_retain(struct znode *zp, cred_t *cr,
    boolean_t issuidroot)
{

	(void)zp;
	(void)issuidroot;
	return (suser_ucred(cr));
}
