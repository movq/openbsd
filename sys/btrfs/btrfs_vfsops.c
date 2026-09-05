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

#include <btrfs/btrfs_var.h>

static int	btrfs_mount(struct mount *, const char *, void *,
		    struct nameidata *, struct proc *);
static int	btrfs_mountfs(struct vnode *, struct mount *, struct proc *);
static int	btrfs_write_extent_valid(
		    const struct btrfs_extent_record *, void *);
static int	btrfs_write_backref_valid(
		    const struct btrfs_backref_record *, void *);
static int	btrfs_validate_writable(struct btrfs_mount *);
static int	btrfs_commit_current(struct btrfs_mount *, struct proc *);
static int	btrfs_start(struct mount *, int, struct proc *);
static int	btrfs_unmount(struct mount *, int, struct proc *);
static int	btrfs_root(struct mount *, struct vnode **);
static int	btrfs_statfs(struct mount *, struct statfs *, struct proc *);
static int	btrfs_sync(struct mount *, int, int, struct ucred *,
		    struct proc *);

const struct vfsops btrfs_vfsops = {
	.vfs_mount	= btrfs_mount,
	.vfs_start	= btrfs_start,
	.vfs_unmount	= btrfs_unmount,
	.vfs_root	= btrfs_root,
	.vfs_quotactl	= (void *)eopnotsupp,
	.vfs_statfs	= btrfs_statfs,
	.vfs_sync	= btrfs_sync,
	.vfs_vget	= btrfs_vget,
	.vfs_fhtovp	= (void *)eopnotsupp,
	.vfs_vptofh	= (void *)eopnotsupp,
	.vfs_init	= (void *)nullop,
	.vfs_sysctl	= (void *)eopnotsupp,
	.vfs_checkexp	= (void *)eopnotsupp,
};

static int
btrfs_mount(struct mount *mp, const char *path, void *data,
    struct nameidata *ndp, struct proc *p)
{
	struct btrfs_args *args = data;
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

	if (devvp->v_type != VBLK)
		error = ENOTBLK;
	else if (major(devvp->v_rdev) >= nblkdev)
		error = ENXIO;
	else
		error = vfs_mountedon(devvp);
	if (error == 0 && vcount(devvp) > 1 && devvp != rootvp)
		error = EBUSY;
	if (error == 0) {
		vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
		error = vinvalbuf(devvp, V_SAVE, p->p_ucred, p, 0, INFSLP);
		VOP_UNLOCK(devvp);
	}
	if (error == 0)
		error = btrfs_mountfs(devvp, mp, p);
	if (error != 0) {
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
btrfs_mountfs(struct vnode *devvp, struct mount *mp, struct proc *p)
{
	const struct btrfs_super_block *anchor, *sb;
	struct btrfs_bootstrap bootstrap = { 0 };
	struct btrfs_mount *bmp = NULL;
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
		error = btrfs_bootstrap_super(devvp, sb, readonly, &bootstrap);
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
	bmp->bm_mount = mp;
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
	bmp->bm_subvol_readonly =
	    (bootstrap.bb_fs_root_flags & BTRFS_ROOT_SUBVOL_RDONLY) != 0;
	bmp->bm_chunks = bootstrap.bb_chunks;
	bmp->bm_nchunks = bootstrap.bb_nchunks;
	bmp->bm_treeid = BTRFS_FS_TREE_OBJECTID;
	memcpy(bmp->bm_chunk_tree_uuid, bootstrap.bb_chunk_tree_uuid,
	    sizeof(bmp->bm_chunk_tree_uuid));
	bmp->bm_root_dirid = BTRFS_FIRST_FREE_OBJECTID;
	btrfs_init_roots(bmp, &bootstrap);
	LIST_INIT(&bmp->bm_extent_buffers);
	mtx_init(&bmp->bm_ebmtx, IPL_NONE);
	LIST_INIT(&bmp->bm_nodes);
	mtx_init(&bmp->bm_nodemtx, IPL_NONE);
	rw_init(&bmp->bm_namespace_lock, "btrfsns");
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

	mp->mnt_data = bmp;
	mp->mnt_stat.f_fsid.val[0] = devvp->v_rdev;
	mp->mnt_stat.f_fsid.val[1] = mp->mnt_vfc->vfc_typenum;
	mp->mnt_stat.f_namemax = BTRFS_NAME_MAX;
	mp->mnt_flag |= MNT_LOCAL;
	devvp->v_specmountpoint = mp;
	bootstrap.bb_chunks = NULL;
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
			free(bmp, M_BTRFS, sizeof(*bmp));
		}
		vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
		(void)VOP_CLOSE(devvp, open_flags, FSCRED, p);
		VOP_UNLOCK(devvp);
	}
	return (error);
}

