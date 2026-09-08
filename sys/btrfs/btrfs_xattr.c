/* Public domain. */

/*
 * Privileged control operations preserve Linux xattrs, including on symlinks,
 * as opaque data. ACLs and security labels are neither enforced nor inherited.
 * Packed-record framing and item edits are shared with directories, but xattr
 * names, values and locations have their own validation. Attribute changes
 * update inode ctime, sequence and transaction ID in the same reserved handle.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/btrfsio.h>

#include <btrfs/btrfs_var.h>
#include <btrfs/btrfs_dir.h>

/* Linux xattrs remain opaque; their names need not be path components. */
static int
btrfs_decode_xattr(const struct btrfs_key *key, const uint8_t *data,
    uint32_t size, struct btrfs_dir_record *record)
{
	int error;

	error = btrfs_decode_dir_record(data, size, record);
	if (error != 0)
		return (error);
	if (record->namelen == 0 || record->namelen > BTRFS_NAME_MAX ||
	    memchr(record->name, '\0', record->namelen) != NULL ||
	    record->item->type != BTRFS_FT_XATTR ||
	    record->item->location.objectid != 0 ||
	    record->item->location.type != 0 ||
	    record->item->location.offset != 0 ||
	    letoh64(key->offset) !=
	    btrfs_name_hash(record->name, record->namelen))
		return (EINVAL);
	return (0);
}

static int
btrfs_get_xattr(struct btrfs_root *root, struct btrfs_ioctl_xattr *args)
{
	struct btrfs_path path = { 0 };
	struct btrfs_key target = { 0 };
	const struct btrfs_key *key;
	const uint8_t *data;
	struct btrfs_dir_record record;
	uint8_t *value = NULL;
	uint64_t cursor = 0;
	uint32_t size, pos;
	int error, found = 0;

	target.objectid = htole64(args->ino);
	target.type = BTRFS_XATTR_ITEM_KEY;
	error = btrfs_search_lower_bound(root, &target, &path);
	while (error == 0) {
		error = btrfs_path_item(&path, &key, &data, &size);
		if (error != 0)
			break;
		if (key->objectid != target.objectid || key->type != target.type) {
			error = ENOENT;
			break;
		}
		if (size == 0) {
			error = EINVAL;
			break;
		}
		for (pos = 0; pos < size; pos += record.size) {
			error = btrfs_decode_xattr(key, data + pos, size - pos,
			    &record);
			if (error != 0)
				break;
			if (cursor++ != args->cursor)
				continue;
			if (args->size < record.datalen) {
				error = ERANGE;
				break;
			}
			memcpy(args->name, record.name, record.namelen);
			args->name[record.namelen] = '\0';
			args->size = record.datalen;
			value = malloc(MAX(args->size, 1), M_BTRFS, M_WAITOK);
			memcpy(value, record.value, args->size);
			found = 1;
			/* Validate the rest of this bucket before returning it. */
		}
		if (error != 0 || found)
			break;
		error = btrfs_next_item(&path);
	}
	btrfs_release_path(&path);
	if (error == 0 && found) {
		error = copyout(value, args->value, args->size);
		if (error == 0)
			args->cursor++;
	}
	free(value, M_BTRFS, MAX(args->size, 1));
	return (error);
}

int
btrfs_control_xattr(struct vnode *select, u_long cmd,
    struct btrfs_ioctl_xattr *args, struct proc *p)
{
	struct btrfs_fs *bmp = VTOBTRFS(select)->bn_mount;
	struct btrfs_node *node;
	struct btrfs_root *root;
	struct vnode *vp;
	struct btrfs_dir_record record;
	struct btrfs_dir_item *di;
	struct btrfs_name_plan *plan = NULL;
	struct btrfs_name_edit *edit;
	struct btrfs_trans_handle *handle;
	struct btrfs_trans_reservation res = { 0 };
	struct btrfs_inode saved;
	uint8_t *value = NULL;
	uint32_t pos, match = 0, matchsize = 0, extra;
	size_t namelen = strlen(args->name);
	int error, enderror;

