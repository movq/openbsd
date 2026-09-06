/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License v2.
 *
 * Reduced offline dump-tree implementation for OpenBSD.
 */

#include "kerncompat.h"
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include "kernel-shared/accessors.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/extent_io.h"
#include "kernel-shared/print-tree.h"
#include "kernel-shared/tree-checker.h"
#include "common/defs.h"
#include "common/device-scan.h"
#include "common/help.h"
#include "common/messages.h"
#include "common/parse-utils.h"
#include "common/string-utils.h"
#include "cmds/commands.h"

struct block_list {
	u64 *bytenrs;
	size_t count;
};

static const char * const cmd_inspect_dump_tree_usage[] = {
	"btrfs inspect-internal dump-tree [options] <device> [<device>...]",
	"Dump tree structures from an unmounted filesystem image or device",
	"",
	OPTLINE("-e, --extents", "print only extent and device trees"),
	OPTLINE("-d, --device", "print only device-related trees"),
	OPTLINE("-r, --roots", "print short root node information"),
	OPTLINE("-R, --backups", "print roots and backup root information"),
	OPTLINE("-u, --uuid", "print only the UUID tree"),
	OPTLINE("-b, --block BYTENR", "print one metadata block; repeatable"),
	OPTLINE("-t, --tree TREEID", "print one tree by name or numeric ID"),
	OPTLINE("--follow", "follow children of blocks selected with --block"),
	OPTLINE("--bfs", "use breadth-first traversal"),
	OPTLINE("--dfs", "use depth-first traversal"),
	OPTLINE("--noscan", "use only the first listed device"),
	OPTLINE("--hide-names", "hide filenames, xattrs, and name references"),
	OPTLINE("--csum-headers", "print metadata checksums"),
	OPTLINE("--csum-items", "print data checksums from checksum items"),
	NULL
};

static int
add_block(struct block_list *blocks, u64 bytenr)
{
	u64 *new_blocks;
	size_t i;

	for (i = 0; i < blocks->count; i++) {
		if (blocks->bytenrs[i] == bytenr) {
			warning("tree block bytenr %llu was specified more than once",
			    bytenr);
			return 0;
		}
	}
	new_blocks = reallocarray(blocks->bytenrs, blocks->count + 1,
	    sizeof(*new_blocks));
	if (new_blocks == NULL)
		return -ENOMEM;
	blocks->bytenrs = new_blocks;
	blocks->bytenrs[blocks->count++] = bytenr;
	return 0;
}

static const char *
tree_name(u64 objectid)
{
	switch (objectid) {
	case BTRFS_ROOT_TREE_OBJECTID:
		return "root";
	case BTRFS_EXTENT_TREE_OBJECTID:
		return "extent";
	case BTRFS_CHUNK_TREE_OBJECTID:
		return "chunk";
	case BTRFS_DEV_TREE_OBJECTID:
		return "device";
	case BTRFS_FS_TREE_OBJECTID:
		return "fs";
	case BTRFS_CSUM_TREE_OBJECTID:
		return "checksum";
	case BTRFS_QUOTA_TREE_OBJECTID:
		return "quota";
	case BTRFS_UUID_TREE_OBJECTID:
		return "uuid";
	case BTRFS_FREE_SPACE_TREE_OBJECTID:
		return "free space";
	case BTRFS_BLOCK_GROUP_TREE_OBJECTID:
		return "block group";
	case BTRFS_RAID_STRIPE_TREE_OBJECTID:
		return "raid stripe";
	default:
		return "file";
	}
}

static bool
tree_selected(u64 objectid, bool extent_only, bool device_only,
    bool uuid_only)
{
	if (extent_only)
		return objectid == BTRFS_EXTENT_TREE_OBJECTID ||
		    objectid == BTRFS_DEV_TREE_OBJECTID;
	if (device_only)
		return objectid == BTRFS_DEV_TREE_OBJECTID;
	if (uuid_only)
		return objectid == BTRFS_UUID_TREE_OBJECTID;
	return true;
}

