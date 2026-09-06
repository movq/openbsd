/* Public domain. */
#include <sys/param.h>
#include <sys/buf.h>
#include <sys/systm.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/filedesc.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <sys/btrfsio.h>
#include <btrfs/btrfs_var.h>
#include <lib/libkern/crc32c.h>

struct subvol_entry {
	uint64_t ino;
	uint64_t index;
	int subvol;
	char name[BTRFS_NAME_MAX + 1];
};

static int
subvol_entry(const struct btrfs_dir_entry *entry, void *arg)
{
	struct subvol_entry *out = arg;

	out->ino = entry->bde_objectid;
	out->index = entry->bde_index;
	out->subvol = entry->bde_subvolume;
	memcpy(out->name, entry->bde_name, entry->bde_namelen);
	out->name[entry->bde_namelen] = '\0';
	return (0);
}

/* No symlinks or parent components: these are filesystem paths, not namei. */
static int
subvol_resolve(struct btrfs_fs *bmp, const char *path,
    struct btrfs_root **rootp, uint64_t *ino)
{
	struct btrfs_root *root;
	struct btrfs_inode inode;
	struct subvol_entry entry;
	const char *end;
	size_t len;
	int error;

	error = btrfs_get_root(bmp, BTRFS_FS_TREE_OBJECTID, &root);
	*ino = BTRFS_FIRST_FREE_OBJECTID;
	while (error == 0 && *path != '\0') {
		if (*path == '/') {
			path++;
			continue;
		}
		end = strchr(path, '/');
		len = end == NULL ? strlen(path) : end - path;
		if (len > BTRFS_NAME_MAX)
			return (ENAMETOOLONG);
		if (len == 2 && memcmp(path, "..", 2) == 0)
			return (EINVAL);
		if (len == 1 && *path == '.') {
			path += len;
			continue;
		}
		memset(&entry, 0, sizeof(entry));
		error = btrfs_lookup_directory(root, *ino, path, len,
		    subvol_entry, &entry);
		if (error != 0)
			break;
		if (entry.ino == 0)
			return (ENOENT);
		if (entry.subvol) {
			error = btrfs_check_subvol_link(root, *ino, entry.ino,
			    path, len);
			if (error != 0)
				break;
			error = btrfs_get_root(bmp, entry.ino, &root);
			*ino = BTRFS_FIRST_FREE_OBJECTID;
		} else
			*ino = entry.ino;
		if (error == 0)
			error = btrfs_find_inode(root, *ino, &inode);
		if (error == 0 && !S_ISDIR(inode.bi_mode))
			error = ENOTDIR;
		path += len;
	}
	*rootp = root;
	return (error);
}

static int
subvol_read(struct btrfs_root *root, const struct btrfs_key *key,
    void *buf, uint32_t max, uint32_t *size)
{
	struct btrfs_path path = { 0 };
	const uint8_t *data;
	uint32_t len;
	int error;

	error = btrfs_search_slot(root, key, &path);
	if (error == 0)
		error = btrfs_path_item(&path, NULL, &data, &len);
	if (error == 0 && len > max)
		error = EOVERFLOW;
	if (error == 0) {
		memcpy(buf, data, len);
		*size = len;
	}
	btrfs_release_path(&path);
	return (error);
}

/*
 * The caller serializes mounts and namespace changes. Finalization fences
 * vget, proves that no vnode user can still mutate this tree, then publishes
 * the identity and read-only flag in one transaction.
 */
int
btrfs_identity(struct btrfs_fs *bmp, u_long cmd,
    struct btrfs_ioctl_identity *args, struct proc *p)
{
	struct btrfs_root *root, *roots, *uuids;
	struct btrfs_root_item item;
	struct btrfs_trans_handle *handle = NULL;
	struct btrfs_trans_reservation res = { 0 };
	struct btrfs_key key = { 0 };
	struct timespec now;
	uint64_t ino, value, *values = NULL;
	uint32_t size, nodesize = letoh32(bmp->bm_super.nodesize);
	int error, enderror, gated = 0;

