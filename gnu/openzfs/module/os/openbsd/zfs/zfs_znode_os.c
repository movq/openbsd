// SPDX-License-Identifier: CDDL-1.0
/*
 * Native OpenBSD znode and vnode construction.
 *
 * OpenBSD owns vnode allocation and recycling.  A vnode's initial znode
 * reference is released only by VOP_RECLAIM; transient references protect a
 * znode while zfs_zget() drops the object hold before calling vget().  That
 * ordering is important because reclaim arrives with vnode recycling already
 * in progress and must be allowed to acquire the per-object ZFS hold.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/lock.h>
#include <sys/stat.h>
#include <sys/kmem.h>
#include <sys/nvpair.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/dmu_tx.h>
#include <sys/fs/zfs.h>
#include <sys/sa.h>
#include <sys/zap.h>
#include <sys/u8_textprep.h>
#include <sys/zfs_rlock.h>
#include <sys/zfs_acl.h>
#include <sys/zfs_dir.h>
#include <sys/zfs_sa.h>
#include <sys/zfs_vnops_os.h>
#include <sys/zfs_znode.h>

#include <uvm/uvm_extern.h>
#include <uvm/uvm_pager.h>
#include <uvm/uvm_vnode.h>

#include "zfs_comutil.h"

static kmem_cache_t *znode_cache;

/* OpenBSD dev_t values already have a stable 32-bit packed representation. */
dev_t
zfs_cmpldev(uint64_t dev)
{

	return ((dev_t)dev);
}

static void
zfs_rangelock_cb(zfs_locked_range_t *new, void *arg)
{
	znode_t *zp = arg;
	uint64_t end_size;

	if (new->lr_type == RL_APPEND) {
		new->lr_offset = zp->z_size;
		new->lr_type = RL_WRITER;
	}

	end_size = MAX(zp->z_size, new->lr_offset + new->lr_length);
	if (zp->z_size <= zp->z_blksz && end_size > zp->z_blksz &&
	    (!ISP2(zp->z_blksz) ||
	    zp->z_blksz < ZTOZSB(zp)->z_max_blksz)) {
		new->lr_offset = 0;
		new->lr_length = UINT64_MAX;
	}
}

static int
zfs_znode_cache_constructor(void *buf, void *arg, int kmflags)
{
	znode_t *zp = buf;

	(void)arg;
	(void)kmflags;

	memset(zp, 0, sizeof (*zp));
	zp->z_zfsvfs = NULL;
	POINTER_INVALIDATE(&zp->z_zfsvfs);
	zp->z_vnode = NULL;
	zp->z_lockf = NULL;
	zp->z_cached_symlink = NULL;
	zp->z_sa_hdl = NULL;
	zp->z_dirlocks = NULL;
	zp->z_acl_cached = NULL;
	zp->z_xattr_cached = NULL;
	zp->z_xattr_parent = 0;
	list_link_init(&zp->z_link_node);
	refcnt_init(&zp->z_refs);

	mutex_init(&zp->z_lock, NULL, MUTEX_DEFAULT, NULL);
	rw_init(&zp->z_parent_lock, NULL, RW_DEFAULT, NULL);
	rw_init(&zp->z_name_lock, NULL, RW_DEFAULT, NULL);
	mutex_init(&zp->z_acl_lock, NULL, MUTEX_DEFAULT, NULL);
	rw_init(&zp->z_xattr_lock, NULL, RW_DEFAULT, NULL);
	/*
	 * Vnode operations may fault on userspace buffers while this lock is
	 * held.  Conversely, physio(9) keeps a process map read-locked while
	 * vnd(4) enters its backing vnode.  Both orders are valid (the map
	 * acquisitions are shared), but WITNESS does not retain acquisition
	 * modes in its class graph and reports a false reversal.  Keep the
	 * native ownership checks, but omit this instance-dependent order from
	 * the WITNESS graph.
	 */
	rrw_init_flags(&zp->z_vlock, "znode",
	    RWL_DUPOK | RWL_IS_VNODE | RWL_NOWITNESS);
	mutex_init(&zp->z_map_lock, NULL, MUTEX_DEFAULT, NULL);
	zfs_rangelock_init(&zp->z_rangelock, zfs_rangelock_cb, zp);

	return (0);
}

static void
zfs_znode_cache_destructor(void *buf, void *arg)
{
	znode_t *zp = buf;

	(void)arg;
	ASSERT(!POINTER_IS_VALID(zp->z_zfsvfs));
	ASSERT0P(zp->z_vnode);
	ASSERT0P(zp->z_sa_hdl);
	ASSERT0P(zp->z_lockf);
	ASSERT0P(zp->z_cached_symlink);
	ASSERT0P(zp->z_dirlocks);
	ASSERT0P(zp->z_acl_cached);
	ASSERT0P(zp->z_xattr_cached);
	ASSERT(!list_link_active(&zp->z_link_node));
	ASSERT3U(refcnt_read(&zp->z_refs), ==, 0);

	mutex_destroy(&zp->z_lock);
	rw_destroy(&zp->z_parent_lock);
	rw_destroy(&zp->z_name_lock);
	mutex_destroy(&zp->z_acl_lock);
	rw_destroy(&zp->z_xattr_lock);
	mutex_destroy(&zp->z_map_lock);
	zfs_rangelock_fini(&zp->z_rangelock);
}

void
zfs_znode_init(void)
{

	ASSERT0P(znode_cache);
	znode_cache = kmem_cache_create("zfs_znode_cache",
	    sizeof (znode_t), 0, zfs_znode_cache_constructor,
	    zfs_znode_cache_destructor, NULL, NULL, NULL, KMC_RECLAIMABLE);
}

