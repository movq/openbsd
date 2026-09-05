/*	$OpenBSD$	*/

/*
 * Copyright (c) 2026 Mike Jones <mike@mjones.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/endian.h>
#include <sys/dkio.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/vnode.h>

#include <btrfs/btrfs_var.h>

static struct btrfs_transaction *
	btrfs_trans_alloc(struct btrfs_fs *, uint64_t);
static int	btrfs_materialize_tree_ref(struct btrfs_trans_handle *,
		    const struct btrfs_delayed_tree_ref *);
static int	btrfs_sync_device(struct btrfs_fs *, struct proc *);

static struct btrfs_transaction *
btrfs_trans_alloc(struct btrfs_fs *bmp, uint64_t generation)
{
	struct btrfs_transaction *trans;

	trans = malloc(sizeof(*trans), M_BTRFS, M_WAITOK | M_ZERO);
	trans->bt_mount = bmp;
	trans->bt_generation = generation;
	trans->bt_state = BTRFS_TRANS_OPEN;
	mtx_init(&trans->bt_lock, IPL_NONE);
	TAILQ_INIT(&trans->bt_commit_reservations);
	TAILQ_INIT(&trans->bt_reclaim_reservations);
	TAILQ_INIT(&trans->bt_allocated_extents);
	TAILQ_INIT(&trans->bt_pinned_extents);
	TAILQ_INIT(&trans->bt_dirty_extent_buffers);
	TAILQ_INIT(&trans->bt_delayed_tree_refs);
	TAILQ_INIT(&trans->bt_delayed_data_refs);
	TAILQ_INIT(&trans->bt_ordered_extents);
	TAILQ_INIT(&trans->bt_dirty_roots);
	return (trans);
}

int
btrfs_trans_init(struct btrfs_fs *bmp)
{
	uint64_t generation;
	int error;

	bmp->bm_last_transid = letoh64(bmp->bm_super.generation);
	mtx_init(&bmp->bm_trans_mtx, IPL_NONE);
	if (bmp->bm_last_transid == UINT64_MAX)
		generation = UINT64_MAX;
	else
		generation = bmp->bm_last_transid + 1;
	bmp->bm_transaction = btrfs_trans_alloc(bmp, generation);
	if (generation == UINT64_MAX && bmp->bm_last_transid == UINT64_MAX) {
		bmp->bm_transaction->bt_error = EOVERFLOW;
		bmp->bm_transaction->bt_state = BTRFS_TRANS_ABORTED;
		if (!bmp->bm_readonly) {
			free(bmp->bm_transaction, M_BTRFS,
			    sizeof(*bmp->bm_transaction));
			bmp->bm_transaction = NULL;
			return (EOVERFLOW);
		}
	}
	if (!bmp->bm_readonly) {
		error = btrfs_space_reserve_commit(bmp->bm_transaction);
		if (error != 0) {
			free(bmp->bm_transaction, M_BTRFS,
			    sizeof(*bmp->bm_transaction));
			bmp->bm_transaction = NULL;
			return (error);
		}
	}
	return (0);
}

void
btrfs_trans_destroy(struct btrfs_fs *bmp)
{
	struct btrfs_transaction *trans = bmp->bm_transaction;

	if (trans == NULL)
		return;
	KASSERT(trans->bt_writers == 0);
	KASSERT(!bmp->bm_committer);
	KASSERT(!trans->bt_commit_handle);
	(void)btrfs_roots_finish(trans, 0);
	(void)btrfs_delayed_refs_finish(trans, 0);
	(void)btrfs_ordered_extents_finish(trans, 0);
	(void)btrfs_extent_buffers_finish(trans, 0);
	KASSERT(TAILQ_EMPTY(&trans->bt_dirty_roots));
	KASSERT(TAILQ_EMPTY(&trans->bt_delayed_tree_refs));
	KASSERT(TAILQ_EMPTY(&trans->bt_delayed_data_refs));
	KASSERT(TAILQ_EMPTY(&trans->bt_ordered_extents));
	KASSERT(TAILQ_EMPTY(&trans->bt_dirty_extent_buffers));
	if (trans->bt_state != BTRFS_TRANS_COMMITTED)
		btrfs_space_abort(trans);
	KASSERT(TAILQ_EMPTY(&trans->bt_commit_reservations));
	KASSERT(TAILQ_EMPTY(&trans->bt_allocated_extents));
	KASSERT(TAILQ_EMPTY(&trans->bt_pinned_extents));
	free(trans, M_BTRFS, sizeof(*trans));
	bmp->bm_transaction = NULL;
}

int
btrfs_trans_join(struct btrfs_fs *bmp,
    const struct btrfs_trans_reservation *reservation,
    struct btrfs_trans_handle **handlep)
{
	struct btrfs_trans_handle *handle;
	struct btrfs_transaction *trans;
	uint64_t generation;
	int dirty, end_error, error, retried = 0, grown = 0;

	*handlep = NULL;
retry:
	if (bmp->bm_readonly)
		return (EROFS);
	handle = malloc(sizeof(*handle), M_BTRFS, M_WAITOK | M_ZERO);
	TAILQ_INIT(&handle->bth_reservations);

	mtx_enter(&bmp->bm_trans_mtx);
	for (;;) {
		trans = bmp->bm_transaction;
		KASSERT(trans != NULL);
		if (trans->bt_state == BTRFS_TRANS_ABORTED) {
			error = trans->bt_error != 0 ? trans->bt_error : EROFS;
			mtx_leave(&bmp->bm_trans_mtx);
			free(handle, M_BTRFS, sizeof(*handle));
			return (error);
		}
		if (trans->bt_state == BTRFS_TRANS_OPEN &&
		    !bmp->bm_committer) {
			if (trans->bt_commit_reserve_target == 0) {
				trans->bt_error = ENOSPC;
				trans->bt_state = BTRFS_TRANS_ABORTED;
				btrfs_fs_set_readonly(bmp);
				mtx_leave(&bmp->bm_trans_mtx);
				free(handle, M_BTRFS, sizeof(*handle));
				return (ENOSPC);
			}
			break;
		}
		msleep(&bmp->bm_transaction, &bmp->bm_trans_mtx, PWAIT,
		    "btrjoin", 0);
	}
	KASSERT(trans->bt_writers != UINT_MAX);
	trans->bt_writers++;
	handle->bth_transaction = trans;
	mtx_leave(&bmp->bm_trans_mtx);

	error = btrfs_space_reserve(handle, reservation);
	if (error != 0) {
		generation = trans->bt_generation;
		mtx_enter(&trans->bt_lock);
		dirty = !TAILQ_EMPTY(&trans->bt_dirty_extent_buffers);
		mtx_leave(&trans->bt_lock);
		end_error = btrfs_trans_end(handle);
		if (end_error != 0)
			return (end_error);
		/*
		 * Pending work may own most of the free metadata.  Publish it
		 * and release unused promises before declaring ENOSPC.  No
		 * mutation from this operation has occurred, and commit never
		 * takes the vnode or namespace locks the caller may hold.
		 */
		if (error == ENOSPC && dirty && !retried) {
			error = btrfs_trans_commit(bmp, generation, curproc);
			if (error != 0)
				return (error);
			retried = 1;
			goto retry;
		}
		if (error == ENOSPC && reservation != NULL &&
		    reservation->btr_data != 0 && !grown) {
			error = btrfs_space_grow_data(bmp, reservation->btr_data);
			if (error != 0)
				return (error);
			grown = 1;
			retried = 0;
			goto retry;
		}
		return (error);
	}
	*handlep = handle;
	return (0);
}

