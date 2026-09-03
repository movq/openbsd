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

#include <lib/libkern/crc32c.h>

#include <btrfs/btrfs.h>
#include <btrfs/btrfs_var.h>

#define BTRFS_SUPER_SIZE		0x1000
#define BTRFS_MIN_SECTORSIZE		0x1000
#define BTRFS_MAX_SECTORSIZE		0x10000
#define BTRFS_MAX_LEVEL			8

struct btrfs_io_map {
	uint64_t	physical[2];
	unsigned int	nmirrors;
};

struct btrfs_chunk_stats {
	unsigned int	data;
	unsigned int	metadata;
	unsigned int	system;
};

struct btrfs_dir_stats {
	unsigned int	entries;
	unsigned int	regular;
	unsigned int	directories;
	unsigned int	symlinks;
	unsigned int	special;
	unsigned int	subvolumes;
};

#define BTRFS_BLOCK_GROUP_PROFILE_MASK	(BTRFS_BLOCK_GROUP_RAID0 |	\
	    BTRFS_BLOCK_GROUP_RAID1 | BTRFS_BLOCK_GROUP_DUP |		\
	    BTRFS_BLOCK_GROUP_RAID10 | BTRFS_BLOCK_GROUP_RAID5 |	\
	    BTRFS_BLOCK_GROUP_RAID6 | BTRFS_BLOCK_GROUP_RAID1C3 |	\
	    BTRFS_BLOCK_GROUP_RAID1C4)
#define BTRFS_BLOCK_GROUP_TYPE_MASK	(BTRFS_BLOCK_GROUP_DATA |	\
	    BTRFS_BLOCK_GROUP_SYSTEM | BTRFS_BLOCK_GROUP_METADATA)

static int	btrfs_mount(struct mount *, const char *, void *,
		    struct nameidata *, struct proc *);
static int	btrfs_mountfs(struct vnode *, struct mount *, const char *,
		    struct proc *);
static int	btrfs_start(struct mount *, int, struct proc *);
static int	btrfs_unmount(struct mount *, int, struct proc *);
static int	btrfs_root(struct mount *, struct vnode **);
static int	btrfs_statfs(struct mount *, struct statfs *, struct proc *);
static int	btrfs_sync(struct mount *, int, int, struct ucred *,
		    struct proc *);
static int	btrfs_validate_super(const struct btrfs_super_block *,
		    uint64_t);
static int	btrfs_parse_system_chunks(const struct btrfs_super_block *,
		    uint64_t, uint32_t, struct btrfs_io_map *, unsigned int *);
static int	btrfs_decode_chunk(const struct btrfs_super_block *,
		    const struct btrfs_key *, const struct btrfs_chunk *, size_t,
		    struct btrfs_chunk_map *);
static int	btrfs_map_logical(const struct btrfs_chunk_map *, uint64_t,
		    uint32_t, struct btrfs_io_map *);
static int	btrfs_lookup_logical(const struct btrfs_chunk_map *,
		    unsigned int, uint64_t, uint32_t, struct btrfs_io_map *);
static int	btrfs_read_tree_block(struct vnode *,
		    const struct btrfs_super_block *, const struct btrfs_io_map *,
		    uint64_t, uint64_t, uint64_t, uint8_t, struct buf **,
		    unsigned int *);
static int	btrfs_load_chunk_tree(const struct btrfs_super_block *,
		    const struct btrfs_header *, const struct btrfs_io_map *,
		    struct btrfs_chunk_map **, unsigned int *,
		    struct btrfs_chunk_stats *);
static int	btrfs_lookup_leaf_item(const struct btrfs_header *,
		    const struct btrfs_key *, const uint8_t **, uint32_t *);
static int	btrfs_find_root_item(const struct btrfs_super_block *,
		    const struct btrfs_header *, uint64_t,
		    const struct btrfs_root_item **);
static int	btrfs_find_inode_item(const struct btrfs_super_block *,
		    const struct btrfs_header *, uint64_t,
		    const struct btrfs_inode_item **);
static int	btrfs_scan_directory(const struct btrfs_super_block *,
		    const struct btrfs_header *, uint64_t,
		    struct btrfs_dir_stats *);
static int	btrfs_validate_dev_item(const struct btrfs_super_block *,
		    const struct btrfs_key *, const struct btrfs_dev_item *,
		    size_t);
static int	btrfs_validate_tree_block(const struct btrfs_super_block *,
		    const struct btrfs_header *, uint64_t, uint64_t, uint64_t,
		    uint8_t);
static int	btrfs_key_cmp(const struct btrfs_key *,
		    const struct btrfs_key *);
