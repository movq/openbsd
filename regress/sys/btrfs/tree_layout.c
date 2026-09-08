/*	$OpenBSD$	*/
/* Public domain. */

/*
 * Exercise the actual static primitives. Unused kernel functions are discarded
 * at link time; extent-buffer accessors are linked from their implementation.
 */
#include <btrfs/btrfs_tree.c>

/* Report kernel assertions in this userspace test. */
void
__assert(const char *prefix, const char *file, int line, const char *test)
{
	printf("%s: %s:%d: %s\n", prefix, file, line, test);
	__builtin_trap();
}

static uint8_t source[16384], rebuilt[16384], packed_leaf[16384];
static uint8_t value[16384];

static struct btrfs_key
key_for(uint64_t id)
{
	struct btrfs_key key = { 0 };

	key.objectid = htole64(id);
	return (key);
}

static void
fixture(uint32_t nodesize, uint32_t count, uint32_t size)
{
	struct btrfs_header *header = (struct btrfs_header *)source;
	struct btrfs_item *items = (struct btrfs_item *)(header + 1);
	uint32_t i, end = nodesize - sizeof(*header);

	memset(source, 0, sizeof(source));
	header->nritems = htole32(count);
	for (i = 0; i < count; i++) {
		end -= size;
		items[i].key = key_for(20 * (i + 1));
		items[i].offset = htole32(end);
		items[i].size = htole32(size);
		memset((uint8_t *)(header + 1) + end, i + 1, size);
	}
}

static void
searches(void)
{
	struct btrfs_header *header = (struct btrfs_header *)source;
	struct btrfs_key_ptr *ptrs = (struct btrfs_key_ptr *)(header + 1);
	struct btrfs_key key;
	uint32_t id, slot, expected, i;

	fixture(4096, 3, 8);
	for (id = 0; id <= 80; id++) {
		key = key_for(id);
		expected = id <= 20 ? 0 : id <= 40 ? 1 : id <= 60 ? 2 : 3;
		assert(btrfs_leaf_slot(header, &key, &slot) ==
		    (id == 20 || id == 40 || id == 60 ? 0 : ENOENT));
		assert(slot == expected);
	}
	header->level = 1;
	for (i = 0; i < 3; i++)
		ptrs[i].key = key_for(20 * (i + 1));
	for (id = 0; id <= 80; id++) {
		key = key_for(id);
		assert(btrfs_node_slot(header, &key, &slot) == 0);
		assert(slot == (id < 40 ? 0 : id < 60 ? 1 : 2));
	}
	header->nritems = 0;
	assert(btrfs_node_slot(header, &key, &slot) == EINVAL);
	header->level = 0;
	assert(btrfs_leaf_slot(header, &key, &slot) == ENOENT && slot == 0);
}

static void
check_contents(const struct btrfs_leaf_edit *edit, uint32_t capacity)
{
	struct btrfs_header *output = (struct btrfs_header *)rebuilt;
	struct btrfs_item *out = (struct btrfs_item *)(output + 1);
	uint8_t *base = (uint8_t *)(output + 1);
	uint32_t count = letoh32(output->nritems), i, j, index, offset;
	int changed;

	/* Check logical contents independently of the packing implementation. */
	for (i = 0; i < count; i++) {
		index = i;
		if (edit->operation == BTRFS_LEAF_INSERT && i > edit->slot)
			index--;
		if (edit->operation == BTRFS_LEAF_DELETE && i >= edit->slot)
			index++;
		changed = edit->operation != BTRFS_LEAF_DELETE &&
		    i == edit->slot;
		if (changed) {
			assert(btrfs_key_cmp(&out[i].key, edit->key) == 0);
			assert(letoh32(out[i].size) == edit->size);
		} else {
			assert(letoh64(out[i].key.objectid) == 20 * (index + 1));
			assert(letoh32(out[i].size) == 8);
		}
		for (j = 0; j < letoh32(out[i].size); j++)
			assert(base[letoh32(out[i].offset) + j] ==
			    (changed ? 0xa5 : index + 1));
	}
	offset = count == 0 ? capacity : letoh32(out[count - 1].offset);
	for (i = count * sizeof(*out); i < offset; i++)
		assert(base[i] == 0);
}

