// SPDX-License-Identifier: CDDL-1.0
/* Minimal native OpenBSD vnode operation vector for ZFS znodes. */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/dirent.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/fcntl.h>
#include <sys/lock.h>
#include <sys/lockf.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/pool.h>
#include <sys/resourcevar.h>
#include <sys/signalvar.h>
#include <sys/stat.h>
#include <sys/u8_textprep.h>
#include <sys/uio.h>
#include <sys/uio_impl.h>
#include <sys/unistd.h>
#include <sys/vnode.h>
#include <sys/zap.h>
#include <sys/dataset_kstats.h>
#include <sys/dsl_pool.h>
#include <sys/taskq.h>
#include <sys/zfs_dir.h>
#include <sys/zfs_fuid.h>
#include <sys/zfs_quota.h>
#include <sys/zfs_rlock.h>
#include <sys/zfs_vnops.h>
#include <sys/zfs_vnops_os.h>
#include <sys/zfs_znode.h>

#include <uvm/uvm.h>
#include <uvm/uvm_extern.h>
#include <uvm/uvm_object.h>
#include <uvm/uvm_pager.h>

#include <uvm/uvm_vnode.h>

#include <miscfs/fifofs/fifo.h>

/* This translation unit implements native vnode locks, not SPL rrwlocks. */
#undef rrw_enter
#undef rrw_exit

static int
zfs_openbsd_unsupported(void *v)
{

	(void)v;
	return (EOPNOTSUPP);
}

static int
zfs_create_node(znode_t *dzp, const char *name, vattr_t *vap,
    znode_t **zpp, cred_t *cr, vsecattr_t *vsecp, boolean_t directory)
{
	zfsvfs_t *zfsvfs = dzp->z_zfsvfs;
	zfs_acl_ids_t acl_ids;
	znode_t *zp = NULL;
	dmu_tx_t *tx;
	uint64_t projid = ZFS_DEFAULT_PROJID;
	uint64_t txtype;
	boolean_t fuid_dirtied;
	int error;

	*zpp = NULL;
	if (strlen(name) > NAME_MAX)
		return (ENAMETOOLONG);
	if ((error = zfs_enter_verify_zp(zfsvfs, dzp, FTAG)) != 0)
		return (error);
	if (dzp->z_unlinked) {
		error = ENOENT;
		goto out;
	}
	if ((dzp->z_pflags & ZFS_XATTR) != 0 &&
	    (directory || vap->va_type != VREG)) {
		error = EINVAL;
		goto out;
	}
	if (zfsvfs->z_utf8 && u8_validate(name, strlen(name), NULL,
	    U8_VALIDATE_ENTIRE, &error) < 0) {
		error = EILSEQ;
		goto out;
	}
	if ((error = zfs_dirent_lookup(dzp, name, &zp, ZNEW)) != 0)
		goto out;
	if ((error = zfs_zaccess(dzp, directory ? ACE_ADD_SUBDIRECTORY :
	    ACE_ADD_FILE, 0, B_FALSE, cr, NULL)) != 0)
		goto out;
	if ((error = zfs_acl_ids_create(dzp, 0, vap, cr, vsecp, &acl_ids,
	    NULL)) != 0)
		goto out;
	if (vap->va_type == VREG || vap->va_type == VDIR)
		projid = zfs_inherit_projid(dzp);
	if (zfs_acl_ids_overquota(zfsvfs, &acl_ids, projid)) {
		error = EDQUOT;
		goto free_acl;
	}

	/* Native vnode allocation must precede transaction assignment. */
	if ((error = zfs_znode_alloc_vnode(zfsvfs, &zp)) != 0)
		goto free_acl;
	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_sa_create(tx, ZFS_SA_BASE_ATTR_SIZE);
	dmu_tx_hold_zap(tx, dzp->z_id, B_TRUE, name);
	dmu_tx_hold_sa(tx, dzp->z_sa_hdl, B_FALSE);
	if (directory)
		dmu_tx_hold_zap(tx, DMU_NEW_OBJECT, B_FALSE, NULL);
	fuid_dirtied = zfsvfs->z_fuid_dirty;
	if (fuid_dirtied)
		zfs_fuid_txhold(zfsvfs, tx);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error != 0) {
		dmu_tx_abort(tx);
		zfs_znode_discard(zp);
		goto free_acl;
	}

	zfs_mknode(dzp, vap, tx, cr, 0, &zp, &acl_ids);
	error = zfs_link_create(dzp, name, zp, tx, ZNEW);
	if (error != 0) {
		zfs_znode_delete(zp, tx);
		zfs_znode_discard(zp);
	} else {
		if (fuid_dirtied)
			zfs_fuid_sync(zfsvfs, tx);
		txtype = zfs_log_create_txtype(directory ? Z_DIR : Z_FILE,
		    vsecp, vap);
		zfs_log_create(zfsvfs->z_log, tx, txtype, dzp, zp, name,
		    vsecp, acl_ids.z_fuidp, vap);
		*zpp = zp;
	}
	dmu_tx_commit(tx);

free_acl:
	zfs_acl_ids_free(&acl_ids);
