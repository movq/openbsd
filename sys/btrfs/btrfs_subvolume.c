/* Public domain. */

/*
 * Privileged /dev/btrfs-control administration selects a filesystem by
 * mountpoint, but resolves all paths from tree 5, including parents outside
 * the selected view. These paths do not follow symlinks or "..".
 *
 * Administration pins views and serializes ancestry, locks a visible parent
 * before the namespace lock, then closes joins, drains handles and commits
 * the source generation. Private creation/snapshot and deletion plans reserve
 * all metadata and reference work before mutation. Only a successful commit
 * permits parent inode and name-cache publication, before reopening joins.
 * Snapshots copy the root block and share lower metadata and file data; nested
 * subvolumes become empty, immutable boundary directories.
 *
 * Deletion rejects mounted hierarchies, active vnodes and nested subvolumes.
 * Namespace removal and final reference drops publish atomically, so large
 * deletions can fail ENOSPC before mutation. Bounded deletion and recovery
 * remain future work. Tombstones and the root-ID high-water mark prevent reuse
 * until filesystem teardown.
 */

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
#include <btrfs/btrfs_dir.h>

struct subvol_entry {
	uint64_t ino;
	uint64_t index;
	int subvol;
	size_t namelen;
	char name[BTRFS_NAME_MAX + 1];
};

static int
subvol_entry(const struct btrfs_dir_entry *entry, void *arg)
{
	struct subvol_entry *out = arg;

	out->ino = entry->bde_objectid;
	out->index = entry->bde_index;
	out->subvol = entry->bde_subvolume;
	out->namelen = entry->bde_namelen;
	memcpy(out->name, entry->bde_name, entry->bde_namelen);
	out->name[entry->bde_namelen] = '\0';
	return (0);
}

/* lookup_directory visits every entry in the name's hash bucket. */
static int
subvol_lookup_entry(const struct btrfs_dir_entry *entry, void *arg)
{
	struct subvol_entry *out = arg;

	if (entry->bde_namelen != out->namelen ||
	    memcmp(entry->bde_name, out->name, out->namelen) != 0)
		return (0);
	return (subvol_entry(entry, out));
}