void
zfs_znode_fini(void)
{

	if (znode_cache != NULL) {
		kmem_cache_destroy(znode_cache);
		znode_cache = NULL;
	}
}

static void
zfs_znode_free_kmem(znode_t *zp)
{

	if (zp->z_xattr_cached != NULL) {
		nvlist_free(zp->z_xattr_cached);
		zp->z_xattr_cached = NULL;
	}
	kmem_cache_free(znode_cache, zp);
}

static void
zfs_znode_rele(znode_t *zp)
{

	if (refcnt_rele(&zp->z_refs))
		zfs_znode_free_kmem(zp);
}

static void
zfs_znode_sa_init(zfsvfs_t *zfsvfs, znode_t *zp, dmu_buf_t *db,
    dmu_object_type_t obj_type)
{

	ASSERT(MUTEX_HELD(ZFS_OBJ_MUTEX(zfsvfs, zp->z_id)));
	ASSERT0P(zp->z_sa_hdl);
	VERIFY0(sa_handle_get_from_db(zfsvfs->z_os, db, zp,
	    SA_HDL_SHARED, &zp->z_sa_hdl));
	zp->z_is_sa = (obj_type == DMU_OT_SA);
}

void
zfs_znode_dmu_fini(znode_t *zp)
{

	ASSERT(MUTEX_HELD(ZFS_OBJ_MUTEX(zp->z_zfsvfs, zp->z_id)) ||
	    ZFS_TEARDOWN_INACTIVE_WRITE_HELD(zp->z_zfsvfs));
	if (zp->z_sa_hdl != NULL) {
		sa_handle_destroy(zp->z_sa_hdl);
		zp->z_sa_hdl = NULL;
	}
}

/*
 * Finish detaching a znode after the vnode no longer publishes it.  The
 * allocation survives until any zfs_zget() transient reference is gone.
 */
void
zfs_znode_free(znode_t *zp)
{
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;

	ASSERT0P(zp->z_vnode);
	ASSERT0P(zp->z_sa_hdl);
	if (list_link_active(&zp->z_link_node)) {
		mutex_enter(&zfsvfs->z_znodes_lock);
		list_remove(&zfsvfs->z_all_znodes, zp);
		mutex_exit(&zfsvfs->z_znodes_lock);
	}
	POINTER_INVALIDATE(&zp->z_zfsvfs);
	zfs_znode_rele(zp);
}

void
zfs_znode_discard(znode_t *zp)
{
	vnode_t *vp = ZTOV(zp);

	/* VOP_INACTIVE observes the absent SA handle and recycles the vnode. */
	vput(vp);
}

static uint64_t zfs_empty_xattr;
static uint64_t zfs_znode_pad[4];
static zfs_acl_phys_t zfs_empty_acl;

/*
 * Create the DMU object and bind it to a vnode allocated by
 * zfs_znode_alloc_vnode().  Keeping these phases separate is required by
 * OpenBSD: vnode allocation can recursively reclaim ZFS vnodes, while this
 * phase runs inside an assigned DMU transaction.
 */
