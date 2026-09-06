#include <sys/types.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "common/open-utils.h"

int
check_mounted(const char *file)
{
	struct statfs *mntbuf;
	char resolved[PATH_MAX];
	const char *path = file;
	int i, n;

	if (realpath(file, resolved) != NULL)
		path = resolved;
	n = getmntinfo(&mntbuf, MNT_NOWAIT);
	if (n == 0)
		return -errno;
	for (i = 0; i < n; i++)
		if (strcmp(path, mntbuf[i].f_mntfromname) == 0)
			return 1;
	return 0;
}

int
check_mounted_where(int fd, const char *file, char *where, size_t size,
    struct btrfs_fs_devices **fs_dev_ret, unsigned sbflags, bool noscan)
{
	int ret;

	(void)fd;
	(void)where;
	(void)size;
	(void)fs_dev_ret;
	(void)sbflags;
	(void)noscan;
	ret = check_mounted(file);
	return ret;
}
