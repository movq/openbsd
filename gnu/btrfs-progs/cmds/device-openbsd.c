/* Public domain. */
#include "kerncompat.h"
#include <sys/ioctl.h>
#include <sys/btrfsio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "common/help.h"
#include "common/messages.h"
#include "cmds/commands.h"

static int
device_command(const struct cmd_struct *cmd, int argc, char **argv)
{
	struct btrfs_ioctl_device args;
	unsigned long request;
	unsigned int flags = 0;
	const char *why;
	int first = 1, control, mountfd, ret = 0, i;

	request = strcmp(cmd->token, "add") == 0 ?
	    BTRFSIOC_DEV_ADD : BTRFSIOC_DEV_REMOVE;
	if (request == BTRFSIOC_DEV_ADD && argc > first &&
	    strcmp(argv[first], "-f") == 0) {
		flags = BTRFS_DEVICE_FORCE;
		first++;
	}
	if (argc > first && strcmp(argv[first], "--") == 0)
		first++;
	if (argc - first < 2) {
		usage_command(cmd, false, false);
		return 1;
	}
	mountfd = open(argv[argc - 1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (mountfd == -1) {
		error("cannot open %s: %s", argv[argc - 1], strerror(errno));
		return 1;
	}
	control = open("/dev/btrfs-control", O_RDWR | O_CLOEXEC);
	if (control == -1) {
		error("cannot open /dev/btrfs-control: %s", strerror(errno));
		close(mountfd);
		return 1;
	}
	for (i = first; i < argc - 1; i++) {
		memset(&args, 0, sizeof(args));
		args.fd = mountfd;
		args.flags = flags;
		if (request == BTRFSIOC_DEV_REMOVE &&
		    strspn(argv[i], "0123456789") == strlen(argv[i])) {
			args.devid = strtonum(argv[i], 1, LLONG_MAX, &why);
			if (why != NULL) {
				error("invalid device ID %s: %s", argv[i], why);
				ret = 1;
				break;
			}
		} else if (strlcpy(args.path, argv[i], sizeof(args.path)) >=
		    sizeof(args.path)) {
			error("device path is too long");
			ret = 1;
			break;
		}
		if (ioctl(control, request, &args) == -1) {
			error("device %s %s: %s", cmd->token, argv[i],
			    strerror(errno));
			ret = 1;
			break;
		}
	}
	close(control);
	close(mountfd);
	return ret;
}

static const char * const device_add_usage[] = {
	"btrfs device add [-f] <device> [<device>...] <mountpoint>",
	"Add block devices to a mounted filesystem",
	"",
	"-f  overwrite existing device signatures",
	NULL
};
static const char * const device_remove_usage[] = {
	"btrfs device remove <device|devid> [<device|devid>...] <mountpoint>",
	"Relocate allocated chunks and remove devices from a mounted filesystem",
	NULL
};
static DEFINE_COMMAND(device_add, "add", device_command,
    device_add_usage, NULL, 0);
static DEFINE_COMMAND(device_remove, "remove", device_command,
    device_remove_usage, NULL, 0);
static const char * const device_usage[] = {
	"btrfs device <command> ...",
	NULL
};
static const struct cmd_group device_group = {
	device_usage,
	"Online SINGLE/DUP device administration. Requires root.",
	{
		&cmd_struct_device_add,
		&cmd_struct_device_remove,
		NULL
	}
};
DEFINE_COMMAND(device, "device", handle_command_group,
    device_usage, &device_group, 0);
