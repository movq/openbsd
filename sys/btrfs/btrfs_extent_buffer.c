/* Copyright (C) 2026 Mike Jones <mike@mjones.org>
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Device buffers supply physical reads; extent buffers supply logical metadata
 * identity, validation, locking and transaction ownership. Loading releases
 * device buffers and retains private bytes. An address index covers active
 * and cached blocks, with idle validated blocks kept in a bounded 8 MiB LRU.
 * Hits must match generation, level and permitted owner, and validated child
 * generations must fit the caller's view. File-tree blocks may retain another
 * subvolume's on-disk owner; sharing is permitted only between file trees.
 *
 * Allocator-authorized reuse may evict an idle old generation, never an active
 * reference. Aborted blocks are discarded. Last-view teardown purges this
 * cache after transactions, then destroys the per-filesystem metadata-node
 * and MAXBSIZE scratch pools. The pools have no cache identity; their idle
 * high-water marks are 64 and 16 objects respectively.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>


#include <btrfs/btrfs_var.h>

/* Bound idle metadata independently of transaction-owned and active blocks. */
#define BTRFS_METADATA_CACHE_BYTES	(8 * 1024 * 1024)

static int	btrfs_extent_buffer_load(const struct btrfs_root *,
		    struct btrfs_extent_buffer *, uint64_t);
static const void *
		btrfs_extent_buffer_bytes(const struct btrfs_extent_buffer *);
static void	btrfs_extent_buffer_fail_transaction(
		    struct btrfs_transaction *, int);
static int	btrfs_extent_buffer_validate(const void *, size_t, void *);
static int	btrfs_validate_tree_block(const struct btrfs_super_block *,
		    const struct btrfs_header *, uint64_t, uint64_t, uint64_t,
		    uint64_t, uint8_t);
static int	btrfs_key_cmp(const struct btrfs_key *,
		    const struct btrfs_key *);

struct btrfs_extent_buffer_validation {
	const struct btrfs_super_block	*ebv_super;
	uint64_t			 ebv_bytenr;
	uint64_t			 ebv_generation;
	uint64_t			 ebv_view_generation;
	uint64_t			 ebv_owner;
	uint8_t				 ebv_level;
};

static inline int
btrfs_extent_buffer_compare(const struct btrfs_extent_buffer *a,
    const struct btrfs_extent_buffer *b)
{
	if (a->eb_bytenr < b->eb_bytenr)
		return (-1);
	if (a->eb_bytenr > b->eb_bytenr)
		return (1);
	return (0);
}

RBT_GENERATE(btrfs_extent_buffer_tree, btrfs_extent_buffer, eb_entry,
    btrfs_extent_buffer_compare);

static void
btrfs_extent_buffer_free(struct btrfs_extent_buffer *eb)
{
	if (eb == NULL)
		return;
	rw_assert_unlocked(&eb->eb_lock);
	if (eb->eb_buf != NULL)
		brelse(eb->eb_buf);
	if (eb->eb_private != NULL)
		pool_put(&eb->eb_mount->bm_metadata_pool, eb->eb_private);
	free(eb, M_BTRFS, sizeof(*eb));
}

/* The caller excludes new users with bm_ebmtx. */
static void
btrfs_extent_buffer_uncache(struct btrfs_fs *bmp,
    struct btrfs_extent_buffer *eb)
{
	MUTEX_ASSERT_LOCKED(&bmp->bm_ebmtx);
	KASSERT(eb->eb_refs == 0);
	KASSERT(eb->eb_transaction == NULL);
	TAILQ_REMOVE(&bmp->bm_eb_lru, eb, eb_lru);
	bmp->bm_eb_cached--;
	RBT_REMOVE(btrfs_extent_buffer_tree, &bmp->bm_extent_buffers, eb);
}

/*
 * An allocator-authorized reuse supersedes an idle old generation. Live
 * references still exclude reuse, just as they did before clean caching.
 */
