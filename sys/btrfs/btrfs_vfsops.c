/* Copyright (C) 2026 Mike Jones <mike@mjones.org>
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/specdev.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <sys/btrfsio.h>

#include <btrfs/btrfs_var.h>

static int	btrfs_mount(struct mount *, const char *, void *,
		    struct nameidata *, struct proc *);
static int	btrfs_mountfs(struct vnode *, struct mount *, uint64_t,
		    struct proc *);
static int	btrfs_attach_view(struct btrfs_fs *, struct mount *, uint64_t);
static int	btrfs_ancestor(struct btrfs_fs *, uint64_t, uint64_t, int *);
static int	btrfs_root_device(struct mount *, struct btrfs_root *);
static int	btrfs_write_extent_valid(
		    const struct btrfs_extent_record *, void *);
static int	btrfs_validate_writable(struct btrfs_fs *);
static int	btrfs_check_write_orphans(struct btrfs_fs *, int);
static int	btrfs_start(struct mount *, int, struct proc *);
static int	btrfs_unmount(struct mount *, int, struct proc *);
static int	btrfs_root(struct mount *, struct vnode **);
static int	btrfs_statfs(struct mount *, struct statfs *, struct proc *);
static int	btrfs_sync(struct mount *, int, int, struct ucred *,
		    struct proc *);
static int	btrfs_fhtovp(struct mount *, struct fid *, struct vnode **);
static int	btrfs_vptofh(struct vnode *, struct fid *);

/*
 * OpenBSD has sixteen payload bytes in a fid. Never truncate a tree ID or
 * generation to fit: VPTOFH rejects identities outside this encoding.
 */
struct btrfs_fid {
	uint16_t	bfid_len;
	uint16_t	bfid_version;
	uint32_t	bfid_treeid;
	uint64_t	bfid_ino;
	uint32_t	bfid_generation;
} __packed;

_Static_assert(sizeof(struct btrfs_fid) <= sizeof(struct fid),
    "btrfs file handle exceeds VFS identifier");

/* Attach/detach serialization; ordinary vnode and transaction paths omit it. */
static struct rwlock btrfs_mount_lock = RWLOCK_INITIALIZER("btrfsmnt");
static LIST_HEAD(, btrfs_fs) btrfs_filesystems =
    LIST_HEAD_INITIALIZER(btrfs_filesystems);
static struct mutex btrfs_device_mtx = MUTEX_INITIALIZER(IPL_NONE);
static unsigned int btrfs_device_minor;

/*
 * The mount lock pins every view for filesystem-wide administration. Rename
 * stabilizes path ancestry. The operation locks a visible parent before
 * closing transaction joins; commit never needs vnode locks.
 */
int
btrfs_control(struct mount *mp, u_long cmd,
    struct btrfs_ioctl_subvolume *args, struct proc *p)
{
	struct btrfs_fs *bmp;
	int error;

	rw_enter_write(&btrfs_mount_lock);
	bmp = VFSTOBTRFS(mp);
	if (cmd != BTRFSIOC_LIST &&
	    ((mp->mnt_flag & MNT_RDONLY) || bmp->bm_readonly)) {
		rw_exit_write(&btrfs_mount_lock);
		return (EROFS);
	}
	rw_enter_write(&bmp->bm_rename_lock);
	error = btrfs_subvolume(bmp, cmd, args, p);
	rw_exit_write(&bmp->bm_rename_lock);
	rw_exit_write(&btrfs_mount_lock);
	return (error);
}

int
btrfs_control_parent(struct btrfs_fs *bmp, uint64_t treeid, uint64_t ino,
    struct vnode **vpp)
{
	struct btrfs_mount *view;
	int error, found;

	*vpp = NULL;
	LIST_FOREACH(view, &bmp->bm_mounts, bmv_entry) {
		error = btrfs_ancestor(bmp, treeid, view->bmv_treeid, &found);
		if (error != 0)
			return (error);
		if (!found)
			continue;
		if (view->bmv_mount->mnt_flag & MNT_RDONLY)
			return (EROFS);
		return (btrfs_vget_tree(view->bmv_mount, treeid, ino, vpp));
	}
	return (0);
}

int
btrfs_identity_control(struct mount *mp, u_long cmd,
    struct btrfs_ioctl_identity *args, struct proc *p)
{
	struct btrfs_fs *bmp;
	struct btrfs_mount *view;
	char *path;
	size_t len;
	int error, found;

	rw_enter_write(&btrfs_mount_lock);
	bmp = VFSTOBTRFS(mp);
	if (cmd == BTRFSIOC_FINISH &&
	    ((mp->mnt_flag & MNT_RDONLY) || bmp->bm_readonly)) {
		rw_exit_write(&btrfs_mount_lock);
		return (EROFS);
	}
	rw_enter_write(&bmp->bm_rename_lock);
	error = btrfs_identity(bmp, cmd, args, p);
	if (error != 0 || cmd != BTRFSIOC_INFO)
		goto out;
	args->access[0] = '\0';
	path = malloc(BTRFS_CTL_PATH_MAX, M_BTRFS, M_WAITOK);
	LIST_FOREACH(view, &bmp->bm_mounts, bmv_entry) {
		error = btrfs_ancestor(bmp, args->id, view->bmv_treeid, &found);
		if (error != 0)
			break;
		if (!found)
			continue;
		error = btrfs_subvol_path(bmp, view->bmv_treeid, path,
		    BTRFS_CTL_PATH_MAX);
		if (error != 0)
			break;
		len = strlen(path);
		if (strncmp(args->path, path, len) != 0 ||
		    (len != 0 && args->path[len] != '\0' &&
		    args->path[len] != '/')) {
			error = EINVAL;
			break;
		}
		if (snprintf(args->access, sizeof(args->access), "%s/%s",
		    view->bmv_mount->mnt_stat.f_mntonname,
		    args->path + len + (args->path[len] == '/')) >=
		    sizeof(args->access))
			error = ENAMETOOLONG;
		break;
	}
	free(path, M_BTRFS, BTRFS_CTL_PATH_MAX);
out:
	rw_exit_write(&bmp->bm_rename_lock);
	rw_exit_write(&btrfs_mount_lock);
	return (error);
}

