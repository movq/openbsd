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

/*
 * Writers join the filesystem's single open transaction with typed,
 * worst-case reservations acquired before visible mutation. Capacity failures
 * must leave namespace and inode state unchanged so later operations can
 * proceed. Errors after partial tree mutation abort the transaction and make
 * every mount view read-only.
 *
 * Before ending a handle, encode affected inodes and attach owned data
 * payloads. Ending a handle does not commit. A committer closes joins and
 * drains handles; new writers wait for publication. Commit must never acquire
 * arbitrary vnode locks, since callers may retain them while joining or
 * requesting a commit. Sync commits the full transaction; fsync may publish
 * a log while retaining the open transaction and its allocations.
 *
 * Abort restores saved roots and marks transaction-owned extent buffers stale
 * before releasing new allocations. Committed metadata is never overwritten,
 * and freed extents remain pinned until durable publication. Overlapping
 * transactions would require root versioning and per-generation ownership of
 * pins, ordered data, and extent buffers.
 *
 * Durability changes need independent checks of unmounted filesystems and
 * data, followed by remount verification (see regress/sys/btrfs/README).
 * Recovery also needs reservation exhaustion and fault injection around data,
 * metadata, cache barriers, and superblock mirrors: either old or new committed
 * state is valid, but mixed generations are not.
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
#include <sys/stat.h>
#include <sys/vnode.h>

#include <btrfs/btrfs_var.h>

static struct btrfs_transaction *
	btrfs_trans_alloc(struct btrfs_fs *, uint64_t);

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
	RBT_INIT(btrfs_tree_ref_tree, &trans->bt_tree_ref_index);
	RBT_INIT(btrfs_data_ref_tree, &trans->bt_data_ref_index);
	RBT_INIT(btrfs_ordered_tree, &trans->bt_ordered_extents);
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
	btrfs_log_destroy(bmp);
	(void)btrfs_roots_finish(trans, 0);
	(void)btrfs_delayed_refs_finish(trans, 0);
	(void)btrfs_ordered_extents_finish(trans, 0);
	(void)btrfs_extent_buffers_finish(trans, 0);
	KASSERT(TAILQ_EMPTY(&trans->bt_dirty_roots));
	KASSERT(TAILQ_EMPTY(&trans->bt_delayed_tree_refs));
	KASSERT(TAILQ_EMPTY(&trans->bt_delayed_data_refs));
	KASSERT(RBT_EMPTY(btrfs_ordered_tree, &trans->bt_ordered_extents));
	KASSERT(TAILQ_EMPTY(&trans->bt_dirty_extent_buffers));
	if (trans->bt_state != BTRFS_TRANS_COMMITTED) {
		btrfs_chunk_abort(trans);
		btrfs_space_abort(trans);
	}
	KASSERT(trans->bt_chunk_op == NULL);
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
	uint64_t generation, failed_type, needed;
	int dirty, end_error, error, retried = 0, grown = 0;

	*handlep = NULL;
retry:
	if (bmp->bm_readonly)
		return (EROFS);
	handle = malloc(sizeof(*handle), M_BTRFS, M_WAITOK | M_ZERO);
	TAILQ_INIT(&handle->bth_reservations);

	mtx_enter(&bmp->bm_trans_mtx);
	for (;;) {
		while (bmp->bm_control != NULL && bmp->bm_control != curproc)
			msleep(&bmp->bm_control, &bmp->bm_trans_mtx, PWAIT,
			    "btrctl", 0);
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
	mtx_enter(&trans->bt_lock);
	dirty = trans->bt_ordered_bytes >= BTRFS_ORDERED_BYTES_MAX;
	mtx_leave(&trans->bt_lock);
	if (dirty) {
		generation = trans->bt_generation;
		mtx_leave(&bmp->bm_trans_mtx);
		free(handle, M_BTRFS, sizeof(*handle));
		error = btrfs_trans_commit(bmp, generation, curproc);
		if (error != 0)
			return (error);
		goto retry;
	}
	KASSERT(trans->bt_writers != UINT_MAX);
	trans->bt_writers++;
	handle->bth_transaction = trans;
	mtx_leave(&bmp->bm_trans_mtx);

	error = btrfs_space_reserve(handle, reservation);
	if (error != 0) {
		failed_type = handle->bth_failed_type;
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
		    !reservation->btr_chunk && grown < 3 && failed_type != 0) {
			if (failed_type == BTRFS_BLOCK_GROUP_DATA)
				needed = reservation->btr_data;
			else if (failed_type == BTRFS_BLOCK_GROUP_SYSTEM)
				needed = reservation->btr_system;
			else {
				needed = reservation->btr_metadata;
				if (letoh64(bmp->bm_super.compat_ro_flags) &
				    BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE)
					needed *= 2;
			}
			error = btrfs_chunk_grow(bmp, failed_type, needed);
			if (error != 0)
				return (error);
			grown++;
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

/*
 * Reference materialization, block-group accounting and root-item updates
 * can COW more trees and queue more references. Reach a fixed point before
 * writing metadata, including allocator change sequence in the stability
 * check even when the delayed-reference queues appear empty.
 */
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

