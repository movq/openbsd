// SPDX-License-Identifier: CDDL-1.0

/* Native OpenBSD VFS lifecycle and registration. */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kthread.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/kmem.h>
#include <sys/kstat.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_prop.h>
#include <sys/fs/zfs.h>
#include <sys/rrwlock.h>
#include <sys/sa.h>
#include <sys/spa.h>
#include <sys/taskq.h>
#include <sys/txg.h>
#include <sys/u8_textprep.h>
#include <sys/zap.h>
#include <sys/zfs_dir.h>
#include <sys/zfs_fuid.h>
#include <sys/zfs_ioctl_impl.h>
#include <sys/zfs_quota.h>
#include <sys/zfs_vnops.h>
#include <sys/zfs_znode.h>
#include <sys/zfs_vfsops_os.h>

#include "zfs_comutil.h"

static int zfs_vfs_root(struct mount *, struct vnode **);
static boolean_t zfs_runtime_initialized;
static boolean_t zfs_runtime_init_deferred;

static void
zfs_allow_log_destroy_openbsd(void *arg)
{
	if (arg != NULL)
		kmem_strfree(arg);
}

void
zfs_init(void)
{
	zfs_znode_init();
	dmu_objset_register_type(DMU_OST_ZFS, zpl_get_file_info);
}

void
zfs_fini(void)
{
	zfs_znode_fini();
}

/*
 * Associate an owned ZFS objset with its in-kernel filesystem state.  This
 * is kept separate from the native mount operation so online receive and
 * suspend/resume can eventually use the same lifecycle.
 */
static int
zfsvfs_init(zfsvfs_t *zfsvfs, objset_t *os)
{
	uint64_t sa_obj = 0;
	uint64_t val;
	int error;

	zfsvfs->z_max_blksz = SPA_OLD_MAXBLOCKSIZE;
	zfsvfs->z_show_ctldir = ZFS_SNAPDIR_VISIBLE;
	zfsvfs->z_os = os;

	error = zfs_get_zplprop(os, ZFS_PROP_VERSION, &zfsvfs->z_version);
	if (error != 0)
		return (error);
	if (zfsvfs->z_version >
	    zfs_zpl_version_map(spa_version(dmu_objset_spa(os)))) {
		printf("zfs: ZPL version %llu is newer than pool version %llu\n",
		    (u_longlong_t)zfsvfs->z_version,
		    (u_longlong_t)spa_version(dmu_objset_spa(os)));
		return (ENOTSUP);
	}

	error = zfs_get_zplprop(os, ZFS_PROP_NORMALIZE, &val);
	if (error != 0)
		return (error);
	zfsvfs->z_norm = (int)val;

	error = zfs_get_zplprop(os, ZFS_PROP_UTF8ONLY, &val);
	if (error != 0)
		return (error);
	zfsvfs->z_utf8 = (val != 0);

	error = zfs_get_zplprop(os, ZFS_PROP_CASE, &val);
	if (error != 0)
		return (error);
	zfsvfs->z_case = (zfs_case_t)val;

	error = zfs_get_zplprop(os, ZFS_PROP_ACLTYPE, &val);
	if (error != 0)
		return (error);
	zfsvfs->z_acl_type = (uint_t)val;

	if (zfsvfs->z_case == ZFS_CASE_INSENSITIVE ||
	    zfsvfs->z_case == ZFS_CASE_MIXED)
		zfsvfs->z_norm |= U8_TEXTPREP_TOUPPER;

	zfsvfs->z_use_fuids = USE_FUIDS(zfsvfs->z_version, os);
	zfsvfs->z_use_sa = USE_SA(zfsvfs->z_version, os);
	if (zfsvfs->z_use_sa) {
		error = zap_lookup(os, MASTER_NODE_OBJ, ZFS_SA_ATTRS, 8, 1,
		    &sa_obj);
		if (error != 0)
			return (error);

		error = zfs_get_zplprop(os, ZFS_PROP_XATTR, &val);
		if (error == 0 && val == ZFS_XATTR_SA)
			zfsvfs->z_xattr_sa = B_TRUE;
	}

	error = zfs_get_zplprop(os, ZFS_PROP_DEFAULTUSERQUOTA,
	    &zfsvfs->z_defaultuserquota);
	if (error != 0)
		return (error);
	error = zfs_get_zplprop(os, ZFS_PROP_DEFAULTGROUPQUOTA,
	    &zfsvfs->z_defaultgroupquota);
	if (error != 0)
		return (error);
	error = zfs_get_zplprop(os, ZFS_PROP_DEFAULTPROJECTQUOTA,
	    &zfsvfs->z_defaultprojectquota);
	if (error != 0)
		return (error);
	error = zfs_get_zplprop(os, ZFS_PROP_DEFAULTUSEROBJQUOTA,
	    &zfsvfs->z_defaultuserobjquota);
	if (error != 0)
		return (error);
	error = zfs_get_zplprop(os, ZFS_PROP_DEFAULTGROUPOBJQUOTA,
	    &zfsvfs->z_defaultgroupobjquota);
	if (error != 0)
		return (error);
	error = zfs_get_zplprop(os, ZFS_PROP_DEFAULTPROJECTOBJQUOTA,
	    &zfsvfs->z_defaultprojectobjquota);
	if (error != 0)
		return (error);

	error = sa_setup(os, sa_obj, zfs_attr_table, ZPL_END,
	    &zfsvfs->z_attr_table);
	if (error != 0)
		return (error);
	if (zfsvfs->z_version >= ZPL_VERSION_SA)
		sa_register_update_callback(os, zfs_sa_upgrade);

	error = zap_lookup(os, MASTER_NODE_OBJ, ZFS_ROOT_OBJ, 8, 1,
	    &zfsvfs->z_root);
	if (error != 0)
		return (error);
	ASSERT3U(zfsvfs->z_root, !=, 0);

	error = zap_lookup(os, MASTER_NODE_OBJ, ZFS_UNLINKED_SET, 8, 1,
	    &zfsvfs->z_unlinkedobj);
	if (error != 0)
		return (error);

#define	ZFS_OPTIONAL_MASTER_NODE(name, field) do {			\
	error = zap_lookup(os, MASTER_NODE_OBJ, (name), 8, 1,		\
	    &(field));						\
	if (error == ENOENT)						\
		(field) = 0;						\
	else if (error != 0)						\
		return (error);						\
} while (0)

	ZFS_OPTIONAL_MASTER_NODE(
	    zfs_userquota_prop_prefixes[ZFS_PROP_USERQUOTA],
	    zfsvfs->z_userquota_obj);
	ZFS_OPTIONAL_MASTER_NODE(
	    zfs_userquota_prop_prefixes[ZFS_PROP_GROUPQUOTA],
	    zfsvfs->z_groupquota_obj);
	ZFS_OPTIONAL_MASTER_NODE(
	    zfs_userquota_prop_prefixes[ZFS_PROP_PROJECTQUOTA],
	    zfsvfs->z_projectquota_obj);
	ZFS_OPTIONAL_MASTER_NODE(
	    zfs_userquota_prop_prefixes[ZFS_PROP_USEROBJQUOTA],
	    zfsvfs->z_userobjquota_obj);
	ZFS_OPTIONAL_MASTER_NODE(
	    zfs_userquota_prop_prefixes[ZFS_PROP_GROUPOBJQUOTA],
	    zfsvfs->z_groupobjquota_obj);
	ZFS_OPTIONAL_MASTER_NODE(
	    zfs_userquota_prop_prefixes[ZFS_PROP_PROJECTOBJQUOTA],
	    zfsvfs->z_projectobjquota_obj);
	ZFS_OPTIONAL_MASTER_NODE(ZFS_FUID_TABLES, zfsvfs->z_fuid_obj);
	ZFS_OPTIONAL_MASTER_NODE(ZFS_SHARES_DIR, zfsvfs->z_shares_dir);

