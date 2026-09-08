/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Emit Linux version 1 full or incremental streams from read-only roots.
 * Pathname/inode inventories live in userspace memory and still visit every
 * name. Paginated tree-item ioctls pin immutable roots with open descriptors;
 * each call excludes root administration and releases all tree buffers before
 * returning. Comparing both directions finds changed/deleted items and skips
 * shared subtrees by block address and generation, even at different heights.
 *
 * Extent iterators compare allocation identities and decoded offsets,
 * including split compressed mappings, and read data only for emitted WRITEs.
 * Unchanged data is omitted or cloned from the parent; holes and preallocation
 * are skipped unless retained parent data must be replaced with zero WRITEs,
 * as version 1 requires. There is no content deduplication or search for clone
 * sources at other inode/offset pairs.
 *
 * Remove old xattrs before creation/data changes and restore them after
 * ownership and mode changes, avoiding Linux ACL inheritance and capability
 * loss. Version 2/3 commands, no-data streams, recursive subvolumes and inode
 * flag preservation are unsupported. Symlink permissions are not transmitted.
 */

#include "kerncompat.h"
#include <sys/ioctl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "common/help.h"
#include "common/messages.h"
#include "common/send-stream.h"
#include "crypto/crc32c.h"
#include "kernel-shared/send.h"
#include "cmds/commands.h"
#include "cmds/stream-openbsd.h"

struct entry {
	char *path;
	struct stat st;
	int keep;
	int changed;
};
struct inventory {
	struct entry *entries;
	struct entry **inodes;
	size_t count;
	size_t capacity;
};
struct sender {
	struct stream_fs *fs;
	struct stream_root root, parent;
	struct inventory current, old;
	unsigned char command[BTRFS_SEND_BUF_SIZE_V1];
	size_t length;
	int fd;
	int error;
};

struct tree_reader {
	struct btrfs_ioctl_tree args;
	unsigned char buffer[BTRFS_TREE_BUFSIZE];
	size_t pos;
};

static void
tree_reader_init(struct tree_reader *r, int fd, int parent, uint64_t ino)
{
	memset(r, 0, sizeof(*r));
	r->args.fd = fd;
	r->args.parent_fd = parent;
	r->args.buffer = r->buffer;
	r->args.max.objectid = UINT64_MAX;
	r->args.max.type = 255;
	r->args.max.offset = UINT64_MAX;
	if (ino) {
		r->args.min.objectid = r->args.max.objectid = ino;
		r->args.min.type = r->args.max.type = BTRFS_EXTENT_DATA_KEY;
	}
}

/* Returned storage remains valid until the next call. */
static int
tree_reader_next(struct sender *s, struct tree_reader *r,
    struct btrfs_tree_item **item)
{
	size_t bytes;
	if (btrfs_send_stream_cancelled)
		return -EINTR;
	while (r->pos == r->args.size) {
		if (r->args.done)
			return 0;
		r->args.size = sizeof(r->buffer);
		if (ioctl(s->fs->control, BTRFSIOC_TREE, &r->args) == -1)
			return -errno;
		r->pos = 0;
	}
	if (r->args.size - r->pos < sizeof(**item))
		return -EIO;
	*item = (void *)(r->buffer + r->pos);
	bytes = (sizeof(**item) + (*item)->size + 7) & ~(size_t)7;
	if (bytes > r->args.size - r->pos)
		return -EIO;
	r->pos += bytes;
	return 1;
}

/*
 * Mark both additions/changes and deletions. Names still use the namespace
 * inventory, but unchanged inodes need no extent or xattr enumeration.
 */