static int
btrfs_extent_buffer_publish(struct btrfs_extent_buffer *eb)
{
	struct btrfs_fs *bmp = eb->eb_mount;
	struct btrfs_extent_buffer *old, *collision;

	mtx_enter(&bmp->bm_ebmtx);
	old = RBT_FIND(btrfs_extent_buffer_tree, &bmp->bm_extent_buffers, eb);
	if (old != NULL) {
		if (old->eb_refs != 0) {
			mtx_leave(&bmp->bm_ebmtx);
			return (EINVAL);
		}
		btrfs_extent_buffer_uncache(bmp, old);
	}
	collision = RBT_INSERT(btrfs_extent_buffer_tree,
	    &bmp->bm_extent_buffers, eb);
	KASSERT(collision == NULL);
	mtx_leave(&bmp->bm_ebmtx);
	btrfs_extent_buffer_free(old);
	return (0);
}

void
btrfs_extent_buffers_purge(struct btrfs_fs *bmp)
{
	struct btrfs_extent_buffer *eb;

	/* Mount failure or last-view teardown: no callers or transactions remain. */
	while ((eb = TAILQ_FIRST(&bmp->bm_eb_lru)) != NULL) {
		mtx_enter(&bmp->bm_ebmtx);
		btrfs_extent_buffer_uncache(bmp, eb);
		mtx_leave(&bmp->bm_ebmtx);
		btrfs_extent_buffer_free(eb);
	}
	KASSERT(RBT_EMPTY(btrfs_extent_buffer_tree, &bmp->bm_extent_buffers));
	KASSERT(bmp->bm_eb_cached == 0);
}

static int
btrfs_extent_buffer_matches(const struct btrfs_extent_buffer *eb,
    uint64_t generation, uint64_t owner, uint8_t level)
{
	return (eb->eb_generation == generation &&
	    (eb->eb_owner == owner ||
	    (btrfs_file_tree(eb->eb_owner) && btrfs_file_tree(owner))) &&
	    eb->eb_level == level);
}

static const void *
btrfs_extent_buffer_bytes(const struct btrfs_extent_buffer *eb)
{
	if (eb->eb_private != NULL)
		return (eb->eb_private);
	KASSERT(eb->eb_buf != NULL);
	return (eb->eb_buf->b_data);
}

int
btrfs_extent_buffer_read(const struct btrfs_root *root, uint64_t logical,
    uint64_t generation, uint64_t view_generation, uint8_t level,
    struct btrfs_extent_buffer **ebp)
{
	struct btrfs_extent_buffer *eb, *new = NULL, key = { 0 }, *collision;
	struct btrfs_fs *bmp = root->br_mount;
	uint32_t sectorsize;
	int error;

	*ebp = NULL;
	sectorsize = letoh32(root->br_super->sectorsize);
	if (logical == 0 || (logical & (sectorsize - 1)) != 0 ||
	    generation == 0 || generation > view_generation ||
	    level >= BTRFS_MAX_LEVEL)
		return (EINVAL);

#ifdef DIAGNOSTIC
	if (bmp != NULL) {
		KASSERT(root->br_devvp == bmp->bm_devvp);
		KASSERT(root->br_super == &bmp->bm_super);
		KASSERT(root->br_bootstrap_chunks == NULL);
		KASSERT(root->br_bootstrap_nchunks == 0);
	}
#endif

	key.eb_bytenr = logical;
again:
	if (bmp != NULL) {
		mtx_enter(&bmp->bm_ebmtx);
		eb = RBT_FIND(btrfs_extent_buffer_tree,
		    &bmp->bm_extent_buffers, &key);
		if (eb != NULL) {
			if (!btrfs_extent_buffer_matches(eb, generation,
			    root->br_owner, level)) {
				if (eb->eb_refs == 0) {
					btrfs_extent_buffer_uncache(bmp, eb);
					mtx_leave(&bmp->bm_ebmtx);
					btrfs_extent_buffer_free(eb);
					goto again;
				}
				mtx_leave(&bmp->bm_ebmtx);
				if (new != NULL) {
					rw_exit_write(&new->eb_lock);
					btrfs_extent_buffer_free(new);
				}
				return (EINVAL);
			}
			if (eb->eb_refs == 0) {
				TAILQ_REMOVE(&bmp->bm_eb_lru, eb, eb_lru);
				bmp->bm_eb_cached--;
			}
			KASSERT(eb->eb_refs != UINT_MAX);
			eb->eb_refs++;
			mtx_leave(&bmp->bm_ebmtx);
			if (new != NULL) {
				rw_exit_write(&new->eb_lock);
				btrfs_extent_buffer_free(new);
			}

			rw_enter_read(&eb->eb_lock);
			KASSERT(eb->eb_loaded);
			if (eb->eb_error != 0 || eb->eb_stale ||
			    eb->eb_max_generation > view_generation) {
				error = eb->eb_error != 0 ? eb->eb_error :
				    eb->eb_stale ? EIO : EINVAL;
				btrfs_extent_buffer_put(eb);
				return (error);
			}
			*ebp = eb;
			return (0);
		}
		if (new != NULL) {
			collision = RBT_INSERT(btrfs_extent_buffer_tree,
			    &bmp->bm_extent_buffers, new);
			KASSERT(collision == NULL);
		}
		mtx_leave(&bmp->bm_ebmtx);
	}
	if (new == NULL) {
		new = malloc(sizeof(*new), M_BTRFS, M_WAITOK | M_ZERO);
		new->eb_mount = bmp;
		new->eb_bytenr = logical;
		new->eb_generation = generation;
		new->eb_owner = root->br_owner;
		new->eb_level = level;
		new->eb_refs = 1;
		rw_init_flags(&new->eb_lock, "btreebuf", RWL_DUPOK);
		rw_enter_write(&new->eb_lock);
		goto again;
	}

