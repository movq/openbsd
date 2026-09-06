#include "kerncompat.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/disklabel.h>
#include <sys/dkio.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "kernel-lib/sizes.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/volumes.h"
#include "common/device-utils.h"

int
device_discard_blocks(int fd, u64 start, u64 len)
{
	(void)fd;
	(void)start;
	(void)len;
	return EOPNOTSUPP;
}

int
device_zero_blocks(int fd, off_t start, size_t len, bool direct)
{
	unsigned char buf[64 * 1024] = { 0 };

	(void)direct;
	while (len != 0) {
		size_t chunk = min(len, sizeof(buf));
		ssize_t done = pwrite(fd, buf, chunk, start);

		if (done < 0)
			return -errno;
		if (done == 0)
			return -EIO;
		start += done;
		len -= done;
	}
	return 0;
}

int
device_get_partition_size_fd_stat(int fd, const struct stat *st, u64 *size_ret)
{
	struct disklabel dl;
	unsigned int part;

	if (S_ISREG(st->st_mode)) {
		*size_ret = st->st_size;
		return 0;
	}
	if (!S_ISBLK(st->st_mode) && !S_ISCHR(st->st_mode))
		return -EINVAL;
	if (ioctl(fd, DIOCGDINFO, &dl) == -1)
		return -errno;
	part = DISKPART(st->st_rdev);
	if (part >= MAXPARTITIONS)
		return -EINVAL;
	*size_ret = DL_GETPSIZE(&dl.d_partitions[part]) * dl.d_secsize;
	return 0;
}

int
device_get_partition_size(const char *path, u64 *size_ret)
{
	struct stat st;
	int fd, ret;

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return -errno;
	if (fstat(fd, &st) == -1) {
		ret = -errno;
		close(fd);
		return ret;
	}
	ret = device_get_partition_size_fd_stat(fd, &st, size_ret);
	close(fd);
	return ret;
}

int
btrfs_prepare_device(int fd, const char *file, u64 *byte_count_ret,
    u64 max_byte_count, unsigned opflags)
{
	struct stat st;
	u64 byte_count;
	int i, ret;

	(void)file;
	if (opflags & PREP_DEVICE_ZONED)
		return 1;
	if (fstat(fd, &st) == -1)
		return 1;
	ret = device_get_partition_size_fd_stat(fd, &st, &byte_count);
	if (ret < 0)
		return 1;
	if (max_byte_count != 0)
		byte_count = min(byte_count, max_byte_count);

	ret = device_zero_blocks(fd, 0, min_t(u64, byte_count, SZ_2M), false);
	for (i = 0; ret == 0 && i < BTRFS_SUPER_MIRROR_MAX; i++) {
		u64 off = btrfs_sb_offset(i);

		if (off < byte_count)
			ret = device_zero_blocks(fd, off,
			    min_t(u64, BTRFS_SUPER_INFO_SIZE, byte_count - off),
			    false);
	}
	if (ret == 0 && (opflags & PREP_DEVICE_ZERO_END) &&
	    byte_count > SZ_2M)
		ret = device_zero_blocks(fd, byte_count - SZ_2M, SZ_2M, false);
	if (ret != 0)
		return 1;
	*byte_count_ret = byte_count;
	return 0;
}

int
device_get_queue_param(const char *file, const char *param, char *buf,
    size_t len)
{
	(void)file;
	(void)param;
	(void)buf;
	(void)len;
	return 0;
}

u64
device_get_zone_unusable(int fd, u64 flags)
{
	(void)fd;
	(void)flags;
	return DEVICE_ZONE_UNUSABLE_UNKNOWN;
}

u64
device_get_zone_size(int fd, const char *name)
{
	(void)fd;
	(void)name;
	return 0;
}

int
device_get_rotational(const char *file)
{
	(void)file;
	return 0;
}

int
device_get_info(int fd, u64 devid, struct btrfs_ioctl_dev_info_args *args)
{
	(void)fd;
	(void)devid;
	(void)args;
	return -EOPNOTSUPP;
}

ssize_t
btrfs_direct_pread(int fd, void *buf, size_t count, off_t offset)
{
	return pread(fd, buf, count, offset);
}

ssize_t
btrfs_direct_pwrite(int fd, const void *buf, size_t count, off_t offset)
{
	return pwrite(fd, buf, count, offset);
}

int
cmp_device_id(void *priv, struct list_head *a, struct list_head *b)
{
	const struct btrfs_device *da;
	const struct btrfs_device *db;

	(void)priv;
	da = list_entry(a, struct btrfs_device, dev_list);
	db = list_entry(b, struct btrfs_device, dev_list);
	return da->devid < db->devid ? -1 : da->devid > db->devid;
}