#undef ZFS_OPTIONAL_MASTER_NODE

	/* OpenBSD namecache lookups are exact on mixed-case datasets. */
	zfsvfs->z_use_namecache = !zfsvfs->z_norm ||
	    (zfsvfs->z_case == ZFS_CASE_MIXED &&
	    !(zfsvfs->z_norm & ~U8_TEXTPREP_TOUPPER));
	mutex_enter(&os->os_user_ptr_lock);
	dmu_objset_set_user(os, zfsvfs);
	mutex_exit(&os->os_user_ptr_lock);
	return (0);
}

int
zfsvfs_create(const char *osname, boolean_t readonly, zfsvfs_t **zfvp)
{
	boolean_t ro;
	zfsvfs_t *zfsvfs;
	objset_t *os;
	int error;

	*zfvp = NULL;
	if (strlen(osname) >= MNAMELEN)
		return (ENAMETOOLONG);

	ro = readonly || strchr(osname, '@') != NULL;
	zfsvfs = kmem_zalloc(sizeof (*zfsvfs), KM_SLEEP);
	error = dmu_objset_own(osname, DMU_OST_ZFS, ro, B_TRUE, zfsvfs, &os);
	if (error != 0) {
		kmem_free(zfsvfs, sizeof (*zfsvfs));
		return (error);
	}

	return (zfsvfs_create_impl(zfvp, zfsvfs, os));
}