int
btrfs_control_busy(struct btrfs_fs *bmp, uint64_t treeid)
{
	struct btrfs_mount *view;
	struct btrfs_node *node;
	struct vnode *vp;
	int error, found;

	LIST_FOREACH(view, &bmp->bm_mounts, bmv_entry) {
		error = btrfs_ancestor(bmp, view->bmv_treeid, treeid, &found);
		if (error != 0 || found)
			return (error != 0 ? error : EBUSY);
	}
restart:
	mtx_enter(&bmp->bm_nodemtx);
	LIST_FOREACH(node, &bmp->bm_nodes, bn_entry) {
		if (node->bn_treeid != treeid)
			continue;
		vp = node->bn_vnode;
		if (vp->v_usecount != 0 || VOP_ISLOCKED(vp)) {
			mtx_leave(&bmp->bm_nodemtx);
			return (EBUSY);
		}
		mtx_leave(&bmp->bm_nodemtx);
		vgone(vp);
		goto restart;
	}
	mtx_leave(&bmp->bm_nodemtx);
	return (0);
}

const struct vfsops btrfs_vfsops = {
	.vfs_mount	= btrfs_mount,
	.vfs_start	= btrfs_start,
	.vfs_unmount	= btrfs_unmount,
	.vfs_root	= btrfs_root,
	.vfs_quotactl	= (void *)eopnotsupp,
	.vfs_statfs	= btrfs_statfs,
	.vfs_sync	= btrfs_sync,
	.vfs_vget	= btrfs_vget,
	.vfs_fhtovp	= btrfs_fhtovp,
	.vfs_vptofh	= btrfs_vptofh,
	.vfs_init	= (void *)nullop,
	.vfs_sysctl	= (void *)eopnotsupp,
	.vfs_checkexp	= (void *)eopnotsupp,
};

static int
btrfs_mount(struct mount *mp, const char *path, void *data,
    struct nameidata *ndp, struct proc *p)
{
	struct btrfs_args *args = data;
	struct btrfs_fs *bmp;
	struct vnode *devvp;
	char fspec[MNAMELEN];
	int error;

	if (mp->mnt_flag & MNT_UPDATE)
		return (EOPNOTSUPP);
	if (args == NULL || args->fspec == NULL)
		return (EINVAL);

	error = copyinstr(args->fspec, fspec, sizeof(fspec), NULL);
	if (error != 0)
		return (error);

	NDINIT(ndp, LOOKUP, FOLLOW, UIO_SYSSPACE, fspec, p);
	error = namei(ndp);
	if (error != 0)
		return (error);
	devvp = ndp->ni_vp;

	rw_enter_write(&btrfs_mount_lock);
	if (devvp->v_type != VBLK)
		error = ENOTBLK;
	else if (major(devvp->v_rdev) >= nblkdev)
		error = ENXIO;
	if (error != 0)
		goto out;
	LIST_FOREACH(bmp, &btrfs_filesystems, bm_entry) {
		if (bmp->bm_dev == devvp->v_rdev)
			break;
	}
	if (bmp != NULL) {
		error = btrfs_attach_view(bmp, mp, args->subvolid);
		/* The filesystem retains its original device reference. */
		vrele(devvp);
		devvp = NULL;
	} else {
		error = vfs_mountedon(devvp);
		if (error == 0 && vcount(devvp) > 1 && devvp != rootvp)
			error = EBUSY;
		if (error == 0) {
			vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
			error = vinvalbuf(devvp, V_SAVE, p->p_ucred, p,
			    0, INFSLP);
			VOP_UNLOCK(devvp);
		}
		if (error == 0)
			error = btrfs_mountfs(devvp, mp, args->subvolid, p);
	}
out:
	rw_exit_write(&btrfs_mount_lock);
	if (error != 0) {
		if (devvp != NULL)
			vrele(devvp);
		return (error);
	}

	memset(mp->mnt_stat.f_mntonname, 0, MNAMELEN);
	strlcpy(mp->mnt_stat.f_mntonname, path, MNAMELEN);
	memset(mp->mnt_stat.f_mntfromname, 0, MNAMELEN);
	strlcpy(mp->mnt_stat.f_mntfromname, fspec, MNAMELEN);
	memset(mp->mnt_stat.f_mntfromspec, 0, MNAMELEN);
	strlcpy(mp->mnt_stat.f_mntfromspec, fspec, MNAMELEN);
	return (0);
}

static int
btrfs_mountfs(struct vnode *devvp, struct mount *mp, uint64_t treeid,
    struct proc *p)
{
	const struct btrfs_super_block *anchor, *sb;
	struct btrfs_bootstrap bootstrap = { 0 };
	struct btrfs_fs *bmp = NULL;
	struct btrfs_super_candidate *candidates = NULL;
	struct btrfs_super_mirror mirrors[BTRFS_SUPER_MIRROR_MAX];
	uint64_t best_generation, selected_generation;
	unsigned int anchor_index, best, i, selected;
	const char *stage = "opening device";
	int error, last_error = EINVAL, mounted = 0, open_flags;
	int readonly = (mp->mnt_flag & MNT_RDONLY) != 0;

	open_flags = FREAD | (readonly ? 0 : FWRITE);
	error = VOP_OPEN(devvp, open_flags, FSCRED, p);
	if (error != 0)
		return (error);

