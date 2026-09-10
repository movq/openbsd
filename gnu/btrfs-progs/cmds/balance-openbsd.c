/* Public domain. */
#include "kerncompat.h"
#include <sys/ioctl.h>
#include <sys/mount.h>
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
balance_filter(struct btrfs_balance_filter *filter, const char *text)
{
	char *copy = strdup(text), *rest = copy, *part, *range;
	const char *why;
	unsigned int seen = 0;
	int ret = 1;

	if (copy == NULL)
		return 1;
	filter->max = 100;
	filter->limit = UINT64_MAX;
	if (*text == '\0') {
		free(copy);
		return 0;
	}
	while ((part = strsep(&rest, ",")) != NULL) {
		if (strncmp(part, "usage=", 6) == 0 && !(seen & 1)) {
			seen |= 1;
			part += 6;
			range = strstr(part, "..");
			if (range != NULL) {
				*range = '\0';
				range += 2;
				if (*part != '\0') {
					filter->min = strtonum(part, 0, 100, &why);
					if (why != NULL)
						goto out;
				}
				part = range;
			}
			if (*part == '\0') {
				if (range == NULL)
					goto out;
			} else {
				filter->max = strtonum(part, 0, 100, &why);
				if (why != NULL)
					goto out;
			}
			if (filter->min > filter->max)
				goto out;
		} else if (strncmp(part, "limit=", 6) == 0 && !(seen & 2)) {
			seen |= 2;
			filter->limit = strtonum(part + 6, 0, LLONG_MAX, &why);
			if (why != NULL)
				goto out;
		} else
			goto out;
	}
	ret = 0;
out:
	if (ret)
		error("invalid or unsupported balance filter: %s", text);
	free(copy);
	return ret;
}