/* bwrite and device VOP_FSYNC alone do not flush volatile device caches. */
int
btrfs_sync_device(struct btrfs_fs *bmp, struct proc *p)
{
	struct vnode *vp;
	unsigned int i;
	int error, flush_error, first_error = 0, force = 1;

	/* Complete every member's barrier, including after a write error. */
	for (i = 0; i < bmp->bm_ndevices; i++) {
		vp = bmp->bm_devices[i].bd_devvp;
		vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
		error = VOP_FSYNC(vp, FSCRED, MNT_WAIT, p);
		flush_error = VOP_IOCTL(vp, DIOCCACHESYNC, &force,
		    FWRITE, FSCRED, p);
		VOP_UNLOCK(vp);
		if (error == 0)
			error = flush_error;
		if (first_error == 0)
			first_error = error;
	}
	return (first_error);
}

/*
 * The highest valid superblock generation on any member is the commit point.
 * First submit ordered data, insert its checksums, and wait for every
 * required mirror.
 * Drain metadata accounting to a fixed point, finalize metadata headers and
 * checksums, then submit and wait for all metadata mirrors. Drain device
 * buffers and cache-sync every member before writing any superblock. Write
 * device-specific superblock mirrors within each recorded member size.
 * A second drain/cache-sync on every member precedes in-memory publication
 * and release of pinned space. Every DUP copy is required.
 *
 * Once superblock writing is attempted, attempt the final barrier even if a
 * mirror failed. Ambiguous publication requires an error and read-only views.
 */
int
btrfs_trans_commit(struct btrfs_fs *bmp, uint64_t minimum_generation,
    struct proc *p)
{
	struct btrfs_super_block *super = NULL;
	struct btrfs_trans_handle *handle = NULL;
	struct btrfs_transaction *trans = NULL;
	struct btrfs_super_mirror *mirror;
	unsigned int d, i;
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
		error = btrfs_write_ordered_extents(handle);
	if (error == 0)
		error = btrfs_coalesce_ordered_extents(handle);
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
		for (d = 0; d < bmp->bm_ndevices; d++) {
			for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
				if (!btrfs_super_mirror_writable(
				    &bmp->bm_devices[d], i))
					continue;
				mirror = &bmp->bm_devices[d].bd_mirrors[i];
				mirror->bsm_generation = trans->bt_generation;
				mirror->bsm_flags |= BTRFS_SUPER_MIRROR_VALID |
				    BTRFS_SUPER_MIRROR_CONSISTENT;
				mirror->bsm_flags &=
				    ~(BTRFS_SUPER_MIRROR_STALE |
				    BTRFS_SUPER_MIRROR_FOREIGN);
			}
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
		KASSERT(RBT_EMPTY(btrfs_ordered_tree,
		    &trans->bt_ordered_extents));
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
		btrfs_log_destroy(bmp);
		btrfs_chunk_publish(trans);
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
		btrfs_chunk_abort(trans);
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

/*
 * Use the same close/drain protocol as commit. No vnode other than the
 * caller's is acquired. A successful log publication reopens this generation;
 * a fallback reopens it before entering the ordinary full commit path.
 */
int
btrfs_log_fsync(struct btrfs_node *node, uint64_t generation, struct proc *p)
{
	struct btrfs_fs *bmp = node->bn_mount;
	struct btrfs_transaction *trans;
	struct btrfs_trans_handle *handle = NULL;
	int error, end_error;

	if (IFTOVT(node->bn_inode.bi_mode) != VREG)
		return (btrfs_trans_commit(bmp, generation, p));
	error = btrfs_trans_close(bmp, generation, &trans);
	if (error != 0 || trans == NULL)
		return (error);
	if (trans->bt_error != 0)
		return (btrfs_trans_finish(bmp, trans, trans->bt_error));
	if (trans->bt_log_full_commit || trans->bt_chunk_op != NULL ||
	    node->bn_inode.bi_generation > bmp->bm_last_transid ||
	    node->bn_inode.bi_nlink == 0)
		error = EAGAIN;
	else {
		error = btrfs_trans_commit_handle(trans, &handle);
		if (error == 0)
			error = btrfs_log_write(handle, node, p);
		if (handle != NULL) {
			end_error = btrfs_trans_end(handle);
			if (end_error != 0)
				error = end_error;
		}
	}
	if (error != 0 && error != EAGAIN)
		return (btrfs_trans_finish(bmp, trans, error));
	mtx_enter(&bmp->bm_trans_mtx);
	trans->bt_state = BTRFS_TRANS_OPEN;
	bmp->bm_committer = 0;
	wakeup(&bmp->bm_transaction);
	mtx_leave(&bmp->bm_trans_mtx);
	if (error == EAGAIN)
		return (btrfs_trans_commit(bmp, generation, p));
	return (0);
}
