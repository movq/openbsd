/*
 * Shim <unistd.h> — adds BSD extensions missing on glibc Linux.
 */
#include_next <unistd.h>

#ifndef BOOTSTRAP_UNISTD_H
#define BOOTSTRAP_UNISTD_H

__BEGIN_DECLS

int	pledge(const char *, const char *);
int	unveil(const char *, const char *);
extern int optreset;

__END_DECLS

#endif /* BOOTSTRAP_UNISTD_H */