int
zfsvfs_create_impl(zfsvfs_t **zfvp, zfsvfs_t *zfsvfs, objset_t *os)
{
	int error, i;

	*zfvp = NULL;
	zfsvfs->z_vfs = NULL;
	zfsvfs->z_parent = zfsvfs;
	mutex_init(&zfsvfs->z_znodes_lock, NULL, MUTEX_DEFAULT, NULL);
	mutex_init(&zfsvfs->z_rename_lock, NULL, MUTEX_DEFAULT, NULL);
	mutex_init(&zfsvfs->z_lock, NULL, MUTEX_DEFAULT, NULL);
	list_create(&zfsvfs->z_all_znodes, sizeof (znode_t),
	    offsetof(znode_t, z_link_node));
	ZFS_TEARDOWN_INIT(zfsvfs);
	ZFS_TEARDOWN_INACTIVE_INIT(zfsvfs);
	rw_init(&zfsvfs->z_fuid_lock, NULL, RW_DEFAULT, NULL);
	for (i = 0; i < ZFS_OBJ_MTX_SZ; i++)
		mutex_init(&zfsvfs->z_hold_mtx[i], NULL, MUTEX_DEFAULT, NULL);
	zfsvfs->z_drain_task = TASKQID_INVALID;

	error = zfsvfs_init(zfsvfs, os);
	if (error != 0) {
		dmu_objset_disown(os, B_TRUE, zfsvfs);
		zfsvfs->z_os = NULL;
		zfsvfs_free(zfsvfs);
		return (error);
	}

	*zfvp = zfsvfs;
	return (0);
}

void
zfsvfs_free(zfsvfs_t *zfsvfs)
{
	int i;

	if (zfsvfs->z_os != NULL) {
		mutex_enter(&zfsvfs->z_os->os_user_ptr_lock);
		if (dmu_objset_get_user(zfsvfs->z_os) == zfsvfs)
			dmu_objset_set_user(zfsvfs->z_os, NULL);
		mutex_exit(&zfsvfs->z_os->os_user_ptr_lock);
	}
	zfs_fuid_destroy(zfsvfs);
	mutex_destroy(&zfsvfs->z_znodes_lock);
	mutex_destroy(&zfsvfs->z_rename_lock);
	mutex_destroy(&zfsvfs->z_lock);
	list_destroy(&zfsvfs->z_all_znodes);
	ZFS_TEARDOWN_DESTROY(zfsvfs);
	ZFS_TEARDOWN_INACTIVE_DESTROY(zfsvfs);
	rw_destroy(&zfsvfs->z_fuid_lock);
	for (i = 0; i < ZFS_OBJ_MTX_SZ; i++)
		mutex_destroy(&zfsvfs->z_hold_mtx[i]);
	dataset_kstats_destroy(&zfsvfs->z_kstat);
	kmem_free(zfsvfs, sizeof (*zfsvfs));
}

boolean_t
zfs_is_readonly(zfsvfs_t *zfsvfs)
{

	ASSERT3P(zfsvfs->z_vfs, !=, NULL);
	return ((zfsvfs->z_vfs->mnt_flag & MNT_RDONLY) != 0);
}

boolean_t
zfs_get_vfs_flag_unmounted(objset_t *os)
{
	zfsvfs_t *zfsvfs;
	boolean_t unmounted = B_FALSE;

	ASSERT3U(dmu_objset_type(os), ==, DMU_OST_ZFS);
	mutex_enter(&os->os_user_ptr_lock);
	zfsvfs = dmu_objset_get_user(os);
	if (zfsvfs != NULL && (zfsvfs->z_unmounted ||
	    (zfsvfs->z_vfs != NULL &&
	    ISSET(zfsvfs->z_vfs->mnt_flag, MNT_UNMOUNT))))
		unmounted = B_TRUE;
	mutex_exit(&os->os_user_ptr_lock);

	return (unmounted);
}

/*
 * Report mount flags which temporarily override an on-disk property.  Keep
 * the objset user-pointer lock across the lookup and inspection so unmount
 * cannot free the zfsvfs while its native mount is being examined.
 */
int
zfs_get_temporary_prop(struct dsl_dataset *ds, zfs_prop_t prop,
    uint64_t *val, char *setpoint)
{
	zfsvfs_t *zfsvfs;
	objset_t *os;
	uint64_t tmp;
	int error;

	error = dmu_objset_from_ds(ds, &os);
	if (error != 0)
		return (error);
	if (dmu_objset_type(os) != DMU_OST_ZFS)
		return (EINVAL);

	tmp = *val;
	mutex_enter(&os->os_user_ptr_lock);
	zfsvfs = dmu_objset_get_user(os);
	if (zfsvfs == NULL) {
		mutex_exit(&os->os_user_ptr_lock);
		return (ESRCH);
	}

	switch (prop) {
	case ZFS_PROP_ATIME:
		if (zfsvfs->z_vfs != NULL &&
		    ISSET(zfsvfs->z_vfs->mnt_flag, MNT_NOATIME))
			tmp = 0;
		break;
	case ZFS_PROP_DEVICES:
		if (zfsvfs->z_vfs != NULL &&
		    ISSET(zfsvfs->z_vfs->mnt_flag, MNT_NODEV))
			tmp = 0;
		break;
	case ZFS_PROP_EXEC:
		if (zfsvfs->z_vfs != NULL &&
		    ISSET(zfsvfs->z_vfs->mnt_flag, MNT_NOEXEC))
			tmp = 0;
		break;
	case ZFS_PROP_SETUID:
		if (zfsvfs->z_vfs != NULL &&
		    ISSET(zfsvfs->z_vfs->mnt_flag, MNT_NOSUID))
			tmp = 0;
		break;
	case ZFS_PROP_READONLY:
		if (zfsvfs->z_vfs != NULL &&
		    ISSET(zfsvfs->z_vfs->mnt_flag, MNT_RDONLY))
			tmp = 1;
		break;
	case ZFS_PROP_XATTR:
		if (ISSET(zfsvfs->z_flags, ZSB_XATTR))
			tmp = zfsvfs->z_xattr;
		break;
	case ZFS_PROP_NBMAND:
		break;
	default:
		mutex_exit(&os->os_user_ptr_lock);
		return (ENOENT);
	}
	mutex_exit(&os->os_user_ptr_lock);

	if (tmp != *val) {
		if (setpoint != NULL)
			(void)strcpy(setpoint, "temporary");
		*val = tmp;
	}
	return (0);
}