	if (args->ino < BTRFS_FIRST_FREE_OBJECTID)
		return (EINVAL);
	error = btrfs_vget_tree(select->v_mount, VTOBTRFS(select)->bn_treeid,
	    args->ino, &vp);
	if (error != 0)
		return (error);
	node = VTOBTRFS(vp);
	root = node->bn_root;
	if (cmd == BTRFSIOC_GETXATTR) {
		error = btrfs_get_xattr(root, args);
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
	plan = btrfs_name_plan_alloc(root);
	if (namelen == 0 || namelen > BTRFS_NAME_MAX || args->cursor != 0 ||
	    args->size > plan->capacity - sizeof(*di) - namelen ||
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
	error = btrfs_name_edit(plan, args->ino, BTRFS_XATTR_ITEM_KEY,
	    btrfs_name_hash(args->name, namelen), &edit);
	if (error != 0)
		goto out;
	for (pos = 0; pos < edit->size; pos += record.size) {
		error = btrfs_decode_xattr(&edit->key, edit->data + pos,
		    edit->size - pos, &record);
		if (error != 0)
			goto out;
		if (record.namelen != namelen ||
		    memcmp(record.name, args->name, namelen) != 0)
			continue;
		if (matchsize != 0) {
			error = EINVAL;
			goto out;
		}
		match = pos;
		matchsize = record.size;
	}
	if (matchsize != 0) {
		memmove(edit->data + match, edit->data + match + matchsize,
		    edit->size - match - matchsize);
		edit->size -= matchsize;
	} else if (cmd == BTRFSIOC_RMXATTR) {
		error = ENOENT;
		goto out;
	}
	if (cmd == BTRFSIOC_SETXATTR) {
		extra = sizeof(*di) + namelen + args->size;
		if (edit->size > plan->capacity - extra) {
			error = ENOSPC;
			goto out;
		}
		di = (struct btrfs_dir_item *)(edit->data + edit->size);
		memset(di, 0, sizeof(*di));
		di->type = BTRFS_FT_XATTR;
		di->name_len = htole16(namelen);
		di->data_len = htole16(args->size);
		memcpy(di + 1, args->name, namelen);
		memcpy((uint8_t *)(di + 1) + namelen, value, args->size);
		edit->size += extra;
	}
	edit->dirty = 1;
	/* The bucket and inode update share the caller's reservation. */
	res.btr_metadata = 128 * plan->nodesize;
	error = btrfs_trans_join(bmp, &res, &handle);
	if (error != 0)
		goto out;
	saved = node->bn_inode;
	error = btrfs_name_plan_apply(handle, plan);
	if (error == 0) {
		nanotime(&node->bn_inode.bi_ctime);
		node->bn_inode.bi_sequence++;
		node->bn_inode.bi_last_dirty_transid =
		    handle->bth_transaction->bt_generation;
		node->bn_inode.bi_dirty_fields |= BTRFS_INODE_DIRTY_CTIME |
		    BTRFS_INODE_DIRTY_SEQUENCE;
		error = btrfs_write_inode(handle, node);
	}
	if (error != 0)
		btrfs_trans_abort(handle, error);
	enderror = btrfs_trans_end(handle);
	if (error == 0)
		error = enderror;
	if (error != 0)
		node->bn_inode = saved;
	else {
		VN_KNOTE(vp, NOTE_ATTRIB);
		if ((vp->v_mount->mnt_flag & MNT_SYNCHRONOUS) ||
		    (node->bn_inode.bi_flags & BTRFS_INODE_SYNC))
			error = btrfs_commit_current(bmp, p);
	}
out:
	free(value, M_BTRFS, MAX(args->size, 1));
	btrfs_name_plan_free(plan);
	vput(vp);
	return (error);
}
