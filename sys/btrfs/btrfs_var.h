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
#include <sys/time.h>

#include <btrfs/btrfs.h>

#define BTRFS_MAX_LEVEL		8
#define BTRFS_MAX_COMPRESSED	(128 * 1024)
#define BTRFS_MAX_UNCOMPRESSED	(128 * 1024)
#define BTRFS_MAX_MIRRORS	2

struct btrfs_chunk_map {
	uint64_t	logical;
	uint64_t	length;
	uint64_t	type;
	uint64_t	owner;
	uint64_t	stripe_len;
	uint64_t	physical[BTRFS_MAX_MIRRORS];
	uint64_t	devid[BTRFS_MAX_MIRRORS];
	uint32_t	io_align;
	uint32_t	io_width;
	uint32_t	sector_size;
	uint16_t	sub_stripes;
	unsigned int	nmirrors;
	uint8_t		dev_uuid[BTRFS_MAX_MIRRORS][BTRFS_UUID_SIZE];
};

struct btrfs_io_map {
	uint64_t	physical[BTRFS_MAX_MIRRORS];
	uint64_t	type;
	unsigned int	nmirrors;
};

/*
 * Reads attempt mirrors through bir_mirror and select that copy.  Writes leave
 * bir_mirror at -1, attempt all bir_nmirrors copies, and record every result.
 */
struct btrfs_io_result {
	int		bir_error[BTRFS_MAX_MIRRORS];
	unsigned int	bir_nmirrors;
	int		bir_mirror;
};

typedef int (*btrfs_io_validate_fn)(const void *, size_t, void *);

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
	uint8_t		 bfe_item_present;
};

struct btrfs_root_location {
	uint64_t	brl_bytenr;
	uint64_t	brl_generation;
	uint8_t		brl_level;
};

/*
 * Allocation iterators return host-endian records.  Any pointer in a record
 * refers to the current leaf and is valid only for the callback invocation.
 */
struct btrfs_extent_record {
	uint64_t	ber_bytenr;
	uint64_t	ber_length;
	uint64_t	ber_refs;
	uint64_t	ber_generation;
	uint64_t	ber_flags;
	uint64_t	ber_owner_root;
	uint64_t	ber_tree_objectid;
	uint64_t	ber_tree_offset;
	uint8_t		ber_tree_type;
	uint8_t		ber_level;
	uint8_t		ber_skinny;
	uint8_t		ber_legacy;
	uint8_t		ber_has_owner;
};

struct btrfs_backref_record {
	uint64_t	bbr_bytenr;
	uint64_t	bbr_root;
	uint64_t	bbr_objectid;
	uint64_t	bbr_offset;
	uint64_t	bbr_key_offset;
	uint64_t	bbr_parent;
	uint64_t	bbr_generation;
	uint32_t	bbr_count;
	uint8_t		bbr_type;
	uint8_t		bbr_inline;
};

struct btrfs_block_group_record {
	uint64_t	bbg_bytenr;
	uint64_t	bbg_length;
	uint64_t	bbg_used;
	uint64_t	bbg_chunk_objectid;
	uint64_t	bbg_flags;
};

struct btrfs_dev_extent_record {
	uint64_t	bde_devid;
	uint64_t	bde_physical;
	uint64_t	bde_length;
	uint64_t	bde_chunk_tree;
	uint64_t	bde_chunk_objectid;
	uint64_t	bde_chunk_offset;
};

#define BTRFS_FREE_SPACE_RECORD_INFO		1
#define BTRFS_FREE_SPACE_RECORD_EXTENT		2
#define BTRFS_FREE_SPACE_RECORD_BITMAP		3

struct btrfs_free_space_record {
	const uint8_t	*bfs_bitmap;
	uint64_t	 bfs_bytenr;
	uint64_t	 bfs_length;
	uint32_t	 bfs_extent_count;
	uint32_t	 bfs_flags;
	uint32_t	 bfs_bitmap_size;
	uint8_t		 bfs_type;
};

struct btrfs_free_extent {
	TAILQ_ENTRY(btrfs_free_extent)	 bfe_entry;
	uint64_t			 bfe_bytenr;
	uint64_t			 bfe_length;
};
TAILQ_HEAD(btrfs_free_extent_list, btrfs_free_extent);

struct btrfs_mount;
struct btrfs_root;
struct btrfs_transaction;
struct btrfs_extent_buffer;
TAILQ_HEAD(btrfs_dirty_extent_buffer_list, btrfs_extent_buffer);