/*
 * OpenBSD does not yet register live DSL property callbacks.  Seed the
 * mount-facing properties once here; explicit mount flags remain temporary
 * overrides of the on-disk values.
 */
static int
zfsvfs_load_mount_props(zfsvfs_t *zfsvfs)
{
	struct mount *mp = zfsvfs->z_vfs;
	struct dsl_dataset *ds = dmu_objset_ds(zfsvfs->z_os);
	dsl_pool_t *dp = dmu_objset_pool(zfsvfs->z_os);
	uint64_t val;
	int error;

	/*
	 * Mount is called with the covered vnode locked.  Avoid the name-based
	 * property lookup here: it enters spa_namespace_lock, while cachefile
	 * writes can enter the vnode layer with spa_namespace_lock held.
	 */
	dsl_pool_config_enter(dp, FTAG);
	error = dsl_prop_get_int_ds(ds,
	    zfs_prop_to_name(ZFS_PROP_RECORDSIZE), &val);
	if (error != 0)
		goto out;
	zfsvfs->z_max_blksz = val;
	mp->mnt_stat.f_iosize = (uint32_t)val;

	error = dsl_prop_get_int_ds(ds,
	    zfs_prop_to_name(ZFS_PROP_ATIME), &val);
	if (error != 0)
		goto out;
	zfsvfs->z_atime = (val != 0 && !ISSET(mp->mnt_flag, MNT_NOATIME));
	if (val == 0)
		SET(mp->mnt_flag, MNT_NOATIME);

	error = dsl_prop_get_int_ds(ds,
	    zfs_prop_to_name(ZFS_PROP_DEVICES), &val);
	if (error != 0)
		goto out;
	if (val == 0)
		SET(mp->mnt_flag, MNT_NODEV);

	error = dsl_prop_get_int_ds(ds,
	    zfs_prop_to_name(ZFS_PROP_EXEC), &val);
	if (error != 0)
		goto out;
	if (val == 0)
		SET(mp->mnt_flag, MNT_NOEXEC);

	error = dsl_prop_get_int_ds(ds,
	    zfs_prop_to_name(ZFS_PROP_SETUID), &val);
	if (error != 0)
		goto out;
	if (val == 0)
		SET(mp->mnt_flag, MNT_NOSUID);

	error = dsl_prop_get_int_ds(ds,
	    zfs_prop_to_name(ZFS_PROP_READONLY), &val);
	if (error != 0)
		goto out;
	if (val != 0)
		SET(mp->mnt_flag, MNT_RDONLY);

	error = dsl_prop_get_int_ds(ds,
	    zfs_prop_to_name(ZFS_PROP_XATTR), &val);
	if (error != 0)
		goto out;
	zfsvfs->z_xattr = (uint8_t)val;
	if (val != ZFS_XATTR_OFF)
		SET(zfsvfs->z_flags, ZSB_XATTR);

out:
	dsl_pool_config_exit(dp, FTAG);
	return (error);
}

