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

struct btrfs_io_map {
	uint64_t	physical[2];
	uint64_t	type;
	unsigned int	nmirrors;
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
static int	btrfs_mountfs(struct vnode *, struct mount *, struct proc *);
static int	btrfs_start(struct mount *, int, struct proc *);
static int	btrfs_unmount(struct mount *, int, struct proc *);
static int	btrfs_root(struct mount *, struct vnode **);
static int	btrfs_statfs(struct mount *, struct statfs *, struct proc *);
static int	btrfs_sync(struct mount *, int, int, struct ucred *,
		    struct proc *);
static int	btrfs_validate_super(const struct btrfs_super_block *,
		    uint64_t);
static int	btrfs_parse_system_chunks(const struct btrfs_super_block *,
		    struct btrfs_chunk_map **, unsigned int *);
static int	btrfs_decode_chunk(const struct btrfs_super_block *,
		    const struct btrfs_key *, const struct btrfs_chunk *, size_t,
		    struct btrfs_chunk_map *);
static int	btrfs_map_logical(const struct btrfs_chunk_map *, uint64_t,
		    uint32_t, struct btrfs_io_map *);
static int	btrfs_lookup_logical(const struct btrfs_chunk_map *,
		    unsigned int, uint64_t, uint32_t, struct btrfs_io_map *);
static int	btrfs_read_tree_block(struct vnode *,
		    const struct btrfs_super_block *, const struct btrfs_io_map *,
		    uint64_t, uint64_t, uint64_t, uint8_t, struct buf **);
static int	btrfs_read_root_block(const struct btrfs_root *, uint64_t,
		    uint64_t, uint8_t, struct buf **);
static int	btrfs_load_chunk_tree(struct btrfs_root *,
		    const struct btrfs_io_map *,
		    struct btrfs_chunk_map **, unsigned int *);
static int	btrfs_find_root_item(struct btrfs_root *, uint64_t, uint64_t,
		    struct btrfs_root_item *);
static int	btrfs_find_inode_item(struct btrfs_root *, uint64_t,
		    struct btrfs_inode_item *);
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
	const struct btrfs_super_block *sb;
	struct btrfs_inode_item inode_item;
	struct btrfs_root_item csum_root_item, fs_root_item;
	struct btrfs_mount *bmp = NULL;
	struct btrfs_chunk_map *chunks = NULL;
	struct btrfs_chunk_map *system_chunks = NULL;
	struct btrfs_root chunk_tree, fs_tree, root_tree;
	struct btrfs_io_map map;
	struct buf *bp = NULL;
	uint64_t chunk_root, csum_root, csum_root_generation;
	uint64_t fs_root, fs_root_generation, generation, root;
	uint32_t nodesize;
	unsigned int nchunks = 0, nsystem_chunks = 0;
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

	chunk_root = letoh64(sb->chunk_root);
	error = btrfs_parse_system_chunks(sb, &system_chunks,
	    &nsystem_chunks);
	if (error != 0)
		goto out;
	error = btrfs_lookup_logical(system_chunks, nsystem_chunks,
	    chunk_root, nodesize, &map);
	if (error != 0)
		goto out;

	memset(&chunk_tree, 0, sizeof(chunk_tree));
	chunk_tree.br_devvp = devvp;
	chunk_tree.br_super = sb;
	chunk_tree.br_chunks = system_chunks;
	chunk_tree.br_nchunks = nsystem_chunks;
	chunk_tree.br_bytenr = chunk_root;
	chunk_tree.br_generation = letoh64(sb->chunk_root_generation);
	chunk_tree.br_owner = BTRFS_CHUNK_TREE_OBJECTID;
	chunk_tree.br_level = sb->chunk_root_level;
	error = btrfs_load_chunk_tree(&chunk_tree, &map, &chunks, &nchunks);
	if (error != 0)
		goto out;

	root = letoh64(sb->root);
	memset(&root_tree, 0, sizeof(root_tree));
	root_tree.br_devvp = devvp;
	root_tree.br_super = sb;
	root_tree.br_chunks = chunks;
	root_tree.br_nchunks = nchunks;
	root_tree.br_bytenr = root;
	root_tree.br_generation = generation;
	root_tree.br_owner = BTRFS_ROOT_TREE_OBJECTID;
	root_tree.br_level = sb->root_level;
	error = btrfs_find_root_item(&root_tree, BTRFS_FS_TREE_OBJECTID,
	    BTRFS_FIRST_FREE_OBJECTID, &fs_root_item);
	if (error != 0)
		goto out;
	fs_root = letoh64(fs_root_item.bytenr);
	fs_root_generation = letoh64(fs_root_item.generation);
	error = btrfs_find_root_item(&root_tree, BTRFS_CSUM_TREE_OBJECTID, 0,
	    &csum_root_item);
	if (error != 0)
		goto out;
	csum_root = letoh64(csum_root_item.bytenr);
	csum_root_generation = letoh64(csum_root_item.generation);

