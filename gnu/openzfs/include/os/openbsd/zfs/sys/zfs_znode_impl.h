// SPDX-License-Identifier: CDDL-1.0
/* OpenBSD-specific znode representation and vnode mappings. */

#ifndef _SYS_ZFS_ZNODE_IMPL_H
#define	_SYS_ZFS_ZNODE_IMPL_H

#ifndef _KERNEL
#error "no user serviceable parts within"
#endif

#include <sys/dmu.h>
#include <sys/list.h>
#include <sys/refcnt.h>
#include <sys/rrwlock.h>
#include <sys/sa.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/zfs_rlock.h>
#include <sys/zfs_sa.h>
#include <sys/zfs_stat.h>
#include <sys/zfs_vfsops.h>

#define	ZNODE_OS_FIELDS				\
	zfsvfs_t	*z_zfsvfs;		\
	vnode_t		*z_vnode;		\
	struct rrwlock	z_vlock;		\
	kmutex_t	z_map_lock;		\
	struct refcnt	z_refs;			\
	struct lockf_state *z_lockf;		\
	char		*z_cached_symlink;	\
	uint64_t	z_uid;			\
	uint64_t	z_gid;			\
	uint64_t	z_gen;			\
	uint64_t	z_atime[2];		\
	uint64_t	z_links;

#define	ZFS_LINK_MAX	UINT32_MAX

#define	ZTOV(zp)	((zp)->z_vnode)
#define	ZTOI(zp)	ZTOV(zp)
#define	VTOZ(vp)	((struct znode *)(vp)->v_data)
#define	ITOZ(vp)	VTOZ(vp)
#define	ZTOZSB(zp)	((zp)->z_zfsvfs)
#define	ITOZSB(vp)	(VTOZ(vp)->z_zfsvfs)

#define	ZTOTYPE(zp)	(ZTOV(zp)->v_type)
#define	ZTOGID(zp)	((zp)->z_gid)
#define	ZTOUID(zp)	((zp)->z_uid)
#define	ZTONLNK(zp)	((zp)->z_links)

#define	Z_ISBLK(type)	((type) == VBLK)
#define	Z_ISCHR(type)	((type) == VCHR)
#define	Z_ISLNK(type)	((type) == VLNK)
#define	Z_ISDIR(type)	((type) == VDIR)

#define	zhold(zp)	vref(ZTOV(zp))
/* zfs_zget() returns a referenced, exclusively locked OpenBSD vnode. */
#define	zrele(zp)	vput(ZTOV(zp))

extern boolean_t zfs_vnode_has_cached_data(vnode_t *, uint64_t, uint64_t);
extern void zfs_vnode_flush_cached_data(vnode_t *, boolean_t);
extern int zfs_rlimit_fsize(off_t);
extern int zfs_rlimit_fsize_uio(struct znode *, zfs_uio_t *);

#define	zn_has_cached_data(zp, start, end)			\
	zfs_vnode_has_cached_data(ZTOV(zp), (start), (end))
#define	zn_flush_cached_data(zp, sync)				\
	zfs_vnode_flush_cached_data(ZTOV(zp), (sync))
#define	zn_rlimit_fsize(size)	zfs_rlimit_fsize((size))
#define	zn_rlimit_fsize_uio(zp, uio)				\
	zfs_rlimit_fsize_uio((zp), (uio))

static inline int
zfs_enter(zfsvfs_t *zfsvfs, const char *tag)
{
	ZFS_TEARDOWN_ENTER_READ(zfsvfs, tag);
	if (__predict_false(zfsvfs->z_unmounted)) {
		ZFS_TEARDOWN_EXIT_READ(zfsvfs, tag);
		return (SET_ERROR(EIO));
	}
	return (0);
}

static inline void
zfs_exit(zfsvfs_t *zfsvfs, const char *tag)
{
	ZFS_TEARDOWN_EXIT_READ(zfsvfs, tag);
}

#define	ZFS_OBJ_HASH(obj)	((obj) & (ZFS_OBJ_MTX_SZ - 1))
#define	ZFS_OBJ_MUTEX(zfsvfs, obj)				\
	(&(zfsvfs)->z_hold_mtx[ZFS_OBJ_HASH(obj)])
#define	ZFS_OBJ_HOLD_ENTER(zfsvfs, obj)				\
	mutex_enter(ZFS_OBJ_MUTEX((zfsvfs), (obj)))
#define	ZFS_OBJ_HOLD_TRYENTER(zfsvfs, obj)			\
	mutex_tryenter(ZFS_OBJ_MUTEX((zfsvfs), (obj)))
#define	ZFS_OBJ_HOLD_EXIT(zfsvfs, obj)				\
	mutex_exit(ZFS_OBJ_MUTEX((zfsvfs), (obj)))

#define	ZFS_TIME_ENCODE(tp, stmp)				\
	do {							\
		(stmp)[0] = (uint64_t)(tp)->tv_sec;		\
		(stmp)[1] = (uint64_t)(tp)->tv_nsec;		\
	} while (0)

#define	ZFS_TIME_DECODE(tp, stmp)				\
	do {							\
		(tp)->tv_sec = (time_t)(stmp)[0];		\
		(tp)->tv_nsec = (long)(stmp)[1];		\
	} while (0)

#define	ZFS_ACCESSTIME_STAMP(zfsvfs, zp)			\
	do {							\
		if ((zfsvfs)->z_atime &&				\
		    !((zfsvfs)->z_vfs->vfs_flag & VFS_RDONLY))	\
			zfs_tstamp_update_setup_ext((zp), ACCESSED,	\
			    NULL, NULL, B_FALSE);			\
	} while (0)

extern void zfs_tstamp_update_setup_ext(struct znode *, uint_t,
    uint64_t [2], uint64_t [2], boolean_t);
extern int zfs_znode_alloc_vnode(zfsvfs_t *, struct znode **);
extern int zfs_zget_unlinked(zfsvfs_t *, uint64_t, struct znode **);
extern void zfs_znode_discard(struct znode *);
extern void zfs_mknode(struct znode *, vattr_t *, dmu_tx_t *, cred_t *,
    uint_t, struct znode **, zfs_acl_ids_t *);
extern void zfs_znode_free(struct znode *);
extern zil_replay_func_t *const zfs_replay_vector[TX_MAX_TYPE];

#endif /* _SYS_ZFS_ZNODE_IMPL_H */
