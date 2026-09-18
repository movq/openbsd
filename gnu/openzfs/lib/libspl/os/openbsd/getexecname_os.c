// SPDX-License-Identifier: CDDL-1.0

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "../../libspl_impl.h"

__attribute__((visibility("hidden"))) ssize_t
getexecname_impl(char *execname)
{
	const char *name = getprogname();
	size_t len;

	if (name == NULL)
		return (-1);
	len = strlcpy(execname, name, PATH_MAX + 1);
	if (len > PATH_MAX)
		return (-1);
	return ((ssize_t)len);
}