static int
inventory_changes(struct sender *s)
{
	struct tree_reader *r;
	struct btrfs_tree_item *item;
	size_t lo, hi, mid;
	int pass, ret = 0;

	r = malloc(sizeof(*r));
	if (!r)
		return -ENOMEM;
	for (pass = 0; pass < 2; pass++) {
		tree_reader_init(r, pass ? s->parent.fd : s->root.fd,
		    pass ? s->root.fd : s->parent.fd, 0);
		r->args.flags = BTRFS_TREE_KEYS;
		while ((ret = tree_reader_next(s, r, &item)) > 0) {
			lo = 0;
			hi = s->current.count;
			while (lo < hi) {
				mid = lo + (hi - lo) / 2;
				if (s->current.inodes[mid]->st.st_ino <
				    item->key.objectid)
					lo = mid + 1;
				else
					hi = mid;
			}
			while (lo < s->current.count &&
			    s->current.inodes[lo]->st.st_ino == item->key.objectid)
				s->current.inodes[lo++]->changed = 1;
		}
		if (ret < 0)
			break;
	}
	free(r);
	return ret;
}

static int
entry_path_cmp(const void *a, const void *b)
{
	const struct entry *ea = a, *eb = b;
	return strcmp(ea->path, eb->path);
}

static int
entry_inode_cmp(const void *a, const void *b)
{
	const struct entry *ea = *(struct entry *const *)a;
	const struct entry *eb = *(struct entry *const *)b;
	if (ea->st.st_ino != eb->st.st_ino)
		return ea->st.st_ino < eb->st.st_ino ? -1 : 1;
	if (ea->st.st_gen != eb->st.st_gen)
		return ea->st.st_gen < eb->st.st_gen ? -1 : 1;
	return 0;
}

static struct entry *
find_path(struct inventory *inv, const char *path)
{
	struct entry key = { .path = (char *)path };
	return bsearch(&key, inv->entries, inv->count,
	    sizeof(*inv->entries), entry_path_cmp);
}

static struct entry *
find_inode(struct inventory *inv, struct entry *entry)
{
	struct entry **found;
	found = bsearch(&entry, inv->inodes, inv->count,
	    sizeof(*inv->inodes), entry_inode_cmp);
	return found ? *found : NULL;
}

static int
inventory_add(struct inventory *inv, const char *path, struct stat *st)
{
	struct entry *entry, *new;
	if (inv->count == inv->capacity) {
		size_t capacity = inv->capacity ? inv->capacity * 2 : 128;
		if (capacity < inv->capacity)
			return -EOVERFLOW;
		new = reallocarray(inv->entries, capacity, sizeof(*new));
		if (!new)
			return -ENOMEM;
		inv->entries = new;
		inv->capacity = capacity;
	}
	entry = &inv->entries[inv->count];
	memset(entry, 0, sizeof(*entry));
	entry->path = strdup(path);
	if (!entry->path)
		return -ENOMEM;
	entry->st = *st;
	inv->count++;
	return 0;
}

static int
inventory_walk(struct inventory *inv, int root, const char *path, dev_t device)
{
	struct stat st;
	struct dirent *de;
	DIR *dir;
	char child[PATH_MAX], name[NAME_MAX + 1];
	int parent, fd, ret;

	ret = stream_stat(root, path, &st);
	if (ret)
		return ret;
	/* Nested subvolumes and snapshot boundary stubs are not recursive. */
	if (st.st_dev != device || st.st_ino == 2)
		return -EOPNOTSUPP;
	ret = inventory_add(inv, path, &st);
	if (ret || !S_ISDIR(st.st_mode))
		return ret;
	parent = stream_parent(root, path, name);
	if (parent < 0)
		return parent;
	fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	close(parent);
	if (fd == -1)
		return -errno;
	dir = fdopendir(fd);
	if (!dir) {
		ret = -errno;
		close(fd);
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
		if (snprintf(child, sizeof(child), "%s%s%s",
		    !strcmp(path, ".") ? "" : path,
		    !strcmp(path, ".") ? "" : "/", de->d_name) >= sizeof(child)) {
			ret = -ENAMETOOLONG;
			break;
		}
		ret = inventory_walk(inv, root, child, device);
		if (ret)
			break;
	}
	closedir(dir);
	return ret;
}

