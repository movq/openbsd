// SPDX-License-Identifier: BSD-2-Clause
/* Allocation-free sorting for OpenZFS kernel callers. */

#include <sys/zfs_context.h>

static void
zfs_qsort_swap(char *a, char *b, size_t size)
{
	char value;

	while (size-- != 0) {
		value = *a;
		*a++ = *b;
		*b++ = value;
	}
}

static void
zfs_qsort_sift(char *base, size_t count, size_t size, size_t root,
	int (*compar)(const void *, const void *))
{
	size_t child;
	char *childp, *rootp;

	while (root < count / 2) {
		child = root * 2 + 1;
		childp = base + child * size;
		if (child + 1 < count &&
		    compar(childp, childp + size) < 0) {
			child++;
			childp += size;
		}

		rootp = base + root * size;
		if (compar(rootp, childp) >= 0)
			return;

		zfs_qsort_swap(rootp, childp, size);
		root = child;
	}
}

void
zfs_qsort(void *array, size_t count, size_t size,
	int (*compar)(const void *, const void *))
{
	char *base = array;
	size_t end, root;

	if (count < 2)
		return;
	if (size == 0 || count > SIZE_MAX / size)
		panic("zfs_qsort: invalid array dimensions");

	root = count / 2;
	while (root != 0) {
		root--;
		zfs_qsort_sift(base, count, size, root, compar);
	}

	end = count;
	while (end > 1) {
		end--;
		zfs_qsort_swap(base, base + end * size, size);
		zfs_qsort_sift(base, end, size, 0, compar);
	}
}
