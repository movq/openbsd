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
#include <sys/disklabel.h>
#include <sys/dkio.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/vnode.h>

#include <lib/libkern/crc32c.h>

#include <btrfs/btrfs_var.h>

#define BTRFS_SUPER_SIZE		0x1000
#define BTRFS_MIN_SECTORSIZE		0x1000
#define BTRFS_MAX_SECTORSIZE		0x10000

#define BTRFS_BLOCK_GROUP_PROFILE_MASK	(BTRFS_BLOCK_GROUP_RAID0 |	\
	    BTRFS_BLOCK_GROUP_RAID1 | BTRFS_BLOCK_GROUP_DUP |		\
	    BTRFS_BLOCK_GROUP_RAID10 | BTRFS_BLOCK_GROUP_RAID5 |	\
	    BTRFS_BLOCK_GROUP_RAID6 | BTRFS_BLOCK_GROUP_RAID1C3 |	\
	    BTRFS_BLOCK_GROUP_RAID1C4)
#define BTRFS_BLOCK_GROUP_TYPE_MASK	(BTRFS_BLOCK_GROUP_DATA |	\
	    BTRFS_BLOCK_GROUP_SYSTEM | BTRFS_BLOCK_GROUP_METADATA)

static int	btrfs_validate_super(const struct btrfs_super_block *,
		    uint64_t);
static int	btrfs_parse_system_chunks(const struct btrfs_super_block *,
		    struct btrfs_chunk_map **, unsigned int *);
static int	btrfs_load_chunk_tree(struct btrfs_root *,
		    const struct btrfs_io_map *,
		    struct btrfs_chunk_map **, unsigned int *);
static int	btrfs_load_root_location(struct btrfs_root *, uint64_t,
		    struct btrfs_root_location *);
static int	btrfs_validate_dev_item(const struct btrfs_super_block *,
		    const struct btrfs_key *, const struct btrfs_dev_item *,
		    size_t);
static int	btrfs_snapshot_root(struct btrfs_transaction *, uint64_t,
		    struct btrfs_root_location *);
static void	btrfs_set_super_csum(struct btrfs_super_block *);
static int	btrfs_ispow2(uint32_t);

int
btrfs_read_super_mirrors(struct vnode *devvp, struct proc *p,
    struct btrfs_super_candidate *candidates,
    struct btrfs_super_mirror *mirrors)
{
	const struct btrfs_super_block *sb;
	struct buf *bp;
	struct partinfo pi;
	uint64_t media_size = UINT64_MAX, partition_size;
	unsigned int i, nvalid = 0;
	int error, result = EINVAL;

	memset(mirrors, 0,
	    BTRFS_SUPER_MIRROR_MAX * sizeof(*mirrors));
	if (VOP_IOCTL(devvp, DIOCGPART, &pi, FREAD, FSCRED, p) == 0 &&
	    pi.disklab->d_secsize != 0) {
		partition_size = DL_GETPSIZE(pi.part);
		if (partition_size <= UINT64_MAX / pi.disklab->d_secsize)
			media_size = partition_size * pi.disklab->d_secsize;
	}
	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		mirrors[i].bsm_bytenr = superblock_addrs[i];
		if (superblock_addrs[i] > media_size ||
		    BTRFS_SUPER_SIZE > media_size - superblock_addrs[i]) {
			mirrors[i].bsm_error = ENXIO;
			continue;
		}
		bp = NULL;
		error = bread(devvp, superblock_addrs[i] / DEV_BSIZE,
		    BTRFS_SUPER_SIZE, &bp);
		if (error == 0 && bp->b_resid != 0)
			error = EIO;
		if (error != 0) {
			mirrors[i].bsm_error = error;
			if (bp != NULL)
				brelse(bp);
			continue;
		}
		mirrors[i].bsm_flags |= BTRFS_SUPER_MIRROR_READABLE;

		sb = (const struct btrfs_super_block *)bp->b_data;
		memcpy(&candidates[i].bsc_super, sb,
		    sizeof(candidates[i].bsc_super));
		brelse(bp);