	error = btrfs_extent_buffer_load(root, new, view_generation);
	new->eb_error = error;
	new->eb_loaded = 1;
	rw_exit_write(&new->eb_lock);
	rw_enter_read(&new->eb_lock);
	if (error != 0) {
		btrfs_extent_buffer_put(new);
		return (error);
	}
	*ebp = new;
	return (0);
}

/*
 * Allocate a transaction-owned metadata block and seed its private bytes from
 * source.  This is deliberately below btrfs_cow_block(): callers must still
 * update the parent or root pointer and queue the corresponding delayed refs.
 */
int
btrfs_extent_buffer_clone(struct btrfs_trans_handle *handle,
    const struct btrfs_extent_buffer *source,
    struct btrfs_extent_buffer **ebp)
{
	struct btrfs_transaction *trans;
	struct btrfs_extent_buffer *eb;
	struct btrfs_fs *bmp;
	struct btrfs_header *header;
	uint64_t bytenr, flags;
	uint32_t nodesize;
	int error;

	if (ebp == NULL)
		return (EINVAL);
	*ebp = NULL;
	if (handle == NULL || source == NULL)
		return (EINVAL);
	trans = handle->bth_transaction;
	bmp = trans->bt_mount;
	nodesize = letoh32(bmp->bm_super.nodesize);

	rw_assert_wrlock((struct rwlock *)&source->eb_lock);
	if (source->eb_mount != bmp || !source->eb_loaded ||
	    source->eb_error != 0 || source->eb_stale ||
	    source->eb_transaction != NULL ||
	    source->eb_generation >= trans->bt_generation)
		return (EINVAL);

	eb = malloc(sizeof(*eb), M_BTRFS, M_WAITOK | M_ZERO);
	eb->eb_private = pool_get(&bmp->bm_metadata_pool, PR_WAITOK);
	memcpy(eb->eb_private, btrfs_extent_buffer_bytes(source), nodesize);
	rw_init_flags(&eb->eb_lock, "btreebuf", RWL_DUPOK);
	rw_enter_write(&eb->eb_lock);

	error = btrfs_space_alloc(handle,
	    source->eb_owner == BTRFS_CHUNK_TREE_OBJECTID ?
	    BTRFS_BLOCK_GROUP_SYSTEM : BTRFS_BLOCK_GROUP_METADATA,
	    nodesize, nodesize, &bytenr);
	if (error != 0) {
		rw_exit_write(&eb->eb_lock);
		pool_put(&bmp->bm_metadata_pool, eb->eb_private);
		free(eb, M_BTRFS, sizeof(*eb));
		return (error);
	}

	header = eb->eb_private;
	memset(header->csum, 0, sizeof(header->csum));
	header->bytenr = htole64(bytenr);
	header->generation = htole64(trans->bt_generation);
	flags = letoh64(header->flags);
	header->flags = htole64(flags & ~BTRFS_HEADER_FLAG_WRITTEN);

	eb->eb_mount = bmp;
	eb->eb_transaction = trans;
	eb->eb_bytenr = bytenr;
	eb->eb_generation = trans->bt_generation;
	eb->eb_max_generation = trans->bt_generation;
	eb->eb_owner = source->eb_owner;
	eb->eb_refs = 2;	/* caller plus transaction dirty-list ownership */
	eb->eb_level = source->eb_level;
	eb->eb_loaded = 1;
	eb->eb_dirty = 1;

