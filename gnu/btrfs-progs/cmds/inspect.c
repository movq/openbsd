/*
 * Reduced OpenBSD inspect-internal command group.
 */

#include "kerncompat.h"
#include <errno.h>
#include "common/messages.h"
#include "cmds/commands.h"

static const char * const inspect_cmd_group_usage[] = {
	"btrfs inspect-internal <command> <args>",
	NULL
};

static const char inspect_cmd_group_info[] =
	"Inspect an unmounted Btrfs filesystem directly from userspace.";

static int
cmd_inspect_unsupported(const struct cmd_struct *cmd, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	error("inspect-internal %s requires Linux Btrfs ioctls and is not "
	    "supported on OpenBSD", cmd->token);
	return EOPNOTSUPP;
}

#define DEFINE_UNSUPPORTED_INSPECT(name, token)				\
	static const char * const cmd_ ## name ## _usage[] = {		\
		"btrfs inspect-internal " token " ...",			\
		"Requires Linux Btrfs ioctls and is not supported",	\
		NULL							\
	};								\
	DEFINE_COMMAND(name, token, cmd_inspect_unsupported,		\
	    cmd_ ## name ## _usage, NULL, 0)

DEFINE_UNSUPPORTED_INSPECT(inspect_inode_resolve, "inode-resolve");
DEFINE_UNSUPPORTED_INSPECT(inspect_list_chunks, "list-chunks");
DEFINE_UNSUPPORTED_INSPECT(inspect_logical_resolve, "logical-resolve");
DEFINE_UNSUPPORTED_INSPECT(inspect_map_swapfile, "map-swapfile");
DEFINE_UNSUPPORTED_INSPECT(inspect_min_dev_size, "min-dev-size");
DEFINE_UNSUPPORTED_INSPECT(inspect_rootid, "rootid");
DEFINE_UNSUPPORTED_INSPECT(inspect_subvolid_resolve,
    "subvolid-resolve");

static const struct cmd_group inspect_cmd_group = {
	inspect_cmd_group_usage,
	inspect_cmd_group_info,
	{
		&cmd_struct_inspect_dump_super,
		&cmd_struct_inspect_dump_tree,
		&cmd_struct_inspect_inode_resolve,
		&cmd_struct_inspect_list_chunks,
		&cmd_struct_inspect_logical_resolve,
		&cmd_struct_inspect_map_swapfile,
		&cmd_struct_inspect_min_dev_size,
		&cmd_struct_inspect_rootid,
		&cmd_struct_inspect_subvolid_resolve,
		&cmd_struct_inspect_tree_stats,
		NULL
	}
};

DEFINE_GROUP_COMMAND(inspect, "inspect-internal");