static int
inventory_load(struct inventory *inv, int fd)
{
	struct stat st;
	size_t i;
	int ret;
	if (fstat(fd, &st) == -1)
		return -errno;
	ret = inventory_walk(inv, fd, ".", st.st_dev);
	if (ret)
		return ret;
	qsort(inv->entries, inv->count, sizeof(*inv->entries), entry_path_cmp);
	inv->inodes = reallocarray(NULL, inv->count, sizeof(*inv->inodes));
	if (!inv->inodes)
		return -ENOMEM;
	for (i = 0; i < inv->count; i++)
		inv->inodes[i] = &inv->entries[i];
	qsort(inv->inodes, inv->count, sizeof(*inv->inodes), entry_inode_cmp);
	return 0;
}

static void
inventory_free(struct inventory *inv)
{
	size_t i;
	for (i = 0; i < inv->count; i++)
		free(inv->entries[i].path);
	free(inv->entries);
	free(inv->inodes);
}

static void
start_cmd(struct sender *s, uint16_t cmd)
{
	struct btrfs_cmd_header *h = (void *)s->command;
	memset(h, 0, sizeof(*h));
	h->cmd = cpu_to_le16(cmd);
	s->length = sizeof(*h);
}

static void
attr(struct sender *s, uint16_t type, const void *data, size_t length)
{
	struct btrfs_tlv_header h;
	if (length > UINT16_MAX || s->length + sizeof(h) + length >
	    sizeof(s->command)) {
		s->error = -EOVERFLOW;
		return;
	}
	h.tlv_type = cpu_to_le16(type);
	h.tlv_len = cpu_to_le16(length);
	memcpy(s->command + s->length, &h, sizeof(h));
	s->length += sizeof(h);
	memcpy(s->command + s->length, data, length);
	s->length += length;
}

static void attr_u64(struct sender *s, uint16_t type, uint64_t value)
{ __le64 v = cpu_to_le64(value); attr(s, type, &v, sizeof(v)); }
static void attr_path(struct sender *s, uint16_t type, const char *path)
{ attr(s, type, path, strlen(path)); }

static void
end_cmd(struct sender *s)
{
	struct btrfs_cmd_header *h = (void *)s->command;
	if (s->error)
		return;
	h->len = cpu_to_le32(s->length - sizeof(*h));
	h->crc = cpu_to_le32(crc32c(0, s->command, s->length));
	s->error = stream_write(s->fd, s->command, s->length);
}

static void
path_cmd(struct sender *s, uint16_t cmd, const char *path)
{
	start_cmd(s, cmd);
	attr_path(s, BTRFS_SEND_A_PATH, path);
	end_cmd(s);
}

static void
number_cmd(struct sender *s, uint16_t cmd, const char *path, uint16_t type,
    uint64_t value)
{
	start_cmd(s, cmd);
	attr_path(s, BTRFS_SEND_A_PATH, path);
	attr_u64(s, type, value);
	end_cmd(s);
}