	error = btrfs_extent_buffer_publish(eb);
	if (error != 0) {
		rw_exit_write(&eb->eb_lock);
		pool_put(&bmp->bm_metadata_pool, eb->eb_private);
		free(eb, M_BTRFS, sizeof(*eb));
		btrfs_trans_abort(handle, EINVAL);
		return (EINVAL);
	}

	mtx_enter(&trans->bt_lock);
	TAILQ_INSERT_TAIL(&trans->bt_dirty_extent_buffers, eb,
	    eb_dirty_entry);
	mtx_leave(&trans->bt_lock);
	*ebp = eb;
	return (0);
}

/*
 * Allocate an empty transaction-owned block using source only as a header
 * template.  Split siblings keep the same level, while root growth creates
 * the one permitted next level.
 */
int
btrfs_extent_buffer_alloc(struct btrfs_trans_handle *handle,
    const struct btrfs_extent_buffer *source, uint8_t level,
    struct btrfs_extent_buffer **ebp)
{
	struct btrfs_transaction *trans;
	struct btrfs_extent_buffer *eb;
	struct btrfs_fs *bmp;
	struct btrfs_header *header;
	const struct btrfs_header *source_header;
	uint64_t bytenr, flags;
	uint32_t nodesize;
	int error;

	if (ebp == NULL)
		return (EINVAL);
	*ebp = NULL;
	if (handle == NULL || handle->bth_transaction == NULL ||
	    source == NULL || level >= BTRFS_MAX_LEVEL)
		return (EINVAL);
	trans = handle->bth_transaction;
	bmp = trans->bt_mount;
	nodesize = letoh32(bmp->bm_super.nodesize);

	rw_assert_anylock((struct rwlock *)&source->eb_lock);
	if (source->eb_mount != bmp || !source->eb_loaded ||
	    source->eb_error != 0 || source->eb_stale ||
	    source->eb_transaction != trans ||
	    source->eb_generation != trans->bt_generation ||
	    source->eb_private == NULL || !source->eb_dirty ||
	    (level != source->eb_level && level != source->eb_level + 1))
		return (EINVAL);
	source_header = btrfs_extent_buffer_bytes(source);

	eb = malloc(sizeof(*eb), M_BTRFS, M_WAITOK | M_ZERO);
	eb->eb_private = pool_get(&bmp->bm_metadata_pool, PR_WAITOK | PR_ZERO);
	rw_init_flags(&eb->eb_lock, "btreebuf", RWL_DUPOK);
	rw_enter_write(&eb->eb_lock);

	error = btrfs_space_alloc(handle,
	    source->eb_owner == BTRFS_CHUNK_TREE_OBJECTID ?
	    BTRFS_BLOCK_GROUP_SYSTEM : BTRFS_BLOCK_GROUP_METADATA,
	    nodesize, nodesize, &bytenr);
	if (error != 0) {
		rw_exit_write(&eb->eb_lock);
		pool_put(&bmp->bm_metadata_pool, eb->eb_private);
		free(eb, M_BTRFS, sizeof(*eb));
		return (error);
	}

	header = eb->eb_private;
	memcpy(header, source_header, sizeof(*header));
	memset(header->csum, 0, sizeof(header->csum));
	header->bytenr = htole64(bytenr);
	header->generation = htole64(trans->bt_generation);
	flags = letoh64(header->flags);
	header->flags = htole64(flags & ~BTRFS_HEADER_FLAG_WRITTEN);
	header->nritems = htole32(0);
	header->level = level;

	eb->eb_mount = bmp;
	eb->eb_transaction = trans;
	eb->eb_bytenr = bytenr;
	eb->eb_generation = trans->bt_generation;
	eb->eb_max_generation = trans->bt_generation;
	eb->eb_owner = source->eb_owner;
	eb->eb_refs = 2;	/* caller plus transaction dirty-list ownership */
	eb->eb_level = level;
	eb->eb_loaded = 1;
	eb->eb_dirty = 1;