void
zfs_mknode(znode_t *dzp, vattr_t *vap, dmu_tx_t *tx, cred_t *cr,
    uint_t flag, znode_t **zpp, zfs_acl_ids_t *acl_ids)
{
	zfsvfs_t *zfsvfs = dzp->z_zfsvfs;
	znode_t *zp = *zpp;
	dmu_object_type_t obj_type;
	dmu_buf_t *db;
	sa_handle_t *sa_hdl;
	sa_bulk_attr_t *attrs;
	dmu_object_info_t doi;
	struct timespec now;
	uint64_t atime[2], mtime[2], ctime[2], crtime[2];
	uint64_t mode, size, links, parent, pflags, projid;
	uint64_t gen, obj, rdev = 0;
	uint64_t dzp_pflags = dzp->z_pflags;
	int bonuslen, dnodesize, count = 0;

	(void)cr;
	ASSERT3P(zp, !=, NULL);
	ASSERT0P(zp->z_sa_hdl);
	ASSERT0(zp->z_id);
	ASSERT3U(vap->va_mask & AT_MODE, !=, 0);

	if (zfsvfs->z_replay) {
		obj = vap->va_nodeid;
		now = vap->va_ctime;
		gen = vap->va_nblocks;
		dnodesize = vap->va_fsid;
	} else {
		obj = 0;
		nanotime(&now);
		gen = dmu_tx_get_txg(tx);
		dnodesize = dmu_objset_dnodesize(zfsvfs->z_os);
	}
	if (dnodesize == 0)
		dnodesize = DNODE_MIN_SIZE;

	obj_type = zfsvfs->z_use_sa ? DMU_OT_SA : DMU_OT_ZNODE;
	bonuslen = (obj_type == DMU_OT_SA) ? DN_BONUS_SIZE(dnodesize) :
	    ZFS_OLD_ZNODE_PHYS_SIZE;
	if (vap->va_type == VDIR) {
		if (zfsvfs->z_replay) {
			VERIFY0(zap_create_claim_norm_dnsize(zfsvfs->z_os, obj,
			    zfsvfs->z_norm, DMU_OT_DIRECTORY_CONTENTS, obj_type,
			    bonuslen, dnodesize, tx));
		} else {
			obj = zap_create_norm_dnsize(zfsvfs->z_os,
			    zfsvfs->z_norm, DMU_OT_DIRECTORY_CONTENTS, obj_type,
			    bonuslen, dnodesize, tx);
		}
	} else if (zfsvfs->z_replay) {
		VERIFY0(dmu_object_claim_dnsize(zfsvfs->z_os, obj,
		    DMU_OT_PLAIN_FILE_CONTENTS, 0, obj_type, bonuslen,
		    dnodesize, tx));
	} else {
		obj = dmu_object_alloc_dnsize(zfsvfs->z_os,
		    DMU_OT_PLAIN_FILE_CONTENTS, 0, obj_type, bonuslen,
		    dnodesize, tx);
	}
	if (flag & IS_ROOT_NODE)
		dzp->z_id = obj;

	if (dzp_pflags & ZFS_XATTR)
		flag |= IS_XATTR;
	pflags = zfsvfs->z_use_fuids ? ZFS_ARCHIVE | ZFS_AV_MODIFIED : 0;
	if (flag & IS_XATTR)
		pflags |= ZFS_XATTR;
	mode = acl_ids->z_mode;
	parent = dzp->z_id;
	projid = zfs_inherit_projid(dzp);
	if ((vap->va_type == VREG || vap->va_type == VDIR) &&
	    obj_type != DMU_OT_ZNODE &&
	    dmu_objset_projectquota_enabled(zfsvfs->z_os))
		pflags |= ZFS_PROJID;
	if (dzp_pflags & ZFS_PROJINHERIT)
		pflags |= ZFS_PROJINHERIT;
	if (vap->va_type == VDIR) {
		size = 2;
		links = (flag & (IS_ROOT_NODE | IS_XATTR)) ? 2 : 1;
	} else {
		size = links = 0;
	}
	if (vap->va_type == VBLK || vap->va_type == VCHR)
		rdev = (uint64_t)vap->va_rdev;

	ZFS_TIME_ENCODE(&now, crtime);
	ZFS_TIME_ENCODE(&now, ctime);
	if (vap->va_mask & AT_ATIME)
		ZFS_TIME_ENCODE(&vap->va_atime, atime);
	else
		ZFS_TIME_ENCODE(&now, atime);
	if (vap->va_mask & AT_MTIME)
		ZFS_TIME_ENCODE(&vap->va_mtime, mtime);
	else
		ZFS_TIME_ENCODE(&now, mtime);

	ZFS_OBJ_HOLD_ENTER(zfsvfs, obj);
	VERIFY0(sa_buf_hold(zfsvfs->z_os, obj, NULL, &db));
	VERIFY0(sa_handle_get_from_db(zfsvfs->z_os, db, zp, SA_HDL_SHARED,
	    &sa_hdl));
	attrs = kmem_alloc(sizeof (*attrs) * ZPL_END, KM_SLEEP);

	/* Legacy znode attribute order is its on-disk physical layout. */
	if (obj_type == DMU_OT_ZNODE) {
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_ATIME(zfsvfs), NULL,
		    atime, sizeof (atime));
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_MTIME(zfsvfs), NULL,
		    mtime, sizeof (mtime));
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_CTIME(zfsvfs), NULL,
		    ctime, sizeof (ctime));
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_CRTIME(zfsvfs), NULL,
		    crtime, sizeof (crtime));
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_GEN(zfsvfs), NULL,
		    &gen, sizeof (gen));
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_MODE(zfsvfs), NULL,
		    &mode, sizeof (mode));
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_SIZE(zfsvfs), NULL,
		    &size, sizeof (size));
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_PARENT(zfsvfs), NULL,
		    &parent, sizeof (parent));
	} else {
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_MODE(zfsvfs), NULL,
		    &mode, sizeof (mode));
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_SIZE(zfsvfs), NULL,
		    &size, sizeof (size));
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_GEN(zfsvfs), NULL,
		    &gen, sizeof (gen));
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_UID(zfsvfs), NULL,
		    &acl_ids->z_fuid, sizeof (acl_ids->z_fuid));
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_GID(zfsvfs), NULL,
		    &acl_ids->z_fgid, sizeof (acl_ids->z_fgid));
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_PARENT(zfsvfs), NULL,
		    &parent, sizeof (parent));
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_FLAGS(zfsvfs), NULL,
		    &pflags, sizeof (pflags));
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_ATIME(zfsvfs), NULL,
		    atime, sizeof (atime));
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_MTIME(zfsvfs), NULL,
		    mtime, sizeof (mtime));
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_CTIME(zfsvfs), NULL,
		    ctime, sizeof (ctime));
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_CRTIME(zfsvfs), NULL,
		    crtime, sizeof (crtime));
	}
	SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_LINKS(zfsvfs), NULL,
	    &links, sizeof (links));
	if (obj_type == DMU_OT_ZNODE) {
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_XATTR(zfsvfs), NULL,
		    &zfs_empty_xattr, sizeof (zfs_empty_xattr));
	} else if (pflags & ZFS_PROJID) {
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_PROJID(zfsvfs), NULL,
		    &projid, sizeof (projid));
	}
	if (obj_type == DMU_OT_ZNODE || vap->va_type == VBLK ||
	    vap->va_type == VCHR) {
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_RDEV(zfsvfs), NULL,
		    &rdev, sizeof (rdev));
	}
	if (obj_type == DMU_OT_ZNODE) {
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_FLAGS(zfsvfs), NULL,
		    &pflags, sizeof (pflags));
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_UID(zfsvfs), NULL,
		    &acl_ids->z_fuid, sizeof (acl_ids->z_fuid));
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_GID(zfsvfs), NULL,
		    &acl_ids->z_fgid, sizeof (acl_ids->z_fgid));
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_PAD(zfsvfs), NULL,
		    zfs_znode_pad, sizeof (zfs_znode_pad));
		SA_ADD_BULK_ATTR(attrs, count, SA_ZPL_ZNODE_ACL(zfsvfs), NULL,
		    &zfs_empty_acl, sizeof (zfs_empty_acl));
	}
	VERIFY0(sa_replace_all_by_template(sa_hdl, attrs, count, tx));
	kmem_free(attrs, sizeof (*attrs) * ZPL_END);

	zp->z_id = obj;
	zp->z_sa_hdl = sa_hdl;
	zp->z_is_sa = (obj_type == DMU_OT_SA);
	zp->z_pflags = pflags;
	zp->z_mode = mode;
	zp->z_size = size;
	zp->z_links = links;
	zp->z_uid = acl_ids->z_fuid;
	zp->z_gid = acl_ids->z_fgid;
	zp->z_gen = gen;
	zp->z_projid = projid;
	bcopy(atime, zp->z_atime, sizeof (atime));
	zp->z_dnodesize = dnodesize;
	zp->z_zn_prefetch = (vap->va_type == VDIR);
	dmu_object_info_from_db(db, &doi);
	zp->z_blksz = doi.doi_data_block_size;

	if ((flag & IS_ROOT_NODE) == 0) {
		ZTOV(zp)->v_type = vap->va_type;
#ifdef FIFO
		if (vap->va_type == VFIFO)
			ZTOV(zp)->v_op = &zfs_fifovops;
#endif
		uvm_vnp_setsize(ZTOV(zp), size);
		mutex_enter(&zfsvfs->z_znodes_lock);
		list_insert_tail(&zfsvfs->z_all_znodes, zp);
		mutex_exit(&zfsvfs->z_znodes_lock);
	}
	ZFS_OBJ_HOLD_EXIT(zfsvfs, obj);
}