	candidates = mallocarray(BTRFS_SUPER_MIRROR_MAX, sizeof(*candidates),
	    M_BTRFS, M_WAITOK | M_ZERO);
	stage = "reading superblocks";
	error = btrfs_read_super_mirrors(devvp, p, candidates, mirrors);
	if (error != 0)
		goto out;

	for (anchor_index = 0; anchor_index < BTRFS_SUPER_MIRROR_MAX;
	    anchor_index++) {
		if (mirrors[anchor_index].bsm_flags &
		    BTRFS_SUPER_MIRROR_VALID)
			break;
	}
	KASSERT(anchor_index < BTRFS_SUPER_MIRROR_MAX);
	anchor = &candidates[anchor_index].bsc_super;

	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		if ((mirrors[i].bsm_flags & BTRFS_SUPER_MIRROR_VALID) != 0 &&
		    !btrfs_super_same_filesystem(anchor,
		    &candidates[i].bsc_super))
			mirrors[i].bsm_flags |= BTRFS_SUPER_MIRROR_FOREIGN;
	}

	selected = BTRFS_SUPER_MIRROR_MAX;
	for (;;) {
		best = BTRFS_SUPER_MIRROR_MAX;
		best_generation = 0;
		for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
			if ((mirrors[i].bsm_flags &
			    (BTRFS_SUPER_MIRROR_VALID |
			    BTRFS_SUPER_MIRROR_FOREIGN)) !=
			    BTRFS_SUPER_MIRROR_VALID ||
			    candidates[i].bsc_tried)
				continue;
			if (best == BTRFS_SUPER_MIRROR_MAX ||
			    mirrors[i].bsm_generation > best_generation) {
				best = i;
				best_generation = mirrors[i].bsm_generation;
			}
		}
		if (best == BTRFS_SUPER_MIRROR_MAX) {
			error = last_error;
			goto out;
		}

		candidates[best].bsc_tried = 1;
		sb = &candidates[best].bsc_super;
		stage = "checking superblock features";
		error = btrfs_check_super_policy(sb, readonly);
		if (error != 0) {
			mirrors[best].bsm_error = error;
			goto out;
		}
		stage = "loading filesystem trees";
		error = btrfs_bootstrap_super(devvp, sb, &bootstrap);
		if (error == 0) {
			selected = best;
			break;
		}
		mirrors[best].bsm_error = error;
		last_error = error;
		if (error == EOPNOTSUPP || error == ENOMEM)
			goto out;
	}
	sb = &candidates[selected].bsc_super;
	selected_generation = mirrors[selected].bsm_generation;
	mirrors[selected].bsm_flags |= BTRFS_SUPER_MIRROR_CONSISTENT |
	    BTRFS_SUPER_MIRROR_SELECTED;
	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		if ((mirrors[i].bsm_flags &
		    (BTRFS_SUPER_MIRROR_VALID | BTRFS_SUPER_MIRROR_FOREIGN)) ==
		    BTRFS_SUPER_MIRROR_VALID &&
		    mirrors[i].bsm_generation < selected_generation)
			mirrors[i].bsm_flags |= BTRFS_SUPER_MIRROR_STALE;
	}
	if (!readonly) {
		stage = "validating writable superblock generation";
		for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
			if ((mirrors[i].bsm_flags &
			    (BTRFS_SUPER_MIRROR_VALID |
			    BTRFS_SUPER_MIRROR_FOREIGN)) ==
			    BTRFS_SUPER_MIRROR_VALID &&
			    mirrors[i].bsm_generation > selected_generation) {
				error = EROFS;
				goto out;
			}
		}
	}

	bmp = malloc(sizeof(*bmp), M_BTRFS, M_WAITOK | M_ZERO);
	LIST_INIT(&bmp->bm_mounts);
	bmp->bm_readonly = readonly;
	bmp->bm_devvp = devvp;
	bmp->bm_dev = devvp->v_rdev;
	bmp->bm_open_flags = open_flags;
	memcpy(&bmp->bm_super, sb, sizeof(bmp->bm_super));
	memcpy(bmp->bm_super_mirrors, mirrors, sizeof(mirrors));
	bmp->bm_selected_super = selected;
	/*
	 * Backup roots are retained for diagnostics only.  Recovery must
	 * roll the tree and chunk roots back as one coordinated operation.
	 */
	bmp->bm_backup_roots_valid = btrfs_validate_backup_roots(sb);
	bmp->bm_seeding =
	    (letoh64(sb->flags) & BTRFS_SUPER_FLAG_SEEDING) != 0;
	rw_init(&bmp->bm_mapping_lock, "btrmap");
	rw_init(&bmp->bm_chunk_alloc_lock, "btrchunk");
	bmp->bm_chunks = bootstrap.bb_chunks;
	bmp->bm_nchunks = bootstrap.bb_nchunks;
	bootstrap.bb_chunks = NULL;
	memcpy(bmp->bm_chunk_tree_uuid, bootstrap.bb_chunk_tree_uuid,
	    sizeof(bmp->bm_chunk_tree_uuid));
	btrfs_init_roots(bmp, &bootstrap);
	LIST_INIT(&bmp->bm_extent_buffers);
	mtx_init(&bmp->bm_ebmtx, IPL_NONE);
	LIST_INIT(&bmp->bm_nodes);
	mtx_init(&bmp->bm_nodemtx, IPL_NONE);
	rw_init(&bmp->bm_namespace_lock, "btrfsns");
	rw_init(&bmp->bm_rename_lock, "btrfsrename");
	stage = "building free-space index";
	error = btrfs_space_init(bmp);
	if (error != 0)
		goto out;
	if (!readonly) {
		stage = "validating writable image";
		error = btrfs_validate_writable(bmp);
		if (error != 0)
			goto out;
	}
	stage = "initializing transaction";
	error = btrfs_trans_init(bmp);
	if (error != 0)
		goto out;
	if (!readonly) {
		stage = "recovering orphaned inodes";
		error = btrfs_check_write_orphans(bmp, 1);
		if (error != 0)
			goto out;
	}

	stage = "selecting subvolume";
	error = btrfs_attach_view(bmp, mp, treeid);
	if (error != 0)
		goto out;
	LIST_INSERT_HEAD(&btrfs_filesystems, bmp, bm_entry);
	devvp->v_specmountpoint = mp;
	mounted = 1;
	error = 0;
