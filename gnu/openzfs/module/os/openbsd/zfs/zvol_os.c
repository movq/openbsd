// SPDX-License-Identifier: CDDL-1.0

/*
 * Zvols do not have an OpenBSD block-device backend yet.  Keep their hooks
 * explicit so importing a pool containing volumes remains harmless, while
 * volume-specific ioctls report that the backend is unavailable.
 */

#include <sys/errno.h>
#include <sys/fs/zfs.h>
#include <sys/dmu_objset.h>
#include <sys/dmu_tx.h>
#include <sys/spa.h>
#include <sys/zvol.h>

void
zvol_create_minors(const char *name)
{
	(void)name;
}

void
zvol_remove_minors(spa_t *spa, const char *name, boolean_t async)
{
	(void)spa;
	(void)name;
	(void)async;
}

void
zvol_rename_minors(spa_t *spa, const char *oldname, const char *newname,
    boolean_t async)
{
	(void)spa;
	(void)oldname;
	(void)newname;
	(void)async;
}

boolean_t
zvol_is_zvol(const char *name)
{
	(void)name;
	return (B_FALSE);
}

int
zvol_check_volsize(uint64_t volsize, uint64_t blocksize)
{
	(void)volsize;
	(void)blocksize;
	return (ENOTSUP);
}

int
zvol_check_volblocksize(const char *name, uint64_t blocksize)
{
	(void)name;
	(void)blocksize;
	return (ENOTSUP);
}

int
zvol_get_stats(objset_t *os, nvlist_t *nv)
{
	(void)os;
	(void)nv;
	return (ENOTSUP);
}

void
zvol_create_cb(objset_t *os, void *arg, cred_t *cr, dmu_tx_t *tx)
{
	(void)os;
	(void)arg;
	(void)cr;
	(void)tx;
}

int
zvol_set_volsize(const char *name, uint64_t size)
{
	(void)name;
	(void)size;
	return (ENOTSUP);
}

int
zvol_set_volthreading(const char *name, boolean_t enabled)
{
	(void)name;
	(void)enabled;
	return (ENOTSUP);
}

int
zvol_set_common(const char *name, zfs_prop_t prop, zprop_source_t source,
    uint64_t value)
{
	(void)name;
	(void)prop;
	(void)source;
	(void)value;
	return (ENOTSUP);
}

int
zvol_set_ro(const char *name, boolean_t readonly)
{
	(void)name;
	(void)readonly;
	return (ENOTSUP);
}

int
zvol_suspend(const char *name, zvol_state_handle_t **zvp)
{
	(void)name;
	*zvp = NULL;
	return (ENOTSUP);
}

int
zvol_resume(zvol_state_handle_t *zv)
{
	(void)zv;
	return (ENOTSUP);
}

void *
zvol_tag(zvol_state_handle_t *zv)
{
	(void)zv;
	return (NULL);
}

int
zvol_init(void)
{
	return (0);
}

void
zvol_fini(void)
{
}

int
zvol_busy(void)
{
	return (0);
}