	if (args->id != 0)
		error = btrfs_get_root(bmp, args->id, &root);
	else {
		error = subvol_resolve(bmp, args->path, &root, &ino);
		if (error == 0 && ino != BTRFS_FIRST_FREE_OBJECTID)
			error = EINVAL;
	}
	if (error != 0)
		return (error);
	if (root->br_owner != BTRFS_FS_TREE_OBJECTID &&
	    (root->br_owner < BTRFS_FIRST_FREE_OBJECTID ||
	    root->br_owner > BTRFS_LAST_FREE_OBJECTID))
		return (EINVAL);
	args->id = root->br_owner;
	error = btrfs_get_root(bmp, BTRFS_ROOT_TREE_OBJECTID, &roots);
	if (error == 0)
		error = btrfs_find_root_item(roots, args->id,
		    BTRFS_FIRST_FREE_OBJECTID, &item, NULL);
	if (error != 0)
		return (error);
	if (cmd == BTRFSIOC_INFO) {
		args->flags = root->br_flags & BTRFS_ROOT_SUBVOL_RDONLY ?
		    BTRFS_CTL_RDONLY : 0;
		args->ctransid = letoh64(item.ctransid);
		args->stransid = letoh64(item.stransid);
		memcpy(args->uuid, item.uuid, sizeof(args->uuid));
		memcpy(args->received_uuid, item.received_uuid,
		    sizeof(args->received_uuid));
		return (btrfs_subvol_path(bmp, args->id, args->path,
		    sizeof(args->path)));
	}
	if (args->id == BTRFS_FS_TREE_OBJECTID || args->stransid == 0 ||
	    memcmp(args->received_uuid, (uint8_t[16]){0}, 16) == 0 ||
	    memcmp(item.received_uuid, (uint8_t[16]){0}, 16) != 0)
		return (EINVAL);
	if (root->br_flags & BTRFS_ROOT_SUBVOL_RDONLY)
		return (EROFS);
	mtx_enter(&bmp->bm_nodemtx);
	root->br_finalizing = 1;
	mtx_leave(&bmp->bm_nodemtx);
	error = btrfs_control_busy(bmp, args->id);
	if (error != 0)
		goto out;
	mtx_enter(&bmp->bm_trans_mtx);
	bmp->bm_control = p;
	while (bmp->bm_transaction->bt_writers != 0)
		msleep(&bmp->bm_transaction->bt_writers, &bmp->bm_trans_mtx,
		    PWAIT, "btrrecv", 0);
	mtx_leave(&bmp->bm_trans_mtx);
	gated = 1;
	error = btrfs_commit_current(bmp, p);
	/* Commit may have changed the root block and generation. */
	if (error == 0)
		error = btrfs_find_root_item(roots, args->id,
		    BTRFS_FIRST_FREE_OBJECTID, &item, NULL);
	if (error != 0)
		goto out;
	error = btrfs_get_root(bmp, BTRFS_UUID_TREE_OBJECTID, &uuids);
	if (error == ENOENT) {
		uuids = NULL;
		error = 0;
	}
	if (error != 0)
		goto out;
	memcpy(&key.objectid, args->received_uuid, 8);
	memcpy(&key.offset, args->received_uuid + 8, 8);
	key.type = BTRFS_UUID_KEY_RECEIVED_SUBVOL;
	size = 0;
	if (uuids != NULL) {
		values = malloc(nodesize, M_BTRFS, M_WAITOK | M_ZERO);
		error = subvol_read(uuids, &key, values, nodesize, &size);
		if (error == ENOENT)
			error = 0;
		if (error != 0)
			goto out;
		if (size % sizeof(value) != 0 || size + sizeof(value) >
		    nodesize - sizeof(struct btrfs_header) -
		    sizeof(struct btrfs_item)) {
			error = EOVERFLOW;
			goto out;
		}
		value = htole64(args->id);
		memcpy((uint8_t *)values + size, &value, sizeof(value));
	}
	res.btr_metadata = 64 * nodesize;
	error = btrfs_trans_join(bmp, &res, &handle);
	if (error != 0)
		goto out;
	memcpy(item.received_uuid, args->received_uuid, 16);
	item.stransid = htole64(args->stransid);
	item.rtransid = htole64(handle->bth_transaction->bt_generation);
	nanotime(&now);
	item.rtime.sec = htole64(now.tv_sec);
	item.rtime.nsec = htole32(now.tv_nsec);
	item.flags = htole64(letoh64(item.flags) | BTRFS_ROOT_SUBVOL_RDONLY);
	item.inode.flags = htole64(letoh64(item.inode.flags) |
	    BTRFS_INODE_ROOT_ITEM_INIT);
	if (uuids != NULL) {
		if (size == 0)
			error = btrfs_insert_item(handle, uuids, &key, values,
			    sizeof(value));
		else
			error = btrfs_replace_item(handle, uuids, &key, values,
			    size + sizeof(value));
	}
	memset(&key, 0, sizeof(key));
	key.objectid = htole64(args->id);
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = htole64(root->br_root_offset);
	if (error == 0)
		error = btrfs_replace_item(handle, roots, &key, &item, sizeof(item));
	if (error != 0)
		btrfs_trans_abort(handle, error);
	enderror = btrfs_trans_end(handle);
	if (error == 0)
		error = enderror;
	if (error == 0)
		error = btrfs_commit_current(bmp, p);
	if (error == 0)
		root->br_flags |= BTRFS_ROOT_SUBVOL_RDONLY;
out:
	free(values, M_BTRFS, nodesize);
	mtx_enter(&bmp->bm_nodemtx);
	root->br_finalizing = 0;
	mtx_leave(&bmp->bm_nodemtx);
	if (gated) {
		mtx_enter(&bmp->bm_trans_mtx);
		bmp->bm_control = NULL;
		wakeup(&bmp->bm_control);
		mtx_leave(&bmp->bm_trans_mtx);
	}
	return (error);
}

struct subvol_name {
	uint64_t ino;
	struct subvol_entry entry;
};

int
btrfs_control_xattr(struct vnode *select, u_long cmd,
    struct btrfs_ioctl_xattr *args, struct proc *p)
{
	struct btrfs_fs *bmp = VTOBTRFS(select)->bn_mount;
	struct btrfs_node *node;
	struct btrfs_root *root;
	struct vnode *vp;
	struct btrfs_path path = { 0 };
	struct btrfs_key target = { 0 };
	const struct btrfs_key *key;
	const uint8_t *data;
	struct btrfs_dir_item *di;
	struct btrfs_trans_handle *handle;
	struct btrfs_trans_reservation res = { 0 };
	uint8_t *buffer, *value = NULL;
	uint64_t cursor = 0;
	uint32_t size = 0, pos, step, len, cap;
	size_t namelen = strlen(args->name);
	int error, enderror, found = 0;

