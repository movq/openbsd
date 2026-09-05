/* Public domain. */

#include <sys/types.h>
#include <sys/mount.h>

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>

#include "mntopts.h"

static void	usage(void);

static const struct mntopt mopts[] = {
	MOPT_STDOPTS,
	MOPT_SYNC,
	{ NULL, 0, 0 }
};

int
main(int argc, char *argv[])
{
	struct btrfs_args args;
	char mountpoint[PATH_MAX];
	int ch, mntflags = 0;

	while ((ch = getopt(argc, argv, "o:")) != -1) {
		switch (ch) {
		case 'o':
			getmntopts(optarg, mopts, &mntflags);
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc != 2)
		usage();
	if (realpath(argv[1], mountpoint) == NULL)
		err(1, "realpath %s", argv[1]);

	args.fspec = argv[0];
	if (mount(MOUNT_BTRFS, mountpoint, mntflags, &args) == -1)
		err(1, "%s on %s", args.fspec, mountpoint);

	return (0);
}

static void
usage(void)
{
	fprintf(stderr, "usage: mount_btrfs [-o options] special node\n");
	exit(1);
}
