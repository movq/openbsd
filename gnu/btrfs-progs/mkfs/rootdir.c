/*
 * Copyright (C) 2017 SUSE.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * Reduced OpenBSD implementation of mkfs --rootdir.  This intentionally
 * supports only directories, regular files, hard links, and symbolic links.
 */

#include "kerncompat.h"
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "kernel-lib/list.h"
#include "kernel-lib/overflow.h"
#include "kernel-lib/sizes.h"
#include "kernel-shared/accessors.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/extent_io.h"
#include "kernel-shared/file-item.h"
#include "kernel-shared/free-space-tree.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/volumes.h"
#include "common/extent-tree-utils.h"
#include "common/internal.h"
#include "common/messages.h"
#include "common/path-utils.h"
#include "common/rbtree-utils.h"
#include "mkfs/rootdir.h"

#define MAX_EXTENT_SIZE SZ_1M

struct inode_entry {
	u64 ino;
	struct btrfs_root *root;
	struct list_head list;
};

struct hardlink_entry {
	struct rb_node node;
	dev_t st_dev;
	ino_t st_ino;
	nlink_t st_nlink;
	nlink_t found_nlink;
	struct btrfs_root *root;
	u64 btrfs_ino;
};

struct rootdir_path {
	int level;
	struct list_head inode_list;
};

static struct rb_root hardlink_root = RB_ROOT;
static struct rootdir_path current_path;
static struct btrfs_trans_handle *rootdir_trans;
static u32 size_sectorsize;
static u64 size_inode_count;
static u64 size_data_bytes;
static dev_t target_dev;
static ino_t target_ino;

static int
hardlink_compare_nodes(const struct rb_node *node1,
    const struct rb_node *node2)
{
	const struct hardlink_entry *entry1;
	const struct hardlink_entry *entry2;

	entry1 = rb_entry(node1, struct hardlink_entry, node);
	entry2 = rb_entry(node2, struct hardlink_entry, node);
	if (entry1->st_dev < entry2->st_dev)
		return -1;
	if (entry1->st_dev > entry2->st_dev)
		return 1;
	if (entry1->st_ino < entry2->st_ino)
		return -1;
	if (entry1->st_ino > entry2->st_ino)
		return 1;
	if (entry1->root < entry2->root)
		return -1;
	if (entry1->root > entry2->root)
		return 1;
	return 0;
}

static struct hardlink_entry *
find_hardlink(struct btrfs_root *root, const struct stat *st)
{
	const struct hardlink_entry key = {
		.st_dev = st->st_dev,
		.st_ino = st->st_ino,
		.root = root,
	};
	struct rb_node *node;

	node = rb_search(&hardlink_root, &key,
	    (rb_compare_keys)hardlink_compare_nodes, NULL);
	if (node == NULL)
		return NULL;
	return rb_entry(node, struct hardlink_entry, node);
}

static int
add_hardlink(struct btrfs_root *root, u64 btrfs_ino,
    const struct stat *st)
{
	struct hardlink_entry *entry;
	int ret;

	entry = calloc(1, sizeof(*entry));
	if (entry == NULL)
		return -ENOMEM;
	entry->st_dev = st->st_dev;
	entry->st_ino = st->st_ino;
	entry->st_nlink = st->st_nlink;
	entry->found_nlink = 1;
	entry->root = root;
	entry->btrfs_ino = btrfs_ino;

	ret = rb_insert(&hardlink_root, &entry->node, hardlink_compare_nodes);
	if (ret != 0)
		free(entry);
	return ret;
}

static void
free_hardlink(struct rb_node *node)
{
	free(rb_entry(node, struct hardlink_entry, node));
}

static struct inode_entry *
rootdir_path_last(void)
{
	UASSERT(!list_empty(&current_path.inode_list));
	return list_entry(current_path.inode_list.prev, struct inode_entry,
	    list);
}

static int
rootdir_path_push(struct btrfs_root *root, u64 ino)
{
	struct inode_entry *entry;

	entry = malloc(sizeof(*entry));
	if (entry == NULL)
		return -ENOMEM;
	entry->root = root;
	entry->ino = ino;
	list_add_tail(&entry->list, &current_path.inode_list);
	current_path.level++;
	return 0;
}

