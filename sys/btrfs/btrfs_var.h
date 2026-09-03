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

#ifndef _BTRFS_VAR_H_
#define _BTRFS_VAR_H_

#include <sys/types.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/rwlock.h>

#include <btrfs/btrfs.h>

struct btrfs_chunk_map {
	uint64_t	logical;
	uint64_t	length;
	uint64_t	type;
	uint64_t	physical[2];
	unsigned int	nmirrors;
};

struct btrfs_dir_entry {
	const uint8_t	*bde_name;
	uint64_t	 bde_objectid;
	uint64_t	 bde_index;
	uint16_t	 bde_namelen;
	uint8_t		 bde_type;
	int		 bde_subvolume;
};

typedef int (*btrfs_dir_iter_fn)(const struct btrfs_dir_entry *, void *);

struct buf;
struct btrfs_node;
LIST_HEAD(btrfs_node_list, btrfs_node);

struct btrfs_mount {
	struct mount			*bm_mount;
	struct vnode			*bm_devvp;
	dev_t				 bm_dev;
	struct btrfs_super_block	 bm_super;
	struct btrfs_chunk_map		*bm_chunks;
	unsigned int			 bm_nchunks;
	uint64_t			 bm_treeid;
	uint64_t			 bm_fs_root;
	uint64_t			 bm_fs_root_generation;
	uint64_t			 bm_csum_root;
	uint64_t			 bm_csum_root_generation;
	uint64_t			 bm_root_dirid;
	uint8_t				 bm_fs_root_level;
	uint8_t				 bm_csum_root_level;
	struct btrfs_node_list		 bm_nodes;
	struct mutex			 bm_nodemtx;
};

struct btrfs_node {
	LIST_ENTRY(btrfs_node)		 bn_entry;
	struct vnode			*bn_vnode;
	struct btrfs_mount		*bn_mount;
	struct rrwlock			 bn_lock;
	uint64_t			 bn_treeid;
	uint64_t			 bn_ino;
	int				 bn_hashed;
	struct btrfs_inode_item		 bn_inode;
};

#define VFSTOBTRFS(mp)	((struct btrfs_mount *)(mp)->mnt_data)
#define VTOBTRFS(vp)	((struct btrfs_node *)(vp)->v_data)

extern const struct vops btrfs_vops;

int	btrfs_iterate_directory(const struct btrfs_super_block *,
	    const struct btrfs_header *, uint64_t, btrfs_dir_iter_fn, void *);
int	btrfs_lookup_data_csum(struct btrfs_mount *,
	    const struct btrfs_header *, uint64_t, uint32_t *);
int	btrfs_read_csum_tree_root(struct btrfs_mount *, struct buf **);
int	btrfs_read_data_block(struct btrfs_mount *, uint64_t,
	    const uint32_t *, struct buf **);
int	btrfs_read_fs_tree_root(struct btrfs_mount *, struct buf **);
int	btrfs_vget(struct mount *, ino_t, struct vnode **);

#endif
