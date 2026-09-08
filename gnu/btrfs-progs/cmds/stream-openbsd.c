/* SPDX-License-Identifier: GPL-2.0 */

/*
 * A mountpoint selects the filesystem for send/receive; subvolume and
 * destination paths start at tree 5 rather than the selected mount root.
 * Access roots through existing views or temporary disjoint mounts.
 */

#include "kerncompat.h"
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "common/send-stream.h"
#include "cmds/stream-openbsd.h"

static void
stream_signal(int signo)
{
	btrfs_send_stream_cancelled = signo;
}

int
stream_fs_open(struct stream_fs *fs, const char *path)
{
	struct sigaction action = { .sa_handler = stream_signal };
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGHUP, &action, NULL);
	action.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &action, NULL);
	fs->control = fs->fd = -1;
	fs->fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fs->fd == -1)
		return -errno;
	fs->control = open("/dev/btrfs-control", O_RDWR | O_CLOEXEC);
	if (fs->control == -1) {
		int ret = -errno;
		stream_fs_close(fs);
		return ret;
	}
	return 0;
}

void
stream_fs_close(struct stream_fs *fs)
{
	if (fs->control != -1)
		close(fs->control);
	if (fs->fd != -1)
		close(fs->fd);
}

int
stream_info(struct stream_fs *fs, const char *path, uint64_t id,
    struct btrfs_ioctl_identity *info)
{
	memset(info, 0, sizeof(*info));
	info->fd = fs->fd;
	info->id = id;
	if (path && strlcpy(info->path, path, sizeof(info->path)) >=
	    sizeof(info->path))
		return -ENAMETOOLONG;
	return ioctl(fs->control, BTRFSIOC_INFO, info) == -1 ? -errno : 0;
}

int
stream_root_open(struct stream_fs *fs, const char *path, uint64_t id,
    struct stream_root *root)
{
	struct statfs st;
	struct btrfs_args args = { 0 };
	int ret;

