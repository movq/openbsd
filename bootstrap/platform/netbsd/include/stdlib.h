#include_next <stdlib.h>

#ifndef BOOTSTRAP_NETBSD_STDLIB_H
#define BOOTSTRAP_NETBSD_STDLIB_H

long long strtonum(const char *, long long, long long, const char **);
void	srandom_deterministic(unsigned int);

#endif
