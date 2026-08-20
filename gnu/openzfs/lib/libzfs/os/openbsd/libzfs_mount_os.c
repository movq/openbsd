// SPDX-License-Identifier: CDDL-1.0

#include <sys/types.h>
#include <sys/mount.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <libzfs.h>
#include <sys/mntent.h>

static void
zfs_mount_option(const char *option, int *mntflags)
{
	if (strcmp(option, MNTOPT_RO) == 0)
		*mntflags |= MNT_RDONLY;
	else if (strcmp(option, MNTOPT_RW) == 0)
		*mntflags &= ~MNT_RDONLY;
	else if (strcmp(option, MNTOPT_NOEXEC) == 0)
		*mntflags |= MNT_NOEXEC;
	else if (strcmp(option, MNTOPT_EXEC) == 0)
		*mntflags &= ~MNT_NOEXEC;
	else if (strcmp(option, MNTOPT_NOSETUID) == 0)
		*mntflags |= MNT_NOSUID;
	else if (strcmp(option, MNTOPT_SETUID) == 0)
		*mntflags &= ~MNT_NOSUID;
	else if (strcmp(option, MNTOPT_NODEVICES) == 0)
		*mntflags |= MNT_NODEV;
	else if (strcmp(option, MNTOPT_DEVICES) == 0)
		*mntflags &= ~MNT_NODEV;
	else if (strcmp(option, MNTOPT_NOATIME) == 0)
		*mntflags |= MNT_NOATIME;
	else if (strcmp(option, MNTOPT_ATIME) == 0)
		*mntflags &= ~MNT_NOATIME;
	else if (strcmp(option, MNTOPT_SYNC) == 0)
		*mntflags |= MNT_SYNCHRONOUS;
	else if (strcmp(option, MNTOPT_ASYNC) == 0)
		*mntflags |= MNT_ASYNC;
	else if (strcmp(option, MNTOPT_REMOUNT) == 0)
		*mntflags |= MNT_UPDATE;
}

int
do_mount(zfs_handle_t *zhp, const char *mntpt, const char *opts, int flags)
{
	struct zfs_args args;
	char *copy, *option, *last;
	int mntflags = 0;

	(void)flags;
	copy = strdup(opts);
	if (copy == NULL)
		return (ENOMEM);
	for (option = strtok_r(copy, ",", &last); option != NULL;
	    option = strtok_r(NULL, ",", &last))
		zfs_mount_option(option, &mntflags);
	free(copy);

	args.fspec = (char *)zfs_get_name(zhp);
	if (mount(MNTTYPE_ZFS, mntpt, mntflags, &args) == -1)
		return (errno);
	return (0);
}

int
do_unmount(zfs_handle_t *zhp, const char *mntpt, int flags)
{
	(void)zhp;
	if (unmount(mntpt, flags) == -1)
		return (errno);
	return (0);
}

int
zfs_mount_setattr(zfs_handle_t *zhp, uint32_t nspflags)
{
	(void)zhp;
	(void)nspflags;
	return (ENOTSUP);
}

int
zfs_mount_delegation_check(void)
{
	return (0);
}

void
zpool_disable_datasets_os(zpool_handle_t *zhp, boolean_t force)
{
	(void)zhp;
	(void)force;
}

void
zpool_disable_volume_os(const char *name)
{
	(void)name;
}
