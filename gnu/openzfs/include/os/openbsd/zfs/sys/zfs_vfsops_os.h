// SPDX-License-Identifier: CDDL-1.0

#ifndef _SYS_ZFS_VFSOPS_OS_H
#define	_SYS_ZFS_VFSOPS_OS_H

#include <sys/dataset_kstats.h>
#include <sys/list.h>
#include <sys/rrwlock.h>
#include <sys/sa.h>
#include <sys/taskq.h>
#include <sys/vfs.h>
#include <sys/zfs_ioctl.h>
#include <sys/zil.h>

typedef struct vnode vnode_t;
typedef struct zfsvfs zfsvfs_t;
struct znode;

struct zfsvfs {
	vfs_t		*z_vfs;		/* native mount structure */
	zfsvfs_t	*z_parent;	/* parent filesystem */
	objset_t	*z_os;		/* owned objset */
	uint64_t	z_flags;
	uint64_t	z_root;
	uint64_t	z_unlinkedobj;
	uint64_t	z_max_blksz;
	uint64_t	z_fuid_obj;
	uint64_t	z_fuid_size;
	avl_tree_t	z_fuid_idx;
	avl_tree_t	z_fuid_domain;
	krwlock_t	z_fuid_lock;
	boolean_t	z_fuid_loaded;
	boolean_t	z_fuid_dirty;
	struct zfs_fuid_info *z_fuid_replay;
	zilog_t		*z_log;
	uint_t		z_acl_type;
	uint_t		z_acl_mode;
	uint_t		z_acl_inherit;
	zfs_case_t	z_case;
	boolean_t	z_utf8;
	int		z_norm;
	boolean_t	z_atime;
	boolean_t	z_unmounted;
	rrmlock_t	z_teardown_lock;
	krwlock_t	z_teardown_inactive_lock;
	list_t		z_all_znodes;
	kmutex_t	z_znodes_lock;
	kmutex_t	z_rename_lock;	/* serializes namespace rename */
	struct zfsctl_root *z_ctldir;
	uint_t		z_show_ctldir;
	boolean_t	z_issnap;
	boolean_t	z_use_fuids;
	boolean_t	z_replay;
	boolean_t	z_use_sa;
	boolean_t	z_xattr_sa;
	boolean_t	z_use_namecache;
	boolean_t	z_longname;
	uint8_t		z_xattr;
	uint64_t	z_version;
	uint64_t	z_shares_dir;
	dataset_kstats_t z_kstat;
	kmutex_t	z_lock;
	uint64_t	z_userquota_obj;
	uint64_t	z_groupquota_obj;
	uint64_t	z_userobjquota_obj;
	uint64_t	z_groupobjquota_obj;
	uint64_t	z_projectquota_obj;
	uint64_t	z_projectobjquota_obj;
	uint64_t	z_defaultuserquota;
	uint64_t	z_defaultgroupquota;
	uint64_t	z_defaultprojectquota;
	uint64_t	z_defaultuserobjquota;
	uint64_t	z_defaultgroupobjquota;
	uint64_t	z_defaultprojectobjquota;
	uint64_t	z_replay_eof;
	sa_attr_type_t	*z_attr_table;
#define	ZFS_OBJ_MTX_SZ	64
	kmutex_t	z_hold_mtx[ZFS_OBJ_MTX_SZ];
	taskqid_t	z_drain_task;
};

#define	ZFS_TEARDOWN_INIT(zfsvfs)				\
	rrm_init(&(zfsvfs)->z_teardown_lock, B_FALSE)
#define	ZFS_TEARDOWN_DESTROY(zfsvfs)				\
	rrm_destroy(&(zfsvfs)->z_teardown_lock)
#define	ZFS_TEARDOWN_ENTER_READ(zfsvfs, tag)			\
	rrm_enter_read(&(zfsvfs)->z_teardown_lock, (tag))
#define	ZFS_TEARDOWN_EXIT_READ(zfsvfs, tag)			\
	rrm_exit(&(zfsvfs)->z_teardown_lock, (tag))
#define	ZFS_TEARDOWN_ENTER_WRITE(zfsvfs, tag)			\
	rrm_enter(&(zfsvfs)->z_teardown_lock, RW_WRITER, (tag))
