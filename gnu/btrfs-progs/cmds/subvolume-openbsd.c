/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * OpenBSD subvolume frontend, using the btrfs-progs command framework.
 */
#include "kerncompat.h"
#include <sys/ioctl.h>
#include <sys/btrfsio.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "common/help.h"
#include "common/messages.h"
#include "cmds/commands.h"

static int
subvolume_command(const struct cmd_struct *cmd, int argc, char **argv)
{
	struct btrfs_ioctl_subvolume args = { 0 };
	unsigned long request;
	uint64_t cursor;
	int control, mountfd, ret = 1, first = 1, readonly = 0, expected;

	if (strcmp(cmd->token, "snapshot") == 0) {
		request = BTRFSIOC_SNAPSHOT;
		if (argc > first && strcmp(argv[first], "-r") == 0) {
			readonly = 1;
			first++;
		}
		expected = 3;
	} else if (strcmp(cmd->token, "create") == 0) {
		request = BTRFSIOC_CREATE;
		expected = 2;
	} else if (strcmp(cmd->token, "delete") == 0) {
		request = BTRFSIOC_DELETE;
		expected = 2;
	} else {
		request = BTRFSIOC_LIST;
		expected = 1;
	}
	if (argc > first && strcmp(argv[first], "--") == 0)
		first++;
	if (argc - first != expected) {
		usage_command(cmd, false, false);
		return 1;
	}
	mountfd = open(argv[first], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (mountfd == -1) {
		error("cannot open %s: %s", argv[first], strerror(errno));
		return 1;
	}
	args.fd = mountfd;
	args.flags = readonly ? BTRFS_CTL_RDONLY : 0;
	if (request == BTRFSIOC_SNAPSHOT &&
	    strlcpy(args.source, argv[first + 1], sizeof(args.source)) >=
	    sizeof(args.source)) {
		error("source path is too long");
		goto close_mount;
	}
	if (request != BTRFSIOC_LIST &&
	    strlcpy(args.path, argv[first + expected - 1], sizeof(args.path)) >=
	    sizeof(args.path)) {
		error("destination path is too long");
		goto close_mount;
	}
	control = open("/dev/btrfs-control",
	    (request == BTRFSIOC_LIST ? O_RDONLY : O_RDWR) | O_CLOEXEC);
	if (control == -1) {
		error("cannot open /dev/btrfs-control: %s", strerror(errno));
		goto close_mount;
	}
	for (;;) {
		if (ioctl(control, request, &args) == -1) {
			if (request == BTRFSIOC_LIST && errno == ENOENT)
				ret = 0;
			else {
				error("subvolume %s: %s", cmd->token, strerror(errno));
				ret = 1;
			}
			break;
		}
		ret = 0;
		if (request != BTRFSIOC_LIST)
			break;
		printf("ID %" PRIu64 " top level %" PRIu64 " %s path %s\n",
		    args.id, args.parent,
		    args.flags & BTRFS_CTL_RDONLY ? "ro" : "rw", args.path);
		cursor = args.cursor;
		memset(&args, 0, sizeof(args));
		args.fd = mountfd;
		args.cursor = cursor;
	}
	close(control);
close_mount:
	close(mountfd);
	return ret;
}

static const char * const subvolume_list_usage[] = {
	"btrfs subvolume list <mountpoint>",
	"List subvolumes throughout the selected filesystem",
	NULL
};
static const char * const subvolume_create_usage[] = {
	"btrfs subvolume create <mountpoint> <path>",
	"Create a subvolume at a path relative to the filesystem root",
	NULL
};
static const char * const subvolume_delete_usage[] = {
	"btrfs subvolume delete <mountpoint> <path>",
	"Delete a subvolume at a path relative to the filesystem root",
	NULL
};
static const char * const subvolume_snapshot_usage[] = {
	"btrfs subvolume snapshot [-r] <mountpoint> <source> <path>",
	"Create a snapshot using filesystem-root-relative source and destination",
	"",
	"-r  create a read-only snapshot",
	NULL
};

DEFINE_COMMAND(subvolume_list, "list", subvolume_command,
    subvolume_list_usage, NULL, 0);
static DEFINE_COMMAND(subvolume_create, "create", subvolume_command,
    subvolume_create_usage, NULL, 0);
static DEFINE_COMMAND(subvolume_delete, "delete", subvolume_command,
    subvolume_delete_usage, NULL, 0);
static DEFINE_COMMAND(subvolume_snapshot, "snapshot", subvolume_command,
    subvolume_snapshot_usage, NULL, 0);

static const char * const subvolume_usage[] = {
	"btrfs subvolume <command> ...",
	NULL
};
static const struct cmd_group subvolume_group = {
	subvolume_usage,
	"The mountpoint selects the filesystem. All subvolume paths start at\n"
	"the top-level subvolume (tree 5), including when only a child is mounted.\n"
	"Administrative commands require root.",
	{
		&cmd_struct_subvolume_list,
		&cmd_struct_subvolume_create,
		&cmd_struct_subvolume_delete,
		&cmd_struct_subvolume_snapshot,
		NULL
	}
};
DEFINE_COMMAND(subvolume, "subvolume", handle_command_group,
    subvolume_usage, &subvolume_group, 0);