static void
print_backup_roots(const struct btrfs_super_block *super)
{
	int i;

	for (i = 0; i < BTRFS_NUM_BACKUP_ROOTS; i++) {
		const struct btrfs_root_backup *backup = &super->super_roots[i];

		pr_default("backup slot %d\n", i);
		pr_default("\ttree root %llu generation %llu\n",
		    btrfs_backup_tree_root(backup),
		    btrfs_backup_tree_root_gen(backup));
		pr_default("\textent root %llu generation %llu\n",
		    btrfs_backup_extent_root(backup),
		    btrfs_backup_extent_root_gen(backup));
		pr_default("\tchunk root %llu generation %llu\n",
		    btrfs_backup_chunk_root(backup),
		    btrfs_backup_chunk_root_gen(backup));
		pr_default("\tfs root %llu generation %llu\n",
		    btrfs_backup_fs_root(backup),
		    btrfs_backup_fs_root_gen(backup));
	}
}

static int
print_tree_root(struct btrfs_root *root, const char *name, bool roots_only,
    unsigned int print_mode)
{
	if (root == NULL || !extent_buffer_uptodate(root->node)) {
		error("cannot read %s tree root", name);
		return -EIO;
	}
	if (roots_only) {
		pr_default("%s tree: %llu level %d\n", name, root->node->start,
		    btrfs_header_level(root->node));
	} else {
		pr_default("%s tree\n", name);
		btrfs_print_tree(root->node,
		    BTRFS_PRINT_TREE_FOLLOW | print_mode);
	}
	return 0;
}

static struct btrfs_root *
find_known_root(struct btrfs_fs_info *fs_info, u64 tree_id)
{
	switch (tree_id) {
	case BTRFS_ROOT_TREE_OBJECTID:
		return fs_info->tree_root;
	case BTRFS_CHUNK_TREE_OBJECTID:
		return fs_info->chunk_root;
	case BTRFS_DEV_TREE_OBJECTID:
		return fs_info->dev_root;
	case BTRFS_FS_TREE_OBJECTID:
		return fs_info->fs_root;
	case BTRFS_QUOTA_TREE_OBJECTID:
		return fs_info->quota_root;
	case BTRFS_UUID_TREE_OBJECTID:
		return fs_info->uuid_root;
	case BTRFS_BLOCK_GROUP_TREE_OBJECTID:
		return fs_info->block_group_root;
	case BTRFS_RAID_STRIPE_TREE_OBJECTID:
		return fs_info->stripe_root;
	default:
		return NULL;
	}
}

static int
print_selected_tree(struct btrfs_fs_info *fs_info, u64 tree_id,
    bool roots_only, unsigned int print_mode)
{
	struct btrfs_key key = {
		.objectid = tree_id,
		.type = BTRFS_ROOT_ITEM_KEY,
		.offset = (u64)-1,
	};
	struct btrfs_root *root;

	root = find_known_root(fs_info, tree_id);
	if (root == NULL)
		root = btrfs_read_fs_root(fs_info, &key);
	if (IS_ERR(root)) {
		error("cannot read tree %llu: %s", tree_id,
		    strerror(-PTR_ERR(root)));
		return PTR_ERR(root);
	}
	return print_tree_root(root, tree_name(tree_id), roots_only,
	    print_mode);
}

static int
print_blocks(struct btrfs_fs_info *fs_info, const struct block_list *blocks,
    unsigned int print_mode)
{
	struct btrfs_tree_parent_check check = { 0 };
	size_t i;
	int ret = 0;

	for (i = 0; i < blocks->count; i++) {
		struct extent_buffer *eb;

		if (!IS_ALIGNED(blocks->bytenrs[i], fs_info->sectorsize)) {
			error("tree block %llu is not aligned to sectorsize %u",
			    blocks->bytenrs[i], fs_info->sectorsize);
			ret = -EINVAL;
			continue;
		}
		eb = read_tree_block(fs_info, blocks->bytenrs[i], &check);
		if (!extent_buffer_uptodate(eb)) {
			error("failed to read tree block %llu",
			    blocks->bytenrs[i]);
			free_extent_buffer(eb);
			ret = -EIO;
			continue;
		}
		btrfs_print_tree(eb, print_mode);
		free_extent_buffer(eb);
	}
	return ret;
}