	memset(&fs_tree, 0, sizeof(fs_tree));
	fs_tree.br_devvp = devvp;
	fs_tree.br_super = sb;
	fs_tree.br_chunks = chunks;
	fs_tree.br_nchunks = nchunks;
	fs_tree.br_bytenr = fs_root;
	fs_tree.br_generation = fs_root_generation;
	fs_tree.br_owner = BTRFS_FS_TREE_OBJECTID;
	fs_tree.br_level = fs_root_item.level;
	error = btrfs_find_inode_item(&fs_tree, BTRFS_FIRST_FREE_OBJECTID,
	    &inode_item);
	if (error != 0)
		goto out;

	error = btrfs_iterate_directory(&fs_tree, BTRFS_FIRST_FREE_OBJECTID,
	    NULL, NULL);
	if (error != 0)
		goto out;

	bmp = malloc(sizeof(*bmp), M_BTRFS, M_WAITOK | M_ZERO);
	bmp->bm_mount = mp;
	bmp->bm_devvp = devvp;
	bmp->bm_dev = devvp->v_rdev;
	memcpy(&bmp->bm_super, sb, sizeof(bmp->bm_super));
	bmp->bm_chunks = chunks;
	bmp->bm_nchunks = nchunks;
	bmp->bm_treeid = BTRFS_FS_TREE_OBJECTID;
	bmp->bm_fs_root = fs_root;
	bmp->bm_fs_root_generation = fs_root_generation;
	bmp->bm_fs_root_level = fs_root_item.level;
	bmp->bm_csum_root = csum_root;
	bmp->bm_csum_root_generation = csum_root_generation;
	bmp->bm_csum_root_level = csum_root_item.level;
	bmp->bm_root_dirid = BTRFS_FIRST_FREE_OBJECTID;
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
	if (system_chunks != NULL)
		free(system_chunks, M_BTRFS,
		    nsystem_chunks * sizeof(*system_chunks));
	if (chunks != NULL)
		free(chunks, M_BTRFS, nchunks * sizeof(*chunks));
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
	struct btrfs_root root;
	struct btrfs_inode_item inode;
	struct btrfs_node *node;
	struct vnode *vp;
	enum vtype type;
	int error;

	if (ino < BTRFS_FIRST_FREE_OBJECTID ||
	    ino > BTRFS_LAST_FREE_OBJECTID)
		return (ENOENT);

again:
	error = btrfs_node_lookup(bmp, bmp->bm_treeid, ino, vpp);
	if (error != 0 || *vpp != NULL)
		return (error);

	btrfs_init_fs_root(bmp, &root);
	error = btrfs_find_inode_item(&root, ino, &inode);
	if (error != 0)
		return (error);
	type = IFTOVT(letoh32(inode.mode));

	node = malloc(sizeof(*node), M_BTRFS, M_WAITOK | M_ZERO);
	error = getnewvnode(VT_BTRFS, mp, &btrfs_vops, &vp);
	if (error != 0) {
		free(node, M_BTRFS, sizeof(*node));
		return (error);
	}

	node->bn_vnode = vp;
	node->bn_mount = bmp;
	node->bn_treeid = bmp->bm_treeid;
	node->bn_ino = ino;
	memcpy(&node->bn_inode, &inode, sizeof(node->bn_inode));
	rrw_init_flags(&node->bn_lock, "btrfsnode",
	    RWL_DUPOK | RWL_IS_VNODE);
	vp->v_data = node;
	vp->v_type = type;
	if (ino == bmp->bm_root_dirid)
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
    struct btrfs_chunk_map **chunksp, unsigned int *nchunksp)
{
	const struct btrfs_key *key;
	const struct btrfs_chunk *chunk;
	const uint8_t *p;
	struct btrfs_chunk_map *chunks;
	struct btrfs_chunk_map chunk_map;
	uint16_t nstripes;
	size_t chunk_base, entry_size, remain;
	uint64_t previous_end = 0;
	unsigned int nchunks = 0, total;
	int error;