static int
subvol_lookup(struct btrfs_root *root, uint64_t ino, const char *name,
    size_t namelen, struct subvol_entry *entry)
{
	if (namelen > BTRFS_NAME_MAX)
		return (ENAMETOOLONG);
	memset(entry, 0, sizeof(*entry));
	entry->namelen = namelen;
	memcpy(entry->name, name, namelen);
	return (btrfs_lookup_directory(root, ino, name, namelen,
	    subvol_lookup_entry, entry));
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
		error = subvol_lookup(root, *ino, path, len, &entry);
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
 * The mount administration lock and bm_rename_lock must be held exclusively.
 * Lock any parent vnode, then bm_namespace_lock if needed, before entering:
 * a writer waiting at this gate may hold a vnode lock. No transaction handle
 * may be held. The caller must leave the gate after ending its own handles.
 */
static void
subvol_admin_enter(struct btrfs_fs *bmp, struct proc *p)
{
	rw_assert_wrlock(&bmp->bm_rename_lock);
	mtx_enter(&bmp->bm_trans_mtx);
	KASSERT(bmp->bm_control == NULL);
	bmp->bm_control = p;
	while (bmp->bm_transaction->bt_writers != 0)
		msleep(&bmp->bm_transaction->bt_writers, &bmp->bm_trans_mtx,
		    PWAIT, "btradmin", 0);
	mtx_leave(&bmp->bm_trans_mtx);
}

static void
subvol_admin_leave(struct btrfs_fs *bmp, struct proc *p)
{
	rw_assert_wrlock(&bmp->bm_rename_lock);
	mtx_enter(&bmp->bm_trans_mtx);
	KASSERT(bmp->bm_control == p);
	bmp->bm_control = NULL;
	wakeup(&bmp->bm_control);
	mtx_leave(&bmp->bm_trans_mtx);
}

struct subvol_uuid_match {
	const uint8_t *uuid;
	uint64_t transid;
	uint64_t id[2];		/* native identity, received identity */
	int multiple[2];
};

static void
subvol_uuid_match(struct subvol_uuid_match *match, uint64_t id,
    const struct btrfs_root_item *item)
{
	int which;

	if (id < BTRFS_FIRST_FREE_OBJECTID || id > BTRFS_LAST_FREE_OBJECTID ||
	    letoh32(item->refs) == 0 ||
	    !(letoh64(item->flags) & BTRFS_ROOT_SUBVOL_RDONLY))
		return;
	for (which = 0; which < 2; which++) {
		if (memcmp(match->uuid, which ? item->received_uuid : item->uuid,
		    BTRFS_UUID_SIZE) != 0 ||
		    match->transid != letoh64(which ? item->stransid :
		    item->ctransid))
			continue;
		if (match->id[which] == 0)
			match->id[which] = id;
		else if (match->id[which] != id)
			match->multiple[which] = 1;
	}
}

/* No path reconstruction or filesystem-root reads for unrelated subvolumes. */
static int
subvol_uuid_scan(struct btrfs_root *roots, struct subvol_uuid_match *match)
{
	struct btrfs_path path = { 0 };
	struct btrfs_key target = { 0 };
	struct btrfs_root_item item;
	const struct btrfs_key *key;
	const uint8_t *data;
	uint32_t size;
	int error;

	target.objectid = htole64(BTRFS_FIRST_FREE_OBJECTID);
	error = btrfs_search_lower_bound(roots, &target, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &key, &data, &size);
		if (error != 0)
			break;
		if (letoh64(key->objectid) > BTRFS_LAST_FREE_OBJECTID)
			break;
		if (key->type == BTRFS_ROOT_ITEM_KEY) {
			if (size < offsetof(struct btrfs_root_item, generation_v2)) {
				error = EINVAL;
				break;
			}
			memset(&item, 0, sizeof(item));
			memcpy(&item, data, MIN(size, sizeof(item)));
			subvol_uuid_match(match, letoh64(key->objectid), &item);
		}
		error = btrfs_next_item(&path);
	}
	btrfs_release_path(&path);
	return (error == ENOENT ? 0 : error);
}

static int
subvol_uuid_index(struct btrfs_root *roots, struct btrfs_root *uuids,
    struct subvol_uuid_match *match)
{
	struct btrfs_key key = { 0 };
	struct btrfs_root_item item;
	uint64_t id, *values;
	uint32_t size, i, nodesize = letoh32(uuids->br_super->nodesize);
	int error = 0, which;

	memcpy(&key.objectid, match->uuid, 8);
	memcpy(&key.offset, match->uuid + 8, 8);
	values = malloc(nodesize, M_BTRFS, M_WAITOK);
	for (which = 0; which < 2; which++) {
		key.type = which ? BTRFS_UUID_KEY_RECEIVED_SUBVOL :
		    BTRFS_UUID_KEY_SUBVOL;
		error = subvol_read(uuids, &key, values, nodesize, &size);
		if (error == ENOENT) {
			error = 0;
			continue;
		}
		if (error != 0)
			break;
		if (size == 0 || size % sizeof(*values) != 0) {
			error = EINVAL;
			break;
		}
		for (i = 0; i < size / sizeof(*values); i++) {
			id = letoh64(values[i]);
			if (id < BTRFS_FIRST_FREE_OBJECTID ||
			    id > BTRFS_LAST_FREE_OBJECTID)
				continue;
			error = btrfs_find_root_item(roots, id,
			    BTRFS_FIRST_FREE_OBJECTID, &item, NULL);
			if (error == ENOENT) {
				error = 0;
				continue;
			}
			if (error != 0)
				break;
			subvol_uuid_match(match, id, &item);
		}
		if (error != 0)
			break;
	}
	free(values, M_BTRFS, nodesize);
	return (error);
}