/*
 * Delayed tree references describe the final reference delta for one metadata
 * extent and backreference identity.  Materialization decides whether a
 * dropped extent reached zero references and must enter pinned space.
 */
struct btrfs_delayed_tree_ref {
	TAILQ_ENTRY(btrfs_delayed_tree_ref) bdr_entry;
	uint64_t			 bdr_bytenr;
	uint64_t			 bdr_parent;
	uint64_t			 bdr_root;
	int64_t				 bdr_ref_mod;
	uint8_t				 bdr_level;
};
TAILQ_HEAD(btrfs_delayed_tree_ref_list, btrfs_delayed_tree_ref);

struct btrfs_delayed_data_ref {
	TAILQ_ENTRY(btrfs_delayed_data_ref) bdr_entry;
	uint64_t			 bdr_bytenr;
	uint64_t			 bdr_length;
	uint64_t			 bdr_root;
	uint64_t			 bdr_objectid;
	uint64_t			 bdr_offset;
	int64_t				 bdr_ref_mod;
};
TAILQ_HEAD(btrfs_delayed_data_ref_list, btrfs_delayed_data_ref);

struct btrfs_ordered_extent {
	TAILQ_ENTRY(btrfs_ordered_extent) boe_entry;
	void				*boe_data;
	uint64_t			 boe_treeid;
	uint64_t			 boe_objectid;
	uint64_t			 boe_file_offset;
	uint64_t			 boe_bytenr;
	uint32_t			 boe_length;
	uint8_t				 boe_written;
};
TAILQ_HEAD(btrfs_ordered_extent_list, btrfs_ordered_extent);

struct btrfs_dirty_root {
	TAILQ_ENTRY(btrfs_dirty_root)	 bdr_entry;
	struct btrfs_root		*bdr_root;
	struct btrfs_root_location	 bdr_old_location;
	uint64_t			 bdr_old_view_generation;
};
TAILQ_HEAD(btrfs_dirty_root_list, btrfs_dirty_root);

struct btrfs_block_group {
	struct mutex			 bbg_lock;
	struct btrfs_free_extent_list	 bbg_free_extents;
	uint64_t			 bbg_bytenr;
	uint64_t			 bbg_length;
	uint64_t			 bbg_disk_used;
	uint64_t			 bbg_free_bytes;
	uint64_t			 bbg_reserved_bytes;
	uint64_t			 bbg_allocated_bytes;
	uint64_t			 bbg_pinned_bytes;
	uint64_t			 bbg_flags;
	/* Scratch fields used only while constructing the mount-time index. */
	uint64_t			 bbg_build_cursor;
	uint64_t			 bbg_build_used;
};

struct btrfs_trans_reservation {
	uint64_t	btr_data;
	uint64_t	btr_metadata;
	uint64_t	btr_system;
};

struct btrfs_reserved_space {
	TAILQ_ENTRY(btrfs_reserved_space) brs_entry;
	struct btrfs_block_group		*brs_group;
	uint64_t			 brs_bytes;
	uint64_t			 brs_type;
};
TAILQ_HEAD(btrfs_reserved_space_list, btrfs_reserved_space);

struct btrfs_trans_extent {
	TAILQ_ENTRY(btrfs_trans_extent)	 bte_entry;
	struct btrfs_block_group		*bte_group;
	uint64_t			 bte_bytenr;
	uint64_t			 bte_length;
	uint64_t			 bte_type;
};
TAILQ_HEAD(btrfs_trans_extent_list, btrfs_trans_extent);

enum btrfs_trans_state {
	BTRFS_TRANS_OPEN,
	BTRFS_TRANS_CLOSING,
	BTRFS_TRANS_COMMITTING,
	BTRFS_TRANS_COMMITTED,
	BTRFS_TRANS_ABORTED
};

struct btrfs_transaction {
	struct btrfs_mount		*bt_mount;
	struct mutex			 bt_lock;
	struct btrfs_reserved_space_list bt_commit_reservations;
	struct btrfs_trans_extent_list	 bt_allocated_extents;
	struct btrfs_trans_extent_list	 bt_pinned_extents;
	struct btrfs_dirty_extent_buffer_list
					 bt_dirty_extent_buffers;
	struct btrfs_delayed_tree_ref_list
					 bt_delayed_tree_refs;
	struct btrfs_delayed_data_ref_list
					 bt_delayed_data_refs;
	struct btrfs_ordered_extent_list bt_ordered_extents;
	struct btrfs_dirty_root_list	 bt_dirty_roots;
	uint64_t			 bt_generation;
	uint64_t			 bt_commit_reserve_target;
	uint64_t			 bt_commit_reserved_bytes;
	uint64_t			 bt_allocated_bytes;
	uint64_t			 bt_pinned_bytes;
	uint64_t			 bt_bytes_used;
	uint64_t			 bt_space_seq;
	unsigned int			 bt_writers;
	int				 bt_error;
	enum btrfs_trans_state		 bt_state;
	uint8_t				 bt_commit_handle;
};

