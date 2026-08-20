// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS diagnostic-list interface; export support is implemented later. */

#ifndef _SPL_SYS_PROCFS_LIST_H
#define _SPL_SYS_PROCFS_LIST_H

#include <sys/list.h>
#include <sys/mutex.h>

struct seq_file;

typedef struct procfs_list procfs_list_t;
struct procfs_list {
	void		*pl_private;
	void		*pl_next_data;
	kmutex_t	pl_lock;
	list_t		pl_list;
	uint64_t	pl_next_id;
	int		(*pl_show)(struct seq_file *, void *);
	int		(*pl_show_header)(struct seq_file *);
	int		(*pl_clear)(procfs_list_t *);
	size_t		pl_node_offset;
};

typedef struct procfs_list_node {
	list_node_t	pln_link;
	uint64_t	pln_id;
} procfs_list_node_t;

void procfs_list_install(const char *, const char *, const char *, mode_t,
    procfs_list_t *, int (*)(struct seq_file *, void *),
    int (*)(struct seq_file *), int (*)(procfs_list_t *), size_t);
void procfs_list_uninstall(procfs_list_t *);
void procfs_list_destroy(procfs_list_t *);
void procfs_list_add(procfs_list_t *, void *);

#endif /* _SPL_SYS_PROCFS_LIST_H */