int
btrfs_trans_commit_handle(struct btrfs_transaction *trans,
    struct btrfs_trans_handle **handlep)
{
	struct btrfs_fs *bmp = trans->bt_mount;
	struct btrfs_trans_handle *handle;
	int error;

	*handlep = NULL;
	handle = malloc(sizeof(*handle), M_BTRFS, M_WAITOK | M_ZERO);
	TAILQ_INIT(&handle->bth_reservations);

	mtx_enter(&bmp->bm_trans_mtx);
	if (bmp->bm_transaction != trans || !bmp->bm_committer ||
	    trans->bt_state != BTRFS_TRANS_COMMITTING ||
	    trans->bt_commit_handle ||
	    trans->bt_commit_reserve_target == 0) {
		error = trans->bt_error != 0 ? trans->bt_error : EINVAL;
		mtx_leave(&bmp->bm_trans_mtx);
		free(handle, M_BTRFS, sizeof(*handle));
		return (error);
	}
	KASSERT(trans->bt_writers == 0);
	trans->bt_commit_handle = 1;
	handle->bth_transaction = trans;
	handle->bth_commit = 1;
	mtx_leave(&bmp->bm_trans_mtx);

	*handlep = handle;
	return (0);
}

int
btrfs_trans_end(struct btrfs_trans_handle *handle)
{
	struct btrfs_transaction *trans = handle->bth_transaction;
	struct btrfs_fs *bmp = trans->bt_mount;
	int error;

	btrfs_space_keep_delayed(handle);
	btrfs_space_release(handle);
	mtx_enter(&bmp->bm_trans_mtx);
	KASSERT(trans == bmp->bm_transaction);
	if (handle->bth_commit) {
		KASSERT(bmp->bm_committer);
		KASSERT(trans->bt_state == BTRFS_TRANS_COMMITTING ||
		    trans->bt_state == BTRFS_TRANS_ABORTED);
		KASSERT(trans->bt_commit_handle);
		trans->bt_commit_handle = 0;
		error = trans->bt_error;
		mtx_leave(&bmp->bm_trans_mtx);
		free(handle, M_BTRFS, sizeof(*handle));
		return (error);
	}
	KASSERT(trans->bt_writers > 0);
	trans->bt_writers--;
	error = trans->bt_error;
	if (trans->bt_writers == 0)
		wakeup(&trans->bt_writers);
	mtx_leave(&bmp->bm_trans_mtx);
	free(handle, M_BTRFS, sizeof(*handle));
	return (error);
}