void
zfs_create_fs(objset_t *os, cred_t *cr, nvlist_t *zplprops, dmu_tx_t *tx)
{
	zfsvfs_t *zfsvfs;
	znode_t *rootzp, *zp;
	zfs_acl_ids_t acl_ids;
	vattr_t vattr;
	nvpair_t *elem = NULL;
	uint64_t moid = MASTER_NODE_OBJ;
	uint64_t obj, sa_obj, version;
	uint64_t sense = ZFS_CASE_SENSITIVE;
	uint64_t norm = 0;
	int error, i;

	error = zap_create_claim(os, moid, DMU_OT_MASTER_NODE,
	    DMU_OT_NONE, 0, tx);
	ASSERT0(error);

	version = zfs_zpl_version_map(spa_version(dmu_objset_spa(os)));
	while ((elem = nvlist_next_nvpair(zplprops, elem)) != NULL) {
		uint64_t val;
		const char *name;

		ASSERT3S(nvpair_type(elem), ==, DATA_TYPE_UINT64);
		val = fnvpair_value_uint64(elem);
		name = nvpair_name(elem);
		if (strcmp(name, zfs_prop_to_name(ZFS_PROP_VERSION)) == 0) {
			if (val < version)
				version = val;
		} else {
			error = zap_update(os, moid, name, 8, 1, &val, tx);
		}
		ASSERT0(error);
		if (strcmp(name, zfs_prop_to_name(ZFS_PROP_NORMALIZE)) == 0)
			norm = val;
		else if (strcmp(name, zfs_prop_to_name(ZFS_PROP_CASE)) == 0)
			sense = val;
	}
	ASSERT3U(version, !=, 0);
	VERIFY0(zap_update(os, moid, ZPL_VERSION_STR, 8, 1, &version, tx));

	if (version >= ZPL_VERSION_SA) {
		sa_obj = zap_create(os, DMU_OT_SA_MASTER_NODE,
		    DMU_OT_NONE, 0, tx);
		VERIFY0(zap_add(os, moid, ZFS_SA_ATTRS, 8, 1, &sa_obj, tx));
	} else {
		sa_obj = 0;
	}

	obj = zap_create(os, DMU_OT_UNLINKED_SET, DMU_OT_NONE, 0, tx);
	VERIFY0(zap_add(os, moid, ZFS_UNLINKED_SET, 8, 1, &obj, tx));

	memset(&vattr, 0, sizeof (vattr));
	vattr.va_mask = AT_MODE | AT_UID | AT_GID;
	vattr.va_type = VDIR;
	vattr.va_mode = S_IFDIR | 0755;
	vattr.va_uid = crgetuid(cr);
	vattr.va_gid = crgetgid(cr);

	zfsvfs = kmem_zalloc(sizeof (*zfsvfs), KM_SLEEP);
	zfsvfs->z_os = os;
	zfsvfs->z_parent = zfsvfs;
	zfsvfs->z_version = version;
	zfsvfs->z_use_fuids = USE_FUIDS(version, os);
	zfsvfs->z_use_sa = USE_SA(version, os);
	zfsvfs->z_norm = norm;
	if (sense == ZFS_CASE_INSENSITIVE || sense == ZFS_CASE_MIXED)
		zfsvfs->z_norm |= U8_TEXTPREP_TOUPPER;

	VERIFY0(sa_setup(os, sa_obj, zfs_attr_table, ZPL_END,
	    &zfsvfs->z_attr_table));
	mutex_init(&zfsvfs->z_znodes_lock, NULL, MUTEX_DEFAULT, NULL);
	list_create(&zfsvfs->z_all_znodes, sizeof (znode_t),
	    offsetof(znode_t, z_link_node));
	for (i = 0; i < ZFS_OBJ_MTX_SZ; i++)
		mutex_init(&zfsvfs->z_hold_mtx[i], NULL, MUTEX_DEFAULT, NULL);

	rootzp = kmem_cache_alloc(znode_cache, KM_SLEEP);
	rootzp->z_zfsvfs = zfsvfs;
	rootzp->z_is_sa = zfsvfs->z_use_sa;
	VERIFY0(zfs_acl_ids_create(rootzp, IS_ROOT_NODE, &vattr,
	    cr, NULL, &acl_ids, NULL));
	zp = rootzp;
	zfs_mknode(rootzp, &vattr, tx, cr, IS_ROOT_NODE, &zp, &acl_ids);
	ASSERT3P(zp, ==, rootzp);
	VERIFY0(zap_add(os, moid, ZFS_ROOT_OBJ, 8, 1, &rootzp->z_id, tx));
	zfs_acl_ids_free(&acl_ids);

	sa_handle_destroy(rootzp->z_sa_hdl);
	rootzp->z_sa_hdl = NULL;
	POINTER_INVALIDATE(&rootzp->z_zfsvfs);
	zfs_znode_rele(rootzp);
	for (i = 0; i < ZFS_OBJ_MTX_SZ; i++)
		mutex_destroy(&zfsvfs->z_hold_mtx[i]);
	list_destroy(&zfsvfs->z_all_znodes);
	mutex_destroy(&zfsvfs->z_znodes_lock);
	kmem_free(zfsvfs, sizeof (*zfsvfs));
}