	error = btrfs_extent_buffer_publish(eb);
	if (error != 0) {
		rw_exit_write(&eb->eb_lock);
		pool_put(&bmp->bm_metadata_pool, eb->eb_private);
		free(eb, M_BTRFS, sizeof(*eb));
		btrfs_trans_abort(handle, EINVAL);
		return (EINVAL);
	}

	mtx_enter(&trans->bt_lock);
	TAILQ_INSERT_TAIL(&trans->bt_dirty_extent_buffers, eb,
	    eb_dirty_entry);
	mtx_leave(&trans->bt_lock);
	*ebp = eb;
	return (0);
}

/*
 * Consume the caller and dirty-list references to an unlinked COW block.
 * Its allocation remains held until delayed references have been drained.
 */
int
btrfs_extent_buffer_discard(struct btrfs_trans_handle *handle,
    struct btrfs_extent_buffer *eb)
{
	struct btrfs_transaction *trans;
	struct btrfs_fs *bmp;
	uint32_t nodesize;
	int error;

	if (handle == NULL || eb == NULL)
		return (EINVAL);
	trans = handle->bth_transaction;
	bmp = trans->bt_mount;
	nodesize = letoh32(bmp->bm_super.nodesize);
	rw_assert_wrlock(&eb->eb_lock);
	if (eb->eb_mount != bmp || eb->eb_transaction != trans ||
	    eb->eb_generation != trans->bt_generation ||
	    eb->eb_private == NULL || !eb->eb_dirty ||
	    eb->eb_writeback || eb->eb_written || eb->eb_stale ||
	    eb->eb_error != 0)
		return (EINVAL);

	error = btrfs_space_discard_alloc(handle, eb->eb_bytenr, nodesize);
	if (error != 0)
		return (error);

	mtx_enter(&trans->bt_lock);
	TAILQ_REMOVE(&trans->bt_dirty_extent_buffers, eb, eb_dirty_entry);
	mtx_leave(&trans->bt_lock);
	eb->eb_transaction = NULL;
	eb->eb_dirty = 0;
	eb->eb_stale = 1;
	eb->eb_error = ECANCELED;

	/* Drop transaction ownership while the caller's lock/reference remains. */
	mtx_enter(&bmp->bm_ebmtx);
	KASSERT(eb->eb_refs >= 2);
	eb->eb_refs--;
	mtx_leave(&bmp->bm_ebmtx);
	btrfs_extent_buffer_put(eb);
	return (0);
}

const void *
btrfs_extent_buffer_data(const struct btrfs_extent_buffer *eb)
{
#ifdef DIAGNOSTIC
	const struct btrfs_header *header;

	KASSERT(eb->eb_refs > 0);
	rw_assert_anylock((struct rwlock *)&eb->eb_lock);
	KASSERT(eb->eb_loaded);
	KASSERT(eb->eb_error == 0);
	KASSERT(!eb->eb_stale);
	KASSERT(eb->eb_buf != NULL || eb->eb_private != NULL);
	header = btrfs_extent_buffer_bytes(eb);
	KASSERT(letoh64(header->bytenr) == eb->eb_bytenr);
	KASSERT(letoh64(header->generation) == eb->eb_generation);
	KASSERT(letoh64(header->owner) == eb->eb_owner);
	KASSERT(header->level == eb->eb_level);
#endif
	return (btrfs_extent_buffer_bytes(eb));
}

void *
btrfs_extent_buffer_data_mutable(struct btrfs_trans_handle *handle,
    struct btrfs_extent_buffer *eb)
{
	struct btrfs_transaction *trans;

	if (handle == NULL || eb == NULL)
		return (NULL);
	trans = handle->bth_transaction;
	rw_assert_wrlock(&eb->eb_lock);
	if (eb->eb_transaction != trans ||
	    eb->eb_generation != trans->bt_generation ||
	    !eb->eb_loaded || !eb->eb_dirty || eb->eb_writeback ||
	    eb->eb_written || eb->eb_stale || eb->eb_error != 0 ||
	    eb->eb_private == NULL)
		return (NULL);
	return (eb->eb_private);
}