		error = btrfs_validate_super(&candidates[i].bsc_super,
		    superblock_addrs[i]);
		mirrors[i].bsm_error = error;
		if (error != 0) {
			if (error == EOPNOTSUPP)
				result = error;
			continue;
		}
		mirrors[i].bsm_flags |= BTRFS_SUPER_MIRROR_VALID;
		mirrors[i].bsm_generation =
		    letoh64(candidates[i].bsc_super.generation);
		nvalid++;
	}

	if (nvalid == 0 && result == EOPNOTSUPP) {
		for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
			if ((mirrors[i].bsm_flags &
			    BTRFS_SUPER_MIRROR_READABLE) == 0 ||
			    mirrors[i].bsm_error != EOPNOTSUPP)
				continue;
			sb = &candidates[i].bsc_super;
			if (letoh16(sb->csum_type) !=
			    BTRFS_CSUM_TYPE_CRC32) {
				printf("btrfs: unsupported checksum type %u\n",
				    letoh16(sb->csum_type));
			} else if (letoh64(sb->num_devices) != 1) {
				printf("btrfs: unsupported device count %llu\n",
				    (unsigned long long)
				    letoh64(sb->num_devices));
			} else {
				printf("btrfs: unsupported superblock format\n");
			}
			break;
		}
	}
	return (nvalid == 0 ? result : 0);
}

int
btrfs_super_same_filesystem(const struct btrfs_super_block *a,
    const struct btrfs_super_block *b)
{
	if (memcmp(a->fsid, b->fsid, BTRFS_UUID_SIZE) != 0 ||
	    letoh64(a->dev_item.devid) != letoh64(b->dev_item.devid) ||
	    memcmp(a->dev_item.uuid, b->dev_item.uuid,
	    BTRFS_UUID_SIZE) != 0)
		return (0);
	return (1);
}

int
btrfs_check_super_policy(const struct btrfs_super_block *sb, int readonly)
{
	uint64_t incompat, unsupported;

	incompat = letoh64(sb->incompat_flags);
	unsupported = incompat & ~BTRFS_FEATURE_INCOMPAT_KNOWN;
	if (unsupported != 0) {
		printf("btrfs: unknown incompat features 0x%llx\n",
		    (unsigned long long)unsupported);
		return (EOPNOTSUPP);
	}
	unsupported = incompat & ~BTRFS_FEATURE_INCOMPAT_SUPPORTED;
	if (unsupported != 0) {
		printf("btrfs: unsupported incompat features 0x%llx\n",
		    (unsigned long long)unsupported);
		return (EOPNOTSUPP);
	}

	if (!readonly) {
		unsupported =
		    incompat & ~BTRFS_FEATURE_INCOMPAT_WRITE_SUPPORTED;
		if (unsupported != 0) {
			printf("btrfs: unsupported write incompat features "
			    "0x%llx\n", (unsigned long long)unsupported);
			return (EOPNOTSUPP);
		}
		if ((incompat & BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA) == 0) {
			printf("btrfs: writable mounts require skinny metadata\n");
			return (EOPNOTSUPP);
		}
		/*
		 * No compat-ro feature is supported for writes yet.  Unknown
		 * compat-ro bits are intentionally harmless on read-only
		 * mounts by the format's definition.
		 */
		if (letoh64(sb->compat_ro_flags) &
		    ~BTRFS_FEATURE_COMPAT_RO_WRITE_SUPPORTED)
			return (EROFS);
		if (letoh64(sb->log_root) != 0)
			return (EROFS);
		if (letoh64(sb->flags) & BTRFS_SUPER_FLAG_SEEDING)
			return (EROFS);
	}

	return (0);
}