out:
	if (error == 0 && zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		error = zil_commit(zfsvfs->z_log, 0);
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

int
zfs_create(znode_t *dzp, const char *name, vattr_t *vap, int excl, int mode,
    znode_t **zpp, cred_t *cr, int flags, vsecattr_t *vsecp,
    zidmap_t *idmap)
{

	(void)excl;
	(void)mode;
	(void)flags;
	(void)flags;
	(void)idmap;
	return (zfs_create_node(dzp, name, vap, zpp, cr, vsecp, B_FALSE));
}

int
zfs_mkdir(znode_t *dzp, const char *name, vattr_t *vap, znode_t **zpp,
    cred_t *cr, int flags, vsecattr_t *vsecp, zidmap_t *idmap)
{

	(void)flags;
	(void)idmap;
	if (vap->va_type != VDIR)
		return (EINVAL);
	return (zfs_create_node(dzp, name, vap, zpp, cr, vsecp, B_TRUE));
}

static int
zfs_openbsd_create(void *v)
{
	struct vop_create_args *ap = v;
	vattr_t va = *ap->a_vap;
	znode_t *zp;
	int error;

	KERNEL_ASSERT_LOCKED();
	KASSERT(VOP_ISLOCKED(ap->a_dvp));
	KASSERT(ap->a_cnp->cn_flags & HASBUF);
	va.va_mask = AT_MODE;
	if (va.va_gid != (gid_t)VNOVAL)
		va.va_mask |= AT_GID;
	error = zfs_create(VTOZ(ap->a_dvp), ap->a_cnp->cn_nameptr, &va,
	    (va.va_vaflags & VA_EXCLUSIVE) != 0, 0, &zp,
	    ap->a_cnp->cn_cred, 0, NULL, NULL);
	if (error == 0) {
		*ap->a_vpp = ZTOV(zp);
		VN_KNOTE(ap->a_dvp, NOTE_WRITE);
	}
	if ((ap->a_cnp->cn_flags & SAVESTART) == 0)
		pool_put(&namei_pool, ap->a_cnp->cn_pnbuf);
	return (error);
}

static int
zfs_openbsd_mknod(void *v)
{
	struct vop_mknod_args *ap = v;
	vattr_t va = *ap->a_vap;
	znode_t *zp;
	int error;

	KERNEL_ASSERT_LOCKED();
	KASSERT(VOP_ISLOCKED(ap->a_dvp));
	KASSERT(ap->a_cnp->cn_flags & HASBUF);
	if (va.va_type != VFIFO)
		return (EOPNOTSUPP);
	va.va_mask = AT_MODE;
	if (va.va_gid != (gid_t)VNOVAL)
		va.va_mask |= AT_GID;
	error = zfs_create(VTOZ(ap->a_dvp), ap->a_cnp->cn_nameptr, &va,
	    1, 0, &zp, ap->a_cnp->cn_cred, 0, NULL, NULL);
	if (error == 0) {
		*ap->a_vpp = ZTOV(zp);
		VN_KNOTE(ap->a_dvp, NOTE_WRITE);
		vput(*ap->a_vpp);
		*ap->a_vpp = NULL;
	}
	if ((ap->a_cnp->cn_flags & SAVESTART) == 0)
		pool_put(&namei_pool, ap->a_cnp->cn_pnbuf);
	return (error);
}

static int
zfs_openbsd_mkdir(void *v)
{
	struct vop_mkdir_args *ap = v;
	vattr_t va = *ap->a_vap;
	znode_t *zp;
	int error;

	KERNEL_ASSERT_LOCKED();
	KASSERT(VOP_ISLOCKED(ap->a_dvp));
	KASSERT(ap->a_cnp->cn_flags & HASBUF);
	va.va_mask = AT_MODE;
	if (va.va_gid != (gid_t)VNOVAL)
		va.va_mask |= AT_GID;
	error = zfs_mkdir(VTOZ(ap->a_dvp), ap->a_cnp->cn_nameptr, &va,
	    &zp, ap->a_cnp->cn_cred, 0, NULL, NULL);
	if (error == 0) {
		*ap->a_vpp = ZTOV(zp);
		VN_KNOTE(ap->a_dvp, NOTE_WRITE | NOTE_LINK);
	}
	if ((ap->a_cnp->cn_flags & SAVESTART) == 0)
		pool_put(&namei_pool, ap->a_cnp->cn_pnbuf);
	vput(ap->a_dvp);
	return (error);
}

static int
zfs_remove_node(znode_t *dzp, const char *name, znode_t *zp, cred_t *cr,
    boolean_t directory)
{
	zfsvfs_t *zfsvfs = dzp->z_zfsvfs;
	dmu_tx_t *tx;
	uint64_t object = zp->z_id;
	int error;

	if ((error = zfs_enter_verify_zp(zfsvfs, dzp, FTAG)) != 0)
		return (error);
	if ((error = zfs_verify_zp(zp)) != 0)
		goto out;
	if ((error = zfs_zaccess_delete(dzp, zp, cr, NULL)) != 0)
		goto out;
	if (directory) {
		if (ZTOV(zp)->v_type != VDIR) {
			error = ENOTDIR;
			goto out;
		}
		if (!zfs_dirempty(zp)) {
			error = ENOTEMPTY;
			goto out;
		}
	} else if (ZTOV(zp)->v_type == VDIR) {
		error = EPERM;
		goto out;
	}

	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_zap(tx, dzp->z_id, B_FALSE, name);
	dmu_tx_hold_sa(tx, dzp->z_sa_hdl, B_FALSE);
	dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
	dmu_tx_hold_zap(tx, zfsvfs->z_unlinkedobj, B_FALSE, NULL);
	zfs_sa_upgrade_txholds(tx, dzp);
	zfs_sa_upgrade_txholds(tx, zp);
	dmu_tx_mark_netfree(tx);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error != 0) {
		dmu_tx_abort(tx);
		goto out;
	}
	error = zfs_link_destroy(dzp, name, zp, tx, ZEXISTS, NULL);
	if (error == 0) {
		zfs_log_remove(zfsvfs->z_log, tx,
		    directory ? TX_RMDIR : TX_REMOVE, dzp, name,
		    directory ? ZFS_NO_OBJECT : object,
		    directory ? B_FALSE : zp->z_unlinked);
	}
	dmu_tx_commit(tx);

out:
	if (error == 0 && zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		error = zil_commit(zfsvfs->z_log, 0);
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

int
zfs_remove(znode_t *dzp, const char *name, cred_t *cr, int flags)
{
	znode_t *zp;
	int error;

	(void)flags;
	if ((error = zfs_dirent_lookup(dzp, name, &zp, ZEXISTS)) != 0)
		return (error);
	error = zfs_remove_node(dzp, name, zp, cr, B_FALSE);
	zrele(zp);
	return (error);
}

int
zfs_rmdir(znode_t *dzp, const char *name, znode_t *cwd, cred_t *cr,
    int flags)
{
	znode_t *zp;
	int error;

	(void)cwd;
	(void)flags;
	if ((error = zfs_dirent_lookup(dzp, name, &zp, ZEXISTS)) != 0)
		return (error);
	error = zfs_remove_node(dzp, name, zp, cr, B_TRUE);
	zrele(zp);
	return (error);
}

static int
zfs_openbsd_remove(void *v)
{
	struct vop_remove_args *ap = v;
	int error;

	KERNEL_ASSERT_LOCKED();
	KASSERT(VOP_ISLOCKED(ap->a_dvp));
	KASSERT(VOP_ISLOCKED(ap->a_vp));
	KASSERT(ap->a_cnp->cn_flags & HASBUF);
	error = zfs_remove_node(VTOZ(ap->a_dvp), ap->a_cnp->cn_nameptr,
	    VTOZ(ap->a_vp), ap->a_cnp->cn_cred, B_FALSE);
	if (error == 0) {
		VN_KNOTE(ap->a_vp, NOTE_DELETE);
		VN_KNOTE(ap->a_dvp, NOTE_WRITE);
		cache_purge(ap->a_vp);
	}
	pool_put(&namei_pool, ap->a_cnp->cn_pnbuf);
	return (error);
}

static int
zfs_openbsd_rmdir(void *v)
{
	struct vop_rmdir_args *ap = v;
	int error;

	KERNEL_ASSERT_LOCKED();
	KASSERT(VOP_ISLOCKED(ap->a_dvp));
	KASSERT(VOP_ISLOCKED(ap->a_vp));
	KASSERT(ap->a_cnp->cn_flags & HASBUF);
	error = zfs_remove_node(VTOZ(ap->a_dvp), ap->a_cnp->cn_nameptr,
	    VTOZ(ap->a_vp), ap->a_cnp->cn_cred, B_TRUE);
	if (error == 0) {
		VN_KNOTE(ap->a_vp, NOTE_DELETE);
		VN_KNOTE(ap->a_dvp, NOTE_WRITE | NOTE_LINK);
		cache_purge(ap->a_dvp);
		cache_purge(ap->a_vp);
	}
	pool_put(&namei_pool, ap->a_cnp->cn_pnbuf);
	vput(ap->a_dvp);
	vput(ap->a_vp);
	return (error);
}

int
zfs_link(znode_t *tdzp, znode_t *szp, const char *name, cred_t *cr,
    int flags)
{
	zfsvfs_t *zfsvfs = tdzp->z_zfsvfs;
	dmu_tx_t *tx;
	znode_t *found;
	int error;

	(void)flags;
	if (strlen(name) > NAME_MAX)
		return (ENAMETOOLONG);
	if ((error = zfs_enter_verify_zp(zfsvfs, tdzp, FTAG)) != 0)
		return (error);
	if ((error = zfs_verify_zp(szp)) != 0)
		goto out;
	if (ZTOV(szp)->v_type == VDIR) {
		error = EPERM;
		goto out;
	}
	if ((szp->z_pflags & (ZFS_APPENDONLY | ZFS_IMMUTABLE |
	    ZFS_READONLY)) != 0) {
		error = EPERM;
		goto out;
	}
	if ((szp->z_pflags & ZFS_XATTR) != (tdzp->z_pflags & ZFS_XATTR)) {
		error = EINVAL;
		goto out;
	}
	if ((tdzp->z_pflags & ZFS_PROJINHERIT) != 0 &&
	    tdzp->z_projid != szp->z_projid) {
		error = EXDEV;
		goto out;
	}
	if ((error = zfs_zaccess(tdzp, ACE_ADD_FILE, 0, B_FALSE, cr,
	    NULL)) != 0)
		goto out;
	if ((error = zfs_dirent_lookup(tdzp, name, &found, ZNEW)) != 0)
		goto out;

	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_sa(tx, szp->z_sa_hdl, B_FALSE);
	dmu_tx_hold_sa(tx, tdzp->z_sa_hdl, B_FALSE);
	dmu_tx_hold_zap(tx, tdzp->z_id, B_TRUE, name);
	zfs_sa_upgrade_txholds(tx, szp);
	zfs_sa_upgrade_txholds(tx, tdzp);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error != 0) {
		dmu_tx_abort(tx);
		goto out;
	}
	error = zfs_link_create(tdzp, name, szp, tx, 0);
	if (error == 0)
		zfs_log_link(zfsvfs->z_log, tx, TX_LINK, tdzp, szp, name);
	dmu_tx_commit(tx);
out:
	if (error == 0 && zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		error = zil_commit(zfsvfs->z_log, 0);
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

static int
zfs_openbsd_link(void *v)
{
	struct vop_link_args *ap = v;
	int error;

	KERNEL_ASSERT_LOCKED();
	KASSERT(VOP_ISLOCKED(ap->a_dvp));
	KASSERT(ap->a_cnp->cn_flags & HASBUF);
	if (ap->a_dvp == ap->a_vp)
		error = EPERM;
	else if ((error = vn_lock(ap->a_vp, LK_EXCLUSIVE)) == 0) {
		error = zfs_link(VTOZ(ap->a_dvp), VTOZ(ap->a_vp),
		    ap->a_cnp->cn_nameptr, ap->a_cnp->cn_cred, 0);
		if (error == 0) {
			VN_KNOTE(ap->a_vp, NOTE_LINK);
			VN_KNOTE(ap->a_dvp, NOTE_WRITE);
		}
		VOP_UNLOCK(ap->a_vp);
	}
	pool_put(&namei_pool, ap->a_cnp->cn_pnbuf);
	vput(ap->a_dvp);
	return (error);
}

static int
zfs_rename_lock_dirs(znode_t *sdzp, znode_t *tdzp)
{
	vnode_t *first, *second;
	int error;

	if (sdzp == tdzp)
		return (vn_lock(ZTOV(sdzp), LK_EXCLUSIVE | LK_RETRY));
	if (sdzp->z_id < tdzp->z_id) {
		first = ZTOV(sdzp);
		second = ZTOV(tdzp);
	} else {
		first = ZTOV(tdzp);
		second = ZTOV(sdzp);
	}
	if ((error = vn_lock(first, LK_EXCLUSIVE | LK_RETRY)) != 0)
		return (error);
	if ((error = vn_lock(second, LK_EXCLUSIVE | LK_RETRY)) != 0)
		VOP_UNLOCK(first);
	return (error);
}

static void
zfs_rename_unlock_dirs(znode_t *sdzp, znode_t *tdzp)
{

	VOP_UNLOCK(ZTOV(sdzp));
	if (sdzp != tdzp)
		VOP_UNLOCK(ZTOV(tdzp));
}

static int
zfs_rename_parent(zfsvfs_t *zfsvfs, uint64_t object, uint64_t *parentp)
{
	sa_handle_t *hdl;
	int error;

	error = sa_handle_get(zfsvfs->z_os, object, NULL, SA_HDL_PRIVATE,
	    &hdl);
	if (error != 0)
		return (error);
	error = sa_lookup(hdl, SA_ZPL_PARENT(zfsvfs), parentp,
	    sizeof (*parentp));
	sa_handle_destroy(hdl);
	return (error);
}

/* z_rename_lock keeps parent pointers stable during this unlocked-SA walk. */
static int
zfs_rename_check_path(znode_t *szp, znode_t *sdzp, znode_t *tdzp)
{
	zfsvfs_t *zfsvfs = sdzp->z_zfsvfs;
	uint64_t object = tdzp->z_id;
	uint64_t parent;
	int error;

	KASSERT(MUTEX_HELD(&zfsvfs->z_rename_lock));
	for (;;) {
		if (object == szp->z_id)
			return (EINVAL);
		if (object == sdzp->z_id || object == zfsvfs->z_root)
			return (0);
		if ((error = zfs_rename_parent(zfsvfs, object, &parent)) != 0)
			return (error);
		if (parent == object)
			return (0);
		object = parent;
	}
}

/*
 * The OpenBSD interface deliberately takes referenced, unlocked directories.
 * A single per-filesystem lock serializes relookup and the parent-pointer
 * change, avoiding the native four-vnode rename lock protocol.
 */
int
zfs_rename(znode_t *sdzp, const char *snm, znode_t *tdzp, const char *tnm,
    cred_t *cr, int flags, uint64_t rflags, vattr_t *wo_vap, zidmap_t *idmap)
{
	zfsvfs_t *zfsvfs;
	znode_t *szp = NULL, *tzp = NULL;
	dmu_tx_t *tx;
	uint64_t sobj, tobj;
	char realname[NAME_MAX + 1];
	boolean_t entered = B_FALSE;
	boolean_t dirs_locked = B_FALSE;
	boolean_t target_exists = B_FALSE;
	boolean_t case_rename = B_FALSE;
	int error;

	(void)flags;
	if (sdzp == NULL || tdzp == NULL || snm == NULL || tnm == NULL)
		return (EINVAL);
	KERNEL_ASSERT_LOCKED();
	KASSERT(!VOP_ISLOCKED(ZTOV(sdzp)));
	KASSERT(sdzp == tdzp || !VOP_ISLOCKED(ZTOV(tdzp)));
	if (rflags != 0 || wo_vap != NULL)
		return (EINVAL);
	if (sdzp->z_zfsvfs != tdzp->z_zfsvfs)
		return (EXDEV);
	if (strlen(snm) > NAME_MAX || strlen(tnm) > NAME_MAX)
		return (ENAMETOOLONG);
	if ((snm[0] == '.' && (snm[1] == '\0' ||
	    (snm[1] == '.' && snm[2] == '\0'))) ||
	    (tnm[0] == '.' && (tnm[1] == '\0' ||
	    (tnm[1] == '.' && tnm[2] == '\0'))))
		return (EINVAL);

	zfsvfs = sdzp->z_zfsvfs;
	if (zfsvfs->z_utf8 &&
	    (u8_validate(snm, strlen(snm), NULL, U8_VALIDATE_ENTIRE,
	    &error) < 0 || u8_validate(tnm, strlen(tnm), NULL,
	    U8_VALIDATE_ENTIRE, &error) < 0))
		return (EILSEQ);

	mutex_enter(&zfsvfs->z_rename_lock);
	error = zfs_rename_lock_dirs(sdzp, tdzp);
	if (error != 0)
		goto out;
	dirs_locked = B_TRUE;
	if ((error = zfs_enter_verify_zp(zfsvfs, sdzp, FTAG)) != 0)
		goto out;
	entered = B_TRUE;
	if ((error = zfs_verify_zp(tdzp)) != 0)
		goto out;
	if (sdzp->z_unlinked || tdzp->z_unlinked) {
		error = ENOENT;
		goto out;
	}
	if ((sdzp->z_pflags & ZFS_XATTR) !=
	    (tdzp->z_pflags & ZFS_XATTR)) {
		error = EINVAL;
		goto out;
	}

	if ((error = zfs_dirent_object_name(sdzp, snm, &sobj, realname,
	    sizeof (realname))) != 0)
		goto out;
	error = zfs_dirent_object(tdzp, tnm, &tobj);
	if (error == 0)
		target_exists = B_TRUE;
	else if (error == ENOENT)
		error = 0;
	else
		goto out;

	/* Do not recursively lock a directory vnode already held above. */
	if (sobj == sdzp->z_id || sobj == tdzp->z_id) {
		error = EINVAL;
		goto out;
	}
	if (target_exists && sobj == tobj) {
		case_rename = (sdzp == tdzp && strcmp(snm, tnm) != 0 &&
		    strcmp(realname, snm) == 0 &&
		    (zfsvfs->z_case == ZFS_CASE_INSENSITIVE ||
		    (zfsvfs->z_case == ZFS_CASE_MIXED &&
		    (flags & FIGNORECASE) != 0)));
		if (!case_rename) {
			error = 0;
			goto out;
		}
		target_exists = B_FALSE;
	}
	if (target_exists &&
	    (tobj == sdzp->z_id || tobj == tdzp->z_id)) {
		error = EINVAL;
		goto out;
	}

	if ((error = zfs_zget(zfsvfs, sobj, &szp)) != 0)
		goto out;
	if (ZTOV(szp)->v_mountedhere != NULL) {
		error = EBUSY;
		goto out;
	}
	if (ZTOV(szp)->v_type == VDIR &&
	    (error = zfs_rename_check_path(szp, sdzp, tdzp)) != 0)
		goto out;
	if (target_exists) {
		if ((error = zfs_zget(zfsvfs, tobj, &tzp)) != 0)
			goto out;
		if (ZTOV(tzp)->v_mountedhere != NULL) {
			error = EBUSY;
			goto out;
		}
		if (ZTOV(szp)->v_type == VDIR && ZTOV(tzp)->v_type != VDIR) {
			error = ENOTDIR;
			goto out;
		}
		if (ZTOV(szp)->v_type != VDIR && ZTOV(tzp)->v_type == VDIR) {
			error = EISDIR;
			goto out;
		}
		if (ZTOV(tzp)->v_type == VDIR && !zfs_dirempty(tzp)) {
			error = ENOTEMPTY;
			goto out;
		}
	}
	if ((tdzp->z_pflags & ZFS_PROJINHERIT) != 0 &&
	    tdzp->z_projid != szp->z_projid) {
		error = EXDEV;
		goto out;
	}
	if ((error = zfs_zaccess_rename(sdzp, szp, tdzp, tzp, cr,
	    idmap)) != 0)
		goto out;

	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_sa(tx, szp->z_sa_hdl, B_FALSE);
	dmu_tx_hold_sa(tx, sdzp->z_sa_hdl, B_FALSE);
	dmu_tx_hold_zap(tx, sdzp->z_id, B_FALSE, snm);
	dmu_tx_hold_zap(tx, tdzp->z_id, B_TRUE, tnm);
	if (sdzp != tdzp)
		dmu_tx_hold_sa(tx, tdzp->z_sa_hdl, B_FALSE);
	if (tzp != NULL)
		dmu_tx_hold_sa(tx, tzp->z_sa_hdl, B_FALSE);
	dmu_tx_hold_zap(tx, zfsvfs->z_unlinkedobj, B_FALSE, NULL);
	zfs_sa_upgrade_txholds(tx, szp);
	zfs_sa_upgrade_txholds(tx, sdzp);
	if (sdzp != tdzp)
		zfs_sa_upgrade_txholds(tx, tdzp);
	if (tzp != NULL)
		zfs_sa_upgrade_txholds(tx, tzp);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error != 0) {
		dmu_tx_abort(tx);
		goto out;
	}

	if (case_rename) {
		error = zfs_link_destroy(sdzp, snm, szp, tx, ZRENAMING,
		    NULL);
		if (error == 0) {
			error = zfs_link_create(tdzp, tnm, szp, tx, ZRENAMING);
			if (error != 0)
				VERIFY0(zfs_link_create(sdzp, snm, szp, tx,
				    ZRENAMING));
		}
	} else {
		if (tzp != NULL)
			error = zfs_link_destroy(tdzp, tnm, tzp, tx, 0, NULL);
		if (error == 0)
			error = zfs_link_create(tdzp, tnm, szp, tx, ZRENAMING);
		if (error == 0) {
			error = zfs_link_destroy(sdzp, snm, szp, tx, ZRENAMING,
			    NULL);
			if (error != 0)
				VERIFY0(zfs_link_destroy(tdzp, tnm, szp, tx,
				    ZRENAMING, NULL));
		}
	}
	if (error == 0) {
		szp->z_pflags |= ZFS_AV_MODIFIED;
		VERIFY0(sa_update(szp->z_sa_hdl, SA_ZPL_FLAGS(zfsvfs),
		    &szp->z_pflags, sizeof (szp->z_pflags), tx));
		zfs_log_rename(zfsvfs->z_log, tx, TX_RENAME, sdzp, snm,
		    tdzp, tnm, szp);
	}
	dmu_tx_commit(tx);

	if (error == 0) {
		VN_KNOTE(ZTOV(szp), NOTE_RENAME);
		VN_KNOTE(ZTOV(sdzp), NOTE_WRITE |
		    (ZTOV(szp)->v_type == VDIR ? NOTE_LINK : 0));
		if (sdzp != tdzp)
			VN_KNOTE(ZTOV(tdzp), NOTE_WRITE |
			    (ZTOV(szp)->v_type == VDIR ? NOTE_LINK : 0));
		if (tzp != NULL)
			VN_KNOTE(ZTOV(tzp), NOTE_DELETE);
		cache_purge(ZTOV(szp));
		if (tzp != NULL)
			cache_purge(ZTOV(tzp));
		cache_purge(ZTOV(sdzp));
		if (sdzp != tdzp)
			cache_purge(ZTOV(tdzp));
	}

out:
	if (tzp != NULL)
		zrele(tzp);
	if (szp != NULL)
		zrele(szp);
	if (entered) {
		if (error == 0 && zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
			error = zil_commit(zfsvfs->z_log, 0);
		zfs_exit(zfsvfs, FTAG);
	}
	if (dirs_locked)
		zfs_rename_unlock_dirs(sdzp, tdzp);
	mutex_exit(&zfsvfs->z_rename_lock);
	return (error);
}

static void
zfs_openbsd_rename_abort(struct vop_rename_args *ap)
{

	VOP_ABORTOP(ap->a_tdvp, ap->a_tcnp);
	if (ap->a_tdvp == ap->a_tvp)
		vrele(ap->a_tdvp);
	else
		vput(ap->a_tdvp);
	if (ap->a_tvp != NULL)
		vput(ap->a_tvp);
	VOP_ABORTOP(ap->a_fdvp, ap->a_fcnp);
	vrele(ap->a_fdvp);
	vrele(ap->a_fvp);
}

static int
zfs_openbsd_rename(void *v)
{
	struct vop_rename_args *ap = v;
	vnode_t *fdvp = ap->a_fdvp;
	vnode_t *fvp = ap->a_fvp;
	vnode_t *tdvp = ap->a_tdvp;
	vnode_t *tvp = ap->a_tvp;
	int error;

	KERNEL_ASSERT_LOCKED();
	KASSERT(fdvp != NULL && fvp != NULL && tdvp != NULL);
	KASSERT(VOP_ISLOCKED(tdvp));
	KASSERT(tvp == NULL || VOP_ISLOCKED(tvp));
	KASSERT(ap->a_fcnp->cn_flags & HASBUF);
	KASSERT(ap->a_tcnp->cn_flags & HASBUF);

	if (fvp->v_mount != tdvp->v_mount ||
	    fdvp->v_mount != fvp->v_mount ||
	    (tvp != NULL && fvp->v_mount != tvp->v_mount)) {
		zfs_openbsd_rename_abort(ap);
		return (EXDEV);
	}

	/* Discard VFS's asymmetric lookup locks and resolve under our lock. */
	VOP_UNLOCK(tdvp);
	if (tvp != NULL && tvp != tdvp)
		VOP_UNLOCK(tvp);
	vrele(fvp);
	if (tvp != NULL)
		vrele(tvp);

	if (tvp == tdvp) {
		error = EINVAL;
	} else {
		error = zfs_rename(VTOZ(fdvp), ap->a_fcnp->cn_nameptr,
		    VTOZ(tdvp), ap->a_tcnp->cn_nameptr,
		    ap->a_fcnp->cn_cred, 0, 0, NULL, NULL);
	}
	vrele(fdvp);
	vrele(tdvp);
	return (error);
}

int
zfs_symlink(znode_t *dzp, const char *name, vattr_t *vap,
    const char *target, znode_t **zpp, cred_t *cr, int flags,
    zidmap_t *idmap)
{
	zfsvfs_t *zfsvfs = dzp->z_zfsvfs;
	zfs_acl_ids_t acl_ids;
	znode_t *zp = NULL;
	dmu_tx_t *tx;
	boolean_t fuid_dirtied;
	size_t len = strlen(target);
	int error;

	(void)flags;
	(void)idmap;
	*zpp = NULL;
	if (vap->va_type != VLNK)
		return (EINVAL);
	if (strlen(name) > NAME_MAX || len > MAXPATHLEN)
		return (ENAMETOOLONG);
	if ((error = zfs_enter_verify_zp(zfsvfs, dzp, FTAG)) != 0)
		return (error);
	if ((error = zfs_dirent_lookup(dzp, name, &zp, ZNEW)) != 0)
		goto out;
	if ((error = zfs_zaccess(dzp, ACE_ADD_FILE, 0, B_FALSE, cr,
	    NULL)) != 0)
		goto out;
	if ((error = zfs_acl_ids_create(dzp, 0, vap, cr, NULL, &acl_ids,
	    NULL)) != 0)
		goto out;
	if (zfs_acl_ids_overquota(zfsvfs, &acl_ids, ZFS_DEFAULT_PROJID)) {
		error = EDQUOT;
		goto free_acl;
	}
	if ((error = zfs_znode_alloc_vnode(zfsvfs, &zp)) != 0)
		goto free_acl;

	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_write(tx, DMU_NEW_OBJECT, 0, MAX(1, len));
	dmu_tx_hold_zap(tx, dzp->z_id, B_TRUE, name);
	dmu_tx_hold_sa_create(tx, ZFS_SA_BASE_ATTR_SIZE + len);
	dmu_tx_hold_sa(tx, dzp->z_sa_hdl, B_FALSE);
	fuid_dirtied = zfsvfs->z_fuid_dirty;
	if (fuid_dirtied)
		zfs_fuid_txhold(zfsvfs, tx);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error != 0) {
		dmu_tx_abort(tx);
		zfs_znode_discard(zp);
		goto free_acl;
	}
	zfs_mknode(dzp, vap, tx, cr, 0, &zp, &acl_ids);
	if (fuid_dirtied)
		zfs_fuid_sync(zfsvfs, tx);
	if (zp->z_is_sa) {
		error = sa_update(zp->z_sa_hdl, SA_ZPL_SYMLINK(zfsvfs),
		    (void *)(uintptr_t)target, len, tx);
	} else {
		zfs_sa_symlink(zp, (char *)(uintptr_t)target, len, tx);
		error = 0;
	}
	if (error == 0) {
		zp->z_size = len;
		error = sa_update(zp->z_sa_hdl, SA_ZPL_SIZE(zfsvfs),
		    &zp->z_size, sizeof (zp->z_size), tx);
	}
	if (error == 0)
		error = zfs_link_create(dzp, name, zp, tx, ZNEW);
	if (error != 0) {
		zfs_znode_delete(zp, tx);
		zfs_znode_discard(zp);
	} else {
		zfs_znode_update_vfs(zp);
		zfs_log_symlink(zfsvfs->z_log, tx, TX_SYMLINK, dzp, zp,
		    name, target);
		*zpp = zp;
	}
	dmu_tx_commit(tx);
free_acl:
	zfs_acl_ids_free(&acl_ids);
out:
	if (error == 0 && zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		error = zil_commit(zfsvfs->z_log, 0);
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

static int
zfs_openbsd_symlink(void *v)
{
	struct vop_symlink_args *ap = v;
	vattr_t va = *ap->a_vap;
	znode_t *zp;
	int error;

	KERNEL_ASSERT_LOCKED();
	KASSERT(VOP_ISLOCKED(ap->a_dvp));
	KASSERT(ap->a_cnp->cn_flags & HASBUF);
	va.va_type = VLNK;
	va.va_mask = AT_MODE;
	error = zfs_symlink(VTOZ(ap->a_dvp), ap->a_cnp->cn_nameptr, &va,
	    ap->a_target, &zp, ap->a_cnp->cn_cred, 0, NULL);
	if (error == 0) {
		*ap->a_vpp = ZTOV(zp);
		VN_KNOTE(ap->a_dvp, NOTE_WRITE);
		vput(ZTOV(zp));
	}
	if ((ap->a_cnp->cn_flags & SAVESTART) == 0)
		pool_put(&namei_pool, ap->a_cnp->cn_pnbuf);
	vput(ap->a_dvp);
	return (error);
}

static int
zfs_openbsd_open(void *v)
{
	struct vop_open_args *ap = v;
	vnode_t *vp = ap->a_vp;
	znode_t *zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	int error;

	KERNEL_ASSERT_LOCKED();
	KASSERT(VOP_ISLOCKED(vp));
	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	if ((ap->a_mode & FWRITE) != 0) {
		if ((zp->z_pflags & ZFS_APPENDONLY) != 0 &&
		    (ap->a_mode & O_APPEND) == 0)
			error = EPERM;
		else if ((vp->v_mount->mnt_flag & MNT_RDONLY) != 0)
			error = EROFS;
	}
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

static int
zfs_openbsd_close(void *v)
{
	struct vop_close_args *ap = v;
	vnode_t *vp = ap->a_vp;
	znode_t *zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	int error;

	KERNEL_ASSERT_LOCKED();
	KASSERT(VOP_ISLOCKED(vp));
	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);
	zfs_exit(zfsvfs, FTAG);
	return (0);
}

static int
zfs_openbsd_read(void *v)
{
	struct vop_read_args *ap = v;
	vnode_t *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	znode_t *zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	zfs_locked_range_t *lr;
	zfs_uio_t zuio;
	uint64_t remaining;
	size_t before;
	size_t nbytes;
	size_t orig_resid;
	int error;

	KERNEL_ASSERT_LOCKED();
	KASSERT(VOP_ISLOCKED(vp));
	if (vp->v_type != VREG)
		return (EISDIR);
	if (uio->uio_offset < 0 || uio->uio_iovcnt <= 0)
		return (EINVAL);
	if (uio->uio_resid == 0)
		return (0);
	if (uio->uio_resid > (uint64_t)LLONG_MAX - uio->uio_offset)
		return (EINVAL);

	mutex_enter(&zp->z_map_lock);
	zfs_uio_init(&zuio, uio);
	orig_resid = uio->uio_resid;
	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0) {
		mutex_exit(&zp->z_map_lock);
		return (error);
	}
	if ((zp->z_pflags & ZFS_AV_QUARANTINED) != 0) {
		error = EACCES;
		goto out;
	}

	lr = zfs_rangelock_enter(&zp->z_rangelock, uio->uio_offset,
	    uio->uio_resid, RL_READER);
	if ((uint64_t)uio->uio_offset >= zp->z_size) {
		error = 0;
		goto unlock;
	}
	remaining = MIN((uint64_t)uio->uio_resid,
	    zp->z_size - (uint64_t)uio->uio_offset);
	error = 0;
	while (remaining != 0) {
		nbytes = MIN(remaining, DMU_MAX_ACCESS / 2);
		before = uio->uio_resid;
		if (zn_has_cached_data(zp, uio->uio_offset,
		    uio->uio_offset + nbytes - 1))
			error = mappedread(zp, nbytes, &zuio);
		else
			error = dmu_read_uio_dbuf(sa_get_db(zp->z_sa_hdl),
			    &zuio, nbytes, DMU_READ_PREFETCH);
		if (error == ECKSUM)
			error = EIO;
		if (error != 0)
			break;
		if (before - uio->uio_resid != nbytes) {
			error = EIO;
			break;
		}
		remaining -= nbytes;
	}
unlock:
	zfs_rangelock_exit(lr);
	dataset_kstats_update_read_kstats(&zfsvfs->z_kstat,
	    orig_resid - uio->uio_resid);
out:
	ZFS_ACCESSTIME_STAMP(zfsvfs, zp);
	zfs_exit(zfsvfs, FTAG);
	mutex_exit(&zp->z_map_lock);
	return (error);
}

/*
 * Page faults must not re-enter zfs_openbsd_read(): the newly allocated UVM
 * page is already visible to the common ZPL cache hooks.  Read the DMU
 * directly, using a non-blocking range lock so that a writer invalidating
 * the busy fault page can make progress.
 */
static int
zfs_openbsd_uvp_read(vnode_t *vp, struct uio *uio)
{
	znode_t *zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	zfs_locked_range_t *lr;
	zfs_uio_t zuio;
	uint64_t nbytes;
	size_t before;
	int error;

	KERNEL_ASSERT_LOCKED();
	KASSERT(uio->uio_rw == UIO_READ);
	KASSERT(uio->uio_segflg == UIO_SYSSPACE);
	if (uio->uio_resid == 0)
		return (0);

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);
	lr = zfs_rangelock_tryenter(&zp->z_rangelock,
	    (uint64_t)uio->uio_offset, uio->uio_resid, RL_READER);
	if (lr == NULL) {
		/*
		 * If this thread already holds z_map_lock, the fault came from
		 * copyin/copyout in an ordinary operation on this vnode.  That
		 * operation cannot drop its writer range lock until the fault
		 * completes, so asking UVM to retry would livelock.  Report a
		 * hard pager error instead; UVM turns it into EFAULT for the
		 * system call.  A different thread's range lock can make
		 * progress, and remains a retryable EBUSY.
		 */
		error = MUTEX_HELD(&zp->z_map_lock) ? EDEADLK : EBUSY;
		goto out;
	}
	if ((uint64_t)uio->uio_offset >= zp->z_size) {
		error = 0;
		goto unlock;
	}

	nbytes = MIN((uint64_t)uio->uio_resid,
	    zp->z_size - (uint64_t)uio->uio_offset);
	before = uio->uio_resid;
	zfs_uio_init(&zuio, uio);
	error = dmu_read_uio_dbuf(sa_get_db(zp->z_sa_hdl), &zuio, nbytes,
	    DMU_READ_PREFETCH);
	if (error == ECKSUM)
		error = EIO;
	if (error == 0 && before - uio->uio_resid != nbytes)
		error = EIO;
	if (error == 0) {
		dataset_kstats_update_read_kstats(&zfsvfs->z_kstat, nbytes);
		ZFS_ACCESSTIME_STAMP(zfsvfs, zp);
	}
unlock:
	zfs_rangelock_exit(lr);
out:
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

static int
zfs_openbsd_uvp_write(vnode_t *vp, struct uio *uio, int flags)
{
	znode_t *zp = VTOZ(vp);
	zfs_uio_t zuio;
	int error;

	KERNEL_ASSERT_LOCKED();
	KASSERT(uio->uio_rw == UIO_WRITE);
	KASSERT(uio->uio_segflg == UIO_SYSSPACE);
	if (uio->uio_resid == 0)
		return (0);

	/*
	 * The pages supplied by UVM are already busy and mapped in the
	 * kernel.  Mark this as pager I/O so zfs_write() does not try to
	 * reconcile the very same pages through update_pages().
	 */
	zfs_uio_init(&zuio, uio);
	zuio.uio_extflg |= UIO_PAGER;
	/*
	 * A regular write may hold z_map_lock while waiting for one of these
	 * busy pages.  Never wait for it here: async pageout can retry, while
	 * a synchronous caller receives an I/O error and can retry once the
	 * competing write has completed.
	 */
	if (!mutex_tryenter(&zp->z_map_lock))
		return (EBUSY);
	error = zfs_write(zp, &zuio,
	    (flags & PGO_SYNCIO) != 0 ? O_SYNC : 0, kcred);
	mutex_exit(&zp->z_map_lock);
	return (error);
}

static int
zfs_openbsd_uvp_mmap(vnode_t *vp, vm_prot_t prot, vm_prot_t *maxprot,
    int flags)
{

	KERNEL_ASSERT_LOCKED();
	KASSERT(vp->v_type == VREG);
	(void)prot;
	(void)maxprot;
	(void)flags;
	return (0);
}

const struct uvn_pagerops zfs_uvn_pagerops = {
	.uvp_mmap = zfs_openbsd_uvp_mmap,
	.uvp_read = zfs_openbsd_uvp_read,
	.uvp_write = zfs_openbsd_uvp_write,
};

/*
 * The common ZPL asks only whether it needs to run its cache-coherency hook,
 * so reporting any resident page is conservative and avoids walking the
 * object twice.
 */
boolean_t
zfs_vnode_has_cached_data(vnode_t *vp, uint64_t start, uint64_t end)
{
	struct uvm_object *uobj;
	boolean_t cached;

	(void)start;
	(void)end;
	if (vp->v_uvm == NULL)
		return (B_FALSE);

	uobj = &vp->v_uvm->u_obj;
	rw_enter(uobj->vmobjlock, RW_READ);
	cached = uobj->uo_npages != 0;
	rw_exit(uobj->vmobjlock);
	return (cached);
}

void
zfs_vnode_flush_cached_data(vnode_t *vp, boolean_t sync)
{
	int flags = PGO_CLEANIT | PGO_ALLPAGES;
	boolean_t locked;

	if (sync)
		flags |= PGO_SYNCIO;
	locked = VOP_ISLOCKED(vp);
	if (locked)
		VOP_UNLOCK(vp);
	(void)uvm_vnp_flush(vp, 0, 0, flags);
	if (locked)
		VERIFY0(vn_lock(vp, LK_EXCLUSIVE | LK_RETRY));
}

int
zfs_rlimit_fsize(off_t fsize)
{
	struct proc *p = curproc;

	if (p == NULL || fsize <= lim_cur_proc(p, RLIMIT_FSIZE))
		return (0);
	psignal(p, SIGXFSZ);
	return (EFBIG);
}

int
zfs_rlimit_fsize_uio(znode_t *zp, zfs_uio_t *uio)
{

	(void)zp;
	(void)uio;
	/* Native VOP_WRITE has already applied vn_fsizechk(). */
	return (0);
}

int
mappedread(znode_t *zp, int nbytes, zfs_uio_t *uio)
{
	vnode_t *vp = ZTOV(zp);
	struct uvm_object *uobj;
	struct vm_page *pp;
	void *buf;
	vaddr_t kva;
	uint64_t bytes;
	voff_t start;
	int off;
	int error = 0;

	KASSERT(MUTEX_HELD(&zp->z_map_lock));
	if (vp->v_uvm == NULL)
		return (dmu_read_uio_dbuf(sa_get_db(zp->z_sa_hdl), uio,
		    nbytes, DMU_READ_PREFETCH));

	uobj = &vp->v_uvm->u_obj;
	buf = kmem_alloc(PAGE_SIZE, KM_SLEEP);
	start = zfs_uio_offset(uio);
	off = start & PAGE_MASK;
	for (start = trunc_page(start); nbytes > 0; start += PAGE_SIZE) {
		bytes = MIN((uint64_t)(PAGE_SIZE - off), (uint64_t)nbytes);
retry:
		rw_enter(uobj->vmobjlock, RW_WRITE);
		pp = uvm_pagelookup(uobj, start);
		if (pp != NULL && (pp->pg_flags & PG_BUSY) != 0) {
			uvm_pagewait(pp, uobj->vmobjlock, "zfsmread");
			goto retry;
		}
		if (pp == NULL) {
			rw_exit(uobj->vmobjlock);
			error = dmu_read_uio_dbuf(sa_get_db(zp->z_sa_hdl), uio,
			    bytes, DMU_READ_PREFETCH);
		} else {
			(void)__atomic_fetch_or(&pp->pg_flags, PG_BUSY,
			    __ATOMIC_SEQ_CST);
			UVM_PAGE_OWN(pp, "zfs_mappedread");
			rw_exit(uobj->vmobjlock);

			kva = uvm_pagermapin(&pp, 1,
			    UVMPAGER_MAPIN_WAITOK | UVMPAGER_MAPIN_WRITE);
			memcpy(buf, (void *)(kva + off), bytes);
			uvm_pagermapout(kva, 1);

			rw_enter(uobj->vmobjlock, RW_WRITE);
			uvm_page_unbusy(&pp, 1);
			rw_exit(uobj->vmobjlock);
			error = zfs_uiomove(buf, bytes, UIO_READ, uio);
		}
		if (error != 0)
			break;
		nbytes -= bytes;
		off = 0;
	}
	kmem_free(buf, PAGE_SIZE);
	return (error);
}

void
update_pages(znode_t *zp, int64_t start, int len, objset_t *os)
{
	vnode_t *vp = ZTOV(zp);
	struct uvm_object *uobj;
	struct vm_page *pp;
	boolean_t dirty;
	vaddr_t kva;
	int nbytes;
	int off;
	int error;

	KASSERT(MUTEX_HELD(&zp->z_map_lock));
	if (len <= 0 || vp->v_uvm == NULL)
		return;

	uobj = &vp->v_uvm->u_obj;
	off = start & PAGE_MASK;
	for (start = trunc_page(start); len > 0; start += PAGE_SIZE) {
		nbytes = MIN(PAGE_SIZE - off, len);
retry:
		rw_enter(uobj->vmobjlock, RW_WRITE);
		pp = uvm_pagelookup(uobj, start);
		if (pp != NULL && (pp->pg_flags & PG_BUSY) != 0) {
			uvm_pagewait(pp, uobj->vmobjlock, "zfsupage");
			goto retry;
		}
		if (pp == NULL) {
			rw_exit(uobj->vmobjlock);
			goto next;
		}

		/* Freeze userspace mappings while replacing the written bytes. */
		(void)__atomic_fetch_or(&pp->pg_flags, PG_BUSY,
		    __ATOMIC_SEQ_CST);
		UVM_PAGE_OWN(pp, "zfs_update_pages");
		pmap_page_protect(pp, PROT_READ);
		dirty = (pp->pg_flags & PG_CLEAN) == 0 ||
		    pmap_is_modified(pp);
		rw_exit(uobj->vmobjlock);

		kva = uvm_pagermapin(&pp, 1,
		    UVMPAGER_MAPIN_WAITOK | UVMPAGER_MAPIN_READ);
		error = dmu_read(os, zp->z_id, start + off, nbytes,
		    (void *)(kva + off), DMU_READ_PREFETCH);
		uvm_pagermapout(kva, 1);

		rw_enter(uobj->vmobjlock, RW_WRITE);
		if (error != 0) {
			(void)__atomic_fetch_or(&pp->pg_flags, PG_RELEASED,
			    __ATOMIC_SEQ_CST);
		} else {
			pmap_clear_modify(pp);
			(void)__atomic_fetch_and(&pp->pg_flags, ~PG_CLEANCHK,
			    __ATOMIC_SEQ_CST);
			if (dirty && (off != 0 || nbytes != PAGE_SIZE))
				(void)__atomic_fetch_and(&pp->pg_flags, ~PG_CLEAN,
				    __ATOMIC_SEQ_CST);
			else
				(void)__atomic_fetch_or(&pp->pg_flags, PG_CLEAN,
				    __ATOMIC_SEQ_CST);
		}
		uvm_page_unbusy(&pp, 1);
		rw_exit(uobj->vmobjlock);
next:
		len -= nbytes;
		off = 0;
	}
}

static void
zfs_openbsd_vrele_task(void *arg)
{
	vnode_t *vp = arg;

	KERNEL_LOCK();
	(void)vrele(vp);
	KERNEL_UNLOCK();
}

void
zfs_zrele_async(znode_t *zp)
{
	vnode_t *vp = ZTOV(zp);
	taskq_t *tq;

	tq = dsl_pool_zrele_taskq(dmu_objset_pool(zp->z_zfsvfs->z_os));
	VERIFY3U(taskq_dispatch(tq, zfs_openbsd_vrele_task, vp, TQ_SLEEP),
	    !=, TASKQID_INVALID);
}

int
zfs_write_simple(znode_t *zp, const void *data, size_t len, loff_t pos,
    size_t *presid)
{
	struct iovec iov;
	struct uio uio;
	zfs_uio_t zuio;
	int error;

	KERNEL_ASSERT_LOCKED();
	iov.iov_base = (void *)data;
	iov.iov_len = len;
	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_offset = pos;
	uio.uio_resid = len;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_rw = UIO_WRITE;
	uio.uio_procp = NULL;
	zfs_uio_init(&zuio, &uio);

	mutex_enter(&zp->z_map_lock);
	error = zfs_write(zp, &zuio, O_SYNC, kcred);
	mutex_exit(&zp->z_map_lock);
	if (presid != NULL)
		*presid = uio.uio_resid;
	else if (error == 0 && uio.uio_resid != 0)
		error = EIO;
	return (error);
}

static int
zfs_openbsd_write(void *v)
{
	struct vop_write_args *ap = v;
	vnode_t *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	znode_t *zp = VTOZ(vp);
	zfs_uio_t zuio;
	ssize_t overrun;
	size_t resid;
	uint64_t oldsize;
	int ioflag = 0;
	int error;

	KERNEL_ASSERT_LOCKED();
	KASSERT(VOP_ISLOCKED(vp));
	if (vp->v_type != VREG)
		return (EISDIR);
	if (uio->uio_offset < 0 || uio->uio_iovcnt <= 0)
		return (EINVAL);
	if (uio->uio_resid > (uint64_t)LLONG_MAX - uio->uio_offset)
		return (EFBIG);
	if (uio->uio_resid == 0)
		return (0);

	if (ap->a_ioflag & IO_APPEND) {
		ioflag |= O_APPEND;
		uio->uio_offset = zp->z_size;
	}
	if (ap->a_ioflag & IO_SYNC)
		ioflag |= O_SYNC;

	error = vn_fsizechk(vp, uio, ap->a_ioflag, &overrun);
	if (error != 0)
		return (error);
	resid = uio->uio_resid;
	oldsize = zp->z_size;
	zfs_uio_init(&zuio, uio);
	mutex_enter(&zp->z_map_lock);
	error = zfs_write(zp, &zuio, ioflag, ap->a_cred);
	mutex_exit(&zp->z_map_lock);
	if (resid != uio->uio_resid) {
		VN_KNOTE(vp, NOTE_WRITE |
		    (zp->z_size > oldsize ? NOTE_EXTEND : 0));
	}
	uio->uio_resid += overrun;
	return (error);
}

static int
zfs_openbsd_fsync(void *v)
{
	struct vop_fsync_args *ap = v;
	vnode_t *vp = ap->a_vp;
	boolean_t flushed;
	int error;

	KERNEL_ASSERT_LOCKED();
	KASSERT(VOP_ISLOCKED(vp));
	VOP_UNLOCK(vp);
	flushed = uvm_vnp_flush(vp, 0, 0,
	    PGO_CLEANIT | PGO_SYNCIO | PGO_ALLPAGES);
	error = vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
	if (error != 0)
		return (error);
	if (!flushed)
		return (EIO);
	return (zfs_fsync(VTOZ(vp), ap->a_waitfor, ap->a_cred));
}

/* Legacy znodes keep short links after the fixed znode bonus structure. */
static int
zfs_openbsd_legacy_readlink(znode_t *zp, zfs_uio_t *uio)
{
	dmu_buf_t *bonus = sa_get_db(zp->z_sa_hdl);
	dmu_buf_t *db;
	size_t len = MIN((size_t)zp->z_size, zfs_uio_resid(uio));
	int error;

	if (bonus->db_size >= ZFS_OLD_ZNODE_PHYS_SIZE &&
	    zp->z_size <= bonus->db_size - ZFS_OLD_ZNODE_PHYS_SIZE)
		return (zfs_uiomove((char *)bonus->db_data +
		    ZFS_OLD_ZNODE_PHYS_SIZE, len, UIO_READ, uio));

	error = dmu_buf_hold(zp->z_zfsvfs->z_os, zp->z_id, 0, FTAG, &db,
	    DMU_READ_NO_PREFETCH);
	if (error == 0) {
		error = zfs_uiomove(db->db_data, len, UIO_READ, uio);
		dmu_buf_rele(db, FTAG);
	}
	return (error);
}

static int
zfs_openbsd_readlink(void *v)
{
	struct vop_readlink_args *ap = v;
	vnode_t *vp = ap->a_vp;
	znode_t *zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	zfs_uio_t zuio;
	int error;

	KERNEL_ASSERT_LOCKED();
	KASSERT(VOP_ISLOCKED(vp));
	KASSERT(vp->v_type == VLNK);
	KASSERT(ap->a_uio->uio_offset == 0);
	zfs_uio_init(&zuio, ap->a_uio);
	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	if (zp->z_is_sa)
		error = sa_lookup_uio(zp->z_sa_hdl, SA_ZPL_SYMLINK(zfsvfs),
		    &zuio);
	else
		error = zfs_openbsd_legacy_readlink(zp, &zuio);
	ZFS_ACCESSTIME_STAMP(zfsvfs, zp);
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

static int
zfs_openbsd_lookup(void *v)
{
	struct vop_lookup_args *ap = v;
	vnode_t *dvp = ap->a_dvp;
	vnode_t **vpp = ap->a_vpp;
	struct componentname *cnp = ap->a_cnp;
	znode_t *zdp = VTOZ(dvp);
	zfsvfs_t *zfsvfs = zdp->z_zfsvfs;
	znode_t *zp = NULL;
	char name[NAME_MAX + 1];
	uint64_t parent;
	uid_t duid, tuid;
	gid_t gid;
	int lastcn = (cnp->cn_flags & ISLASTCN) != 0;
	int lockparent = (cnp->cn_flags & LOCKPARENT) != 0;
	int use_cache;
	int error;

	KERNEL_ASSERT_LOCKED();
	KASSERT(VOP_ISLOCKED(dvp));
	cnp->cn_flags &= ~PDIRUNLOCK;
	*vpp = NULL;

	if (dvp->v_type != VDIR)
		return (ENOTDIR);
	if (cnp->cn_namelen > NAME_MAX)
		return (ENAMETOOLONG);

	error = VOP_ACCESS(dvp, VEXEC, cnp->cn_cred, cnp->cn_proc);
	if (error != 0)
		goto out;
	if (lastcn && (dvp->v_mount->mnt_flag & MNT_RDONLY) != 0 &&
	    (cnp->cn_nameiop == DELETE || cnp->cn_nameiop == RENAME)) {
		error = EROFS;
		goto out;
	}

	use_cache = zfsvfs->z_use_namecache && !zfsvfs->z_replay;
	if (use_cache) {
		error = cache_lookup(dvp, vpp, cnp);
		if (error >= 0)
			return (error);
		*vpp = NULL;
	}

	bcopy(cnp->cn_nameptr, name, cnp->cn_namelen);
	name[cnp->cn_namelen] = '\0';
	if (zfsvfs->z_utf8 && u8_validate(name, cnp->cn_namelen, NULL,
	    U8_VALIDATE_ENTIRE, &error) < 0) {
		error = EILSEQ;
		goto out;
	}

	if ((error = zfs_enter_verify_zp(zfsvfs, zdp, FTAG)) != 0)
		goto out;

	if (cnp->cn_flags & ISDOTDOT) {
		if (lastcn && cnp->cn_nameiop == RENAME) {
			error = EINVAL;
			goto exit;
		}
		if (lastcn && cnp->cn_nameiop == DELETE)
			cnp->cn_flags |= SAVENAME;
		if (zdp->z_unlinked) {
			error = ENOENT;
			goto exit;
		}
		error = sa_lookup(zdp->z_sa_hdl, SA_ZPL_PARENT(zfsvfs),
		    &parent, sizeof (parent));
		if (error != 0)
			goto exit;
		if (parent == zdp->z_id) {
			vref(dvp);
			*vpp = dvp;
			goto exit;
		}

		/* Acquire ancestors before descendants, as native namei does. */
		VOP_UNLOCK(dvp);
		error = zfs_zget(zfsvfs, parent, &zp);
		VERIFY0(vn_lock(dvp, LK_EXCLUSIVE | LK_RETRY));
		if (error == 0)
			*vpp = ZTOV(zp);
		goto exit;
	}

	if (cnp->cn_namelen == 1 && name[0] == '.') {
		if (lastcn && cnp->cn_nameiop == RENAME) {
			error = EISDIR;
			goto exit;
		}
		vref(dvp);
		*vpp = dvp;
		goto exit;
	}

	error = zfs_dirent_lookup(zdp, name, &zp, ZEXISTS);
	if (error == ENOENT) {
		zfs_exit(zfsvfs, FTAG);
		if (lastcn && (cnp->cn_nameiop == CREATE ||
		    cnp->cn_nameiop == RENAME)) {
			error = VOP_ACCESS(dvp, VWRITE, cnp->cn_cred,
			    cnp->cn_proc);
			if (error == 0) {
				cnp->cn_flags |= SAVENAME;
				error = EJUSTRETURN;
			}
		} else if (use_cache && (cnp->cn_flags & MAKEENTRY)) {
			cache_enter(dvp, NULL, cnp);
		}
		goto out;
	}
	if (error != 0)
		goto exit;

	zdp->z_zn_prefetch = B_TRUE;
	*vpp = ZTOV(zp);
	if (!lastcn && (*vpp)->v_type != VDIR && (*vpp)->v_type != VLNK) {
		vput(*vpp);
		*vpp = NULL;
		error = ENOTDIR;
		goto exit;
	}

exit:
	zfs_exit(zfsvfs, FTAG);
	if (error != 0)
		goto out;

	if (lastcn && (cnp->cn_nameiop == DELETE ||
	    cnp->cn_nameiop == RENAME)) {
		error = VOP_ACCESS(dvp, VWRITE, cnp->cn_cred, cnp->cn_proc);
		if (error != 0)
			goto badvp;
		if ((zdp->z_mode & S_ISTXT) != 0 && *vpp != dvp) {
			zfs_fuid_map_ids(zdp, cnp->cn_cred, &duid, &gid);
			zfs_fuid_map_ids(VTOZ(*vpp), cnp->cn_cred, &tuid, &gid);
			if (cnp->cn_cred->cr_uid != 0 &&
			    cnp->cn_cred->cr_uid != duid &&
			    cnp->cn_cred->cr_uid != tuid) {
				error = EPERM;
				goto badvp;
			}
		}
		cnp->cn_flags |= SAVENAME;
	}

	if (use_cache && (cnp->cn_flags & MAKEENTRY) &&
	    cnp->cn_nameiop != CREATE)
		cache_enter(dvp, *vpp, cnp);
	goto out;

badvp:
	if (*vpp != NULL) {
		if (*vpp == dvp)
			vrele(*vpp);
		else
			vput(*vpp);
		*vpp = NULL;
	}
out:
	/* A self-parent is also the returned vnode, so it must remain locked. */
	if ((error == 0 || error == EJUSTRETURN) &&
	    *vpp != dvp &&
	    (!lockparent || !lastcn)) {
		VOP_UNLOCK(dvp);
		cnp->cn_flags |= PDIRUNLOCK;
	} else {
		KASSERT(VOP_ISLOCKED(dvp));
	}
	KASSERT((*vpp != NULL && VOP_ISLOCKED(*vpp)) || error != 0);
	return (error);
}

static int
zfs_openbsd_readdir(void *v)
{
	struct vop_readdir_args *ap = v;
	vnode_t *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	znode_t *zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	zap_attribute_t *zap;
	zap_cursor_t zc;
	struct dirent dirent;
	uint64_t parent;
	uint64_t offset;
	uint64_t next;
	uint64_t obj;
	size_t orig_resid;
	size_t namelen;
	size_t reclen;
	int eof = 0;
	int error;

	KERNEL_ASSERT_LOCKED();
	KASSERT(VOP_ISLOCKED(vp));
	if (vp->v_type != VDIR)
		return (ENOTDIR);
	if (uio->uio_offset == -1) {
		if (ap->a_eofflag != NULL)
			*ap->a_eofflag = 1;
		return (0);
	}
	if (uio->uio_iovcnt <= 0)
		return (EINVAL);
	offset = (uint64_t)uio->uio_offset;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);
	if ((error = sa_lookup(zp->z_sa_hdl, SA_ZPL_PARENT(zfsvfs),
	    &parent, sizeof (parent))) != 0)
		goto out;
	if (zp->z_unlinked) {
		eof = 1;
		error = 0;
		goto out;
	}

	orig_resid = uio->uio_resid;
	zap = zap_attribute_long_alloc();
	if (offset <= 2)
		zap_cursor_init(&zc, zfsvfs->z_os, zp->z_id);
	else
		zap_cursor_init_serialized(&zc, zfsvfs->z_os, zp->z_id,
		    offset);

	error = 0;
	while (uio->uio_resid != 0) {
		bzero(&dirent, sizeof (dirent));
		if (offset == 0) {
			dirent.d_fileno = zp->z_id;
			dirent.d_type = DT_DIR;
			dirent.d_name[0] = '.';
			dirent.d_name[1] = '\0';
			namelen = 1;
			next = 1;
		} else if (offset == 1) {
			dirent.d_fileno = parent;
			dirent.d_type = DT_DIR;
			strlcpy(dirent.d_name, "..", sizeof (dirent.d_name));
			namelen = 2;
			next = 2;
		} else {
			error = zap_cursor_retrieve(&zc, zap);
			if (error != 0) {
				if (error == ENOENT) {
					eof = 1;
					error = 0;
				}
				break;
			}
			if (zap->za_integer_length != 8 ||
			    zap->za_num_integers != 1) {
				error = ENXIO;
				break;
			}
			namelen = strlen(zap->za_name);
			if (namelen > MAXNAMLEN) {
				error = EOVERFLOW;
				break;
			}
			obj = zap->za_first_integer;
			dirent.d_fileno = ZFS_DIRENT_OBJ(obj);
			dirent.d_type = ZFS_DIRENT_TYPE(obj);
			bcopy(zap->za_name, dirent.d_name, namelen + 1);
			zap_cursor_advance(&zc);
			next = zap_cursor_serialize(&zc);
		}

		reclen = DIRENT_RECSIZE(namelen);
		if (reclen > uio->uio_resid) {
			if (uio->uio_resid == orig_resid)
				error = EINVAL;
			break;
		}
		dirent.d_namlen = namelen;
		dirent.d_reclen = reclen;
		dirent.d_off = (off_t)next;
		error = uiomove(&dirent, reclen, uio);
		if (error != 0) {
			uio->uio_offset = (off_t)offset;
			break;
		}
		offset = next;
		uio->uio_offset = (off_t)offset;
	}

	zap_cursor_fini(&zc);
	zap_attribute_free(zap);
	zp->z_zn_prefetch = B_FALSE;
	ZFS_ACCESSTIME_STAMP(zfsvfs, zp);
out:
	zfs_exit(zfsvfs, FTAG);
	if (ap->a_eofflag != NULL)
		*ap->a_eofflag = (error == 0 && eof);
	return (error);
}

static int
zfs_openbsd_access(void *v)
{
	struct vop_access_args *ap = v;
	vnode_t *vp = ap->a_vp;
	znode_t *zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	uid_t uid;
	gid_t gid;
	int error;

	KASSERT(VOP_ISLOCKED(vp));
	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	if (ap->a_mode & VWRITE) {
		switch (vp->v_type) {
		case VDIR:
		case VLNK:
		case VREG:
			if (vp->v_mount->mnt_flag & MNT_RDONLY) {
				error = EROFS;
				goto out;
			}
			break;
		case VBLK:
		case VCHR:
		case VSOCK:
		case VFIFO:
			break;
		default:
			error = EINVAL;
			goto out;
		}

		if (zp->z_pflags & ZFS_IMMUTABLE) {
			error = EPERM;
			goto out;
		}
		/* ZFS_READONLY is a DOS attribute and does not affect dirs. */
		if (vp->v_type != VDIR && (zp->z_pflags & ZFS_READONLY)) {
			error = EPERM;
			goto out;
		}
	}

	zfs_fuid_map_ids(zp, ap->a_cred, &uid, &gid);
	error = vaccess(vp->v_type, zp->z_mode & ALLPERMS, uid, gid,
	    ap->a_mode, ap->a_cred);
out:
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

static int
zfs_openbsd_getattr(void *v)
{
	struct vop_getattr_args *ap = v;
	vnode_t *vp = ap->a_vp;
	znode_t *zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	struct vattr *vap = ap->a_vap;
	uint64_t mtime[2], ctime[2], rdev = 0;
	u_longlong_t nblocks;
	uint32_t blksize;
	sa_bulk_attr_t bulk[3];
	uid_t uid;
	gid_t gid;
	int count = 0;
	int error;

	/* OpenBSD permits VOP_GETATTR on an unlocked vnode (e.g. fstat(2)). */
	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MTIME(zfsvfs), NULL,
	    mtime, sizeof (mtime));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_CTIME(zfsvfs), NULL,
	    ctime, sizeof (ctime));
	if (vp->v_type == VBLK || vp->v_type == VCHR) {
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_RDEV(zfsvfs), NULL,
		    &rdev, sizeof (rdev));
	}
	if ((error = sa_bulk_lookup(zp->z_sa_hdl, bulk, count)) != 0)
		goto out;

	zfs_fuid_map_ids(zp, ap->a_cred, &uid, &gid);
	sa_object_size(zp->z_sa_hdl, &blksize, &nblocks);

	vattr_null(vap);
	vap->va_type = vp->v_type;
	vap->va_mode = zp->z_mode & ALLPERMS;
	vap->va_nlink = (nlink_t)zp->z_links;
	vap->va_uid = uid;
	vap->va_gid = gid;
	vap->va_fsid = vp->v_mount->mnt_stat.f_fsid.val[0];
	vap->va_fileid = zp->z_id;
	vap->va_size = zp->z_size;
	vap->va_blocksize = zp->z_blksz != 0 ? blksize : zfsvfs->z_max_blksz;
	ZFS_TIME_DECODE(&vap->va_atime, zp->z_atime);
	ZFS_TIME_DECODE(&vap->va_mtime, mtime);
	ZFS_TIME_DECODE(&vap->va_ctime, ctime);
	vap->va_gen = (u_long)zp->z_gen;
	vap->va_flags = 0;
	if (zp->z_pflags & ZFS_NODUMP)
		vap->va_flags |= UF_NODUMP;
	if (zp->z_pflags & ZFS_OPAQUE)
		vap->va_flags |= UF_OPAQUE;
	if (zp->z_pflags & ZFS_IMMUTABLE)
		vap->va_flags |= SF_IMMUTABLE;
	if (zp->z_pflags & ZFS_APPENDONLY)
		vap->va_flags |= SF_APPEND;
	vap->va_rdev = (vp->v_type == VBLK || vp->v_type == VCHR) ?
	    zfs_cmpldev(rdev) : NODEV;
	vap->va_bytes = nblocks << 9;
	vap->va_filerev = zp->z_seq;
	vap->va_spare = VNOVAL;
out:
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

/*
 * Apply the mode-bit ZPL's mutable attributes.  ACL rewriting and ownership
 * propagation to an external xattr directory are intentionally omitted.
 */
int
zfs_setattr(znode_t *zp, vattr_t *vap, int flags, cred_t *cr,
    zidmap_t *idmap)
{
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	xvattr_t *xvap = (xvattr_t *)vap;
	zfs_fuid_info_t *fuidp = NULL;
	sa_bulk_attr_t bulk[7];
	dmu_tx_t *tx;
	uint64_t new_uid = zp->z_uid;
	uint64_t new_gid = zp->z_gid;
	uint64_t new_mode = zp->z_mode;
	uint64_t mtime[2], ctime[2];
	uint_t mask = vap->va_mask;
	boolean_t fuid_dirtied;
	int count = 0;
	int error;

	(void)idmap;
	KERNEL_ASSERT_LOCKED();
	KASSERT(VOP_ISLOCKED(ZTOV(zp)));
	if (mask == 0)
		return (0);
	if (mask & ~(AT_MODE | AT_UID | AT_GID | AT_SIZE | AT_ATIME |
	    AT_MTIME | AT_CTIME | AT_XVATTR))
		return (EINVAL);
	if (mask & AT_XVATTR) {
		uint32_t supported = XAT0_CREATETIME | XAT0_ARCHIVE |
		    XAT0_SYSTEM | XAT0_READONLY | XAT0_HIDDEN |
		    XAT0_NOUNLINK | XAT0_IMMUTABLE | XAT0_APPENDONLY |
		    XAT0_NODUMP | XAT0_OPAQUE | XAT0_AV_QUARANTINED |
		    XAT0_AV_MODIFIED | XAT0_REPARSE | XAT0_OFFLINE |
		    XAT0_SPARSE | XAT0_PROJINHERIT;

		if (xvap->xva_magic != XVA_MAGIC ||
		    xvap->xva_mapsize != XVA_MAPSIZE)
			return (EINVAL);
		if ((xvap->xva_reqattrmap[0] & ~supported) != 0 ||
		    xvap->xva_reqattrmap[1] != 0 ||
		    xvap->xva_reqattrmap[2] != 0)
			return (EOPNOTSUPP);
	}
	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);
	if (zfs_is_readonly(zfsvfs)) {
		error = EROFS;
		goto out;
	}
	if ((mask & AT_SIZE) != 0) {
		if (ZTOV(zp)->v_type == VDIR) {
			error = EISDIR;
			goto out;
		}
		if (ZTOV(zp)->v_type != VREG) {
			error = EINVAL;
			goto out;
		}
		if (vap->va_size > MAXOFFSET_T) {
			error = EFBIG;
			goto out;
		}
		error = zfs_freesp(zp, vap->va_size, 0, 0, B_FALSE);
		if (error != 0)
			goto out;
	}

	if (mask & AT_UID) {
		new_uid = zfs_fuid_create(zfsvfs, vap->va_uid, cr,
		    ZFS_OWNER, &fuidp);
		if (new_uid != zp->z_uid && zfs_id_overquota(zfsvfs,
		    DMU_USERUSED_OBJECT, new_uid)) {
			error = EDQUOT;
			goto out;
		}
	}
	if (mask & AT_GID) {
		new_gid = zfs_fuid_create(zfsvfs, vap->va_gid, cr,
		    ZFS_GROUP, &fuidp);
		if (new_gid != zp->z_gid && zfs_id_overquota(zfsvfs,
		    DMU_GROUPUSED_OBJECT, new_gid)) {
			error = EDQUOT;
			goto out;
		}
	}
	if (mask & AT_MODE)
		new_mode = (zp->z_mode & S_IFMT) | (vap->va_mode & ALLPERMS);
	if (crgetuid(cr) != 0 &&
	    (((mask & AT_UID) && new_uid != zp->z_uid) ||
	    ((mask & AT_GID) && new_gid != zp->z_gid))) {
		new_mode &= ~(S_ISUID | S_ISGID);
		mask |= AT_MODE;
		vap->va_mask |= AT_MODE;
		vap->va_mode = new_mode & ALLPERMS;
	}

	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
	fuid_dirtied = zfsvfs->z_fuid_dirty;
	if (fuid_dirtied)
		zfs_fuid_txhold(zfsvfs, tx);
	zfs_sa_upgrade_txholds(tx, zp);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error != 0) {
		dmu_tx_abort(tx);
		goto out;
	}

	mutex_enter(&zp->z_acl_lock);
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_FLAGS(zfsvfs), NULL,
	    &zp->z_pflags, sizeof (zp->z_pflags));
	if (mask & AT_UID) {
		zp->z_uid = new_uid;
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_UID(zfsvfs), NULL,
		    &zp->z_uid, sizeof (zp->z_uid));
	}
	if (mask & AT_GID) {
		zp->z_gid = new_gid;
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_GID(zfsvfs), NULL,
		    &zp->z_gid, sizeof (zp->z_gid));
	}
	if (mask & AT_MODE) {
		zp->z_mode = (mode_t)new_mode;
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MODE(zfsvfs), NULL,
		    &new_mode, sizeof (new_mode));
	}
	mutex_exit(&zp->z_acl_lock);

	if (mask & AT_ATIME) {
		ZFS_TIME_ENCODE(&vap->va_atime, zp->z_atime);
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_ATIME(zfsvfs), NULL,
		    zp->z_atime, sizeof (zp->z_atime));
	}
	if (mask & AT_MTIME) {
		ZFS_TIME_ENCODE(&vap->va_mtime, mtime);
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MTIME(zfsvfs), NULL,
		    mtime, sizeof (mtime));
	}
	if ((mask & AT_SIZE) && !(mask & AT_MTIME)) {
		zfs_tstamp_update_setup(zp, CONTENT_MODIFIED, mtime, ctime);
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MTIME(zfsvfs), NULL,
		    mtime, sizeof (mtime));
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_CTIME(zfsvfs), NULL,
		    ctime, sizeof (ctime));
	} else {
		zfs_tstamp_update_setup(zp, STATE_CHANGED, mtime, ctime);
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_CTIME(zfsvfs), NULL,
		    ctime, sizeof (ctime));
	}
	if (mask & AT_XVATTR)
		zfs_xvattr_set(zp, xvap, tx);
	if (fuid_dirtied)
		zfs_fuid_sync(zfsvfs, tx);
	zfs_log_setattr(zfsvfs->z_log, tx, TX_SETATTR, zp, vap, mask, fuidp);
	VERIFY0(sa_bulk_update(zp->z_sa_hdl, bulk, count, tx));
	dmu_tx_commit(tx);
	error = 0;