	if (args->ino < BTRFS_FIRST_FREE_OBJECTID)
		return (EINVAL);
	error = btrfs_vget_tree(select->v_mount, VTOBTRFS(select)->bn_treeid,
	    args->ino, &vp);
	if (error != 0)
		return (error);
	node = VTOBTRFS(vp);
	root = node->bn_root;
	cap = letoh32(bmp->bm_super.nodesize);
	buffer = malloc(cap, M_BTRFS, M_WAITOK);
	target.objectid = htole64(args->ino);
	target.type = BTRFS_XATTR_ITEM_KEY;
	if (cmd == BTRFSIOC_GETXATTR) {
		error = btrfs_search_lower_bound(root, &target, &path);
		while (error == 0) {
			error = btrfs_path_item(&path, &key, &data, &size);
			if (error != 0)
				break;
			if (key->objectid != target.objectid ||
			    key->type != target.type) {
				error = ENOENT;
				break;
			}
			for (pos = 0; pos < size; pos += step) {
				di = (struct btrfs_dir_item *)(data + pos);
				if (size - pos < sizeof(*di)) {
					error = EINVAL;
					break;
				}
				len = letoh16(di->name_len);
				step = sizeof(*di) + len + letoh16(di->data_len);
				if (len == 0 || len > BTRFS_NAME_MAX ||
				    step > size - pos || memchr(di + 1, '\0', len)) {
					error = EINVAL;
					break;
				}
				if (cursor++ != args->cursor)
					continue;
				if (args->size < letoh16(di->data_len)) {
					error = ERANGE;
					break;
				}
				memcpy(args->name, di + 1, len);
				args->name[len] = '\0';
				args->size = letoh16(di->data_len);
				memcpy(buffer, (uint8_t *)(di + 1) + len, args->size);
				found = 1;
				break;
			}
			if (error != 0 || found)
				break;
			error = btrfs_next_item(&path);
		}
		btrfs_release_path(&path);
		if (error == 0 && found) {
			error = copyout(buffer, args->value, args->size);
			if (error == 0)
				args->cursor++;
		}
		goto out;
	}
	if (bmp->bm_readonly || (select->v_mount->mnt_flag & MNT_RDONLY) ||
	    (root->br_flags & BTRFS_ROOT_SUBVOL_RDONLY)) {
		error = EROFS;
		goto out;
	}
	if (node->bn_inode.bi_flags &
	    (BTRFS_INODE_READONLY | BTRFS_INODE_IMMUTABLE | BTRFS_INODE_APPEND)) {
		error = EPERM;
		goto out;
	}
	if (namelen == 0 || args->cursor != 0 ||
	    args->size > cap - sizeof(struct btrfs_header) -
	    sizeof(struct btrfs_item) - sizeof(*di) - namelen ||
	    (cmd == BTRFSIOC_RMXATTR && args->size != 0)) {
		error = EINVAL;
		goto out;
	}
	if (cmd == BTRFSIOC_SETXATTR) {
		value = malloc(MAX(args->size, 1), M_BTRFS, M_WAITOK);
		error = copyin(args->value, value, args->size);
		if (error != 0)
			goto out;
	}
	target.offset = htole64(crc32c(1, (const uint8_t *)args->name,
	    namelen) ^ 0xffffffffU);
	error = subvol_read(root, &target, buffer, cap, &size);
	if (error == ENOENT)
		error = 0;
	if (error != 0)
		goto out;
	for (pos = 0; pos < size; pos += step) {
		di = (struct btrfs_dir_item *)(buffer + pos);
		if (size - pos < sizeof(*di)) {
			error = EINVAL;
			goto out;
		}
		step = sizeof(*di) + letoh16(di->name_len) + letoh16(di->data_len);
		if (step > size - pos) {
			error = EINVAL;
			goto out;
		}
		if (letoh16(di->name_len) == namelen &&
		    memcmp(di + 1, args->name, namelen) == 0) {
			found = 1;
			break;
		}
	}
	len = size;
	if (found) {
		memmove(buffer + pos, buffer + pos + step, size - pos - step);
		size -= step;
	} else if (cmd == BTRFSIOC_RMXATTR) {
		error = ENOENT;
		goto out;
	}
	if (cmd == BTRFSIOC_SETXATTR) {
		step = sizeof(*di) + namelen + args->size;
		if (size + step > cap - sizeof(struct btrfs_header) -
		    sizeof(struct btrfs_item)) {
			error = ENOSPC;
			goto out;
		}
		di = (struct btrfs_dir_item *)(buffer + size);
		memset(di, 0, sizeof(*di));
		di->type = BTRFS_FT_XATTR;
		di->name_len = htole16(namelen);
		di->data_len = htole16(args->size);
		memcpy(di + 1, args->name, namelen);
		memcpy((uint8_t *)(di + 1) + namelen, value, args->size);
		size += step;
	}
	res.btr_metadata = 64 * cap;
	error = btrfs_trans_join(bmp, &res, &handle);
	if (error != 0)
		goto out;
	if (size == 0)
		error = btrfs_delete_item(handle, root, &target);
	else if (len == 0)
		error = btrfs_insert_item(handle, root, &target, buffer, size);
	else
		error = btrfs_replace_item(handle, root, &target, buffer, size);
	if (error != 0)
		btrfs_trans_abort(handle, error);
	enderror = btrfs_trans_end(handle);
	if (error == 0)
		error = enderror;
out:
	free(value, M_BTRFS, MAX(args->size, 1));
	free(buffer, M_BTRFS, cap);
	vput(vp);
	return (error);
}