/*
 * The administration locks keep root identities, index entries and namespace
 * paths together throughout selection and the subsequent INFO operation.
 * An old generation can mean missing entries, so even an index hit would
 * not suffice to establish uniqueness: scan all root items in that case.
 */
static int
subvol_find_uuid(struct btrfs_fs *bmp, struct btrfs_ioctl_identity *args)
{
	struct subvol_uuid_match match = {
	    .uuid = args->uuid, .transid = args->stransid
	};
	struct btrfs_root *roots, *uuids = NULL;
	int error, which;

	if (memcmp(args->uuid, (uint8_t[16]){0}, 16) == 0)
		return (ENOENT);
	error = btrfs_get_root(bmp, BTRFS_ROOT_TREE_OBJECTID, &roots);
	if (error != 0)
		return (error);
	if (bmp->bm_super.uuid_tree_generation == bmp->bm_super.generation) {
		error = btrfs_get_root(bmp, BTRFS_UUID_TREE_OBJECTID, &uuids);
		if (error != 0 && error != ENOENT)
			return (error);
	}
	if (uuids != NULL)
		error = subvol_uuid_index(roots, uuids, &match);
	else
		error = subvol_uuid_scan(roots, &match);
	if (error != 0)
		return (error);
	which = match.id[1] != 0;
	if (match.id[which] == 0)
		return (ENOENT);
	if (match.multiple[which])
		return (EEXIST);
	args->id = match.id[which];
	return (0);
}

/*
 * The caller serializes mounts and namespace changes. Finalization fences
 * vget, proves that no vnode user can still mutate this tree, then publishes
 * the identity and read-only flag in one transaction.
 * The shared administration gate excludes writers. Receive completion
 * atomically publishes the received UUID, sender transaction ID and read-only
 * flag; failed or interrupted replay leaves an incomplete writable tree with
 * no received identity. The destination must stay private during replay:
 * finalization requires no active vnodes or mounted descendant views.
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

	if (cmd == BTRFSIOC_FIND_UUID) {
		error = subvol_find_uuid(bmp, args);
		if (error != 0)
			return (error);
		cmd = BTRFSIOC_INFO;
	}
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
	subvol_admin_enter(bmp, p);
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
	if (gated)
		subvol_admin_leave(bmp, p);
	return (error);
}

struct subvol_name {
	uint64_t ino;
	struct subvol_entry entry;
};

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

struct subvol_parent {
	struct btrfs_root *dir;
	uint64_t ino;
	size_t namelen;
	char name[BTRFS_NAME_MAX + 1];
};

/* Private copies of the parent items, with the planned size and hash edits. */
struct subvol_namespace {
	struct btrfs_inode_item inode;
	struct btrfs_key ikey, backkey, refkey;
	struct btrfs_name_plan *plan;
};

struct subvol_create {
	struct btrfs_root *empty_template;
	struct btrfs_root *snapshot;
	struct btrfs_root_item item, snapshot_item;
	uint8_t reference[sizeof(struct btrfs_root_ref) + BTRFS_NAME_MAX];
};

struct subvol_delete {
	/* Non-NULL owns temporary br_deleted exclusion until delete_finish. */
	struct btrfs_root *victim;
	struct btrfs_root_item item;
};

/*
 * Preparation runs behind the administration gate after preceding work is
 * committed. It changes no reachable tree items: only private item copies,
 * the root-ID high-water mark, and deletion's temporary vnode exclusion.
 * The plan starts zeroed; success describes the complete reservation and
 * namespace edit.
 * Apply functions require that reservation in a joined handle; the lifecycle
 * must abort on ANY apply error, end the handle, and commit before publishing.
 */
struct subvol_operation {
	struct subvol_parent parent;
	struct subvol_namespace ns;
	struct btrfs_root *roots;
	uint64_t id;
	struct btrfs_key rkey;
	struct btrfs_trans_reservation reservation;
	union {
		struct subvol_create create;
		struct subvol_delete delete;
	} u;
};

