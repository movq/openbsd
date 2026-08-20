// SPDX-License-Identifier: BSD-2-Clause
/* Core OpenZFS privilege checks for OpenBSD. */

#ifndef _SPL_SYS_POLICY_H
#define	_SPL_SYS_POLICY_H

#include <sys/types.h>
#include <sys/cred.h>

struct znode;

int	secpolicy_nfs(cred_t *);
int	secpolicy_zfs(cred_t *);
int	secpolicy_sys_config(cred_t *, int);
int	secpolicy_zinject(cred_t *);
int	secpolicy_vnode_setid_retain(struct znode *, cred_t *, boolean_t);

/* Vnode-specific policy entry points arrive with vnode integration. */

#endif /* _SPL_SYS_POLICY_H */
