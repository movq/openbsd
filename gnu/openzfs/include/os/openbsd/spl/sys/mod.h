// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS module annotation compatibility for the built-in OpenBSD port. */

#ifndef _SPL_SYS_MOD_H
#define _SPL_SYS_MOD_H

/*
 * OpenZFS shared objects include this header for module annotations.  The
 * built-in OpenBSD port does not need module annotations.  Tunable export is
 * deferred, so parameter declarations currently retain their backing globals
 * without registering a control node.
 */

#define ZMOD_RW 0
#define ZMOD_RD 0
#define ZFS_MODULE_PARAM_ARGS void

#define ZFS_MODULE_PARAM(scope, prefix, name, type, perm, desc)
#define ZFS_MODULE_PARAM_CALL(scope, prefix, name, set, get, perm, desc)
#define ZFS_MODULE_VIRTUAL_PARAM_CALL(scope, prefix, name, set, get, perm, desc)

#endif /* _SPL_SYS_MOD_H */
