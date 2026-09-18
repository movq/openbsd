// SPDX-License-Identifier: CDDL-1.0
/* ZFS directory helpers for OpenBSD. */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/dataset_kstats.h>
#include <sys/dmu_objset.h>
#include <sys/sa.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <sys/zap.h>
#include <sys/zfs_acl.h>
#include <sys/zfs_dir.h>

static int
zfs_match_find(zfsvfs_t *zfsvfs, znode_t *dzp, const char *name,
    uint64_t *zoid, char *realname, size_t realnamelen)
{
	matchtype_t mt = 0;
	uint64_t dirent;
	int error;

	if (zfsvfs->z_norm != 0) {
		mt = MT_NORMALIZE;
		if (zfsvfs->z_case == ZFS_CASE_MIXED)
			mt |= MT_MATCH_CASE;
		error = zap_lookup_norm(zfsvfs->z_os, dzp->z_id, name, 8, 1,
		    &dirent, mt, realname, realnamelen, NULL);
	} else {
		error = zap_lookup(zfsvfs->z_os, dzp->z_id, name, 8, 1,
		    &dirent);
		if (error == 0 && realname != NULL)
			strlcpy(realname, name, realnamelen);
	}
	if (error == 0)
		*zoid = ZFS_DIRENT_OBJ(dirent);
	return (error);
}

/* Resolve a ZAP entry without instantiating or locking its vnode. */
int
zfs_dirent_object(znode_t *dzp, const char *name, uint64_t *objp)
{

	return (zfs_dirent_object_name(dzp, name, objp, NULL, 0));
}

int
zfs_dirent_object_name(znode_t *dzp, const char *name, uint64_t *objp,
    char *realname, size_t realnamelen)
{

	KERNEL_ASSERT_LOCKED();
	KASSERT(VOP_ISLOCKED(ZTOV(dzp)));
	if (name[0] == '.' && (name[1] == '\0' ||
	    (name[1] == '.' && name[2] == '\0')))
		return (EINVAL);
	if (dzp->z_unlinked)
		return (ENOENT);
	return (zfs_match_find(dzp->z_zfsvfs, dzp, name, objp, realname,
	    realnamelen));
}

/*
 * Look up a real ZAP directory entry.  Dot entries are handled by
 * VOP_LOOKUP because their vnode-lock ordering is an OS contract.
 * A successful lookup returns a referenced, exclusively locked znode.
 */
int
zfs_dirent_lookup(znode_t *dzp, const char *name, znode_t **zpp, int flags)
{
	zfsvfs_t *zfsvfs = dzp->z_zfsvfs;
	uint64_t zoid;
	int error;

	KERNEL_ASSERT_LOCKED();
	KASSERT(VOP_ISLOCKED(ZTOV(dzp)));
	*zpp = NULL;

	if (name[0] == '.' && (name[1] == '\0' ||
	    (name[1] == '.' && name[2] == '\0')))
		return (EEXIST);
	if (dzp->z_unlinked)
		return (ENOENT);

	error = zfs_match_find(zfsvfs, dzp, name, &zoid, NULL, 0);
	if (error != 0) {
		if (error == ENOENT && (flags & ZEXISTS) == 0)
			return (0);
		return (error);
	}
	if (flags & ZNEW)
		return (EEXIST);

	error = zfs_zget(zfsvfs, zoid, zpp);
	if (error == 0)
		KASSERT(!(*zpp)->z_unlinked);
	return (error);
}

static uint64_t
zfs_dirent_value(znode_t *zp)
{
	uint64_t value = zp->z_id;

	if (zp->z_zfsvfs->z_version >= ZPL_VERSION_DIRENT_TYPE)
		value |= (uint64_t)IFTODT(zp->z_mode) << 60;
	return (value);
}

