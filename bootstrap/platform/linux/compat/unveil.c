/*
 * unveil(2) stub - OpenBSD filesystem visibility, not available on Linux.
 */
#include <unistd.h>

int
unveil(const char *path, const char *permissions)
{
	(void)path;
	(void)permissions;
	return 0;
}
