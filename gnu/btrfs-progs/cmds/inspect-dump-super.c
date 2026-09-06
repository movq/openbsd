/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License v2.
 */

#include "kerncompat.h"
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "kernel-shared/accessors.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/print-tree.h"
#include "kernel-shared/zoned.h"
#include "common/help.h"
#include "common/messages.h"
#include "common/string-utils.h"
#include "cmds/commands.h"

static int
load_and_dump_sb(const char *filename, int fd, u64 sb_bytenr, bool full,
    bool force)
{
	struct btrfs_super_block sb;
	struct stat st;
	size_t bytes;

	if (fstat(fd, &st) < 0) {
		error("unable to stat %s when loading superblock: %s", filename,
		    strerror(errno));
		return 1;
	}
	if (S_ISBLK(st.st_mode) || S_ISREG(st.st_mode)) {
		off_t last_byte = lseek(fd, 0, SEEK_END);

		if (last_byte == -1) {
			error("cannot determine size of %s: %s", filename,
			    strerror(errno));
			return 1;
		}
		if (sb_bytenr > (u64)last_byte)
			return 0;
	}

	errno = 0;
	bytes = sbread(fd, &sb, sb_bytenr);
	if (bytes != BTRFS_SUPER_INFO_SIZE) {
		if (bytes == 0 && errno == 0)
			return 0;
		error("failed to read superblock on %s at %llu: %zu/%d bytes: "
		    "%s", filename, sb_bytenr, bytes, BTRFS_SUPER_INFO_SIZE,
		    strerror(errno));
		return 1;
	}

	pr_default("superblock: bytenr=%llu, device=%s\n", sb_bytenr,
	    filename);
	pr_default("---------------------------------------------------------\n");
	if (btrfs_super_magic(&sb) != BTRFS_MAGIC && !force) {
		error("bad magic on superblock on %s at %llu "
		    "(use --force to dump it anyway)", filename, sb_bytenr);
		return 1;
	}
	btrfs_print_superblock(&sb, full);
	putchar('\n');
	return 0;
}

static const char * const cmd_inspect_dump_super_usage[] = {
	"btrfs inspect-internal dump-super [options] device [device...]",
	"Dump superblock from a device or image in textual form",
	"",
	OPTLINE("-f, --full", "print full superblock information"),
	OPTLINE("-a, --all", "print all superblock mirrors"),
	OPTLINE("-s, --super NUMBER", "select mirror 0, 1, or 2"),
	OPTLINE("-F, --force", "dump a superblock with bad magic"),
	OPTLINE("--bytenr BYTENR", "use an alternate superblock offset"),
	NULL
};

static int
cmd_inspect_dump_super(const struct cmd_struct *cmd, int argc, char **argv)
{
	bool all = false;
	bool full = false;
	bool force = false;
	u64 sb_bytenr = btrfs_sb_offset(0);
	int i;

	optind = 1;
	while (1) {
		enum { GETOPT_VAL_BYTENR = GETOPT_VAL_FIRST };
		static const struct option long_options[] = {
			{ "all", no_argument, NULL, 'a' },
			{ "bytenr", required_argument, NULL, GETOPT_VAL_BYTENR },
			{ "full", no_argument, NULL, 'f' },
			{ "force", no_argument, NULL, 'F' },
			{ "super", required_argument, NULL, 's' },
			{ NULL, 0, NULL, 0 }
		};
		u64 arg;
		int c;

		c = getopt_long(argc, argv, "fFas:", long_options, NULL);
		if (c < 0)
			break;
		switch (c) {
		case 'a':
			all = true;
			break;
		case 'f':
			full = true;
			break;
		case 'F':
			force = true;
			break;
		case 's':
			arg = arg_strtou64(optarg);
			if (arg >= BTRFS_SUPER_MIRROR_MAX) {
				error("super mirror too big: %llu", arg);
				return 1;
			}
			sb_bytenr = btrfs_sb_offset(arg);
			all = false;
			break;
		case GETOPT_VAL_BYTENR:
			sb_bytenr = arg_strtou64(optarg);
			all = false;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}
	if (check_argc_min(argc - optind, 1))
		return 1;

	for (i = optind; i < argc; i++) {
		const char *filename = argv[i];
		int fd = open(filename, O_RDONLY);

		if (fd < 0) {
			error("cannot open %s: %s", filename, strerror(errno));
			return 1;
		}
		if (all) {
			int mirror;

			for (mirror = 0; mirror < BTRFS_SUPER_MIRROR_MAX;
			    mirror++) {
				sb_bytenr = btrfs_sb_offset(mirror);
				if (load_and_dump_sb(filename, fd, sb_bytenr,
				    full, force) != 0) {
					close(fd);
					return 1;
				}
			}
		} else if (load_and_dump_sb(filename, fd, sb_bytenr, full,
		    force) != 0) {
			close(fd);
			return 1;
		}
		close(fd);
	}
	return 0;
}
DEFINE_SIMPLE_COMMAND(inspect_dump_super, "dump-super");