static int	btrfs_ispow2(uint32_t);

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

	if ((mp->mnt_flag & MNT_RDONLY) == 0)
		return (EROFS);
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
		error = btrfs_mountfs(devvp, mp, fspec, p);
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
btrfs_mountfs(struct vnode *devvp, struct mount *mp, const char *fspec,
    struct proc *p)
{
	const struct btrfs_super_block *sb;
	const struct btrfs_header *header;
	const struct btrfs_inode_item *inode_item;
	const struct btrfs_root_item *root_item;
	struct btrfs_mount *bmp = NULL;
	struct btrfs_chunk_map *chunks = NULL;
	struct btrfs_chunk_stats chunk_stats;
	struct btrfs_dir_stats dir_stats;
	struct btrfs_io_map fs_map, map, root_map;
	struct buf *bp = NULL, *fsbp = NULL, *rootbp = NULL, *treebp = NULL;
	uint64_t chunk_root, fs_root, fs_root_generation, generation, root;
	uint32_t gid, mode, nlink, nritems, nodesize, sectorsize, uid;
	unsigned int mirror, nchunks = 0, nsystem_chunks;
	int error, mounted = 0;

	error = VOP_OPEN(devvp, FREAD, FSCRED, p);
	if (error != 0)
		return (error);

	error = bread(devvp, superblock_addrs[0] / DEV_BSIZE,
	    BTRFS_SUPER_SIZE, &bp);
	if (error != 0)
		goto out;

	sb = (const struct btrfs_super_block *)bp->b_data;
	error = btrfs_validate_super(sb, superblock_addrs[0]);
	if (error != 0)
		goto out;

	generation = letoh64(sb->generation);
	nodesize = letoh32(sb->nodesize);
	sectorsize = letoh32(sb->sectorsize);
	printf("btrfs: %s: valid superblock, generation %llu, "
	    "nodesize %u, sectorsize %u\n", fspec,
	    (unsigned long long)generation, nodesize, sectorsize);

	chunk_root = letoh64(sb->chunk_root);
	error = btrfs_parse_system_chunks(sb, chunk_root, nodesize, &map,
	    &nsystem_chunks);
	if (error != 0)
		goto out;
	printf("btrfs: %s: loaded %u system chunk mapping%s\n", fspec,
	    nsystem_chunks, nsystem_chunks == 1 ? "" : "s");

	error = btrfs_read_tree_block(devvp, sb, &map, chunk_root,
	    letoh64(sb->chunk_root_generation), BTRFS_CHUNK_TREE_OBJECTID,
	    sb->chunk_root_level, &treebp, &mirror);
	if (error != 0)
		goto out;

	header = (const struct btrfs_header *)treebp->b_data;
	nritems = letoh32(header->nritems);
	printf("btrfs: %s: chunk root logical %llu, physical %llu, "
	    "level %u, %u items, mirror %u\n", fspec,
	    (unsigned long long)chunk_root,
	    (unsigned long long)map.physical[mirror], header->level, nritems,
	    mirror + 1);

	error = btrfs_load_chunk_tree(sb, header, &map, &chunks, &nchunks,
	    &chunk_stats);
	if (error != 0)
		goto out;
	printf("btrfs: %s: loaded complete chunk map: %u chunks "
	    "(data %u, metadata %u, system %u)\n", fspec, nchunks,
	    chunk_stats.data, chunk_stats.metadata, chunk_stats.system);

	root = letoh64(sb->root);
	error = btrfs_lookup_logical(chunks, nchunks, root, nodesize,
	    &root_map);
	if (error != 0)
		goto out;
	error = btrfs_read_tree_block(devvp, sb, &root_map, root, generation,
	    BTRFS_ROOT_TREE_OBJECTID, sb->root_level, &rootbp, &mirror);
	if (error != 0)
		goto out;

	header = (const struct btrfs_header *)rootbp->b_data;
	nritems = letoh32(header->nritems);
	printf("btrfs: %s: root tree logical %llu, physical %llu, "
	    "level %u, %u items, mirror %u\n", fspec,
	    (unsigned long long)root,
	    (unsigned long long)root_map.physical[mirror], header->level,
	    nritems, mirror + 1);

	error = btrfs_find_root_item(sb, header, BTRFS_FS_TREE_OBJECTID,
	    &root_item);
	if (error != 0)
		goto out;
	fs_root = letoh64(root_item->bytenr);
	fs_root_generation = letoh64(root_item->generation);
	printf("btrfs: %s: filesystem root item logical %llu, "
	    "generation %llu, level %u\n", fspec,
	    (unsigned long long)fs_root,
	    (unsigned long long)fs_root_generation, root_item->level);

	error = btrfs_lookup_logical(chunks, nchunks, fs_root, nodesize,
	    &fs_map);
	if (error != 0)
		goto out;
	error = btrfs_read_tree_block(devvp, sb, &fs_map, fs_root,
	    fs_root_generation, BTRFS_FS_TREE_OBJECTID, root_item->level,
	    &fsbp, &mirror);
	if (error != 0)
		goto out;

	header = (const struct btrfs_header *)fsbp->b_data;
	nritems = letoh32(header->nritems);
	printf("btrfs: %s: filesystem tree root logical %llu, physical %llu, "
	    "level %u, %u items, mirror %u\n", fspec,
	    (unsigned long long)fs_root,
	    (unsigned long long)fs_map.physical[mirror], header->level,
	    nritems, mirror + 1);