void
zfs_znode_delete(znode_t *zp, dmu_tx_t *tx)
{
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	uint64_t obj = zp->z_id;
	uint64_t acl_obj = zfs_external_acl(zp);

	ZFS_OBJ_HOLD_ENTER(zfsvfs, obj);
	if (acl_obj != 0)
		VERIFY0(dmu_object_free(zfsvfs->z_os, acl_obj, tx));
	VERIFY0(dmu_object_free(zfsvfs->z_os, obj, tx));
	zfs_znode_dmu_fini(zp);
	ZFS_OBJ_HOLD_EXIT(zfsvfs, obj);
}

void
zfs_tstamp_update_setup_ext(znode_t *zp, uint_t flags, uint64_t mtime[2],
    uint64_t ctime[2], boolean_t have_tx)
{
	struct timespec now;

	nanotime(&now);
	if (have_tx) {
		zp->z_atime_dirty = B_FALSE;
		zp->z_seq++;
	} else {
		zp->z_atime_dirty = B_TRUE;
	}
	if (flags & AT_ATIME)
		ZFS_TIME_ENCODE(&now, zp->z_atime);
	if (flags & AT_MTIME) {
		ZFS_TIME_ENCODE(&now, mtime);
		if (zp->z_zfsvfs->z_use_fuids)
			zp->z_pflags |= ZFS_ARCHIVE | ZFS_AV_MODIFIED;
	}
	if (flags & AT_CTIME) {
		ZFS_TIME_ENCODE(&now, ctime);
		if (zp->z_zfsvfs->z_use_fuids)
			zp->z_pflags |= ZFS_ARCHIVE;
	}
}

void
zfs_tstamp_update_setup(znode_t *zp, uint_t flags, uint64_t mtime[2],
    uint64_t ctime[2])
{

	zfs_tstamp_update_setup_ext(zp, flags, mtime, ctime, B_TRUE);
}

void
zfs_grow_blocksize(znode_t *zp, uint64_t size, dmu_tx_t *tx)
{
	u_longlong_t dummy;
	int error;

	if (size <= zp->z_blksz)
		return;
	if (zp->z_blksz != 0 && zp->z_size > zp->z_blksz)
		return;

	error = dmu_object_set_blocksize(zp->z_zfsvfs->z_os, zp->z_id,
	    size, 0, tx);
	if (error == ENOTSUP)
		return;
	ASSERT0(error);
	dmu_object_size_from_db(sa_get_db(zp->z_sa_hdl), &zp->z_blksz,
	    &dummy);
}

/*
 * Update the optional on-disk attributes used by ZIL replay and by the
 * small native chflags(2) mapping.  Authorization is done by VOP_SETATTR;
 * this routine only changes the SA state inside an assigned transaction.
 */
void
zfs_xvattr_set(znode_t *zp, xvattr_t *xvap, dmu_tx_t *tx)
{
	xoptattr_t *xoap = xva_getxoptattr(xvap);

	ASSERT3P(xoap, !=, NULL);
	if (XVA_ISSET_REQ(xvap, XAT_CREATETIME)) {
		uint64_t times[2];

		ZFS_TIME_ENCODE(&xoap->xoa_createtime, times);
		VERIFY0(sa_update(zp->z_sa_hdl, SA_ZPL_CRTIME(zp->z_zfsvfs),
		    times, sizeof (times), tx));
		XVA_SET_RTN(xvap, XAT_CREATETIME);
	}
#define	ZFS_XVATTR_SET(xat, zflag, field) do {                    \
	if (XVA_ISSET_REQ(xvap, (xat))) {                          \
		ZFS_ATTR_SET(zp, (zflag), xoap->field, zp->z_pflags, tx); \
		XVA_SET_RTN(xvap, (xat));                              \
	}                                                            \
} while (0)
	ZFS_XVATTR_SET(XAT_READONLY, ZFS_READONLY, xoa_readonly);
	ZFS_XVATTR_SET(XAT_HIDDEN, ZFS_HIDDEN, xoa_hidden);
	ZFS_XVATTR_SET(XAT_SYSTEM, ZFS_SYSTEM, xoa_system);
	ZFS_XVATTR_SET(XAT_ARCHIVE, ZFS_ARCHIVE, xoa_archive);
	ZFS_XVATTR_SET(XAT_IMMUTABLE, ZFS_IMMUTABLE, xoa_immutable);
	ZFS_XVATTR_SET(XAT_NOUNLINK, ZFS_NOUNLINK, xoa_nounlink);
	ZFS_XVATTR_SET(XAT_APPENDONLY, ZFS_APPENDONLY, xoa_appendonly);
	ZFS_XVATTR_SET(XAT_NODUMP, ZFS_NODUMP, xoa_nodump);
	ZFS_XVATTR_SET(XAT_OPAQUE, ZFS_OPAQUE, xoa_opaque);
	ZFS_XVATTR_SET(XAT_AV_QUARANTINED, ZFS_AV_QUARANTINED,
	    xoa_av_quarantined);
	ZFS_XVATTR_SET(XAT_AV_MODIFIED, ZFS_AV_MODIFIED, xoa_av_modified);
	ZFS_XVATTR_SET(XAT_REPARSE, ZFS_REPARSE, xoa_reparse);
	ZFS_XVATTR_SET(XAT_OFFLINE, ZFS_OFFLINE, xoa_offline);
	ZFS_XVATTR_SET(XAT_SPARSE, ZFS_SPARSE, xoa_sparse);
	ZFS_XVATTR_SET(XAT_PROJINHERIT, ZFS_PROJINHERIT, xoa_projinherit);