struct btrfs_trans_handle {
	struct btrfs_transaction	*bth_transaction;
	struct btrfs_reserved_space_list bth_reservations;
	uint8_t				 bth_commit;
};

typedef int (*btrfs_extent_iter_fn)(const struct btrfs_extent_record *,
		    void *);
typedef int (*btrfs_backref_iter_fn)(const struct btrfs_backref_record *,
		    void *);
typedef int (*btrfs_block_group_iter_fn)(
		    const struct btrfs_block_group_record *, void *);
typedef int (*btrfs_chunk_iter_fn)(const struct btrfs_chunk_map *, void *);
typedef int (*btrfs_dev_extent_iter_fn)(
		    const struct btrfs_dev_extent_record *, void *);
typedef int (*btrfs_free_space_iter_fn)(
		    const struct btrfs_free_space_record *, void *);

/*
 * Mutable inode state is host-endian.  Dirty fields and the transaction which
 * owns them are kept separate from the generation stored in the inode item.
 */
#define BTRFS_INODE_DIRTY_SIZE		0x00000001
#define BTRFS_INODE_DIRTY_NBYTES	0x00000002
#define BTRFS_INODE_DIRTY_NLINK		0x00000004
#define BTRFS_INODE_DIRTY_UID		0x00000008
#define BTRFS_INODE_DIRTY_GID		0x00000010
#define BTRFS_INODE_DIRTY_MODE		0x00000020
#define BTRFS_INODE_DIRTY_RDEV		0x00000040
#define BTRFS_INODE_DIRTY_FLAGS		0x00000080
#define BTRFS_INODE_DIRTY_SEQUENCE	0x00000100
#define BTRFS_INODE_DIRTY_ATIME		0x00000200
#define BTRFS_INODE_DIRTY_CTIME		0x00000400
#define BTRFS_INODE_DIRTY_MTIME		0x00000800
#define BTRFS_INODE_DIRTY_OTIME		0x00001000
#define BTRFS_INODE_DIRTY_ALL		0x00001fff

struct btrfs_inode {
	uint64_t	bi_generation;
	uint64_t	bi_transid;
	uint64_t	bi_size;
	uint64_t	bi_nbytes;
	uint64_t	bi_block_group;
	uint64_t	bi_rdev;
	uint64_t	bi_flags;
	uint64_t	bi_sequence;
	uint64_t	bi_last_dirty_transid;
	struct timespec	bi_atime;
	struct timespec	bi_ctime;
	struct timespec	bi_mtime;
	struct timespec	bi_otime;
	uint32_t	bi_nlink;
	uint32_t	bi_uid;
	uint32_t	bi_gid;
	uint32_t	bi_mode;
	uint32_t	bi_dirty_fields;
};

struct buf;
struct btrfs_node;
struct btrfs_root_entry;
struct proc;
struct vnode;
LIST_HEAD(btrfs_extent_buffer_list, btrfs_extent_buffer);
LIST_HEAD(btrfs_node_list, btrfs_node);
LIST_HEAD(btrfs_root_list, btrfs_root_entry);

struct btrfs_root {
	struct btrfs_mount		*br_mount;
	struct vnode			*br_devvp;
	const struct btrfs_super_block	*br_super;
	const struct btrfs_chunk_map	*br_chunks;
	struct rwlock			*br_lock;
	unsigned int			 br_nchunks;
	uint64_t			 br_bytenr;
	uint64_t			 br_generation;
	/* Maximum metadata generation visible through this root. */
	uint64_t			 br_view_generation;
	uint64_t			 br_owner;
	struct btrfs_transaction	*br_transaction;
	uint8_t				 br_level;
};

struct btrfs_root_entry {
	LIST_ENTRY(btrfs_root_entry)	 bre_entry;
	struct rwlock			 bre_lock;
	struct btrfs_root		 bre_root;
};