static void
rootdir_path_pop(void)
{
	struct inode_entry *entry;

	UASSERT(current_path.level > 0);
	entry = rootdir_path_last();
	list_del(&entry->list);
	current_path.level--;
	free(entry);
}

static bool
supported_file_type(mode_t mode)
{
	return S_ISDIR(mode) || S_ISREG(mode) || S_ISLNK(mode);
}

static int
validate_source_entry(const char *path, const struct stat *st, int type,
    struct FTW *ftwbuf)
{
	char link_target[PATH_MAX];
	int fd;
	int ret;

	(void)ftwbuf;
	if (type == FTW_DNR || type == FTW_NS) {
		error("cannot access source path %s", path);
		return -EACCES;
	}
	if (!supported_file_type(st->st_mode)) {
		error("unsupported file type in --rootdir source: %s", path);
		return -EOPNOTSUPP;
	}
	if (S_ISREG(st->st_mode)) {
		fd = open(path, O_RDONLY | O_NOFOLLOW);
			if (fd < 0) {
				ret = errno;
				error("cannot open source file %s: %s", path,
				    strerror(ret));
				return -ret;
			}
		close(fd);
	} else if (S_ISLNK(st->st_mode)) {
		ret = path_readlink(link_target, path);
		if (ret < 0) {
			error("cannot read source symlink %s: %s", path,
			    strerror(-ret));
			return ret;
		}
	}
	return 0;
}

int
btrfs_mkfs_validate_source_dir(const char *source_dir)
{
	struct stat st;
	int ret;

	if (source_dir == NULL)
		return 0;
	if (lstat(source_dir, &st) < 0) {
		ret = errno;
		error("cannot stat source directory %s: %s", source_dir,
		    strerror(ret));
		return -ret;
	}
	if (!S_ISDIR(st.st_mode)) {
		error("--rootdir source is not a directory: %s", source_dir);
		return -ENOTDIR;
	}

	ret = nftw(source_dir, validate_source_entry, 32, FTW_PHYS);
	if (ret != 0) {
		if (ret > 0)
			ret = -EIO;
		return ret;
	}
	return 0;
}

static int
validate_target_entry(const char *path, const struct stat *st, int type,
    struct FTW *ftwbuf)
{
	(void)ftwbuf;
	if (type == FTW_DNR || type == FTW_NS) {
		error("cannot access source path %s", path);
		return -EACCES;
	}
	if (st->st_dev != target_dev || st->st_ino != target_ino)
		return 0;

	error("target image must not be hard-linked into --rootdir source: %s",
	    path);
	return -EINVAL;
}

int
btrfs_mkfs_validate_target(const char *source_dir, const char *target)
{
	struct stat st;
	char base_copy[PATH_MAX];
	char parent_copy[PATH_MAX];
	char parent[PATH_MAX];
	char resolved[PATH_MAX];
	const char *base;
	const char *dir;
	int ret;

	if (source_dir == NULL)
		return 0;
	if (realpath(target, resolved) == NULL) {
		ret = errno;
		if (lstat(target, &st) == 0 && S_ISLNK(st.st_mode)) {
			error("cannot resolve target image symlink %s: %s", target,
			    strerror(ret));
			return -ret;
		}
		if (strlcpy(base_copy, target, sizeof(base_copy)) >=
		    sizeof(base_copy) ||
		    strlcpy(parent_copy, target, sizeof(parent_copy)) >=
		    sizeof(parent_copy))
			return -ENAMETOOLONG;
		base = basename(base_copy);
		dir = dirname(parent_copy);
		if (realpath(dir, parent) == NULL)
			return 0;
		ret = snprintf(resolved, sizeof(resolved), "%s/%s", parent,
		    base);
		if (ret < 0 || (size_t)ret >= sizeof(resolved))
			return -ENAMETOOLONG;
	}
	if (path_is_in_dir(source_dir, resolved)) {
		error("target image must not be inside --rootdir source: %s",
		    target);
		return -EINVAL;
	}
	if (stat(target, &st) == 0 && S_ISREG(st.st_mode)) {
		target_dev = st.st_dev;
		target_ino = st.st_ino;
		ret = nftw(source_dir, validate_target_entry, 32, FTW_PHYS);
		if (ret != 0) {
			if (ret > 0)
				ret = -EIO;
			return ret;
		}
	}
	return 0;
}

