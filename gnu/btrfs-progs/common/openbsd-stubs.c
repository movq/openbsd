/*
 * OpenBSD host integration for the offline btrfs userspace implementation.
 */

#include "kerncompat.h"
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include "kernel-lib/overflow.h"
#include "kernel-lib/list.h"
#include "kernel-shared/accessors.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/zoned.h"
#include "common/device-scan.h"
#include "common/messages.h"
#include "common/open-utils.h"
#include "common/units.h"
#include "libbtrfsutil/btrfsutil.h"

/*
 * There is no system-wide btrfs device registry on OpenBSD.  Offline mkfs
 * already has every target open, so scanning and registration are unnecessary.
 */
int
btrfs_scan_devices(int verbose)
{
	(void)verbose;
	return 0;
}

int
btrfs_scan_argv_devices(int dev_optind, int argc, char **argv)
{
	while (dev_optind < argc) {
		struct btrfs_fs_devices *fs_devices;
		u64 num_devices;
		int fd;
		int ret;

		fd = open(argv[dev_optind], O_RDONLY);
		if (fd < 0) {
			ret = errno;
			error("cannot open %s: %s", argv[dev_optind],
			    strerror(ret));
			return -ret;
		}
		ret = btrfs_scan_one_device(fd, argv[dev_optind], &fs_devices,
		    &num_devices, BTRFS_SUPER_INFO_OFFSET, SBREAD_DEFAULT);
		close(fd);
		if (ret < 0) {
			error("device scan of %s failed: %s", argv[dev_optind],
			    strerror(-ret));
			return ret;
		}
		dev_optind++;
	}
	return 0;
}

int
btrfs_register_one_device(const char *fname)
{
	(void)fname;
	return 0;
}

/*
 * Without libblkid there is no authoritative index to query.  Collision
 * checks against the selected target devices are still done by mkfs.
 */
int
test_uuid_unique(const char *uuid_str)
{
	static bool warned;

	(void)uuid_str;
	if (!warned) {
		warning("global UUID uniqueness cannot be checked on OpenBSD");
		warned = true;
	}
	return 1;
}

int
btrfs_device_already_in_root(struct btrfs_root *root, int fd,
    int super_offset)
{
	struct btrfs_super_block disk_super;
	int ret;

	ret = sbread(fd, &disk_super, super_offset);
	if (ret != BTRFS_SUPER_INFO_SIZE)
		return 0;
	if (btrfs_super_magic(&disk_super) != BTRFS_MAGIC &&
	    btrfs_super_magic(&disk_super) != BTRFS_MAGIC_TEMPORARY)
		return 0;

	return memcmp(disk_super.fsid, root->fs_info->super_copy->fsid,
	    BTRFS_FSID_SIZE) == 0;
}

int
btrfs_add_to_fsid(struct btrfs_trans_handle *trans, struct btrfs_root *root,
    int fd, const char *path, u64 device_total_bytes, u32 io_width,
    u32 io_align, u32 sectorsize)
{
	struct btrfs_super_block *disk_super;
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_super_block *super = fs_info->super_copy;
	struct btrfs_device *device;
	struct btrfs_dev_item *dev_item;
	char *buf = NULL;
	const u64 old_size = btrfs_super_total_bytes(super);
	u64 new_size;
	u64 num_devs;
	int ret;

	device_total_bytes = (device_total_bytes / sectorsize) * sectorsize;
	device = calloc(1, sizeof(*device));
	if (device == NULL)
		return -ENOMEM;

	buf = calloc(1, BTRFS_SUPER_INFO_SIZE);
	if (buf == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	disk_super = (struct btrfs_super_block *)buf;
	dev_item = &disk_super->dev_item;
	uuid_generate(device->uuid);
	device->fs_info = fs_info;
	device->devid = 0;
	device->type = 0;
	device->io_width = io_width;
	device->io_align = io_align;
	device->sector_size = sectorsize;
	device->fd = fd;
	device->writeable = 1;
	device->total_bytes = device_total_bytes;
	device->bytes_used = 0;
	device->total_ios = 0;
	device->dev_root = fs_info->dev_root;
	device->name = strdup(path);
	if (device->name == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	if (check_add_overflow(old_size, device_total_bytes, &new_size)) {
		error("adding device of %llu (%s) bytes would exceed max file system size",
		    device->total_bytes, pretty_size(device->total_bytes));
		ret = -EOVERFLOW;
		goto out;
	}

	INIT_LIST_HEAD(&device->dev_list);
	ret = btrfs_add_device(trans, fs_info, device);
	if (ret != 0)
		goto out;

	btrfs_set_super_total_bytes(super, new_size);
	num_devs = btrfs_super_num_devices(super) + 1;
	btrfs_set_super_num_devices(super, num_devs);
	memcpy(disk_super, super, sizeof(*disk_super));

	btrfs_set_super_bytenr(disk_super, BTRFS_SUPER_INFO_OFFSET);
	btrfs_set_stack_device_id(dev_item, device->devid);
	btrfs_set_stack_device_type(dev_item, device->type);
	btrfs_set_stack_device_io_align(dev_item, device->io_align);
	btrfs_set_stack_device_io_width(dev_item, device->io_width);
	btrfs_set_stack_device_sector_size(dev_item, device->sector_size);
	btrfs_set_stack_device_total_bytes(dev_item, device->total_bytes);
	btrfs_set_stack_device_bytes_used(dev_item, device->bytes_used);
	memcpy(&dev_item->uuid, device->uuid, BTRFS_UUID_SIZE);

	ret = sbwrite(fd, buf, BTRFS_SUPER_INFO_OFFSET);
	if (ret != BTRFS_SUPER_INFO_SIZE) {
		error_msg(ERROR_MSG_WRITE, "superblock when adding device: %m");
		ret = -EIO;
		goto out;
	}

	free(buf);
	list_add(&device->dev_list, &fs_info->fs_devices->devices);
	device->fs_devices = fs_info->fs_devices;
	return 0;

out:
	free(device->zone_info);
	free(device->name);
	free(device);
	free(buf);
	return ret;
}

/*
 * These interfaces inspect a mounted Linux btrfs filesystem.  They remain
 * linkable because shared parsers reference them, but mkfs does not call them.
 */
enum btrfs_util_error
btrfs_util_subvolume_is_valid(const char *path)
{
	(void)path;
	errno = EOPNOTSUPP;
	return BTRFS_UTIL_ERROR_STOP_ITERATION;
}

int
lookup_path_rootid(int fd, u64 *rootid)
{
	(void)fd;
	(void)rootid;
	return -EOPNOTSUPP;
}

int
btrfs_open_file_or_dir(const char *path)
{
	(void)path;
	return -EOPNOTSUPP;
}

int
sysfs_open_file(const char *name)
{
	(void)name;
	return -ENOENT;
}

int
sysfs_open_fsid_file(int fd, const char *filename)
{
	(void)fd;
	(void)filename;
	return -ENOENT;
}

int
sysfs_read_file(int fd, char *buf, size_t size)
{
	(void)fd;
	if (size != 0)
		buf[0] = '\0';
	return -ENOENT;
}

int
sysfs_read_fsid_file_clean_str(int fd, const char *name, char *buf,
    size_t size)
{
	(void)fd;
	(void)name;
	if (size != 0)
		buf[0] = '\0';
	return -ENOENT;
}