static int
btrfs_write_extent_valid(const struct btrfs_extent_record *extent, void *arg)
{
	if (extent->ber_legacy) {
		printf("btrfs: legacy extents are not writable\n");
		return (EOPNOTSUPP);
	}
	if (extent->ber_has_owner) {
		printf("btrfs: simple-quota owner refs are not writable\n");
		return (EOPNOTSUPP);
	}
	return (0);
}

static int
btrfs_write_backref_valid(const struct btrfs_backref_record *backref,
    void *arg)
{
	(void)arg;

	if (backref->bbr_type == BTRFS_SHARED_BLOCK_REF_KEY ||
	    backref->bbr_type == BTRFS_SHARED_DATA_REF_KEY) {
		printf("btrfs: shared extents are not writable\n");
		return (EOPNOTSUPP);
	}
	return (0);
}

static int
btrfs_validate_writable(struct btrfs_mount *bmp)
{
	unsigned int i;
	uint64_t profile;

	if (bmp->bm_subvol_readonly)
		return (EROFS);
	for (i = 0; i < bmp->bm_nchunks; i++) {
		profile = bmp->bm_chunks[i].type &
		    (BTRFS_BLOCK_GROUP_RAID0 | BTRFS_BLOCK_GROUP_RAID1 |
		    BTRFS_BLOCK_GROUP_DUP | BTRFS_BLOCK_GROUP_RAID10 |
		    BTRFS_BLOCK_GROUP_RAID5 | BTRFS_BLOCK_GROUP_RAID6 |
		    BTRFS_BLOCK_GROUP_RAID1C3 | BTRFS_BLOCK_GROUP_RAID1C4);
		if (profile != 0 && profile != BTRFS_BLOCK_GROUP_DUP)
			return (EOPNOTSUPP);
	}
	return (btrfs_iterate_extent_items(bmp, btrfs_write_extent_valid,
	    btrfs_write_backref_valid, bmp));
}

static int
btrfs_start(struct mount *mp, int flags, struct proc *p)
{
	return (0);
}

static int
btrfs_commit_current(struct btrfs_mount *bmp, struct proc *p)
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
	struct btrfs_mount *bmp = VFSTOBTRFS(mp);
	struct btrfs_transaction *trans;
	struct vnode *devvp = bmp->bm_devvp;
	int aborted = 0, error, flags = 0;

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
				return (error);
		}
	}
	error = vflush(mp, NULL, flags);
	if (error != 0)
		return (error);
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
	mp->mnt_data = NULL;
	mp->mnt_flag &= ~MNT_LOCAL;
	return (0);
}

static int
btrfs_root(struct mount *mp, struct vnode **vpp)
{
	struct btrfs_mount *bmp = VFSTOBTRFS(mp);
	int error;

	error = btrfs_vget(mp, bmp->bm_root_dirid, vpp);
	if (error == 0)
		(*vpp)->v_flag |= VROOT;
	return (error);
}