int
btrfs_mkfs_validate_subvols(const char *source_dir, struct list_head *subvols)
{
	(void)source_dir;
	if (list_empty(subvols))
		return 0;
	error("--subvol is not supported on OpenBSD");
	return -EOPNOTSUPP;
}

int
btrfs_mkfs_validate_inode_flags(const char *source_dir,
    struct list_head *inode_flags)
{
	(void)source_dir;
	if (list_empty(inode_flags))
		return 0;
	error("--inode-flags is not supported on OpenBSD");
	return -EOPNOTSUPP;
}

static void
stat_to_inode_item(struct btrfs_inode_item *inode, const struct stat *st)
{
	if (!S_ISDIR(st->st_mode))
		btrfs_set_stack_inode_size(inode, st->st_size);
	btrfs_set_stack_inode_nbytes(inode, 0);
	btrfs_set_stack_inode_block_group(inode, 0);
	btrfs_set_stack_inode_uid(inode, st->st_uid);
	btrfs_set_stack_inode_gid(inode, st->st_gid);
	btrfs_set_stack_inode_mode(inode, st->st_mode);
	btrfs_set_stack_inode_rdev(inode, 0);
	btrfs_set_stack_inode_flags(inode, 0);
	btrfs_set_stack_timespec_sec(&inode->atime, st->st_atim.tv_sec);
	btrfs_set_stack_timespec_nsec(&inode->atime, st->st_atim.tv_nsec);
	btrfs_set_stack_timespec_sec(&inode->ctime, st->st_ctim.tv_sec);
	btrfs_set_stack_timespec_nsec(&inode->ctime, st->st_ctim.tv_nsec);
	btrfs_set_stack_timespec_sec(&inode->mtime, st->st_mtim.tv_sec);
	btrfs_set_stack_timespec_nsec(&inode->mtime, st->st_mtim.tv_nsec);
	btrfs_set_stack_timespec_sec(&inode->otime, 0);
	btrfs_set_stack_timespec_nsec(&inode->otime, 0);
}

static u8
file_type_to_btrfs(mode_t mode)
{
	if (S_ISREG(mode))
		return BTRFS_FT_REG_FILE;
	if (S_ISDIR(mode))
		return BTRFS_FT_DIR;
	if (S_ISLNK(mode))
		return BTRFS_FT_SYMLINK;
	return BTRFS_FT_UNKNOWN;
}

static int
read_inode_item(struct btrfs_root *root, struct btrfs_inode_item *inode,
    u64 ino)
{
	struct btrfs_path path = { 0 };
	struct btrfs_key key = {
		.objectid = ino,
		.type = BTRFS_INODE_ITEM_KEY,
		.offset = 0,
	};
	int ret;

	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret > 0)
		ret = -ENOENT;
	if (ret == 0)
		read_extent_buffer(path.nodes[0], inode,
		    btrfs_item_ptr_offset(path.nodes[0], path.slots[0]),
		    sizeof(*inode));
	btrfs_release_path(&path);
	return ret;
}

static int
update_inode_item(struct btrfs_trans_handle *trans, struct btrfs_root *root,
    const struct btrfs_inode_item *inode, u64 ino)
{
	struct btrfs_path path = { 0 };
	struct btrfs_key key = {
		.objectid = ino,
		.type = BTRFS_INODE_ITEM_KEY,
		.offset = 0,
	};
	u32 offset;
	int ret;

	ret = btrfs_lookup_inode(trans, root, &path, &key, 1);
	if (ret > 0)
		ret = -ENOENT;
	if (ret < 0)
		goto out;

	offset = btrfs_item_ptr_offset(path.nodes[0], path.slots[0]);
	write_extent_buffer(path.nodes[0], inode, offset, sizeof(*inode));
	btrfs_mark_buffer_dirty(path.nodes[0]);
	ret = 0;
out:
	btrfs_release_path(&path);
	return ret;
}