static int
send_create(struct sender *s, struct entry *entry)
{
	char target[PATH_MAX], name[NAME_MAX + 1];
	struct stat *st = &entry->st;
	uint64_t dev;
	ssize_t len = 0;
	int cmd, fd;

	if (S_ISREG(st->st_mode))
		cmd = BTRFS_SEND_C_MKFILE;
	else if (S_ISDIR(st->st_mode))
		cmd = BTRFS_SEND_C_MKDIR;
	else if (S_ISFIFO(st->st_mode))
		cmd = BTRFS_SEND_C_MKFIFO;
	else if (S_ISSOCK(st->st_mode))
		cmd = BTRFS_SEND_C_MKSOCK;
	else if (S_ISCHR(st->st_mode) || S_ISBLK(st->st_mode))
		cmd = BTRFS_SEND_C_MKNOD;
	else if (S_ISLNK(st->st_mode)) {
		cmd = BTRFS_SEND_C_SYMLINK;
		fd = stream_parent(s->root.fd, entry->path, name);
		if (fd < 0)
			return fd;
		len = readlinkat(fd, name, target, sizeof(target));
		close(fd);
		if (len < 0)
			return -errno;
		if (len == sizeof(target))
			return -ENAMETOOLONG;
	} else
		return -EOPNOTSUPP;
	start_cmd(s, cmd);
	attr_path(s, BTRFS_SEND_A_PATH, entry->path);
	attr_u64(s, BTRFS_SEND_A_INO, st->st_ino);
	if (cmd == BTRFS_SEND_C_MKNOD) {
		dev = (minor(st->st_rdev) & 0xff) |
		    ((uint64_t)major(st->st_rdev) << 8) |
		    ((uint64_t)(minor(st->st_rdev) & ~0xff) << 12);
		attr_u64(s, BTRFS_SEND_A_MODE, st->st_mode);
		attr_u64(s, BTRFS_SEND_A_RDEV, dev);
	}
	if (cmd == BTRFS_SEND_C_SYMLINK)
		attr(s, BTRFS_SEND_A_PATH_LINK, target, len);
	end_cmd(s);
	return s->error;
}

static void
send_time(struct sender *s, uint16_t type, struct timespec time)
{
	struct btrfs_timespec value;
	value.sec = cpu_to_le64(time.tv_sec);
	value.nsec = cpu_to_le32(time.tv_nsec);
	attr(s, type, &value, sizeof(value));
}

static int
send_xattrs(struct sender *s, struct entry *entry, int set)
{
	struct btrfs_ioctl_xattr args;
	struct entry *old = entry->keep ? find_path(&s->old, entry->path) : NULL;
	char value[65536];
	uint64_t cursor = 0;

	if (!set && !old)
		return 0;
	for (;;) {
		memset(&args, 0, sizeof(args));
		args.fd = set ? s->root.fd : s->parent.fd;
		args.ino = set ? entry->st.st_ino : old->st.st_ino;
		args.cursor = cursor;
		args.value = value;
		args.size = sizeof(value);
		if (ioctl(s->fs->control, BTRFSIOC_GETXATTR, &args) == -1) {
			if (errno == ENOENT)
				break;
			return -errno;
		}
		cursor = args.cursor;
		start_cmd(s, set ? BTRFS_SEND_C_SET_XATTR :
		    BTRFS_SEND_C_REMOVE_XATTR);
		attr_path(s, BTRFS_SEND_A_PATH, entry->path);
		attr_path(s, BTRFS_SEND_A_XATTR_NAME, args.name);
		if (set)
			attr(s, BTRFS_SEND_A_XATTR_DATA, value, args.size);
		end_cmd(s);
		if (s->error)
			return s->error;
	}
	return 0;
}

static int
send_metadata(struct sender *s, struct entry *entry)
{
	struct stat *st = &entry->st;
	int ret;

	start_cmd(s, BTRFS_SEND_C_CHOWN);
	attr_path(s, BTRFS_SEND_A_PATH, entry->path);
	attr_u64(s, BTRFS_SEND_A_UID, st->st_uid);
	attr_u64(s, BTRFS_SEND_A_GID, st->st_gid);
	end_cmd(s);
	if (!S_ISLNK(st->st_mode))
		number_cmd(s, BTRFS_SEND_C_CHMOD, entry->path,
		    BTRFS_SEND_A_MODE, st->st_mode & 07777);
	/* Linux writes/chown clear capabilities and chmod adjusts ACLs.
	 * Restore xattrs after those operations, then restore timestamps. */
	ret = send_xattrs(s, entry, 1);
	if (ret)
		return ret;
	start_cmd(s, BTRFS_SEND_C_UTIMES);
	attr_path(s, BTRFS_SEND_A_PATH, entry->path);
	send_time(s, BTRFS_SEND_A_ATIME, st->st_atim);
	send_time(s, BTRFS_SEND_A_MTIME, st->st_mtim);
	send_time(s, BTRFS_SEND_A_CTIME, st->st_ctim);
	end_cmd(s);
	return s->error;
}