static int
subvol_parent_resolve(struct btrfs_fs *bmp, const char *path,
    struct subvol_parent *parent)
{
	char *parentpath, *name;
	int error;

	parentpath = malloc(BTRFS_CTL_PATH_MAX, M_BTRFS, M_WAITOK);
	strlcpy(parentpath, path, BTRFS_CTL_PATH_MAX);
	name = strrchr(parentpath, '/');
	if (name != NULL)
		*name++ = '\0';
	else
		name = parentpath;
	parent->namelen = strlen(name);
	if (parent->namelen == 0 || parent->namelen > BTRFS_NAME_MAX ||
	    strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		error = EINVAL;
		goto out;
	}
	memcpy(parent->name, name, parent->namelen + 1);
	error = subvol_resolve(bmp, name == parentpath ? "" : parentpath,
	    &parent->dir, &parent->ino);
	if (error == 0 && parent->dir->br_flags & BTRFS_ROOT_SUBVOL_RDONLY)
		error = EROFS;
out:
	free(parentpath, M_BTRFS, BTRFS_CTL_PATH_MAX);
	return (error);
}

static int
subvol_namespace_prepare(struct subvol_operation *op)
{
	struct subvol_parent *parent = &op->parent;
	struct subvol_namespace *ns = &op->ns;
	uint32_t size;
	int error;

	ns->ikey.objectid = htole64(parent->ino);
	ns->ikey.type = BTRFS_INODE_ITEM_KEY;
	error = subvol_read(parent->dir, &ns->ikey, &ns->inode,
	    sizeof(ns->inode), &size);
	if (error != 0)
		return (error);
	if (size != sizeof(ns->inode) || letoh32(ns->inode.nlink) == 0)
		return (EINVAL);
	if (letoh64(ns->inode.flags) &
	    (BTRFS_INODE_IMMUTABLE | BTRFS_INODE_APPEND))
		return (EPERM);
	ns->backkey.objectid = htole64(op->id);
	ns->backkey.type = BTRFS_ROOT_BACKREF_KEY;
	ns->backkey.offset = htole64(parent->dir->br_owner);
	ns->refkey.objectid = ns->backkey.offset;
	ns->refkey.type = BTRFS_ROOT_REF_KEY;
	ns->refkey.offset = ns->backkey.objectid;
	ns->plan = btrfs_name_plan_alloc(parent->dir);
	return (0);
}

/* Root identity and directory index allocation leave no tree paths held. */
static int
subvol_create_identity(struct btrfs_fs *bmp, struct subvol_operation *op,
    uint64_t *index)
{
	struct btrfs_path path = { 0 };
	struct btrfs_key target = { 0 };
	const struct btrfs_key *key;
	int error;

	target.objectid = htole64(BTRFS_LAST_FREE_OBJECTID);
	target.type = 0xff;
	target.offset = htole64(UINT64_MAX);
	error = btrfs_search_predecessor(op->roots, &target, &path);
	if (error == 0)
		error = btrfs_path_item(&path, &key, NULL, NULL);
	if (error == 0) {
		op->id = MAX(letoh64(key->objectid) + 1,
		    BTRFS_FIRST_FREE_OBJECTID);
		op->id = MAX(op->id, bmp->bm_last_rootid + 1);
	}
	btrfs_release_path(&path);
	if (error != 0)
		return (error);
	if (op->id > BTRFS_LAST_FREE_OBJECTID)
		return (ENOSPC);
	bmp->bm_last_rootid = op->id;
	op->rkey.objectid = htole64(op->id);
	op->rkey.type = BTRFS_ROOT_ITEM_KEY;

	return (btrfs_next_dir_index(op->parent.dir, op->parent.ino, index));
}