static int
zfsvfs_setup_mount(zfsvfs_t *zfsvfs)
{
	boolean_t readonly;
	zap_stats_t zs;
	int error;

	if (!zfs_is_readonly(zfsvfs) &&
	    dmu_objset_incompatible_encryption_version(zfsvfs->z_os))
		return (EROFS);

	error = dataset_kstats_create(&zfsvfs->z_kstat, zfsvfs->z_os);
	if (error != 0)
		return (error);
	zfsvfs->z_log = zil_open(zfsvfs->z_os, zfs_get_data,
	    &zfsvfs->z_kstat.dk_zil_sums);

	readonly = zfs_is_readonly(zfsvfs);
	if (readonly)
		CLR(zfsvfs->z_vfs->mnt_flag, MNT_RDONLY);
	else {
		if (zap_get_stats(zfsvfs->z_os, zfsvfs->z_unlinkedobj,
		    &zs) == 0)
			dataset_kstats_update_nunlinks_kstat(&zfsvfs->z_kstat,
			    zs.zs_num_entries);
		zfs_unlinked_drain(zfsvfs);
	}

	if (spa_writeable(dmu_objset_spa(zfsvfs->z_os))) {
		if (zil_replay_disable) {
			(void)zil_destroy(zfsvfs->z_log, B_FALSE);
		} else {
			boolean_t use_namecache = zfsvfs->z_use_namecache;

			zfsvfs->z_use_namecache = B_FALSE;
			zfsvfs->z_replay = B_TRUE;
			(void)zil_replay(zfsvfs->z_os, zfsvfs,
			    zfs_replay_vector);
			zfsvfs->z_replay = B_FALSE;
			zfsvfs->z_use_namecache = use_namecache;
		}
	}
	if (readonly)
		SET(zfsvfs->z_vfs->mnt_flag, MNT_RDONLY);

	return (0);
}

static void
zfsvfs_disown(zfsvfs_t *zfsvfs)
{
	objset_t *os = zfsvfs->z_os;

	if (os == NULL)
		return;
	mutex_enter(&os->os_user_ptr_lock);
	if (dmu_objset_get_user(os) == zfsvfs)
		dmu_objset_set_user(os, NULL);
	mutex_exit(&os->os_user_ptr_lock);
	dmu_objset_disown(os, B_TRUE, zfsvfs);
	zfsvfs->z_os = NULL;
}

static void
zfsvfs_mount_abort(struct mount *mp, zfsvfs_t *zfsvfs)
{

	(void)vflush(mp, NULL, FORCECLOSE);
	if (zfsvfs->z_log != NULL) {
		zil_close(zfsvfs->z_log);
		zfsvfs->z_log = NULL;
	}
	zfsvfs_disown(zfsvfs);
	mp->mnt_data = NULL;
	zfsvfs_free(zfsvfs);
}

static int
zfs_vfs_mount(struct mount *mp, const char *path, void *data,
    struct nameidata *ndp, struct proc *p)
{
	struct zfs_args *args = data;
	char osname[ZFS_MAX_DATASET_NAME_LEN];
	zfsvfs_t *zfsvfs;
	vnode_t *rootvp;
	int error;

	(void)ndp;
	(void)p;
	KERNEL_ASSERT_LOCKED();
	if (!zfs_runtime_initialized)
		return (ENXIO);
	if (ISSET(mp->mnt_flag, MNT_UPDATE))
		return (EOPNOTSUPP);
	if (args == NULL || args->fspec == NULL)
		return (EINVAL);
	error = copyinstr(args->fspec, osname, sizeof (osname), NULL);
	if (error != 0)
		return (error);

	error = zfsvfs_create(osname, ISSET(mp->mnt_flag, MNT_RDONLY),
	    &zfsvfs);
	if (error != 0)
		return (error);
	zfsvfs->z_vfs = mp;
	mp->mnt_data = zfsvfs;
	SET(mp->mnt_flag, MNT_LOCAL);
	mp->mnt_stat.f_bsize = SPA_MINBLOCKSIZE;
	mp->mnt_stat.f_namemax = ZAP_MAXNAMELEN - 1;
	vfs_getnewfsid(mp);

	bzero(&mp->mnt_stat.mount_info.zfs_args,
	    sizeof (mp->mnt_stat.mount_info.zfs_args));
	bzero(mp->mnt_stat.f_mntonname, MNAMELEN);
	bzero(mp->mnt_stat.f_mntfromname, MNAMELEN);
	bzero(mp->mnt_stat.f_mntfromspec, MNAMELEN);
	strlcpy(mp->mnt_stat.f_mntonname, path, MNAMELEN);
	strlcpy(mp->mnt_stat.f_mntfromname, osname, MNAMELEN);
	strlcpy(mp->mnt_stat.f_mntfromspec, osname, MNAMELEN);

	error = zfsvfs_load_mount_props(zfsvfs);
	if (error == 0 && !spa_writeable(dmu_objset_spa(zfsvfs->z_os)))
		SET(mp->mnt_flag, MNT_RDONLY);
	if (error == 0 && dmu_objset_is_snapshot(zfsvfs->z_os)) {
		zfsvfs->z_issnap = B_TRUE;
		SET(mp->mnt_flag, MNT_RDONLY);
		zfsvfs->z_os->os_sync = ZFS_SYNC_DISABLED;
	} else if (error == 0) {
		error = zfsvfs_setup_mount(zfsvfs);
	}
	if (error == 0) {
		error = zfs_vfs_root(mp, &rootvp);
		if (error == 0)
			vput(rootvp);
	}
	if (error != 0) {
		zfsvfs_mount_abort(mp, zfsvfs);
		return (error);
	}

	return (0);
}