out:
	if (fuidp != NULL)
		zfs_fuid_info_free(fuidp);
	if (error == 0 && zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		error = zil_commit(zfsvfs->z_log, 0);
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

int
zfs_space(znode_t *zp, int cmd, flock64_t *fl, int flag, offset_t offset,
    cred_t *cr)
{
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	uint64_t off, len;
	int error;

	(void)offset;
	KERNEL_ASSERT_LOCKED();
	KASSERT(VOP_ISLOCKED(ZTOV(zp)));
	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);
	if (cmd != F_FREESP || fl->l_whence != SEEK_SET ||
	    fl->l_start < 0 || fl->l_len < 0) {
		error = EINVAL;
		goto out;
	}
	if (zfs_is_readonly(zfsvfs)) {
		error = EROFS;
		goto out;
	}
	if (ZTOV(zp)->v_type != VREG) {
		error = EINVAL;
		goto out;
	}
	if ((zp->z_pflags & (ZFS_IMMUTABLE | ZFS_APPENDONLY)) != 0) {
		error = EPERM;
		goto out;
	}
	if (!zfsvfs->z_replay && (error = zfs_zaccess(zp, ACE_WRITE_DATA,
	    0, B_FALSE, cr, NULL)) != 0)
		goto out;
	off = fl->l_start;
	len = fl->l_len;
	if (off > MAXOFFSET_T || (len != 0 && len > MAXOFFSET_T - off)) {
		error = EFBIG;
		goto out;
	}
	error = zfs_freesp(zp, off, len, flag, B_TRUE);
out:
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

static int
zfs_openbsd_setattr(void *v)
{
	struct vop_setattr_args *ap = v;
	vnode_t *vp = ap->a_vp;
	znode_t *zp = VTOZ(vp);
	struct vattr *vap = ap->a_vap;
	xvattr_t xva;
	vattr_t *zvap = &xva.xva_vattr;
	uid_t owner;
	gid_t group;
	uint_t mask = 0;
	u_long allowed_flags = UF_NODUMP | UF_IMMUTABLE | UF_APPEND |
	    UF_OPAQUE | SF_IMMUTABLE | SF_APPEND;
	uint64_t oldsize = zp->z_size;
	boolean_t privileged;
	int error;

	KERNEL_ASSERT_LOCKED();
	KASSERT(VOP_ISLOCKED(vp));
	if (vap->va_type != VNON || vap->va_nlink != VNOVAL ||
	    vap->va_fsid != VNOVAL || vap->va_fileid != VNOVAL ||
	    vap->va_blocksize != VNOVAL || vap->va_ctime.tv_nsec != VNOVAL ||
	    vap->va_gen != VNOVAL || vap->va_rdev != VNOVAL ||
	    vap->va_bytes != VNOVAL)
		return (EINVAL);
	if (vp->v_mount->mnt_flag & MNT_RDONLY)
		return (EROFS);

	zfs_fuid_map_ids(zp, ap->a_cred, &owner, &group);
	privileged = ap->a_cred->cr_uid == 0 || vnoperm(vp);
	if (vap->va_uid != (uid_t)VNOVAL || vap->va_gid != (gid_t)VNOVAL) {
		uid_t uid = vap->va_uid == (uid_t)VNOVAL ? owner : vap->va_uid;
		gid_t gid = vap->va_gid == (gid_t)VNOVAL ? group : vap->va_gid;

		if (!privileged && (ap->a_cred->cr_uid != owner || uid != owner ||
		    (gid != group && !groupmember(gid, ap->a_cred))))
			return (EPERM);
		if (vap->va_uid != (uid_t)VNOVAL)
			mask |= AT_UID;
		if (vap->va_gid != (gid_t)VNOVAL)
			mask |= AT_GID;
	}
	if (vap->va_mode != (mode_t)VNOVAL) {
		if (!privileged && ap->a_cred->cr_uid != owner)
			return (EPERM);
		if (!privileged && vp->v_type != VDIR &&
		    (vap->va_mode & S_ISTXT))
			return (EFTYPE);
		if (!privileged && (vap->va_mode & S_ISGID) &&
		    !groupmember(group, ap->a_cred))
			return (EPERM);
		mask |= AT_MODE;
	}
	if (vap->va_size != VNOVAL) {
		if (vp->v_type == VDIR)
			return (EISDIR);
		if (vp->v_type != VREG)
			return (EINVAL);
		if (vap->va_size > MAXOFFSET_T)
			return (EFBIG);
		if (!privileged && (error = vaccess(vp->v_type,
		    zp->z_mode & ALLPERMS, owner, group, VWRITE,
		    ap->a_cred)) != 0)
			return (error);
		mask |= AT_SIZE;
	}
	if (vap->va_atime.tv_nsec != VNOVAL)
		mask |= AT_ATIME;
	if (vap->va_mtime.tv_nsec != VNOVAL)
		mask |= AT_MTIME;
	if (vap->va_vaflags & VA_UTIMES_CHANGE)
		mask |= AT_CTIME;
	if (mask & (AT_ATIME | AT_MTIME | AT_CTIME)) {
		if (!privileged && ap->a_cred->cr_uid != owner &&
		    (vap->va_vaflags & VA_UTIMES_NULL) == 0)
			return (EPERM);
		if (!privileged && ap->a_cred->cr_uid != owner &&
		    (error = vaccess(vp->v_type, zp->z_mode & ALLPERMS,
		    owner, group, VWRITE, ap->a_cred)) != 0)
			return (error);
	}

	xva_init(&xva);
	*zvap = *vap;
	zvap->va_mask = mask;
	if (vap->va_flags != VNOVAL) {
		uint64_t zflags = zp->z_pflags;
		u_long fflags = vap->va_flags;

		if (fflags & ~allowed_flags)
			return (EOPNOTSUPP);
		if (!privileged && ap->a_cred->cr_uid != owner)
			return (EPERM);
		if (!privileged && (fflags & SF_SETTABLE))
			return (EPERM);
		if ((zflags & (ZFS_IMMUTABLE | ZFS_APPENDONLY)) != 0 &&
		    securelevel > 0)
			return (EPERM);
		zvap->va_mask |= AT_XVATTR;
#define	ZFS_FLAG_CHANGE(fflag, zflag, xat, field) do {             \
	if (((fflags & (fflag)) != 0) != ((zflags & (zflag)) != 0)) { \
		XVA_SET_REQ(&xva, (xat));                                \
		xva.xva_xoptattrs.field = (fflags & (fflag)) != 0;       \
	}                                                              \
} while (0)
		ZFS_FLAG_CHANGE(IMMUTABLE, ZFS_IMMUTABLE,
		    XAT_IMMUTABLE, xoa_immutable);
		ZFS_FLAG_CHANGE(APPEND, ZFS_APPENDONLY,
		    XAT_APPENDONLY, xoa_appendonly);
		ZFS_FLAG_CHANGE(UF_NODUMP, ZFS_NODUMP,
		    XAT_NODUMP, xoa_nodump);
		ZFS_FLAG_CHANGE(UF_OPAQUE, ZFS_OPAQUE,
		    XAT_OPAQUE, xoa_opaque);
#undef ZFS_FLAG_CHANGE
		if ((fflags & (IMMUTABLE | APPEND)) != 0 &&
		    (zvap->va_mask & ~AT_XVATTR) != 0)
			return (EPERM);
	}
	if ((zp->z_pflags & (ZFS_IMMUTABLE | ZFS_APPENDONLY)) != 0 &&
	    vap->va_flags == VNOVAL && zvap->va_mask != 0)
		return (EPERM);

	error = zfs_setattr(zp, zvap, ATTR_NOACLCHECK, ap->a_cred, NULL);
	if (error == 0 && zvap->va_mask != 0) {
		long note = NOTE_ATTRIB;

		if ((zvap->va_mask & AT_SIZE) && vap->va_size < oldsize)
			note |= NOTE_TRUNCATE;
		else if ((zvap->va_mask & AT_SIZE) && vap->va_size > oldsize)
			note |= NOTE_EXTEND;
		VN_KNOTE(vp, note);
	}
	return (error);
}