void
btrfs_trans_abort(struct btrfs_trans_handle *handle, int error)
{
	struct btrfs_transaction *trans = handle->bth_transaction;
	struct btrfs_fs *bmp = trans->bt_mount;

	if (error == 0)
		error = EIO;
	mtx_enter(&bmp->bm_trans_mtx);
	KASSERT(trans == bmp->bm_transaction);
	if (trans->bt_error == 0)
		trans->bt_error = error;
	trans->bt_state = BTRFS_TRANS_ABORTED;
	btrfs_fs_set_readonly(bmp);
	wakeup(&bmp->bm_transaction);
	mtx_leave(&bmp->bm_trans_mtx);
}

int
btrfs_delayed_ref_add(struct btrfs_trans_handle *handle, uint64_t bytenr,
    uint64_t parent, uint64_t root, uint8_t level, int ref_mod)
{
	struct btrfs_delayed_tree_ref *ref, *new;
	struct btrfs_transaction *trans;
	struct btrfs_fs *bmp;
	uint32_t sectorsize;

	if (handle == NULL || handle->bth_transaction == NULL)
		return (EINVAL);
	trans = handle->bth_transaction;
	bmp = trans->bt_mount;
	sectorsize = letoh32(bmp->bm_super.sectorsize);
	if (bytenr == 0 || (bytenr & (sectorsize - 1)) != 0 ||
	    (parent != 0 && (parent & (sectorsize - 1)) != 0) ||
	    root == 0 || level >= BTRFS_MAX_LEVEL ||
	    (ref_mod != -1 && ref_mod != 1))
		return (EINVAL);

	new = malloc(sizeof(*new), M_BTRFS, M_WAITOK | M_ZERO);
	new->bdr_bytenr = bytenr;
	new->bdr_parent = parent;
	new->bdr_root = root;
	new->bdr_level = level;
	new->bdr_ref_mod = ref_mod;

	mtx_enter(&trans->bt_lock);
	TAILQ_FOREACH(ref, &trans->bt_delayed_tree_refs, bdr_entry) {
		if (ref->bdr_bytenr == bytenr &&
		    ref->bdr_parent == parent && ref->bdr_root == root &&
		    ref->bdr_level == level)
			break;
	}
	if (ref == NULL) {
		TAILQ_INSERT_TAIL(&trans->bt_delayed_tree_refs, new,
		    bdr_entry);
		new = NULL;
	} else {
		KASSERT(ref->bdr_ref_mod != 0);
		ref->bdr_ref_mod += ref_mod;
		if (ref->bdr_ref_mod == 0) {
			TAILQ_REMOVE(&trans->bt_delayed_tree_refs, ref,
			    bdr_entry);
			free(ref, M_BTRFS, sizeof(*ref));
		}
	}
	mtx_leave(&trans->bt_lock);
	if (new != NULL)
		free(new, M_BTRFS, sizeof(*new));
	handle->bth_delayed = 1;
	return (0);
}

