/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Native callbacks for the upstream btrfs-progs send-stream decoder.
 * The callback model and receive identity rules follow cmds/receive.c.
 */
#include "kerncompat.h"
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "common/help.h"
#include "common/messages.h"
#include "common/send-stream.h"
#include "cmds/commands.h"
#include "cmds/stream-openbsd.h"

struct pending_xattr {
	struct pending_xattr *next;
	uint64_t ino;
	char *name;
	void *value;
	size_t size;
};

struct receiver {
	struct stream_fs fs;
	struct stream_root root;
	struct stream_root parent;
	char destination[PATH_MAX];
	char path[PATH_MAX];
	uint8_t uuid[16];
	uint64_t transid;
	struct pending_xattr *xattrs;
	char xattr_buffer[65536];
};

static int receive_strip_dirs(struct receiver *, int);

static int
begin_receive(const char *path, const u8 *uuid, u64 transid,
    const u8 *parent_uuid, u64 parent_transid, struct receiver *r)
{
	struct btrfs_ioctl_identity info;
	int ret;

	if (r->root.fd != -1 || !path[0] || strchr(path, '/') ||
	    !strcmp(path, ".") || !strcmp(path, "..") ||
	    !memcmp(uuid, (uint8_t[16]){0}, 16) || !transid)
		return -EINVAL;
	if (snprintf(r->path, sizeof(r->path), "%s/%s",
	    r->destination, path) >= sizeof(r->path))
		return -ENAMETOOLONG;
	if (parent_uuid) {
		ret = stream_find_uuid(&r->fs, parent_uuid, parent_transid, &info);
		if (ret)
			return ret;
		ret = stream_root_open(&r->fs, NULL, info.id, &r->parent);
		if (ret)
			return ret;
		ret = stream_subvol(&r->fs, BTRFSIOC_SNAPSHOT,
		    r->path, info.path);
	} else
		ret = stream_subvol(&r->fs, BTRFSIOC_CREATE, r->path, NULL);
	if (ret)
		return ret;
	ret = stream_root_open(&r->fs, r->path, 0, &r->root);
	if (ret)
		return ret;
	memcpy(r->uuid, uuid, 16);
	r->transid = transid;
	/* Defer directory xattrs so native creation need not inherit Linux
	 * ACLs. Every child's complete metadata arrives in the stream. */
	return parent_uuid ? receive_strip_dirs(r, r->root.fd) : 0;
}

static int
recv_subvol(const char *path, const u8 *uuid, u64 transid, void *user)
{
	return begin_receive(path, uuid, transid, NULL, 0, user);
}

static int
recv_snapshot(const char *path, const u8 *uuid, u64 transid,
    const u8 *parent_uuid, u64 parent_transid, void *user)
{
	return begin_receive(path, uuid, transid, parent_uuid, parent_transid, user);
}

static int
receive_parent(struct receiver *r, const char *path, char *name, int namespace)
{
	int fd;
	if (r->root.fd == -1)
		return -EINVAL;
	fd = stream_parent(r->root.fd, path, name);
	if (fd >= 0 && namespace && !strcmp(name, ".")) {
		close(fd);
		return -EINVAL;
	}
	return fd;
}

static int
recv_node(const char *path, mode_t mode, dev_t dev, const char *target,
    struct receiver *r)
{
	char name[NAME_MAX + 1];
	int fd, file, ret;
	fd = receive_parent(r, path, name, 1);
	if (fd < 0)
		return fd;
	if (S_ISREG(mode)) {
		file = openat(fd, name,
		    O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600);
		ret = file == -1 ? -errno : 0;
		if (file != -1)
			close(file);
	} else if (S_ISDIR(mode))
		ret = mkdirat(fd, name, 0700) == -1 ? -errno : 0;
	else if (S_ISLNK(mode))
		ret = symlinkat(target, fd, name) == -1 ? -errno : 0;
	else
		ret = mknodat(fd, name, mode, dev) == -1 ? -errno : 0;
	close(fd);
	return ret;
}

static int recv_mkfile(const char *p, void *r)
{ return recv_node(p, S_IFREG | 0600, 0, NULL, r); }
static int recv_mkdir(const char *p, void *r)
{ return recv_node(p, S_IFDIR | 0700, 0, NULL, r); }
static int recv_mkfifo(const char *p, void *r)
{ return recv_node(p, S_IFIFO | 0600, 0, NULL, r); }