static int
subvol_create_prepare(struct btrfs_fs *bmp, struct subvol_operation *op,
    u_long cmd, const struct btrfs_ioctl_subvolume *args, struct proc *p)
{
	struct subvol_create *create = &op->u.create;
	struct subvol_parent *parent = &op->parent;
	struct subvol_namespace *ns = &op->ns;
	struct btrfs_root_item *item = &create->item;
	struct btrfs_dir_item record = { 0 };
	struct btrfs_root_ref *rr = (void *)create->reference;
	struct subvol_entry entry;
	uint64_t sourceino, index;
	uint32_t nodesize = letoh32(bmp->bm_super.nodesize);
	int error;

	error = subvol_lookup(parent->dir, parent->ino, parent->name,
	    parent->namelen, &entry);
	if (error != 0 && error != ENOENT)
		return (error);
	if (entry.ino != 0)
		return (EEXIST);
	if (cmd == BTRFSIOC_SNAPSHOT) {
		error = subvol_resolve(bmp, args->source, &create->snapshot,
		    &sourceino);
		if (error == 0 && sourceino != BTRFS_FIRST_FREE_OBJECTID)
			error = EINVAL;
		if (error == 0)
			error = btrfs_find_root_item(op->roots,
			    create->snapshot->br_owner,
			    BTRFS_FIRST_FREE_OBJECTID, &create->snapshot_item,
			    NULL);
		if (error != 0)
			return (error);
		*item = create->snapshot_item;
		memcpy(item->parent_uuid, create->snapshot_item.uuid,
		    BTRFS_UUID_SIZE);
	} else {
		error = btrfs_get_root(bmp, BTRFS_FS_TREE_OBJECTID,
		    &create->empty_template);
		if (error != 0)
			return (error);
	}
	error = subvol_create_identity(bmp, op, &index);
	if (error == 0)
		error = subvol_namespace_prepare(op);
	if (error != 0)
		return (error);
	if (letoh64(ns->inode.size) > UINT64_MAX - parent->namelen * 2)
		return (EOVERFLOW);
	ns->inode.size = htole64(letoh64(ns->inode.size) +
	    parent->namelen * 2);
	record.location.objectid = htole64(op->id);
	record.location.type = BTRFS_ROOT_ITEM_KEY;
	record.location.offset = htole64(UINT64_MAX);
	record.type = BTRFS_FT_DIR;
	error = btrfs_plan_dir_add(ns->plan, parent->ino, parent->name,
	    parent->namelen, index, &record);
	if (error != 0)
		return (error);
	rr->dirid = htole64(parent->ino);
	rr->sequence = htole64(index);
	rr->name_len = htole16(parent->namelen);
	memcpy(rr + 1, parent->name, parent->namelen);

	if (create->snapshot == NULL) {
		item->inode.mode = htole32(S_IFDIR |
		    (0777 & ~p->p_fd->fd_cmask) |
		    (letoh32(ns->inode.mode) & S_ISGID));
		item->inode.nlink = htole32(1);
		item->inode.uid = htole32(p->p_ucred->cr_uid);
		item->inode.gid = ns->inode.gid;
		item->inode.flags = htole64(letoh64(ns->inode.flags) &
		    (BTRFS_INODE_NODATACOW | BTRFS_INODE_COMPRESS |
		    BTRFS_INODE_NOCOMPRESS));
	}
	arc4random_buf(item->uuid, sizeof(item->uuid));
	item->uuid[6] = (item->uuid[6] & 0x0f) | 0x40;
	item->uuid[8] = (item->uuid[8] & 0x3f) | 0x80;
	memset(item->received_uuid, 0, sizeof(item->received_uuid));
	memset(&item->drop_progress, 0, sizeof(item->drop_progress));
	item->drop_level = 0;
	item->root_dirid = htole64(BTRFS_FIRST_FREE_OBJECTID);
	item->refs = htole32(1);
	item->flags = htole64(args->flags & BTRFS_CTL_RDONLY ?
	    BTRFS_ROOT_SUBVOL_RDONLY : 0);
	item->stransid = item->rtransid = 0;
	op->reservation.btr_metadata = (256 + 8) * nodesize;
	return (0);
}