static int
btrfs_materialize_tree_ref(struct btrfs_trans_handle *handle,
    const struct btrfs_delayed_tree_ref *ref)
{
	struct btrfs_transaction *trans = handle->bth_transaction;
	struct btrfs_fs *bmp = trans->bt_mount;
	struct btrfs_extent_item *extent;
	struct btrfs_extent_inline_ref *candidate, *inline_ref;
	struct btrfs_path path = { 0 };
	struct btrfs_root *root;
	struct btrfs_key extent_key, backref_key;
	const struct btrfs_key *found_key;
	const uint8_t *data;
	uint8_t *payload = NULL;
	uint64_t refs;
	uint32_t alloc_size = 0, i, remain, size;
	uint8_t type;
	int error, found_inline = 0;

	if ((letoh64(bmp->bm_super.incompat_flags) &
	    BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA) == 0 ||
	    ref->bdr_ref_mod < -1 || ref->bdr_ref_mod > 1 ||
	    ref->bdr_ref_mod == 0)
		return (EOPNOTSUPP);
	error = btrfs_get_root(bmp, BTRFS_EXTENT_TREE_OBJECTID, &root);
	if (error != 0)
		return (error);

	memset(&extent_key, 0, sizeof(extent_key));
	extent_key.objectid = htole64(ref->bdr_bytenr);
	extent_key.type = BTRFS_METADATA_ITEM_KEY;
	extent_key.offset = htole64(ref->bdr_level);
	type = ref->bdr_parent != 0 ? BTRFS_SHARED_BLOCK_REF_KEY :
	    BTRFS_TREE_BLOCK_REF_KEY;
	memset(&backref_key, 0, sizeof(backref_key));
	backref_key.objectid = htole64(ref->bdr_bytenr);
	backref_key.type = type;
	backref_key.offset = htole64(ref->bdr_parent != 0 ?
	    ref->bdr_parent : ref->bdr_root);

	error = btrfs_search_slot(root, &extent_key, &path);
	if (error == ENOENT) {
		btrfs_release_path(&path);
		if (ref->bdr_ref_mod < 0)
			return (EINVAL);
		size = sizeof(*extent) + sizeof(*inline_ref);
		payload = malloc(size, M_BTRFS, M_WAITOK | M_ZERO);
		extent = (struct btrfs_extent_item *)payload;
		extent->refs = htole64(1);
		extent->generation = htole64(trans->bt_generation);
		extent->flags = htole64(BTRFS_EXTENT_FLAG_TREE_BLOCK);
		inline_ref = (struct btrfs_extent_inline_ref *)(extent + 1);
		inline_ref->type = type;
		inline_ref->offset = backref_key.offset;
		error = btrfs_insert_item(handle, root, &extent_key, payload,
		    size);
		free(payload, M_BTRFS, size);
		if (error == 0)
			error = btrfs_update_free_space(handle, ref->bdr_bytenr,
			    letoh32(bmp->bm_super.nodesize), 0);
		return (error);
	}
	if (error != 0)
		return (error);
	error = btrfs_path_item(&path, &found_key, &data, &size);
	if (error != 0)
		goto out;
	if (memcmp(found_key, &extent_key, sizeof(*found_key)) != 0 ||
	    size < sizeof(*extent)) {
		error = EINVAL;
		goto out;
	}
	payload = malloc(size, M_BTRFS, M_WAITOK);
	alloc_size = size;
	memcpy(payload, data, size);
	btrfs_release_path(&path);

	extent = (struct btrfs_extent_item *)payload;
	refs = letoh64(extent->refs);
	if (refs == 0 ||
	    letoh64(extent->flags) != BTRFS_EXTENT_FLAG_TREE_BLOCK ||
	    letoh64(extent->generation) > trans->bt_generation) {
		error = EINVAL;
		goto done;
	}
	remain = size - sizeof(*extent);
	inline_ref = (struct btrfs_extent_inline_ref *)(extent + 1);
	for (i = 0; i < remain; i += sizeof(*inline_ref)) {
		candidate = (struct btrfs_extent_inline_ref *)
		    ((uint8_t *)inline_ref + i);
		if (remain - i < sizeof(*candidate) ||
		    (candidate->type != BTRFS_TREE_BLOCK_REF_KEY &&
		    candidate->type != BTRFS_SHARED_BLOCK_REF_KEY)) {
			error = EOPNOTSUPP;
			goto done;
		}
		if (candidate->type == type &&
		    candidate->offset == backref_key.offset) {
			found_inline = 1;
			break;
		}
	}

	if (ref->bdr_ref_mod > 0) {
		if (found_inline || refs == UINT64_MAX) {
			error = EINVAL;
			goto done;
		}
		error = btrfs_search_slot(root, &backref_key, &path);
		if (error == 0) {
			btrfs_release_path(&path);
			error = EINVAL;
			goto done;
		}
		btrfs_release_path(&path);
		if (error != ENOENT)
			goto done;
		error = btrfs_insert_item(handle, root, &backref_key, NULL, 0);
		if (error != 0)
			goto done;
		extent->refs = htole64(refs + 1);
		error = btrfs_replace_item(handle, root, &extent_key, payload,
		    size);
		goto done;
	}

	if (found_inline) {
		memmove((uint8_t *)inline_ref + i,
		    (uint8_t *)inline_ref + i + sizeof(*inline_ref),
		    remain - i - sizeof(*inline_ref));
		size -= sizeof(*inline_ref);
	} else {
		error = btrfs_delete_item(handle, root, &backref_key);
		if (error != 0)
			goto done;
	}
	if (refs == 1) {
		error = btrfs_delete_item(handle, root, &extent_key);
		if (error == 0)
			error = btrfs_space_pin(handle, ref->bdr_bytenr,
			    letoh32(bmp->bm_super.nodesize));
		if (error == 0)
			error = btrfs_update_free_space(handle, ref->bdr_bytenr,
			    letoh32(bmp->bm_super.nodesize), 1);
	} else {
		extent->refs = htole64(refs - 1);
		error = btrfs_replace_item(handle, root, &extent_key, payload,
		    size);
	}
	goto done;

out:
	btrfs_release_path(&path);
done:
	if (payload != NULL)
		free(payload, M_BTRFS, alloc_size);
	return (error);
}