static int
zfs_vfs_start(struct mount *mp, int flags, struct proc *p)
{
	(void)mp;
	(void)flags;
	(void)p;
	KERNEL_ASSERT_LOCKED();

	return (0);
}

static int
zfs_vfs_unmount(struct mount *mp, int mntflags, struct proc *p)
{
	zfsvfs_t *zfsvfs = mp->mnt_data;
	objset_t *os;
	boolean_t readonly, forced;
	boolean_t dirty = B_FALSE;
	int error, txg;

	(void)p;
	KERNEL_ASSERT_LOCKED();
	if (zfsvfs == NULL)
		return (EIO);
	forced = ISSET(mntflags, MNT_FORCE);
	if (forced) {
		ZFS_TEARDOWN_ENTER_WRITE(zfsvfs, FTAG);
		zfsvfs->z_unmounted = B_TRUE;
		ZFS_TEARDOWN_EXIT(zfsvfs, FTAG);
	}
	error = vflush(mp, NULL, forced ? FORCECLOSE : 0);
	if (error != 0) {
		if (forced) {
			ZFS_TEARDOWN_ENTER_WRITE(zfsvfs, FTAG);
			zfsvfs->z_unmounted = B_FALSE;
			ZFS_TEARDOWN_EXIT(zfsvfs, FTAG);
		}
		return (error);
	}

	KASSERT(list_is_empty(&zfsvfs->z_all_znodes));
	readonly = zfs_is_readonly(zfsvfs);
	ZFS_TEARDOWN_ENTER_WRITE(zfsvfs, FTAG);
	if (zfsvfs->z_log != NULL) {
		zil_close(zfsvfs->z_log);
		zfsvfs->z_log = NULL;
	}
	ZFS_TEARDOWN_INACTIVE_ENTER_WRITE(zfsvfs);
	zfsvfs->z_unmounted = B_TRUE;
	ZFS_TEARDOWN_INACTIVE_EXIT_WRITE(zfsvfs);
	ZFS_TEARDOWN_EXIT(zfsvfs, FTAG);

	os = zfsvfs->z_os;
	if (!readonly) {
		for (txg = 0; txg < TXG_SIZE; txg++) {
			if (dmu_objset_is_dirty(os, txg)) {
				dirty = B_TRUE;
				break;
			}
		}
		if (dirty)
			txg_wait_synced(dmu_objset_pool(os), 0);
	}
	dmu_objset_evict_dbufs(os);
	zfsvfs_disown(zfsvfs);
	mp->mnt_data = NULL;
	zfsvfs_free(zfsvfs);

	return (0);
}

static int
zfs_vfs_root(struct mount *mp, struct vnode **vpp)
{
	zfsvfs_t *zfsvfs = mp->mnt_data;
	znode_t *zp;
	int error;

	KERNEL_ASSERT_LOCKED();
	*vpp = NULL;
	if (zfsvfs == NULL)
		return (EIO);
	if ((error = zfs_enter(zfsvfs, FTAG)) != 0)
		return (error);
	error = zfs_zget(zfsvfs, zfsvfs->z_root, &zp);
	zfs_exit(zfsvfs, FTAG);
	if (error == 0)
		*vpp = ZTOV(zp);
	return (error);
}

static int
zfs_vfs_quotactl(struct mount *mp, int cmd, uid_t uid, caddr_t arg,
    struct proc *p)
{
	(void)mp;
	(void)cmd;
	(void)uid;
	(void)arg;
	(void)p;

	return (EOPNOTSUPP);
}

static int
zfs_vfs_statfs(struct mount *mp, struct statfs *sbp, struct proc *p)
{
	zfsvfs_t *zfsvfs = mp->mnt_data;
	uint64_t refdbytes, availbytes, usedobjs, availobjs;
	int error;

	(void)p;
	KERNEL_ASSERT_LOCKED();
	if (zfsvfs == NULL)
		return (EIO);
	if ((error = zfs_enter(zfsvfs, FTAG)) != 0)
		return (error);
	dmu_objset_space(zfsvfs->z_os, &refdbytes, &availbytes, &usedobjs,
	    &availobjs);
	sbp->f_bsize = SPA_MINBLOCKSIZE;
	sbp->f_iosize = mp->mnt_stat.f_iosize;
	sbp->f_blocks = (refdbytes + availbytes) >> SPA_MINBLOCKSHIFT;
	sbp->f_bfree = availbytes >> SPA_MINBLOCKSHIFT;
	sbp->f_bavail = sbp->f_bfree & INT64_MAX;
	sbp->f_ffree = MIN(availobjs, sbp->f_bfree);
	sbp->f_favail = sbp->f_ffree & INT64_MAX;
	sbp->f_files = sbp->f_ffree + usedobjs;
	copy_statfs_info(sbp, mp);
	zfs_exit(zfsvfs, FTAG);

	return (0);
}

