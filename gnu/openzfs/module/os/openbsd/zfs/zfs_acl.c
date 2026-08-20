// SPDX-License-Identifier: CDDL-1.0
/* Unix-mode authorization for the initial OpenBSD ZPL. */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/acl.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <sys/zfs_acl.h>
#include <sys/zfs_dir.h>
#include <sys/zfs_fuid.h>
#include <sys/zfs_quota.h>
#include <sys/zfs_znode.h>

int
zfs_acl_ids_create(znode_t *dzp, int flag, vattr_t *vap, cred_t *cr,
    vsecattr_t *vsecp, zfs_acl_ids_t *ids, zidmap_t *idmap)
{
	gid_t gid;

	(void)vsecp;
	(void)idmap;
	memset(ids, 0, sizeof (*ids));
	ids->z_mode = MAKEIMODE(vap->va_type, vap->va_mode);

	if ((flag & IS_ROOT_NODE) || dzp->z_zfsvfs->z_replay) {
		ids->z_fuid = vap->va_uid;
		ids->z_fgid = vap->va_gid;
		return (0);
	}

	ids->z_fuid = crgetuid(cr);
	gid = zfs_fuid_map_id(dzp->z_zfsvfs, dzp->z_gid, cr, ZFS_GROUP);
	if ((vap->va_mask & AT_GID) != 0 &&
	    (groupmember(vap->va_gid, cr) || suser_ucred(cr) == 0))
		gid = vap->va_gid;
	ids->z_fgid = gid;
	if ((dzp->z_mode & S_ISGID) != 0) {
		ids->z_fgid = dzp->z_gid;
		if (vap->va_type == VDIR)
			ids->z_mode |= S_ISGID;
	} else if ((ids->z_mode & S_ISGID) != 0 &&
	    !groupmember(gid, cr) && suser_ucred(cr) != 0) {
		ids->z_mode &= ~S_ISGID;
	}
	return (0);
}

void
zfs_acl_ids_free(zfs_acl_ids_t *ids)
{

	if (ids->z_fuidp != NULL)
		zfs_fuid_info_free(ids->z_fuidp);
	ids->z_fuidp = NULL;
	ids->z_aclp = NULL;
}

boolean_t
zfs_acl_ids_overquota(zfsvfs_t *zfsvfs, zfs_acl_ids_t *ids, uint64_t projid)
{

	return (zfs_id_overquota(zfsvfs, DMU_USERUSED_OBJECT, ids->z_fuid) ||
	    zfs_id_overquota(zfsvfs, DMU_GROUPUSED_OBJECT, ids->z_fgid) ||
	    (projid != ZFS_DEFAULT_PROJID && projid != ZFS_INVALID_PROJID &&
	    zfs_id_overquota(zfsvfs, DMU_PROJECTUSED_OBJECT, projid)));
}

uint64_t
zfs_external_acl(znode_t *zp)
{
	zfs_acl_phys_t acl;

	if (zp->z_is_sa || sa_lookup(zp->z_sa_hdl,
	    SA_ZPL_ZNODE_ACL(zp->z_zfsvfs), &acl, sizeof (acl)) != 0)
		return (0);
	return (acl.z_acl_extern_obj);
}

static int
zfs_openbsd_vaccess(znode_t *zp, int mode, cred_t *cr)
{
	uid_t uid;
	gid_t gid;

	if ((mode & VWRITE) != 0 &&
	    ((zp->z_pflags & ZFS_IMMUTABLE) != 0 ||
	    (ZTOV(zp)->v_type != VDIR &&
	    (zp->z_pflags & ZFS_READONLY) != 0)))
		return (EPERM);
	zfs_fuid_map_ids(zp, cr, &uid, &gid);
	return (vaccess(ZTOV(zp)->v_type, zp->z_mode & ALLPERMS, uid, gid,
	    mode, cr));
}

int
zfs_zaccess_rwx(znode_t *zp, mode_t mode, int flags, cred_t *cr,
    zidmap_t *idmap)
{

	(void)flags;
	(void)idmap;
	return (zfs_openbsd_vaccess(zp, mode, cr));
}

