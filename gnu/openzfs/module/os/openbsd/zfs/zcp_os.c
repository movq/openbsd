// SPDX-License-Identifier: CDDL-1.0
/* Channel programs are not part of the initial OpenBSD kernel port. */

#include <sys/zfs_context.h>
#include <sys/zcp.h>

uint64_t zfs_lua_max_instrlimit;
uint64_t zfs_lua_max_memlimit;

int
zcp_eval(const char *poolname, const char *program, boolean_t sync,
    uint64_t instrlimit, uint64_t memlimit, nvpair_t *arg, nvlist_t *result)
{

	(void)poolname;
	(void)program;
	(void)sync;
	(void)instrlimit;
	(void)memlimit;
	(void)arg;
	(void)result;
	return (EOPNOTSUPP);
}
