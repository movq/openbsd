// SPDX-License-Identifier: CDDL-1.0

#ifndef _SPL_SYS_CMN_ERR_H
#define _SPL_SYS_CMN_ERR_H

#include <sys/atomic.h>
#include <sys/stdarg.h>

#define	CE_CONT		0
#define	CE_NOTE		1
#define	CE_WARN		2
#define	CE_PANIC	3
#define	CE_IGNORE	4

void cmn_err(int, const char *, ...)
    __attribute__((__format__(__kprintf__, 2, 3)));
void vcmn_err(int, const char *, va_list)
    __attribute__((__format__(__kprintf__, 2, 0)));

#define	cmn_err_once(ce, ...)	do { \
	static volatile uint32_t _printed; \
	if (atomic_cas_32(&_printed, 0, 1) == 0) \
		cmn_err((ce), __VA_ARGS__); \
} while (0)

#define	vcmn_err_once(ce, fmt, ap)	do { \
	static volatile uint32_t _printed; \
	if (atomic_cas_32(&_printed, 0, 1) == 0) \
		vcmn_err((ce), (fmt), (ap)); \
} while (0)

#endif /* _SPL_SYS_CMN_ERR_H */
