/* NetBSD random(3) is suitable for deterministic host-side generation. */
#include <stdlib.h>

void
srandom_deterministic(unsigned int seed)
{
	srandom(seed);
}
