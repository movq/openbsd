// SPDX-License-Identifier: CDDL-1.0
/*
 * OpenBSD-specific zpool vdev helpers.
 */

#include <sys/types.h>
#include <sys/param.h>

#include <errno.h>
#include <paths.h>
#include <stdio.h>
#include <string.h>

#include <libzfs.h>

#include "zpool_util.h"

int
check_device(const char *name, boolean_t force, boolean_t isspare,
    boolean_t iswholedisk)
{
	char path[MAXPATHLEN];

	(void)iswholedisk;
	if (strncmp(name, _PATH_DEV, sizeof (_PATH_DEV) - 1) != 0)
		(void)snprintf(path, sizeof (path), "%s%s", _PATH_DEV, name);
	else
		(void)strlcpy(path, name, sizeof (path));

	return (check_file(path, force, isspare));
}

boolean_t
check_sector_size_database(char *path, int *sector_size)
{
	(void)path;
	(void)sector_size;
	return (B_FALSE);
}

void
after_zpool_upgrade(zpool_handle_t *zhp)
{
	(void)zhp;
}

int
check_file(const char *file, boolean_t force, boolean_t isspare)
{
	return (check_file_generic(file, force, isspare));
}

int
zpool_power_current_state(zpool_handle_t *zhp, char *vdev)
{
	(void)zhp;
	(void)vdev;
	return (-1);
}

int
zpool_power(zpool_handle_t *zhp, char *vdev, boolean_t turn_on)
{
	(void)zhp;
	(void)vdev;
	(void)turn_on;
	return (ENOTSUP);
}