int
btrfs_bootstrap_super(struct vnode *devvp,
    const struct btrfs_super_block *sb,
    struct btrfs_bootstrap *bootstrap)
{
	struct btrfs_inode inode;
	struct btrfs_root_item fs_root_item;
	struct btrfs_chunk_map *chunks = NULL;
	struct btrfs_chunk_map *system_chunks = NULL;
	struct btrfs_root chunk_tree, csum_tree, fs_tree, root_tree;
	struct btrfs_root_location block_group_root = { 0 };
	struct btrfs_root_location csum_root = { 0 };
	struct btrfs_root_location dev_root = { 0 };
	struct btrfs_root_location extent_root = { 0 };
	struct btrfs_root_location free_space_root = { 0 };
	struct btrfs_io_map map;
	struct btrfs_extent_buffer *eb = NULL;
	const struct btrfs_header *header;
	uint8_t chunk_tree_uuid[BTRFS_UUID_SIZE];
	uint64_t chunk_root, generation, root;
	uint64_t compat_ro;
	uint32_t nodesize;
	unsigned int nchunks = 0, nsystem_chunks = 0;
	int error;

	memset(bootstrap, 0, sizeof(*bootstrap));
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
	chunk_tree.br_view_generation = generation;
	chunk_tree.br_owner = BTRFS_CHUNK_TREE_OBJECTID;
	chunk_tree.br_level = sb->chunk_root_level;
	error = btrfs_extent_buffer_read(&chunk_tree, chunk_tree.br_bytenr,
	    chunk_tree.br_generation, chunk_tree.br_view_generation,
	    chunk_tree.br_level, &eb);
	if (error != 0)
		goto out;
	header = btrfs_extent_buffer_data(eb);
	memcpy(chunk_tree_uuid, header->chunk_tree_uuid,
	    sizeof(chunk_tree_uuid));
	btrfs_extent_buffer_put(eb);
	eb = NULL;
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
	root_tree.br_view_generation = generation;
	root_tree.br_owner = BTRFS_ROOT_TREE_OBJECTID;
	root_tree.br_level = sb->root_level;
	error = btrfs_find_root_item(&root_tree, BTRFS_FS_TREE_OBJECTID,
	    BTRFS_FIRST_FREE_OBJECTID, &fs_root_item);
	if (error != 0)
		goto out;
	error = btrfs_load_root_location(&root_tree,
	    BTRFS_CSUM_TREE_OBJECTID, &csum_root);
	if (error != 0)
		goto out;
	error = btrfs_load_root_location(&root_tree,
	    BTRFS_EXTENT_TREE_OBJECTID, &extent_root);
	if (error != 0)
		goto out;
	error = btrfs_load_root_location(&root_tree, BTRFS_DEV_TREE_OBJECTID,
	    &dev_root);
	if (error != 0)
		goto out;

	compat_ro = letoh64(sb->compat_ro_flags);
	if (compat_ro & BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE) {
		error = btrfs_load_root_location(&root_tree,
		    BTRFS_FREE_SPACE_TREE_OBJECTID, &free_space_root);
		if (error != 0)
			goto out;
	}
	if (compat_ro & BTRFS_FEATURE_COMPAT_RO_BLOCK_GROUP_TREE) {
		error = btrfs_load_root_location(&root_tree,
		    BTRFS_BLOCK_GROUP_TREE_OBJECTID, &block_group_root);
		if (error != 0)
			goto out;
	}

	memset(&csum_tree, 0, sizeof(csum_tree));
	csum_tree.br_devvp = devvp;
	csum_tree.br_super = sb;
	csum_tree.br_chunks = chunks;
	csum_tree.br_nchunks = nchunks;
	csum_tree.br_bytenr = csum_root.brl_bytenr;
	csum_tree.br_generation = csum_root.brl_generation;
	csum_tree.br_view_generation = generation;
	csum_tree.br_owner = BTRFS_CSUM_TREE_OBJECTID;
	csum_tree.br_level = csum_root.brl_level;
	error = btrfs_extent_buffer_read(&csum_tree, csum_tree.br_bytenr,
	    csum_tree.br_generation, csum_tree.br_view_generation,
	    csum_tree.br_level, &eb);
	if (error != 0)
		goto out;
	btrfs_extent_buffer_put(eb);
	eb = NULL;