#undef ZFS_XVATTR_SET
}

static int
zfs_extend(znode_t *zp, uint64_t end)
{
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	zfs_locked_range_t *lr;
	dmu_tx_t *tx;
	uint64_t newblksz;
	int error;

	lr = zfs_rangelock_enter(&zp->z_rangelock, 0, UINT64_MAX, RL_WRITER);
	if (end <= zp->z_size) {
		zfs_rangelock_exit(lr);
		return (0);
	}

	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
	zfs_sa_upgrade_txholds(tx, zp);
	if (end > zp->z_blksz &&
	    (!ISP2(zp->z_blksz) || zp->z_blksz < zfsvfs->z_max_blksz)) {
		if (zp->z_blksz > zfsvfs->z_max_blksz) {
			ASSERT(!ISP2(zp->z_blksz));
			newblksz = MIN(end, 1ULL << highbit64(zp->z_blksz));
		} else {
			newblksz = MIN(end, zfsvfs->z_max_blksz);
		}
		dmu_tx_hold_write(tx, zp->z_id, 0, newblksz);
	} else {
		newblksz = 0;
	}

	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error != 0) {
		dmu_tx_abort(tx);
		zfs_rangelock_exit(lr);
		return (error);
	}
	if (newblksz != 0)
		zfs_grow_blocksize(zp, newblksz, tx);
	zp->z_size = end;
	VERIFY0(sa_update(zp->z_sa_hdl, SA_ZPL_SIZE(zfsvfs), &zp->z_size,
	    sizeof (zp->z_size), tx));
	dmu_tx_commit(tx);
	zfs_znode_update_vfs(zp);
	zfs_rangelock_exit(lr);
	return (0);
}

static int
zfs_free_range(znode_t *zp, uint64_t off, uint64_t len)
{
	zfs_locked_range_t *lr;
	int error;

	zfs_vnode_flush_cached_data(ZTOV(zp), B_TRUE);
	mutex_enter(&zp->z_map_lock);
	lr = zfs_rangelock_enter(&zp->z_rangelock, off, len, RL_WRITER);
	if (off >= zp->z_size) {
		zfs_rangelock_exit(lr);
		mutex_exit(&zp->z_map_lock);
		return (0);
	}
	if (len > zp->z_size - off)
		len = zp->z_size - off;
	error = dmu_free_long_range(zp->z_zfsvfs->z_os, zp->z_id, off, len);
	if (error == 0)
		(void)uvm_vnp_flush(ZTOV(zp), off, off + len, PGO_FREE);
	zfs_rangelock_exit(lr);
	mutex_exit(&zp->z_map_lock);
	return (error);
}

static int
zfs_trunc(znode_t *zp, uint64_t end)
{
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	zfs_locked_range_t *lr;
	sa_bulk_attr_t bulk[2];
	dmu_tx_t *tx;
	int count = 0;
	int error;

	zfs_vnode_flush_cached_data(ZTOV(zp), B_TRUE);
	mutex_enter(&zp->z_map_lock);
	lr = zfs_rangelock_enter(&zp->z_rangelock, 0, UINT64_MAX, RL_WRITER);
	if (end >= zp->z_size) {
		zfs_rangelock_exit(lr);
		mutex_exit(&zp->z_map_lock);
		return (0);
	}
	error = dmu_free_long_range(zfsvfs->z_os, zp->z_id, end,
	    DMU_OBJECT_END);
	if (error != 0) {
		zfs_rangelock_exit(lr);
		mutex_exit(&zp->z_map_lock);
		return (error);
	}

	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
	zfs_sa_upgrade_txholds(tx, zp);
	dmu_tx_mark_netfree(tx);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error != 0) {
		dmu_tx_abort(tx);
		zfs_rangelock_exit(lr);
		mutex_exit(&zp->z_map_lock);
		return (error);
	}
	zp->z_size = end;
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_SIZE(zfsvfs), NULL,
	    &zp->z_size, sizeof (zp->z_size));
	if (end == 0) {
		zp->z_pflags &= ~ZFS_SPARSE;
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_FLAGS(zfsvfs), NULL,
		    &zp->z_pflags, sizeof (zp->z_pflags));
	}
	VERIFY0(sa_bulk_update(zp->z_sa_hdl, bulk, count, tx));
	dmu_tx_commit(tx);
	zfs_znode_update_vfs(zp);
	zfs_rangelock_exit(lr);
	mutex_exit(&zp->z_map_lock);
	return (0);
}