static int
add_symlink(struct btrfs_trans_handle *trans, struct btrfs_root *root,
    struct btrfs_inode_item *inode, u64 ino, const char *path)
{
	char target[PATH_MAX];
	u64 nbytes;
	int ret;

	ret = path_readlink(target, path);
	if (ret < 0)
		return ret;
	nbytes = ret + 1;
	ret = btrfs_insert_inline_extent(trans, root, ino, 0, target, nbytes,
	    BTRFS_COMPRESS_NONE, nbytes);
	if (ret == 0)
		btrfs_set_stack_inode_nbytes(inode, nbytes);
	return ret;
}

static int
insert_reserved_file_extent(struct btrfs_trans_handle *trans,
    struct btrfs_root *root, u64 ino, struct btrfs_inode_item *inode,
    u64 file_pos, struct btrfs_file_extent_item *file_extent)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	u64 disk_bytenr;
	u64 disk_num_bytes;
	u64 num_bytes;
	struct btrfs_extent_item *extent_item;
	struct btrfs_root *extent_root;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	struct btrfs_path *path;
	int ret;

	disk_bytenr = btrfs_stack_file_extent_disk_bytenr(file_extent);
	disk_num_bytes = btrfs_stack_file_extent_disk_num_bytes(file_extent);
	num_bytes = btrfs_stack_file_extent_num_bytes(file_extent);
	extent_root = btrfs_extent_root(fs_info, disk_bytenr);

	path = btrfs_alloc_path();
	if (path == NULL)
		return -ENOMEM;
	key.objectid = disk_bytenr;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = disk_num_bytes;

	ret = btrfs_insert_empty_item(trans, extent_root, path, &key,
	    sizeof(*extent_item));
	if (ret == 0) {
		leaf = path->nodes[0];
		extent_item = btrfs_item_ptr(leaf, path->slots[0],
		    struct btrfs_extent_item);
		btrfs_set_extent_refs(leaf, extent_item, 0);
		btrfs_set_extent_generation(leaf, extent_item, trans->transid);
		btrfs_set_extent_flags(leaf, extent_item,
		    BTRFS_EXTENT_FLAG_DATA);
		btrfs_mark_buffer_dirty(leaf);
		ret = btrfs_update_block_group(trans, disk_bytenr,
		    disk_num_bytes, 1, 0);
		if (ret != 0)
			goto out;
	} else if (ret != -EEXIST) {
		goto out;
	}
	btrfs_release_path(path);

	ret = remove_from_free_space_tree(trans, disk_bytenr,
	    disk_num_bytes);
	if (ret != 0)
		goto out;
	ret = btrfs_run_delayed_refs(trans, -1);
	if (ret != 0)
		goto out;
	ret = btrfs_insert_file_extent(trans, root, ino, file_pos,
	    file_extent);
	if (ret != 0)
		goto out;

	btrfs_set_stack_inode_nbytes(inode,
	    btrfs_stack_inode_nbytes(inode) + num_bytes);
	ret = btrfs_inc_extent_ref(trans, disk_bytenr, disk_num_bytes, 0,
	    root->root_key.objectid, ino, file_pos);
out:
	btrfs_free_path(path);
	return ret;
}

static int
read_file_range(int fd, void *buffer, size_t length, off_t offset,
    const char *path)
{
	size_t done = 0;

	while (done < length) {
		ssize_t ret;

		ret = pread(fd, (char *)buffer + done, length - done,
		    offset + done);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			ret = errno;
			error("cannot read source file %s: %s", path,
			    strerror(ret));
			return -ret;
		}
		if (ret == 0) {
			error("source file changed while reading: %s", path);
			return -EIO;
		}
		done += ret;
	}
	return 0;
}