	memset(&fs_tree, 0, sizeof(fs_tree));
	fs_tree.br_devvp = devvp;
	fs_tree.br_super = sb;
	fs_tree.br_chunks = chunks;
	fs_tree.br_nchunks = nchunks;
	fs_tree.br_bytenr = letoh64(fs_root_item.bytenr);
	fs_tree.br_generation = letoh64(fs_root_item.generation);
	fs_tree.br_view_generation = generation;
	fs_tree.br_owner = BTRFS_FS_TREE_OBJECTID;
	fs_tree.br_level = fs_root_item.level;
	error = btrfs_find_inode(&fs_tree, BTRFS_FIRST_FREE_OBJECTID, &inode);
	if (error != 0)
		goto out;
	error = btrfs_iterate_directory(&fs_tree, BTRFS_FIRST_FREE_OBJECTID,
	    NULL, NULL);
	if (error != 0)
		goto out;

	bootstrap->bb_chunks = chunks;
	bootstrap->bb_nchunks = nchunks;
	bootstrap->bb_extent_root = extent_root;
	bootstrap->bb_dev_root = dev_root;
	bootstrap->bb_free_space_root = free_space_root;
	bootstrap->bb_block_group_root = block_group_root;
	bootstrap->bb_fs_root = letoh64(fs_root_item.bytenr);
	bootstrap->bb_fs_root_generation =
	    letoh64(fs_root_item.generation);
	bootstrap->bb_fs_root_flags = letoh64(fs_root_item.flags);
	bootstrap->bb_fs_root_level = fs_root_item.level;
	bootstrap->bb_csum_root = csum_root.brl_bytenr;
	bootstrap->bb_csum_root_generation =
	    csum_root.brl_generation;
	bootstrap->bb_csum_root_level = csum_root.brl_level;
	memcpy(bootstrap->bb_chunk_tree_uuid, chunk_tree_uuid,
	    sizeof(bootstrap->bb_chunk_tree_uuid));
	chunks = NULL;
	error = 0;
out:
	if (eb != NULL)
		btrfs_extent_buffer_put(eb);
	if (system_chunks != NULL)
		free(system_chunks, M_BTRFS,
		    nsystem_chunks * sizeof(*system_chunks));
	if (chunks != NULL)
		free(chunks, M_BTRFS, nchunks * sizeof(*chunks));
	return (error);
}

uint8_t
btrfs_validate_backup_roots(const struct btrfs_super_block *sb)
{
	const struct btrfs_root_backup *backup;
	uint64_t bytes_used, generation, num_devices, total_bytes;
	uint32_t sectorsize;
	uint8_t valid = 0;
	unsigned int i;

	sectorsize = letoh32(sb->sectorsize);
	for (i = 0; i < BTRFS_NUM_BACKUP_ROOTS; i++) {
		backup = &sb->super_roots[i];
		generation = letoh64(backup->tree_root_gen);
		total_bytes = letoh64(backup->total_bytes);
		bytes_used = letoh64(backup->bytes_used);
		num_devices = letoh64(backup->num_devices);

		if (generation == 0 ||
		    generation > letoh64(sb->generation) ||
		    letoh64(backup->chunk_root_gen) == 0 ||
		    letoh64(backup->chunk_root_gen) >
		    letoh64(sb->generation) ||
		    letoh64(backup->fs_root_gen) == 0 ||
		    letoh64(backup->fs_root_gen) > letoh64(sb->generation) ||
		    letoh64(backup->csum_root_gen) == 0 ||
		    letoh64(backup->csum_root_gen) >
		    letoh64(sb->generation) ||
		    letoh64(backup->tree_root) == 0 ||
		    (letoh64(backup->tree_root) & (sectorsize - 1)) != 0 ||
		    letoh64(backup->chunk_root) == 0 ||
		    (letoh64(backup->chunk_root) & (sectorsize - 1)) != 0 ||
		    letoh64(backup->fs_root) == 0 ||
		    (letoh64(backup->fs_root) & (sectorsize - 1)) != 0 ||
		    letoh64(backup->csum_root) == 0 ||
		    (letoh64(backup->csum_root) & (sectorsize - 1)) != 0 ||
		    backup->tree_root_level >= BTRFS_MAX_LEVEL ||
		    backup->chunk_root_level >= BTRFS_MAX_LEVEL ||
		    backup->fs_root_level >= BTRFS_MAX_LEVEL ||
		    backup->csum_root_level >= BTRFS_MAX_LEVEL ||
		    total_bytes == 0 || bytes_used > total_bytes ||
		    num_devices != 1)
			continue;
		valid |= 1U << i;
	}

	return (valid);
}

