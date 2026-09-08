/*	$OpenBSD$	*/
/* Public domain. */

#include <sys/types.h>

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <btrfs/btrfs.h>
#include <btrfs/btrfs_ref.h>

/*
 * Disk ABI fixtures, independent of the writer's encoder. Include an unsigned
 * negative file base, shared counts greater than one, and legacy generation.
 */
static const uint8_t tree[] = {
	0xb0, 5, 0, 0, 0, 0, 0, 0, 0
};
static const uint8_t shared_tree[] = {
	0xb6, 0, 0x10, 0, 0, 0, 0, 0, 0
};
static const uint8_t data[] = {
	0xb2, 5, 0, 0, 0, 0, 0, 0, 0,
	0x01, 0x01, 0, 0, 0, 0, 0, 0,
	0, 0xf0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	3, 0, 0, 0
};
static const uint8_t shared_data[] = {
	0xb8, 0, 0x20, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0
};
static const uint8_t legacy[] = {
	5, 0, 0, 0, 0, 0, 0, 0,
	42, 0, 0, 0, 0, 0, 0, 0,
	0x01, 0x01, 0, 0, 0, 0, 0, 0,
	2, 0, 0, 0
};

static void
inline_ref(const uint8_t *bytes, size_t size, uint64_t flags,
    uint32_t count, uint32_t count_offset)
{
	struct btrfs_ref_decoded ref;
	uint8_t item[128] = { 0 };
	uint32_t start = 7, i;

	memcpy(item + start, bytes, size);
	assert(btrfs_ref_inline(item, start + size, start, flags, &ref) == 0);
	assert(ref.position == start && ref.size == size);
	assert(ref.count == count && ref.type == bytes[0]);
	if (count_offset != 0)
		assert(ref.count_offset == start + count_offset);
	if (bytes[0] == 0xb0 || bytes[0] == 0xb2) {
		assert(ref.owner.kind == BTRFS_REF_IMPLICIT);
		assert(ref.owner.u.implicit.root == 5);
		if (bytes[0] == 0xb2) {
			assert(ref.owner.u.implicit.objectid == 257);
			assert(ref.owner.u.implicit.offset == UINT64_MAX - 4095);
		}
	} else {
		assert(ref.owner.kind == BTRFS_REF_SHARED);
		assert(ref.owner.u.parent == (bytes[0] == 0xb6 ? 4096 : 8192));
	}
	for (i = 0; i < size; i++)
		assert(btrfs_ref_inline(item, start + i, start, flags,
		    &ref) == EINVAL);
	assert(btrfs_ref_inline(item, start - 1, start, flags, &ref) == EINVAL);
	assert(btrfs_ref_inline(item, sizeof(item), UINT32_MAX, flags,
	    &ref) == EINVAL);
	assert(btrfs_ref_inline(item, start + size, start,
	    flags ^ (BTRFS_EXTENT_FLAG_DATA | BTRFS_EXTENT_FLAG_TREE_BLOCK),
	    &ref) == EINVAL);

	/* A zero owner, or a present reference with zero count, is corrupt. */
	memset(item + start + 1, 0, 8);
	assert(btrfs_ref_inline(item, start + size, start, flags, &ref) == EINVAL);
	if (count_offset != 0) {
		memcpy(item + start, bytes, size);
		memset(item + start + count_offset, 0, 4);
		assert(btrfs_ref_inline(item, start + size, start, flags,
		    &ref) == EINVAL);
	}
}