static int
add_file_extent(struct btrfs_trans_handle *trans, struct btrfs_root *root,
    struct btrfs_inode_item *inode, u64 ino, int fd, const char *path,
    u64 file_pos, u64 file_size, void *buffer)
{
	struct btrfs_file_extent_item file_extent = { 0 };
	struct btrfs_key key;
	const u32 sectorsize = root->fs_info->sectorsize;
	u64 to_read;
	u64 to_write;
	u64 logical;
	unsigned int i;
	int ret;

	to_read = min_t(u64, MAX_EXTENT_SIZE, file_size - file_pos);
	to_write = round_up(to_read, sectorsize);
	memset(buffer, 0, to_write);
	ret = read_file_range(fd, buffer, to_read, file_pos, path);
	if (ret != 0)
		return ret;

	ret = btrfs_reserve_extent(trans, root, to_write, 0, 0, (u64)-1,
	    &key, 1);
	if (ret != 0)
		return ret;
	logical = key.objectid;

	ret = write_data_to_disk(root->fs_info, buffer, logical, to_write);
	if (ret != 0) {
		error("failed to write data for %s", path);
		return ret;
	}
	for (i = 0; i < to_write / sectorsize; i++) {
		ret = btrfs_csum_file_block(trans, logical +
		    (i * sectorsize), BTRFS_EXTENT_CSUM_OBJECTID,
		    root->fs_info->csum_type,
		    (char *)buffer + (i * sectorsize));
		if (ret != 0)
			return ret;
	}

	btrfs_set_stack_file_extent_type(&file_extent,
	    BTRFS_FILE_EXTENT_REG);
	btrfs_set_stack_file_extent_disk_bytenr(&file_extent, logical);
	btrfs_set_stack_file_extent_disk_num_bytes(&file_extent, to_write);
	btrfs_set_stack_file_extent_num_bytes(&file_extent, to_write);
	btrfs_set_stack_file_extent_ram_bytes(&file_extent, to_write);
	ret = insert_reserved_file_extent(trans, root, ino, inode, file_pos,
	    &file_extent);
	if (ret != 0)
		return ret;
	return to_read;
}

static int
add_regular_file(struct btrfs_trans_handle *trans, struct btrfs_root *root,
    struct btrfs_inode_item *inode, u64 ino, const struct stat *st,
    const char *path)
{
	struct stat opened_st;
	void *buffer = NULL;
	u64 file_pos = 0;
	int fd;
	int ret = 0;

	if (st->st_size == 0)
		return 0;
	fd = open(path, O_RDONLY | O_NOFOLLOW);
	if (fd < 0) {
		ret = errno;
		error("cannot open source file %s: %s", path, strerror(ret));
		return -ret;
	}
	if (fstat(fd, &opened_st) < 0) {
		ret = -errno;
		error("cannot stat open source file %s: %s", path,
		    strerror(-ret));
		goto out;
	}
	if (!S_ISREG(opened_st.st_mode) || opened_st.st_dev != st->st_dev ||
	    opened_st.st_ino != st->st_ino ||
	    opened_st.st_size != st->st_size) {
		error("source file changed during traversal: %s", path);
		ret = -EAGAIN;
		goto out;
	}

	if ((u64)st->st_size <= BTRFS_MAX_INLINE_DATA_SIZE(root->fs_info) &&
	    st->st_size < root->fs_info->sectorsize) {
		buffer = malloc(st->st_size);
		if (buffer == NULL) {
			ret = -ENOMEM;
			goto out;
		}
		ret = read_file_range(fd, buffer, st->st_size, 0, path);
		if (ret != 0)
			goto out;
		ret = btrfs_insert_inline_extent(trans, root, ino, 0, buffer,
		    st->st_size, BTRFS_COMPRESS_NONE, st->st_size);
		if (ret == 0)
			btrfs_set_stack_inode_nbytes(inode, st->st_size);
		goto out;
	}

	buffer = malloc(MAX_EXTENT_SIZE);
	if (buffer == NULL) {
		ret = -ENOMEM;
		goto out;
	}
	while (file_pos < (u64)st->st_size) {
		ret = add_file_extent(trans, root, inode, ino, fd, path,
		    file_pos, st->st_size, buffer);
		if (ret < 0)
			goto out;
		file_pos += ret;
	}
	ret = 0;
out:
	free(buffer);
	close(fd);
	return ret;
}

