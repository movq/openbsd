// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS kernel string helpers for OpenBSD. */

#include <sys/types.h>
#include <sys/systm.h>
#include <sys/string.h>

char *
zfs_strcpy(char *dst, const char *src)
{
	char *start = dst;

	while ((*dst++ = *src++) != '\0')
		;
	return (start);
}

char *
strpbrk(const char *str, const char *accept)
{
	const char *p;

	do {
		for (p = accept; *p != '\0' && *p != *str; p++)
			;
		if (*p != '\0')
			return ((char *)str);
	} while (*str++ != '\0');

	return (NULL);
}

size_t
strcspn(const char *str, const char *reject)
{
	const char *start = str;

	while (*str != '\0') {
		if (strchr(reject, *str) != NULL)
			break;
		str++;
	}
	return ((size_t)(str - start));
}

void
strident_canon(char *str, size_t len)
{
	char c;
	char *end;

	if (len == 0)
		return;

	end = str + len - 1;
	c = *str;
	if (c == '\0')
		return;
	if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) &&
	    c != '_')
		*str = '_';

	while (str < end && (c = *++str) != '\0') {
		if (!((c >= 'a' && c <= 'z') ||
		    (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '_'))
			*str = '_';
	}
	*str = '\0';
}