static int
recv_mksock(const char *path, void *user)
{
	struct receiver *r = user;
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	char name[NAME_MAX + 1];
	int parent, cwd, sock, ret, bound = 0;
	parent = receive_parent(r, path, name, 1);
	if (parent < 0)
		return parent;
	cwd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (cwd == -1) {
		ret = -errno;
		goto out_parent;
	}
	sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (sock == -1) {
		ret = -errno;
		goto out_cwd;
	}
	if (fchdir(parent) == -1) {
		ret = -errno;
		goto out_socket;
	}
	/*
	 * A short private name avoids sockaddr_un's smaller pathname limit.
	 * linkat publishes the requested name without replacing an existing
	 * entry. Keep the saved cwd fd until restoration has succeeded.
	 */
	snprintf(address.sun_path, sizeof(address.sun_path),
	    ".btrfs-receive-%08x%08x", arc4random(), arc4random());
	address.sun_len = sizeof(address);
	ret = bind(sock, (struct sockaddr *)&address, sizeof(address));
	if (ret == -1)
		ret = -errno;
	else {
		bound = 1;
		ret = linkat(parent, address.sun_path, parent, name, 0) == -1 ?
		    -errno : 0;
	}
	if (bound)
		unlinkat(parent, address.sun_path, 0);
	if (fchdir(cwd) == -1)
		ret = -errno;
out_socket:
	close(sock);
out_cwd:
	close(cwd);
out_parent:
	close(parent);
	return ret;
}
static int recv_symlink(const char *p, const char *target, void *r)
{ return recv_node(p, S_IFLNK | 0777, 0, target, r); }

static int
recv_mknod(const char *path, u64 mode, u64 dev, void *r)
{
	uint64_t maj = (dev >> 8) & 0xfff;
	uint64_t min = (dev & 0xff) | ((dev >> 12) & 0xfffff00);
	if (maj > 255 || min > 0xfffff || dev >> 32 ||
	    mode > UINT32_MAX || (!S_ISCHR(mode) && !S_ISBLK(mode)))
		return -EOVERFLOW;
	return recv_node(path, mode, makedev(maj, min), NULL, r);
}

static int
recv_two_paths(const char *from, const char *to, struct receiver *r, int link)
{
	char a[NAME_MAX + 1], b[NAME_MAX + 1];
	int fd, td, ret;
	fd = receive_parent(r, from, a, 1);
	if (fd < 0)
		return fd;
	td = receive_parent(r, to, b, 1);
	if (td < 0) {
		close(fd);
		return td;
	}
	ret = link ? linkat(fd, a, td, b, 0) : renameat(fd, a, td, b);
	ret = ret == -1 ? -errno : 0;
	close(fd);
	close(td);
	return ret;
}
static int recv_rename(const char *a, const char *b, void *r)
{ return recv_two_paths(a, b, r, 0); }
static int recv_link(const char *path, const char *target, void *r)
{ return recv_two_paths(target, path, r, 1); }

static int
recv_remove(const char *path, struct receiver *r, int flags)
{
	char name[NAME_MAX + 1];
	int fd = receive_parent(r, path, name, 1), ret;
	if (fd < 0)
		return fd;
	ret = unlinkat(fd, name, flags) == -1 ? -errno : 0;
	close(fd);
	return ret;
}
static int recv_unlink(const char *p, void *r)
{ return recv_remove(p, r, 0); }
static int recv_rmdir(const char *p, void *r)
{ return recv_remove(p, r, AT_REMOVEDIR); }

static int
recv_write(const char *path, const void *data, u64 offset, u64 len, void *user)
{
	struct receiver *r = user;
	int fd, ret;
	if (r->root.fd == -1 || len > SIZE_MAX)
		return -EINVAL;
	fd = stream_open_file(r->root.fd, path, O_WRONLY);
	if (fd < 0)
		return fd;
	ret = stream_pwrite(fd, data, len, offset);
	close(fd);
	return ret;
}