out:
	if (bootstrap.bb_chunks != NULL)
		free(bootstrap.bb_chunks, M_BTRFS,
		    bootstrap.bb_nchunks * sizeof(*bootstrap.bb_chunks));
	if (candidates != NULL)
		free(candidates, M_BTRFS,
		    BTRFS_SUPER_MIRROR_MAX * sizeof(*candidates));
	if (!mounted) {
		printf("btrfs: mount failed while %s: error %d\n", stage,
		    error);
		if (bmp != NULL) {
			btrfs_trans_destroy(bmp);
			btrfs_space_destroy(bmp);
			KASSERT(LIST_EMPTY(&bmp->bm_extent_buffers));
			btrfs_free_roots(bmp);
			free(bmp->bm_chunks, M_BTRFS,
			    bmp->bm_nchunks * sizeof(*bmp->bm_chunks));
			free(bmp, M_BTRFS, sizeof(*bmp));
		}
		vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
		(void)VOP_CLOSE(devvp, open_flags, FSCRED, p);
		VOP_UNLOCK(devvp);
	}
	return (error);
}

/*
 * Inodes retain their on-disk numbers, which are unique only within a tree.
 * Use the anonymous device major reserved for this VFS type, as
 * vfs_getnewfsid() does, but allocate for every visited filesystem tree.
 * Never reuse minors during this boot, including after the last view closes.
 */
static int
btrfs_root_device(struct mount *mp, struct btrfs_root *root)
{
	int error = 0;

	mtx_enter(&btrfs_device_mtx);
	if (root->br_dev == NODEV) {
		if (btrfs_device_minor == 0xffffff)
			error = ENOSPC;
		else {
			btrfs_device_minor++;
			root->br_dev = makedev(nblkdev +
			    mp->mnt_vfc->vfc_typenum, btrfs_device_minor);
		}
	}
	mtx_leave(&btrfs_device_mtx);
	return (error);
}

/*
 * Follow immutable ROOT_BACKREF ancestry to the top-level tree. Check the
 * whole chain even after finding ancestor, and use Floyd's cycle detection
 * so corrupt images cannot loop indefinitely or evade overlap checks.
 */
static int
btrfs_ancestor(struct btrfs_fs *bmp, uint64_t treeid, uint64_t ancestor,
    int *found)
{
	uint64_t slow = treeid, fast = treeid, dirid;
	int error, i;

	*found = treeid == ancestor;
	while (slow != BTRFS_FS_TREE_OBJECTID) {
		error = btrfs_find_subvol_parent(bmp, slow, &slow, &dirid);
		if (error != 0)
			return (error);
		if (slow == ancestor)
			*found = 1;
		for (i = 0; i < 2 && fast != BTRFS_FS_TREE_OBJECTID; i++) {
			error = btrfs_find_subvol_parent(bmp, fast, &fast,
			    &dirid);
			if (error != 0)
				return (error);
		}
		if (slow == fast && slow != BTRFS_FS_TREE_OBJECTID)
			return (EINVAL);
	}
	return (0);
}

static int
btrfs_attach_view(struct btrfs_fs *bmp, struct mount *mp, uint64_t treeid)
{
	struct btrfs_mount *view, *other;
	struct btrfs_root *root;
	struct btrfs_inode inode;
	int error, found, readonly = (mp->mnt_flag & MNT_RDONLY) != 0;

	rw_assert_wrlock(&btrfs_mount_lock);
	if (treeid == 0)
		treeid = BTRFS_FS_TREE_OBJECTID;
	if (treeid != BTRFS_FS_TREE_OBJECTID &&
	    (treeid < BTRFS_FIRST_FREE_OBJECTID ||
	    treeid > BTRFS_LAST_FREE_OBJECTID))
		return (EINVAL);
	error = btrfs_get_root(bmp, treeid, &root);
	if (error != 0)
		return (error);
	if (!readonly && (root->br_flags & BTRFS_ROOT_SUBVOL_RDONLY))
		return (EROFS);
	error = btrfs_find_inode(root, BTRFS_FIRST_FREE_OBJECTID, &inode);
	if (error != 0)
		return (error);
	if (IFTOVT(inode.bi_mode) != VDIR)
		return (EINVAL);
	error = btrfs_ancestor(bmp, treeid, BTRFS_FS_TREE_OBJECTID, &found);
	if (error != 0)
		return (error);
	LIST_FOREACH(other, &bmp->bm_mounts, bmv_entry) {
		error = btrfs_ancestor(bmp, treeid, other->bmv_treeid,
		    &found);
		if (error != 0 || found)
			return (error != 0 ? error : EBUSY);
		error = btrfs_ancestor(bmp, other->bmv_treeid, treeid,
		    &found);
		if (error != 0 || found)
			return (error != 0 ? error : EBUSY);
	}
	error = btrfs_root_device(mp, root);
	if (error != 0)
		return (error);
	view = malloc(sizeof(*view), M_BTRFS, M_WAITOK | M_ZERO);
	view->bmv_mount = mp;
	view->bmv_fs = bmp;
	view->bmv_treeid = treeid;
	view->bmv_root_dirid = BTRFS_FIRST_FREE_OBJECTID;
	mtx_enter(&bmp->bm_trans_mtx);
	/* Also serialize attachment with transaction failure. */
	if (!readonly && bmp->bm_readonly) {
		mtx_leave(&bmp->bm_trans_mtx);
		free(view, M_BTRFS, sizeof(*view));
		return (EROFS);
	}
	LIST_INSERT_HEAD(&bmp->bm_mounts, view, bmv_entry);
	mp->mnt_data = view;
	mtx_leave(&bmp->bm_trans_mtx);
	mp->mnt_stat.f_fsid.val[0] = root->br_dev;
	mp->mnt_stat.f_fsid.val[1] = mp->mnt_vfc->vfc_typenum;
	mp->mnt_stat.f_namemax = BTRFS_NAME_MAX;
	mp->mnt_flag |= MNT_LOCAL;
	return (0);
}