static int
subvol_find_name(const struct btrfs_dir_entry *entry, void *arg)
{
	struct subvol_name *name = arg;

	if (!entry->bde_subvolume && entry->bde_objectid == name->ino)
		return (subvol_entry(entry, &name->entry));
	return (0);
}

static int
subvol_prepend(char *path, size_t size, const char *name, size_t len)
{
	size_t old = strlen(path);

	if (len == 0 || len > BTRFS_NAME_MAX || memchr(name, '/', len) ||
	    memchr(name, '\0', len))
		return (EINVAL);
	if (len + (old != 0) + old >= size)
		return (ENAMETOOLONG);
	memmove(path + len + (old != 0), path, old + 1);
	memcpy(path, name, len);
	if (old != 0)
		path[len] = '/';
	return (0);
}

int
btrfs_subvol_path(struct btrfs_fs *bmp, uint64_t id, char *out, size_t len)
{
	struct btrfs_root *roots, *root;
	struct btrfs_path path = { 0 };
	struct btrfs_key target = { 0 };
	const struct btrfs_key *key;
	const struct btrfs_root_ref *ref;
	const uint8_t *data;
	struct subvol_name name;
	uint64_t ino, parent;
	uint32_t size;
	int error, steps = 0;

	out[0] = '\0';
	error = btrfs_get_root(bmp, BTRFS_ROOT_TREE_OBJECTID, &roots);
	while (error == 0 && id != BTRFS_FS_TREE_OBJECTID) {
		if (++steps > BTRFS_CTL_PATH_MAX)
			return (ELOOP);
		target.objectid = htole64(id);
		target.type = BTRFS_ROOT_BACKREF_KEY;
		target.offset = 0;
		error = btrfs_search_lower_bound(roots, &target, &path);
		if (error == 0)
			error = btrfs_path_item(&path, &key, &data, &size);
		if (error != 0)
			break;
		if (key->objectid != target.objectid ||
		    key->type != target.type || size < sizeof(*ref)) {
			error = EINVAL;
			break;
		}
		ref = (const struct btrfs_root_ref *)data;
		if (size - sizeof(*ref) != letoh16(ref->name_len)) {
			error = EINVAL;
			break;
		}
		id = letoh64(key->offset);
		ino = letoh64(ref->dirid);
		error = subvol_prepend(out, len, (const char *)(ref + 1),
		    letoh16(ref->name_len));
		btrfs_release_path(&path);
		if (error != 0)
			break;
		error = btrfs_get_root(bmp, id, &root);
		while (error == 0 && ino != BTRFS_FIRST_FREE_OBJECTID) {
			if (++steps > BTRFS_CTL_PATH_MAX)
				return (ELOOP);
			error = btrfs_find_dir_parent(root, ino, &parent);
			if (error != 0)
				break;
			memset(&name, 0, sizeof(name));
			name.ino = ino;
			error = btrfs_iterate_directory(root, parent, 2,
			    subvol_find_name, &name);
			if (error == 0 && name.entry.ino == 0)
				error = EINVAL;
			if (error == 0)
				error = subvol_prepend(out, len, name.entry.name,
				    strlen(name.entry.name));
			ino = parent;
		}
	}
	btrfs_release_path(&path);
	return (error);
}

static int
subvol_list(struct btrfs_fs *bmp, struct btrfs_ioctl_subvolume *args)
{
	struct btrfs_root *roots;
	struct btrfs_path path = { 0 };
	struct btrfs_key target = { 0 };
	const struct btrfs_key *key;
	const struct btrfs_root_item *item;
	const uint8_t *data;
	uint64_t dirid;
	uint32_t size;
	int error;

	if (args->cursor >= BTRFS_LAST_FREE_OBJECTID)
		return (ENOENT);
	error = btrfs_get_root(bmp, BTRFS_ROOT_TREE_OBJECTID, &roots);
	if (error != 0)
		return (error);
	target.objectid = htole64(MAX(args->cursor + 1,
	    BTRFS_FIRST_FREE_OBJECTID));
	error = btrfs_search_lower_bound(roots, &target, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &key, &data, &size);
		if (error != 0)
			break;
		if (letoh64(key->objectid) > BTRFS_LAST_FREE_OBJECTID) {
			error = ENOENT;
			break;
		}
		if (key->type == BTRFS_ROOT_ITEM_KEY) {
			if (size < offsetof(struct btrfs_root_item, generation_v2)) {
				error = EINVAL;
				break;
			}
			item = (const struct btrfs_root_item *)data;
			if (letoh32(item->refs) != 0) {
				args->id = letoh64(key->objectid);
				args->flags = letoh64(item->flags) &
				    BTRFS_ROOT_SUBVOL_RDONLY ? BTRFS_CTL_RDONLY : 0;
				break;
			}
		}
		error = btrfs_next_item(&path);
	}
	btrfs_release_path(&path);
	if (error == 0)
		error = btrfs_find_subvol_parent(bmp, args->id,
		    &args->parent, &dirid);
	if (error == 0)
		error = btrfs_subvol_path(bmp, args->id, args->path, sizeof(args->path));
	if (error == 0)
		args->cursor = args->id;
	return (error);
}