static int
subvol_delete_prepare(struct btrfs_fs *bmp, struct subvol_operation *op)
{
	struct subvol_delete *delete = &op->u.delete;
	struct subvol_parent *parent = &op->parent;
	struct subvol_namespace *ns = &op->ns;
	struct subvol_entry entry;
	struct btrfs_path path = { 0 };
	struct btrfs_key target = { 0 };
	const struct btrfs_key *key;
	struct btrfs_root_ref *rr;
	uint8_t reference[sizeof(*rr) + BTRFS_NAME_MAX];
	uint64_t blocks, refs, index;
	uint32_t nodesize = letoh32(bmp->bm_super.nodesize);
	uint32_t size;
	int error;

	error = subvol_lookup(parent->dir, parent->ino, parent->name,
	    parent->namelen, &entry);
	if (error != 0 && error != ENOENT)
		return (error);
	if (entry.ino == 0)
		return (ENOENT);
	if (!entry.subvol)
		return (EINVAL);
	op->id = entry.ino;
	bmp->bm_last_rootid = MAX(bmp->bm_last_rootid, op->id);
	error = btrfs_get_root(bmp, op->id, &delete->victim);
	if (error != 0)
		return (error);
	/* Own the fence even if eligibility or reservation subsequently fails. */
	mtx_enter(&bmp->bm_nodemtx);
	delete->victim->br_deleted = 1;
	mtx_leave(&bmp->bm_nodemtx);
	error = btrfs_control_busy(bmp, op->id);
	if (error != 0)
		return (error);
	/* A nested subvolume must be removed explicitly first. */
	target.objectid = htole64(op->id);
	target.type = BTRFS_ROOT_REF_KEY;
	error = btrfs_search_lower_bound(op->roots, &target, &path);
	if (error == 0) {
		error = btrfs_path_item(&path, &key, NULL, NULL);
		if (error == 0 && key->objectid == target.objectid &&
		    key->type == target.type)
			error = ENOTEMPTY;
	}
	btrfs_release_path(&path);
	if (error != 0 && error != ENOENT)
		return (error);
	error = btrfs_find_root_item(op->roots, op->id,
	    BTRFS_FIRST_FREE_OBJECTID, &delete->item, NULL);
	if (error == 0)
		error = btrfs_count_tree(delete->victim, &blocks, &refs);
	if (error != 0)
		return (error);
	op->rkey.objectid = htole64(op->id);
	op->rkey.type = BTRFS_ROOT_ITEM_KEY;
	op->rkey.offset = htole64(delete->victim->br_root_offset);
	target.type = BTRFS_ROOT_BACKREF_KEY;
	target.offset = htole64(parent->dir->br_owner);
	error = subvol_read(op->roots, &target, reference, sizeof(reference),
	    &size);
	if (error != 0)
		return (error);
	rr = (void *)reference;
	if (size != sizeof(*rr) + parent->namelen ||
	    letoh64(rr->dirid) != parent->ino ||
	    letoh16(rr->name_len) != parent->namelen ||
	    memcmp(rr + 1, parent->name, parent->namelen) != 0)
		return (EINVAL);
	index = letoh64(rr->sequence);
	error = subvol_namespace_prepare(op);
	if (error != 0)
		return (error);
	if (letoh64(ns->inode.size) < parent->namelen * 2)
		return (EOVERFLOW);
	error = btrfs_plan_dir_remove(ns->plan, parent->ino, parent->name,
	    parent->namelen, index, op->id, BTRFS_ROOT_ITEM_KEY, NULL);
	if (error != 0)
		return (error);
	ns->inode.size = htole64(letoh64(ns->inode.size) -
	    parent->namelen * 2);
	/* Whole deletion, including all reference drops, is one reservation. */
	if (blocks > UINT64_MAX / nodesize / 32 ||
	    refs > UINT64_MAX / nodesize / 32)
		return (EOVERFLOW);
	op->reservation.btr_metadata =
	    (256 + blocks * 8 + refs * 16) * nodesize;
	return (0);
}