static int
recv_clone(const char *path, u64 offset, u64 len, const u8 *uuid,
    u64 transid, const char *source, u64 source_offset, void *user)
{
	struct receiver *r = user;
	struct stream_root clone = { .fd = -1 };
	struct btrfs_ioctl_identity info;
	struct btrfs_ioctl_clone args;
	int root, fd = -1, out = -1, ret;

	if (r->root.fd == -1 || offset > INT64_MAX ||
	    len > INT64_MAX - offset || source_offset > INT64_MAX ||
	    len > INT64_MAX - source_offset)
		return -EINVAL;
	if (!memcmp(uuid, r->uuid, 16) && transid == r->transid)
		root = r->root.fd;
	else {
		ret = stream_find_uuid(&r->fs, uuid, transid, &info);
		if (ret)
			return ret;
		ret = stream_root_open(&r->fs, NULL, info.id, &clone);
		if (ret)
			return ret;
		root = clone.fd;
	}
	fd = stream_open_file(root, source, O_RDONLY);
	if (fd < 0) {
		ret = fd;
		goto out;
	}
	out = stream_open_file(r->root.fd, path, O_WRONLY);
	if (out < 0) {
		ret = out;
		goto out;
	}
	memset(&args, 0, sizeof(args));
	args.src_fd = fd;
	args.dst_fd = out;
	args.src_offset = source_offset;
	args.dst_offset = offset;
	args.length = len;
	ret = ioctl(r->fs.control, BTRFSIOC_CLONE, &args) == -1 ? -errno : 0;
out:
	if (fd >= 0)
		close(fd);
	if (out >= 0)
		close(out);
	{
		int err = stream_root_close(&clone);
		if (!ret)
			ret = err;
	}
	return ret;
}

static int
recv_truncate(const char *path, u64 size, void *user)
{
	struct receiver *r = user;
	int fd, ret;
	if (r->root.fd == -1 || size > INT64_MAX)
		return -EINVAL;
	fd = stream_open_file(r->root.fd, path, O_WRONLY);
	if (fd < 0)
		return fd;
	ret = ftruncate(fd, size) == -1 ? -errno : 0;
	close(fd);
	return ret;
}

static int
recv_chmod(const char *path, u64 mode, void *user)
{
	struct receiver *r = user;
	char name[NAME_MAX + 1];
	int fd, ret;
	if (mode & ~07777ULL)
		return -EINVAL;
	fd = receive_parent(r, path, name, 0);
	if (fd < 0)
		return fd;
	ret = fchmodat(fd, name, mode, AT_SYMLINK_NOFOLLOW) == -1 ? -errno : 0;
	close(fd);
	return ret;
}

static int
recv_chown(const char *path, u64 uid, u64 gid, void *user)
{
	struct receiver *r = user;
	char name[NAME_MAX + 1];
	int fd, ret;
	if (uid >= UINT32_MAX || gid >= UINT32_MAX)
		return -EOVERFLOW;
	fd = receive_parent(r, path, name, 0);
	if (fd < 0)
		return fd;
	ret = fchownat(fd, name, uid, gid, AT_SYMLINK_NOFOLLOW) == -1 ? -errno : 0;
	close(fd);
	return ret;
}

static int
recv_utimes(const char *path, struct timespec *at, struct timespec *mt,
    struct timespec *ct, void *user)
{
	struct receiver *r = user;
	char name[NAME_MAX + 1];
	struct timespec times[2] = { *at, *mt };
	int fd, ret;
	if (at->tv_nsec < 0 || at->tv_nsec >= 1000000000 ||
	    mt->tv_nsec < 0 || mt->tv_nsec >= 1000000000 ||
	    ct->tv_nsec < 0 || ct->tv_nsec >= 1000000000)
		return -EINVAL;
	fd = receive_parent(r, path, name, 0);
	if (fd < 0)
		return fd;
	ret = utimensat(fd, name, times, AT_SYMLINK_NOFOLLOW) == -1 ? -errno : 0;
	close(fd);
	return ret;
}

