/* unveil(2) is an OpenBSD-only filesystem sandboxing interface. */
#include <unistd.h>

int
unveil(const char *path, const char *permissions)
{
	(void)path;
	(void)permissions;
	return 0;
}