	*chunksp = NULL;
	*nchunksp = 0;
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

		nchunks++;
		p += entry_size;
		remain -= entry_size;
	}

	if (nchunks == 0)
		return (EINVAL);
	total = nchunks;
	chunks = mallocarray(nchunks, sizeof(*chunks), M_BTRFS,
	    M_WAITOK | M_ZERO);

	p = sb->sys_chunk_array;
	remain = letoh32(sb->sys_chunk_array_size);
	nchunks = 0;
	while (remain != 0) {
		key = (const struct btrfs_key *)p;
		chunk = (const struct btrfs_chunk *)(p + sizeof(*key));
		nstripes = letoh16(chunk->num_stripes);
		entry_size = sizeof(*key) + chunk_base +
		    nstripes * sizeof(struct btrfs_stripe);
		error = btrfs_decode_chunk(sb, key, chunk,
		    entry_size - sizeof(*key), &chunks[nchunks]);
		if (error != 0) {
			free(chunks, M_BTRFS, total * sizeof(*chunks));
			return (error);
		}
		nchunks++;
		p += entry_size;
		remain -= entry_size;
	}

	*chunksp = chunks;
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
	map->type = chunk->type;
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
    struct buf **bpp)
{
	struct buf *bp;
	unsigned int i;
	int error = EIO;

	*bpp = NULL;
	for (i = 0; i < map->nmirrors; i++) {
		bp = NULL;
		error = bread(devvp, map->physical[i] / DEV_BSIZE,
		    letoh32(sb->nodesize), &bp);
		if (error == 0 && bp->b_resid != 0)
			error = EIO;
		if (error == 0)
			error = btrfs_validate_tree_block(sb,
			    (const struct btrfs_header *)bp->b_data,
			    logical, generation, owner, level);
		if (error == 0) {
			*bpp = bp;
			return (0);
		}
		if (bp != NULL)
			brelse(bp);
	}

	return (error);
}

static int
btrfs_read_root_block(const struct btrfs_root *root, uint64_t logical,
    uint64_t generation, uint8_t level, struct buf **bpp)
{
	struct btrfs_io_map map;
	int error;

	error = btrfs_lookup_logical(root->br_chunks, root->br_nchunks,
	    logical, letoh32(root->br_super->nodesize), &map);
	if (error != 0)
		return (error == ENOENT ? EINVAL : error);
	if ((map.type & (BTRFS_BLOCK_GROUP_METADATA |
	    BTRFS_BLOCK_GROUP_SYSTEM)) == 0)
		return (EINVAL);
	return (btrfs_read_tree_block(root->br_devvp, root->br_super, &map,
	    logical, generation, root->br_owner, level, bpp));
}

void
btrfs_init_fs_root(struct btrfs_mount *bmp, struct btrfs_root *root)
{
	memset(root, 0, sizeof(*root));
	root->br_devvp = bmp->bm_devvp;
	root->br_super = &bmp->bm_super;
	root->br_chunks = bmp->bm_chunks;
	root->br_nchunks = bmp->bm_nchunks;
	root->br_bytenr = bmp->bm_fs_root;
	root->br_generation = bmp->bm_fs_root_generation;
	root->br_owner = bmp->bm_treeid;
	root->br_level = bmp->bm_fs_root_level;
}

void
btrfs_init_csum_root(struct btrfs_mount *bmp, struct btrfs_root *root)
{
	memset(root, 0, sizeof(*root));
	root->br_devvp = bmp->bm_devvp;
	root->br_super = &bmp->bm_super;
	root->br_chunks = bmp->bm_chunks;
	root->br_nchunks = bmp->bm_nchunks;
	root->br_bytenr = bmp->bm_csum_root;
	root->br_generation = bmp->bm_csum_root_generation;
	root->br_owner = BTRFS_CSUM_TREE_OBJECTID;
	root->br_level = bmp->bm_csum_root_level;
}

static const struct btrfs_key *
btrfs_block_key(const struct btrfs_header *header, uint32_t slot)
{
	const struct btrfs_item *items;
	const struct btrfs_key_ptr *ptrs;

	if (header->level == 0) {
		items = (const struct btrfs_item *)(header + 1);
		return (&items[slot].key);
	}
	ptrs = (const struct btrfs_key_ptr *)(header + 1);
	return (&ptrs[slot].key);
}