int
btrfs_run_delayed_refs(struct btrfs_trans_handle *handle)
{
	struct btrfs_transaction *trans;
	struct btrfs_delayed_tree_ref *ref;
	int error;

	if (handle == NULL || !handle->bth_commit)
		return (EINVAL);
	trans = handle->bth_transaction;
	for (;;) {
		mtx_enter(&trans->bt_lock);
		ref = TAILQ_FIRST(&trans->bt_delayed_tree_refs);
		if (ref != NULL)
			TAILQ_REMOVE(&trans->bt_delayed_tree_refs, ref,
			    bdr_entry);
		mtx_leave(&trans->bt_lock);
		if (ref == NULL)
			return (btrfs_run_delayed_data_refs(handle));
		error = btrfs_materialize_tree_ref(handle, ref);
		free(ref, M_BTRFS, sizeof(*ref));
		if (error != 0) {
			btrfs_trans_abort(handle, error);
			return (error);
		}
	}
}

int
btrfs_prepare_metadata_commit(struct btrfs_trans_handle *handle)
{
	struct btrfs_transaction *trans;
	uint64_t space_seq;
	unsigned int pass;
	int error, pending;

	if (handle == NULL || !handle->bth_commit)
		return (EINVAL);
	trans = handle->bth_transaction;
	for (pass = 0; pass < BTRFS_MAX_LEVEL * 4; pass++) {
		error = btrfs_run_delayed_refs(handle);
		if (error != 0)
			return (error);
		error = btrfs_space_release_discarded(handle);
		if (error != 0) {
			btrfs_trans_abort(handle, error);
			return (error);
		}
		mtx_enter(&trans->bt_lock);
		space_seq = trans->bt_space_seq;
		mtx_leave(&trans->bt_lock);

		error = btrfs_update_space_items(handle);
		if (error == 0)
			error = btrfs_update_dirty_root_items(handle);
		if (error != 0) {
			btrfs_trans_abort(handle, error);
			return (error);
		}

		mtx_enter(&trans->bt_lock);
		pending = !TAILQ_EMPTY(&trans->bt_delayed_tree_refs) ||
		    !TAILQ_EMPTY(&trans->bt_delayed_data_refs) ||
		    space_seq != trans->bt_space_seq;
		mtx_leave(&trans->bt_lock);
		if (!pending)
			return (0);
	}
	btrfs_trans_abort(handle, ELOOP);
	return (ELOOP);
}