static int
btrfs_snapshot_root(struct btrfs_transaction *trans, uint64_t owner,
    struct btrfs_root_location *location)
{
	struct btrfs_root *root;
	uint32_t sectorsize;
	int error;

	error = btrfs_get_root(trans->bt_mount, owner, &root);
	if (error != 0)
		return (error);
	rw_enter_read(root->br_lock);
	location->brl_bytenr = root->br_bytenr;
	location->brl_generation = root->br_generation;
	location->brl_level = root->br_level;
	if (root->br_transaction != NULL &&
	    root->br_transaction != trans)
		error = EBUSY;
	rw_exit_read(root->br_lock);
	sectorsize = letoh32(trans->bt_mount->bm_super.sectorsize);
	if (error == 0 &&
	    (location->brl_bytenr == 0 ||
	    (location->brl_bytenr & (sectorsize - 1)) != 0 ||
	    location->brl_generation == 0 ||
	    location->brl_generation > trans->bt_generation ||
	    location->brl_level >= BTRFS_MAX_LEVEL))
		error = EINVAL;
	return (error);
}

static void
btrfs_set_super_csum(struct btrfs_super_block *sb)
{
	uint32_t csum;

	memset(sb->csum, 0, sizeof(sb->csum));
	csum = htole32(crc32c(0,
	    (const uint8_t *)sb + sizeof(sb->csum),
	    sizeof(*sb) - sizeof(sb->csum)));
	memcpy(sb->csum, &csum, sizeof(csum));
}

int
btrfs_build_super(struct btrfs_transaction *trans,
    struct btrfs_super_block *sb)
{
	struct btrfs_fs *bmp;
	struct btrfs_root_backup *backup;
	struct btrfs_root_location csum, dev, extent, fs, root, chunk;
	unsigned int slot;
	int error;

	if (trans == NULL || sb == NULL)
		return (EINVAL);
	bmp = trans->bt_mount;
	if (bmp == NULL || trans->bt_generation == 0 ||
	    trans->bt_bytes_used > letoh64(bmp->bm_super.total_bytes))
		return (EINVAL);
	mtx_enter(&bmp->bm_trans_mtx);
	if (bmp->bm_transaction != trans || !bmp->bm_committer ||
	    trans->bt_state != BTRFS_TRANS_COMMITTING ||
	    trans->bt_writers != 0 || trans->bt_commit_handle)
		error = trans->bt_error != 0 ? trans->bt_error : EINVAL;
	else
		error = 0;
	mtx_leave(&bmp->bm_trans_mtx);
	if (error != 0)
		return (error);
	error = btrfs_snapshot_root(trans, BTRFS_ROOT_TREE_OBJECTID, &root);
	if (error == 0)
		error = btrfs_snapshot_root(trans,
		    BTRFS_CHUNK_TREE_OBJECTID, &chunk);
	if (error == 0)
		error = btrfs_snapshot_root(trans,
		    BTRFS_EXTENT_TREE_OBJECTID, &extent);
	if (error == 0)
		error = btrfs_snapshot_root(trans, BTRFS_FS_TREE_OBJECTID, &fs);
	if (error == 0)
		error = btrfs_snapshot_root(trans, BTRFS_DEV_TREE_OBJECTID,
		    &dev);
	if (error == 0)
		error = btrfs_snapshot_root(trans, BTRFS_CSUM_TREE_OBJECTID,
		    &csum);
	if (error != 0)
		return (error);
	if (root.brl_generation != trans->bt_generation)
		return (EINVAL);

	memcpy(sb, &bmp->bm_super, sizeof(*sb));
	sb->generation = htole64(trans->bt_generation);
	sb->root = htole64(root.brl_bytenr);
	sb->root_level = root.brl_level;
	sb->chunk_root = htole64(chunk.brl_bytenr);
	sb->chunk_root_generation = htole64(chunk.brl_generation);
	sb->chunk_root_level = chunk.brl_level;
	sb->bytes_used = htole64(trans->bt_bytes_used);
	sb->log_root = 0;
	sb->__unused_log_root_transid = 0;
	sb->log_root_level = 0;

