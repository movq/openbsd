// SPDX-License-Identifier: BSD-2-Clause
/* ASCII character classification used by OpenZFS kernel code. */

#ifndef _SPL_SYS_CTYPE_H
#define _SPL_SYS_CTYPE_H

static inline int
zfs_isalpha(int c)
{

	return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
}

static inline int
zfs_isdigit(int c)
{

	return (c >= '0' && c <= '9');
}

static inline int
zfs_iscntrl(int c)
{

	return ((unsigned int)c <= 0x1f || c == 0x7f);
}

static inline int
zfs_isspace(int c)
{

	return (c == ' ' || (c >= '\t' && c <= '\r'));
}

static inline int
zfs_tolower(int c)
{

	return (c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
}

static inline int
zfs_toupper(int c)
{

	return (c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c);
}

#define	isascii(c)	((unsigned int)(c) <= 0x7f)
#define	isalpha(c)	zfs_isalpha(c)
#define	isdigit(c)	zfs_isdigit(c)
#define	isalnum(c)	(zfs_isalpha(c) || zfs_isdigit(c))
#define	iscntrl(c)	zfs_iscntrl(c)
#define	isgraph(c)	((c) >= 0x21 && (c) <= 0x7e)
#define	islower(c)	((c) >= 'a' && (c) <= 'z')
#define	isprint(c)	((c) >= 0x20 && (c) <= 0x7e)
#define	ispunct(c)	(isgraph(c) && !isalnum(c))
#define	isspace(c)	zfs_isspace(c)
#define	isupper(c)	((c) >= 'A' && (c) <= 'Z')
#define	isxdigit(c)	(isdigit(c) || ((c) >= 'a' && (c) <= 'f') || \
	((c) >= 'A' && (c) <= 'F'))
#define	isblank(c)	((c) == ' ' || (c) == '\t')
#define	tolower(c)	zfs_tolower(c)
#define	toupper(c)	zfs_toupper(c)

#endif /* _SPL_SYS_CTYPE_H */