static int
subvol_namespace_apply(struct btrfs_trans_handle *handle,
    struct subvol_operation *op, const struct timespec *now)
{
	struct subvol_namespace *ns = &op->ns;
	struct btrfs_root *dir = op->parent.dir;
	uint64_t gen = handle->bth_transaction->bt_generation;
	int error;

	error = btrfs_name_plan_apply(handle, ns->plan);
	ns->inode.transid = htole64(gen);
	ns->inode.sequence = htole64(letoh64(ns->inode.sequence) + 1);
	ns->inode.ctime.sec = ns->inode.mtime.sec = htole64(now->tv_sec);
	ns->inode.ctime.nsec = ns->inode.mtime.nsec = htole32(now->tv_nsec);
	if (error == 0)
		error = btrfs_replace_item(handle, dir, &ns->ikey, &ns->inode,
		    sizeof(ns->inode));
	return (error);
}

static int
subvol_create_apply(struct btrfs_trans_handle *handle,
    struct subvol_operation *op)
{
	struct subvol_create *create = &op->u.create;
	struct subvol_namespace *ns = &op->ns;
	struct btrfs_root_item *item = &create->item;
	struct btrfs_key skey = { 0 };
	struct timespec now;
	uint64_t gen = handle->bth_transaction->bt_generation;
	uint32_t refsize = sizeof(struct btrfs_root_ref) + op->parent.namelen;
	int error;

	nanotime(&now);
	if (create->snapshot == NULL) {
		item->inode.generation = item->inode.transid = htole64(gen);
		item->inode.atime.sec = item->inode.mtime.sec =
		    item->inode.ctime.sec = item->inode.otime.sec =
		    htole64(now.tv_sec);
		item->inode.atime.nsec = item->inode.mtime.nsec =
		    item->inode.ctime.nsec = item->inode.otime.nsec =
		    htole32(now.tv_nsec);
	}
	item->ctransid = item->otransid = htole64(gen);
	item->last_snapshot = create->snapshot != NULL ? htole64(gen) : 0;
	item->ctime.sec = item->otime.sec = htole64(now.tv_sec);
	item->ctime.nsec = item->otime.nsec = htole32(now.tv_nsec);
	error = btrfs_new_subvolume_root(handle,
	    create->snapshot != NULL ? create->snapshot : create->empty_template,
	    op->id, item, create->snapshot != NULL);
	/*
	 * Linux needs this marker to retain initialized root flags. Set it
	 * after building the tree so it is not copied into the directory inode.
	 */
	item->inode.flags = htole64(letoh64(item->inode.flags) |
	    BTRFS_INODE_ROOT_ITEM_INIT);
	if (error == 0)
		error = btrfs_insert_item(handle, op->roots, &op->rkey, item,
		    sizeof(*item));
	if (error == 0)
		error = subvol_uuid(handle, item, op->id, 0);
	if (error == 0 && create->snapshot != NULL) {
		create->snapshot_item.last_snapshot = htole64(gen);
		skey.objectid = htole64(create->snapshot->br_owner);
		skey.type = BTRFS_ROOT_ITEM_KEY;
		skey.offset = htole64(create->snapshot->br_root_offset);
		error = btrfs_replace_item(handle, op->roots, &skey,
		    &create->snapshot_item, sizeof(create->snapshot_item));
	}
	if (error == 0)
		error = btrfs_insert_item(handle, op->roots, &ns->backkey,
		    create->reference, refsize);
	if (error == 0)
		error = btrfs_insert_item(handle, op->roots, &ns->refkey,
		    create->reference, refsize);
	if (error == 0)
		error = subvol_namespace_apply(handle, op, &now);
	return (error);
}

static int
subvol_delete_apply(struct btrfs_trans_handle *handle,
    struct subvol_operation *op)
{
	struct subvol_delete *delete = &op->u.delete;
	struct subvol_namespace *ns = &op->ns;
	struct timespec now;
	int error;

