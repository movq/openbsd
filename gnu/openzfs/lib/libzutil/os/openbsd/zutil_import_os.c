// SPDX-License-Identifier: CDDL-1.0

#include <sys/types.h>
#include <sys/dkio.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/vdev_impl.h>

#include <libzutil.h>

#include "zutil_import.h"

void
update_vdev_config_dev_strs(nvlist_t *nv)
{
	/* OpenBSD has no persistent devid or physical-path namespace. */
	(void)nvlist_remove_all(nv, ZPOOL_CONFIG_DEVID);
	(void)nvlist_remove_all(nv, ZPOOL_CONFIG_PHYS_PATH);
}

void
zpool_open_func(void *arg)
{
	rdsk_node_t *rn = arg;
	struct stat64 sb;
	nvlist_t *config;
	int fd, num_labels;

	fd = open(rn->rn_name, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd == -1)
		return;
	if (fstat64_blk(fd, &sb) == -1)
		goto out;
	if (S_ISREG(sb.st_mode)) {
		if (sb.st_size < SPA_MINDEVSIZE)
			goto out;
	} else if (!S_ISBLK(sb.st_mode) || sb.st_size < SPA_MINDEVSIZE) {
		goto out;
	}

	if (zpool_read_label(fd, &config, &num_labels) != 0)
		goto out;
	if (num_labels == 0) {
		nvlist_free(config);
		goto out;
	}

	rn->rn_config = config;
	rn->rn_num_labels = num_labels;
out:
	(void)close(fd);
}

static const char * const zpool_default_import_path[] = {
	"/dev"
};

const char * const *
zpool_default_search_paths(size_t *count)
{
	*count = nitems(zpool_default_import_path);
	return (zpool_default_import_path);
}

/*
 * OpenBSD has no blkid or GEOM inventory.  Build the default candidate set
 * from block-device nodes in /dev; zpool_open_func() performs the definitive
 * ZFS-label check.  Character-device aliases are intentionally omitted since
 * the OpenBSD kernel vdev implementation opens VBLK vnodes.
 */
int
zpool_find_import_blkid(libpc_handle_t *hdl, pthread_mutex_t *lock,
    avl_tree_t **slice_cache)
{
	avl_tree_t *cache;
	avl_index_t where;
	struct dirent *dp;
	struct stat sb;
	rdsk_node_t *slice;
	char path[MAXPATHLEN];
	DIR *dirp;

	cache = zutil_alloc(hdl, sizeof (*cache));
	avl_create(cache, slice_cache_compare, sizeof (rdsk_node_t),
	    offsetof(rdsk_node_t, rn_node));
	*slice_cache = cache;

	dirp = opendir("/dev");
	if (dirp == NULL) {
		int error = errno;

		free(cache);
		*slice_cache = NULL;
		return (error);
	}

	while ((dp = readdir(dirp)) != NULL) {
		if (dp->d_name[0] == '.' && (dp->d_name[1] == '\0' ||
		    (dp->d_name[1] == '.' && dp->d_name[2] == '\0')))
			continue;
		if (snprintf(path, sizeof (path), "/dev/%s", dp->d_name) >=
		    sizeof (path))
			continue;
		if (stat(path, &sb) == -1 || !S_ISBLK(sb.st_mode))
			continue;

		slice = zutil_alloc(hdl, sizeof (*slice));
		slice->rn_name = zutil_strdup(hdl, path);
		slice->rn_vdev_guid = 0;
		slice->rn_lock = lock;
		slice->rn_avl = cache;
		slice->rn_hdl = hdl;
		slice->rn_labelpaths = B_FALSE;
		slice->rn_order = IMPORT_ORDER_DEFAULT;

		pthread_mutex_lock(lock);
		if (avl_find(cache, slice, &where) != NULL) {
			free(slice->rn_name);
			free(slice);
		} else {
			avl_insert(cache, slice, where);
		}
		pthread_mutex_unlock(lock);
	}

	(void)closedir(dirp);
	return (0);
}

int
zfs_dev_flush(int fd)
{
	int force = 0;

	return (ioctl(fd, DIOCCACHESYNC, &force));
}

void
update_vdev_config_dev_sysfs_path(nvlist_t *nv, const char *path,
    const char *key)
{
	(void)nv;
	(void)path;
	(void)key;
}

void
update_vdevs_config_dev_sysfs_path(nvlist_t *config)
{
	(void)config;
}

int
zpool_disk_wait(const char *path)
{
	return (zpool_label_disk_wait(path, DISK_LABEL_WAIT));
}