int
zfs_link_create(znode_t *dzp, const char *name, znode_t *zp, dmu_tx_t *tx,
    int flags)
{
	zfsvfs_t *zfsvfs = dzp->z_zfsvfs;
	boolean_t isdir = (ZTOV(zp)->v_type == VDIR);
	sa_bulk_attr_t bulk[5];
	uint64_t mtime[2], ctime[2];
	uint64_t value;
	int count = 0;
	int error;

	KASSERT(VOP_ISLOCKED(ZTOV(dzp)));
	KASSERT(VOP_ISLOCKED(ZTOV(zp)));
	if (isdir && dzp->z_links >= ZFS_LINK_MAX)
		return (EMLINK);
	if ((flags & ZRENAMING) == 0) {
		if (zp->z_unlinked)
			return (ENOENT);
		if (zp->z_links >= ZFS_LINK_MAX - isdir)
			return (EMLINK);
		zp->z_links++;
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_LINKS(zfsvfs), NULL,
		    &zp->z_links, sizeof (zp->z_links));
	}

	value = zfs_dirent_value(zp);
	error = zap_add(zfsvfs->z_os, dzp->z_id, name, 8, 1, &value, tx);
	if (error != 0) {
		if ((flags & (ZRENAMING | ZNEW)) == 0)
			zp->z_links--;
		return (error);
	}

	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_PARENT(zfsvfs), NULL,
	    &dzp->z_id, sizeof (dzp->z_id));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_FLAGS(zfsvfs), NULL,
	    &zp->z_pflags, sizeof (zp->z_pflags));
	if ((flags & ZNEW) == 0) {
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_CTIME(zfsvfs), NULL,
		    ctime, sizeof (ctime));
		zfs_tstamp_update_setup(zp, STATE_CHANGED, mtime, ctime);
	}
	VERIFY0(sa_bulk_update(zp->z_sa_hdl, bulk, count, tx));

	dzp->z_size++;
	dzp->z_links += isdir;
	count = 0;
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_SIZE(zfsvfs), NULL,
	    &dzp->z_size, sizeof (dzp->z_size));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_LINKS(zfsvfs), NULL,
	    &dzp->z_links, sizeof (dzp->z_links));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MTIME(zfsvfs), NULL,
	    mtime, sizeof (mtime));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_CTIME(zfsvfs), NULL,
	    ctime, sizeof (ctime));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_FLAGS(zfsvfs), NULL,
	    &dzp->z_pflags, sizeof (dzp->z_pflags));
	zfs_tstamp_update_setup(dzp, CONTENT_MODIFIED, mtime, ctime);
	VERIFY0(sa_bulk_update(dzp->z_sa_hdl, bulk, count, tx));
	return (0);
}

static int
zfs_dropname(znode_t *dzp, const char *name, dmu_tx_t *tx)
{
	zfsvfs_t *zfsvfs = dzp->z_zfsvfs;
	matchtype_t mt;

	if (zfsvfs->z_norm == 0)
		return (zap_remove(zfsvfs->z_os, dzp->z_id, name, tx));
	mt = MT_NORMALIZE;
	if (zfsvfs->z_case == ZFS_CASE_MIXED)
		mt |= MT_MATCH_CASE;
	return (zap_remove_norm(zfsvfs->z_os, dzp->z_id, name, mt, tx));
}

int
zfs_link_destroy(znode_t *dzp, const char *name, znode_t *zp, dmu_tx_t *tx,
    int flags, boolean_t *unlinkedp)
{
	zfsvfs_t *zfsvfs = dzp->z_zfsvfs;
	boolean_t isdir = (ZTOV(zp)->v_type == VDIR);
	boolean_t unlinked = B_FALSE;
	sa_bulk_attr_t bulk[5];
	uint64_t mtime[2], ctime[2];
	int count = 0;
	int error;

	KASSERT(VOP_ISLOCKED(ZTOV(dzp)));
	KASSERT(VOP_ISLOCKED(ZTOV(zp)));
	if ((flags & ZRENAMING) == 0) {
		if (isdir && !zfs_dirempty(zp))
			return (ENOTEMPTY);
		error = zfs_dropname(dzp, name, tx);
		if (error != 0)
			return (error);
		if (zp->z_links <= isdir)
			zp->z_links = isdir + 1;
		if (--zp->z_links == isdir) {
			zp->z_unlinked = B_TRUE;
			zp->z_links = 0;
			unlinked = B_TRUE;
		} else {
			SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_CTIME(zfsvfs),
			    NULL, ctime, sizeof (ctime));
			SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_FLAGS(zfsvfs),
			    NULL, &zp->z_pflags, sizeof (zp->z_pflags));
			zfs_tstamp_update_setup(zp, STATE_CHANGED, mtime, ctime);
		}
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_LINKS(zfsvfs), NULL,
		    &zp->z_links, sizeof (zp->z_links));
		VERIFY0(sa_bulk_update(zp->z_sa_hdl, bulk, count, tx));
	} else {
		error = zfs_dropname(dzp, name, tx);
		if (error != 0)
			return (error);
	}

	dzp->z_size--;
	dzp->z_links -= isdir;
	count = 0;
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_LINKS(zfsvfs), NULL,
	    &dzp->z_links, sizeof (dzp->z_links));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_SIZE(zfsvfs), NULL,
	    &dzp->z_size, sizeof (dzp->z_size));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_CTIME(zfsvfs), NULL,
	    ctime, sizeof (ctime));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MTIME(zfsvfs), NULL,
	    mtime, sizeof (mtime));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_FLAGS(zfsvfs), NULL,
	    &dzp->z_pflags, sizeof (dzp->z_pflags));
	zfs_tstamp_update_setup(dzp, CONTENT_MODIFIED, mtime, ctime);
	VERIFY0(sa_bulk_update(dzp->z_sa_hdl, bulk, count, tx));

	if (unlinkedp != NULL)
		*unlinkedp = unlinked;
	else if (unlinked)
		zfs_unlinked_add(zp, tx);
	return (0);
}

