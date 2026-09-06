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

/*
 * Read stable snapshot items in [min, max], in key order. fd and parent_fd
 * must name subvolume root directories on the same filesystem, both read-only.
 * parent_fd == -1 enumerates everything; otherwise omit byte-identical items
 * and shared subtrees. Reverse the descriptors to discover deleted items.
 * Open root descriptors pin the trees across calls. No writable-tree queries.
 *
 * Buffer contains native btrfs_tree_item headers followed by on-disk bytes,
 * padded to eight bytes. KEYS omits payloads (size is then zero).
 * min advances to the first unreturned key; done marks exhaustion. Input
 * size is capacity (at most 64 KiB); output size is bytes used. ENOBUFS means
 * one record cannot fit. Counters describe this call, including comparison
 * searches, and allow callers to measure metadata work without tracing I/O.
 */
struct btrfs_tree_key {
	uint64_t	objectid;
	uint64_t	offset;
	uint32_t	type;
	uint32_t	reserved;
};
struct btrfs_tree_item {
	struct btrfs_tree_key key;
	uint32_t	size;
	uint32_t	reserved;
};
#define BTRFS_TREE_KEYS	1
#define BTRFS_TREE_BUFSIZE	65536
struct btrfs_ioctl_tree {
	int32_t		fd;
	int32_t		parent_fd;
	uint32_t	flags;
	uint32_t	size;
	struct btrfs_tree_key min;
	struct btrfs_tree_key max;
	void		*buffer;
	uint64_t	blocks;
	uint64_t	shared;
	uint64_t	items;
	uint32_t	done;
	uint32_t	sectorsize;	/* output: alignment for clone ranges */
};
#define BTRFSIOC_TREE	_IOWR('B', 10, struct btrfs_ioctl_tree)

#endif
