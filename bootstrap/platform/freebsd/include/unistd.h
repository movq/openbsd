#include_next <unistd.h>

#ifndef BOOTSTRAP_FREEBSD_UNISTD_H
#define BOOTSTRAP_FREEBSD_UNISTD_H

int	pledge(const char *, const char *);
int	unveil(const char *, const char *);

#endif