struct extent_reader {
	struct tree_reader tree;
	struct btrfs_tree_item *item;
	struct btrfs_file_extent_item *fi;
	uint64_t start, end, size;
};

/*
 * Return a data mapping or a hole and its next boundary. Preallocation,
 * explicit holes, absent NO_HOLES items, and space past EOF all read as zero.
 * Iterators retain only one bounded batch each, even for fragmented files.
 */
static int
extent_at(struct sender *s, struct extent_reader *r, uint64_t pos,
    uint64_t *end, int *hole)
{
	uint64_t length;
	int ret;

	*hole = 1;
	*end = UINT64_MAX;
	if (pos >= r->size)
		return 0;
	while (r->end <= pos) {
		ret = tree_reader_next(s, &r->tree, &r->item);
		if (ret < 0)
			return ret;
		if (!ret) {
			r->fi = NULL;
			r->end = UINT64_MAX;
			break;
		}
		if (r->item->size < offsetof(struct btrfs_file_extent_item,
		    disk_bytenr))
			return -EIO;
		r->fi = (void *)(r->item + 1);
		r->start = r->item->key.offset;
		if (r->start < r->end)
			return -EIO;
		if (r->fi->type == BTRFS_FILE_EXTENT_INLINE) {
			if (r->start != 0)
				return -EIO;
			length = le64_to_cpu(r->fi->ram_bytes);
		} else {
			if (r->item->size != sizeof(*r->fi) ||
			    (r->fi->type != BTRFS_FILE_EXTENT_REG &&
			    r->fi->type != BTRFS_FILE_EXTENT_PREALLOC))
				return -EIO;
			length = le64_to_cpu(r->fi->num_bytes);
			if (le64_to_cpu(r->fi->offset) > UINT64_MAX - length)
				return -EIO;
		}
		if (!length || length > UINT64_MAX - r->start)
			return -EIO;
		r->end = r->start + length;
	}
	*end = MIN(r->end, r->size);
	if (r->fi && pos < r->start)
		*end = MIN(r->start, r->size);
	else if (r->fi)
		*hole = r->fi->type == BTRFS_FILE_EXTENT_PREALLOC ||
		    (r->fi->type == BTRFS_FILE_EXTENT_REG &&
		    r->fi->disk_bytenr == 0);
	return 0;
}

static int
extent_equal(struct extent_reader *a, struct extent_reader *b, uint64_t pos)
{
	struct btrfs_file_extent_item *x = a->fi, *y = b->fi;
	if (x->type != y->type)
		return 0;
	if (x->type == BTRFS_FILE_EXTENT_INLINE)
		return a->item->size == b->item->size &&
		    !memcmp(x, y, a->item->size);
	return x->disk_bytenr == y->disk_bytenr &&
	    x->disk_num_bytes == y->disk_num_bytes &&
	    x->ram_bytes == y->ram_bytes &&
	    x->compression == y->compression && x->encryption == y->encryption &&
	    x->other_encoding == y->other_encoding &&
	    le64_to_cpu(x->offset) + pos - a->start ==
	    le64_to_cpu(y->offset) + pos - b->start;
}