	memset(root, 0, sizeof(*root));
	root->fd = -1;
	ret = stream_info(fs, path, id, &root->info);
	if (ret)
		return ret;
	if (root->info.access[0] == '\0') {
		if (fstatfs(fs->fd, &st) == -1)
			return -errno;
		strlcpy(root->temporary, "/tmp/btrfs-stream.XXXXXXXXXX",
		    sizeof(root->temporary));
		if (mkdtemp(root->temporary) == NULL) {
			root->temporary[0] = '\0';
			return -errno;
		}
		args.fspec = st.f_mntfromname;
		args.subvolid = root->info.id;
		if (mount(MOUNT_BTRFS, root->temporary,
		    MNT_NODEV | MNT_NOSUID |
		    (root->info.flags & BTRFS_CTL_RDONLY ? MNT_RDONLY : 0),
		    &args) == -1) {
			ret = -errno;
			rmdir(root->temporary);
			root->temporary[0] = '\0';
			return ret;
		}
		strlcpy(root->info.access, root->temporary,
		    sizeof(root->info.access));
	}
	root->fd = open(root->info.access,
	    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (root->fd == -1) {
		ret = -errno;
		stream_root_close(root);
		return ret;
	}
	/*
	 * Paths returned by INFO are advisory. Verify the opened view and root
	 * after any rename/mount race, and retain the descriptor to pin it.
	 */
	{
		struct stream_fs opened = *fs;
		struct btrfs_ioctl_identity check;
		struct stat sb;
		opened.fd = root->fd;
		ret = stream_info(&opened, NULL, root->info.id, &check);
		if (!ret && (fstat(root->fd, &sb) == -1 || sb.st_ino != 256))
			ret = -EINVAL;
		if (!ret && memcmp(check.uuid, root->info.uuid, 16))
			ret = -ESTALE;
		if (!ret && check.fd_treeid != root->info.id)
			ret = -ESTALE;
	}
	if (ret)
		stream_root_close(root);
	return ret;
}

int
stream_root_close(struct stream_root *root)
{
	int ret = 0;
	if (root->fd != -1) {
		close(root->fd);
		root->fd = -1;
	}
	if (root->temporary[0]) {
		if (unmount(root->temporary, 0) == -1)
			ret = -errno;
		else {
			rmdir(root->temporary);
			root->temporary[0] = '\0';
		}
	}
	return ret;
}

const uint8_t *
stream_uuid(const struct btrfs_ioctl_identity *info)
{
	return memcmp(info->received_uuid, (uint8_t[16]){0}, 16) ?
	    info->received_uuid : info->uuid;
}

uint64_t
stream_transid(const struct btrfs_ioctl_identity *info)
{
	return memcmp(info->received_uuid, (uint8_t[16]){0}, 16) ?
	    info->stransid : info->ctransid;
}

int
stream_find_uuid(struct stream_fs *fs, const uint8_t *uuid, uint64_t transid,
    struct btrfs_ioctl_identity *info)
{
	memset(info, 0, sizeof(*info));
	info->fd = fs->fd;
	memcpy(info->uuid, uuid, sizeof(info->uuid));
	info->stransid = transid;
	return ioctl(fs->control, BTRFSIOC_FIND_UUID, info) == -1 ? -errno : 0;
}

int
stream_subvol(struct stream_fs *fs, unsigned long op, const char *path,
    const char *source)
{
	struct btrfs_ioctl_subvolume args = { 0 };
	args.fd = fs->fd;
	if (strlcpy(args.path, path, sizeof(args.path)) >= sizeof(args.path) ||
	    (source && strlcpy(args.source, source, sizeof(args.source)) >=
	    sizeof(args.source)))
		return -ENAMETOOLONG;
	return ioctl(fs->control, op, &args) == -1 ? -errno : 0;
}

/*
 * Resolve each directory component without following symlinks. The last
 * component is left to an *at operation with its own no-follow semantics.
 * A stream may name the root as "." but may never escape it.
 */
int
stream_parent(int root, const char *path, char *name)
{
	char copy[PATH_MAX], *part, *next;
	int fd, child;
	/* Linux spells the subvolume root as the empty path. */
	if (!path[0])
		path = ".";
	if (path[0] == '/' ||
	    strlcpy(copy, path, sizeof(copy)) >= sizeof(copy))
		return -EINVAL;
	fd = fcntl(root, F_DUPFD_CLOEXEC, 0);
	if (fd == -1)
		return -errno;
	part = copy;
	for (;;) {
		next = strchr(part, '/');
		if (next)
			*next++ = '\0';
		if (!part[0] || !strcmp(part, "..") ||
		    (strcmp(part, ".") == 0 && (next || part != copy)) ||
		    strlen(part) > NAME_MAX) {
			close(fd);
			return -EINVAL;
		}
		if (!next) {
			strlcpy(name, part, NAME_MAX + 1);
			return fd;
		}
		child = openat(fd, part,
		    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		close(fd);
		if (child == -1)
			return -errno;
		fd = child;
		part = next;
	}
}

int
stream_open_file(int root, const char *path, int flags)
{
	char name[NAME_MAX + 1];
	struct stat sb;
	int parent, fd, ret;
	parent = stream_parent(root, path, name);
	if (parent < 0)
		return parent;
	/* Never open a device or FIFO supplied by an untrusted stream. */
	if (fstatat(parent, name, &sb, AT_SYMLINK_NOFOLLOW) == -1)
		ret = -errno;
	else if (!S_ISREG(sb.st_mode))
		ret = -EINVAL;
	else {
		fd = openat(parent, name, flags | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
		ret = fd == -1 ? -errno : fd;
		if (fd != -1 && (fstat(fd, &sb) == -1 || !S_ISREG(sb.st_mode))) {
			close(fd);
			ret = -EINVAL;
		}
	}
	close(parent);
	return ret;
}

int
stream_stat(int root, const char *path, struct stat *st)
{
	char name[NAME_MAX + 1];
	int fd = stream_parent(root, path, name), ret;
	if (fd < 0)
		return fd;
	ret = fstatat(fd, name, st, AT_SYMLINK_NOFOLLOW) == -1 ? -errno : 0;
	close(fd);
	return ret;
}

int
stream_write(int fd, const void *data, size_t len)
{
	ssize_t n;
	while (len) {
		if (btrfs_send_stream_cancelled)
			return -EINTR;
		n = write(fd, data, len);
		if (n == -1 && errno == EINTR)
			continue;
		if (n <= 0)
			return n == 0 ? -EIO : -errno;
		data = (const char *)data + n;
		len -= n;
	}
	return 0;
}

int
stream_pread(int fd, void *data, size_t len, uint64_t offset)
{
	ssize_t n;
	if (offset > INT64_MAX || len > INT64_MAX - offset)
		return -EOVERFLOW;
	while (len) {
		if (btrfs_send_stream_cancelled)
			return -EINTR;
		n = pread(fd, data, len, offset);
		if (n == -1 && errno == EINTR)
			continue;
		if (n <= 0)
			return n == 0 ? -EIO : -errno;
		data = (char *)data + n;
		len -= n;
		offset += n;
	}
	return 0;
}

int
stream_pwrite(int fd, const void *data, size_t len, uint64_t offset)
{
	ssize_t n;
	if (offset > INT64_MAX || len > INT64_MAX - offset)
		return -EOVERFLOW;
	while (len) {
		if (btrfs_send_stream_cancelled)
			return -EINTR;
		n = pwrite(fd, data, len, offset);
		if (n == -1 && errno == EINTR)
			continue;
		if (n <= 0)
			return n == 0 ? -EIO : -errno;
		data = (const char *)data + n;
		len -= n;
		offset += n;
	}
	return 0;
}