static int
add_source_inode(const char *path, const struct stat *st, int type,
    struct FTW *ftwbuf)
{
	struct btrfs_fs_info *fs_info = rootdir_trans->fs_info;
	struct btrfs_inode_item inode = { 0 };
	struct hardlink_entry *hardlink;
	struct inode_entry *parent;
	struct btrfs_root *root;
	bool track_hardlink;
	const char *name;
	u64 ino;
	int namelen;
	int ret;

	if (type == FTW_DNR || type == FTW_NS)
		return -EACCES;
	if (!supported_file_type(st->st_mode))
		return -EOPNOTSUPP;

	if (ftwbuf->level == 0) {
		root = fs_info->fs_root;
		ino = btrfs_root_dirid(&root->root_item);
		stat_to_inode_item(&inode, st);
		btrfs_set_stack_inode_nlink(&inode, 1);
		ret = update_inode_item(rootdir_trans, root, &inode, ino);
		if (ret != 0)
			return ret;
		INIT_LIST_HEAD(&current_path.inode_list);
		current_path.level = 0;
		return rootdir_path_push(root, ino);
	}

	while (current_path.level > ftwbuf->level)
		rootdir_path_pop();
	parent = rootdir_path_last();
	root = parent->root;
	name = path + ftwbuf->base;
	namelen = strlen(path) - ftwbuf->base;
	track_hardlink = !S_ISDIR(st->st_mode) && st->st_nlink > 1;

	if (track_hardlink) {
		hardlink = find_hardlink(root, st);
		if (hardlink != NULL) {
			ret = btrfs_add_link(rootdir_trans, root,
			    hardlink->btrfs_ino, parent->ino, name, namelen,
			    file_type_to_btrfs(st->st_mode), NULL, 1, 0);
			if (ret != 0)
				return ret;
			hardlink->found_nlink++;
			if (hardlink->found_nlink >= hardlink->st_nlink) {
				rb_erase(&hardlink->node, &hardlink_root);
				free(hardlink);
			}
			return 0;
		}
	}

	ret = btrfs_find_free_objectid(rootdir_trans, root,
	    BTRFS_FIRST_FREE_OBJECTID, &ino);
	if (ret != 0)
		return ret;
	stat_to_inode_item(&inode, st);
	ret = btrfs_insert_inode(rootdir_trans, root, ino, &inode);
	if (ret != 0)
		return ret;
	ret = btrfs_add_link(rootdir_trans, root, ino, parent->ino, name,
	    namelen, file_type_to_btrfs(st->st_mode), NULL, 1, 0);
	if (ret != 0)
		return ret;

	if (track_hardlink) {
		ret = add_hardlink(root, ino, st);
		if (ret != 0)
			return ret;
	}

	ret = read_inode_item(root, &inode, ino);
	if (ret != 0)
		return ret;
	if (S_ISDIR(st->st_mode)) {
		return rootdir_path_push(root, ino);
	} else if (S_ISREG(st->st_mode)) {
		ret = add_regular_file(rootdir_trans, root, &inode, ino, st,
		    path);
	} else {
		ret = add_symlink(rootdir_trans, root, &inode, ino, path);
	}
	if (ret != 0)
		return ret;
	return update_inode_item(rootdir_trans, root, &inode, ino);
}

int
btrfs_mkfs_fill_dir(struct btrfs_trans_handle *trans, const char *source_dir,
    struct btrfs_root *root, struct list_head *subvols,
    struct list_head *inode_flags, enum btrfs_compression_type compression,
    unsigned int compression_level, bool do_reflink)
{
	int ret;

	(void)root;
	(void)compression_level;
	if (!list_empty(subvols) || !list_empty(inode_flags) ||
	    compression != BTRFS_COMPRESS_NONE || do_reflink)
		return -EOPNOTSUPP;

	rootdir_trans = trans;
	current_path.level = 0;
	INIT_LIST_HEAD(&current_path.inode_list);
	ret = nftw(source_dir, add_source_inode, 32, FTW_PHYS);
	while (current_path.level > 0)
		rootdir_path_pop();
	rb_free_nodes(&hardlink_root, free_hardlink);
	rootdir_trans = NULL;

	if (ret != 0) {
		if (ret > 0)
			ret = -EIO;
		error("unable to populate image from %s: %s", source_dir,
		    strerror(-ret));
	}
	return ret;
}