int
zfs_zaccess(znode_t *zp, int ace, int flags, boolean_t skipaclchk,
    cred_t *cr, zidmap_t *idmap)
{
	int mode = 0;

	(void)flags;
	(void)idmap;
	if (skipaclchk)
		return (0);
	if (ace & (ACE_READ_DATA | ACE_READ_NAMED_ATTRS | ACE_READ_ACL |
	    ACE_READ_ATTRIBUTES))
		mode |= VREAD;
	if (ace & (ACE_WRITE_DATA | ACE_APPEND_DATA | ACE_ADD_FILE |
	    ACE_ADD_SUBDIRECTORY | ACE_DELETE_CHILD | ACE_WRITE_NAMED_ATTRS |
	    ACE_WRITE_ATTRIBUTES | ACE_WRITE_ACL | ACE_WRITE_OWNER | ACE_DELETE))
		mode |= VWRITE;
	if (ace & ACE_EXECUTE)
		mode |= VEXEC;
	return (zfs_openbsd_vaccess(zp, mode, cr));
}

int
zfs_zaccess_delete(znode_t *dzp, znode_t *zp, cred_t *cr, zidmap_t *idmap)
{
	uid_t duid, zuid;
	gid_t gid;
	int error;

	(void)idmap;
	if ((dzp->z_pflags & ZFS_APPENDONLY) != 0 ||
	    (zp->z_pflags & (ZFS_NOUNLINK | ZFS_IMMUTABLE | ZFS_APPENDONLY)) != 0)
		return (EPERM);
	if ((error = zfs_openbsd_vaccess(dzp, VWRITE | VEXEC, cr)) != 0)
		return (error);
	if ((dzp->z_mode & S_ISTXT) != 0 && crgetuid(cr) != 0) {
		zfs_fuid_map_ids(dzp, cr, &duid, &gid);
		zfs_fuid_map_ids(zp, cr, &zuid, &gid);
		if (crgetuid(cr) != duid && crgetuid(cr) != zuid)
			return (EPERM);
	}
	return (0);
}

int
zfs_zaccess_rename(znode_t *sdzp, znode_t *szp, znode_t *tdzp,
    znode_t *tzp, cred_t *cr, zidmap_t *idmap)
{
	int ace;
	int error;

	if ((szp->z_pflags & ZFS_AV_QUARANTINED) != 0)
		return (EACCES);
	if (ZTOV(szp)->v_type == VDIR && sdzp != tdzp &&
	    (error = zfs_openbsd_vaccess(szp, VWRITE, cr)) != 0)
		return (error);
	if ((error = zfs_zaccess_delete(sdzp, szp, cr, idmap)) != 0)
		return (error);
	if (tzp != NULL &&
	    (error = zfs_zaccess_delete(tdzp, tzp, cr, idmap)) != 0)
		return (error);
	ace = ZTOV(szp)->v_type == VDIR ? ACE_ADD_SUBDIRECTORY : ACE_ADD_FILE;
	if ((error = zfs_zaccess(tdzp, ace, 0, B_FALSE, cr, idmap)) != 0)
		return (error);
	return (zfs_openbsd_vaccess(tdzp, VEXEC, cr));
}

int
zfs_getacl(znode_t *zp, vsecattr_t *vsecp, boolean_t skip, cred_t *cr)
{

	(void)zp;
	(void)vsecp;
	(void)skip;
	(void)cr;
	return (EOPNOTSUPP);
}

int
zfs_setacl(znode_t *zp, vsecattr_t *vsecp, boolean_t skip, cred_t *cr)
{

	(void)zp;
	(void)vsecp;
	(void)skip;
	(void)cr;
	/* ACL log records are accepted; authorization remains mode-only. */
	return (0);
}

/*
 * The initial OpenBSD port deliberately uses only the Unix mode bits.  An
 * absent cached ACL is therefore represented as ENOENT to callers which can
 * legitimately proceed without one (notably SA project-ID upgrades).
 */
int
zfs_acl_node_read(znode_t *zp, boolean_t have_lock, zfs_acl_t **aclpp,
    boolean_t will_modify)
{

	(void)zp;
	(void)have_lock;
	(void)will_modify;
	*aclpp = NULL;
	return (ENOENT);
}

void
zfs_acl_xform(znode_t *zp, zfs_acl_t *aclp, cred_t *cr)
{

	(void)zp;
	(void)aclp;
	(void)cr;
}

void
zfs_acl_data_locator(void **dataptr, uint32_t *length, uint32_t buflen,
    boolean_t start, void *userdata)
{

	(void)buflen;
	(void)start;
	(void)userdata;
	*dataptr = NULL;
	*length = 0;
}