int
zfs_freesp(znode_t *zp, uint64_t off, uint64_t len, int flag, boolean_t log)
{
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	sa_bulk_attr_t bulk[3];
	uint64_t mtime[2], ctime[2];
	dmu_tx_t *tx;
	int count = 0;
	int error;

	(void)flag;
	KERNEL_ASSERT_LOCKED();
	KASSERT(VOP_ISLOCKED(ZTOV(zp)));
	if (len != 0 && off > UINT64_MAX - len)
		return (EFBIG);
	if (off > zp->z_size)
		error = zfs_extend(zp, off + len);
	else if (len == 0)
		error = zfs_trunc(zp, off);
	else if ((error = zfs_free_range(zp, off, len)) == 0 &&
	    off + len > zp->z_size)
		error = zfs_extend(zp, off + len);
	if (error != 0 || !log)
		return (error);

	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
	zfs_sa_upgrade_txholds(tx, zp);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error != 0) {
		dmu_tx_abort(tx);
		return (error);
	}
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MTIME(zfsvfs), NULL,
	    mtime, sizeof (mtime));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_CTIME(zfsvfs), NULL,
	    ctime, sizeof (ctime));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_FLAGS(zfsvfs), NULL,
	    &zp->z_pflags, sizeof (zp->z_pflags));
	zfs_tstamp_update_setup(zp, CONTENT_MODIFIED, mtime, ctime);
	VERIFY0(sa_bulk_update(zp->z_sa_hdl, bulk, count, tx));
	zfs_log_truncate(zfsvfs->z_log, tx, TX_TRUNCATE, zp, off, len);
	dmu_tx_commit(tx);
	return (0);
}

/*
 * Allocate and lock the native vnode before beginning a DMU transaction.
 * getnewvnode() may reclaim another ZFS vnode, so callers must not hold an
 * object hold or an assigned transaction while entering here.
 */
int
zfs_znode_alloc_vnode(zfsvfs_t *zfsvfs, znode_t **zpp)
{
	znode_t *zp;
	vnode_t *vp;
	int error;

	KERNEL_ASSERT_LOCKED();
	*zpp = NULL;
	zp = kmem_cache_alloc(znode_cache, KM_SLEEP);
	zp->z_zfsvfs = zfsvfs;
	zp->z_unlinked = B_FALSE;
	zp->z_atime_dirty = B_FALSE;
	zp->z_mapcnt = 0;
	zp->z_seq = 0x7a4653;
	zp->z_sync_cnt = 0;

	error = getnewvnode(VT_ZFS, zfsvfs->z_vfs, &zfs_vnodeops, &vp);
	if (error != 0) {
		POINTER_INVALIDATE(&zp->z_zfsvfs);
		zfs_znode_rele(zp);
		return (error);
	}
	zp->z_vnode = vp;
	vp->v_data = zp;
	vp->v_uvn_ops = &zfs_uvn_pagerops;
	VERIFY0(vn_lock(vp, LK_EXCLUSIVE | LK_RETRY));
	*zpp = zp;
	return (0);
}

static int
zfs_znode_load(zfsvfs_t *zfsvfs, znode_t *zp, dmu_buf_t *db,
    dmu_object_info_t *doi)
{
	uint64_t mode, parent;
	uint64_t projid = ZFS_DEFAULT_PROJID;
	sa_bulk_attr_t bulk[9];
	int count = 0;
	int error;

	zfs_znode_sa_init(zfsvfs, zp, db, doi->doi_bonus_type);
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MODE(zfsvfs), NULL,
	    &mode, sizeof (mode));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_GEN(zfsvfs), NULL,
	    &zp->z_gen, sizeof (zp->z_gen));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_SIZE(zfsvfs), NULL,
	    &zp->z_size, sizeof (zp->z_size));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_LINKS(zfsvfs), NULL,
	    &zp->z_links, sizeof (zp->z_links));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_FLAGS(zfsvfs), NULL,
	    &zp->z_pflags, sizeof (zp->z_pflags));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_PARENT(zfsvfs), NULL,
	    &parent, sizeof (parent));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_ATIME(zfsvfs), NULL,
	    &zp->z_atime, sizeof (zp->z_atime));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_UID(zfsvfs), NULL,
	    &zp->z_uid, sizeof (zp->z_uid));
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_GID(zfsvfs), NULL,
	    &zp->z_gid, sizeof (zp->z_gid));

	error = sa_bulk_lookup(zp->z_sa_hdl, bulk, count);
	if (error != 0)
		return (error);
	if (zp->z_gen == 0)
		return (ENOENT);

	if (dmu_objset_projectquota_enabled(zfsvfs->z_os) &&
	    (zp->z_pflags & ZFS_PROJID)) {
		error = sa_lookup(zp->z_sa_hdl, SA_ZPL_PROJID(zfsvfs),
		    &projid, sizeof (projid));
		if (error != 0)
			return (error);
	}

	zp->z_mode = (mode_t)mode;
	zp->z_projid = projid;
	zp->z_unlinked = (zp->z_links == 0);
	zp->z_zn_prefetch = (IFTOVT(zp->z_mode) == VDIR);
	if (zp->z_pflags & ZFS_XATTR)
		zp->z_xattr_parent = parent;

	return (0);
}

static int
zfs_znode_bonus_hold(zfsvfs_t *zfsvfs, uint64_t obj, dmu_buf_t **dbp,
    dmu_object_info_t *doi)
{
	int error;

	error = sa_buf_hold(zfsvfs->z_os, obj, NULL, dbp);
	if (error != 0)
		return (error);
	dmu_object_info_from_db(*dbp, doi);
	if (doi->doi_bonus_type != DMU_OT_SA &&
	    (doi->doi_bonus_type != DMU_OT_ZNODE ||
	    doi->doi_bonus_size < sizeof (znode_phys_t))) {
		sa_buf_rele(*dbp, NULL);
		*dbp = NULL;
		return (EINVAL);
	}
	return (0);
}

/* The caller holds a transient znode reference taken under the object hold. */
static int
zfs_znode_vget(znode_t *zp, vnode_t *vp, boolean_t allow_unlinked,
    znode_t **zpp)
{
	const struct vops *vops = &zfs_vnodeops;
	boolean_t stale = B_FALSE;
	int error;

#ifdef FIFO
	if (vp->v_type == VFIFO)
		vops = &zfs_fifovops;
#endif
	error = vget(vp, LK_EXCLUSIVE);
	if (error == ENOENT)
		stale = B_TRUE;
	if (error == 0 &&
	    (vp->v_data != zp || vp->v_op != vops)) {
		vput(vp);
		error = ENOENT;
		stale = B_TRUE;
	} else if (error == 0 && zp->z_unlinked && !allow_unlinked) {
		vput(vp);
		error = ENOENT;
	}
	if (error == 0)
		*zpp = zp;
	zfs_znode_rele(zp);
	return (stale ? EAGAIN : error);
}

