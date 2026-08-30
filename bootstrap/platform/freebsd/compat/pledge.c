/* pledge(2) is an OpenBSD-only sandboxing interface. */
#include <unistd.h>

int
pledge(const char *promises, const char *execpromises)
{
	(void)promises;
	(void)execpromises;
	return 0;
}
