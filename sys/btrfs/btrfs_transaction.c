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
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/mount.h>

#include <btrfs/btrfs_var.h>

static struct btrfs_transaction *
	btrfs_trans_alloc(struct btrfs_mount *, uint64_t);

static struct btrfs_transaction *
btrfs_trans_alloc(struct btrfs_mount *bmp, uint64_t generation)
{
	struct btrfs_transaction *trans;

	trans = malloc(sizeof(*trans), M_BTRFS, M_WAITOK | M_ZERO);
	trans->bt_mount = bmp;
	trans->bt_generation = generation;
	trans->bt_state = BTRFS_TRANS_OPEN;
	mtx_init(&trans->bt_lock, IPL_NONE);
	TAILQ_INIT(&trans->bt_commit_reservations);
	TAILQ_INIT(&trans->bt_allocated_extents);
	TAILQ_INIT(&trans->bt_pinned_extents);
	TAILQ_INIT(&trans->bt_dirty_extent_buffers);
	TAILQ_INIT(&trans->bt_delayed_tree_refs);
	TAILQ_INIT(&trans->bt_dirty_roots);
	return (trans);
}

int
btrfs_trans_init(struct btrfs_mount *bmp)
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
		if ((bmp->bm_mount->mnt_flag & MNT_RDONLY) == 0) {
			free(bmp->bm_transaction, M_BTRFS,
			    sizeof(*bmp->bm_transaction));
			bmp->bm_transaction = NULL;
			return (EOVERFLOW);
		}
	}
	if ((bmp->bm_mount->mnt_flag & MNT_RDONLY) == 0) {
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
btrfs_trans_destroy(struct btrfs_mount *bmp)
{
	struct btrfs_transaction *trans = bmp->bm_transaction;

	if (trans == NULL)
		return;
	KASSERT(trans->bt_writers == 0);
	KASSERT(!bmp->bm_committer);
	KASSERT(!trans->bt_commit_handle);
	(void)btrfs_roots_finish(trans, 0);
	(void)btrfs_delayed_refs_finish(trans, 0);
	(void)btrfs_extent_buffers_finish(trans, 0);
	KASSERT(TAILQ_EMPTY(&trans->bt_dirty_roots));
	KASSERT(TAILQ_EMPTY(&trans->bt_delayed_tree_refs));
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
btrfs_trans_join(struct btrfs_mount *bmp,
    const struct btrfs_trans_reservation *reservation,
    struct btrfs_trans_handle **handlep)
{
	struct btrfs_trans_handle *handle;
	struct btrfs_transaction *trans;
	int error;

	*handlep = NULL;
	if (bmp->bm_mount->mnt_flag & MNT_RDONLY)
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
				bmp->bm_mount->mnt_flag |= MNT_RDONLY;
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
		(void)btrfs_trans_end(handle);
		return (error);
	}
	*handlep = handle;
	return (0);
}

int
btrfs_trans_commit_handle(struct btrfs_transaction *trans,
    struct btrfs_trans_handle **handlep)
{
	struct btrfs_mount *bmp = trans->bt_mount;
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
	struct btrfs_mount *bmp = trans->bt_mount;
	int error;

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
	struct btrfs_mount *bmp = trans->bt_mount;

	if (error == 0)
		error = EIO;
	mtx_enter(&bmp->bm_trans_mtx);
	KASSERT(trans == bmp->bm_transaction);
	if (trans->bt_error == 0)
		trans->bt_error = error;
	trans->bt_state = BTRFS_TRANS_ABORTED;
	bmp->bm_mount->mnt_flag |= MNT_RDONLY;
	wakeup(&bmp->bm_transaction);
	mtx_leave(&bmp->bm_trans_mtx);
}

int
btrfs_delayed_ref_add(struct btrfs_trans_handle *handle, uint64_t bytenr,
    uint64_t parent, uint64_t root, uint8_t level, int ref_mod)
{
	struct btrfs_delayed_tree_ref *ref, *new;
	struct btrfs_transaction *trans;
	struct btrfs_mount *bmp;
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
	return (0);
}

int
btrfs_delayed_refs_finish(struct btrfs_transaction *trans, int committed)
{
	struct btrfs_delayed_tree_ref *ref;

	KASSERT(trans->bt_writers == 0);
	KASSERT(!trans->bt_commit_handle);
	if (committed && !TAILQ_EMPTY(&trans->bt_delayed_tree_refs))
		return (EBUSY);

	while ((ref = TAILQ_FIRST(&trans->bt_delayed_tree_refs)) != NULL) {
		TAILQ_REMOVE(&trans->bt_delayed_tree_refs, ref, bdr_entry);
		free(ref, M_BTRFS, sizeof(*ref));
	}
	return (0);
}

int
btrfs_trans_close(struct btrfs_mount *bmp, uint64_t minimum_generation,
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
	if (trans->bt_error != 0)
		trans->bt_state = BTRFS_TRANS_ABORTED;
	else
		trans->bt_state = BTRFS_TRANS_COMMITTING;
	*transp = trans;
	mtx_leave(&bmp->bm_trans_mtx);
	return (0);
}

int
btrfs_trans_finish(struct btrfs_mount *bmp,
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
			bmp->bm_mount->mnt_flag |= MNT_RDONLY;
	} else {
		if (trans->bt_error == 0)
			trans->bt_error = error;
		trans->bt_state = BTRFS_TRANS_ABORTED;
		bmp->bm_mount->mnt_flag |= MNT_RDONLY;
	}
	bmp->bm_committer = 0;
	wakeup(&bmp->bm_transaction);
	mtx_leave(&bmp->bm_trans_mtx);

	if (error == 0)
		free(trans, M_BTRFS, sizeof(*trans));
	return (error);
}