static int
balance_command(const struct cmd_struct *cmd, int argc, char **argv)
{
	struct btrfs_ioctl_balance args = { 0 }, status = { 0 };
	struct statfs *mounts;
	struct btrfs_balance_filter *filter;
	unsigned long request;
	unsigned int flag;
	int control, mountfd = -1, i = 1, ret = 0, saved, nmounts, n;
	size_t pathlen;

	request = strcmp(cmd->token, "start") == 0 ? BTRFSIOC_BALANCE :
	    strcmp(cmd->token, "status") == 0 ? BTRFSIOC_BALANCE_STATUS :
	    BTRFSIOC_BALANCE_CANCEL;
	if (request == BTRFSIOC_BALANCE) {
		for (; i < argc && argv[i][0] == '-'; i++) {
			if (strcmp(argv[i], "--") == 0) {
				i++;
				break;
			}
			if (strcmp(argv[i], "-f") == 0 ||
			    strcmp(argv[i], "--full-balance") == 0)
				continue;
			switch (argv[i][1]) {
			case 'd':
				flag = BTRFS_BALANCE_DATA;
				filter = &args.data;
				break;
			case 'm':
				flag = BTRFS_BALANCE_METADATA;
				filter = &args.metadata;
				break;
			case 's':
				flag = BTRFS_BALANCE_SYSTEM;
				filter = &args.system;
				break;
			default:
				error("unsupported balance option: %s", argv[i]);
				return 1;
			}
			if (args.flags & flag) {
				error("duplicate balance option: %s", argv[i]);
				return 1;
			}
			args.flags |= flag;
			if (balance_filter(filter, argv[i] + 2))
				return 1;
		}
		if (args.flags == 0) {
			args.flags = BTRFS_BALANCE_DATA | BTRFS_BALANCE_METADATA |
			    BTRFS_BALANCE_SYSTEM;
			args.data.max = args.metadata.max = args.system.max = 100;
			args.data.limit = args.metadata.limit =
			    args.system.limit = UINT64_MAX;
		} else if ((args.flags & BTRFS_BALANCE_METADATA) &&
		    !(args.flags & BTRFS_BALANCE_SYSTEM)) {
			args.flags |= BTRFS_BALANCE_SYSTEM;
			args.system = args.metadata;
		}
	}
	if (argc - i != 1) {
		usage_command(cmd, false, false);
		return 1;
	}
	/*
	 * Find an explicitly named mount without touching its vnodes: a writer
	 * sleeping behind balance can hold the root directory's vnode lock.
	 * Aliases and paths within a mount still use the descriptor interface.
	 */
	nmounts = getmntinfo(&mounts, MNT_NOWAIT);
	pathlen = strlen(argv[i]);
	while (pathlen > 1 && argv[i][pathlen - 1] == '/')
		pathlen--;
	for (n = 0; n < nmounts; n++)
		if (strlen(mounts[n].f_mntonname) == pathlen &&
		    strncmp(mounts[n].f_mntonname, argv[i], pathlen) == 0)
			break;
	if (n < nmounts)
		memcpy(args.fsid, mounts[n].f_fsid.val, sizeof(args.fsid));
	else {
		mountfd = open(argv[i], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (mountfd == -1) {
			error("cannot open %s: %s", argv[i], strerror(errno));
			return 1;
		}
	}
	control = open("/dev/btrfs-control", O_RDWR | O_CLOEXEC);
	if (control == -1) {
		error("cannot open /dev/btrfs-control: %s", strerror(errno));
		if (mountfd != -1)
			close(mountfd);
		return 1;
	}
	args.fd = mountfd;
	if (ioctl(control, request, &args) == -1) {
		saved = errno;
		if (request == BTRFSIOC_BALANCE_STATUS && saved == ENOTCONN)
			printf("No balance found on '%s'\n", argv[i]);
		else {
			error("balance %s: %s", cmd->token, strerror(saved));
			ret = 1;
		}
		if (request != BTRFSIOC_BALANCE)
			goto out;
		status.fd = mountfd;
		memcpy(status.fsid, args.fsid, sizeof(status.fsid));
		if (ioctl(control, BTRFSIOC_BALANCE_STATUS, &status) == -1)
			goto out;
		args = status;
	}
	if (request == BTRFSIOC_BALANCE_CANCEL)
		printf("Balance cancellation requested\n");
	else if (request == BTRFSIOC_BALANCE_STATUS) {
		printf("Balance %s on '%s'\n",
		    args.state & BTRFS_BALANCE_CANCELING ? "canceling" :
		    args.state & BTRFS_BALANCE_RUNNING ? "running" : "finished",
		    argv[i]);
		printf("%llu out of about %llu chunks relocated (%llu considered)\n",
		    (unsigned long long)args.completed,
		    (unsigned long long)args.expected,
		    (unsigned long long)args.considered);
		if (args.error)
			printf("Last balance error: %s\n", strerror(args.error));
	} else
		printf("%s, had to relocate %llu out of %llu chunks\n",
		    ret ? "Stopped" : "Done",
		    (unsigned long long)args.completed,
		    (unsigned long long)args.considered);
out:
	close(control);
	if (mountfd != -1)
		close(mountfd);
	return ret;
}

static const char * const balance_start_usage[] = {
	"btrfs balance start [-d[filters]] [-m[filters]] [-s[filters]] <mountpoint>",
	"Compact selected block groups, preserving their profiles",
	"",
	"filters: usage=N, usage=MIN..MAX, limit=N (comma separated)",
	"usage=0 selects empty groups; usage=100 includes full groups",
	"Metadata filters also apply to system groups unless -s is specified.",
	"Runs synchronously. Other filesystem operations wait during relocation.",
	NULL
};
static const char * const balance_status_usage[] = {
	"btrfs balance status <mountpoint>",
	"Show current or last balance progress",
	NULL
};
static const char * const balance_cancel_usage[] = {
	"btrfs balance cancel <mountpoint>",
	"Request cancellation at the next relocation transaction boundary",
	NULL
};
static DEFINE_COMMAND(balance_start, "start", balance_command,
    balance_start_usage, NULL, 0);
static DEFINE_COMMAND(balance_status, "status", balance_command,
    balance_status_usage, NULL, 0);
static DEFINE_COMMAND(balance_cancel, "cancel", balance_command,
    balance_cancel_usage, NULL, 0);
static const char * const balance_usage[] = {
	"btrfs balance <command> ...",
	NULL
};
static const struct cmd_group balance_group = {
	balance_usage,
	"Online SINGLE/DUP balance. Requires root.",
	{
		&cmd_struct_balance_start,
		&cmd_struct_balance_status,
		&cmd_struct_balance_cancel,
		NULL
	}
};

static int
balance_dispatch(const struct cmd_struct *cmd, int argc, char **argv)
{
	/* Linux also accepts "btrfs balance <mountpoint>". */
	if (argc == 2 && argv[1][0] != '-' &&
	    strcmp(argv[1], "start") && strcmp(argv[1], "status") &&
	    strcmp(argv[1], "cancel") && strcmp(argv[1], "pause") &&
	    strcmp(argv[1], "resume"))
		return balance_command(&cmd_struct_balance_start, argc, argv);
	return handle_command_group(cmd, argc, argv);
}
DEFINE_COMMAND(balance, "balance", balance_dispatch,
    balance_usage, &balance_group, 0);
