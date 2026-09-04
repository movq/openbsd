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

#define BTRFS_MAX_LEVEL		8
#define BTRFS_MAX_COMPRESSED	(128 * 1024)
#define BTRFS_MAX_UNCOMPRESSED	(128 * 1024)

struct btrfs_chunk_map {
	uint64_t	logical;
	uint64_t	length;
	uint64_t	type;
	uint64_t	physical[2];
	unsigned int	nmirrors;
};

struct btrfs_io_map {
	uint64_t	physical[2];
	uint64_t	type;
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

/*
 * Decoded file extents point into the leaf held by the caller's path.
 * Inline data therefore remains valid until that path is advanced or released.
 */
#define BTRFS_FILE_EXTENT_HOLE	3

struct btrfs_file_extent {
	const uint8_t	*bfe_inline_data;
	uint64_t	 bfe_logical;
	uint64_t	 bfe_length;
	uint64_t	 bfe_disk_bytenr;
	uint64_t	 bfe_disk_num_bytes;
	uint64_t	 bfe_disk_offset;
	uint64_t	 bfe_ram_bytes;
	size_t		 bfe_inline_size;
	uint16_t	 bfe_other_encoding;
	uint8_t		 bfe_compression;
	uint8_t		 bfe_encryption;
	uint8_t		 bfe_type;
};

struct buf;
struct btrfs_node;
struct proc;
struct vnode;
LIST_HEAD(btrfs_node_list, btrfs_node);

struct btrfs_root {
	struct vnode			*br_devvp;
	const struct btrfs_super_block	*br_super;
	const struct btrfs_chunk_map	*br_chunks;
	unsigned int			 br_nchunks;
	uint64_t			 br_bytenr;
	uint64_t			 br_generation;
	uint64_t			 br_owner;
	uint8_t				 br_level;
};

struct btrfs_path {
	struct btrfs_root	*bp_root;
	struct buf		*bp_buf[BTRFS_MAX_LEVEL];
	uint32_t		 bp_slot[BTRFS_MAX_LEVEL];
};

#define BTRFS_SUPER_MIRROR_READABLE	0x01
#define BTRFS_SUPER_MIRROR_VALID	0x02
#define BTRFS_SUPER_MIRROR_CONSISTENT	0x04
#define BTRFS_SUPER_MIRROR_SELECTED	0x08
#define BTRFS_SUPER_MIRROR_STALE	0x10
#define BTRFS_SUPER_MIRROR_FOREIGN	0x20

struct btrfs_super_mirror {
	uint64_t	bsm_bytenr;
	uint64_t	bsm_generation;
	int		bsm_error;
	uint8_t		bsm_flags;
};

struct btrfs_super_candidate {
	struct btrfs_super_block	 bsc_super;
	int			 bsc_tried;
};

struct btrfs_bootstrap {
	struct btrfs_chunk_map	*bb_chunks;
	unsigned int		 bb_nchunks;
	uint64_t		 bb_fs_root;
	uint64_t		 bb_fs_root_generation;
	uint64_t		 bb_csum_root;
	uint64_t		 bb_csum_root_generation;
	uint64_t		 bb_fs_root_flags;
	uint8_t			 bb_fs_root_level;
	uint8_t			 bb_csum_root_level;
};

struct btrfs_mount {
	struct mount			*bm_mount;
	struct vnode			*bm_devvp;
	dev_t				 bm_dev;
	struct btrfs_super_block	 bm_super;
	struct btrfs_super_mirror	 bm_super_mirrors[
					    BTRFS_SUPER_MIRROR_MAX];
	unsigned int			 bm_selected_super;
	uint8_t				 bm_backup_roots_valid;
	uint8_t				 bm_seeding;
	uint8_t				 bm_subvol_readonly;
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

int	btrfs_read_super_mirrors(struct vnode *, struct proc *,
	    struct btrfs_super_candidate *, struct btrfs_super_mirror *);
int	btrfs_super_same_filesystem(const struct btrfs_super_block *,
	    const struct btrfs_super_block *);
int	btrfs_check_super_policy(const struct btrfs_super_block *, int);
int	btrfs_bootstrap_super(struct vnode *, const struct btrfs_super_block *,
	    int, struct btrfs_bootstrap *);
uint8_t	btrfs_validate_backup_roots(const struct btrfs_super_block *);
void	btrfs_init_root_tree(struct btrfs_mount *, struct btrfs_root *);
int	btrfs_init_fs_root(struct btrfs_mount *, uint64_t,
	    struct btrfs_root *);
void	btrfs_init_csum_root(struct btrfs_mount *, struct btrfs_root *);
int	btrfs_find_root_item(struct btrfs_root *, uint64_t, uint64_t,
	    struct btrfs_root_item *);
int	btrfs_lookup_logical(const struct btrfs_chunk_map *, unsigned int,
	    uint64_t, uint32_t, struct btrfs_io_map *);
int	btrfs_read_root_block(const struct btrfs_root *, uint64_t, uint64_t,
	    uint8_t, struct buf **);
/*
 * An exact miss leaves path at the insertion point.  Paths must initially
 * be zeroed and retain item pointers until advanced or released.
 */
int	btrfs_search_slot(struct btrfs_root *, const struct btrfs_key *,
	    struct btrfs_path *);
int	btrfs_search_lower_bound(struct btrfs_root *,
	    const struct btrfs_key *, struct btrfs_path *);
int	btrfs_search_predecessor(struct btrfs_root *,
	    const struct btrfs_key *, struct btrfs_path *);
int	btrfs_next_item(struct btrfs_path *);
int	btrfs_prev_item(struct btrfs_path *);
int	btrfs_path_item(const struct btrfs_path *, const struct btrfs_key **,
	    const uint8_t **, uint32_t *);
void	btrfs_release_path(struct btrfs_path *);
int	btrfs_find_inode_item(struct btrfs_root *, uint64_t,
	    struct btrfs_inode_item *);
int	btrfs_find_dir_parent(struct btrfs_root *, uint64_t, uint64_t *);
int	btrfs_find_subvol_parent(struct btrfs_mount *, uint64_t, uint64_t *,
	    uint64_t *);
int	btrfs_iterate_directory(struct btrfs_root *, uint64_t,
	    btrfs_dir_iter_fn, void *);
int	btrfs_find_file_extent(const struct btrfs_mount *,
	    struct btrfs_root *, struct btrfs_path *, uint64_t, uint64_t,
	    uint64_t, struct btrfs_file_extent *);
int	btrfs_lookup_data_csum(struct btrfs_mount *, uint64_t, uint32_t *);
int	btrfs_read_data_block(struct btrfs_mount *, uint64_t,
	    const uint32_t *, struct buf **);
int	btrfs_read_compressed_extent(struct btrfs_node *,
	    const struct btrfs_file_extent *, size_t, struct uio *);
int	btrfs_vget(struct mount *, ino_t, struct vnode **);
int	btrfs_vget_tree(struct mount *, uint64_t, uint64_t, struct vnode **);

#endif
