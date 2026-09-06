/*
 * Copyright (C) 2012 Hugo Mills.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License v2.
 *
 * Reduced OpenBSD btrfs command frontend.  Offline inspect-internal
 * commands are available; mounted-filesystem commands are stubbed.
 */

#include "kerncompat.h"
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "kernel-shared/volumes.h"
#include "crypto/hash.h"
#include "common/cpu-utils.h"
#include "common/help.h"
#include "common/messages.h"
#include "common/string-utils.h"
#include "common/utils.h"
#include "cmds/commands.h"

static const char * const btrfs_cmd_group_usage[] = {
	"btrfs [global options] <group> <command> [options] [<args>]\n"
	"\n"
	"Global options:\n"
	"  -v, --verbose      increase verbosity\n"
	"  -q, --quiet        print only errors\n"
	"  --help             print help\n"
	"  --version          print version",
	NULL
};

static const char btrfs_cmd_group_info[] =
	"Offline inspection is available through 'btrfs inspect-internal'.\n"
	"Commands that require Linux Btrfs ioctls are not supported on OpenBSD.";

static const char *
skip_prefix(const char *str, const char *prefix)
{
	size_t len = strlen(prefix);

	return strncmp(str, prefix, len) == 0 ? str + len : NULL;
}

static int
parse_one_token(const char *arg, const struct cmd_group *group,
    const struct cmd_struct **cmd_ret)
{
	const struct cmd_struct *abbrev = NULL;
	bool ambiguous = false;
	int i;

	for (i = 0; group->commands[i] != NULL; i++) {
		const struct cmd_struct *cmd = group->commands[i];
		const char *rest = skip_prefix(arg, cmd->token);

		if (rest != NULL && *rest == '\0') {
			*cmd_ret = cmd;
			return 0;
		}
		if (string_has_prefix(cmd->token, arg)) {
			ambiguous = abbrev != NULL;
			abbrev = cmd;
		}
	}
	if (ambiguous)
		return -2;
	if (abbrev != NULL) {
		*cmd_ret = abbrev;
		return 0;
	}
	return -1;
}

static const struct cmd_struct *
parse_command_token(const char *arg, const struct cmd_group *group)
{
	const struct cmd_struct *cmd = NULL;
	int ret;

	ret = parse_one_token(arg, group, &cmd);
	if (ret == -1)
		help_unknown_token(arg, group);
	if (ret == -2)
		help_ambiguous_token(arg, group);
	return cmd;
}

static void
handle_help_option(const struct cmd_struct *cmd, int argc, char **argv)
{
	if (argc < 2 || strcmp(argv[1], "--help") != 0)
		return;
	if (cmd->next != NULL)
		help_command_group(cmd->next, argc - 1, argv + 1);
	else
		usage_command(cmd, true, false);
	exit(0);
}

int
handle_command_group(const struct cmd_struct *cmd, int argc, char **argv)
{
	const struct cmd_struct *subcmd;

	argc--;
	argv++;
	if (argc < 1) {
		usage_command_group(cmd->next, false, false);
		return 1;
	}
	subcmd = parse_command_token(argv[0], cmd->next);
	handle_help_option(subcmd, argc, argv);
	fixup_argv0(argv, subcmd->token);
	return cmd_execute(subcmd, argc, argv);
}

static const struct cmd_group btrfs_cmd_group;

static const char * const cmd_help_usage[] = {
	"btrfs help",
	"Display help information",
	NULL
};

static int
cmd_help(const struct cmd_struct *cmd, int argc, char **argv)
{
	(void)cmd;
	help_command_group(&btrfs_cmd_group, argc, argv);
	return 0;
}
static DEFINE_SIMPLE_COMMAND(help, "help");

static const char * const cmd_version_usage[] = {
	"btrfs version",
	"Display btrfs-progs version",
	NULL
};

static int
cmd_version(const struct cmd_struct *cmd, int argc, char **argv)
{
	(void)cmd;
	(void)argc;
	(void)argv;
	help_builtin_features("");
	return 0;
}
static DEFINE_SIMPLE_COMMAND(version, "version");

static const struct cmd_group btrfs_cmd_group = {
	btrfs_cmd_group_usage,
	btrfs_cmd_group_info,
	{
		&cmd_struct_balance,
		&cmd_struct_check,
		&cmd_struct_device,
		&cmd_struct_filesystem,
		&cmd_struct_inspect,
		&cmd_struct_property,
		&cmd_struct_qgroup,
		&cmd_struct_quota,
		&cmd_struct_receive,
		&cmd_struct_replace,
		&cmd_struct_rescue,
		&cmd_struct_restore,
		&cmd_struct_scrub,
		&cmd_struct_send,
		&cmd_struct_subvolume,
		&cmd_struct_help,
		&cmd_struct_version,
		NULL
	}
};

static int
handle_global_options(int argc, char **argv)
{
	enum { OPT_HELP = 256, OPT_VERSION };
	static const struct option long_options[] = {
		{ "help", no_argument, NULL, OPT_HELP },
		{ "version", no_argument, NULL, OPT_VERSION },
		{ "verbose", no_argument, NULL, 'v' },
		{ "quiet", no_argument, NULL, 'q' },
		{ NULL, 0, NULL, 0 }
	};
	int c;

	opterr = 0;
	while ((c = getopt_long(argc, argv, "+vq", long_options, NULL)) != -1) {
		switch (c) {
		case OPT_HELP:
			help_command_group(&btrfs_cmd_group, argc, argv);
			exit(0);
		case OPT_VERSION:
			help_builtin_features("");
			exit(0);
		case 'v':
			bconf_be_verbose();
			break;
		case 'q':
			bconf_be_quiet();
			break;
		default:
			error("unknown global option: %s", argv[optind - 1]);
			exit(1);
		}
	}
	c = optind;
	optind = 1;
	return c;
}

int
main(int argc, char **argv)
{
	const struct cmd_struct *cmd;
	int shift;
	int ret;

	btrfs_config_init();
	shift = handle_global_options(argc, argv);
	argc -= shift;
	argv += shift;
	if (argc < 1) {
		usage_command_group_short(&btrfs_cmd_group);
		return 1;
	}

	cmd = parse_command_token(argv[0], &btrfs_cmd_group);
	handle_help_option(cmd, argc, argv);
	cpu_detect_flags();
	hash_init_accel();
	fixup_argv0(argv, cmd->token);
	ret = cmd_execute(cmd, argc, argv);
	btrfs_close_all_devices();
	return ret;
}