static int
zfs_vfs_sync(struct mount *mp, int waitfor, int stall, struct ucred *cred,
    struct proc *p)
{
	zfsvfs_t *zfsvfs = mp->mnt_data;
	int error;

	(void)stall;
	(void)cred;
	(void)p;
	KERNEL_ASSERT_LOCKED();
	if (panicstr != NULL || waitfor == MNT_LAZY)
		return (0);
	if (zfsvfs == NULL)
		return (EIO);
	if ((error = zfs_enter(zfsvfs, FTAG)) != 0)
		return (error);
	if (zfsvfs->z_log != NULL)
		error = zil_commit(zfsvfs->z_log, 0);
	else
		error = 0;
	zfs_exit(zfsvfs, FTAG);

	return (error);
}

static int
zfs_vfs_vget(struct mount *mp, ino_t ino, struct vnode **vpp)
{
	zfsvfs_t *zfsvfs = mp->mnt_data;
	znode_t *zp;
	int error;

	KERNEL_ASSERT_LOCKED();
	*vpp = NULL;
	if (zfsvfs == NULL)
		return (EIO);
	if ((error = zfs_enter(zfsvfs, FTAG)) != 0)
		return (error);
	error = zfs_zget(zfsvfs, (uint64_t)ino, &zp);
	zfs_exit(zfsvfs, FTAG);
	if (error == 0)
		*vpp = ZTOV(zp);
	return (error);
}

static int
zfs_vfs_fhtovp(struct mount *mp, struct fid *fhp, struct vnode **vpp)
{
	(void)mp;
	(void)fhp;
	(void)vpp;

	return (EOPNOTSUPP);
}

static int
zfs_vfs_vptofh(struct vnode *vp, struct fid *fhp)
{
	(void)vp;
	(void)fhp;

	return (EOPNOTSUPP);
}

/*
 * Online rollback and receive require detaching and reconstructing every
 * native vnode.  Refuse that operation until the OpenBSD reattachment path
 * exists; ordinary unmounted dataset operations remain unaffected.
 */
int
zfs_suspend_fs(zfsvfs_t *zfsvfs)
{

	(void)zfsvfs;
	return (EOPNOTSUPP);
}

int
zfs_resume_fs(zfsvfs_t *zfsvfs, struct dsl_dataset *ds)
{

	(void)zfsvfs;
	(void)ds;
	return (EOPNOTSUPP);
}

int
zfs_end_fs(zfsvfs_t *zfsvfs, struct dsl_dataset *ds)
{

	(void)zfsvfs;
	(void)ds;
	return (EOPNOTSUPP);
}

int
zfs_set_version(zfsvfs_t *zfsvfs, uint64_t newvers)
{
	objset_t *os = zfsvfs->z_os;
	dmu_tx_t *tx;
	int error;

	if (newvers < ZPL_VERSION_INITIAL || newvers > ZPL_VERSION ||
	    newvers < zfsvfs->z_version)
		return (EINVAL);
	if (zfs_spa_version_map(newvers) >
	    spa_version(dmu_objset_spa(os)))
		return (ENOTSUP);

	tx = dmu_tx_create(os);
	dmu_tx_hold_zap(tx, MASTER_NODE_OBJ, B_FALSE, ZPL_VERSION_STR);
	if (newvers >= ZPL_VERSION_SA && !zfsvfs->z_use_sa) {
		dmu_tx_hold_zap(tx, MASTER_NODE_OBJ, B_TRUE, ZFS_SA_ATTRS);
		dmu_tx_hold_zap(tx, DMU_NEW_OBJECT, B_FALSE, NULL);
	}
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error != 0) {
		dmu_tx_abort(tx);
		return (error);
	}

	error = zap_update(os, MASTER_NODE_OBJ, ZPL_VERSION_STR,
	    8, 1, &newvers, tx);
	if (error == 0 && newvers >= ZPL_VERSION_SA && !zfsvfs->z_use_sa) {
		uint64_t sa_obj;

		sa_obj = zap_create(os, DMU_OT_SA_MASTER_NODE,
		    DMU_OT_NONE, 0, tx);
		VERIFY0(zap_add(os, MASTER_NODE_OBJ, ZFS_SA_ATTRS,
		    8, 1, &sa_obj, tx));
		VERIFY0(sa_set_sa_object(os, sa_obj));
		sa_register_update_callback(os, zfs_sa_upgrade);
	}
	dmu_tx_commit(tx);
	if (error != 0)
		return (error);

	zfsvfs->z_version = newvers;
	os->os_version = newvers;
	zfsvfs->z_use_fuids = USE_FUIDS(newvers, os);
	zfsvfs->z_use_sa = USE_SA(newvers, os);
	return (0);
}