static int
subvol_uuid(struct btrfs_trans_handle *handle, struct btrfs_root_item *item,
    uint64_t id, int remove)
{
	struct btrfs_root *root;
	struct btrfs_key key = { 0 };
	uint64_t value = htole64(id);
	uint64_t *values;
	uint32_t nodesize, size, i;
	int error, which;

	error = btrfs_get_root(handle->bth_transaction->bt_mount,
	    BTRFS_UUID_TREE_OBJECTID, &root);
	/* The UUID index is optional on filesystems created without one. */
	if (error == ENOENT)
		return (0);
	if (error != 0)
		return (error);
	memcpy(&key.objectid, item->uuid, 8);
	memcpy(&key.offset, item->uuid + 8, 8);
	key.type = BTRFS_UUID_KEY_SUBVOL;
	if (!remove)
		return (btrfs_insert_item(handle, root, &key, &value, sizeof(value)));
	nodesize = letoh32(root->br_super->nodesize);
	values = malloc(nodesize, M_BTRFS, M_WAITOK);
	for (which = 0; which < 2; which++) {
		if (which != 0) {
			memcpy(&key.objectid, item->received_uuid, 8);
			memcpy(&key.offset, item->received_uuid + 8, 8);
			key.type = BTRFS_UUID_KEY_RECEIVED_SUBVOL;
			if (key.objectid == 0 && key.offset == 0)
				break;
		}
		error = subvol_read(root, &key, values, nodesize, &size);
		if (error == ENOENT) {
			error = 0;
			continue;
		}
		if (error != 0)
			break;
		if (size == 0 || size % sizeof(value) != 0) {
			error = EINVAL;
			break;
		}
		for (i = 0; i < size / sizeof(value); i++)
			if (values[i] == value)
				break;
		if (i == size / sizeof(value)) {
			error = EINVAL;
			break;
		}
		size -= sizeof(value);
		memmove(values + i, values + i + 1, size - i * sizeof(value));
		if (size == 0)
			error = btrfs_delete_item(handle, root, &key);
		else
			error = btrfs_replace_item(handle, root, &key, values, size);
		if (error != 0)
			break;
	}
	free(values, M_BTRFS, nodesize);
	return (error);
}

int
btrfs_subvolume(struct btrfs_fs *bmp, u_long cmd,
    struct btrfs_ioctl_subvolume *args, struct proc *p)
{
	struct btrfs_root *roots, *dir, *source = NULL;
	struct btrfs_root_item item, source_item;
	struct btrfs_inode_item inode;
	struct btrfs_trans_reservation reservation = { 0 };
	struct btrfs_trans_handle *handle = NULL;
	struct btrfs_path path = { 0 };
	struct btrfs_key ikey = { 0 }, hkey = { 0 }, dkey = { 0 };
	struct btrfs_key rkey = { 0 }, backkey = { 0 }, refkey = { 0 };
	const struct btrfs_key *key;
	struct btrfs_dir_item *di;
	struct btrfs_root_ref *rr;
	struct btrfs_node *node;
	struct subvol_entry entry;
	struct vnode *vp = NULL;
	struct timespec now;
	char *parentpath, *name;
	uint8_t *bucket = NULL;
	uint8_t *record, *reference;
	uint64_t ino, sourceino, id, index = 2, blocks = 1, refs = 0, gen;
	uint32_t nodesize = letoh32(bmp->bm_super.nodesize);
	uint32_t size, bucketsize = 0, recordsize, pos, step;
	size_t namelen;
	int error, enderror, remove = cmd == BTRFSIOC_DELETE;
	int gated = 0, mutated = 0;
	int deleting = 0;