void
btrfs_extent_buffer_put(struct btrfs_extent_buffer *eb)
{
	struct btrfs_fs *bmp = eb->eb_mount;
	struct btrfs_extent_buffer *victim = NULL;
	unsigned int limit;

	KASSERT(eb->eb_refs > 0);
	rw_assert_anylock(&eb->eb_lock);
	rw_exit(&eb->eb_lock);

	if (bmp != NULL) {
		mtx_enter(&bmp->bm_ebmtx);
		KASSERT(eb->eb_refs > 0);
		if (--eb->eb_refs == 0) {
			KASSERT(eb->eb_transaction == NULL);
			if (eb->eb_error != 0 || eb->eb_stale) {
				RBT_REMOVE(btrfs_extent_buffer_tree,
				    &bmp->bm_extent_buffers, eb);
				victim = eb;
			} else {
				KASSERT(eb->eb_loaded && !eb->eb_dirty);
				KASSERT(eb->eb_buf == NULL);
				TAILQ_INSERT_TAIL(&bmp->bm_eb_lru, eb, eb_lru);
				bmp->bm_eb_cached++;
				limit = BTRFS_METADATA_CACHE_BYTES /
				    letoh32(bmp->bm_super.nodesize);
				if (bmp->bm_eb_cached > limit) {
					victim = TAILQ_FIRST(&bmp->bm_eb_lru);
					btrfs_extent_buffer_uncache(bmp, victim);
				}
			}
		}
		mtx_leave(&bmp->bm_ebmtx);
	} else {
		KASSERT(eb->eb_refs == 1);
		eb->eb_refs = 0;
		victim = eb;
	}
	btrfs_extent_buffer_free(victim);
}

static void
btrfs_extent_buffer_fail_transaction(struct btrfs_transaction *trans,
    int error)
{
	struct btrfs_fs *bmp = trans->bt_mount;

	if (error == 0)
		error = EIO;
	mtx_enter(&bmp->bm_trans_mtx);
	if (trans->bt_error == 0)
		trans->bt_error = error;
	trans->bt_state = BTRFS_TRANS_ABORTED;
	btrfs_fs_set_readonly(bmp);
	wakeup(&bmp->bm_transaction);
	mtx_leave(&bmp->bm_trans_mtx);
}

int
btrfs_write_dirty_metadata(struct btrfs_transaction *trans)
{
	struct btrfs_extent_buffer *eb;
	struct btrfs_write_batch batch;
	struct btrfs_fs *bmp = trans->bt_mount;
	struct btrfs_header *header;
	uint64_t flags;
	uint32_t nodesize;
	int error;

	mtx_enter(&bmp->bm_trans_mtx);
	if (bmp->bm_transaction != trans ||
	    trans->bt_state != BTRFS_TRANS_COMMITTING ||
	    trans->bt_writers != 0 || trans->bt_commit_handle) {
		error = trans->bt_error != 0 ? trans->bt_error : EINVAL;
		mtx_leave(&bmp->bm_trans_mtx);
		return (error);
	}
	mtx_leave(&bmp->bm_trans_mtx);

	nodesize = letoh32(bmp->bm_super.nodesize);
	btrfs_write_batch_init(&batch);
	TAILQ_FOREACH(eb, &trans->bt_dirty_extent_buffers, eb_dirty_entry) {
		rw_enter_write(&eb->eb_lock);
		if (eb->eb_transaction != trans || !eb->eb_dirty ||
		    eb->eb_stale || eb->eb_private == NULL) {
			error = EINVAL;
			goto fail;
		}
		if (eb->eb_error != 0) {
			error = eb->eb_error;
			goto fail;
		}
		if (eb->eb_written) {
			rw_exit_write(&eb->eb_lock);
			continue;
		}

		eb->eb_writeback = 1;
		header = eb->eb_private;
		flags = letoh64(header->flags);
		header->flags = htole64(flags | BTRFS_HEADER_FLAG_WRITTEN);
		memset(header->csum, 0, sizeof(header->csum));
		btrfs_csum(&bmp->bm_super,
		    (const uint8_t *)header + sizeof(header->csum),
		    nodesize - sizeof(header->csum), header->csum);
		error = btrfs_validate_tree_block(&bmp->bm_super, header,
		    eb->eb_bytenr, eb->eb_generation, trans->bt_generation,
		    eb->eb_owner, eb->eb_level);
		if (error == 0)
			error = btrfs_write_logical(bmp, eb->eb_bytenr,
			    nodesize, BTRFS_BLOCK_GROUP_METADATA |
			    BTRFS_BLOCK_GROUP_SYSTEM, header, &batch);
		eb->eb_writeback = 0;
		if (error != 0)
			goto fail;
		/* Freeze this block; phase completion gates durable publication. */
		eb->eb_written = 1;
		rw_exit_write(&eb->eb_lock);
	}
	error = btrfs_write_batch_wait(&batch);
	if (error != 0)
		btrfs_extent_buffer_fail_transaction(trans, error);
	return (error);

fail:
	eb->eb_writeback = 0;
	eb->eb_error = error;
	rw_exit_write(&eb->eb_lock);
	(void)btrfs_write_batch_wait(&batch);
	btrfs_extent_buffer_fail_transaction(trans, error);
	return (error);
}

