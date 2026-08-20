// SPDX-License-Identifier: CDDL-1.0
/*
 * Generic doubly-linked list implementation.
 */

#include <sys/param.h>
#include <sys/list.h>
#include <sys/list_impl.h>
#include <sys/types.h>
#include <sys/debug.h>

#define	list_d2l(a, obj)	((list_node_t *)((char *)(obj) + \
	    (a)->list_offset))
#define	list_object(a, node)	((void *)((char *)(node) - (a)->list_offset))
#define	list_empty(a)		((a)->list_head.list_next == &(a)->list_head)

void
list_create(list_t *list, size_t size, size_t offset)
{
	ASSERT3P(list, !=, NULL);
	ASSERT3U(size, >=, offset + sizeof (list_node_t));

	list->list_offset = offset;
	list->list_head.list_next = &list->list_head;
	list->list_head.list_prev = &list->list_head;
}

void
list_destroy(list_t *list)
{
	list_node_t *node = &list->list_head;

	ASSERT3P(list, !=, NULL);
	ASSERT3P(node->list_next, ==, node);
	ASSERT3P(node->list_prev, ==, node);
	node->list_next = NULL;
	node->list_prev = NULL;
}

void
list_insert_after(list_t *list, void *object, void *nobject)
{
	list_node_t *node, *nnode;

	if (object == NULL) {
		list_insert_head(list, nobject);
		return;
	}
	node = list_d2l(list, object);
	nnode = list_d2l(list, nobject);
	nnode->list_prev = node;
	nnode->list_next = node->list_next;
	node->list_next->list_prev = nnode;
	node->list_next = nnode;
}

void
list_insert_before(list_t *list, void *object, void *nobject)
{
	list_node_t *node, *nnode;

	if (object == NULL) {
		list_insert_tail(list, nobject);
		return;
	}
	node = list_d2l(list, object);
	nnode = list_d2l(list, nobject);
	nnode->list_next = node;
	nnode->list_prev = node->list_prev;
	node->list_prev->list_next = nnode;
	node->list_prev = nnode;
}

void
list_insert_head(list_t *list, void *object)
{
	list_node_t *node = &list->list_head;
	list_node_t *nnode = list_d2l(list, object);

	nnode->list_prev = node;
	nnode->list_next = node->list_next;
	node->list_next->list_prev = nnode;
	node->list_next = nnode;
}

void
list_insert_tail(list_t *list, void *object)
{
	list_node_t *node = &list->list_head;
	list_node_t *nnode = list_d2l(list, object);

	nnode->list_next = node;
	nnode->list_prev = node->list_prev;
	node->list_prev->list_next = nnode;
	node->list_prev = nnode;
}

void
list_remove(list_t *list, void *object)
{
	list_node_t *node = list_d2l(list, object);

	ASSERT(!list_empty(list));
	ASSERT3P(node->list_next, !=, NULL);
	node->list_prev->list_next = node->list_next;
	node->list_next->list_prev = node->list_prev;
	node->list_next = NULL;
	node->list_prev = NULL;
}

void *
list_remove_head(list_t *list)
{
	void *object = list_head(list);

	if (object != NULL)
		list_remove(list, object);
	return (object);
}

void *
list_remove_tail(list_t *list)
{
	void *object = list_tail(list);

	if (object != NULL)
		list_remove(list, object);
	return (object);
}

void *
list_head(list_t *list)
{
	if (list_empty(list))
		return (NULL);
	return (list_object(list, list->list_head.list_next));
}

void *
list_tail(list_t *list)
{
	if (list_empty(list))
		return (NULL);
	return (list_object(list, list->list_head.list_prev));
}

void *
list_next(list_t *list, void *object)
{
	list_node_t *node = list_d2l(list, object);

	if (node->list_next == &list->list_head)
		return (NULL);
	return (list_object(list, node->list_next));
}

void *
list_prev(list_t *list, void *object)
{
	list_node_t *node = list_d2l(list, object);

	if (node->list_prev == &list->list_head)
		return (NULL);
	return (list_object(list, node->list_prev));
}

void
list_move_tail(list_t *dst, list_t *src)
{
	list_node_t *dhead = &dst->list_head;
	list_node_t *shead = &src->list_head;

	ASSERT3U(dst->list_offset, ==, src->list_offset);
	if (list_empty(src))
		return;
	dhead->list_prev->list_next = shead->list_next;
	shead->list_next->list_prev = dhead->list_prev;
	dhead->list_prev = shead->list_prev;
	shead->list_prev->list_next = dhead;
	shead->list_next = shead;
	shead->list_prev = shead;
}

void
list_link_replace(list_node_t *old, list_node_t *new)
{
	ASSERT(list_link_active(old));
	ASSERT(!list_link_active(new));
	new->list_next = old->list_next;
	new->list_prev = old->list_prev;
	old->list_prev->list_next = new;
	old->list_next->list_prev = new;
	old->list_next = NULL;
	old->list_prev = NULL;
}

void
list_link_init(list_node_t *link)
{
	link->list_next = NULL;
	link->list_prev = NULL;
}

int
list_link_active(list_node_t *link)
{
	EQUIV(link->list_next == NULL, link->list_prev == NULL);
	return (link->list_next != NULL);
}

int
list_is_empty(list_t *list)
{
	return (list_empty(list));
}