int
zfs_set_default_quota(zfsvfs_t *zfsvfs, zfs_prop_t prop, uint64_t quota)
{
	objset_t *os = zfsvfs->z_os;
	const char *propstr = zfs_prop_to_name(prop);
	dmu_tx_t *tx;
	int error;

	tx = dmu_tx_create(os);
	dmu_tx_hold_zap(tx, MASTER_NODE_OBJ, B_FALSE, propstr);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error != 0) {
		dmu_tx_abort(tx);
		return (error);
	}
	if (quota == 0) {
		error = zap_remove(os, MASTER_NODE_OBJ, propstr, tx);
		if (error == ENOENT)
			error = 0;
	} else {
		error = zap_update(os, MASTER_NODE_OBJ, propstr,
		    8, 1, &quota, tx);
	}
	if (error == 0) {
		switch (prop) {
		case ZFS_PROP_DEFAULTUSERQUOTA:
			zfsvfs->z_defaultuserquota = quota;
			break;
		case ZFS_PROP_DEFAULTGROUPQUOTA:
			zfsvfs->z_defaultgroupquota = quota;
			break;
		case ZFS_PROP_DEFAULTPROJECTQUOTA:
			zfsvfs->z_defaultprojectquota = quota;
			break;
		case ZFS_PROP_DEFAULTUSEROBJQUOTA:
			zfsvfs->z_defaultuserobjquota = quota;
			break;
		case ZFS_PROP_DEFAULTGROUPOBJQUOTA:
			zfsvfs->z_defaultgroupobjquota = quota;
			break;
		case ZFS_PROP_DEFAULTPROJECTOBJQUOTA:
			zfsvfs->z_defaultprojectobjquota = quota;
			break;
		default:
			break;
		}
	}
	dmu_tx_commit(tx);
	return (error);
}

void
zfsvfs_update_fromname(const char *oldname, const char *newname)
{

	/* Mounted filesystem names may remain stale until remounted. */
	(void)oldname;
	(void)newname;
}

static void
zfs_runtime_init(void *arg)
{
	int error;

	(void)arg;
	KERNEL_ASSERT_LOCKED();
	if (zfs_runtime_initialized)
		return;

	/*
	 * ZFS is a static filesystem on OpenBSD, so its common kernel runtime
	 * lives for the lifetime of the kernel.  This callback runs after the
	 * scheduler and real timecounter have started, but before mountroot.
	 * Native taskqs can therefore create workers before SPA initialization
	 * starts ARC and ZIO, and checksum implementation benchmarks see a real
	 * monotonic clock.  The rrwlock key is needed once mounted objsets begin
	 * using teardown locks.
	 */
	error = spl_kstat_init();
	if (error != 0) {
		printf("zfs: cannot initialize kstats: %d\n", error);
		return;
	}
	system_taskq_init();
	tsd_create(&rrw_tsd_key, rrw_tsd_destroy);
	spa_init(SPA_MODE_READ | SPA_MODE_WRITE);
	zfs_init();
	zfsdev_init();
	tsd_create(&zfs_allow_log_key, zfs_allow_log_destroy_openbsd);
	zfs_runtime_initialized = B_TRUE;
	printf("zfs: initialized\n");
	return;
}

static int
zfs_vfs_init(struct vfsconf *vfc)
{

	(void)vfc;
	KERNEL_ASSERT_LOCKED();
	if (zfs_runtime_initialized || zfs_runtime_init_deferred)
		return (0);

	/*
	 * vfsinit() runs before initclocks() and scheduler_start().  Queue the
	 * runtime initialization for kthread_run_deferred_queue(), which runs
	 * after both and before mountroot.
	 */
	zfs_runtime_init_deferred = B_TRUE;
	kthread_create_deferred(zfs_runtime_init, NULL);
	return (0);
}

static int
zfs_vfs_sysctl(int *name, u_int namelen, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen, struct proc *p)
{
	(void)name;
	(void)namelen;
	(void)oldp;
	(void)oldlenp;
	(void)newp;
	(void)newlen;
	(void)p;

	return (EOPNOTSUPP);
}

static int
zfs_vfs_checkexp(struct mount *mp, struct mbuf *nam, int *extflagsp,
    struct ucred **credanonp)
{
	(void)mp;
	(void)nam;
	(void)extflagsp;
	(void)credanonp;

	return (EOPNOTSUPP);
}

const struct vfsops zfs_vfsops = {
	.vfs_mount	= zfs_vfs_mount,
	.vfs_start	= zfs_vfs_start,
	.vfs_unmount	= zfs_vfs_unmount,
	.vfs_root	= zfs_vfs_root,
	.vfs_quotactl	= zfs_vfs_quotactl,
	.vfs_statfs	= zfs_vfs_statfs,
	.vfs_sync	= zfs_vfs_sync,
	.vfs_vget	= zfs_vfs_vget,
	.vfs_fhtovp	= zfs_vfs_fhtovp,
	.vfs_vptofh	= zfs_vfs_vptofh,
	.vfs_init	= zfs_vfs_init,
	.vfs_sysctl	= zfs_vfs_sysctl,
	.vfs_checkexp	= zfs_vfs_checkexp,
};
