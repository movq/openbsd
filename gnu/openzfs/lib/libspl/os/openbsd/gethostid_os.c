// SPDX-License-Identifier: CDDL-1.0

#include <unistd.h>

unsigned long
get_system_hostid(void)
{
	return ((unsigned long)gethostid());
}