static int
zfs_openbsd_pathconf(void *v)
{
	struct vop_pathconf_args *ap = v;

	KASSERT(VOP_ISLOCKED(ap->a_vp));
	switch (ap->a_name) {
	case _PC_LINK_MAX:
		*ap->a_retval = ZFS_LINK_MAX;
		break;
	case _PC_NAME_MAX:
		*ap->a_retval = NAME_MAX;
		break;
	case _PC_CHOWN_RESTRICTED:
	case _PC_NO_TRUNC:
	case _PC_2_SYMLINKS:
		*ap->a_retval = 1;
		break;
	case _PC_FILESIZEBITS:
		*ap->a_retval = 64;
		break;
	case _PC_SYMLINK_MAX:
		*ap->a_retval = MAXPATHLEN;
		break;
	case _PC_TIMESTAMP_RESOLUTION:
		*ap->a_retval = 1;
		break;
	default:
		return (EINVAL);
	}
	return (0);
}

static int
zfs_openbsd_lock(void *v)
{
	struct vop_lock_args *ap = v;
	znode_t *zp = VTOZ(ap->a_vp);

	return (rrw_enter(&zp->z_vlock, ap->a_flags & LK_RWFLAGS));
}

static int
zfs_openbsd_unlock(void *v)
{
	struct vop_unlock_args *ap = v;
	znode_t *zp = VTOZ(ap->a_vp);

	rrw_exit(&zp->z_vlock);
	return (0);
}