struct btrfs_extent_buffer {
	LIST_ENTRY(btrfs_extent_buffer)	 eb_entry;
	TAILQ_ENTRY(btrfs_extent_buffer) eb_dirty_entry;
	struct btrfs_mount		*eb_mount;
	struct btrfs_transaction	*eb_transaction;
	struct buf			*eb_buf;
	void				*eb_private;
	struct rwlock			 eb_lock;
	uint64_t			 eb_bytenr;
	uint64_t			 eb_generation;
	uint64_t			 eb_owner;
	unsigned int			 eb_refs;
	int				 eb_error;
	uint8_t				 eb_level;
	uint8_t				 eb_loaded;
	uint8_t				 eb_dirty;
	uint8_t				 eb_writeback;
	uint8_t				 eb_written;
	uint8_t				 eb_stale;
};

struct btrfs_path {
	struct btrfs_root	*bp_root;
	struct btrfs_extent_buffer
				*bp_eb[BTRFS_MAX_LEVEL];
	uint32_t		 bp_slot[BTRFS_MAX_LEVEL];
	uint64_t		 bp_view_generation;
	struct btrfs_trans_handle *bp_handle;
	uint8_t			 bp_level;
	uint8_t			 bp_write;
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
	struct btrfs_root_location bb_extent_root;
	struct btrfs_root_location bb_dev_root;
	struct btrfs_root_location bb_free_space_root;
	struct btrfs_root_location bb_block_group_root;
	uint64_t		 bb_fs_root;
	uint64_t		 bb_fs_root_generation;
	uint64_t		 bb_csum_root;
	uint64_t		 bb_csum_root_generation;
	uint64_t		 bb_fs_root_flags;
	uint8_t			 bb_fs_root_level;
	uint8_t			 bb_csum_root_level;
	uint8_t			 bb_chunk_tree_uuid[BTRFS_UUID_SIZE];
};

struct btrfs_mount {
	struct mount			*bm_mount;
	struct vnode			*bm_devvp;
	dev_t				 bm_dev;
	int				 bm_open_flags;
	struct btrfs_super_block	 bm_super;
	struct btrfs_super_mirror	 bm_super_mirrors[
					    BTRFS_SUPER_MIRROR_MAX];
	unsigned int			 bm_selected_super;
	uint8_t				 bm_backup_roots_valid;
	uint8_t				 bm_seeding;
	uint8_t				 bm_subvol_readonly;
	struct btrfs_chunk_map		*bm_chunks;
	unsigned int			 bm_nchunks;
	struct btrfs_block_group	*bm_block_groups;
	unsigned int			 bm_nblock_groups;
	struct btrfs_transaction	*bm_transaction;
	struct mutex			 bm_trans_mtx;
	uint64_t			 bm_last_transid;
	int				 bm_committer;
	uint64_t			 bm_treeid;
	uint64_t			 bm_root_dirid;
	uint8_t				 bm_chunk_tree_uuid[BTRFS_UUID_SIZE];
	struct btrfs_root_list		 bm_roots;
	struct mutex			 bm_rootmtx;
	struct btrfs_extent_buffer_list	 bm_extent_buffers;
	struct mutex			 bm_ebmtx;
	struct btrfs_node_list		 bm_nodes;
	struct mutex			 bm_nodemtx;
	/* Serializes namespace allocation across parent directories. */
	struct rwlock			 bm_namespace_lock;
};