int
btrfs_extent_buffers_finish(struct btrfs_transaction *trans, int committed)
{
	struct btrfs_extent_buffer *eb;
	int error = 0;

	KASSERT(trans->bt_writers == 0);
	KASSERT(!trans->bt_commit_handle);
	if (committed) {
		TAILQ_FOREACH(eb, &trans->bt_dirty_extent_buffers,
		    eb_dirty_entry) {
			rw_enter_read(&eb->eb_lock);
			if (eb->eb_transaction != trans || !eb->eb_dirty ||
			    eb->eb_writeback || !eb->eb_written ||
			    eb->eb_stale || eb->eb_error != 0)
				error = eb->eb_error != 0 ?
				    eb->eb_error : EBUSY;
			rw_exit_read(&eb->eb_lock);
			if (error != 0)
				return (error);
		}
	}

	for (;;) {
		mtx_enter(&trans->bt_lock);
		eb = TAILQ_FIRST(&trans->bt_dirty_extent_buffers);
		if (eb != NULL)
			TAILQ_REMOVE(&trans->bt_dirty_extent_buffers, eb,
			    eb_dirty_entry);
		mtx_leave(&trans->bt_lock);
		if (eb == NULL)
			break;

		rw_enter_write(&eb->eb_lock);
		KASSERT(eb->eb_transaction == trans);
		KASSERT(eb->eb_dirty);
		KASSERT(!eb->eb_writeback);
		eb->eb_transaction = NULL;
		eb->eb_dirty = 0;
		eb->eb_written = 0;
		if (!committed) {
			eb->eb_stale = 1;
			if (eb->eb_error == 0)
				eb->eb_error = trans->bt_error != 0 ?
				    trans->bt_error : EIO;
		}
		btrfs_extent_buffer_put(eb);
	}
	return (0);
}

static int
btrfs_extent_buffer_load(const struct btrfs_root *root,
    struct btrfs_extent_buffer *eb, uint64_t view_generation)
{
	struct btrfs_extent_buffer_validation validation;
	const struct btrfs_header *header;
	const struct btrfs_key_ptr *ptrs;
	struct buf *bp;
	uint32_t i;
	int error;

	rw_assert_wrlock(&eb->eb_lock);
	validation.ebv_super = root->br_super;
	validation.ebv_bytenr = eb->eb_bytenr;
	validation.ebv_generation = eb->eb_generation;
	validation.ebv_view_generation = view_generation;
	validation.ebv_owner = eb->eb_owner;
	validation.ebv_level = eb->eb_level;
	error = btrfs_read_logical(root, eb->eb_bytenr,
	    letoh32(root->br_super->nodesize),
	    BTRFS_BLOCK_GROUP_METADATA | BTRFS_BLOCK_GROUP_SYSTEM,
	    btrfs_extent_buffer_validate, &validation, NULL, &bp);
	if (error == 0) {
		header = (const struct btrfs_header *)bp->b_data;
		eb->eb_owner = letoh64(header->owner);
		eb->eb_max_generation = eb->eb_generation;
		if (header->level != 0) {
			ptrs = (const struct btrfs_key_ptr *)(header + 1);
			for (i = 0; i < letoh32(header->nritems); i++)
				eb->eb_max_generation = MAX(eb->eb_max_generation,
				    letoh64(ptrs[i].generation));
		}
		if (eb->eb_mount != NULL) {
			eb->eb_private = pool_get(&eb->eb_mount->bm_metadata_pool,
			    PR_WAITOK);
			memcpy(eb->eb_private, bp->b_data,
			    letoh32(root->br_super->nodesize));
			brelse(bp);
		} else
			eb->eb_buf = bp;
	} else if (error == ENOENT)
		error = EINVAL;
	return (error);
}