static int
btrfs_read_child(struct btrfs_path *path, uint8_t parent_level,
    uint32_t slot, struct buf **bpp)
{
	const struct btrfs_header *child, *parent;
	const struct btrfs_header *ancestor;
	const struct btrfs_key_ptr *ancestor_ptrs, *ptrs;
	const struct btrfs_key *first, *last, *upper = NULL;
	uint32_t ancestor_slot, nritems;
	uint8_t level;
	int error;

	parent = (const struct btrfs_header *)
	    path->bp_buf[parent_level]->b_data;
	ptrs = (const struct btrfs_key_ptr *)(parent + 1);
	error = btrfs_read_root_block(path->bp_root,
	    letoh64(ptrs[slot].blockptr), letoh64(ptrs[slot].generation),
	    parent_level - 1, bpp);
	if (error != 0)
		return (error);

	child = (const struct btrfs_header *)(*bpp)->b_data;
	nritems = letoh32(child->nritems);
	first = btrfs_block_key(child, 0);
	last = btrfs_block_key(child, nritems - 1);
	if (slot + 1 < letoh32(parent->nritems))
		upper = &ptrs[slot + 1].key;
	for (level = parent_level + 1;
	    upper == NULL && level <= path->bp_root->br_level; level++) {
		ancestor = (const struct btrfs_header *)
		    path->bp_buf[level]->b_data;
		ancestor_slot = path->bp_slot[level];
		if (ancestor_slot + 1 >= letoh32(ancestor->nritems))
			continue;
		ancestor_ptrs =
		    (const struct btrfs_key_ptr *)(ancestor + 1);
		upper = &ancestor_ptrs[ancestor_slot + 1].key;
	}
	if (btrfs_key_cmp(first, &ptrs[slot].key) != 0 ||
	    (upper != NULL && btrfs_key_cmp(last, upper) >= 0)) {
		brelse(*bpp);
		*bpp = NULL;
		return (EINVAL);
	}
	return (0);
}

void
btrfs_release_path(struct btrfs_path *path)
{
	unsigned int level;

	for (level = 0; level < BTRFS_MAX_LEVEL; level++) {
		if (path->bp_buf[level] != NULL) {
			brelse(path->bp_buf[level]);
			path->bp_buf[level] = NULL;
		}
		path->bp_slot[level] = 0;
	}
	path->bp_root = NULL;
}

int
btrfs_search_slot(struct btrfs_root *root, const struct btrfs_key *target,
    struct btrfs_path *path)
{
	const struct btrfs_header *header;
	const struct btrfs_key_ptr *ptrs;
	const struct btrfs_item *items;
	uint32_t high, low, mid, slot;
	uint8_t level;
	int cmp, error;

	if (path->bp_root != NULL)
		btrfs_release_path(path);
	if (root->br_level >= BTRFS_MAX_LEVEL)
		return (EINVAL);
	path->bp_root = root;
	error = btrfs_read_root_block(root, root->br_bytenr,
	    root->br_generation, root->br_level,
	    &path->bp_buf[root->br_level]);
	if (error != 0)
		goto fail;

	for (level = root->br_level; level != 0; level--) {
		header = (const struct btrfs_header *)path->bp_buf[level]->b_data;
		ptrs = (const struct btrfs_key_ptr *)(header + 1);
		low = 0;
		high = letoh32(header->nritems);
		while (low < high) {
			mid = low + (high - low) / 2;
			if (btrfs_key_cmp(&ptrs[mid].key, target) <= 0)
				low = mid + 1;
			else
				high = mid;
		}
		slot = low == 0 ? 0 : low - 1;
		path->bp_slot[level] = slot;
		error = btrfs_read_child(path, level, slot,
		    &path->bp_buf[level - 1]);
		if (error != 0)
			goto fail;
	}

	header = (const struct btrfs_header *)path->bp_buf[0]->b_data;
	items = (const struct btrfs_item *)(header + 1);
	low = 0;
	high = letoh32(header->nritems);
	while (low < high) {
		mid = low + (high - low) / 2;
		if (btrfs_key_cmp(&items[mid].key, target) < 0)
			low = mid + 1;
		else
			high = mid;
	}
	path->bp_slot[0] = low;
	if (low < letoh32(header->nritems)) {
		cmp = btrfs_key_cmp(&items[low].key, target);
		if (cmp == 0)
			return (0);
	}
	return (ENOENT);

fail:
	btrfs_release_path(path);
	return (error);
}