struct btrfs_node {
	LIST_ENTRY(btrfs_node)		 bn_entry;
	struct vnode			*bn_vnode;
	struct btrfs_mount		*bn_mount;
	struct rrwlock			 bn_lock;
	struct lockf_state		*bn_lockf;
	uint64_t			 bn_treeid;
	uint64_t			 bn_ino;
	int				 bn_hashed;
	struct btrfs_inode		 bn_inode;
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
int	btrfs_build_super(struct btrfs_transaction *,
	    struct btrfs_super_block *);
int	btrfs_super_mirror_writable(const struct btrfs_mount *, unsigned int);
int	btrfs_write_super_mirrors(struct btrfs_mount *,
	    const struct btrfs_super_block *);
void	btrfs_init_roots(struct btrfs_mount *, const struct btrfs_bootstrap *);
void	btrfs_free_roots(struct btrfs_mount *);
int	btrfs_get_root(struct btrfs_mount *, uint64_t, struct btrfs_root **);
int	btrfs_find_root_item(struct btrfs_root *, uint64_t, uint64_t,
	    struct btrfs_root_item *);
int	btrfs_lookup_logical(const struct btrfs_chunk_map *, unsigned int,
	    uint64_t, uint32_t, struct btrfs_io_map *);
int	btrfs_read_logical(struct vnode *, const struct btrfs_chunk_map *,
	    unsigned int, uint64_t, uint32_t, uint64_t,
	    btrfs_io_validate_fn, void *, struct btrfs_io_result *,
	    struct buf **);
int	btrfs_write_logical(struct vnode *, const struct btrfs_chunk_map *,
	    unsigned int, uint64_t, uint32_t, uint64_t, const void *,
	    struct btrfs_io_result *);
int	btrfs_decode_chunk_item(const struct btrfs_super_block *,
	    const struct btrfs_key *, const struct btrfs_chunk *, size_t,
	    struct btrfs_chunk_map *);
int	btrfs_extent_buffer_read(const struct btrfs_root *, uint64_t, uint64_t,
	    uint64_t, uint8_t, struct btrfs_extent_buffer **);
int	btrfs_extent_buffer_clone(struct btrfs_trans_handle *,
	    const struct btrfs_extent_buffer *, struct btrfs_extent_buffer **);
int	btrfs_extent_buffer_alloc(struct btrfs_trans_handle *,
	    const struct btrfs_extent_buffer *, uint8_t,
	    struct btrfs_extent_buffer **);
int	btrfs_extent_buffer_discard(struct btrfs_trans_handle *,
	    struct btrfs_extent_buffer *);
const void *btrfs_extent_buffer_data(const struct btrfs_extent_buffer *);
void	*btrfs_extent_buffer_data_mutable(struct btrfs_trans_handle *,
	    struct btrfs_extent_buffer *);
void	btrfs_extent_buffer_put(struct btrfs_extent_buffer *);
int	btrfs_write_dirty_metadata(struct btrfs_transaction *);
int	btrfs_extent_buffers_finish(struct btrfs_transaction *, int);
int	btrfs_delayed_ref_add(struct btrfs_trans_handle *, uint64_t,
	    uint64_t, uint64_t, uint8_t, int);
int	btrfs_delayed_data_ref_add(struct btrfs_trans_handle *, uint64_t,
	    uint64_t, uint64_t, uint64_t, uint64_t, int);
int	btrfs_run_delayed_refs(struct btrfs_trans_handle *);
int	btrfs_run_delayed_data_refs(struct btrfs_trans_handle *);
int	btrfs_prepare_metadata_commit(struct btrfs_trans_handle *);
int	btrfs_delayed_refs_finish(struct btrfs_transaction *, int);
int	btrfs_delayed_data_refs_finish(struct btrfs_transaction *, int);
int	btrfs_ordered_extents_finish(struct btrfs_transaction *, int);
int	btrfs_write_ordered_extents(struct btrfs_transaction *);
int	btrfs_roots_finish(struct btrfs_transaction *, int);
int	btrfs_update_dirty_root_items(struct btrfs_trans_handle *);
/*
 * An exact miss leaves path at the insertion point.  Paths must initially
 * be zeroed and retain item pointers until advanced or released.  A write
 * path also retains its transaction handle and must be released before that
 * handle ends.
 */
int	btrfs_search_slot(struct btrfs_root *, const struct btrfs_key *,
	    struct btrfs_path *);
int	btrfs_search_slot_write(struct btrfs_trans_handle *,
	    struct btrfs_root *, const struct btrfs_key *,
	    struct btrfs_path *);
int	btrfs_cow_block(struct btrfs_trans_handle *, struct btrfs_root *,
	    struct btrfs_extent_buffer *, uint32_t,
	    struct btrfs_extent_buffer **);
/*
 * Item keys and payloads are supplied in their packed on-disk encoding.
 * Insertion grows tree topology as needed; empty-node removal is not present.
 */
int	btrfs_insert_item(struct btrfs_trans_handle *, struct btrfs_root *,
	    const struct btrfs_key *, const void *, uint32_t);
int	btrfs_replace_item(struct btrfs_trans_handle *, struct btrfs_root *,
	    const struct btrfs_key *, const void *, uint32_t);
int	btrfs_delete_item(struct btrfs_trans_handle *, struct btrfs_root *,
	    const struct btrfs_key *);
int	btrfs_search_lower_bound(struct btrfs_root *,
	    const struct btrfs_key *, struct btrfs_path *);
int	btrfs_search_predecessor(struct btrfs_root *,
	    const struct btrfs_key *, struct btrfs_path *);
int	btrfs_next_item(struct btrfs_path *);
int	btrfs_prev_item(struct btrfs_path *);
int	btrfs_path_item(const struct btrfs_path *, const struct btrfs_key **,
	    const uint8_t **, uint32_t *);
void	btrfs_release_path(struct btrfs_path *);
int	btrfs_find_inode(struct btrfs_root *, uint64_t, struct btrfs_inode *);
int	btrfs_write_inode(struct btrfs_trans_handle *, struct btrfs_node *);
int	btrfs_create_inode(struct btrfs_node *, const char *, size_t,
	    mode_t, uid_t, gid_t, const char *, struct vnode **);
int	btrfs_find_dir_parent(struct btrfs_root *, uint64_t, uint64_t *);
int	btrfs_find_subvol_parent(struct btrfs_mount *, uint64_t, uint64_t *,
	    uint64_t *);
int	btrfs_iterate_directory(struct btrfs_root *, uint64_t,
	    btrfs_dir_iter_fn, void *);
int	btrfs_find_file_extent(const struct btrfs_mount *,
	    struct btrfs_root *, struct btrfs_path *, uint64_t, uint64_t,
	    uint64_t, struct btrfs_file_extent *);
int	btrfs_read_ordered_sector(struct btrfs_node *, uint64_t, void *);
int	btrfs_write_file_sector(struct btrfs_trans_handle *,
	    struct btrfs_node *, uint64_t, const void *, uint64_t);
int	btrfs_iterate_extent_items(struct btrfs_mount *,
	    btrfs_extent_iter_fn, btrfs_backref_iter_fn, void *);
int	btrfs_iterate_block_groups(struct btrfs_mount *,
	    btrfs_block_group_iter_fn, void *);
int	btrfs_iterate_chunk_items(struct btrfs_mount *,
	    btrfs_chunk_iter_fn, void *);
int	btrfs_iterate_device_extents(struct btrfs_mount *,
	    btrfs_dev_extent_iter_fn, void *);
int	btrfs_iterate_free_space(struct btrfs_mount *,
	    btrfs_free_space_iter_fn, void *);
int	btrfs_space_init(struct btrfs_mount *);
void	btrfs_space_destroy(struct btrfs_mount *);
int	btrfs_space_reserve(struct btrfs_trans_handle *,
	    const struct btrfs_trans_reservation *);
int	btrfs_space_reserve_commit(struct btrfs_transaction *);
void	btrfs_space_release(struct btrfs_trans_handle *);
int	btrfs_space_alloc(struct btrfs_trans_handle *, uint64_t, uint64_t,
	    uint64_t, uint64_t *);
int	btrfs_space_cancel_alloc(struct btrfs_trans_handle *, uint64_t,
	    uint64_t);
int	btrfs_update_space_items(struct btrfs_trans_handle *);
int	btrfs_space_pin(struct btrfs_trans_handle *, uint64_t, uint64_t);
void	btrfs_space_commit(struct btrfs_transaction *);
void	btrfs_space_abort(struct btrfs_transaction *);
int	btrfs_trans_init(struct btrfs_mount *);
void	btrfs_trans_destroy(struct btrfs_mount *);
int	btrfs_trans_join(struct btrfs_mount *,
	    const struct btrfs_trans_reservation *,
	    struct btrfs_trans_handle **);
int	btrfs_trans_commit_handle(struct btrfs_transaction *,
	    struct btrfs_trans_handle **);
int	btrfs_trans_end(struct btrfs_trans_handle *);
void	btrfs_trans_abort(struct btrfs_trans_handle *, int);
int	btrfs_trans_close(struct btrfs_mount *, uint64_t,
	    struct btrfs_transaction **);
int	btrfs_trans_commit(struct btrfs_mount *, uint64_t, struct proc *);
int	btrfs_trans_finish(struct btrfs_mount *, struct btrfs_transaction *,
	    int);
int	btrfs_read_data_csums(struct btrfs_mount *, uint64_t, uint64_t,
	    uint32_t *);
int	btrfs_lookup_data_csum(struct btrfs_mount *, uint64_t, uint32_t *);
int	btrfs_read_data_block(struct btrfs_mount *, uint64_t,
	    const uint32_t *, struct buf **);
int	btrfs_read_compressed_extent(struct btrfs_node *,
	    const struct btrfs_file_extent *, uint64_t, size_t, void *);
int	btrfs_vget(struct mount *, ino_t, struct vnode **);
int	btrfs_vget_tree(struct mount *, uint64_t, uint64_t, struct vnode **);

#endif
