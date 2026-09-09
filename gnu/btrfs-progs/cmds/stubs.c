/*
 * Command groups outside the initial offline inspection port.
 */

#include "kerncompat.h"
#include <errno.h>
#include "common/messages.h"
#include "cmds/commands.h"

static int
cmd_unsupported(const struct cmd_struct *cmd, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	error("%s is not included in this initial OpenBSD btrfs port",
	    cmd->token);
	return EOPNOTSUPP;
}

#define DEFINE_UNSUPPORTED(name, token)					\
	static const char * const cmd_ ## name ## _usage[] = {		\
		"btrfs " token " ...",					\
		"Not included in this initial OpenBSD port",		\
		NULL							\
	};								\
	DEFINE_COMMAND(name, token, cmd_unsupported,			\
	    cmd_ ## name ## _usage, NULL, 0)

DEFINE_UNSUPPORTED(balance, "balance");
DEFINE_UNSUPPORTED(check, "check");
DEFINE_UNSUPPORTED(filesystem, "filesystem");
DEFINE_UNSUPPORTED(property, "property");
DEFINE_UNSUPPORTED(qgroup, "qgroup");
DEFINE_UNSUPPORTED(quota, "quota");
DEFINE_UNSUPPORTED(replace, "replace");
DEFINE_UNSUPPORTED(rescue, "rescue");
DEFINE_UNSUPPORTED(restore, "restore");
DEFINE_UNSUPPORTED(scrub, "scrub");