static int
btrfs_statfs(struct mount *mp, struct statfs *sbp, struct proc *p)
{
	struct btrfs_mount *bmp = VFSTOBTRFS(mp);
	uint64_t bytes_used, sectorsize, total_bytes;

	sectorsize = letoh32(bmp->bm_super.sectorsize);
	total_bytes = letoh64(bmp->bm_super.total_bytes);
	bytes_used = letoh64(bmp->bm_super.bytes_used);

	sbp->f_bsize = sectorsize;
	sbp->f_iosize = letoh32(bmp->bm_super.nodesize);
	sbp->f_blocks = total_bytes / sectorsize;
	sbp->f_bfree = (total_bytes - bytes_used) / sectorsize;
	sbp->f_bavail = sbp->f_bfree;
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
btrfs_node_lookup(struct btrfs_mount *bmp, uint64_t treeid, uint64_t ino,
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
	*vpp = vp;
	return (0);
}

static int
btrfs_node_insert(struct btrfs_node *node)
{
	struct btrfs_mount *bmp = node->bn_mount;
	struct btrfs_node *other;

	vn_lock(node->bn_vnode, LK_EXCLUSIVE | LK_RETRY);
	mtx_enter(&bmp->bm_nodemtx);
	LIST_FOREACH(other, &bmp->bm_nodes, bn_entry) {
		if (other->bn_treeid == node->bn_treeid &&
		    other->bn_ino == node->bn_ino) {
			mtx_leave(&bmp->bm_nodemtx);
			VOP_UNLOCK(node->bn_vnode);
			return (EEXIST);
		}
	}
	LIST_INSERT_HEAD(&bmp->bm_nodes, node, bn_entry);
	node->bn_hashed = 1;
	mtx_leave(&bmp->bm_nodemtx);
	return (0);
}

int
btrfs_vget(struct mount *mp, ino_t ino, struct vnode **vpp)
{
	struct btrfs_mount *bmp = VFSTOBTRFS(mp);

	return (btrfs_vget_tree(mp, bmp->bm_treeid, ino, vpp));
}

int
btrfs_vget_tree(struct mount *mp, uint64_t treeid, uint64_t ino,
    struct vnode **vpp)
{
	struct btrfs_mount *bmp = VFSTOBTRFS(mp);
	struct btrfs_root *root;
	struct btrfs_inode inode;
	struct btrfs_node *node;
	struct vnode *vp;
	enum vtype type;
	int error;

	if (ino < BTRFS_FIRST_FREE_OBJECTID ||
	    ino > BTRFS_LAST_FREE_OBJECTID)
		return (ENOENT);

again:
	error = btrfs_node_lookup(bmp, treeid, ino, vpp);
	if (error != 0 || *vpp != NULL)
		return (error);

	error = btrfs_get_root(bmp, treeid, &root);
	if (error != 0)
		return (error);
	error = btrfs_find_inode(root, ino, &inode);
	if (error != 0)
		return (error);
	type = IFTOVT(inode.bi_mode);

	node = malloc(sizeof(*node), M_BTRFS, M_WAITOK | M_ZERO);
	error = getnewvnode(VT_BTRFS, mp, &btrfs_vops, &vp);
	if (error != 0) {
		free(node, M_BTRFS, sizeof(*node));
		return (error);
	}

	node->bn_vnode = vp;
	node->bn_mount = bmp;
	node->bn_treeid = treeid;
	node->bn_ino = ino;
	node->bn_inode = inode;
	rrw_init_flags(&node->bn_lock, "btrfsnode",
	    RWL_DUPOK | RWL_IS_VNODE);
	vp->v_data = node;
	vp->v_type = type;
	if (treeid == bmp->bm_treeid && ino == bmp->bm_root_dirid)
		vp->v_flag |= VROOT;

	error = btrfs_node_insert(node);
	if (error == EEXIST) {
		vrele(vp);
		goto again;
	}
	if (error != 0) {
		vrele(vp);
		return (error);
	}

	*vpp = vp;
	return (0);
}
