/*
 * srandom_deterministic(3) compatibility for GNU/Linux.
 */
#include <stdlib.h>

void
srandom_deterministic(unsigned int seed)
{
	srandom(seed);
}