int
btrfs_path_item(const struct btrfs_path *path,
    const struct btrfs_key **keyp, const uint8_t **datap, uint32_t *sizep)
{
	const struct btrfs_header *header;
	const struct btrfs_item *items;
	uint32_t offset, slot;

	if (keyp != NULL)
		*keyp = NULL;
	if (datap != NULL)
		*datap = NULL;
	if (sizep != NULL)
		*sizep = 0;
	if (path->bp_buf[0] == NULL)
		return (ENOENT);
	header = (const struct btrfs_header *)path->bp_buf[0]->b_data;
	slot = path->bp_slot[0];
	if (slot >= letoh32(header->nritems))
		return (ENOENT);
	items = (const struct btrfs_item *)(header + 1);
	offset = letoh32(items[slot].offset);
	if (keyp != NULL)
		*keyp = &items[slot].key;
	if (datap != NULL)
		*datap = (const uint8_t *)(header + 1) + offset;
	if (sizep != NULL)
		*sizep = letoh32(items[slot].size);
	return (0);
}

int
btrfs_next_item(struct btrfs_path *path)
{
	const struct btrfs_header *header;
	uint32_t nritems;
	uint8_t level, child_level;
	int error;

	if (path->bp_root == NULL || path->bp_buf[0] == NULL)
		return (ENOENT);
	header = (const struct btrfs_header *)path->bp_buf[0]->b_data;
	nritems = letoh32(header->nritems);
	if (path->bp_slot[0] < nritems &&
	    path->bp_slot[0] + 1 < nritems) {
		path->bp_slot[0]++;
		return (0);
	}

	for (level = 1; level <= path->bp_root->br_level; level++) {
		header = (const struct btrfs_header *)
		    path->bp_buf[level]->b_data;
		if (path->bp_slot[level] + 1 >=
		    letoh32(header->nritems))
			continue;
		path->bp_slot[level]++;
		for (child_level = level; child_level != 0; child_level--) {
			if (path->bp_buf[child_level - 1] != NULL) {
				brelse(path->bp_buf[child_level - 1]);
				path->bp_buf[child_level - 1] = NULL;
			}
			error = btrfs_read_child(path, child_level,
			    path->bp_slot[child_level],
			    &path->bp_buf[child_level - 1]);
			if (error != 0) {
				btrfs_release_path(path);
				return (error);
			}
			path->bp_slot[child_level - 1] = 0;
		}
		return (0);
	}

	path->bp_slot[0] = nritems;
	return (ENOENT);
}

int
btrfs_prev_item(struct btrfs_path *path)
{
	const struct btrfs_header *header;
	uint8_t level, child_level;
	int error;

	if (path->bp_root == NULL || path->bp_buf[0] == NULL)
		return (ENOENT);
	if (path->bp_slot[0] != 0) {
		path->bp_slot[0]--;
		return (0);
	}

	for (level = 1; level <= path->bp_root->br_level; level++) {
		if (path->bp_slot[level] == 0)
			continue;
		path->bp_slot[level]--;
		for (child_level = level; child_level != 0; child_level--) {
			if (path->bp_buf[child_level - 1] != NULL) {
				brelse(path->bp_buf[child_level - 1]);
				path->bp_buf[child_level - 1] = NULL;
			}
			error = btrfs_read_child(path, child_level,
			    path->bp_slot[child_level],
			    &path->bp_buf[child_level - 1]);
			if (error != 0) {
				btrfs_release_path(path);
				return (error);
			}
			header = (const struct btrfs_header *)
			    path->bp_buf[child_level - 1]->b_data;
			path->bp_slot[child_level - 1] =
			    letoh32(header->nritems) - 1;
		}
		return (0);
	}

	return (ENOENT);
}

int
btrfs_search_lower_bound(struct btrfs_root *root,
    const struct btrfs_key *target, struct btrfs_path *path)
{
	int error;

	error = btrfs_search_slot(root, target, path);
	if (error == 0)
		return (0);
	if (error != ENOENT)
		return (error);
	error = btrfs_path_item(path, NULL, NULL, NULL);
	if (error == 0)
		return (0);
	return (btrfs_next_item(path));
}

