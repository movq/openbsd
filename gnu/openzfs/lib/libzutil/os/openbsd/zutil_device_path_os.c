// SPDX-License-Identifier: CDDL-1.0

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/dkio.h>

#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libzutil.h>

/*
 * OpenBSD's raw partition (normally 'c') names the complete device.  It is
 * already the path ZFS must use, so there is no child partition to append or
 * remove.  In particular, ZFS must not install or modify an OpenBSD disklabel
 * when a raw-partition vdev is prepared.
 */
char *
zfs_strip_partition(const char *dev)
{
	return (strdup(dev));
}

int
zfs_append_partition(char *path, size_t max_len)
{
	return (strnlen(path, max_len));
}

const char *
zfs_strip_path(const char *path)
{
	if (strncmp(path, _PATH_DEV, sizeof (_PATH_DEV) - 1) == 0)
		return (path + sizeof (_PATH_DEV) - 1);
	return (path);
}

char *
zfs_get_underlying_path(const char *dev_name)
{
	if (dev_name == NULL)
		return (NULL);
	return (realpath(dev_name, NULL));
}

boolean_t
zfs_dev_is_whole_disk(const char *dev_name)
{
	struct disklabel dl;
	struct stat sb;
	int fd;

	if (stat(dev_name, &sb) == -1 || !S_ISBLK(sb.st_mode) ||
	    DISKPART(sb.st_rdev) != RAW_PART)
		return (B_FALSE);

	fd = open(dev_name, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd == -1)
		return (B_FALSE);
	if (ioctl(fd, DIOCGDINFO, &dl) == -1) {
		(void)close(fd);
		return (B_FALSE);
	}
	(void)close(fd);
	return (B_TRUE);
}

int
zpool_label_disk_wait(const char *path, int timeout_ms)
{
	hrtime_t start = gethrtime();
	struct stat sb;

	do {
		if (stat(path, &sb) == 0)
			return (0);
		if (errno != ENOENT)
			return (errno);
		(void)usleep(10 * MILLISEC);
	} while (NSEC2MSEC(gethrtime() - start) < timeout_ms);

	return (ENODEV);
}

boolean_t
is_mpath_whole_disk(const char *path)
{
	(void)path;
	return (B_FALSE);
}