	slot = trans->bt_generation % BTRFS_NUM_BACKUP_ROOTS;
	backup = &sb->super_roots[slot];
	memset(backup, 0, sizeof(*backup));
	backup->tree_root = htole64(root.brl_bytenr);
	backup->tree_root_gen = htole64(root.brl_generation);
	backup->chunk_root = htole64(chunk.brl_bytenr);
	backup->chunk_root_gen = htole64(chunk.brl_generation);
	backup->extent_root = htole64(extent.brl_bytenr);
	backup->extent_root_gen = htole64(extent.brl_generation);
	backup->fs_root = htole64(fs.brl_bytenr);
	backup->fs_root_gen = htole64(fs.brl_generation);
	backup->dev_root = htole64(dev.brl_bytenr);
	backup->dev_root_gen = htole64(dev.brl_generation);
	backup->csum_root = htole64(csum.brl_bytenr);
	backup->csum_root_gen = htole64(csum.brl_generation);
	backup->total_bytes = sb->total_bytes;
	backup->bytes_used = sb->bytes_used;
	backup->num_devices = sb->num_devices;
	backup->tree_root_level = root.brl_level;
	backup->chunk_root_level = chunk.brl_level;
	backup->extent_root_level = extent.brl_level;
	backup->fs_root_level = fs.brl_level;
	backup->dev_root_level = dev.brl_level;
	backup->csum_root_level = csum.brl_level;
	btrfs_set_super_csum(sb);
	return (btrfs_validate_super(sb, letoh64(sb->bytenr)));
}

/*
 * The filesystem can occupy only a prefix of its device.  A readable sector
 * outside that prefix is not a superblock mirror of this filesystem.
 */
int
btrfs_super_mirror_writable(const struct btrfs_fs *bmp, unsigned int index)
{
	const struct btrfs_super_mirror *mirror;
	uint64_t size;

	KASSERT(index < BTRFS_SUPER_MIRROR_MAX);
	mirror = &bmp->bm_super_mirrors[index];
	size = letoh64(bmp->bm_super.dev_item.total_bytes);
	return ((mirror->bsm_flags & BTRFS_SUPER_MIRROR_READABLE) != 0 &&
	    mirror->bsm_bytenr <= size &&
	    sizeof(struct btrfs_super_block) <= size - mirror->bsm_bytenr);
}

int
btrfs_write_super_mirrors(struct btrfs_fs *bmp,
    const struct btrfs_super_block *template)
{
	struct btrfs_super_block *sb;
	struct btrfs_super_mirror *mirror;
	struct buf *bp;
	unsigned int i, nwritten = 0;
	int error, first_error = 0;

	if (bmp == NULL || template == NULL)
		return (EINVAL);
	sb = malloc(sizeof(*sb), M_BTRFS, M_WAITOK);
	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		mirror = &bmp->bm_super_mirrors[i];
		if (!btrfs_super_mirror_writable(bmp, i))
			continue;
		memcpy(sb, template, sizeof(*sb));
		sb->bytenr = htole64(mirror->bsm_bytenr);
		btrfs_set_super_csum(sb);
		error = btrfs_validate_super(sb, mirror->bsm_bytenr);
		if (error != 0) {
			mirror->bsm_error = error;
			if (first_error == 0)
				first_error = error;
			continue;
		}
		bp = getblk(bmp->bm_devvp,
		    mirror->bsm_bytenr / DEV_BSIZE, sizeof(*sb), 0, INFSLP);
		memcpy(bp->b_data, sb, sizeof(*sb));
		SET(bp->b_flags, B_NOCACHE);
		error = bwrite(bp);
		mirror->bsm_error = error;
		if (error == 0)
			nwritten++;
		else if (first_error == 0)
			first_error = error;
	}
	free(sb, M_BTRFS, sizeof(*sb));
	if (nwritten == 0 && first_error == 0)
		return (ENXIO);
	return (first_error);
}