static int
btrfs_sync_device(struct btrfs_fs *bmp, struct proc *p)
{
	int error, flush_error, force = 1;

	vn_lock(bmp->bm_devvp, LK_EXCLUSIVE | LK_RETRY);
	error = VOP_FSYNC(bmp->bm_devvp, FSCRED, MNT_WAIT, p);
	flush_error = VOP_IOCTL(bmp->bm_devvp, DIOCCACHESYNC, &force,
	    FWRITE, FSCRED, p);
	VOP_UNLOCK(bmp->bm_devvp);
	if (error == 0)
		error = flush_error;
	return (error);
}

int
btrfs_trans_commit(struct btrfs_fs *bmp, uint64_t minimum_generation,
    struct proc *p)
{
	struct btrfs_super_block *super = NULL;
	struct btrfs_trans_handle *handle = NULL;
	struct btrfs_transaction *trans = NULL;
	unsigned int i;
	int end_error, error, finish_error, write_error;

	if (bmp == NULL || p == NULL)
		return (EINVAL);
	error = btrfs_trans_close(bmp, minimum_generation, &trans);
	if (error != 0 || trans == NULL)
		return (error);

	if (trans->bt_error != 0)
		error = trans->bt_error;
	if (error == 0)
		error = btrfs_trans_commit_handle(trans, &handle);
	if (error == 0)
		error = btrfs_write_ordered_extents(trans);
	if (error != 0 && handle != NULL)
		btrfs_trans_abort(handle, error);
	if (error == 0)
		error = btrfs_prepare_metadata_commit(handle);
	if (handle != NULL) {
		end_error = btrfs_trans_end(handle);
		handle = NULL;
		if (error == 0)
			error = end_error;
	}
	if (error == 0)
		error = btrfs_write_dirty_metadata(trans);
	if (error == 0)
		error = btrfs_sync_device(bmp, p);
	if (error == 0) {
		super = malloc(sizeof(*super), M_BTRFS, M_WAITOK);
		error = btrfs_build_super(trans, super);
	}
	if (error == 0) {
		write_error = btrfs_write_super_mirrors(bmp, super);
		end_error = btrfs_sync_device(bmp, p);
		error = write_error != 0 ? write_error : end_error;
	}
	if (error == 0) {
		memcpy(&bmp->bm_super, super, sizeof(bmp->bm_super));
		bmp->bm_backup_roots_valid =
		    btrfs_validate_backup_roots(&bmp->bm_super);
		for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
			if (!btrfs_super_mirror_writable(bmp, i))
				continue;
			bmp->bm_super_mirrors[i].bsm_generation =
			    trans->bt_generation;
			bmp->bm_super_mirrors[i].bsm_flags |=
			    BTRFS_SUPER_MIRROR_VALID |
			    BTRFS_SUPER_MIRROR_CONSISTENT;
			bmp->bm_super_mirrors[i].bsm_flags &=
			    ~BTRFS_SUPER_MIRROR_STALE;
		}
	}
	if (super != NULL)
		free(super, M_BTRFS, sizeof(*super));

	finish_error = btrfs_trans_finish(bmp, trans, error);
	if (error == 0)
		error = finish_error;
	return (error);
}