static int
count_source_entry(const char *path, const struct stat *st, int type,
    struct FTW *ftwbuf)
{
	u64 unrounded;
	u64 rounded;
	int ret;

	(void)path;
	(void)ftwbuf;
	if (type == FTW_DNR || type == FTW_NS)
		return -EACCES;
	if (!supported_file_type(st->st_mode))
		return -EOPNOTSUPP;
	if (check_add_overflow(size_inode_count, (u64)1, &size_inode_count))
		return -EOVERFLOW;
	if (!S_ISREG(st->st_mode) || st->st_size == 0)
		return 0;

	if (st->st_nlink > 1) {
		ret = add_hardlink(NULL, 0, st);
		if (ret == -EEXIST)
			return 0;
		if (ret != 0)
			return ret;
	}
	if (check_add_overflow((u64)st->st_size,
	    (u64)size_sectorsize - 1, &unrounded))
		return -EOVERFLOW;
	rounded = round_down(unrounded, size_sectorsize);
	if (check_add_overflow(size_data_bytes, rounded, &size_data_bytes))
		return -EOVERFLOW;
	return 0;
}

int
btrfs_mkfs_size_dir(const char *source_dir, u32 sectorsize,
    u64 min_dev_size, u64 metadata_profile, u64 data_profile,
    u64 *size_ret)
{
	u64 metadata_size;
	u64 metadata_chunk = 0;
	u64 data_chunk = 0;
	u64 metadata_threshold = SZ_8M;
	u64 data_threshold = SZ_8M;
	u64 metadata_multiplier = 1;
	u64 data_multiplier = 1;
	u64 rounded;
	u64 total;
	int ret;

	size_sectorsize = sectorsize;
	size_inode_count = 0;
	size_data_bytes = 0;
	ret = nftw(source_dir, count_source_entry, 32, FTW_PHYS);
	rb_free_nodes(&hardlink_root, free_hardlink);
	if (ret != 0) {
		if (ret > 0)
			ret = -EIO;
		return ret;
	}

	if (check_mul_overflow(size_inode_count,
	    (u64)(PATH_MAX * 3) + sectorsize, &metadata_size) ||
	    check_add_overflow(metadata_size, size_data_bytes / 8,
	    &metadata_size))
		return -EOVERFLOW;

	if (metadata_profile & BTRFS_BLOCK_GROUP_DUP) {
		metadata_threshold = SZ_32M;
		metadata_multiplier = 2;
	}
	if (data_profile & BTRFS_BLOCK_GROUP_DUP) {
		data_threshold = SZ_64M;
		data_multiplier = 2;
	}
	if (metadata_size > metadata_threshold) {
		if (check_add_overflow(metadata_size, metadata_threshold - 1,
		    &rounded))
			return -EOVERFLOW;
		rounded = round_down(rounded, metadata_threshold);
		if (check_mul_overflow(rounded - metadata_threshold,
		    metadata_multiplier, &metadata_chunk))
			return -EOVERFLOW;
	}
	if (size_data_bytes > data_threshold) {
		if (check_add_overflow(size_data_bytes, data_threshold - 1,
		    &rounded))
			return -EOVERFLOW;
		rounded = round_down(rounded, data_threshold);
		if (check_mul_overflow(rounded - data_threshold,
		    data_multiplier, &data_chunk))
			return -EOVERFLOW;
	}
	if (check_add_overflow(min_dev_size, metadata_chunk, &total) ||
	    check_add_overflow(total, data_chunk, &total))
		return -EOVERFLOW;
	*size_ret = total;
	return 0;
}

int
btrfs_mkfs_shrink_fs(struct btrfs_fs_info *fs_info, u64 *new_size_ret,
    bool shrink_file_size)
{
	(void)fs_info;
	(void)new_size_ret;
	(void)shrink_file_size;
	return -EOPNOTSUPP;
}
