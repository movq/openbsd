/*
 * pledge(2) stub - OpenBSD process sandboxing, not available on Linux.
 */
#include <unistd.h>

int
pledge(const char *promises, const char *execpromises)
{
	(void)promises;
	(void)execpromises;
	return 0;
}