static int
pending_xattr(struct receiver *r, uint64_t ino, const char *name,
    const void *value, size_t size, int remove)
{
	struct pending_xattr **link, *x;
	for (link = &r->xattrs; (x = *link) != NULL; link = &x->next)
		if (x->ino == ino && !strcmp(x->name, name))
			break;
	if (remove) {
		if (!x)
			return -ENOENT;
		*link = x->next;
		free(x->name);
		free(x->value);
		free(x);
		return 0;
	}
	if (!x) {
		x = calloc(1, sizeof(*x));
		if (!x)
			return -ENOMEM;
		x->name = strdup(name);
		if (!x->name) {
			free(x);
			return -ENOMEM;
		}
		x->ino = ino;
		*link = x;
	}
	free(x->value);
	x->value = malloc(size ? size : 1);
	if (!x->value)
		return -ENOMEM;
	memcpy(x->value, value, size);
	x->size = size;
	return 0;
}

static int
receive_strip_xattrs(struct receiver *r, uint64_t ino)
{
	struct btrfs_ioctl_xattr args;
	char *value = r->xattr_buffer;
	int ret;

	for (;;) {
		memset(&args, 0, sizeof(args));
		args.fd = r->root.fd;
		args.ino = ino;
		args.size = sizeof(r->xattr_buffer);
		args.value = value;
		if (ioctl(r->fs.control, BTRFSIOC_GETXATTR, &args) == -1) {
			if (errno == ENOENT)
				break;
			return -errno;
		}
		ret = pending_xattr(r, args.ino, args.name, value, args.size, 0);
		if (ret)
			return ret;
		args.cursor = args.size = 0;
		if (ioctl(r->fs.control, BTRFSIOC_RMXATTR, &args) == -1)
			return -errno;
	}
	return 0;
}