static int
btrfs_write_extent_valid(const struct btrfs_extent_record *extent, void *arg)
{
	struct btrfs_fs *bmp = arg;
	struct btrfs_root *root;
	struct btrfs_extent_buffer *eb;
	int error;

	if (extent->ber_legacy) {
		printf("btrfs: legacy extents are not writable\n");
		return (EOPNOTSUPP);
	}
	if (extent->ber_has_owner) {
		printf("btrfs: simple-quota owner refs are not writable\n");
		return (EOPNOTSUPP);
	}
	/* Only file trees implement the full-reference COW transition. */
	if (extent->ber_flags & BTRFS_BLOCK_FLAG_FULL_BACKREF) {
		error = btrfs_get_root(bmp, BTRFS_FS_TREE_OBJECTID, &root);
		if (error != 0)
			return (error);
		error = btrfs_extent_buffer_read(root, extent->ber_bytenr,
		    extent->ber_generation, letoh64(bmp->bm_super.generation),
		    extent->ber_level, &eb);
		if (error != 0)
			return (error);
		btrfs_extent_buffer_put(eb);
	}
	return (0);
}

static int
btrfs_check_root_orphans(struct btrfs_root *root, int recover)
{
	struct btrfs_path path = { 0 };
	struct btrfs_key target = { 0 };
	const struct btrfs_key *key;
	uint64_t ino;
	uint32_t size;
	int error;

	target.objectid = htole64(BTRFS_ORPHAN_OBJECTID);
	target.type = BTRFS_ORPHAN_ITEM_KEY;
	for (;;) {
		error = btrfs_search_lower_bound(root, &target, &path);
		if (error != 0)
			break;
		error = btrfs_path_item(&path, &key, NULL, &size);
		if (error != 0)
			break;
		if (key->objectid != target.objectid || key->type != target.type)
			break;
		ino = letoh64(key->offset);
		btrfs_release_path(&path);
		if (root->br_owner == BTRFS_ROOT_TREE_OBJECTID)
			return (EOPNOTSUPP);
		if (size != 0 || ino <= BTRFS_FIRST_FREE_OBJECTID ||
		    ino > BTRFS_LAST_FREE_OBJECTID)
			return (EINVAL);
		error = recover ? btrfs_reap_inode(root, ino) :
		    btrfs_check_orphan(root, ino);
		if (error != 0)
			return (error);
		target.offset = htole64(ino + 1);
	}
	btrfs_release_path(&path);
	return (error == ENOENT ? 0 : error);
}

static int
btrfs_check_write_orphans(struct btrfs_fs *bmp, int recover)
{
	struct btrfs_root *root_tree, *root;
	struct btrfs_path path = { 0 };
	struct btrfs_key target = { 0 };
	const struct btrfs_key *key;
	uint64_t owner;
	int error;

	error = btrfs_get_root(bmp, BTRFS_ROOT_TREE_OBJECTID, &root_tree);
	if (error != 0)
		return (error);
	/* Root-tree orphans describe unfinished subvolume deletion. */
	error = btrfs_check_root_orphans(root_tree, recover);
	if (error != 0)
		return (error);
	target.objectid = htole64(BTRFS_FS_TREE_OBJECTID);
	for (;;) {
		error = btrfs_search_lower_bound(root_tree, &target, &path);
		while (error == 0) {
			error = btrfs_path_item(&path, &key, NULL, NULL);
			if (error != 0)
				break;
			owner = letoh64(key->objectid);
			if (owner > BTRFS_LAST_FREE_OBJECTID) {
				error = ENOENT;
				break;
			}
			if (key->type == BTRFS_ROOT_ITEM_KEY &&
			    (owner == BTRFS_FS_TREE_OBJECTID ||
			    owner >= BTRFS_FIRST_FREE_OBJECTID))
				break;
			error = btrfs_next_item(&path);
		}
		btrfs_release_path(&path);
		if (error != 0)
			return (error == ENOENT ? 0 : error);
		/*
		 * Inspect every file tree, including trees outside the initial
		 * view: a later view may attach to this writable filesystem.
		 * Drop the root-tree path before loading another root.
		 */
		error = btrfs_get_root(bmp, owner, &root);
		if (error == 0)
			error = btrfs_check_root_orphans(root, recover);
		if (error != 0)
			return (error);
		target.objectid = htole64(owner + 1);
	}
}

static int
btrfs_validate_writable(struct btrfs_fs *bmp)
{
	unsigned int i;
	uint64_t profile;
	int error;

	for (i = 0; i < bmp->bm_nchunks; i++) {
		profile = bmp->bm_chunks[i].type &
		    (BTRFS_BLOCK_GROUP_RAID0 | BTRFS_BLOCK_GROUP_RAID1 |
		    BTRFS_BLOCK_GROUP_DUP | BTRFS_BLOCK_GROUP_RAID10 |
		    BTRFS_BLOCK_GROUP_RAID5 | BTRFS_BLOCK_GROUP_RAID6 |
		    BTRFS_BLOCK_GROUP_RAID1C3 | BTRFS_BLOCK_GROUP_RAID1C4);
		if (profile != 0 && profile != BTRFS_BLOCK_GROUP_DUP)
			return (EOPNOTSUPP);
	}
	error = btrfs_iterate_extent_items(bmp,
	    btrfs_write_extent_valid, NULL, bmp);
	if (error != 0)
		return (error);
	error = btrfs_iterate_device_extents(bmp, NULL, NULL);
	if (error != 0)
		return (error);
	return (btrfs_check_write_orphans(bmp, 0));
}

