/* Public domain. */
#ifndef _SYS_BTRFSIO_H_
#define _SYS_BTRFSIO_H_

#include <sys/types.h>
#include <sys/ioccom.h>

/*
 * fd selects a mounted filesystem. Paths start at tree 5, independently
 * of the selected mount view. LIST returns the first ID above cursor;
 * ENOENT denotes the end. Reserved input fields must be zero.
 */
#define BTRFS_CTL_PATH_MAX	1024
#define BTRFS_CTL_RDONLY		0x00000001
struct btrfs_ioctl_subvolume {
	int32_t		fd;
	uint32_t	flags;
	uint64_t	cursor;
	uint64_t	id;
	uint64_t	parent;
	char		source[BTRFS_CTL_PATH_MAX];
	char		path[BTRFS_CTL_PATH_MAX];
};

#define BTRFSIOC_LIST	_IOWR('B', 1, struct btrfs_ioctl_subvolume)
#define BTRFSIOC_CREATE	_IOW('B', 2, struct btrfs_ioctl_subvolume)
#define BTRFSIOC_DELETE	_IOW('B', 3, struct btrfs_ioctl_subvolume)
#define BTRFSIOC_SNAPSHOT	_IOW('B', 4, struct btrfs_ioctl_subvolume)

#endif
