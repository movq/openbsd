/*
 * Shim <stdlib.h> — adds BSD extensions missing on glibc Linux.
 */
#include_next <stdlib.h>

#ifndef BOOTSTRAP_STDLIB_H
#define BOOTSTRAP_STDLIB_H

#include <sys/types.h>

__BEGIN_DECLS

void	*reallocarray(void *, size_t, size_t);
long long strtonum(const char *, long long, long long, const char **);
void	 srandom_deterministic(unsigned int);

__END_DECLS

#endif /* BOOTSTRAP_STDLIB_H */