static int
zfs_openbsd_islocked(void *v)
{
	struct vop_islocked_args *ap = v;

	return (rrw_status(&VTOZ(ap->a_vp)->z_vlock));
}

static int
zfs_openbsd_inactive(void *v)
{
	struct vop_inactive_args *ap = v;
	vnode_t *vp = ap->a_vp;
	znode_t *zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	boolean_t recycle;

	ZFS_TEARDOWN_INACTIVE_ENTER_READ(zfsvfs);
	if (zp->z_sa_hdl != NULL && zp->z_unlinked &&
	    (zfsvfs->z_vfs->vfs_flag & VFS_RDONLY) == 0)
		zfs_rmnode(zp);
	recycle = (zp->z_sa_hdl == NULL || zp->z_unlinked);
	ZFS_TEARDOWN_INACTIVE_EXIT_READ(zfsvfs);
	VOP_UNLOCK(vp);
	if (recycle)
		(void)vrecycle(vp, ap->a_p);
	return (0);
}

static int
zfs_openbsd_reclaim(void *v)
{
	struct vop_reclaim_args *ap = v;
	vnode_t *vp = ap->a_vp;
	znode_t *zp = VTOZ(vp);
	zfsvfs_t *zfsvfs;

	if (zp == NULL)
		return (0);
	zfsvfs = zp->z_zfsvfs;

	/* Interrupt any waiters and discard locks before detaching the znode. */
	lf_purgelocks(&zp->z_lockf);
	ZFS_TEARDOWN_INACTIVE_ENTER_READ(zfsvfs);
	ZFS_OBJ_HOLD_ENTER(zfsvfs, zp->z_id);
	vp->v_data = NULL;
	zp->z_vnode = NULL;
	if (zp->z_sa_hdl != NULL)
		zfs_znode_dmu_fini(zp);
	ZFS_OBJ_HOLD_EXIT(zfsvfs, zp->z_id);
	zfs_znode_free(zp);
	ZFS_TEARDOWN_INACTIVE_EXIT_READ(zfsvfs);

	return (0);
}