static int
print_all_root_items(struct btrfs_fs_info *fs_info, bool roots_only,
    bool extent_only, bool device_only, bool uuid_only,
    unsigned int print_mode)
{
	struct btrfs_path path = { 0 };
	struct btrfs_key key = {
		.objectid = 0,
		.type = BTRFS_ROOT_ITEM_KEY,
		.offset = 0,
	};
	int ret;

	ret = btrfs_search_slot(NULL, fs_info->tree_root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;
	while (1) {
		struct extent_buffer *leaf = path.nodes[0];
		int slot = path.slots[0];

		if (slot >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(fs_info->tree_root, &path);
			if (ret != 0)
				break;
			leaf = path.nodes[0];
			slot = path.slots[0];
		}
		{
			struct btrfs_disk_key disk_key;
			struct btrfs_key found;

			btrfs_item_key(leaf, &disk_key, slot);
			btrfs_disk_key_to_cpu(&found, &disk_key);
			if (found.type == BTRFS_ROOT_ITEM_KEY &&
			    found.objectid != BTRFS_ROOT_TREE_OBJECTID &&
			    found.objectid != BTRFS_CHUNK_TREE_OBJECTID &&
			    tree_selected(found.objectid, extent_only,
			    device_only, uuid_only)) {
				struct btrfs_root_item item;
				struct btrfs_tree_parent_check check = { 0 };
				struct extent_buffer *node;

				read_extent_buffer(leaf, &item,
				    btrfs_item_ptr_offset(leaf, slot),
				    sizeof(item));
				node = read_tree_block(fs_info,
				    btrfs_root_bytenr(&item), &check);
				if (extent_buffer_uptodate(node)) {
					if (roots_only) {
						pr_default("%s tree %llu: %llu "
						    "level %d\n",
						    tree_name(found.objectid),
						    found.objectid, node->start,
						    btrfs_header_level(node));
					} else {
						pr_default("%s tree ",
						    tree_name(found.objectid));
						btrfs_print_key(&disk_key);
						pr_default("\n");
						btrfs_print_tree(node,
						    BTRFS_PRINT_TREE_FOLLOW |
						    print_mode);
					}
				} else {
					warning("cannot read root %llu at %llu",
					    found.objectid,
					    btrfs_root_bytenr(&item));
				}
				free_extent_buffer(node);
			}
		}
		path.slots[0]++;
	}
	if (ret > 0)
		ret = 0;
out:
	btrfs_release_path(&path);
	return ret;
}

static int
cmd_inspect_dump_tree(const struct cmd_struct *cmd, int argc, char **argv)
{
	struct open_ctree_args open_args = { 0 };
	struct btrfs_fs_info *fs_info;
	struct block_list blocks = { 0 };
	bool extent_only = false;
	bool device_only = false;
	bool uuid_only = false;
	bool roots_only = false;
	bool backups = false;
	bool noscan = false;
	u64 tree_id = 0;
	unsigned int traverse = BTRFS_PRINT_TREE_BFS;
	unsigned int follow = 0;
	unsigned int csum_mode = 0;
	unsigned int print_mode;
	int ret = 0;

	optind = 1;
	while (1) {
		enum {
			GETOPT_VAL_FOLLOW = GETOPT_VAL_FIRST,
			GETOPT_VAL_DFS,
			GETOPT_VAL_BFS,
			GETOPT_VAL_NOSCAN,
			GETOPT_VAL_HIDE_NAMES,
			GETOPT_VAL_CSUM_HEADERS,
			GETOPT_VAL_CSUM_ITEMS,
		};
		static const struct option long_options[] = {
			{ "extents", no_argument, NULL, 'e' },
			{ "device", no_argument, NULL, 'd' },
			{ "roots", no_argument, NULL, 'r' },
			{ "backups", no_argument, NULL, 'R' },
			{ "uuid", no_argument, NULL, 'u' },
			{ "block", required_argument, NULL, 'b' },
			{ "tree", required_argument, NULL, 't' },
			{ "follow", no_argument, NULL, GETOPT_VAL_FOLLOW },
			{ "dfs", no_argument, NULL, GETOPT_VAL_DFS },
			{ "bfs", no_argument, NULL, GETOPT_VAL_BFS },
			{ "noscan", no_argument, NULL, GETOPT_VAL_NOSCAN },
			{ "hide-names", no_argument, NULL,
			    GETOPT_VAL_HIDE_NAMES },
			{ "csum-headers", no_argument, NULL,
			    GETOPT_VAL_CSUM_HEADERS },
			{ "csum-items", no_argument, NULL,
			    GETOPT_VAL_CSUM_ITEMS },
			{ NULL, 0, NULL, 0 }
		};
		int c = getopt_long(argc, argv, "edrRub:t:", long_options, NULL);

		if (c < 0)
			break;
		switch (c) {
		case 'e':
			extent_only = true;
			break;
		case 'd':
			device_only = true;
			break;
		case 'r':
			roots_only = true;
			break;
		case 'R':
			roots_only = true;
			backups = true;
			break;
		case 'u':
			uuid_only = true;
			break;
		case 'b':
			ret = add_block(&blocks, arg_strtou64(optarg));
			if (ret < 0)
				goto out;
			break;
		case 't':
			ret = parse_tree_id(optarg, &tree_id);
			if (ret < 0) {
				error("cannot parse tree id: %s", optarg);
				goto out;
			}
			break;
		case GETOPT_VAL_FOLLOW:
			follow = BTRFS_PRINT_TREE_FOLLOW;
			break;
		case GETOPT_VAL_DFS:
			traverse = BTRFS_PRINT_TREE_DFS;
			break;
		case GETOPT_VAL_BFS:
			traverse = BTRFS_PRINT_TREE_BFS;
			break;
		case GETOPT_VAL_NOSCAN:
			noscan = true;
			open_args.flags |= OPEN_CTREE_NO_DEVICES;
			break;
		case GETOPT_VAL_HIDE_NAMES:
			open_args.flags |= OPEN_CTREE_HIDE_NAMES;
			break;
		case GETOPT_VAL_CSUM_HEADERS:
			csum_mode |= BTRFS_PRINT_TREE_CSUM_HEADERS;
			break;
		case GETOPT_VAL_CSUM_ITEMS:
			csum_mode |= BTRFS_PRINT_TREE_CSUM_ITEMS;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}
	if (check_argc_min(argc - optind, 1)) {
		ret = 1;
		goto out;
	}
	if (!noscan) {
		ret = btrfs_scan_argv_devices(optind, argc, argv);
		if (ret < 0)
			goto out;
	}

	open_args.filename = argv[optind];
	open_args.flags |= OPEN_CTREE_PARTIAL | OPEN_CTREE_NO_BLOCK_GROUPS |
	    OPEN_CTREE_SKIP_LEAF_ITEM_CHECKS;
	fs_info = open_ctree_fs_info(&open_args);
	if (fs_info == NULL) {
		error("unable to open %s", argv[optind]);
		ret = 1;
		goto out;
	}

	pr_default("%s\n", PACKAGE_STRING);
	print_mode = traverse | follow | csum_mode;
	if (blocks.count != 0) {
		ret = print_blocks(fs_info, &blocks, print_mode);
		goto close;
	}
	if (tree_id != 0) {
		ret = print_selected_tree(fs_info, tree_id, roots_only,
		    print_mode);
		goto close;
	}

	if (!extent_only && !device_only && !uuid_only) {
		ret = print_tree_root(fs_info->tree_root, "root", roots_only,
		    print_mode);
		if (ret < 0)
			goto close;
		ret = print_tree_root(fs_info->chunk_root, "chunk", roots_only,
		    print_mode);
		if (ret < 0)
			goto close;
	}
	ret = print_all_root_items(fs_info, roots_only, extent_only,
	    device_only, uuid_only, print_mode);
	if (backups)
		print_backup_roots(fs_info->super_copy);
	if (!roots_only) {
		char uuid[BTRFS_UUID_UNPARSED_SIZE];

		uuid_unparse(fs_info->super_copy->fsid, uuid);
		pr_default("total bytes %llu\n",
		    btrfs_super_total_bytes(fs_info->super_copy));
		pr_default("bytes used %llu\n",
		    btrfs_super_bytes_used(fs_info->super_copy));
		pr_default("uuid %s\n", uuid);
	}
close:
	if (close_ctree_fs_info(fs_info) != 0 && ret == 0)
		ret = -EIO;
out:
	free(blocks.bytenrs);
	return ret < 0 ? 1 : ret;
}
DEFINE_SIMPLE_COMMAND(inspect_dump_tree, "dump-tree");