	error = btrfs_find_inode_item(sb, header, BTRFS_FIRST_FREE_OBJECTID,
	    &inode_item);
	if (error != 0)
		goto out;
	mode = letoh32(inode_item->mode);
	uid = letoh32(inode_item->uid);
	gid = letoh32(inode_item->gid);
	nlink = letoh32(inode_item->nlink);
	printf("btrfs: %s: root inode %llu, mode %o, uid %u, gid %u, "
	    "size %llu, links %u\n", fspec,
	    (unsigned long long)BTRFS_FIRST_FREE_OBJECTID, mode, uid, gid,
	    (unsigned long long)letoh64(inode_item->size), nlink);

	error = btrfs_scan_directory(sb, header, BTRFS_FIRST_FREE_OBJECTID,
	    &dir_stats);
	if (error != 0)
		goto out;
	printf("btrfs: %s: root directory has %u entries: %u regular, "
	    "%u directories, %u symlinks, %u special, %u subvolumes\n",
	    fspec, dir_stats.entries, dir_stats.regular,
	    dir_stats.directories, dir_stats.symlinks, dir_stats.special,
	    dir_stats.subvolumes);

	bmp = malloc(sizeof(*bmp), M_BTRFS, M_WAITOK | M_ZERO);
	bmp->bm_mount = mp;
	bmp->bm_devvp = devvp;
	bmp->bm_dev = devvp->v_rdev;
	memcpy(&bmp->bm_super, sb, sizeof(bmp->bm_super));
	bmp->bm_chunks = chunks;
	bmp->bm_nchunks = nchunks;
	bmp->bm_fs_root = fs_root;
	bmp->bm_fs_root_generation = fs_root_generation;
	bmp->bm_fs_root_level = root_item->level;
	bmp->bm_root_dirid = BTRFS_FIRST_FREE_OBJECTID;
	memcpy(&bmp->bm_root_inode, inode_item, sizeof(bmp->bm_root_inode));
	LIST_INIT(&bmp->bm_nodes);
	mtx_init(&bmp->bm_nodemtx, IPL_NONE);

	mp->mnt_data = bmp;
	mp->mnt_stat.f_fsid.val[0] = devvp->v_rdev;
	mp->mnt_stat.f_fsid.val[1] = mp->mnt_vfc->vfc_typenum;
	mp->mnt_stat.f_namemax = BTRFS_NAME_MAX;
	mp->mnt_flag |= MNT_LOCAL;
	devvp->v_specmountpoint = mp;
	chunks = NULL;
	mounted = 1;
	error = 0;
out:
	if (chunks != NULL)
		free(chunks, M_BTRFS, nchunks * sizeof(*chunks));
	if (fsbp != NULL)
		brelse(fsbp);
	if (rootbp != NULL)
		brelse(rootbp);
	if (treebp != NULL)
		brelse(treebp);
	if (bp != NULL)
		brelse(bp);
	if (!mounted) {
		if (bmp != NULL)
			free(bmp, M_BTRFS, sizeof(*bmp));
		vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
		(void)VOP_CLOSE(devvp, FREAD, FSCRED, p);
		VOP_UNLOCK(devvp);
	}
	return (error);
}

static int
btrfs_start(struct mount *mp, int flags, struct proc *p)
{
	return (0);
}