static int
btrfs_start(struct mount *mp, int flags, struct proc *p)
{
	return (0);
}

int
btrfs_commit_current(struct btrfs_fs *bmp, struct proc *p)
{
	struct btrfs_transaction *trans;
	uint64_t generation;
	int error = 0;

	if ((bmp->bm_open_flags & FWRITE) == 0)
		return (0);
	mtx_enter(&bmp->bm_trans_mtx);
	trans = bmp->bm_transaction;
	if (trans->bt_state == BTRFS_TRANS_ABORTED)
		error = trans->bt_error != 0 ? trans->bt_error : EIO;
	generation = trans->bt_generation;
	mtx_leave(&bmp->bm_trans_mtx);
	if (error != 0)
		return (error);
	return (btrfs_trans_commit(bmp, generation, p));
}

static int
btrfs_unmount(struct mount *mp, int mntflags, struct proc *p)
{
	struct btrfs_fs *bmp = VFSTOBTRFS(mp);
	struct btrfs_mount *view = VFSTOBTRFSVIEW(mp);
	struct btrfs_transaction *trans;
	struct vnode *devvp = bmp->bm_devvp;
	int aborted = 0, error, flags = 0;

	rw_enter_write(&btrfs_mount_lock);
	if (mntflags & MNT_FORCE)
		flags |= FORCECLOSE;
	if (bmp->bm_open_flags & FWRITE) {
		mtx_enter(&bmp->bm_trans_mtx);
		trans = bmp->bm_transaction;
		aborted = trans->bt_state == BTRFS_TRANS_ABORTED;
		mtx_leave(&bmp->bm_trans_mtx);
		if (!aborted) {
			error = btrfs_commit_current(bmp, p);
			if (error != 0 && (mntflags & MNT_FORCE) == 0)
				goto out;
		}
	}
	error = vflush(mp, NULL, flags);
	if (error != 0)
		goto out;
	mtx_enter(&bmp->bm_trans_mtx);
	LIST_REMOVE(view, bmv_entry);
	if (!LIST_EMPTY(&bmp->bm_mounts)) {
		devvp->v_specmountpoint =
		    LIST_FIRST(&bmp->bm_mounts)->bmv_mount;
		mtx_leave(&bmp->bm_trans_mtx);
		goto detached;
	}
	mtx_leave(&bmp->bm_trans_mtx);
	LIST_REMOVE(bmp, bm_entry);
	KASSERT(LIST_EMPTY(&bmp->bm_nodes));
	btrfs_trans_destroy(bmp);
	KASSERT(LIST_EMPTY(&bmp->bm_extent_buffers));
	btrfs_space_destroy(bmp);
	btrfs_free_roots(bmp);

	devvp->v_specmountpoint = NULL;
	vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
	(void)vinvalbuf(devvp, V_SAVE, NOCRED, p, 0, INFSLP);
	(void)VOP_CLOSE(devvp, bmp->bm_open_flags, NOCRED, p);
	VOP_UNLOCK(devvp);
	vrele(devvp);

	free(bmp->bm_chunks, M_BTRFS,
	    bmp->bm_nchunks * sizeof(*bmp->bm_chunks));
	free(bmp, M_BTRFS, sizeof(*bmp));
detached:
	free(view, M_BTRFS, sizeof(*view));
	mp->mnt_data = NULL;
	mp->mnt_flag &= ~MNT_LOCAL;
	error = 0;
out:
	rw_exit_write(&btrfs_mount_lock);
	return (error);
}

static int
btrfs_root(struct mount *mp, struct vnode **vpp)
{
	struct btrfs_mount *view = VFSTOBTRFSVIEW(mp);
	int error;

	error = btrfs_vget(mp, view->bmv_root_dirid, vpp);
	if (error == 0)
		(*vpp)->v_flag |= VROOT;
	return (error);
}

static int
btrfs_statfs(struct mount *mp, struct statfs *sbp, struct proc *p)
{
	struct btrfs_fs *bmp = VFSTOBTRFS(mp);
	int error;

	error = btrfs_space_statfs(bmp, sbp);
	if (error != 0)
		return (error);

	sbp->f_bsize = letoh32(bmp->bm_super.sectorsize);
	sbp->f_iosize = letoh32(bmp->bm_super.nodesize);
	sbp->f_files = 0;
	sbp->f_ffree = 0;
	sbp->f_favail = 0;
	copy_statfs_info(sbp, mp);
	return (0);
}

static int
btrfs_sync(struct mount *mp, int waitfor, int stall, struct ucred *cred,
    struct proc *p)
{
	return (btrfs_commit_current(VFSTOBTRFS(mp), p));
}