static int
zfs_openbsd_print(void *v)
{
	struct vop_print_args *ap = v;
	znode_t *zp = VTOZ(ap->a_vp);

	printf("tag ZFS, object %llu, gen %llu\n",
	    (unsigned long long)zp->z_id,
	    (unsigned long long)zp->z_gen);
	return (0);
}

static int
zfs_openbsd_advlock(void *v)
{
	struct vop_advlock_args *ap = v;
	znode_t *zp = VTOZ(ap->a_vp);

	return (lf_advlock(&zp->z_lockf, zp->z_size, ap->a_id, ap->a_op,
	    ap->a_fl, ap->a_flags));
}

#ifdef FIFO
static int
zfs_openbsd_fifo_read(void *v)
{
	struct vop_read_args *ap = v;
	znode_t *zp = VTOZ(ap->a_vp);
	size_t resid = ap->a_uio->uio_resid;
	int error;

	error = fifo_read(v);
	if (resid != ap->a_uio->uio_resid)
		ZFS_ACCESSTIME_STAMP(zp->z_zfsvfs, zp);
	return (error);
}

static int
zfs_openbsd_fifo_write(void *v)
{
	struct vop_write_args *ap = v;
	size_t resid = ap->a_uio->uio_resid;
	int error;

	error = fifo_write(v);
	if (resid != ap->a_uio->uio_resid)
		VN_KNOTE(ap->a_vp, NOTE_WRITE);
	return (error);
}