static int
btrfs_unmount(struct mount *mp, int mntflags, struct proc *p)
{
	struct btrfs_mount *bmp = VFSTOBTRFS(mp);
	struct vnode *devvp = bmp->bm_devvp;
	int error, flags = 0;

	if (mntflags & MNT_FORCE)
		flags |= FORCECLOSE;
	error = vflush(mp, NULL, flags);
	if (error != 0)
		return (error);
	KASSERT(LIST_EMPTY(&bmp->bm_nodes));

	devvp->v_specmountpoint = NULL;
	vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
	(void)vinvalbuf(devvp, V_SAVE, NOCRED, p, 0, INFSLP);
	(void)VOP_CLOSE(devvp, FREAD, NOCRED, p);
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
	return (0);
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
	struct btrfs_node *node;
	struct vnode *vp;
	int error;

	if (ino != bmp->bm_root_dirid)
		return (EOPNOTSUPP);

again:
	error = btrfs_node_lookup(bmp, BTRFS_FS_TREE_OBJECTID, ino, vpp);
	if (error != 0 || *vpp != NULL)
		return (error);

	node = malloc(sizeof(*node), M_BTRFS, M_WAITOK | M_ZERO);
	error = getnewvnode(VT_BTRFS, mp, &btrfs_vops, &vp);
	if (error != 0) {
		free(node, M_BTRFS, sizeof(*node));
		return (error);
	}

	node->bn_vnode = vp;
	node->bn_mount = bmp;
	node->bn_treeid = BTRFS_FS_TREE_OBJECTID;
	node->bn_ino = ino;
	memcpy(&node->bn_inode, &bmp->bm_root_inode, sizeof(node->bn_inode));
	rrw_init_flags(&node->bn_lock, "btrfsnode",
	    RWL_DUPOK | RWL_IS_VNODE);
	vp->v_data = node;
	vp->v_type = VDIR;
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

static int
btrfs_validate_super(const struct btrfs_super_block *sb, uint64_t bytenr)
{
	uint64_t bytes_used, chunk_root, root, total_bytes;
	uint32_t csum, disk_csum, nodesize, sectorsize;

	if (letoh64(sb->magic) != BTRFS_MAGIC)
		return (EINVAL);
	if (letoh64(sb->bytenr) != bytenr)
		return (EINVAL);
	if (letoh16(sb->csum_type) != BTRFS_CSUM_TYPE_CRC32)
		return (EOPNOTSUPP);

	memcpy(&disk_csum, sb->csum, sizeof(disk_csum));
	disk_csum = letoh32(disk_csum);
	csum = crc32c(0, (const uint8_t *)sb + sizeof(sb->csum),
	    BTRFS_SUPER_SIZE - sizeof(sb->csum));
	if (csum != disk_csum)
		return (EINVAL);

	sectorsize = letoh32(sb->sectorsize);
	nodesize = letoh32(sb->nodesize);
	if (!btrfs_ispow2(sectorsize) ||
	    sectorsize < BTRFS_MIN_SECTORSIZE ||
	    sectorsize > BTRFS_MAX_SECTORSIZE)
		return (EINVAL);
	if (!btrfs_ispow2(nodesize) || nodesize < sectorsize ||
	    nodesize > MAXBSIZE)
		return (EINVAL);
	if (letoh32(sb->sys_chunk_array_size) >
	    BTRFS_SYSTEM_CHUNK_ARRAY_SIZE)
		return (EINVAL);
	if (sb->root_level >= BTRFS_MAX_LEVEL ||
	    sb->chunk_root_level >= BTRFS_MAX_LEVEL ||
	    sb->log_root_level >= BTRFS_MAX_LEVEL)
		return (EINVAL);

	total_bytes = letoh64(sb->total_bytes);
	bytes_used = letoh64(sb->bytes_used);
	if (total_bytes < bytenr + BTRFS_SUPER_SIZE ||
	    bytes_used > total_bytes)
		return (EINVAL);
	if (letoh64(sb->num_devices) != 1)
		return (EOPNOTSUPP);

	root = letoh64(sb->root);
	chunk_root = letoh64(sb->chunk_root);
	if (root == 0 || (root & (sectorsize - 1)) != 0 ||
	    chunk_root == 0 || (chunk_root & (sectorsize - 1)) != 0)
		return (EINVAL);

	if (memcmp(sb->fsid, sb->dev_item.fsid, BTRFS_UUID_SIZE) != 0)
		return (EINVAL);

	return (0);
}

static int
btrfs_parse_system_chunks(const struct btrfs_super_block *sb,
    uint64_t target, uint32_t target_len, struct btrfs_io_map *map,
    unsigned int *nchunksp)
{
	const struct btrfs_key *key;
	const struct btrfs_chunk *chunk;
	const uint8_t *p;
	struct btrfs_chunk_map chunk_map;
	struct btrfs_io_map target_map;
	uint16_t nstripes;
	size_t chunk_base, entry_size, remain;
	uint64_t previous_end = 0;
	unsigned int nchunks = 0;
	int mapped = 0;
	int error;

	memset(map, 0, sizeof(*map));
	p = sb->sys_chunk_array;
	remain = letoh32(sb->sys_chunk_array_size);
	chunk_base = offsetof(struct btrfs_chunk, stripe);

	while (remain != 0) {
		if (remain < sizeof(*key) + chunk_base)
			return (EINVAL);

		key = (const struct btrfs_key *)p;
		chunk = (const struct btrfs_chunk *)(p + sizeof(*key));
		nstripes = letoh16(chunk->num_stripes);
		if (nstripes == 0 ||
		    nstripes > (remain - sizeof(*key) - chunk_base) /
		    sizeof(struct btrfs_stripe))
			return (EINVAL);
		entry_size = sizeof(*key) + chunk_base +
		    nstripes * sizeof(struct btrfs_stripe);

		error = btrfs_decode_chunk(sb, key, chunk,
		    entry_size - sizeof(*key), &chunk_map);
		if (error != 0)
			return (error);
		if ((chunk_map.type & BTRFS_BLOCK_GROUP_SYSTEM) == 0)
			return (EINVAL);
		if (nchunks != 0 && chunk_map.logical < previous_end)
			return (EINVAL);
		previous_end = chunk_map.logical + chunk_map.length;

		error = btrfs_map_logical(&chunk_map, target, target_len,
		    &target_map);
		if (error == 0) {
			if (mapped)
				return (EINVAL);
			*map = target_map;
			mapped = 1;
		} else if (error != ENOENT)
			return (error);

		nchunks++;
		p += entry_size;
		remain -= entry_size;
	}

	if (!mapped || nchunks == 0)
		return (EINVAL);
	*nchunksp = nchunks;
	return (0);
}

static int
btrfs_decode_chunk(const struct btrfs_super_block *sb,
    const struct btrfs_key *key, const struct btrfs_chunk *chunk,
    size_t item_size, struct btrfs_chunk_map *map)
{
	const struct btrfs_stripe *stripe;
	uint64_t devid, dev_bytes, logical, profile, stripe_offset, type;
	uint64_t chunk_len, stripe_len;
	uint32_t sectorsize;
	uint16_t nstripes;
	size_t expected_size;
	unsigned int i;

	if (item_size < offsetof(struct btrfs_chunk, stripe))
		return (EINVAL);
	nstripes = letoh16(chunk->num_stripes);
	if (nstripes == 0)
		return (EINVAL);
	expected_size = offsetof(struct btrfs_chunk, stripe) +
	    nstripes * sizeof(*stripe);
	if (item_size != expected_size)
		return (EINVAL);

	if (letoh64(key->objectid) != BTRFS_FIRST_CHUNK_TREE_OBJECTID ||
	    key->type != BTRFS_CHUNK_ITEM_KEY)
		return (EINVAL);

	memset(map, 0, sizeof(*map));
	devid = letoh64(sb->dev_item.devid);
	dev_bytes = letoh64(sb->dev_item.total_bytes);
	sectorsize = letoh32(sb->sectorsize);
	logical = letoh64(key->offset);
	chunk_len = letoh64(chunk->length);
	stripe_len = letoh64(chunk->stripe_len);
	type = letoh64(chunk->type);
	profile = type & BTRFS_BLOCK_GROUP_PROFILE_MASK;

	if (chunk_len == 0 || logical > UINT64_MAX - chunk_len ||
	    (type & BTRFS_BLOCK_GROUP_TYPE_MASK) == 0 ||
	    (logical & (sectorsize - 1)) != 0 ||
	    (chunk_len & (sectorsize - 1)) != 0 ||
	    stripe_len == 0 || !btrfs_ispow2(stripe_len) ||
	    (stripe_len & (sectorsize - 1)) != 0 ||
	    letoh32(chunk->sector_size) != sectorsize)
		return (EINVAL);
	if (type & ~(BTRFS_BLOCK_GROUP_TYPE_MASK |
	    BTRFS_BLOCK_GROUP_PROFILE_MASK))
		return (EOPNOTSUPP);

	if ((profile == 0 && nstripes != 1) ||
	    (profile == BTRFS_BLOCK_GROUP_DUP && nstripes != 2))
		return (EOPNOTSUPP);
	if (profile != 0 && profile != BTRFS_BLOCK_GROUP_DUP)
		return (EOPNOTSUPP);

	map->logical = logical;
	map->length = chunk_len;
	map->type = type;
	map->nmirrors = nstripes;
	for (i = 0; i < nstripes; i++) {
		stripe = &chunk->stripe[i];
		stripe_offset = letoh64(stripe->offset);
		if (letoh64(stripe->devid) != devid ||
		    memcmp(stripe->dev_uuid, sb->dev_item.uuid,
		    BTRFS_UUID_SIZE) != 0 ||
		    (stripe_offset & (sectorsize - 1)) != 0 ||
		    stripe_offset > dev_bytes ||
		    chunk_len > dev_bytes - stripe_offset)
			return (EINVAL);
		map->physical[i] = stripe_offset;
	}

	return (0);
}

static int
btrfs_map_logical(const struct btrfs_chunk_map *chunk, uint64_t logical,
    uint32_t length, struct btrfs_io_map *map)
{
	uint64_t delta;
	unsigned int i;

	if (chunk->length < length || logical < chunk->logical)
		return (ENOENT);
	delta = logical - chunk->logical;
	if (delta > chunk->length - length)
		return (ENOENT);

	memset(map, 0, sizeof(*map));
	map->nmirrors = chunk->nmirrors;
	for (i = 0; i < chunk->nmirrors; i++) {
		if (chunk->physical[i] > UINT64_MAX - delta)
			return (EINVAL);
		map->physical[i] = chunk->physical[i] + delta;
	}
	return (0);
}

static int
btrfs_lookup_logical(const struct btrfs_chunk_map *chunks,
    unsigned int nchunks, uint64_t logical, uint32_t length,
    struct btrfs_io_map *map)
{
	unsigned int i;
	int error;

	for (i = 0; i < nchunks; i++) {
		error = btrfs_map_logical(&chunks[i], logical, length, map);
		if (error == 0)
			return (0);
		if (error != ENOENT)
			return (error);
		if (logical < chunks[i].logical)
			break;
	}

	return (ENOENT);
}

static int
btrfs_read_tree_block(struct vnode *devvp,
    const struct btrfs_super_block *sb, const struct btrfs_io_map *map,
    uint64_t logical, uint64_t generation, uint64_t owner, uint8_t level,
    struct buf **bpp, unsigned int *mirrorp)
{
	struct buf *bp;
	unsigned int i;
	int error = EIO;

	*bpp = NULL;
	for (i = 0; i < map->nmirrors; i++) {
		bp = NULL;
		error = bread(devvp, map->physical[i] / DEV_BSIZE,
		    letoh32(sb->nodesize), &bp);
		if (error == 0)
			error = btrfs_validate_tree_block(sb,
			    (const struct btrfs_header *)bp->b_data,
			    logical, generation, owner, level);
		if (error == 0) {
			*bpp = bp;
			*mirrorp = i;
			return (0);
		}
		if (bp != NULL)
			brelse(bp);
	}

	return (error);
}

static int
btrfs_load_chunk_tree(const struct btrfs_super_block *sb,
    const struct btrfs_header *header, const struct btrfs_io_map *bootstrap,
    struct btrfs_chunk_map **chunksp, unsigned int *nchunksp,
    struct btrfs_chunk_stats *stats)
{
	const struct btrfs_item *items;
	const struct btrfs_key *key;
	const uint8_t *leaf_data, *item_data;
	struct btrfs_chunk_map *chunks;
	struct btrfs_io_map root_map;
	uint64_t previous_end = 0;
	uint32_t i, nritems, offset, size;
	unsigned int chunk_index = 0, device_items = 0, root_maps = 0;
	int error;

	*chunksp = NULL;
	*nchunksp = 0;
	memset(stats, 0, sizeof(*stats));

	if (header->level != 0)
		return (EOPNOTSUPP);

	nritems = letoh32(header->nritems);
	items = (const struct btrfs_item *)(header + 1);
	leaf_data = (const uint8_t *)(header + 1);
	for (i = 0; i < nritems; i++) {
		key = &items[i].key;
		offset = letoh32(items[i].offset);
		size = letoh32(items[i].size);
		item_data = leaf_data + offset;

		switch (key->type) {
		case BTRFS_DEV_ITEM_KEY:
			error = btrfs_validate_dev_item(sb, key,
			    (const struct btrfs_dev_item *)item_data, size);
			if (error != 0)
				return (error);
			device_items++;
			break;
		case BTRFS_CHUNK_ITEM_KEY:
			(*nchunksp)++;
			break;
		default:
			return (EINVAL);
		}
	}

	if (device_items != 1 || *nchunksp == 0)
		return (EINVAL);
	chunks = mallocarray(*nchunksp, sizeof(*chunks), M_BTRFS,
	    M_WAITOK | M_ZERO);

	for (i = 0; i < nritems; i++) {
		key = &items[i].key;
		if (key->type != BTRFS_CHUNK_ITEM_KEY)
			continue;
		offset = letoh32(items[i].offset);
		size = letoh32(items[i].size);
		item_data = leaf_data + offset;

		error = btrfs_decode_chunk(sb, key,
		    (const struct btrfs_chunk *)item_data, size,
		    &chunks[chunk_index]);
		if (error != 0)
			goto fail;
		if (chunk_index != 0 &&
		    chunks[chunk_index].logical < previous_end) {
			error = EINVAL;
			goto fail;
		}
		previous_end = chunks[chunk_index].logical +
		    chunks[chunk_index].length;

		if (chunks[chunk_index].type & BTRFS_BLOCK_GROUP_DATA)
			stats->data++;
		if (chunks[chunk_index].type & BTRFS_BLOCK_GROUP_METADATA)
			stats->metadata++;
		if (chunks[chunk_index].type & BTRFS_BLOCK_GROUP_SYSTEM)
			stats->system++;

		error = btrfs_map_logical(&chunks[chunk_index],
		    letoh64(sb->chunk_root), letoh32(sb->nodesize), &root_map);
		if (error == 0) {
			if ((chunks[chunk_index].type &
			    BTRFS_BLOCK_GROUP_SYSTEM) == 0 ||
			    root_map.nmirrors != bootstrap->nmirrors ||
			    memcmp(root_map.physical, bootstrap->physical,
			    root_map.nmirrors * sizeof(root_map.physical[0])) != 0) {
				error = EINVAL;
				goto fail;
			}
			root_maps++;
		} else if (error != ENOENT)
			goto fail;

		chunk_index++;
	}

	if (chunk_index != *nchunksp || root_maps != 1) {
		error = EINVAL;
		goto fail;
	}

	*chunksp = chunks;
	return (0);

fail:
	free(chunks, M_BTRFS, *nchunksp * sizeof(*chunks));
	*nchunksp = 0;
	return (error);
}

static int
btrfs_lookup_leaf_item(const struct btrfs_header *header,
    const struct btrfs_key *target, const uint8_t **datap, uint32_t *sizep)
{
	const struct btrfs_item *items;
	int cmp;
	uint32_t i, nritems, offset;

	*datap = NULL;
	*sizep = 0;
	if (header->level != 0)
		return (EOPNOTSUPP);

	nritems = letoh32(header->nritems);
	items = (const struct btrfs_item *)(header + 1);
	for (i = 0; i < nritems; i++) {
		cmp = btrfs_key_cmp(&items[i].key, target);
		if (cmp < 0)
			continue;
		if (cmp > 0)
			break;

		offset = letoh32(items[i].offset);
		*datap = (const uint8_t *)(header + 1) + offset;
		*sizep = letoh32(items[i].size);
		return (0);
	}

	return (ENOENT);
}

static int
btrfs_find_root_item(const struct btrfs_super_block *sb,
    const struct btrfs_header *header, uint64_t objectid,
    const struct btrfs_root_item **root_itemp)
{
	const uint8_t *data;
	const struct btrfs_root_item *root_item;
	struct btrfs_key target;
	uint64_t bytenr, generation, root_dirid;
	uint32_t refs, sectorsize, size;
	int error;

	*root_itemp = NULL;
	memset(&target, 0, sizeof(target));
	target.objectid = htole64(objectid);
	target.type = BTRFS_ROOT_ITEM_KEY;
	error = btrfs_lookup_leaf_item(header, &target, &data, &size);
	if (error != 0)
		return (error);
	if (size < offsetof(struct btrfs_root_item, generation_v2))
		return (EINVAL);
	root_item = (const struct btrfs_root_item *)data;

	bytenr = letoh64(root_item->bytenr);
	generation = letoh64(root_item->generation);
	root_dirid = letoh64(root_item->root_dirid);
	refs = letoh32(root_item->refs);
	sectorsize = letoh32(sb->sectorsize);
	if (bytenr == 0 || (bytenr & (sectorsize - 1)) != 0 ||
	    generation == 0 || generation > letoh64(sb->generation) ||
	    root_dirid != BTRFS_FIRST_FREE_OBJECTID || refs == 0 ||
	    root_item->level >= BTRFS_MAX_LEVEL)
		return (EINVAL);

	*root_itemp = root_item;
	return (0);
}

static int
btrfs_find_inode_item(const struct btrfs_super_block *sb,
    const struct btrfs_header *header, uint64_t objectid,
    const struct btrfs_inode_item **inode_itemp)
{
	const uint8_t *data;
	const struct btrfs_inode_item *inode_item;
	struct btrfs_key target;
	uint64_t generation, transid;
	uint32_t mode, nlink, size;
	int error;

	*inode_itemp = NULL;
	memset(&target, 0, sizeof(target));
	target.objectid = htole64(objectid);
	target.type = BTRFS_INODE_ITEM_KEY;
	error = btrfs_lookup_leaf_item(header, &target, &data, &size);
	if (error != 0)
		return (error);
	if (size != sizeof(*inode_item))
		return (EINVAL);
	inode_item = (const struct btrfs_inode_item *)data;

	generation = letoh64(inode_item->generation);
	transid = letoh64(inode_item->transid);
	mode = letoh32(inode_item->mode);
	nlink = letoh32(inode_item->nlink);
	if (generation == 0 || generation > letoh64(sb->generation) ||
	    transid > letoh64(sb->generation) ||
	    (mode & S_IFMT) != S_IFDIR || nlink == 0 ||
	    letoh32(inode_item->atime.nsec) >= 1000000000 ||
	    letoh32(inode_item->ctime.nsec) >= 1000000000 ||
	    letoh32(inode_item->mtime.nsec) >= 1000000000 ||
	    letoh32(inode_item->otime.nsec) >= 1000000000)
		return (EINVAL);

	*inode_itemp = inode_item;
	return (0);
}

static int
btrfs_scan_directory(const struct btrfs_super_block *sb,
    const struct btrfs_header *header, uint64_t objectid,
    struct btrfs_dir_stats *stats)
{
	const struct btrfs_dir_item *dir_item;
	const struct btrfs_item *items;
	const struct btrfs_key *key;
	const uint8_t *data, *name;
	uint64_t location, transid;
	uint32_t i, nritems, offset, size;
	uint16_t data_len, name_len;
	size_t record_size, remaining;

	memset(stats, 0, sizeof(*stats));
	if (header->level != 0)
		return (EOPNOTSUPP);

	nritems = letoh32(header->nritems);
	items = (const struct btrfs_item *)(header + 1);
	for (i = 0; i < nritems; i++) {
		key = &items[i].key;
		if (letoh64(key->objectid) < objectid)
			continue;
		if (letoh64(key->objectid) > objectid ||
		    key->type > BTRFS_DIR_INDEX_KEY)
			break;
		if (key->type < BTRFS_DIR_INDEX_KEY)
			continue;

		offset = letoh32(items[i].offset);
		size = letoh32(items[i].size);
		data = (const uint8_t *)(header + 1) + offset;
		remaining = size;
		while (remaining != 0) {
			if (remaining < sizeof(*dir_item))
				return (EINVAL);
			dir_item = (const struct btrfs_dir_item *)data;
			data_len = letoh16(dir_item->data_len);
			name_len = letoh16(dir_item->name_len);
			if (name_len == 0 || name_len > BTRFS_NAME_MAX ||
			    data_len != 0 ||
			    name_len > remaining - sizeof(*dir_item))
				return (EINVAL);
			record_size = sizeof(*dir_item) + name_len;
			name = data + sizeof(*dir_item);
			if (memchr(name, '\0', name_len) != NULL ||
			    memchr(name, '/', name_len) != NULL)
				return (EINVAL);

			location = letoh64(dir_item->location.objectid);
			transid = letoh64(dir_item->transid);
			if (location < BTRFS_FIRST_FREE_OBJECTID ||
			    transid > letoh64(sb->generation) ||
			    dir_item->type > BTRFS_FT_SYMLINK)
				return (EINVAL);
			if (dir_item->location.type == BTRFS_ROOT_ITEM_KEY) {
				if (dir_item->type != BTRFS_FT_DIR)
					return (EINVAL);
				stats->subvolumes++;
			} else if (dir_item->location.type !=
			    BTRFS_INODE_ITEM_KEY ||
			    letoh64(dir_item->location.offset) != 0)
				return (EINVAL);

			stats->entries++;
			switch (dir_item->type) {
			case BTRFS_FT_REG_FILE:
				stats->regular++;
				break;
			case BTRFS_FT_DIR:
				stats->directories++;
				break;
			case BTRFS_FT_SYMLINK:
				stats->symlinks++;
				break;
			default:
				stats->special++;
				break;
			}

			data += record_size;
			remaining -= record_size;
		}
	}

	return (0);
}

static int
btrfs_validate_dev_item(const struct btrfs_super_block *sb,
    const struct btrfs_key *key, const struct btrfs_dev_item *dev_item,
    size_t item_size)
{
	uint64_t bytes_used, devid, total_bytes;

	if (item_size != sizeof(*dev_item) ||
	    letoh64(key->objectid) != BTRFS_DEV_ITEMS_OBJECTID ||
	    key->type != BTRFS_DEV_ITEM_KEY)
		return (EINVAL);

	devid = letoh64(dev_item->devid);
	total_bytes = letoh64(dev_item->total_bytes);
	bytes_used = letoh64(dev_item->bytes_used);
	if (letoh64(key->offset) != devid ||
	    devid != letoh64(sb->dev_item.devid) ||
	    total_bytes != letoh64(sb->dev_item.total_bytes) ||
	    bytes_used > total_bytes ||
	    letoh32(dev_item->sector_size) != letoh32(sb->sectorsize) ||
	    memcmp(dev_item->uuid, sb->dev_item.uuid, BTRFS_UUID_SIZE) != 0 ||
	    memcmp(dev_item->fsid, sb->fsid, BTRFS_UUID_SIZE) != 0)
		return (EINVAL);

	return (0);
}

static int
btrfs_validate_tree_block(const struct btrfs_super_block *sb,
    const struct btrfs_header *header, uint64_t bytenr, uint64_t generation,
    uint64_t owner, uint8_t level)
{
	const struct btrfs_item *items;
	const struct btrfs_key_ptr *ptrs;
	const uint8_t *fsid;
	uint32_t csum, disk_csum, i, nritems, nodesize, offset, size;
	size_t array_end, data_end;

	nodesize = letoh32(sb->nodesize);
	memcpy(&disk_csum, header->csum, sizeof(disk_csum));
	disk_csum = letoh32(disk_csum);
	csum = crc32c(0, (const uint8_t *)header + sizeof(header->csum),
	    nodesize - sizeof(header->csum));
	if (csum != disk_csum)
		return (EINVAL);

	fsid = sb->fsid;
	if (letoh64(sb->incompat_flags) &
	    BTRFS_FEATURE_INCOMPAT_METADATA_UUID)
		fsid = sb->metadata_uuid;
	if (memcmp(header->fsid, fsid, BTRFS_UUID_SIZE) != 0 ||
	    letoh64(header->bytenr) != bytenr ||
	    letoh64(header->generation) != generation ||
	    letoh64(header->owner) != owner || header->level != level ||
	    level >= BTRFS_MAX_LEVEL)
		return (EINVAL);

	nritems = letoh32(header->nritems);
	if (nritems == 0)
		return (EINVAL);

	if (level == 0) {
		if (nritems > (nodesize - sizeof(*header)) / sizeof(*items))
			return (EINVAL);
		array_end = nritems * sizeof(*items);
		data_end = nodesize - sizeof(*header);
		items = (const struct btrfs_item *)(header + 1);
		for (i = 0; i < nritems; i++) {
			offset = letoh32(items[i].offset);
			size = letoh32(items[i].size);
			if (offset < array_end || offset > data_end ||
			    size > data_end - offset)
				return (EINVAL);
			if (i != 0 &&
			    btrfs_key_cmp(&items[i - 1].key,
			    &items[i].key) >= 0)
				return (EINVAL);
			data_end = offset;
		}
	} else {
		if (nritems > (nodesize - sizeof(*header)) / sizeof(*ptrs))
			return (EINVAL);
		ptrs = (const struct btrfs_key_ptr *)(header + 1);
		for (i = 0; i < nritems; i++) {
			if (letoh64(ptrs[i].blockptr) == 0 ||
			    (letoh64(ptrs[i].blockptr) &
			    (letoh32(sb->sectorsize) - 1)) != 0 ||
			    letoh64(ptrs[i].generation) == 0)
				return (EINVAL);
			if (i != 0 &&
			    btrfs_key_cmp(&ptrs[i - 1].key,
			    &ptrs[i].key) >= 0)
				return (EINVAL);
		}
	}

	return (0);
}

static int
btrfs_key_cmp(const struct btrfs_key *a, const struct btrfs_key *b)
{
	uint64_t a_objectid, a_offset, b_objectid, b_offset;

	a_objectid = letoh64(a->objectid);
	b_objectid = letoh64(b->objectid);
	if (a_objectid < b_objectid)
		return (-1);
	if (a_objectid > b_objectid)
		return (1);
	if (a->type < b->type)
		return (-1);
	if (a->type > b->type)
		return (1);
	a_offset = letoh64(a->offset);
	b_offset = letoh64(b->offset);
	if (a_offset < b_offset)
		return (-1);
	if (a_offset > b_offset)
		return (1);
	return (0);
}

static int
btrfs_ispow2(uint32_t value)
{
	return (value != 0 && (value & (value - 1)) == 0);
}
