// SPDX-License-Identifier: BSD-2-Clause
/* OpenBSD file interfaces plus the OpenZFS kernel-address ioctl flag. */

#ifndef _SPL_SYS_FILE_H
#define	_SPL_SYS_FILE_H

#include_next <sys/file.h>

/* The ioctl argument already names kernel memory. */
#define	FKIOCTL	0x80000000

#endif /* _SPL_SYS_FILE_H */