static int
zfs_openbsd_fifo_reclaim(void *v)
{

	(void)fifo_reclaim(v);
	return (zfs_openbsd_reclaim(v));
}

const struct vops zfs_fifovops = {
	.vop_lookup	= vop_generic_lookup,
	.vop_create	= vop_generic_badop,
	.vop_mknod	= vop_generic_badop,
	.vop_open	= fifo_open,
	.vop_close	= fifo_close,
	.vop_access	= zfs_openbsd_access,
	.vop_getattr	= zfs_openbsd_getattr,
	.vop_setattr	= zfs_openbsd_setattr,
	.vop_read	= zfs_openbsd_fifo_read,
	.vop_write	= zfs_openbsd_fifo_write,
	.vop_ioctl	= fifo_ioctl,
	.vop_kqfilter	= fifo_kqfilter,
	.vop_revoke	= vop_generic_revoke,
	.vop_fsync	= zfs_openbsd_fsync,
	.vop_remove	= vop_generic_badop,
	.vop_link	= vop_generic_badop,
	.vop_rename	= vop_generic_badop,
	.vop_mkdir	= vop_generic_badop,
	.vop_rmdir	= vop_generic_badop,
	.vop_symlink	= vop_generic_badop,
	.vop_readdir	= vop_generic_badop,
	.vop_readlink	= vop_generic_badop,
	.vop_abortop	= vop_generic_badop,
	.vop_inactive	= zfs_openbsd_inactive,
	.vop_reclaim	= zfs_openbsd_fifo_reclaim,
	.vop_lock	= zfs_openbsd_lock,
	.vop_unlock	= zfs_openbsd_unlock,
	.vop_islocked	= zfs_openbsd_islocked,
	.vop_bmap	= vop_generic_bmap,
	.vop_strategy	= vop_generic_badop,
	.vop_print	= zfs_openbsd_print,
	.vop_pathconf	= fifo_pathconf,
	.vop_advlock	= fifo_advlock,
	.vop_bwrite	= zfs_openbsd_unsupported,
};
#endif /* FIFO */

