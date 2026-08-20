// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS signal-interruption test for the OpenBSD kernel. */

#ifndef _SPL_SYS_SIG_H
#define _SPL_SYS_SIG_H

#include <sys/proc.h>
#include <sys/signalvar.h>

static inline int
issig(void)
{
	struct sigctx ctx;

	if (curproc == NULL || SIGPENDING(curproc) == 0)
		return (0);
	return (cursig(curproc, &ctx, 1) != 0);
}

#endif /* _SPL_SYS_SIG_H */
