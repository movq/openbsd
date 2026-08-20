// SPDX-License-Identifier: CDDL-1.0
/* ZFS directory interfaces used by the OpenBSD vnode layer. */

#ifndef _SYS_ZFS_DIR_H
#define	_SYS_ZFS_DIR_H

#include <sys/zfs_znode.h>

/* zfs_dirent_lookup() flags */
#define	ZNEW		0x0001		/* entry should not exist */
#define	ZEXISTS		0x0002		/* entry should exist */
#define	ZRENAMING	0x0010

/* zfs_mknode() flags */
#define	IS_ROOT_NODE	0x01
#define	IS_XATTR	0x02

extern int zfs_dirent_lookup(znode_t *, const char *, znode_t **, int);
extern int zfs_dirent_object(znode_t *, const char *, uint64_t *);
extern int zfs_dirent_object_name(znode_t *, const char *, uint64_t *,
    char *, size_t);
extern int zfs_link_create(znode_t *, const char *, znode_t *, dmu_tx_t *,
    int);
extern int zfs_link_destroy(znode_t *, const char *, znode_t *, dmu_tx_t *,
    int, boolean_t *);
extern void zfs_unlinked_add(znode_t *, dmu_tx_t *);
extern void zfs_unlinked_drain(zfsvfs_t *);
extern void zfs_rmnode(znode_t *);
extern boolean_t zfs_dirempty(znode_t *);
extern int zfs_make_xattrdir(znode_t *, vattr_t *, znode_t **, cred_t *);

#endif /* _SYS_ZFS_DIR_H */