int
btrfs_delayed_refs_finish(struct btrfs_transaction *trans, int committed)
{
	struct btrfs_delayed_tree_ref *ref;

	KASSERT(trans->bt_writers == 0);
	KASSERT(!trans->bt_commit_handle);
	if (committed && !TAILQ_EMPTY(&trans->bt_delayed_tree_refs))
		return (EBUSY);
	if (committed && !TAILQ_EMPTY(&trans->bt_delayed_data_refs))
		return (EBUSY);

	while ((ref = TAILQ_FIRST(&trans->bt_delayed_tree_refs)) != NULL) {
		TAILQ_REMOVE(&trans->bt_delayed_tree_refs, ref, bdr_entry);
		free(ref, M_BTRFS, sizeof(*ref));
	}
	return (btrfs_delayed_data_refs_finish(trans, committed));
}

int
btrfs_trans_close(struct btrfs_fs *bmp, uint64_t minimum_generation,
    struct btrfs_transaction **transp)
{
	struct btrfs_transaction *trans;

	*transp = NULL;
	mtx_enter(&bmp->bm_trans_mtx);
	for (;;) {
		if (bmp->bm_last_transid >= minimum_generation) {
			mtx_leave(&bmp->bm_trans_mtx);
			return (0);
		}
		trans = bmp->bm_transaction;
		KASSERT(trans != NULL);
		if (!bmp->bm_committer)
			break;
		msleep(&bmp->bm_transaction, &bmp->bm_trans_mtx, PWAIT,
		    "btrclose", 0);
	}
	if (minimum_generation > trans->bt_generation) {
		mtx_leave(&bmp->bm_trans_mtx);
		return (EINVAL);
	}
	bmp->bm_committer = 1;
	if (trans->bt_state == BTRFS_TRANS_OPEN)
		trans->bt_state = BTRFS_TRANS_CLOSING;
	while (trans->bt_writers != 0)
		msleep(&trans->bt_writers, &bmp->bm_trans_mtx, PWAIT,
		    "btrwr", 0);
	/*
	 * An idle sync has no new root-tree generation to publish.  Joins are
	 * closed and all handles have drained, so these lists are stable.
	 * Retain the open generation and its reserve for the next mutation.
	 */
	if (trans->bt_error == 0 &&
	    TAILQ_EMPTY(&trans->bt_dirty_extent_buffers)) {
		KASSERT(TAILQ_EMPTY(&trans->bt_dirty_roots));
		KASSERT(TAILQ_EMPTY(&trans->bt_delayed_tree_refs));
		KASSERT(TAILQ_EMPTY(&trans->bt_delayed_data_refs));
		KASSERT(TAILQ_EMPTY(&trans->bt_ordered_extents));
		KASSERT(TAILQ_EMPTY(&trans->bt_allocated_extents));
		KASSERT(TAILQ_EMPTY(&trans->bt_pinned_extents));
		trans->bt_state = BTRFS_TRANS_OPEN;
		bmp->bm_committer = 0;
		wakeup(&bmp->bm_transaction);
		mtx_leave(&bmp->bm_trans_mtx);
		return (0);
	}
	if (trans->bt_error != 0)
		trans->bt_state = BTRFS_TRANS_ABORTED;
	else
		trans->bt_state = BTRFS_TRANS_COMMITTING;
	*transp = trans;
	mtx_leave(&bmp->bm_trans_mtx);
	return (0);
}