	if (cmd == BTRFSIOC_LIST)
		return (subvol_list(bmp, args));
	record = malloc(sizeof(*di) + BTRFS_NAME_MAX, M_BTRFS, M_WAITOK);
	reference = malloc(sizeof(*rr) + BTRFS_NAME_MAX, M_BTRFS, M_WAITOK);
	parentpath = malloc(BTRFS_CTL_PATH_MAX, M_BTRFS, M_WAITOK);
	strlcpy(parentpath, args->path, BTRFS_CTL_PATH_MAX);
	name = strrchr(parentpath, '/');
	if (name != NULL)
		*name++ = '\0';
	else {
		name = parentpath;
	}
	namelen = strlen(name);
	if (namelen == 0 || namelen > BTRFS_NAME_MAX ||
	    strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		error = EINVAL;
		goto done;
	}
	error = subvol_resolve(bmp, name == parentpath ? "" : parentpath,
	    &dir, &ino);
	if (error != 0)
		goto done;
	if (dir->br_flags & BTRFS_ROOT_SUBVOL_RDONLY) {
		error = EROFS;
		goto done;
	}
	error = btrfs_control_parent(bmp, dir->br_owner, ino, &vp);
	if (error != 0)
		goto done;
	rw_enter_write(&bmp->bm_namespace_lock);
	/*
	 * Closing joins after locking the parent avoids waiting for a vnode
	 * held by a writer asleep at the gate. Existing handles finish before
	 * the source generation is committed and shared.
	 */
	mtx_enter(&bmp->bm_trans_mtx);
	bmp->bm_control = p;
	while (bmp->bm_transaction->bt_writers != 0)
		msleep(&bmp->bm_transaction->bt_writers, &bmp->bm_trans_mtx,
		    PWAIT, "btrsnap", 0);
	mtx_leave(&bmp->bm_trans_mtx);
	gated = 1;
	error = btrfs_commit_current(bmp, p);
	if (error != 0)
		goto unlock;
	error = btrfs_get_root(bmp, BTRFS_ROOT_TREE_OBJECTID, &roots);
	if (error != 0)
		goto unlock;
	memset(&entry, 0, sizeof(entry));
	error = btrfs_lookup_directory(dir, ino, name, namelen,
	    subvol_entry, &entry);
	if (error != 0 && error != ENOENT)
		goto unlock;
	if (remove) {
		if (entry.ino == 0) {
			error = ENOENT;
			goto unlock;
		}
		if (!entry.subvol) {
			error = EINVAL;
			goto unlock;
		}
		id = entry.ino;
		bmp->bm_last_rootid = MAX(bmp->bm_last_rootid, id);
		error = btrfs_get_root(bmp, id, &source);
		if (error == 0) {
			mtx_enter(&bmp->bm_nodemtx);
			source->br_deleted = 1;
			deleting = 1;
			mtx_leave(&bmp->bm_nodemtx);
			error = btrfs_control_busy(bmp, id);
		}
		if (error != 0)
			goto unlock;
		/* A nested subvolume must be removed explicitly first. */
		rkey.objectid = htole64(id);
		rkey.type = BTRFS_ROOT_REF_KEY;
		error = btrfs_search_lower_bound(roots, &rkey, &path);
		if (error == 0) {
			error = btrfs_path_item(&path, &key, NULL, NULL);
			if (error == 0 && key->objectid == rkey.objectid &&
			    key->type == rkey.type)
				error = ENOTEMPTY;
		}
		btrfs_release_path(&path);
		if (error != 0 && error != ENOENT)
			goto unlock;
		error = btrfs_find_root_item(roots, id,
		    BTRFS_FIRST_FREE_OBJECTID, &item, NULL);
		if (error == 0)
			error = btrfs_count_tree(source, &blocks, &refs);
		if (error != 0)
			goto unlock;
		backkey.objectid = htole64(id);
		backkey.type = BTRFS_ROOT_BACKREF_KEY;
		backkey.offset = htole64(dir->br_owner);
		error = subvol_read(roots, &backkey, reference,
		    sizeof(*rr) + BTRFS_NAME_MAX,
		    &size);
		if (error != 0)
			goto unlock;
		rr = (struct btrfs_root_ref *)reference;
		if (size != sizeof(*rr) + namelen ||
		    letoh64(rr->dirid) != ino ||
		    letoh16(rr->name_len) != namelen ||
		    memcmp(rr + 1, name, namelen) != 0) {
			error = EINVAL;
			goto unlock;
		}
		index = letoh64(rr->sequence);
	} else {
		if (entry.ino != 0) {
			error = EEXIST;
			goto unlock;
		}
		memset(&item, 0, sizeof(item));
		if (cmd == BTRFSIOC_SNAPSHOT) {
			error = subvol_resolve(bmp, args->source, &source, &sourceino);
			if (error == 0 && sourceino != BTRFS_FIRST_FREE_OBJECTID)
				error = EINVAL;
			if (error == 0)
				error = btrfs_find_root_item(roots, source->br_owner,
				    BTRFS_FIRST_FREE_OBJECTID, &source_item, NULL);
			if (error != 0)
				goto unlock;
			item = source_item;
			memcpy(item.parent_uuid, source_item.uuid, BTRFS_UUID_SIZE);
		} else {
			error = btrfs_get_root(bmp, BTRFS_FS_TREE_OBJECTID, &source);
			if (error != 0)
				goto unlock;
		}
		rkey.objectid = htole64(BTRFS_LAST_FREE_OBJECTID);
		rkey.type = 0xff;
		rkey.offset = htole64(UINT64_MAX);
		error = btrfs_search_predecessor(roots, &rkey, &path);
		if (error == 0)
			error = btrfs_path_item(&path, &key, NULL, NULL);
		if (error != 0)
			goto unlock;
		id = MAX(letoh64(key->objectid) + 1, BTRFS_FIRST_FREE_OBJECTID);
		id = MAX(id, bmp->bm_last_rootid + 1);
		btrfs_release_path(&path);
		if (id > BTRFS_LAST_FREE_OBJECTID) {
			error = ENOSPC;
			goto unlock;
		}
		bmp->bm_last_rootid = id;
		dkey.objectid = htole64(ino);
		dkey.type = BTRFS_DIR_INDEX_KEY;
		dkey.offset = htole64(UINT64_MAX);
		error = btrfs_search_predecessor(dir, &dkey, &path);
		if (error == 0) {
			error = btrfs_path_item(&path, &key, NULL, NULL);
			if (error == 0 && key->objectid == dkey.objectid &&
			    key->type == dkey.type)
				index = letoh64(key->offset) + 1;
		}
		btrfs_release_path(&path);
		if (error != 0 && error != ENOENT)
			goto unlock;
		if (index < 2 || index >= INT64_MAX) {
			error = EOVERFLOW;
			goto unlock;
		}
	}
	ikey.objectid = htole64(ino);
	ikey.type = BTRFS_INODE_ITEM_KEY;
	error = subvol_read(dir, &ikey, &inode, sizeof(inode), &size);
	if (error != 0)
		goto unlock;
	if (size != sizeof(inode) || letoh32(inode.nlink) == 0) {
		error = EINVAL;
		goto unlock;
	}
	if (letoh64(inode.flags) &
	    (BTRFS_INODE_IMMUTABLE | BTRFS_INODE_APPEND)) {
		error = EPERM;
		goto unlock;
	}
	if (cmd == BTRFSIOC_CREATE) {
		struct btrfs_key xkey = ikey;
		xkey.type = BTRFS_XATTR_ITEM_KEY;
		error = btrfs_search_lower_bound(dir, &xkey, &path);
		if (error == 0) {
			error = btrfs_path_item(&path, &key, NULL, NULL);
			if (error == 0 && key->objectid == xkey.objectid &&
			    key->type == xkey.type)
				error = EOPNOTSUPP;
		}
		btrfs_release_path(&path);
		if (error != 0 && error != ENOENT)
			goto unlock;
	}
	if ((remove && letoh64(inode.size) < namelen * 2) ||
	    (!remove && letoh64(inode.size) > UINT64_MAX - namelen * 2)) {
		error = EOVERFLOW;
		goto unlock;
	}
	bucket = malloc(nodesize, M_BTRFS, M_WAITOK | M_ZERO);
	hkey.objectid = htole64(ino);
	hkey.type = BTRFS_DIR_ITEM_KEY;
	hkey.offset = htole64(crc32c(1, (const uint8_t *)name, namelen) ^
	    0xffffffffU);
	error = subvol_read(dir, &hkey, bucket, nodesize, &bucketsize);
	if (error != 0 && error != ENOENT)
		goto unlock;
	recordsize = sizeof(*di) + namelen;
	if (remove) {
		for (pos = 0; pos < bucketsize; pos += step) {
			di = (struct btrfs_dir_item *)(bucket + pos);
			if (bucketsize - pos < sizeof(*di)) {
				error = EINVAL;
				goto unlock;
			}
			step = sizeof(*di) + letoh16(di->name_len) +
			    letoh16(di->data_len);
			if (step > bucketsize - pos) {
				error = EINVAL;
				goto unlock;
			}
			if (letoh16(di->name_len) == namelen &&
			    memcmp(di + 1, name, namelen) == 0)
				break;
		}
		if (pos == bucketsize || step != recordsize) {
			error = EINVAL;
			goto unlock;
		}
		memmove(bucket + pos, bucket + pos + step,
		    bucketsize - pos - step);
		bucketsize -= step;
	} else {
		if (bucketsize + recordsize > nodesize -
		    sizeof(struct btrfs_header) - sizeof(struct btrfs_item)) {
			error = ENOSPC;
			goto unlock;
		}
		memset(record, 0, recordsize);
		di = (struct btrfs_dir_item *)record;
		di->location.objectid = htole64(id);
		di->location.type = BTRFS_ROOT_ITEM_KEY;
		di->location.offset = htole64(UINT64_MAX);
		di->name_len = htole16(namelen);
		di->type = BTRFS_FT_DIR;
	}
	/* Whole deletion is atomic; reserve before changing any reachable item. */
	if (blocks > UINT64_MAX / nodesize / 32 ||
	    refs > UINT64_MAX / nodesize / 32) {
		error = EOVERFLOW;
		goto unlock;
	}
	reservation.btr_metadata = (256 + blocks * 8 + refs * 16) * nodesize;
	error = btrfs_trans_join(bmp, &reservation, &handle);
	if (error != 0)
		goto unlock;
	gen = handle->bth_transaction->bt_generation;
	nanotime(&now);
	if (!remove) {
		if (cmd == BTRFSIOC_CREATE) {
			item.inode.generation = htole64(gen);
			item.inode.transid = htole64(gen);
			item.inode.mode = htole32(S_IFDIR |
			    (0777 & ~p->p_fd->fd_cmask) |
			    (letoh32(inode.mode) & S_ISGID));
			item.inode.nlink = htole32(1);
			item.inode.uid = htole32(p->p_ucred->cr_uid);
			item.inode.gid = inode.gid;
			item.inode.flags = htole64(letoh64(inode.flags) &
			    (BTRFS_INODE_NODATACOW | BTRFS_INODE_COMPRESS |
			    BTRFS_INODE_NOCOMPRESS));
			item.inode.atime.sec = item.inode.mtime.sec =
			    item.inode.ctime.sec = item.inode.otime.sec =
			    htole64(now.tv_sec);
			item.inode.atime.nsec = item.inode.mtime.nsec =
			    item.inode.ctime.nsec = item.inode.otime.nsec =
			    htole32(now.tv_nsec);
		}
		arc4random_buf(item.uuid, sizeof(item.uuid));
		item.uuid[6] = (item.uuid[6] & 0x0f) | 0x40;
		item.uuid[8] = (item.uuid[8] & 0x3f) | 0x80;
		memset(item.received_uuid, 0, sizeof(item.received_uuid));
		memset(&item.drop_progress, 0, sizeof(item.drop_progress));
		item.drop_level = 0;
		item.root_dirid = htole64(BTRFS_FIRST_FREE_OBJECTID);
		item.refs = htole32(1);
		item.flags = htole64(args->flags & BTRFS_CTL_RDONLY ?
		    BTRFS_ROOT_SUBVOL_RDONLY : 0);
		item.ctransid = item.otransid = htole64(gen);
		item.last_snapshot = cmd == BTRFSIOC_SNAPSHOT ? htole64(gen) : 0;
		item.stransid = item.rtransid = 0;
		item.ctime.sec = item.otime.sec = htole64(now.tv_sec);
		item.ctime.nsec = item.otime.nsec = htole32(now.tv_nsec);
	}
	mutated = 1;
	if (remove)
		error = btrfs_drop_subvolume_tree(handle, source);
	else
		error = btrfs_new_subvolume_root(handle, source, id, &item,
		    cmd == BTRFSIOC_SNAPSHOT);
	/*
	 * Linux uses this marker in the embedded root-item inode to distinguish
	 * initialized root flags from legacy garbage. Without it Linux clears
	 * the read-only flag when opening the subvolume. Set it after building
	 * a new file tree so the marker is not copied into its directory inode.
	 */
	if (!remove)
		item.inode.flags = htole64(letoh64(item.inode.flags) |
		    BTRFS_INODE_ROOT_ITEM_INIT);
	rkey.objectid = htole64(id);
	rkey.type = BTRFS_ROOT_ITEM_KEY;
	rkey.offset = remove ? htole64(source->br_root_offset) : 0;
	if (error == 0 && remove)
		error = btrfs_delete_item(handle, roots, &rkey);
	else if (error == 0)
		error = btrfs_insert_item(handle, roots, &rkey, &item, sizeof(item));
	if (error == 0)
		error = subvol_uuid(handle, &item, id, remove);
	if (error == 0 && !remove && cmd == BTRFSIOC_SNAPSHOT) {
		struct btrfs_key skey = { 0 };
		source_item.last_snapshot = htole64(gen);
		skey.objectid = htole64(source->br_owner);
		skey.type = BTRFS_ROOT_ITEM_KEY;
		skey.offset = htole64(source->br_root_offset);
		error = btrfs_replace_item(handle, roots, &skey,
		    &source_item, sizeof(source_item));
	}
	if (!remove) {
		di = (struct btrfs_dir_item *)record;
		di->transid = htole64(gen);
		memcpy(di + 1, name, namelen);
		memcpy(bucket + bucketsize, record, recordsize);
		bucketsize += recordsize;
		memset(reference, 0, sizeof(*rr) + namelen);
		rr = (struct btrfs_root_ref *)reference;
		rr->dirid = htole64(ino);
		rr->sequence = htole64(index);
		rr->name_len = htole16(namelen);
		memcpy(rr + 1, name, namelen);
	}
	backkey.objectid = htole64(id);
	backkey.type = BTRFS_ROOT_BACKREF_KEY;
	backkey.offset = htole64(dir->br_owner);
	refkey.objectid = backkey.offset;
	refkey.type = BTRFS_ROOT_REF_KEY;
	refkey.offset = backkey.objectid;
	dkey.objectid = htole64(ino);
	dkey.type = BTRFS_DIR_INDEX_KEY;
	dkey.offset = htole64(index);
	if (error == 0 && remove)
		error = btrfs_delete_item(handle, roots, &backkey);
	else if (error == 0)
		error = btrfs_insert_item(handle, roots, &backkey, reference,
		    sizeof(*rr) + namelen);
	if (error == 0 && remove)
		error = btrfs_delete_item(handle, roots, &refkey);
	else if (error == 0)
		error = btrfs_insert_item(handle, roots, &refkey, reference,
		    sizeof(*rr) + namelen);
	if (error == 0 && remove)
		error = btrfs_delete_item(handle, dir, &dkey);
	else if (error == 0)
		error = btrfs_insert_item(handle, dir, &dkey, record, recordsize);
	if (error == 0) {
		if (bucketsize == 0)
			error = btrfs_delete_item(handle, dir, &hkey);
		else if (!remove && bucketsize == recordsize)
			error = btrfs_insert_item(handle, dir, &hkey, bucket, bucketsize);
		else
			error = btrfs_replace_item(handle, dir, &hkey, bucket, bucketsize);
	}
	inode.size = htole64(letoh64(inode.size) +
	    (remove ? -(int64_t)(namelen * 2) : namelen * 2));
	inode.transid = htole64(gen);
	inode.sequence = htole64(letoh64(inode.sequence) + 1);
	inode.ctime.sec = inode.mtime.sec = htole64(now.tv_sec);
	inode.ctime.nsec = inode.mtime.nsec = htole32(now.tv_nsec);
	if (error == 0)
		error = btrfs_replace_item(handle, dir, &ikey, &inode, sizeof(inode));
	if (error != 0)
		btrfs_trans_abort(handle, error);
	enderror = btrfs_trans_end(handle);
	handle = NULL;
	if (error == 0)
		error = enderror;
	if (error == 0)
		error = btrfs_commit_current(bmp, p);
	if (error == 0 && vp != NULL) {
		node = VTOBTRFS(vp);
		error = btrfs_find_inode(dir, ino, &node->bn_inode);
		cache_purge(vp);
		VN_KNOTE(vp, NOTE_WRITE);
	}
	if (error == 0 && remove)
		btrfs_forget_root(bmp, id);
unlock:
	if (deleting && error != 0) {
		mtx_enter(&bmp->bm_nodemtx);
		source->br_deleted = 0;
		mtx_leave(&bmp->bm_nodemtx);
	}
	btrfs_release_path(&path);
	if (handle != NULL) {
		if (mutated)
			btrfs_trans_abort(handle, error);
		btrfs_trans_end(handle);
	}
	if (gated) {
		mtx_enter(&bmp->bm_trans_mtx);
		bmp->bm_control = NULL;
		wakeup(&bmp->bm_control);
		mtx_leave(&bmp->bm_trans_mtx);
	}
	rw_exit_write(&bmp->bm_namespace_lock);
done:
	if (vp != NULL)
		vput(vp);
	free(bucket, M_BTRFS, nodesize);
	free(parentpath, M_BTRFS, BTRFS_CTL_PATH_MAX);
	free(record, M_BTRFS, sizeof(*di) + BTRFS_NAME_MAX);
	free(reference, M_BTRFS, sizeof(*rr) + BTRFS_NAME_MAX);
	return (error);
}
