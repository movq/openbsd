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

/*
 * INFO accepts either id or path. It returns a canonical tree-5 path and,
 * if the tree is reachable in a live view, a host pathname for that root.
 * FINISH accepts id, received_uuid and stransid, and atomically records the
 * receive identity and makes the tree read-only. Close/unmount the receive
 * tree first: finalization rejects active vnodes and mounted hierarchies.
 */
struct btrfs_ioctl_identity {
	int32_t		fd;
	uint32_t	flags;
	uint64_t	id;
	uint64_t	ctransid;
	uint64_t	stransid;
	uint64_t	fd_treeid;
	uint8_t		uuid[16];
	uint8_t		received_uuid[16];
	char		path[BTRFS_CTL_PATH_MAX];
	char		access[BTRFS_CTL_PATH_MAX];
};
#define BTRFSIOC_INFO	_IOWR('B', 5, struct btrfs_ioctl_identity)
#define BTRFSIOC_FINISH	_IOW('B', 6, struct btrfs_ioctl_identity)

/* fd selects the inode's tree; ino may identify a symlink. GET enumerates
 * attributes in tree order, advancing cursor. ENOENT denotes the end.
 * Values are opaque Linux xattrs, not OpenBSD access-control policy. */
struct btrfs_ioctl_xattr {
	int32_t		fd;
	uint32_t	size;
	uint64_t	ino;
	uint64_t	cursor;
	void		*value;
	char		name[256];
};
#define BTRFSIOC_GETXATTR	_IOWR('B', 7, struct btrfs_ioctl_xattr)
#define BTRFSIOC_SETXATTR	_IOW('B', 8, struct btrfs_ioctl_xattr)
#define BTRFSIOC_RMXATTR	_IOW('B', 9, struct btrfs_ioctl_xattr)

#endif