#define	ZFS_TEARDOWN_EXIT_WRITE(zfsvfs)			\
	rrm_exit(&(zfsvfs)->z_teardown_lock, (zfsvfs))
#define	ZFS_TEARDOWN_EXIT(zfsvfs, tag)				\
	rrm_exit(&(zfsvfs)->z_teardown_lock, (tag))
#define	ZFS_TEARDOWN_READ_HELD(zfsvfs)				\
	RRM_READ_HELD(&(zfsvfs)->z_teardown_lock)
#define	ZFS_TEARDOWN_WRITE_HELD(zfsvfs)			\
	RRM_WRITE_HELD(&(zfsvfs)->z_teardown_lock)
#define	ZFS_TEARDOWN_HELD(zfsvfs)				\
	RRM_LOCK_HELD(&(zfsvfs)->z_teardown_lock)

#define	ZFS_TEARDOWN_INACTIVE_INIT(zfsvfs)			\
	rw_init(&(zfsvfs)->z_teardown_inactive_lock, NULL, RW_DEFAULT, NULL)
#define	ZFS_TEARDOWN_INACTIVE_DESTROY(zfsvfs)			\
	rw_destroy(&(zfsvfs)->z_teardown_inactive_lock)
#define	ZFS_TEARDOWN_INACTIVE_TRY_ENTER_READ(zfsvfs)		\
	rw_tryenter(&(zfsvfs)->z_teardown_inactive_lock, RW_READER)
#define	ZFS_TEARDOWN_INACTIVE_ENTER_READ(zfsvfs)			\
	rw_enter(&(zfsvfs)->z_teardown_inactive_lock, RW_READER)
#define	ZFS_TEARDOWN_INACTIVE_EXIT_READ(zfsvfs)			\
	rw_exit(&(zfsvfs)->z_teardown_inactive_lock)
#define	ZFS_TEARDOWN_INACTIVE_ENTER_WRITE(zfsvfs)		\
	rw_enter(&(zfsvfs)->z_teardown_inactive_lock, RW_WRITER)
#define	ZFS_TEARDOWN_INACTIVE_EXIT_WRITE(zfsvfs)			\
	rw_exit(&(zfsvfs)->z_teardown_inactive_lock)
#define	ZFS_TEARDOWN_INACTIVE_WRITE_HELD(zfsvfs)			\
	RW_WRITE_HELD(&(zfsvfs)->z_teardown_inactive_lock)

#define	ZSB_XATTR	0x0001

typedef struct zfid_short {
	uint16_t	zf_len;
	uint8_t		zf_object[6];
	uint8_t		zf_gen[4];
} zfid_short_t;

typedef struct zfid_long {
	zfid_short_t	zf_fid;
	uint8_t		zf_setid[6];
	uint8_t		zf_setgen[2];
} zfid_long_t;

#define	SHORT_FID_LEN	(sizeof (zfid_short_t) - sizeof (uint16_t))
#define	LONG_FID_LEN	(sizeof (zfid_long_t) - sizeof (uint16_t))

extern const struct vfsops zfs_vfsops;

extern void zfs_init(void);
extern void zfs_fini(void);

extern int zfs_suspend_fs(zfsvfs_t *);
extern int zfs_resume_fs(zfsvfs_t *, struct dsl_dataset *);
extern int zfs_end_fs(zfsvfs_t *, struct dsl_dataset *);
extern int zfs_set_version(zfsvfs_t *, uint64_t);
extern int zfsvfs_create(const char *, boolean_t, zfsvfs_t **);
extern int zfsvfs_create_impl(zfsvfs_t **, zfsvfs_t *, objset_t *);
extern void zfsvfs_free(zfsvfs_t *);
extern boolean_t zfs_is_readonly(zfsvfs_t *);
extern int zfs_get_temporary_prop(struct dsl_dataset *, zfs_prop_t,
    uint64_t *, char *);
extern int zfs_set_default_quota(zfsvfs_t *, zfs_prop_t, uint64_t);

#endif /* _SYS_ZFS_VFSOPS_OS_H */
