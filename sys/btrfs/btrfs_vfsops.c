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
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/specdev.h>
#include <sys/vnode.h>

#include <lib/libkern/crc32c.h>

#include <btrfs/btrfs.h>

#define BTRFS_SUPER_SIZE		0x1000
#define BTRFS_MIN_SECTORSIZE		0x1000
#define BTRFS_MAX_SECTORSIZE		0x10000
#define BTRFS_MAX_LEVEL			8

static int	btrfs_mount(struct mount *, const char *, void *,
		    struct nameidata *, struct proc *);
static int	btrfs_probe(struct vnode *, const char *, struct proc *);
static int	btrfs_validate_super(const struct btrfs_super_block *,
		    uint64_t);
static int	btrfs_ispow2(uint32_t);

const struct vfsops btrfs_vfsops = {
	.vfs_mount	= btrfs_mount,
	.vfs_start	= (void *)eopnotsupp,
	.vfs_unmount	= (void *)eopnotsupp,
	.vfs_root	= (void *)eopnotsupp,
	.vfs_quotactl	= (void *)eopnotsupp,
	.vfs_statfs	= (void *)eopnotsupp,
	.vfs_sync	= (void *)eopnotsupp,
	.vfs_vget	= (void *)eopnotsupp,
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

	(void)path;

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
		error = btrfs_probe(devvp, fspec, p);

	vrele(devvp);
	return (error);
}

static int
btrfs_probe(struct vnode *devvp, const char *fspec, struct proc *p)
{
	const struct btrfs_super_block *sb;
	struct buf *bp = NULL;
	uint64_t generation;
	uint32_t nodesize, sectorsize;
	int error;

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

	/* Tree and vnode support is the next implementation milestone. */
	error = EOPNOTSUPP;
out:
	if (bp != NULL)
		brelse(bp);
	vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
	(void)VOP_CLOSE(devvp, FREAD, FSCRED, p);
	VOP_UNLOCK(devvp);
	return (error);
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
	    bytes_used > total_bytes || letoh64(sb->num_devices) == 0)
		return (EINVAL);

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
btrfs_ispow2(uint32_t value)
{
	return (value != 0 && (value & (value - 1)) == 0);
}