static int
send_data(struct sender *s, struct entry *entry)
{
	struct entry *old = find_inode(&s->old, entry);
	struct extent_reader *a, *b;
	unsigned char data[32768];
	uint64_t offset, end, oldend, len, sector, size = entry->st.st_size;
	int fd = -1, ret = 0, hole, oldhole, equal;

	if (entry->keep && !entry->changed && s->parent.fd != -1)
		return 0;
	if (old && !S_ISREG(old->st.st_mode))
		old = NULL;
	a = calloc(2, sizeof(*a));
	if (!a)
		return -ENOMEM;
	b = a + 1;
	tree_reader_init(&a->tree, s->root.fd, -1, entry->st.st_ino);
	a->size = size;
	if (old) {
		tree_reader_init(&b->tree, s->parent.fd, -1, old->st.st_ino);
		b->size = old->st.st_size;
	}
	for (offset = 0; offset < size; offset += len) {
		ret = extent_at(s, a, offset, &end, &hole);
		if (!ret)
			ret = extent_at(s, b, offset, &oldend, &oldhole);
		if (ret)
			break;
		sector = a->tree.args.sectorsize;
		len = MIN(end, oldend) - offset;
		equal = hole ? oldhole :
		    (!oldhole && extent_equal(a, b, offset));
		if ((equal && entry->keep) || (hole && !entry->keep))
			continue;
		/*
		 * Linux requires aligned clone starts and lengths, except source
		 * EOF. Inline data cannot be reflinked. Split off a partial tail
		 * and WRITE it if the target is shorter than the clone source.
		 */
		equal = equal && !hole && a->fi->type == BTRFS_FILE_EXTENT_REG &&
		    offset % sector == 0;
		if (equal && len % sector && offset + len != b->size) {
			if (len >= sector)
				len -= len % sector;
			else
				equal = 0;
		}
		if (!equal) {
			len = MIN(len, sizeof(data));
			if (hole)
				memset(data, 0, len);
			else {
				if (fd == -1)
					fd = stream_open_file(s->root.fd,
					    entry->path, O_RDONLY);
				if (fd < 0) {
					ret = fd;
					break;
				}
				ret = stream_pread(fd, data, len, offset);
				if (ret)
					break;
			}
		}
		start_cmd(s, equal ? BTRFS_SEND_C_CLONE : BTRFS_SEND_C_WRITE);
		attr_path(s, BTRFS_SEND_A_PATH, entry->path);
		attr_u64(s, BTRFS_SEND_A_FILE_OFFSET, offset);
		if (equal) {
			attr(s, BTRFS_SEND_A_CLONE_UUID,
			    stream_uuid(&s->parent.info), 16);
			attr_u64(s, BTRFS_SEND_A_CLONE_CTRANSID,
			    stream_transid(&s->parent.info));
			attr_path(s, BTRFS_SEND_A_CLONE_PATH, old->path);
			attr_u64(s, BTRFS_SEND_A_CLONE_OFFSET, offset);
			attr_u64(s, BTRFS_SEND_A_CLONE_LEN, len);
		} else
			attr(s, BTRFS_SEND_A_DATA, data, len);
		end_cmd(s);
		if ((ret = s->error) != 0)
			break;
	}
	if (!ret) {
		number_cmd(s, BTRFS_SEND_C_TRUNCATE, entry->path,
		    BTRFS_SEND_A_SIZE, size);
		ret = s->error;
	}
	if (fd != -1)
		close(fd);
	free(a);
	return ret;
}

static int
same_symlink(struct sender *s, struct entry *a, struct entry *b)
{
	char name[NAME_MAX + 1], first[PATH_MAX], second[PATH_MAX];
	int fd;
	ssize_t alen, blen;
	fd = stream_parent(s->root.fd, a->path, name);
	if (fd < 0)
		return fd;
	alen = readlinkat(fd, name, first, sizeof(first));
	close(fd);
	if (alen < 0)
		return -errno;
	fd = stream_parent(s->parent.fd, b->path, name);
	if (fd < 0)
		return fd;
	blen = readlinkat(fd, name, second, sizeof(second));
	close(fd);
	if (blen < 0)
		return -errno;
	return alen == blen && !memcmp(first, second, alen);
}

/*
 * Reconcile names before updating contents. Matching directory paths stay
 * in place, regardless of inode identity. Non-directories stay only when
 * inode and creation generation match. Remove obsolete children before
 * parents, create directories before files, then form target hardlink groups.
 * Thus directory rename cycles require no special temporary-name protocol.
 */