	nanotime(&now);
	error = btrfs_drop_subvolume_tree(handle, delete->victim);
	if (error == 0)
		error = btrfs_delete_item(handle, op->roots, &op->rkey);
	if (error == 0)
		error = subvol_uuid(handle, &delete->item, op->id, 1);
	if (error == 0)
		error = btrfs_delete_item(handle, op->roots, &ns->backkey);
	if (error == 0)
		error = btrfs_delete_item(handle, op->roots, &ns->refkey);
	if (error == 0)
		error = subvol_namespace_apply(handle, op, &now);
	return (error);
}

/*
 * Always release preparation's exclusion, including on preparation failure.
 * Successful commit transfers it to the root cache as a permanent tombstone.
 * A later parent-cache refresh error must not undo that transfer.
 */
static void
subvol_delete_finish(struct btrfs_fs *bmp, struct subvol_delete *delete,
    int committed)
{
	if (delete->victim == NULL)
		return;
	if (committed)
		btrfs_forget_root(bmp, delete->victim->br_owner);
	else {
		mtx_enter(&bmp->bm_nodemtx);
		delete->victim->br_deleted = 0;
		mtx_leave(&bmp->bm_nodemtx);
	}
	delete->victim = NULL;
}

int
btrfs_subvolume(struct btrfs_fs *bmp, u_long cmd,
    struct btrfs_ioctl_subvolume *args, struct proc *p)
{
	struct subvol_operation *op;
	struct btrfs_trans_handle *handle;
	struct btrfs_node *node;
	struct vnode *vp = NULL;
	int error, enderror, committed = 0;

	if (cmd == BTRFSIOC_LIST)
		return (subvol_list(bmp, args));
	op = malloc(sizeof(*op), M_BTRFS, M_WAITOK | M_ZERO);
	error = subvol_parent_resolve(bmp, args->path, &op->parent);
	if (error != 0)
		goto done;
	/* Parent vnode before namespace lock before writer gate. */
	error = btrfs_control_parent(bmp, op->parent.dir->br_owner,
	    op->parent.ino, &vp);
	if (error != 0)
		goto done;
	rw_enter_write(&bmp->bm_namespace_lock);
	subvol_admin_enter(bmp, p);
	error = btrfs_commit_current(bmp, p);
	if (error == 0)
		error = btrfs_get_root(bmp, BTRFS_ROOT_TREE_OBJECTID, &op->roots);
	if (error != 0)
		goto unlock;
	if (cmd == BTRFSIOC_DELETE)
		error = subvol_delete_prepare(bmp, op);
	else
		error = subvol_create_prepare(bmp, op, cmd, args, p);
	if (error != 0)
		goto unlock;
	error = btrfs_trans_join(bmp, &op->reservation, &handle);
	if (error != 0)
		goto unlock;

	/* From apply through end there are no exits that can skip abort/end. */
	if (cmd == BTRFSIOC_DELETE)
		error = subvol_delete_apply(handle, op);
	else
		error = subvol_create_apply(handle, op);
	if (error != 0)
		btrfs_trans_abort(handle, error);
	enderror = btrfs_trans_end(handle);
	if (error == 0)
		error = enderror;
	if (error == 0)
		error = btrfs_commit_current(bmp, p);
	committed = error == 0;
unlock:
	if (cmd == BTRFSIOC_DELETE)
		subvol_delete_finish(bmp, &op->u.delete, committed);
	if (committed && vp != NULL) {
		node = VTOBTRFS(vp);
		error = btrfs_find_inode(op->parent.dir, op->parent.ino,
		    &node->bn_inode);
		cache_purge(vp);
		VN_KNOTE(vp, NOTE_WRITE);
	}
	subvol_admin_leave(bmp, p);
	rw_exit_write(&bmp->bm_namespace_lock);
done:
	if (vp != NULL)
		vput(vp);
	btrfs_name_plan_free(op->ns.plan);
	free(op, M_BTRFS, sizeof(*op));
	return (error);
}
