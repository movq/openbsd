// SPDX-License-Identifier: CDDL-1.0

#ifndef _SPL_SYS_LIST_IMPL_H
#define _SPL_SYS_LIST_IMPL_H

#include <sys/types.h>

struct list_node {
	struct list_node *list_next;
	struct list_node *list_prev;
};

struct list {
	size_t list_offset;
	struct list_node list_head;
};

#endif /* _SPL_SYS_LIST_IMPL_H */