static int
receive_strip_dirs(struct receiver *r, int fd)
{
	struct stat st;
	struct dirent *de;
	DIR *dir;
	int child, copy, ret;

	if (fstat(fd, &st) == -1)
		return -errno;
	ret = receive_strip_xattrs(r, st.st_ino);
	if (ret)
		return ret;
	copy = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (copy == -1)
		return -errno;
	dir = fdopendir(copy);
	if (!dir) {
		ret = -errno;
		close(copy);
		return ret;
	}
	for (;;) {
		errno = 0;
		de = readdir(dir);
		if (!de) {
			ret = -errno;
			break;
		}
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (fstatat(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) == -1) {
			ret = -errno;
			break;
		}
		if (!S_ISDIR(st.st_mode))
			continue;
		if (st.st_ino == 2 || st.st_ino == 256) {
			ret = -EOPNOTSUPP;
			break;
		}
		child = openat(fd, de->d_name,
		    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (child == -1) {
			ret = -errno;
			break;
		}
		ret = receive_strip_dirs(r, child);
		close(child);
		if (ret)
			break;
	}
	closedir(dir);
	return ret;
}

static int
recv_xattr(const char *path, const char *name, const void *data, int len,
    void *user, int remove)
{
	struct receiver *r = user;
	struct btrfs_ioctl_xattr args = { 0 };
	struct stat st;
	int ret;
	if (r->root.fd == -1 || len < 0 || !name[0] ||
	    strlcpy(args.name, name, sizeof(args.name)) >= sizeof(args.name))
		return -EINVAL;
	ret = stream_stat(r->root.fd, path, &st);
	if (ret)
		return ret;
	if (S_ISDIR(st.st_mode))
		return pending_xattr(r, st.st_ino, name, data, len, remove);
	args.fd = r->root.fd;
	args.ino = st.st_ino;
	args.value = (void *)data;
	args.size = len;
	return ioctl(r->fs.control, remove ? BTRFSIOC_RMXATTR :
	    BTRFSIOC_SETXATTR, &args) == -1 ? -errno : 0;
}

static int recv_set_xattr(const char *p, const char *n, const void *d,
    int len, void *r)
{ return recv_xattr(p, n, d, len, r, 0); }
static int recv_remove_xattr(const char *p, const char *n, void *r)
{ return recv_xattr(p, n, NULL, 0, r, 1); }

static int
receive_flush_xattrs(struct receiver *r, int apply)
{
	struct pending_xattr *x;
	struct btrfs_ioctl_xattr args;
	int ret = 0;
	while ((x = r->xattrs) != NULL) {
		r->xattrs = x->next;
		if (apply && !ret) {
			memset(&args, 0, sizeof(args));
			args.fd = r->root.fd;
			args.ino = x->ino;
			args.size = x->size;
			args.value = x->value;
			strlcpy(args.name, x->name, sizeof(args.name));
			if (ioctl(r->fs.control, BTRFSIOC_SETXATTR, &args) == -1 &&
			    errno != ENOENT)
				ret = -errno;
		}
		free(x->name);
		free(x->value);
		free(x);
	}
	return ret;
}

static int recv_update_extent(const char *p, u64 offset, u64 len, void *r)
{ return -EOPNOTSUPP; }

static struct btrfs_send_ops receive_ops = {
	.subvol = recv_subvol,
	.snapshot = recv_snapshot,
	.mkfile = recv_mkfile,
	.mkdir = recv_mkdir,
	.mknod = recv_mknod,
	.mkfifo = recv_mkfifo,
	.mksock = recv_mksock,
	.symlink = recv_symlink,
	.rename = recv_rename,
	.link = recv_link,
	.unlink = recv_unlink,
	.rmdir = recv_rmdir,
	.write = recv_write,
	.clone = recv_clone,
	.truncate = recv_truncate,
	.chmod = recv_chmod,
	.chown = recv_chown,
	.utimes = recv_utimes,
	.set_xattr = recv_set_xattr,
	.remove_xattr = recv_remove_xattr,
	.update_extent = recv_update_extent,
};

static const char * const receive_usage[] = {
	"btrfs receive [-f <stream>] <mountpoint> <path>",
	"Receive Linux version 1 streams below a filesystem-root-relative directory",
	"",
	"-f <stream>  read from a file instead of standard input",
	NULL
};

static int
cmd_receive(const struct cmd_struct *cmd, int argc, char **argv)
{
	struct receiver r = { .root.fd = -1, .parent.fd = -1 };
	struct btrfs_ioctl_identity finish;
	const char *file = NULL;
	int ch, fd = STDIN_FILENO, ret, count = 0, err;

	optind = 1;
	while ((ch = getopt(argc, argv, "f:")) != -1) {
		if (ch != 'f')
			goto usage;
		file = optarg;
	}
	if (argc - optind != 2)
		goto usage;
	ret = stream_fs_open(&r.fs, argv[optind]);
	if (ret)
		goto error;
	if (strlcpy(r.destination, argv[optind + 1], sizeof(r.destination)) >=
	    sizeof(r.destination)) {
		ret = -ENAMETOOLONG;
		goto out;
	}
	if (file) {
		fd = open(file, O_RDONLY | O_CLOEXEC);
		if (fd == -1) {
			ret = -errno;
			goto out;
		}
	}
	umask(0);
	for (;;) {
		ret = btrfs_read_and_process_send_stream(fd, &receive_ops, &r, 1, 1);
		if (ret == -ENODATA && count) {
			ret = 0;
			break;
		}
		if (ret != 1)
			break;
		if (r.root.fd == -1) {
			ret = -EINVAL;
			break;
		}
		ret = receive_flush_xattrs(&r, 1);
		if (ret)
			break;
		memset(&finish, 0, sizeof(finish));
		finish.fd = r.fs.fd;
		finish.id = r.root.info.id;
		finish.stransid = r.transid;
		memcpy(finish.received_uuid, r.uuid, 16);
		ret = stream_root_close(&r.root);
		if (!ret)
			ret = stream_root_close(&r.parent);
		if (ret)
			break;
		if (ioctl(r.fs.control, BTRFSIOC_FINISH, &finish) == -1) {
			ret = -errno;
			break;
		}
		r.path[0] = '\0';
		count++;
	}
	if (fd != STDIN_FILENO)
		close(fd);
out:
	receive_flush_xattrs(&r, 0);
	err = stream_root_close(&r.root);
	if (!ret)
		ret = err;
	err = stream_root_close(&r.parent);
	if (!ret)
		ret = err;
	stream_fs_close(&r.fs);
error:
	if (ret) {
		error("receive: %s", strerror(-ret));
		if (r.path[0])
			error("incomplete receive at %s; it has not been finalized",
			    r.path);
	}
	return ret ? 1 : 0;
usage:
	usage_command(cmd, false, false);
	return 1;
}
DEFINE_COMMAND(receive, "receive", cmd_receive, receive_usage, NULL, 0);