void
zfs_unlinked_add(znode_t *zp, dmu_tx_t *tx)
{
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;

	ASSERT(zp->z_unlinked);
	ASSERT0(zp->z_links);
	VERIFY0(zap_add_int(zfsvfs->z_os, zfsvfs->z_unlinkedobj, zp->z_id,
	    tx));
	dataset_kstats_update_nunlinks_kstat(&zfsvfs->z_kstat, 1);
}

/*
 * Revisit objects which were unlinked when the dataset last stopped.  The
 * final vput() runs the normal inactive path, which removes the object and its
 * unlinked-set entry transactionally.
 */
void
zfs_unlinked_drain(zfsvfs_t *zfsvfs)
{
	zap_cursor_t zc;
	zap_attribute_t *za;
	dmu_object_info_t doi;
	znode_t *zp;
	dmu_tx_t *tx;
	int error;

	KERNEL_ASSERT_LOCKED();
	za = zap_attribute_alloc();
	for (zap_cursor_init(&zc, zfsvfs->z_os, zfsvfs->z_unlinkedobj);
	    zap_cursor_retrieve(&zc, za) == 0; zap_cursor_advance(&zc)) {
		error = dmu_object_info(zfsvfs->z_os, za->za_first_integer,
		    &doi);
		if (error != 0 ||
		    (doi.doi_type != DMU_OT_PLAIN_FILE_CONTENTS &&
		    doi.doi_type != DMU_OT_DIRECTORY_CONTENTS))
			continue;

		error = zfs_zget_unlinked(zfsvfs, za->za_first_integer, &zp);
		if (error != 0)
			continue;

		if (zp->z_links != 0) {
			tx = dmu_tx_create(zfsvfs->z_os);
			dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
			error = dmu_tx_assign(tx, DMU_TX_WAIT);
			if (error != 0) {
				dmu_tx_abort(tx);
				vput(ZTOV(zp));
				continue;
			}
			zp->z_links = 0;
			VERIFY0(sa_update(zp->z_sa_hdl, SA_ZPL_LINKS(zfsvfs),
			    &zp->z_links, sizeof (zp->z_links), tx));
			dmu_tx_commit(tx);
		}
		zp->z_unlinked = B_TRUE;
		vput(ZTOV(zp));
	}
	zap_cursor_fini(&zc);
	zap_attribute_free(za);
}

boolean_t
zfs_dirempty(znode_t *dzp)
{

	return (dzp->z_size == 2);
}

int
zfs_make_xattrdir(znode_t *zp, vattr_t *vap, znode_t **xzpp, cred_t *cr)
{

	(void)zp;
	(void)vap;
	(void)cr;
	*xzpp = NULL;
	return (EOPNOTSUPP);
}

/* Final deletion after the last native vnode reference has gone away. */
void
zfs_rmnode(znode_t *zp)
{
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	objset_t *os = zfsvfs->z_os;
	dmu_tx_t *tx;
	uint64_t xattr_obj = 0;
	uint64_t acl_obj;
	int error;

	ASSERT0(zp->z_links);
	KASSERT(VOP_ISLOCKED(ZTOV(zp)));
	/* Extended-attribute trees need a recursive purge implementation. */
	if (sa_lookup(zp->z_sa_hdl, SA_ZPL_XATTR(zfsvfs), &xattr_obj,
	    sizeof (xattr_obj)) == 0 && xattr_obj != 0)
		return;
	error = dmu_free_long_range(os, zp->z_id, 0, DMU_OBJECT_END);
	if (error != 0)
		return;

	acl_obj = zfs_external_acl(zp);
	tx = dmu_tx_create(os);
	dmu_tx_hold_free(tx, zp->z_id, 0, DMU_OBJECT_END);
	dmu_tx_hold_zap(tx, zfsvfs->z_unlinkedobj, B_FALSE, NULL);
	if (acl_obj != 0)
		dmu_tx_hold_free(tx, acl_obj, 0, DMU_OBJECT_END);
	zfs_sa_upgrade_txholds(tx, zp);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error != 0) {
		dmu_tx_abort(tx);
		return;
	}
	VERIFY0(zap_remove_int(os, zfsvfs->z_unlinkedobj, zp->z_id, tx));
	dataset_kstats_update_nunlinked_kstat(&zfsvfs->z_kstat, 1);
	zfs_znode_delete(zp, tx);
	dmu_tx_commit(tx);
}