static int
send_changes(struct sender *s)
{
	struct entry *a, *b, *canonical;
	size_t i, j, k;
	int ret;

	for (i = 0; i < s->current.count; i++) {
		a = &s->current.entries[i];
		b = find_path(&s->old, a->path);
		if (!strcmp(a->path, ".") ||
		    (b && ((S_ISDIR(a->st.st_mode) && S_ISDIR(b->st.st_mode)) ||
		    ((a->st.st_mode & S_IFMT) == (b->st.st_mode & S_IFMT) &&
		    a->st.st_ino == b->st.st_ino && a->st.st_gen == b->st.st_gen)))) {
			/* Unrelated trees can allocate matching inode/generation
			 * pairs. Regular data is compared later; immutable node
			 * contents need an explicit comparison before retaining
			 * their names. */
			if (b && S_ISLNK(a->st.st_mode)) {
				ret = same_symlink(s, a, b);
				if (ret < 0)
					return ret;
				if (!ret)
					continue;
			}
			if (b && (S_ISCHR(a->st.st_mode) || S_ISBLK(a->st.st_mode)) &&
			    a->st.st_rdev != b->st.st_rdev)
				continue;
			a->keep = 1;
			if (b)
				b->keep = 1;
		}
	}
	/* Retained Linux default ACLs would otherwise be inherited by newly
	 * created children. Each inode's final xattrs are restored explicitly. */
	for (i = 0; i < s->current.count; i++) {
		a = &s->current.entries[i];
		if (S_ISDIR(a->st.st_mode)) {
			ret = send_xattrs(s, a, 0);
			if (ret)
				return ret;
		}
	}
	for (i = s->old.count; i > 0; i--) {
		a = &s->old.entries[i - 1];
		if (!a->keep)
			path_cmd(s, S_ISDIR(a->st.st_mode) ?
			    BTRFS_SEND_C_RMDIR : BTRFS_SEND_C_UNLINK, a->path);
	}
	for (i = 0; i < s->current.count; i++) {
		a = &s->current.entries[i];
		if (!a->keep && S_ISDIR(a->st.st_mode)) {
			ret = send_create(s, a);
			if (ret)
				return ret;
		}
	}
	for (i = 0; i < s->current.count; i = j) {
		canonical = s->current.inodes[i];
		for (j = i + 1; j < s->current.count &&
		    !entry_inode_cmp(&s->current.inodes[i],
		    &s->current.inodes[j]); j++)
			if (s->current.inodes[j]->keep)
				canonical = s->current.inodes[j];
		if (S_ISDIR(canonical->st.st_mode))
			continue;
		if (!canonical->keep) {
			ret = send_create(s, canonical);
			if (ret)
				return ret;
		}
		for (k = i; k < j; k++) {
			a = s->current.inodes[k];
			if (a == canonical || a->keep)
				continue;
			start_cmd(s, BTRFS_SEND_C_LINK);
			attr_path(s, BTRFS_SEND_A_PATH, a->path);
			attr_path(s, BTRFS_SEND_A_PATH_LINK, canonical->path);
			end_cmd(s);
		}
		if (canonical->keep && !canonical->changed && s->parent.fd != -1)
			continue;
		/* Remove parent xattrs before data changes can invalidate them on
		 * Linux; a later REMOVE_XATTR would fail for a cleared capability. */
		ret = send_xattrs(s, canonical, 0);
		if (ret)
			return ret;
		if (S_ISREG(canonical->st.st_mode)) {
			ret = send_data(s, canonical);
			if (ret)
				return ret;
		}
		ret = send_metadata(s, canonical);
		if (ret)
			return ret;
	}
	/* Restore directory times after all namespace changes. */
	for (i = s->current.count; i > 0; i--) {
		a = &s->current.entries[i - 1];
		if (S_ISDIR(a->st.st_mode)) {
			ret = send_metadata(s, a);
			if (ret)
				return ret;
		}
	}
	start_cmd(s, BTRFS_SEND_C_END);
	end_cmd(s);
	return s->error;
}