int
main(void)
{
	struct btrfs_ref_decoded ref;
	uint8_t item[128] = { 0 };
	uint32_t i, position;

	inline_ref(tree, sizeof(tree), BTRFS_EXTENT_FLAG_TREE_BLOCK, 1, 0);
	inline_ref(shared_tree, sizeof(shared_tree),
	    BTRFS_EXTENT_FLAG_TREE_BLOCK | BTRFS_BLOCK_FLAG_FULL_BACKREF, 1, 0);
	inline_ref(data, sizeof(data), BTRFS_EXTENT_FLAG_DATA, 3, 25);
	inline_ref(shared_data, sizeof(shared_data), BTRFS_EXTENT_FLAG_DATA, 7, 9);

	memcpy(item, data, sizeof(data));
	memcpy(item + sizeof(data), shared_data, sizeof(shared_data));
	position = 0;
	assert(btrfs_ref_inline(item, sizeof(data) + sizeof(shared_data),
	    position, BTRFS_EXTENT_FLAG_DATA, &ref) == 0);
	position += ref.size;
	assert(btrfs_ref_inline(item, sizeof(data) + sizeof(shared_data),
	    position, BTRFS_EXTENT_FLAG_DATA, &ref) == 0);
	assert(ref.position == sizeof(data) && ref.count == 7);
	/* The tail must still be decoded after the first valid reference. */
	item[sizeof(data)] = 0xff;
	assert(btrfs_ref_inline(item, sizeof(data) + sizeof(shared_data),
	    position, BTRFS_EXTENT_FLAG_DATA, &ref) == EINVAL);

	assert(btrfs_ref_decode(0xb0, 5, NULL, 0, &ref) == 0);
	assert(ref.owner.u.implicit.root == 5 && ref.count == 1);
	assert(btrfs_ref_decode(0xb6, 4096, NULL, 0, &ref) == 0);
	assert(ref.owner.kind == BTRFS_REF_SHARED && ref.owner.u.parent == 4096);
	assert(btrfs_ref_decode(0xb0, 0, NULL, 0, &ref) == EINVAL);
	assert(btrfs_ref_decode(0xb6, 0, NULL, 0, &ref) == EINVAL);
	assert(btrfs_ref_decode(0xb0, 5, item, 1, &ref) == EINVAL);
	assert(btrfs_ref_decode(0xb6, 4096, item, 1, &ref) == EINVAL);
	assert(btrfs_ref_decode(0xb2, 12345, data + 1, sizeof(data) - 1,
	    &ref) == 0);
	assert(ref.owner.u.implicit.offset == UINT64_MAX - 4095);
	assert(ref.count_offset == 24 && ref.count == 3);
	assert(btrfs_ref_decode(0xb8, 8192, shared_data + 9, 4, &ref) == 0);
	assert(ref.owner.u.parent == 8192 && ref.count == 7);
	assert(btrfs_ref_decode(0xb8, 0, shared_data + 9, 4, &ref) == EINVAL);
	for (i = 0; i < 28; i++) {
		assert(btrfs_ref_decode(0xb2, 12345, data + 1, i,
		    &ref) == EINVAL);
		assert(btrfs_ref_decode(0xb4, 4096, legacy, i, &ref) == EINVAL);
	}
	for (i = 0; i < 4; i++)
		assert(btrfs_ref_decode(0xb8, 8192, shared_data + 9, i,
		    &ref) == EINVAL);
	assert(btrfs_ref_decode(0xb2, 12345, item, 29, &ref) == EINVAL);
	assert(btrfs_ref_decode(0xb8, 8192, item, 5, &ref) == EINVAL);
	assert(btrfs_ref_decode(0xb4, 4096, legacy, sizeof(legacy), &ref) == 0);
	assert(ref.owner.u.implicit.root == 5);
	assert(ref.owner.u.implicit.objectid == 257);
	assert(ref.generation == 42 && ref.count == 2);
	assert(btrfs_ref_decode(0xb4, 4096, item, 29, &ref) == EINVAL);
	item[0] = 0xb4;
	assert(btrfs_ref_inline(item, sizeof(item), 0, BTRFS_EXTENT_FLAG_DATA,
	    &ref) == EINVAL);
	item[0] = 0xac;	/* Quota ownership is not an allocation reference. */
	assert(btrfs_ref_inline(item, sizeof(item), 0, BTRFS_EXTENT_FLAG_DATA,
	    &ref) == EINVAL);
	assert(btrfs_ref_decode(0xff, 5, item, 0, &ref) == EINVAL);
	puts("reference decoding: passed");
	return (0);
}
