#include_next <stdlib.h>

#ifndef BOOTSTRAP_FREEBSD_STDLIB_H
#define BOOTSTRAP_FREEBSD_STDLIB_H

#include <sys/types.h>

void	srandom_deterministic(unsigned int);

#endif