int
btrfs_search_predecessor(struct btrfs_root *root,
    const struct btrfs_key *target, struct btrfs_path *path)
{
	int error;

	error = btrfs_search_slot(root, target, path);
	if (error == 0)
		return (0);
	if (error != ENOENT)
		return (error);
	return (btrfs_prev_item(path));
}

int
btrfs_lookup_data_csum(struct btrfs_mount *bmp, uint64_t logical,
    uint32_t *csump)
{
	const struct btrfs_key *key;
	const uint8_t *data;
	struct btrfs_path path = { 0 };
	struct btrfs_root root;
	struct btrfs_key target;
	uint64_t end, span, start;
	uint32_t csum, item_size, sectorsize;
	size_t csum_offset;
	int error;

	sectorsize = letoh32(bmp->bm_super.sectorsize);
	if ((logical & (sectorsize - 1)) != 0)
		return (EINVAL);

	memset(&target, 0, sizeof(target));
	target.objectid = htole64(BTRFS_EXTENT_CSUM_OBJECTID);
	target.type = BTRFS_EXTENT_CSUM_KEY;
	target.offset = htole64(logical);
	btrfs_init_csum_root(bmp, &root);
	error = btrfs_search_predecessor(&root, &target, &path);
	if (error != 0)
		goto out;
	error = btrfs_path_item(&path, &key, &data, &item_size);
	if (error != 0)
		goto out;
	if (letoh64(key->objectid) != BTRFS_EXTENT_CSUM_OBJECTID ||
	    key->type != BTRFS_EXTENT_CSUM_KEY) {
		error = ENOENT;
		goto out;
	}

	start = letoh64(key->offset);
	if ((start & (sectorsize - 1)) != 0 || item_size == 0 ||
	    item_size % sizeof(csum) != 0) {
		error = EINVAL;
		goto out;
	}
	span = (uint64_t)(item_size / sizeof(csum)) * sectorsize;
	if (start > UINT64_MAX - span) {
		error = EINVAL;
		goto out;
	}
	end = start + span;
	if (logical < start || logical >= end) {
		error = ENOENT;
		goto out;
	}
	csum_offset = (logical - start) / sectorsize * sizeof(csum);
	memcpy(&csum, data + csum_offset, sizeof(csum));
	*csump = letoh32(csum);
	error = 0;
out:
	btrfs_release_path(&path);
	return (error);
}

int
btrfs_read_data_block(struct btrfs_mount *bmp, uint64_t logical,
    const uint32_t *expected_csum, struct buf **bpp)
{
	struct btrfs_io_map map;
	struct buf *bp;
	uint32_t actual_csum;
	uint32_t sectorsize;
	unsigned int i;
	int error = EIO;

	*bpp = NULL;
	sectorsize = letoh32(bmp->bm_super.sectorsize);
	if ((logical & (sectorsize - 1)) != 0)
		return (EINVAL);
	error = btrfs_lookup_logical(bmp->bm_chunks, bmp->bm_nchunks,
	    logical, sectorsize, &map);
	if (error != 0)
		return (error == ENOENT ? EINVAL : error);
	if ((map.type & BTRFS_BLOCK_GROUP_DATA) == 0)
		return (EINVAL);

	for (i = 0; i < map.nmirrors; i++) {
		bp = NULL;
		error = bread(bmp->bm_devvp, map.physical[i] / DEV_BSIZE,
		    sectorsize, &bp);
		if (error == 0 && bp->b_resid == 0) {
			if (expected_csum == NULL) {
				*bpp = bp;
				return (0);
			}
			actual_csum = crc32c(0, bp->b_data, sectorsize);
			if (actual_csum == *expected_csum) {
				*bpp = bp;
				return (0);
			}
			error = EIO;
		}
		if (bp != NULL)
			brelse(bp);
		if (error == 0)
			error = EIO;
	}

	return (error);
}

static int
btrfs_load_chunk_tree(struct btrfs_root *root,
    const struct btrfs_io_map *bootstrap,
    struct btrfs_chunk_map **chunksp, unsigned int *nchunksp)
{
	const struct btrfs_key *key;
	const struct btrfs_super_block *sb = root->br_super;
	const uint8_t *item_data;
	struct btrfs_path path = { 0 };
	struct btrfs_key target;
	struct btrfs_chunk_map *chunks;
	struct btrfs_io_map root_map;
	uint64_t previous_end = 0;
	uint32_t size;
	unsigned int chunk_index = 0, device_items = 0, root_maps = 0;
	int error;

