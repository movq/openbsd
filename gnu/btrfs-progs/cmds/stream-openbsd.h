/* SPDX-License-Identifier: GPL-2.0 */
#ifndef STREAM_OPENBSD_H
#define STREAM_OPENBSD_H
#include <sys/btrfsio.h>
#include <sys/stat.h>

struct stream_fs {
	int control;
	int fd;
};
struct stream_root {
	struct btrfs_ioctl_identity info;
	int fd;
	char temporary[128];
};
int stream_fs_open(struct stream_fs *, const char *);
void stream_fs_close(struct stream_fs *);
int stream_info(struct stream_fs *, const char *, uint64_t,
    struct btrfs_ioctl_identity *);
int stream_root_open(struct stream_fs *, const char *, uint64_t,
    struct stream_root *);
int stream_root_close(struct stream_root *);
int stream_find_uuid(struct stream_fs *, const uint8_t *, uint64_t,
    struct btrfs_ioctl_identity *);
const uint8_t *stream_uuid(const struct btrfs_ioctl_identity *);
uint64_t stream_transid(const struct btrfs_ioctl_identity *);
int stream_subvol(struct stream_fs *, unsigned long, const char *, const char *);
int stream_parent(int, const char *, char *);
int stream_open_file(int, const char *, int);
int stream_stat(int, const char *, struct stat *);
int stream_write(int, const void *, size_t);
int stream_pread(int, void *, size_t, uint64_t);
int stream_pwrite(int, const void *, size_t, uint64_t);
#endif