static const char * const send_usage[] = {
	"btrfs send [-f <stream>] [-p <parent>] <mountpoint> <path>",
	"Send a read-only subvolume as a Linux version 1 stream",
	"",
	"-f <stream>  write to a new file instead of standard output",
	"-p <parent> send changes relative to a read-only parent",
	"",
	"Both paths start at the filesystem root (tree 5).",
	NULL
};

static int
cmd_send(const struct cmd_struct *cmd, int argc, char **argv)
{
	struct sender s = { .root.fd = -1, .parent.fd = -1, .fd = STDOUT_FILENO };
	struct stream_fs fs;
	struct btrfs_stream_header header = {
		.magic = BTRFS_SEND_STREAM_MAGIC, .version = cpu_to_le32(1)
	};
	const char *parent = NULL, *file = NULL, *name;
	int ch, ret, err;

	optind = 1;
	while ((ch = getopt(argc, argv, "p:f:")) != -1) {
		switch (ch) {
		case 'p': parent = optarg; break;
		case 'f': file = optarg; break;
		default: goto usage;
		}
	}
	if (argc - optind != 2)
		goto usage;
	ret = stream_fs_open(&fs, argv[optind]);
	if (ret)
		goto error;
	s.fs = &fs;
	ret = stream_root_open(&fs, argv[optind + 1], 0, &s.root);
	if (ret)
		goto out;
	if (!(s.root.info.flags & BTRFS_CTL_RDONLY)) {
		ret = -EROFS;
		goto out;
	}
	if (parent) {
		ret = stream_root_open(&fs, parent, 0, &s.parent);
		if (ret)
			goto out;
		if (!(s.parent.info.flags & BTRFS_CTL_RDONLY) ||
		    s.parent.info.id == s.root.info.id) {
			ret = -EINVAL;
			goto out;
		}
		ret = inventory_load(&s.old, s.parent.fd);
		if (ret)
			goto out;
	}
	ret = inventory_load(&s.current, s.root.fd);
	if (ret)
		goto out;
	if (parent) {
		ret = inventory_changes(&s);
		if (ret)
			goto out;
	}
	if (file) {
		s.fd = open(file, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (s.fd == -1) {
			ret = -errno;
			goto out;
		}
	} else if (isatty(s.fd)) {
		ret = -EINVAL;
		goto out;
	}
	ret = stream_write(s.fd, &header, sizeof(header));
	if (ret)
		goto out;
	name = strrchr(s.root.info.path, '/');
	name = name ? name + 1 : s.root.info.path;
	start_cmd(&s, parent ? BTRFS_SEND_C_SNAPSHOT : BTRFS_SEND_C_SUBVOL);
	attr_path(&s, BTRFS_SEND_A_PATH, name);
	attr(&s, BTRFS_SEND_A_UUID, stream_uuid(&s.root.info), 16);
	attr_u64(&s, BTRFS_SEND_A_CTRANSID, stream_transid(&s.root.info));
	if (parent) {
		attr(&s, BTRFS_SEND_A_CLONE_UUID, stream_uuid(&s.parent.info), 16);
		attr_u64(&s, BTRFS_SEND_A_CLONE_CTRANSID, stream_transid(&s.parent.info));
	}
	end_cmd(&s);
	ret = send_changes(&s);
out:
	if (s.fd != STDOUT_FILENO && s.fd != -1 && close(s.fd) == -1 && !ret)
		ret = -errno;
	err = stream_root_close(&s.parent);
	if (!ret)
		ret = err;
	err = stream_root_close(&s.root);
	if (!ret)
		ret = err;
	inventory_free(&s.current);
	inventory_free(&s.old);
	stream_fs_close(&fs);
error:
	if (ret)
		error("send: %s", strerror(-ret));
	return ret ? 1 : 0;
usage:
	usage_command(cmd, false, false);
	return 1;
}
DEFINE_COMMAND(send, "send", cmd_send, send_usage, NULL, 0);