static void
edit_case(uint32_t nodesize, int operation, uint32_t slot, uint32_t size,
    int gap)
{
	struct btrfs_header *header = (struct btrfs_header *)source;
	struct btrfs_header *output = (struct btrfs_header *)rebuilt;
	struct btrfs_item *items = (struct btrfs_item *)(header + 1);
	struct btrfs_key key;
	struct btrfs_leaf_edit edit = { .source = header, .data = value,
	    .key = &key, .slot = slot, .size = size, .operation = operation };
	uint32_t capacity = nodesize - sizeof(*header);
	uint32_t count, offset;
	int packed;

	fixture(nodesize, 3, 8);
	if (gap) {
		offset = letoh32(items[2].offset);
		memmove((uint8_t *)(header + 1) + offset - 8,
		    (uint8_t *)(header + 1) + offset, 8);
		items[2].offset = htole32(offset - 8);
	}
	key = key_for(20 * slot +
	    (operation == BTRFS_LEAF_INSERT ? 10 : 20));
	assert(btrfs_leaf_check_edit(&edit, capacity, &count, &packed) == 0);
	assert(packed == !gap);
	memcpy(rebuilt, source, nodesize);
	assert(btrfs_leaf_build(output, capacity, &edit, 0, count) == 0);
	if (packed) {
		memcpy(packed_leaf, source, nodesize);
		btrfs_leaf_edit_packed((struct btrfs_header *)packed_leaf,
		    &key, value, size, capacity, slot, operation);
		assert(memcmp(packed_leaf, rebuilt, nodesize) == 0);
	}
	check_contents(&edit, capacity);
}

static void
edits(uint32_t nodesize)
{
	struct btrfs_header *header = (struct btrfs_header *)source;
	struct btrfs_header *output = (struct btrfs_header *)rebuilt;
	struct btrfs_item *items = (struct btrfs_item *)(header + 1);
	struct btrfs_item *out = (struct btrfs_item *)(output + 1);
	struct btrfs_key key = key_for(10);
	struct btrfs_leaf_edit edit = { .source = header };
	uint32_t capacity = nodesize - sizeof(*header), count, slot, size;
	int operation, packed;

	memset(value, 0xa5, sizeof(value));
	for (operation = BTRFS_LEAF_INSERT; operation <= BTRFS_LEAF_DELETE;
	    operation++) {
		for (slot = 0; slot < 3 + (operation == BTRFS_LEAF_INSERT);
		    slot++) {
			for (size = 0; size <= 32; size += 8) {
				edit_case(nodesize, operation, slot, size, 0);
				edit_case(nodesize, operation, slot, size, 1);
			}
		}
	}
	/* Aliased input is consumed before the source is replaced. */
	fixture(nodesize, 3, 8);
	edit.operation = BTRFS_LEAF_REPLACE;
	edit.slot = 1;
	edit.key = &items[1].key;
	edit.data = (uint8_t *)(header + 1) + letoh32(items[0].offset);
	edit.size = 4;
	memcpy(rebuilt, source, nodesize);
	assert(btrfs_leaf_build(output, capacity, &edit, 0, 3) == 0);
	assert(memcmp((uint8_t *)(output + 1) + letoh32(out[1].offset),
	    edit.data, 4) == 0);
	items[1].offset = items[0].offset;
	assert(btrfs_leaf_check_edit(&edit, capacity, &count, &packed) == EINVAL);

	/* Inserting into and deleting the last item from a root leaf. */
	fixture(nodesize, 0, 0);
	edit.operation = BTRFS_LEAF_INSERT;
	edit.slot = 0;
	edit.key = &key;
	edit.data = value;
	edit.size = 0;
	assert(btrfs_leaf_check_edit(&edit, capacity, &count, &packed) == 0);
	assert(count == 1 && packed);
	btrfs_leaf_edit_packed(header, &key, value, 0, capacity, 0,
	    BTRFS_LEAF_INSERT);
	edit.operation = BTRFS_LEAF_DELETE;
	assert(btrfs_leaf_check_edit(&edit, capacity, &count, &packed) == 0);
	assert(count == 0 && packed);
	memcpy(rebuilt, source, nodesize);
	assert(btrfs_leaf_build(output, capacity, &edit, 0, count) == 0);
	btrfs_leaf_edit_packed(header, &key, NULL, 0, capacity, 0,
	    BTRFS_LEAF_DELETE);
	assert(memcmp(source, rebuilt, nodesize) == 0);
	check_contents(&edit, capacity);
}

