/*
 * Copyright (C) 2011 Red Hat.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License v2.
 *
 * Reduced offline tree-stats implementation for OpenBSD.
 */

#include "kerncompat.h"
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "kernel-shared/accessors.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/extent_io.h"
#include "kernel-shared/file-item.h"
#include "kernel-shared/tree-checker.h"
#include "common/help.h"
#include "common/messages.h"
#include "common/parse-utils.h"
#include "common/string-utils.h"
#include "common/units.h"
#include "cmds/commands.h"

struct tree_stats {
	u64 total_nodes;
	u64 total_bytes;
	u64 total_inline;
	u64 lowest_bytenr;
	u64 highest_bytenr;
	u64 node_counts[BTRFS_MAX_LEVEL];
};

static const char * const cmd_inspect_tree_stats_usage[] = {
	"btrfs inspect-internal tree-stats [options] <device>",
	"Print offline allocation statistics for filesystem trees",
	"",
	OPTLINE("-b, --raw", "print sizes as raw byte counts"),
	OPTLINE("-t, --tree TREEID", "inspect one tree by name or numeric ID"),
	NULL
};

static int
walk_tree_block(struct btrfs_root *root, struct extent_buffer *eb,
    struct tree_stats *stats)
{
	int level = btrfs_header_level(eb);
	u32 items = btrfs_header_nritems(eb);
	u32 i;
	int ret = 0;

	stats->total_nodes++;
	stats->total_bytes += root->fs_info->nodesize;
	stats->node_counts[level]++;
	if (eb->start < stats->lowest_bytenr)
		stats->lowest_bytenr = eb->start;
	if (eb->start > stats->highest_bytenr)
		stats->highest_bytenr = eb->start;

	if (level == 0) {
		for (i = 0; i < items; i++) {
			struct btrfs_file_extent_item *extent;
			struct btrfs_key key;

			btrfs_item_key_to_cpu(eb, &key, i);
			if (key.type != BTRFS_EXTENT_DATA_KEY)
				continue;
			extent = btrfs_item_ptr(eb, i,
			    struct btrfs_file_extent_item);
			if (btrfs_file_extent_type(eb, extent) ==
			    BTRFS_FILE_EXTENT_INLINE)
				stats->total_inline +=
				    btrfs_file_extent_inline_item_len(eb, i);
		}
		return 0;
	}

	for (i = 0; i < items; i++) {
		struct btrfs_tree_parent_check check = {
			.owner_root = btrfs_header_owner(eb),
			.transid = btrfs_node_ptr_generation(eb, i),
			.level = level - 1,
		};
		struct extent_buffer *child;
		int child_ret;

		child = read_tree_block(root->fs_info,
		    btrfs_node_blockptr(eb, i), &check);
		if (!extent_buffer_uptodate(child)) {
			warning("failed to read tree block %llu",
			    btrfs_node_blockptr(eb, i));
			free_extent_buffer(child);
			ret = -EIO;
			continue;
		}
		child_ret = walk_tree_block(root, child, stats);
		free_extent_buffer(child);
		if (child_ret < 0)
			ret = child_ret;
	}
	return ret;
}

static void
print_size(const char *label, u64 value, unsigned int unit_mode)
{
	if (unit_mode == UNITS_RAW)
		pr_default("\t%s: %llu\n", label, value);
	else
		pr_default("\t%s: %s\n", label,
		    pretty_size_mode(value, unit_mode));
}

static int
calculate_tree_stats(struct btrfs_fs_info *fs_info, u64 tree_id,
    const char *name, unsigned int unit_mode)
{
	struct btrfs_key key = {
		.objectid = tree_id,
		.type = BTRFS_ROOT_ITEM_KEY,
		.offset = (u64)-1,
	};
	struct tree_stats stats = {
		.lowest_bytenr = (u64)-1,
	};
	struct btrfs_root *root;
	int level;
	int ret;
	int i;

	if (tree_id == BTRFS_ROOT_TREE_OBJECTID)
		root = fs_info->tree_root;
	else if (tree_id == BTRFS_CHUNK_TREE_OBJECTID)
		root = fs_info->chunk_root;
	else if (tree_id == BTRFS_DEV_TREE_OBJECTID)
		root = fs_info->dev_root;
	else if (tree_id == BTRFS_FS_TREE_OBJECTID)
		root = fs_info->fs_root;
	else
		root = btrfs_read_fs_root(fs_info, &key);
	if (IS_ERR(root) || root == NULL) {
		int error_code = root == NULL ? EIO : -PTR_ERR(root);

		error("cannot read %s tree %llu: %s", name, tree_id,
		    strerror(error_code));
		return -error_code;
	}

	pr_default("Calculating size of %s tree (%llu)\n", name, tree_id);
	level = btrfs_header_level(root->node);
	ret = walk_tree_block(root, root->node, &stats);
	print_size("Total size", stats.total_bytes, unit_mode);
	print_size("Inline data", stats.total_inline, unit_mode);
	print_size("Disk spread",
	    stats.highest_bytenr - stats.lowest_bytenr, unit_mode);
	pr_default("\tLevels: %d\n", level + 1);
	pr_default("\tTotal nodes: %llu\n", stats.total_nodes);
	for (i = 0; i <= level; i++)
		pr_default("\t\tOn level %d: %llu\n", i,
		    stats.node_counts[i]);
	return ret;
}

static const char *
stats_tree_name(u64 tree_id)
{
	switch (tree_id) {
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
	default:
		return "file";
	}
}

static int
cmd_inspect_tree_stats(const struct cmd_struct *cmd, int argc, char **argv)
{
	struct btrfs_root *root;
	unsigned int unit_mode = UNITS_DEFAULT;
	u64 tree_id = 0;
	int ret = 0;

	optind = 1;
	while (1) {
		static const struct option long_options[] = {
			{ "raw", no_argument, NULL, 'b' },
			{ "tree", required_argument, NULL, 't' },
			{ NULL, 0, NULL, 0 }
		};
		int c = getopt_long(argc, argv, "bt:", long_options, NULL);

		if (c < 0)
			break;
		switch (c) {
		case 'b':
			unit_mode = UNITS_RAW;
			break;
		case 't':
			ret = parse_tree_id(optarg, &tree_id);
			if (ret < 0) {
				error("cannot parse tree id: %s", optarg);
				return 1;
			}
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}
	if (check_argc_exact(argc - optind, 1))
		return 1;

	root = open_ctree(argv[optind], 0, 0);
	if (root == NULL) {
		error("cannot open filesystem tree from %s", argv[optind]);
		return 1;
	}
	if (tree_id != 0) {
		ret = calculate_tree_stats(root->fs_info, tree_id,
		    stats_tree_name(tree_id), unit_mode);
		goto out;
	}

	ret = calculate_tree_stats(root->fs_info,
	    BTRFS_ROOT_TREE_OBJECTID, "root", unit_mode);
	if (ret == 0)
		ret = calculate_tree_stats(root->fs_info,
		    BTRFS_EXTENT_TREE_OBJECTID, "extent", unit_mode);
	if (ret == 0)
		ret = calculate_tree_stats(root->fs_info,
		    BTRFS_CSUM_TREE_OBJECTID, "checksum", unit_mode);
	if (ret == 0)
		ret = calculate_tree_stats(root->fs_info,
		    BTRFS_FS_TREE_OBJECTID, "fs", unit_mode);
out:
	if (close_ctree(root) != 0 && ret == 0)
		ret = -EIO;
	return ret < 0 ? 1 : ret;
}
DEFINE_SIMPLE_COMMAND(inspect_tree_stats, "tree-stats");