/*
 * Return a referenced, exclusively locked vnode, matching OpenBSD VFS_VGET.
 */
static int
zfs_zget_impl(zfsvfs_t *zfsvfs, uint64_t obj, boolean_t allow_unlinked,
    znode_t **zpp)
{
	dmu_object_info_t doi;
	dmu_buf_t *db;
	sa_handle_t *hdl;
	znode_t *zp;
	vnode_t *vp;
	int error;

	KERNEL_ASSERT_LOCKED();
	*zpp = NULL;
again:
	/* The usual incore case does not allocate or recycle a vnode. */
	ZFS_OBJ_HOLD_ENTER(zfsvfs, obj);
	error = zfs_znode_bonus_hold(zfsvfs, obj, &db, &doi);
	if (error != 0) {
		ZFS_OBJ_HOLD_EXIT(zfsvfs, obj);
		return (error);
	}
	hdl = dmu_buf_get_user(db);
	if (hdl != NULL) {
		zp = sa_get_userdata(hdl);
		ASSERT3P(zp, !=, NULL);
		ASSERT3U(zp->z_id, ==, obj);
		refcnt_take(&zp->z_refs);
		vp = zp->z_vnode;
		sa_buf_rele(db, NULL);
		ZFS_OBJ_HOLD_EXIT(zfsvfs, obj);
		error = zfs_znode_vget(zp, vp, allow_unlinked, zpp);
		if (error == EAGAIN)
			goto again;
		return (error);
	}
	sa_buf_rele(db, NULL);
	ZFS_OBJ_HOLD_EXIT(zfsvfs, obj);

	/* Allocate before taking a ZFS object hold: getnewvnode() may reclaim. */
	error = zfs_znode_alloc_vnode(zfsvfs, &zp);
	if (error != 0)
		return (error);
	zp->z_id = obj;
	vp = ZTOV(zp);

	ZFS_OBJ_HOLD_ENTER(zfsvfs, obj);
	error = zfs_znode_bonus_hold(zfsvfs, obj, &db, &doi);
	if (error != 0) {
		ZFS_OBJ_HOLD_EXIT(zfsvfs, obj);
		zfs_znode_discard(zp);
		return (error);
	}

	hdl = dmu_buf_get_user(db);
	if (hdl != NULL) {
		znode_t *found = sa_get_userdata(hdl);
		vnode_t *foundvp;

		ASSERT3P(found, !=, NULL);
		ASSERT3U(found->z_id, ==, obj);
		refcnt_take(&found->z_refs);
		foundvp = found->z_vnode;
		sa_buf_rele(db, NULL);
		ZFS_OBJ_HOLD_EXIT(zfsvfs, obj);
		zfs_znode_discard(zp);

		error = zfs_znode_vget(found, foundvp, allow_unlinked, zpp);
		if (error == EAGAIN)
			goto again;
		return (error);
	}

	zp->z_blksz = doi.doi_data_block_size;
	zp->z_dnodesize = doi.doi_dnodesize;
	error = zfs_znode_load(zfsvfs, zp, db, &doi);
	if (error != 0) {
		zfs_znode_dmu_fini(zp);
		ZFS_OBJ_HOLD_EXIT(zfsvfs, obj);
		zfs_znode_discard(zp);
		return (error);
	}

	vp->v_type = IFTOVT(zp->z_mode);
	if (vp->v_type == VNON || vp->v_type == VBAD) {
		zfs_znode_dmu_fini(zp);
		ZFS_OBJ_HOLD_EXIT(zfsvfs, obj);
		zfs_znode_discard(zp);
		return (EINVAL);
	}
#ifdef FIFO
	if (vp->v_type == VFIFO)
		vp->v_op = &zfs_fifovops;
#endif
	if (obj == zfsvfs->z_root && zfsvfs->z_parent == zfsvfs)
		vp->v_flag |= VROOT;
	uvm_vnp_setsize(vp, zp->z_size);

	mutex_enter(&zfsvfs->z_znodes_lock);
	list_insert_tail(&zfsvfs->z_all_znodes, zp);
	mutex_exit(&zfsvfs->z_znodes_lock);

	ZFS_OBJ_HOLD_EXIT(zfsvfs, obj);
	if (zp->z_unlinked && !allow_unlinked) {
		zfs_znode_discard(zp);
		return (ENOENT);
	}
	*zpp = zp;
	return (0);
}

/* Return ordinary namespace objects only. */
int
zfs_zget(zfsvfs_t *zfsvfs, uint64_t obj, znode_t **zpp)
{

	return (zfs_zget_impl(zfsvfs, obj, B_FALSE, zpp));
}

/* Mount-time recovery must instantiate entries from the unlinked set. */
int
zfs_zget_unlinked(zfsvfs_t *zfsvfs, uint64_t obj, znode_t **zpp)
{

	return (zfs_zget_impl(zfsvfs, obj, B_TRUE, zpp));
}

int
zfs_rezget(znode_t *zp)
{

	(void)zp;
	/* Suspend/resume also needs the native VM invalidation contract. */
	return (EOPNOTSUPP);
}

void
zfs_znode_update_vfs(znode_t *zp)
{

	KERNEL_ASSERT_LOCKED();
	if (zp->z_vnode != NULL)
		uvm_vnp_setsize(zp->z_vnode, zp->z_size);
}
