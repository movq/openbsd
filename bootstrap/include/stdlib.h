#ifdef __unused
#undef __unused
#endif
#define _DONT_DEFINE_UNUSED
#include_next <stdlib.h>
#undef _DONT_DEFINE_UNUSED

#define __unused __attribute__((__unused__))

#ifndef BOOTSTRAP_STDLIB_H
#define BOOTSTRAP_STDLIB_H

#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

void	*reallocarray(void *, size_t, size_t);
long long strtonum(const char *, long long, long long, const char **);
void	 srandom_deterministic(unsigned int);

__END_DECLS

#endif /* BOOTSTRAP_STDLIB_H */