	*chunksp = NULL;
	*nchunksp = 0;
	memset(&target, 0, sizeof(target));
	error = btrfs_search_lower_bound(root, &target, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &key, &item_data, &size);
		if (error != 0)
			break;
		switch (key->type) {
		case BTRFS_DEV_ITEM_KEY:
			error = btrfs_validate_dev_item(sb, key,
			    (const struct btrfs_dev_item *)item_data, size);
			if (error != 0)
				goto count_out;
			device_items++;
			break;
		case BTRFS_CHUNK_ITEM_KEY:
			(*nchunksp)++;
			break;
		default:
			error = EINVAL;
			goto count_out;
		}
		error = btrfs_next_item(&path);
	}
	if (error == ENOENT)
		error = 0;
count_out:
	btrfs_release_path(&path);
	if (error != 0)
		return (error);

	if (device_items != 1 || *nchunksp == 0)
		return (EINVAL);
	chunks = mallocarray(*nchunksp, sizeof(*chunks), M_BTRFS,
	    M_WAITOK | M_ZERO);

	error = btrfs_search_lower_bound(root, &target, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &key, &item_data, &size);
		if (error != 0)
			goto fail;
		if (key->type != BTRFS_CHUNK_ITEM_KEY) {
			error = btrfs_next_item(&path);
			continue;
		}

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
		error = btrfs_next_item(&path);
	}
	if (error == ENOENT)
		error = 0;
	if (error != 0)
		goto fail;
	btrfs_release_path(&path);

	if (chunk_index != *nchunksp || root_maps != 1) {
		error = EINVAL;
		goto fail;
	}

	*chunksp = chunks;
	return (0);

fail:
	btrfs_release_path(&path);
	free(chunks, M_BTRFS, *nchunksp * sizeof(*chunks));
	*nchunksp = 0;
	return (error);
}

static int
btrfs_find_root_item(struct btrfs_root *root, uint64_t objectid,
    uint64_t root_dirid, struct btrfs_root_item *result)
{
	const uint8_t *data;
	const struct btrfs_root_item *root_item;
	const struct btrfs_super_block *sb = root->br_super;
	struct btrfs_path path = { 0 };
	struct btrfs_key target;
	uint64_t bytenr, generation;
	uint32_t refs, sectorsize, size;
	int error;

	memset(&target, 0, sizeof(target));
	target.objectid = htole64(objectid);
	target.type = BTRFS_ROOT_ITEM_KEY;
	error = btrfs_search_slot(root, &target, &path);
	if (error != 0)
		goto out;
	error = btrfs_path_item(&path, NULL, &data, &size);
	if (error != 0)
		goto out;
	if (size < offsetof(struct btrfs_root_item, generation_v2))
		goto invalid;
	root_item = (const struct btrfs_root_item *)data;

	bytenr = letoh64(root_item->bytenr);
	generation = letoh64(root_item->generation);
	refs = letoh32(root_item->refs);
	sectorsize = letoh32(sb->sectorsize);
	if (bytenr == 0 || (bytenr & (sectorsize - 1)) != 0 ||
	    generation == 0 || generation > letoh64(sb->generation) ||
	    letoh64(root_item->root_dirid) != root_dirid || refs == 0 ||
	    root_item->level >= BTRFS_MAX_LEVEL)
		goto invalid;

	memset(result, 0, sizeof(*result));
	memcpy(result, root_item, MIN(size, sizeof(*result)));
	error = 0;
	goto out;
invalid:
	error = EINVAL;
out:
	btrfs_release_path(&path);
	return (error);
}

static int
btrfs_find_inode_item(struct btrfs_root *root, uint64_t objectid,
    struct btrfs_inode_item *result)
{
	const uint8_t *data;
	const struct btrfs_inode_item *inode_item;
	const struct btrfs_super_block *sb = root->br_super;
	struct btrfs_path path = { 0 };
	struct btrfs_key target;
	uint64_t generation, transid;
	uint32_t mode, nlink, size;
	int error;

	memset(&target, 0, sizeof(target));
	target.objectid = htole64(objectid);
	target.type = BTRFS_INODE_ITEM_KEY;
	error = btrfs_search_slot(root, &target, &path);
	if (error != 0)
		goto out;
	error = btrfs_path_item(&path, NULL, &data, &size);
	if (error != 0)
		goto out;
	if (size != sizeof(*inode_item))
		goto invalid;
	inode_item = (const struct btrfs_inode_item *)data;