static int
btrfs_fhtovp(struct mount *mp, struct fid *fhp, struct vnode **vpp)
{
	struct btrfs_mount *view = VFSTOBTRFSVIEW(mp);
	struct btrfs_fid *fid = (struct btrfs_fid *)fhp;
	struct btrfs_node *node;
	struct btrfs_root *root;
	int error, found;

	*vpp = NULL;
	if (fid->bfid_len != sizeof(*fid) || fid->bfid_version != 1)
		return (EINVAL);
	if (fid->bfid_treeid != BTRFS_FS_TREE_OBJECTID &&
	    fid->bfid_treeid < BTRFS_FIRST_FREE_OBJECTID)
		return (ESTALE);
	error = btrfs_get_root(view->bmv_fs, fid->bfid_treeid, &root);
	if (error != 0)
		return (error == ENOENT ? ESTALE : error);
	/*
	 * Check scope before vget: disjoint views own separate vnode
	 * identities, and a handle must not escape its selected hierarchy.
	 */
	error = btrfs_ancestor(view->bmv_fs, fid->bfid_treeid,
	    view->bmv_treeid, &found);
	if (error != 0)
		return (error == ENOENT ? ESTALE : error);
	if (!found)
		return (ESTALE);
	error = btrfs_vget_tree(mp, fid->bfid_treeid, fid->bfid_ino, vpp);
	if (error != 0)
		return (error == ENOENT ? ESTALE : error);
	node = VTOBTRFS(*vpp);
	if (node->bn_inode.bi_generation != fid->bfid_generation ||
	    node->bn_inode.bi_nlink == 0) {
		vput(*vpp);
		*vpp = NULL;
		return (ESTALE);
	}
	return (0);
}

static int
btrfs_vptofh(struct vnode *vp, struct fid *fhp)
{
	struct btrfs_node *node = VTOBTRFS(vp);
	struct btrfs_fid *fid = (struct btrfs_fid *)fhp;

	KASSERT(VOP_ISLOCKED(vp));
	if (node->bn_stub_parent != 0)
		return (EOPNOTSUPP);
	if (node->bn_inode.bi_nlink == 0)
		return (ESTALE);
	if (node->bn_treeid > UINT32_MAX ||
	    node->bn_inode.bi_generation > UINT32_MAX)
		return (EOVERFLOW);
	memset(fhp, 0, sizeof(*fhp));
	fid->bfid_len = sizeof(*fid);
	fid->bfid_version = 1;
	fid->bfid_treeid = node->bn_treeid;
	fid->bfid_ino = node->bn_ino;
	fid->bfid_generation = node->bn_inode.bi_generation;
	return (0);
}

static int
btrfs_node_lookup(struct btrfs_fs *bmp, uint64_t treeid, uint64_t ino,
    struct vnode **vpp)
{
	struct btrfs_node *node;
	struct vnode *vp;
	u_int vpid;
	int error;

	*vpp = NULL;
again:
	mtx_enter(&bmp->bm_nodemtx);
	LIST_FOREACH(node, &bmp->bm_nodes, bn_entry) {
		if (node->bn_treeid == treeid && node->bn_ino == ino)
			break;
	}
	if (node == NULL) {
		mtx_leave(&bmp->bm_nodemtx);
		return (0);
	}
	if (node->bn_root->br_deleted || node->bn_root->br_finalizing) {
		mtx_leave(&bmp->bm_nodemtx);
		return (ENOENT);
	}
	vp = node->bn_vnode;
	vpid = vp->v_id;
	mtx_leave(&bmp->bm_nodemtx);

	error = vget(vp, LK_EXCLUSIVE);
	if (error == ENOENT)
		goto again;
	if (error != 0)
		return (error);
	if (vpid != vp->v_id) {
		vput(vp);
		goto again;
	}
	if (VTOBTRFS(vp)->bn_root->br_deleted ||
	    VTOBTRFS(vp)->bn_root->br_finalizing) {
		vput(vp);
		return (ENOENT);
	}
	*vpp = vp;
	return (0);
}

static int
btrfs_node_insert(struct btrfs_node *node)
{
	struct btrfs_fs *bmp = node->bn_mount;
	struct btrfs_node *other;

	KASSERT(VOP_ISLOCKED(node->bn_vnode));
	mtx_enter(&bmp->bm_nodemtx);
	LIST_FOREACH(other, &bmp->bm_nodes, bn_entry) {
		if (other->bn_treeid == node->bn_treeid &&
		    other->bn_ino == node->bn_ino &&
		    other->bn_stub_parent == node->bn_stub_parent &&
		    other->bn_stub_id == node->bn_stub_id) {
			mtx_leave(&bmp->bm_nodemtx);
			return (EEXIST);
		}
	}
	if (node->bn_root->br_deleted || node->bn_root->br_finalizing) {
		mtx_leave(&bmp->bm_nodemtx);
		return (ENOENT);
	}
	LIST_INSERT_HEAD(&bmp->bm_nodes, node, bn_entry);
	node->bn_hashed = 1;
	mtx_leave(&bmp->bm_nodemtx);
	return (0);
}

int
btrfs_vget(struct mount *mp, ino_t ino, struct vnode **vpp)
{
	struct btrfs_mount *view = VFSTOBTRFSVIEW(mp);

	return (btrfs_vget_tree(mp, view->bmv_treeid, ino, vpp));
}

int
btrfs_vget_stub(struct btrfs_node *dir, uint64_t id, struct vnode **vpp)
{
	struct btrfs_fs *bmp = dir->bn_mount;
	struct btrfs_node *node;
	struct btrfs_inode inode = { 0 };
	struct vnode *vp;
	u_int vpid;
	int error;

again:
	mtx_enter(&bmp->bm_nodemtx);
	LIST_FOREACH(node, &bmp->bm_nodes, bn_entry)
		if (node->bn_treeid == dir->bn_treeid &&
		    node->bn_stub_parent == dir->bn_ino && node->bn_stub_id == id)
			break;
	if (node != NULL) {
		vp = node->bn_vnode;
		vpid = vp->v_id;
		mtx_leave(&bmp->bm_nodemtx);
		error = vget(vp, LK_EXCLUSIVE);
		if (error == ENOENT)
			goto again;
		if (error != 0)
			return (error);
		if (vp->v_id != vpid) {
			vput(vp);
			goto again;
		}
		*vpp = vp;
		return (0);
	}
	mtx_leave(&bmp->bm_nodemtx);
	inode.bi_mode = S_IFDIR | 0555;
	inode.bi_nlink = 1;
	inode.bi_flags = BTRFS_INODE_READONLY;
	inode.bi_generation = dir->bn_root->br_generation;
	error = btrfs_alloc_node(dir->bn_vnode->v_mount, dir->bn_root, 2,
	    &inode, &vp);
	if (error != 0)
		return (error);
	node = VTOBTRFS(vp);
	node->bn_stub_parent = dir->bn_ino;
	node->bn_stub_id = id;
	error = btrfs_init_node(&vp);
	if (error != 0) {
		vput(vp);
		vgone(vp);
		if (error == EEXIST)
			goto again;
		return (error);
	}
	*vpp = vp;
	return (0);
}

