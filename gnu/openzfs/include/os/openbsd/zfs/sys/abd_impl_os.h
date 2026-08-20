// SPDX-License-Identifier: CDDL-1.0

#ifndef _ABD_IMPL_OS_H
#define _ABD_IMPL_OS_H

/* OpenBSD does not need temporary mappings for pool-backed ABD chunks. */
#define	abd_enter_critical(flags)	((void)(flags))
#define	abd_exit_critical(flags)	((void)(flags))

void	*abd_alloc_linear_buf(size_t, boolean_t);
void	 abd_free_linear_buf(void *, size_t, boolean_t);

#endif /* _ABD_IMPL_OS_H */
