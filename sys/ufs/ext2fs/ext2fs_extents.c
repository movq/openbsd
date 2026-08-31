/*-
 * Copyright (c) 2010 Zheng Liu <lz@freebsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/vnode.h>
#include <sys/buf.h>

#include <ufs/ufs/quota.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/inode.h>
#include <ufs/ext2fs/ext2fs.h>
#include <ufs/ext2fs/ext2fs_extents.h>
#include <ufs/ext2fs/ext2fs_extern.h>

#define	EXT4_PATH_SIZE	((EXT4_EXT_DEPTH_MAX + 1) * \
			    sizeof(struct ext4_extent_path))

static inline struct ext4_extent_header *
ext4_ext_inode_header(struct inode *ip)
{
	return ((struct ext4_extent_header *)(char *)ip->i_e2fs_blocks);
}

static inline int
ext4_ext_inode_depth(struct inode *ip)
{
	return (letoh16(ext4_ext_inode_header(ip)->eh_depth));
}

static inline uint64_t
ext4_ext_index_pblock(struct ext4_extent_index *index)
{
	return ((uint64_t)letoh16(index->ei_leaf_hi) << 32 |
	    letoh32(index->ei_leaf_lo));
}

static inline void
ext4_ext_store_index_pblock(struct ext4_extent_index *index, uint64_t pblk)
{
	index->ei_leaf_lo = htole32(pblk);
	index->ei_leaf_hi = htole16(pblk >> 32);
}

static inline uint64_t
ext4_ext_extent_pblock(struct ext4_extent *extent)
{
	return ((uint64_t)letoh16(extent->e_start_hi) << 32 |
	    letoh32(extent->e_start_lo));
}

static inline void
ext4_ext_store_pblock(struct ext4_extent *extent, uint64_t pblk)
{
	extent->e_start_lo = htole32(pblk);
	extent->e_start_hi = htole16(pblk >> 32);
}

static inline uint16_t
ext4_ext_get_actual_len(struct ext4_extent *extent)
{
	uint16_t len;

	len = letoh16(extent->e_len);
	return (len <= EXT_INIT_MAX_LEN ? len : len - EXT_INIT_MAX_LEN);
}

static inline int
ext4_ext_space_root(void)
{
	return ((sizeof(((struct ext2fs_dinode *)0)->e2di_blocks) -
	    sizeof(struct ext4_extent_header)) / sizeof(struct ext4_extent));
}

static inline int
ext4_ext_space_block(struct inode *ip)
{
	return ((ip->i_e2fs->e2fs_bsize -
	    sizeof(struct ext4_extent_header)) / sizeof(struct ext4_extent));
}

static int
ext4_ext_max_entries(struct inode *ip, int depth)
{
	if (depth == ext4_ext_inode_depth(ip))
		return (ext4_ext_space_root());
	return (ext4_ext_space_block(ip));
}

static int
ext4_ext_validate_block(struct inode *ip, uint64_t pblk, uint32_t count)
{
	struct m_ext2fs *fs = ip->i_e2fs;

	if (pblk <= fs->e2fs.e2fs_first_dblock ||
	    pblk + count < pblk ||
	    pblk + count > fs->e2fs.e2fs_bcount)
		return (EIO);
	return (0);
}

static int
ext4_ext_check_header(struct inode *ip, struct ext4_extent_header *eh,
    int depth)
{
	struct ext4_extent_index *index;
	struct ext4_extent *extent;
	uint64_t next;
	uint32_t count, i, lblk;
	uint16_t len, max;

	if (letoh16(eh->eh_magic) != EXT4_EXT_MAGIC ||
	    letoh16(eh->eh_depth) != depth ||
	    depth > EXT4_EXT_DEPTH_MAX)
		return (EIO);

	count = letoh16(eh->eh_ecount);
	max = letoh16(eh->eh_max);
	if (max == 0 || max > ext4_ext_max_entries(ip, depth) || count > max)
		return (EIO);

	next = 0;
	if (depth == 0) {
		extent = EXT_FIRST_EXTENT(eh);
		for (i = 0; i < count; i++, extent++) {
			lblk = letoh32(extent->e_blk);
			len = ext4_ext_get_actual_len(extent);
			if (len == 0 || (i != 0 && lblk < next) ||
			    (uint64_t)lblk + len > (uint64_t)EXT4_MAX_BLOCKS + 1 ||
			    ext4_ext_validate_block(ip,
			    ext4_ext_extent_pblock(extent), len) != 0)
				return (EIO);
			next = (uint64_t)lblk + len;
		}
	} else {
		index = EXT_FIRST_INDEX(eh);
		for (i = 0; i < count; i++, index++) {
			lblk = letoh32(index->ei_blk);
			if ((i != 0 && lblk <= next) ||
			    ext4_ext_validate_block(ip,
			    ext4_ext_index_pblock(index), 1) != 0)
				return (EIO);
			next = lblk;
		}
	}
	return (0);
}

static void
ext4_ext_binsearch_index(struct ext4_extent_path *path, daddr_t lbn)
{
	struct ext4_extent_header *eh = path->ep_header;
	struct ext4_extent_index *left, *right, *middle;

	left = EXT_FIRST_INDEX(eh) + 1;
	right = EXT_LAST_INDEX(eh);
	while (left <= right) {
		middle = left + (right - left) / 2;
		if (lbn < letoh32(middle->ei_blk))
			right = middle - 1;
		else
			left = middle + 1;
	}
	path->ep_index = left - 1;
}

static void
ext4_ext_binsearch_extent(struct ext4_extent_path *path, daddr_t lbn)
{
	struct ext4_extent_header *eh = path->ep_header;
	struct ext4_extent *left, *right, *middle;

	if (letoh16(eh->eh_ecount) == 0)
		return;

	left = EXT_FIRST_EXTENT(eh) + 1;
	right = EXT_LAST_EXTENT(eh);
	while (left <= right) {
		middle = left + (right - left) / 2;
		if (lbn < letoh32(middle->e_blk))
			right = middle - 1;
		else
			left = middle + 1;
	}
	path->ep_ext = left - 1;
}

static void
ext4_ext_drop_refs(struct ext4_extent_path *path)
{
	int depth, i;

	if (path == NULL)
		return;
	depth = path[0].ep_depth;
	if (depth > EXT4_EXT_DEPTH_MAX)
		depth = EXT4_EXT_DEPTH_MAX;
	for (i = 0; i <= depth; i++) {
		if (path[i].ep_data != NULL)
			free(path[i].ep_data, M_TEMP, path[i].ep_size);
		path[i].ep_data = NULL;
		path[i].ep_size = 0;
	}
}

void
ext4_ext_path_free(struct ext4_extent_path *path)
{
	if (path == NULL)
		return;
	ext4_ext_drop_refs(path);
	free(path, M_TEMP, EXT4_PATH_SIZE);
}

int
ext4_ext_find_extent(struct inode *ip, daddr_t lbn,
    struct ext4_extent_path **ppath)
{
	struct ext4_extent_header *eh;
	struct ext4_extent_path *path;
	struct buf *bp;
	uint64_t pblk;
	int alloc, depth, error, level;

	if (ppath == NULL || lbn < 0 || (uint64_t)lbn > EXT4_MAX_BLOCKS)
		return (EINVAL);

	eh = ext4_ext_inode_header(ip);
	depth = ext4_ext_inode_depth(ip);
	error = ext4_ext_check_header(ip, eh, depth);
	if (error)
		return (error);

	path = *ppath;
	alloc = path == NULL;
	if (alloc)
		path = malloc(EXT4_PATH_SIZE, M_TEMP, M_WAITOK | M_ZERO);
	else {
		ext4_ext_drop_refs(path);
		memset(path, 0, EXT4_PATH_SIZE);
	}
	*ppath = path;
	path[0].ep_depth = depth;
	path[0].ep_header = eh;

	for (level = 0; level < depth; level++) {
		if (letoh16(path[level].ep_header->eh_ecount) == 0) {
			error = EIO;
			goto fail;
		}
		ext4_ext_binsearch_index(&path[level], lbn);
		pblk = ext4_ext_index_pblock(path[level].ep_index);
		error = bread(ip->i_devvp, fsbtodb(ip->i_e2fs, pblk),
		    ip->i_e2fs->e2fs_bsize, &bp);
		if (error) {
			brelse(bp);
			goto fail;
		}
		path[level + 1].ep_size = ip->i_e2fs->e2fs_bsize;
		path[level + 1].ep_data = malloc(path[level + 1].ep_size,
		    M_TEMP, M_WAITOK);
		memcpy(path[level + 1].ep_data, bp->b_data,
		    path[level + 1].ep_size);
		brelse(bp);
		path[level + 1].ep_blk = pblk;
		path[level + 1].ep_header =
		    (struct ext4_extent_header *)path[level + 1].ep_data;
		error = ext4_ext_check_header(ip,
		    path[level + 1].ep_header, depth - level - 1);
		if (error)
			goto fail;
	}

	path[depth].ep_ext = NULL;
	path[depth].ep_index = NULL;
	ext4_ext_binsearch_extent(&path[depth], lbn);
	return (0);

fail:
	ext4_ext_drop_refs(path);
	if (alloc) {
		free(path, M_TEMP, EXT4_PATH_SIZE);
		*ppath = NULL;
	}
	return (error);
}

int
ext4_ext_in_cache(struct inode *ip, daddr_t lbn, struct ext4_extent *extent)
{
	struct ext4_extent_cache *cache = &ip->i_e2fs_ext_cache;

	if (cache->ec_type == EXT4_EXT_CACHE_NO)
		return (EXT4_EXT_CACHE_NO);
	if (lbn < cache->ec_blk ||
	    (uint64_t)lbn >= (uint64_t)cache->ec_blk + cache->ec_len)
		return (EXT4_EXT_CACHE_NO);

	extent->e_blk = htole32(cache->ec_blk);
	extent->e_len = htole16(cache->ec_len);
	ext4_ext_store_pblock(extent, cache->ec_start);
	return (cache->ec_type);
}

void
ext4_ext_put_cache(struct inode *ip, struct ext4_extent *extent, int type)
{
	struct ext4_extent_cache *cache = &ip->i_e2fs_ext_cache;

	cache->ec_type = type;
	cache->ec_blk = letoh32(extent->e_blk);
	cache->ec_len = ext4_ext_get_actual_len(extent);
	cache->ec_start = ext4_ext_extent_pblock(extent);
}

void
ext4_ext_tree_init(struct inode *ip)
{
	struct ext4_extent_header *eh;

	memset(ip->i_e2fs_blocks, 0, sizeof(ip->i_e2din->e2di_blocks));
	eh = ext4_ext_inode_header(ip);
	eh->eh_magic = htole16(EXT4_EXT_MAGIC);
	eh->eh_max = htole16(ext4_ext_space_root());
	ip->i_e2fs_flags |= EXT4_EXTENTS;
	ip->i_e2fs_ext_cache.ec_type = EXT4_EXT_CACHE_NO;
	ip->i_flag |= IN_CHANGE | IN_UPDATE;
}

static int
ext4_ext_dirty(struct inode *ip, struct ext4_extent_path *path)
{
	struct buf *bp;
	int error;

	ip->i_flag |= IN_CHANGE | IN_UPDATE;
	if (path->ep_data == NULL)
		return (ext2fs_update(ip, 1));

	bp = getblk(ip->i_devvp, fsbtodb(ip->i_e2fs, path->ep_blk),
	    ip->i_e2fs->e2fs_bsize, 0, INFSLP);
	memcpy(bp->b_data, path->ep_data, ip->i_e2fs->e2fs_bsize);
	error = bwrite(bp);
	return (error);
}

static uint64_t
ext4_ext_blkpref(struct inode *ip, struct ext4_extent_path *path,
    daddr_t lbn)
{
	struct ext4_extent *extent;
	uint64_t pblk;
	int depth;

	if (path != NULL) {
		depth = path[0].ep_depth;
		extent = path[depth].ep_ext;
		if (extent != NULL) {
			pblk = ext4_ext_extent_pblock(extent);
			if (lbn >= letoh32(extent->e_blk))
				return (pblk + lbn - letoh32(extent->e_blk));
			if (pblk >= letoh32(extent->e_blk) - lbn)
				return (pblk - (letoh32(extent->e_blk) - lbn));
		}
		if (path[depth].ep_data != NULL)
			return (path[depth].ep_blk);
	}
	return (ext2fs_blkpref(ip, lbn, 0, NULL));
}

static int
ext4_ext_can_merge(struct ext4_extent *left, struct ext4_extent *right)
{
	uint16_t llen, rlen;

	llen = ext4_ext_get_actual_len(left);
	rlen = ext4_ext_get_actual_len(right);
	if ((letoh16(left->e_len) > EXT_INIT_MAX_LEN) !=
	    (letoh16(right->e_len) > EXT_INIT_MAX_LEN))
		return (0);
	if (letoh32(left->e_blk) + llen != letoh32(right->e_blk) ||
	    llen + rlen > EXT4_MAX_LEN)
		return (0);
	return (ext4_ext_extent_pblock(left) + llen ==
	    ext4_ext_extent_pblock(right));
}

static int
ext4_ext_insert_index(struct inode *ip, struct ext4_extent_path *path,
    uint32_t lbn, uint64_t pblk)
{
	struct ext4_extent_index *index;
	int count, len;

	count = letoh16(path->ep_header->eh_ecount);
	if (count >= letoh16(path->ep_header->eh_max))
		return (EIO);

	if (lbn > letoh32(path->ep_index->ei_blk))
		index = path->ep_index + 1;
	else
		index = path->ep_index;
	len = EXT_FIRST_INDEX(path->ep_header) + count - index;
	if (len > 0)
		memmove(index + 1, index,
		    len * sizeof(struct ext4_extent_index));
	index->ei_blk = htole32(lbn);
	ext4_ext_store_index_pblock(index, pblk);
	path->ep_header->eh_ecount = htole16(count + 1);
	return (ext4_ext_dirty(ip, path));
}

static int
ext4_ext_alloc_meta(struct inode *ip, daddr_t lbn, uint64_t pref,
    struct ucred *cred, uint32_t *pblk)
{
	if (pref > EXT4_MAX_BLOCKS)
		pref = 0;
	return (ext2fs_alloc(ip, lbn, pref, cred, pblk));
}

static void
ext4_ext_free_blocks(struct inode *ip, uint64_t pblk, uint32_t count)
{
	uint64_t released;
	uint32_t i;

	for (i = 0; i < count; i++)
		ext2fs_blkfree(ip, pblk + i);
	released = (uint64_t)btodb(ip->i_e2fs->e2fs_bsize) * count;
	if (released >= ip->i_e2fs_nblock)
		ip->i_e2fs_nblock = 0;
	else
		ip->i_e2fs_nblock -= released;
	ip->i_flag |= IN_CHANGE | IN_UPDATE;
}

static int
ext4_ext_split(struct inode *ip, struct ext4_extent_path *path,
    struct ext4_extent *newext, int at, struct ucred *cred)
{
	struct ext4_extent_header *neh;
	struct ext4_extent_index *first;
	struct ext4_extent *extent;
	struct m_ext2fs *fs = ip->i_e2fs;
	struct buf *bp = NULL;
	uint32_t *blocks, border, newblk, oldblk;
	int depth, error, i, k, moved, nalloc, total;

	depth = ext4_ext_inode_depth(ip);
	border = path[depth].ep_ext != EXT_LAST_EXTENT(path[depth].ep_header) ?
	    letoh32(path[depth].ep_ext[1].e_blk) : letoh32(newext->e_blk);
	total = nalloc = depth - at;
	blocks = mallocarray(nalloc, sizeof(*blocks), M_TEMP,
	    M_WAITOK | M_ZERO);

	for (i = 0; i < nalloc; i++) {
		error = ext4_ext_alloc_meta(ip, letoh32(newext->e_blk),
		    ext4_ext_blkpref(ip, path, letoh32(newext->e_blk)),
		    cred, &blocks[i]);
		if (error)
			goto fail;
	}

	newblk = blocks[nalloc - 1];
	bp = getblk(ip->i_devvp, fsbtodb(fs, newblk), fs->e2fs_bsize,
	    0, INFSLP);
	clrbuf(bp);
	neh = (struct ext4_extent_header *)bp->b_data;
	neh->eh_magic = htole16(EXT4_EXT_MAGIC);
	neh->eh_max = htole16(ext4_ext_space_block(ip));
	extent = EXT_FIRST_EXTENT(neh);

	moved = EXT_LAST_EXTENT(path[depth].ep_header) -
	    path[depth].ep_ext;
	if (moved > 0) {
		memcpy(extent, path[depth].ep_ext + 1,
		    moved * sizeof(*extent));
		neh->eh_ecount = htole16(moved);
		path[depth].ep_header->eh_ecount =
		    htole16(letoh16(path[depth].ep_header->eh_ecount) -
		    moved);
	}
	error = bwrite(bp);
	bp = NULL;
	if (error)
		goto fail;
	if (moved != 0 && (error =
	    ext4_ext_dirty(ip, &path[depth])) != 0)
		goto fail;

	k = depth - at - 1;
	i = depth - 1;
	while (k-- > 0) {
		oldblk = newblk;
		newblk = blocks[--nalloc - 1];
		bp = getblk(ip->i_devvp, fsbtodb(fs, newblk),
		    fs->e2fs_bsize, 0, INFSLP);
		clrbuf(bp);
		neh = (struct ext4_extent_header *)bp->b_data;
		neh->eh_magic = htole16(EXT4_EXT_MAGIC);
		neh->eh_max = htole16(ext4_ext_space_block(ip));
		neh->eh_depth = htole16(depth - i);
		neh->eh_ecount = htole16(1);
		first = EXT_FIRST_INDEX(neh);
		first->ei_blk = htole32(border);
		ext4_ext_store_index_pblock(first, oldblk);

		moved = EXT_LAST_INDEX(path[i].ep_header) -
		    path[i].ep_index;
		if (moved > 0) {
			memcpy(first + 1, path[i].ep_index + 1,
			    moved * sizeof(*first));
			neh->eh_ecount = htole16(moved + 1);
			path[i].ep_header->eh_ecount =
			    htole16(letoh16(path[i].ep_header->eh_ecount) -
			    moved);
		}
		error = bwrite(bp);
		bp = NULL;
		if (error)
			goto fail;
		if (moved != 0 &&
		    (error = ext4_ext_dirty(ip, &path[i])) != 0)
			goto fail;
		i--;
	}

	error = ext4_ext_insert_index(ip, &path[at], border, newblk);
	free(blocks, M_TEMP, total * sizeof(*blocks));
	return (error);

fail:
	if (bp != NULL)
		brelse(bp);
	for (i = 0; i < total; i++) {
		if (blocks[i] != 0)
			ext4_ext_free_blocks(ip, blocks[i], 1);
	}
	free(blocks, M_TEMP, total * sizeof(*blocks));
	return (error);
}

static int
ext4_ext_grow_indepth(struct inode *ip, struct ext4_extent_path *path,
    struct ext4_extent *newext, struct ucred *cred)
{
	struct ext4_extent_header *eh, *neh;
	struct ext4_extent_index *index;
	struct buf *bp;
	uint32_t first_lbn, newblk;
	int depth, error;

	depth = ext4_ext_inode_depth(ip);
	eh = ext4_ext_inode_header(ip);
	first_lbn = depth == 0 ? letoh32(EXT_FIRST_EXTENT(eh)->e_blk) :
	    letoh32(EXT_FIRST_INDEX(eh)->ei_blk);
	error = ext4_ext_alloc_meta(ip, letoh32(newext->e_blk),
	    ext4_ext_blkpref(ip, path, letoh32(newext->e_blk)),
	    cred, &newblk);
	if (error)
		return (error);

	bp = getblk(ip->i_devvp, fsbtodb(ip->i_e2fs, newblk),
	    ip->i_e2fs->e2fs_bsize, 0, INFSLP);
	clrbuf(bp);
	memcpy(bp->b_data, ip->i_e2fs_blocks,
	    sizeof(ip->i_e2din->e2di_blocks));
	neh = (struct ext4_extent_header *)bp->b_data;
	neh->eh_max = htole16(ext4_ext_space_block(ip));
	error = bwrite(bp);
	if (error) {
		ext4_ext_free_blocks(ip, newblk, 1);
		return (error);
	}

	memset(ip->i_e2fs_blocks, 0, sizeof(ip->i_e2din->e2di_blocks));
	eh = ext4_ext_inode_header(ip);
	eh->eh_magic = htole16(EXT4_EXT_MAGIC);
	eh->eh_ecount = htole16(1);
	eh->eh_max = htole16(ext4_ext_space_root());
	eh->eh_depth = htole16(depth + 1);
	index = EXT_FIRST_INDEX(eh);
	index->ei_blk = htole32(first_lbn);
	ext4_ext_store_index_pblock(index, newblk);
	ip->i_flag |= IN_CHANGE | IN_UPDATE;
	return (ext2fs_update(ip, 1));
}

static int
ext4_ext_create_leaf(struct inode *ip, struct ext4_extent_path *path,
    struct ext4_extent *newext, struct ucred *cred)
{
	struct ext4_extent_path *current;
	int depth, error, level;

again:
	depth = ext4_ext_inode_depth(ip);
	level = depth;
	current = &path[depth];
	while (level > 0 && !EXT_HAS_FREE_INDEX(current)) {
		level--;
		current--;
	}

	if (EXT_HAS_FREE_INDEX(current))
		error = ext4_ext_split(ip, path, newext, level, cred);
	else if (depth < EXT4_EXT_DEPTH_MAX)
		error = ext4_ext_grow_indepth(ip, path, newext, cred);
	else
		error = EFBIG;
	if (error)
		return (error);

	error = ext4_ext_find_extent(ip, letoh32(newext->e_blk), &path);
	if (error)
		return (error);
	depth = ext4_ext_inode_depth(ip);
	if (letoh16(path[depth].ep_header->eh_ecount) ==
	    letoh16(path[depth].ep_header->eh_max))
		goto again;
	return (0);
}

static int
ext4_ext_correct_indexes(struct inode *ip, struct ext4_extent_path *path)
{
	struct ext4_extent *extent;
	uint32_t border;
	int depth, error, level;

	depth = ext4_ext_inode_depth(ip);
	extent = path[depth].ep_ext;
	if (depth == 0 || extent == NULL ||
	    extent != EXT_FIRST_EXTENT(path[depth].ep_header))
		return (0);

	border = letoh32(extent->e_blk);
	for (level = depth - 1; level >= 0; level--) {
		path[level].ep_index->ei_blk = htole32(border);
		error = ext4_ext_dirty(ip, &path[level]);
		if (error)
			return (error);
		if (level != 0 && path[level].ep_index !=
		    EXT_FIRST_INDEX(path[level].ep_header))
			break;
	}
	return (0);
}

static int
ext4_ext_insert_extent(struct inode *ip, struct ext4_extent_path *path,
    struct ext4_extent *newext, struct ucred *cred)
{
	struct ext4_extent_header *eh;
	struct ext4_extent *extent, *first, *last, *pos;
	int count, depth, error, len;

	depth = ext4_ext_inode_depth(ip);
	eh = path[depth].ep_header;
	extent = path[depth].ep_ext;
	if (ext4_ext_get_actual_len(newext) == 0 || eh == NULL)
		return (EINVAL);

	if (extent != NULL && ext4_ext_can_merge(extent, newext)) {
		extent->e_len = htole16(ext4_ext_get_actual_len(extent) +
		    ext4_ext_get_actual_len(newext));
		pos = extent;
		goto merge_right;
	}

	if (letoh16(eh->eh_ecount) == letoh16(eh->eh_max)) {
		error = ext4_ext_create_leaf(ip, path, newext, cred);
		if (error)
			return (error);
		depth = ext4_ext_inode_depth(ip);
		eh = path[depth].ep_header;
		extent = path[depth].ep_ext;
	}

	count = letoh16(eh->eh_ecount);
	first = EXT_FIRST_EXTENT(eh);
	if (extent == NULL)
		pos = first;
	else if (letoh32(newext->e_blk) > letoh32(extent->e_blk))
		pos = extent + 1;
	else
		pos = extent;
	len = first + count - pos;
	if (len > 0)
		memmove(pos + 1, pos, len * sizeof(*pos));
	*pos = *newext;
	eh->eh_ecount = htole16(count + 1);
	path[depth].ep_ext = pos;

	if (pos > first && ext4_ext_can_merge(pos - 1, pos)) {
		pos[-1].e_len = htole16(ext4_ext_get_actual_len(pos - 1) +
		    ext4_ext_get_actual_len(pos));
		memmove(pos, pos + 1,
		    (first + count - pos) * sizeof(*pos));
		eh->eh_ecount = htole16(letoh16(eh->eh_ecount) - 1);
		pos--;
		path[depth].ep_ext = pos;
	}

merge_right:
	last = EXT_LAST_EXTENT(eh);
	while (pos < last && ext4_ext_can_merge(pos, pos + 1)) {
		pos->e_len = htole16(ext4_ext_get_actual_len(pos) +
		    ext4_ext_get_actual_len(pos + 1));
		memmove(pos + 1, pos + 2,
		    (last - pos - 1) * sizeof(*pos));
		eh->eh_ecount = htole16(letoh16(eh->eh_ecount) - 1);
		last--;
	}

	error = ext4_ext_correct_indexes(ip, path);
	if (error == 0)
		error = ext4_ext_dirty(ip, &path[depth]);
	ip->i_e2fs_ext_cache.ec_type = EXT4_EXT_CACHE_NO;
	return (error);
}

static int
ext4_ext_convert_unwritten(struct inode *ip, daddr_t lbn,
    struct ext4_extent_path *path, struct ext4_extent *extent,
    struct ucred *cred, daddr_t *pblk)
{
	struct ext4_extent pieces[3], cached;
	struct ext4_extent_header *eh;
	uint64_t start;
	uint32_t eblk, offset;
	uint16_t count, len;
	int depth, error, i, npieces;

	depth = ext4_ext_inode_depth(ip);
	eh = path[depth].ep_header;
	count = letoh16(eh->eh_ecount);
	eblk = letoh32(extent->e_blk);
	len = ext4_ext_get_actual_len(extent);
	offset = lbn - eblk;
	start = ext4_ext_extent_pblock(extent);

	npieces = 0;
	if (offset != 0) {
		memset(&pieces[npieces], 0, sizeof(pieces[npieces]));
		pieces[npieces].e_blk = htole32(eblk);
		pieces[npieces].e_len =
		    htole16(EXT_INIT_MAX_LEN + offset);
		ext4_ext_store_pblock(&pieces[npieces], start);
		npieces++;
	}

	memset(&pieces[npieces], 0, sizeof(pieces[npieces]));
	pieces[npieces].e_blk = htole32(lbn);
	pieces[npieces].e_len = htole16(1);
	ext4_ext_store_pblock(&pieces[npieces], start + offset);
	cached = pieces[npieces];
	npieces++;

	if (offset + 1 < len) {
		memset(&pieces[npieces], 0, sizeof(pieces[npieces]));
		pieces[npieces].e_blk = htole32(lbn + 1);
		pieces[npieces].e_len =
		    htole16(EXT_INIT_MAX_LEN + len - offset - 1);
		ext4_ext_store_pblock(&pieces[npieces], start + offset + 1);
		npieces++;
	}

	memmove(extent, extent + 1,
	    (EXT_FIRST_EXTENT(eh) + count - extent - 1) *
	    sizeof(*extent));
	eh->eh_ecount = htole16(count - 1);
	error = ext4_ext_dirty(ip, &path[depth]);
	if (error)
		return (error);

	for (i = 0; i < npieces; i++) {
		error = ext4_ext_find_extent(ip, letoh32(pieces[i].e_blk),
		    &path);
		if (error)
			return (error);
		error = ext4_ext_insert_extent(ip, path, &pieces[i], cred);
		if (error)
			return (error);
	}

	*pblk = start + offset;
	ext4_ext_put_cache(ip, &cached, EXT4_EXT_CACHE_IN);
	return (0);
}

int
ext4_ext_get_blocks(struct inode *ip, daddr_t lbn, u_long max_blocks,
    struct ucred *cred, int *allocated, daddr_t *pblk)
{
	struct ext4_extent_path *path = NULL;
	struct ext4_extent cached, newext, *extent;
	uint64_t block, pref;
	uint32_t newblk;
	uint16_t len, rawlen;
	int depth, error, type;

	if (allocated == NULL || pblk == NULL || max_blocks == 0 ||
	    lbn < 0 || (uint64_t)lbn > EXT4_MAX_BLOCKS)
		return (EINVAL);
	*allocated = 0;
	*pblk = 0;

	type = ext4_ext_in_cache(ip, lbn, &cached);
	if (type == EXT4_EXT_CACHE_IN) {
		*pblk = ext4_ext_extent_pblock(&cached) +
		    lbn - letoh32(cached.e_blk);
		return (0);
	}

	error = ext4_ext_find_extent(ip, lbn, &path);
	if (error)
		goto out;
	depth = ext4_ext_inode_depth(ip);
	extent = path[depth].ep_ext;
	if (extent != NULL) {
		rawlen = letoh16(extent->e_len);
		len = ext4_ext_get_actual_len(extent);
		if (lbn >= letoh32(extent->e_blk) &&
		    lbn < (uint64_t)letoh32(extent->e_blk) + len) {
			if (rawlen > EXT_INIT_MAX_LEN) {
				error = ext4_ext_convert_unwritten(ip, lbn, path,
				    extent, cred, pblk);
				if (error == 0)
					*allocated = 1;
				goto out;
			}
			block = ext4_ext_extent_pblock(extent);
			*pblk = block + lbn - letoh32(extent->e_blk);
			ext4_ext_put_cache(ip, extent, EXT4_EXT_CACHE_IN);
			error = 0;
			goto out;
		}
	}

	pref = ext4_ext_blkpref(ip, path, lbn);
	error = ext4_ext_alloc_meta(ip, lbn, pref, cred, &newblk);
	if (error)
		goto out;

	memset(&newext, 0, sizeof(newext));
	newext.e_blk = htole32(lbn);
	newext.e_len = htole16(1);
	ext4_ext_store_pblock(&newext, newblk);
	error = ext4_ext_insert_extent(ip, path, &newext, cred);
	if (error) {
		ext4_ext_free_blocks(ip, newblk, 1);
		goto out;
	}

	ip->i_e2fs_last_lblk = lbn;
	ip->i_e2fs_last_blk = newblk;
	ext4_ext_put_cache(ip, &newext, EXT4_EXT_CACHE_IN);
	*allocated = 1;
	*pblk = newblk;

out:
	ext4_ext_path_free(path);
	return (error);
}

static int
ext4_ext_remove_node(struct inode *ip, struct ext4_extent_header *eh,
    int depth, daddr_t start, int *empty)
{
	struct ext4_extent_index *index;
	struct ext4_extent *extent;
	struct buf *bp;
	uint64_t pblk;
	uint32_t count, cut, i, keep, len, lblk;
	int child_empty, error;

	error = ext4_ext_check_header(ip, eh, depth);
	if (error)
		return (error);
	count = letoh16(eh->eh_ecount);

	if (depth == 0) {
		extent = EXT_FIRST_EXTENT(eh);
		keep = count;
		for (i = 0; i < count; i++) {
			lblk = letoh32(extent[i].e_blk);
			len = ext4_ext_get_actual_len(&extent[i]);
			if ((uint64_t)lblk + len <= start)
				continue;
			keep = i;
			if (start > lblk) {
				cut = start - lblk;
				pblk = ext4_ext_extent_pblock(&extent[i]) + cut;
				ext4_ext_free_blocks(ip, pblk, len - cut);
				if (letoh16(extent[i].e_len) >
				    EXT_INIT_MAX_LEN)
					cut += EXT_INIT_MAX_LEN;
				extent[i].e_len = htole16(cut);
				keep++;
				i++;
			}
			for (; i < count; i++)
				ext4_ext_free_blocks(ip,
				    ext4_ext_extent_pblock(&extent[i]),
				    ext4_ext_get_actual_len(&extent[i]));
			eh->eh_ecount = htole16(keep);
			break;
		}
		*empty = letoh16(eh->eh_ecount) == 0;
		return (0);
	}

	index = EXT_FIRST_INDEX(eh);
	i = count;
	while (i > 0) {
		i--;
		if (i + 1 < count && start >= letoh32(index[i + 1].ei_blk))
			break;
		pblk = ext4_ext_index_pblock(&index[i]);
		error = bread(ip->i_devvp, fsbtodb(ip->i_e2fs, pblk),
		    ip->i_e2fs->e2fs_bsize, &bp);
		if (error) {
			brelse(bp);
			return (error);
		}
		child_empty = 0;
		error = ext4_ext_remove_node(ip,
		    (struct ext4_extent_header *)bp->b_data, depth - 1,
		    start, &child_empty);
		if (error) {
			brelse(bp);
			return (error);
		}
		if (child_empty) {
			bp->b_flags |= B_INVAL;
			brelse(bp);
			ext4_ext_free_blocks(ip, pblk, 1);
			memmove(&index[i], &index[i + 1],
			    (count - i - 1) * sizeof(*index));
			count--;
			eh->eh_ecount = htole16(count);
		} else {
			error = bwrite(bp);
			if (error)
				return (error);
		}
	}
	*empty = count == 0;
	return (0);
}

int
ext4_ext_remove_space(struct inode *ip, daddr_t start)
{
	struct ext4_extent_header *eh;
	int depth, empty, error;

	eh = ext4_ext_inode_header(ip);
	depth = ext4_ext_inode_depth(ip);
	empty = 0;
	error = ext4_ext_remove_node(ip, eh, depth, start, &empty);
	if (error)
		return (error);

	if (empty && depth != 0) {
		memset(ip->i_e2fs_blocks, 0,
		    sizeof(ip->i_e2din->e2di_blocks));
		eh = ext4_ext_inode_header(ip);
		eh->eh_magic = htole16(EXT4_EXT_MAGIC);
		eh->eh_max = htole16(ext4_ext_space_root());
	}
	ip->i_e2fs_ext_cache.ec_type = EXT4_EXT_CACHE_NO;
	ip->i_flag |= IN_CHANGE | IN_UPDATE;
	return (ext2fs_update(ip, 1));
}