static int
btrfs_extent_buffer_validate(const void *data, size_t length, void *arg)
{
	const struct btrfs_extent_buffer_validation *validation = arg;

	if (length != letoh32(validation->ebv_super->nodesize))
		return (EINVAL);
	return (btrfs_validate_tree_block(validation->ebv_super,
	    data, validation->ebv_bytenr, validation->ebv_generation,
	    validation->ebv_view_generation, validation->ebv_owner,
	    validation->ebv_level));
}

static int
btrfs_validate_tree_block(const struct btrfs_super_block *sb,
    const struct btrfs_header *header, uint64_t bytenr, uint64_t generation,
    uint64_t view_generation, uint64_t owner, uint8_t level)
{
	const struct btrfs_item *items;
	const struct btrfs_key_ptr *ptrs;
	const uint8_t *fsid;
	uint32_t i, nritems, nodesize, offset, size;
	size_t array_end, data_end;

	nodesize = letoh32(sb->nodesize);
	if (!btrfs_csum_valid(sb,
	    (const uint8_t *)header + sizeof(header->csum),
	    nodesize - sizeof(header->csum), header->csum))
		return (EINVAL);

	fsid = sb->fsid;
	if (letoh64(sb->incompat_flags) &
	    BTRFS_FEATURE_INCOMPAT_METADATA_UUID)
		fsid = sb->metadata_uuid;
	if (memcmp(header->fsid, fsid, BTRFS_UUID_SIZE) != 0 ||
	    bytenr == 0 ||
	    (bytenr & (letoh32(sb->sectorsize) - 1)) != 0 ||
	    generation == 0 || generation > view_generation ||
	    letoh64(header->bytenr) != bytenr ||
	    letoh64(header->generation) != generation ||
	    (letoh64(header->owner) != owner &&
	    !(btrfs_file_tree(owner) &&
	    btrfs_file_tree(letoh64(header->owner)))) ||
	    header->level != level ||
	    level >= BTRFS_MAX_LEVEL)
		return (EINVAL);

	nritems = letoh32(header->nritems);
	if (level != 0 && nritems == 0)
		return (EINVAL);

	if (level == 0) {
		if (nritems > (nodesize - sizeof(*header)) / sizeof(*items))
			return (EINVAL);
		array_end = nritems * sizeof(*items);
		data_end = nodesize - sizeof(*header);
		items = (const struct btrfs_item *)(header + 1);
		for (i = 0; i < nritems; i++) {
			offset = letoh32(items[i].offset);
			size = letoh32(items[i].size);
			if (offset < array_end || offset > data_end ||
			    size > data_end - offset)
				return (EINVAL);
			if (i != 0 &&
			    btrfs_key_cmp(&items[i - 1].key, &items[i].key) >= 0)
				return (EINVAL);
			data_end = offset;
		}
	} else {
		if (nritems > (nodesize - sizeof(*header)) / sizeof(*ptrs))
			return (EINVAL);
		ptrs = (const struct btrfs_key_ptr *)(header + 1);
		for (i = 0; i < nritems; i++) {
			if (letoh64(ptrs[i].blockptr) == 0 ||
			    (letoh64(ptrs[i].blockptr) &
			    (letoh32(sb->sectorsize) - 1)) != 0 ||
			    letoh64(ptrs[i].generation) == 0 ||
			    letoh64(ptrs[i].generation) > view_generation)
				return (EINVAL);
			if (i != 0 &&
			    btrfs_key_cmp(&ptrs[i - 1].key,
			    &ptrs[i].key) >= 0)
				return (EINVAL);
		}
	}

	return (0);
}

static int
btrfs_key_cmp(const struct btrfs_key *a, const struct btrfs_key *b)
{
	uint64_t a_objectid, a_offset, b_objectid, b_offset;

	a_objectid = letoh64(a->objectid);
	b_objectid = letoh64(b->objectid);
	if (a_objectid < b_objectid)
		return (-1);
	if (a_objectid > b_objectid)
		return (1);
	if (a->type < b->type)
		return (-1);
	if (a->type > b->type)
		return (1);
	a_offset = letoh64(a->offset);
	b_offset = letoh64(b->offset);
	if (a_offset < b_offset)
		return (-1);
	if (a_offset > b_offset)
		return (1);
	return (0);
}