int
btrfs_vget_tree(struct mount *mp, uint64_t treeid, uint64_t ino,
    struct vnode **vpp)
{
	struct btrfs_fs *bmp = VFSTOBTRFS(mp);
	struct btrfs_root *root;
	struct btrfs_inode inode;
	struct vnode *vp;
	int error;

	if (ino < BTRFS_FIRST_FREE_OBJECTID ||
	    ino > BTRFS_LAST_FREE_OBJECTID)
		return (ENOENT);

again:
	error = btrfs_node_lookup(bmp, treeid, ino, vpp);
	if (error != 0 || *vpp != NULL) {
		KASSERT(*vpp == NULL || (*vpp)->v_mount == mp);
		return (error);
	}

	error = btrfs_get_root(bmp, treeid, &root);
	if (error != 0)
		return (error);
	error = btrfs_root_device(mp, root);
	if (error != 0)
		return (error);
	error = btrfs_find_inode(root, ino, &inode);
	if (error != 0)
		return (error);
	error = btrfs_alloc_node(mp, root, ino, &inode, &vp);
	if (error != 0)
		return (error);
	error = btrfs_init_node(&vp);
	if (error != 0) {
		vput(vp);
		vgone(vp);
		if (error == EEXIST)
			goto again;
		return (error);
	}
	*vpp = vp;
	return (0);
}

/*
 * Preallocate a locked, private vnode. Creation does this before joining a
 * transaction so vnode exhaustion cannot leave a new directory entry behind.
 * Alias registration and cache publication belong to btrfs_init_node.
 */
int
btrfs_alloc_node(struct mount *mp, struct btrfs_root *root, uint64_t ino,
    const struct btrfs_inode *inode, struct vnode **vpp)
{
	struct btrfs_fs *bmp = VFSTOBTRFS(mp);
	struct btrfs_mount *view = VFSTOBTRFSVIEW(mp);
	struct btrfs_node *node;
	struct vnode *vp;
	enum vtype type = IFTOVT(inode->bi_mode);
	dev_t rdev;
	int error;

	*vpp = NULL;
	if (type == VCHR || type == VBLK) {
		error = btrfs_decode_rdev(inode->bi_rdev, &rdev);
		if (error != 0)
			return (error);
	}
#ifndef FIFO
	if (type == VFIFO)
		return (EOPNOTSUPP);
#endif

	node = malloc(sizeof(*node), M_BTRFS, M_WAITOK | M_ZERO);
	error = getnewvnode(VT_BTRFS, mp, &btrfs_vops, &vp);
	if (error != 0) {
		free(node, M_BTRFS, sizeof(*node));
		return (error);
	}

	node->bn_vnode = vp;
	node->bn_mount = bmp;
	node->bn_root = root;
	node->bn_treeid = root->br_owner;
	node->bn_ino = ino;
	node->bn_inode = *inode;
	rrw_init_flags(&node->bn_lock, "btrfsnode",
	    RWL_DUPOK | RWL_IS_VNODE);
	vp->v_data = node;
	vp->v_type = type;
#ifdef FIFO
	if (type == VFIFO)
		vp->v_op = &btrfs_fifo_vops;
#endif
	if (node->bn_treeid == view->bmv_treeid && ino == view->bmv_root_dirid)
		vp->v_flag |= VROOT;

	vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
	*vpp = vp;
	return (0);
}

/*
 * checkalias may lock unrelated vnodes, including one whose fsync is draining
 * transaction handles. Never call with a handle. Keep the node private until
 * alias adoption has settled its vnode identity.
 */
int
btrfs_init_node(struct vnode **vpp)
{
	struct vnode *vp = *vpp, *alias;
	struct btrfs_node *node = VTOBTRFS(vp);
	dev_t rdev;
	int error;

	KASSERT(VOP_ISLOCKED(vp));
	KASSERT(!node->bn_hashed);
	if (vp->v_type == VCHR || vp->v_type == VBLK) {
		error = btrfs_decode_rdev(node->bn_inode.bi_rdev, &rdev);
		if (error != 0)
			return (error);
		vp->v_op = &btrfs_spec_vops;
		alias = checkalias(vp, rdev, vp->v_mount);
		if (alias != NULL) {
			/* Carry the locked inode over to an anonymous device. */
			alias->v_data = node;
			vp->v_data = NULL;
			vp->v_op = &spec_vops;
			vrele(vp);
			vgone(vp);
			vp = alias;
			node->bn_vnode = vp;
		}
	}

	*vpp = vp;
	return (btrfs_node_insert(node));
}

void
btrfs_fs_set_readonly(struct btrfs_fs *bmp)
{
	struct btrfs_mount *view;

	MUTEX_ASSERT_LOCKED(&bmp->bm_trans_mtx);
	bmp->bm_readonly = 1;
	LIST_FOREACH(view, &bmp->bm_mounts, bmv_entry)
		view->bmv_mount->mnt_flag |= MNT_RDONLY;
}
