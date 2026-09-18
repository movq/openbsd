// SPDX-License-Identifier: CDDL-1.0

#include <sys/types.h>

#include <errno.h>
#include <libintl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libzfs.h>
#include <libzutil.h>

#include "../../libzfs_impl.h"

static __thread char libzfs_errbuf[ERRBUFLEN];

const char *
libzfs_error_init(int error)
{
	(void)snprintf(libzfs_errbuf, sizeof (libzfs_errbuf), "%s",
	    zfs_strerror(error));
	return (libzfs_errbuf);
}

int
libzfs_load_module(void)
{
	if (access(ZFS_DEV, F_OK) == 0)
		return (0);
	return (errno);
}

int
find_shares_object(differ_info_t *di)
{
	(void)di;
	return (0);
}

int
zfs_destroy_snaps_nvl_os(libzfs_handle_t *hdl, nvlist_t *snaps)
{
	(void)hdl;
	(void)snaps;
	return (0);
}

/* OpenBSD raw-partition vdevs need no partition table or disklabel. */
int
zpool_relabel_disk(libzfs_handle_t *hdl, const char *path, const char *msg)
{
	(void)hdl;
	(void)path;
	(void)msg;
	return (0);
}

int
zpool_label_disk(libzfs_handle_t *hdl, zpool_handle_t *zhp, const char *name)
{
	(void)hdl;
	(void)zhp;
	(void)name;
	return (0);
}

char *
zfs_version_kernel(void)
{
	/* OpenBSD does not yet expose a module-version query interface. */
	return (strdup("unknown"));
}

int
zpool_nextboot(libzfs_handle_t *hdl, uint64_t pool_guid, uint64_t dev_guid,
    const char *command)
{
	(void)hdl;
	(void)pool_guid;
	(void)dev_guid;
	(void)command;
	errno = ENOTSUP;
	return (-1);
}