const struct vops zfs_vnodeops = {
	.vop_lookup	= zfs_openbsd_lookup,
	.vop_create	= zfs_openbsd_create,
	.vop_mknod	= zfs_openbsd_mknod,
	.vop_open	= zfs_openbsd_open,
	.vop_close	= zfs_openbsd_close,
	.vop_access	= zfs_openbsd_access,
	.vop_getattr	= zfs_openbsd_getattr,
	.vop_setattr	= zfs_openbsd_setattr,
	.vop_read	= zfs_openbsd_read,
	.vop_write	= zfs_openbsd_write,
	.vop_ioctl	= zfs_openbsd_unsupported,
	.vop_kqfilter	= zfs_openbsd_unsupported,
	.vop_revoke	= vop_generic_revoke,
	.vop_fsync	= zfs_openbsd_fsync,
	.vop_remove	= zfs_openbsd_remove,
	.vop_link	= zfs_openbsd_link,
	.vop_rename	= zfs_openbsd_rename,
	.vop_mkdir	= zfs_openbsd_mkdir,
	.vop_rmdir	= zfs_openbsd_rmdir,
	.vop_symlink	= zfs_openbsd_symlink,
	.vop_readdir	= zfs_openbsd_readdir,
	.vop_readlink	= zfs_openbsd_readlink,
	.vop_abortop	= vop_generic_abortop,
	.vop_inactive	= zfs_openbsd_inactive,
	.vop_reclaim	= zfs_openbsd_reclaim,
	.vop_lock	= zfs_openbsd_lock,
	.vop_unlock	= zfs_openbsd_unlock,
	.vop_islocked	= zfs_openbsd_islocked,
	.vop_bmap	= zfs_openbsd_unsupported,
	.vop_strategy	= zfs_openbsd_unsupported,
	.vop_print	= zfs_openbsd_print,
	.vop_pathconf	= zfs_openbsd_pathconf,
	.vop_advlock	= zfs_openbsd_advlock,
	.vop_bwrite	= zfs_openbsd_unsupported,
};