int
btrfs_trans_finish(struct btrfs_fs *bmp,
    struct btrfs_transaction *trans, int error)
{
	struct btrfs_transaction *next = NULL;
	uint64_t generation;
	int reserve_error = 0;

	mtx_enter(&bmp->bm_trans_mtx);
	if (bmp->bm_transaction != trans || !bmp->bm_committer ||
	    trans->bt_writers != 0 ||
	    trans->bt_commit_handle ||
	    (trans->bt_state != BTRFS_TRANS_COMMITTING &&
	    trans->bt_state != BTRFS_TRANS_ABORTED)) {
		mtx_leave(&bmp->bm_trans_mtx);
		return (EINVAL);
	}
	mtx_leave(&bmp->bm_trans_mtx);

	if (error == 0 && trans->bt_error != 0)
		error = trans->bt_error;
	if (error == 0 && trans->bt_generation == UINT64_MAX)
		error = EOVERFLOW;
	if (error == 0)
		error = btrfs_delayed_refs_finish(trans, 1);
	if (error == 0)
		error = btrfs_ordered_extents_finish(trans, 1);
	if (error == 0)
		error = btrfs_extent_buffers_finish(trans, 1);
	if (error == 0) {
		(void)btrfs_roots_finish(trans, 1);
		generation = trans->bt_generation + 1;
		btrfs_space_commit(trans);
		next = btrfs_trans_alloc(bmp, generation);
		reserve_error = btrfs_space_reserve_commit(next);
		if (reserve_error != 0) {
			next->bt_error = reserve_error;
			next->bt_state = BTRFS_TRANS_ABORTED;
		}
	} else {
		(void)btrfs_roots_finish(trans, 0);
		(void)btrfs_delayed_refs_finish(trans, 0);
		(void)btrfs_ordered_extents_finish(trans, 0);
		(void)btrfs_extent_buffers_finish(trans, 0);
		btrfs_space_abort(trans);
	}

	mtx_enter(&bmp->bm_trans_mtx);
	KASSERT(bmp->bm_transaction == trans);
	KASSERT(bmp->bm_committer);
	KASSERT(trans->bt_writers == 0);
	if (error == 0) {
		trans->bt_state = BTRFS_TRANS_COMMITTED;
		bmp->bm_last_transid = trans->bt_generation;
		bmp->bm_transaction = next;
		if (reserve_error != 0)
			btrfs_fs_set_readonly(bmp);
	} else {
		if (trans->bt_error == 0)
			trans->bt_error = error;
		trans->bt_state = BTRFS_TRANS_ABORTED;
		btrfs_fs_set_readonly(bmp);
	}
	bmp->bm_committer = 0;
	wakeup(&bmp->bm_transaction);
	mtx_leave(&bmp->bm_trans_mtx);

	if (error == 0)
		free(trans, M_BTRFS, sizeof(*trans));
	return (error);
}