static void
splits(uint32_t nodesize)
{
	struct btrfs_header *header = (struct btrfs_header *)source;
	struct btrfs_header *output = (struct btrfs_header *)rebuilt;
	struct btrfs_key key;
	struct btrfs_leaf_edit edit = { .source = header, .key = &key,
	    .data = value, .operation = BTRFS_LEAF_INSERT };
	uint32_t capacity = nodesize - sizeof(*header);
	uint32_t counts[3], nleaves, count, slot, first, i;
	int packed;

	for (slot = 0; slot <= 2; slot++) {
		fixture(nodesize, 2, capacity / 4);
		key = key_for(20 * slot + 10);
		edit.slot = slot;
		edit.size = capacity - sizeof(struct btrfs_item);
		assert(btrfs_leaf_check_edit(&edit, capacity, &count,
		    &packed) == ENOSPC);
		assert(btrfs_leaf_split_counts(&edit, capacity, counts,
		    &nleaves) == 0);
		assert(nleaves == (slot == 1 ? 3 : 2));
		first = 0;
		for (i = 0; i < nleaves; i++) {
			memcpy(rebuilt, source, nodesize);
			assert(counts[i] != 0);
			assert(btrfs_leaf_build(output, capacity, &edit,
			    first, counts[i]) == 0);
			first += counts[i];
		}
		assert(first == 3);
		edit.size++;
		assert(btrfs_leaf_split_counts(&edit, capacity, counts,
		    &nleaves) == ENOSPC);
	}
	/* A tie in byte balance keeps the earlier two-way partition. */
	fixture(nodesize, 2, capacity / 3);
	key = key_for(30);
	edit.slot = 1;
	edit.size = capacity / 3;
	assert(btrfs_leaf_check_edit(&edit, capacity, &count, &packed) == ENOSPC);
	assert(btrfs_leaf_split_counts(&edit, capacity, counts, &nleaves) == 0);
	assert(nleaves == 2 && counts[0] == 1 && counts[1] == 2);
	assert(!btrfs_leaf_fits(capacity, UINT32_MAX, 0));
	assert(!btrfs_leaf_fits(capacity, 1, SIZE_MAX));
}

static void
separators(void)
{
	struct btrfs_transaction trans = { .bt_generation = 7 };
	struct btrfs_trans_handle handle = { .bth_transaction = &trans };
	struct btrfs_path path = { .bp_handle = &handle, .bp_level = 4 };
	struct btrfs_extent_buffer eb[5] = { 0 };
	struct {
		struct btrfs_header header;
		struct btrfs_key_ptr ptrs[2];
	} nodes[5] = { 0 }, before[5];
	struct btrfs_key old = key_for(20), new = key_for(10);
	uint32_t i, slot;

	path.bp_slot[3] = 1;	/* This ancestor changes, but its parent does not. */
	for (i = 0; i <= path.bp_level; i++) {
		path.bp_eb[i] = &eb[i];
		eb[i].eb_bytenr = (i + 1) * 4096;
		eb[i].eb_generation = 7;
		eb[i].eb_transaction = &trans;
		eb[i].eb_loaded = eb[i].eb_dirty = 1;
		eb[i].eb_private = (uint8_t *)&nodes[i];
		nodes[i].header.level = i;
		nodes[i].header.nritems = htole32(2);
		if (i == 0)
			continue;
		slot = path.bp_slot[i];
		nodes[i].ptrs[slot].key = old;
		nodes[i].ptrs[slot].blockptr = htole64(eb[i - 1].eb_bytenr);
		nodes[i].ptrs[slot].generation = htole64(7);
	}
	assert(btrfs_check_separators(&path, 1, &old) == 0);
	nodes[3].ptrs[1].key = new;
	memcpy(before, nodes, sizeof(nodes));
	assert(btrfs_check_separators(&path, 1, &old) == EINVAL);
	assert(memcmp(before, nodes, sizeof(nodes)) == 0);
	nodes[3].ptrs[1].key = old;
	nodes[2].ptrs[0].generation = htole64(6);
	assert(btrfs_check_separators(&path, 1, &old) == EINVAL);
	nodes[2].ptrs[0].generation = htole64(7);
	path.bp_eb[0] = NULL;	/* Pruning starts at a surviving internal node. */
	assert(btrfs_check_separators(&path, 2, &old) == 0);
	btrfs_update_separators(&path, 2, &new);
	assert(btrfs_key_cmp(&nodes[1].ptrs[0].key, &old) == 0);
	assert(btrfs_key_cmp(&nodes[2].ptrs[0].key, &new) == 0);
	assert(btrfs_key_cmp(&nodes[3].ptrs[1].key, &new) == 0);
	assert(btrfs_key_cmp(&nodes[4].ptrs[0].key, &old) == 0);
	assert(btrfs_check_separators(&path, 5, &old) == 0);
}

int
main(void)
{
	searches();
	edits(4096);
	edits(16384);
	splits(4096);
	splits(16384);
	separators();
	printf("tree slot, layout, split and separator checks passed\n");
	return (0);
}
