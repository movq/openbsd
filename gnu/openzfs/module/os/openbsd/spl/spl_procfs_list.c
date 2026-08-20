// SPDX-License-Identifier: BSD-2-Clause
/* In-kernel backing for OpenZFS diagnostic history lists. */

#include <sys/zfs_context.h>
#include <sys/procfs_list.h>

void
procfs_list_install(const char *module, const char *submodule,
    const char *name, mode_t mode, procfs_list_t *pl,
    int (*show)(struct seq_file *, void *),
    int (*show_header)(struct seq_file *), int (*clear)(procfs_list_t *),
    size_t node_offset)
{
	(void) module;
	(void) submodule;
	(void) name;
	(void) mode;

	mutex_init(&pl->pl_lock, NULL, MUTEX_DEFAULT, NULL);
	list_create(&pl->pl_list,
	    node_offset + sizeof (procfs_list_node_t),
	    node_offset + offsetof(procfs_list_node_t, pln_link));
	pl->pl_next_data = NULL;
	pl->pl_next_id = 1;
	pl->pl_show = show;
	pl->pl_show_header = show_header;
	pl->pl_clear = clear;
	pl->pl_node_offset = node_offset;
}

void
procfs_list_uninstall(procfs_list_t *pl)
{
	(void) pl;
}

void
procfs_list_destroy(procfs_list_t *pl)
{
	ASSERT(list_is_empty(&pl->pl_list));
	list_destroy(&pl->pl_list);
	mutex_destroy(&pl->pl_lock);
}

void
procfs_list_add(procfs_list_t *pl, void *entry)
{
	procfs_list_node_t *node;

	ASSERT(MUTEX_HELD(&pl->pl_lock));
	node = (procfs_list_node_t *)((char *)entry + pl->pl_node_offset);
	node->pln_id = pl->pl_next_id++;
	list_insert_tail(&pl->pl_list, entry);
}