	generation = letoh64(inode_item->generation);
	transid = letoh64(inode_item->transid);
	mode = letoh32(inode_item->mode);
	nlink = letoh32(inode_item->nlink);
	if (generation == 0 || generation > letoh64(sb->generation) ||
	    transid > letoh64(sb->generation) ||
	    IFTOVT(mode) == VNON || IFTOVT(mode) == VBAD || nlink == 0 ||
	    letoh32(inode_item->atime.nsec) >= 1000000000 ||
	    letoh32(inode_item->ctime.nsec) >= 1000000000 ||
	    letoh32(inode_item->mtime.nsec) >= 1000000000 ||
	    letoh32(inode_item->otime.nsec) >= 1000000000)
		goto invalid;

	memcpy(result, inode_item, sizeof(*result));
	error = 0;
	goto out;
invalid:
	error = EINVAL;
out:
	btrfs_release_path(&path);
	return (error);
}

int
btrfs_iterate_directory(struct btrfs_root *root, uint64_t objectid,
    btrfs_dir_iter_fn callback, void *arg)
{
	const struct btrfs_dir_item *dir_item;
	const struct btrfs_key *key;
	const struct btrfs_super_block *sb = root->br_super;
	struct btrfs_path path = { 0 };
	struct btrfs_key target;
	struct btrfs_dir_entry entry;
	const uint8_t *data, *name;
	uint64_t location, transid;
	uint32_t size;
	uint16_t data_len, name_len;
	size_t record_size, remaining;
	int error;

	memset(&target, 0, sizeof(target));
	target.objectid = htole64(objectid);
	target.type = BTRFS_DIR_INDEX_KEY;
	error = btrfs_search_lower_bound(root, &target, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &key, &data, &size);
		if (error != 0)
			break;
		if (letoh64(key->objectid) > objectid ||
		    key->type > BTRFS_DIR_INDEX_KEY)
			break;
		remaining = size;
		while (remaining != 0) {
			if (remaining < sizeof(*dir_item))
				goto invalid;
			dir_item = (const struct btrfs_dir_item *)data;
			data_len = letoh16(dir_item->data_len);
			name_len = letoh16(dir_item->name_len);
			if (name_len == 0 || name_len > BTRFS_NAME_MAX ||
			    data_len != 0 ||
			    name_len > remaining - sizeof(*dir_item))
				goto invalid;
			record_size = sizeof(*dir_item) + name_len;
			name = data + sizeof(*dir_item);
			if (memchr(name, '\0', name_len) != NULL ||
			    memchr(name, '/', name_len) != NULL ||
			    (name_len == 1 && name[0] == '.') ||
			    (name_len == 2 && name[0] == '.' &&
			    name[1] == '.'))
				goto invalid;

			location = letoh64(dir_item->location.objectid);
			transid = letoh64(dir_item->transid);
			if (location < BTRFS_FIRST_FREE_OBJECTID ||
			    transid > letoh64(sb->generation) ||
			    dir_item->type > BTRFS_FT_SYMLINK)
				goto invalid;
			if (dir_item->location.type == BTRFS_ROOT_ITEM_KEY) {
				if (dir_item->type != BTRFS_FT_DIR)
					goto invalid;
			} else if (dir_item->location.type !=
			    BTRFS_INODE_ITEM_KEY ||
			    letoh64(dir_item->location.offset) != 0)
				goto invalid;

			if (callback != NULL) {
				entry.bde_name = name;
				entry.bde_objectid = location;
				entry.bde_index = letoh64(key->offset);
				entry.bde_namelen = name_len;
				entry.bde_type = dir_item->type;
				entry.bde_subvolume =
				    dir_item->location.type ==
				    BTRFS_ROOT_ITEM_KEY;
				error = callback(&entry, arg);
				if (error != 0)
					goto out;
			}

			data += record_size;
			remaining -= record_size;
		}
		error = btrfs_next_item(&path);
	}

	if (error == ENOENT)
		error = 0;
	goto out;
invalid:
	error = EINVAL;
out:
	btrfs_release_path(&path);
	return (error);
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
	    bytenr == 0 ||
	    (bytenr & (letoh32(sb->sectorsize) - 1)) != 0 ||
	    generation == 0 || generation > letoh64(sb->generation) ||
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
			    letoh64(ptrs[i].generation) == 0 ||
			    letoh64(ptrs[i].generation) >
			    letoh64(sb->generation))
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