static int
btrfs_validate_super(const struct btrfs_super_block *sb, uint64_t bytenr)
{
	uint64_t bytes_used, chunk_generation, chunk_root, generation;
	uint64_t log_root, root, total_bytes;
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
	if (letoh64(sb->dev_item.total_bytes) > total_bytes ||
	    letoh64(sb->dev_item.bytes_used) >
	    letoh64(sb->dev_item.total_bytes) ||
	    letoh32(sb->dev_item.sector_size) != sectorsize)
		return (EINVAL);

	generation = letoh64(sb->generation);
	chunk_generation = letoh64(sb->chunk_root_generation);
	root = letoh64(sb->root);
	chunk_root = letoh64(sb->chunk_root);
	log_root = letoh64(sb->log_root);
	if (generation == 0 || chunk_generation == 0 ||
	    chunk_generation > generation ||
	    root == 0 || (root & (sectorsize - 1)) != 0 ||
	    chunk_root == 0 || (chunk_root & (sectorsize - 1)) != 0)
		return (EINVAL);
	if (log_root != 0 && (log_root & (sectorsize - 1)) != 0)
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

		error = btrfs_decode_chunk_item(sb, key, chunk,
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
		error = btrfs_decode_chunk_item(sb, key, chunk,
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

int
btrfs_decode_chunk_item(const struct btrfs_super_block *sb,
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
	    letoh64(chunk->owner) != BTRFS_EXTENT_TREE_OBJECTID ||
	    (type & BTRFS_BLOCK_GROUP_TYPE_MASK) == 0 ||
	    (logical & (sectorsize - 1)) != 0 ||
	    (chunk_len & (sectorsize - 1)) != 0 ||
	    stripe_len == 0 || !btrfs_ispow2(stripe_len) ||
	    (stripe_len & (sectorsize - 1)) != 0 ||
	    letoh32(chunk->sector_size) != sectorsize)
		return (EINVAL);
	if (type & ~(BTRFS_BLOCK_GROUP_TYPE_MASK |
	    BTRFS_BLOCK_GROUP_PROFILE_MASK)) {
		printf("btrfs: unsupported chunk type 0x%llx\n",
		    (unsigned long long)type);
		return (EOPNOTSUPP);
	}

	if ((profile == 0 && nstripes != 1) ||
	    (profile == BTRFS_BLOCK_GROUP_DUP && nstripes != 2)) {
		printf("btrfs: invalid stripe count %u for chunk profile "
		    "0x%llx\n", nstripes, (unsigned long long)profile);
		return (EOPNOTSUPP);
	}
	if (profile != 0 && profile != BTRFS_BLOCK_GROUP_DUP) {
		printf("btrfs: unsupported chunk profile 0x%llx\n",
		    (unsigned long long)profile);
		return (EOPNOTSUPP);
	}

	map->logical = logical;
	map->length = chunk_len;
	map->type = type;
	map->owner = letoh64(chunk->owner);
	map->stripe_len = stripe_len;
	map->io_align = letoh32(chunk->io_align);
	map->io_width = letoh32(chunk->io_width);
	map->sector_size = letoh32(chunk->sector_size);
	map->sub_stripes = letoh16(chunk->sub_stripes);
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
		map->devid[i] = letoh64(stripe->devid);
		memcpy(map->dev_uuid[i], stripe->dev_uuid,
		    sizeof(map->dev_uuid[i]));
	}

	return (0);
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

		error = btrfs_decode_chunk_item(sb, key,
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

		error = btrfs_lookup_logical(&chunks[chunk_index], 1,
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
btrfs_load_root_location(struct btrfs_root *root_tree, uint64_t objectid,
    struct btrfs_root_location *location)
{
	struct btrfs_root_item item;
	int error;

	memset(location, 0, sizeof(*location));
	error = btrfs_find_root_item(root_tree, objectid, 0, &item);
	if (error != 0)
		return (error);
	location->brl_bytenr = letoh64(item.bytenr);
	location->brl_generation = letoh64(item.generation);
	location->brl_level = item.level;
	return (0);
}

int
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
btrfs_ispow2(uint32_t value)
{
	return (value != 0 && (value & (value - 1)) == 0);
}
