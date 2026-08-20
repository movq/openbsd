// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS kernel string helpers for OpenBSD. */

#ifndef _SPL_SYS_STRING_H
#define _SPL_SYS_STRING_H

#include <sys/types.h>
#include <sys/systm.h>

char *strpbrk(const char *, const char *);
size_t strcspn(const char *, const char *);
char *zfs_strcpy(char *, const char *);
void strident_canon(char *, size_t);

#define	strcpy(dst, src)	zfs_strcpy((dst), (src))

#endif /* _SPL_SYS_STRING_H */
