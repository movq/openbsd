#ifdef __unused
#undef __unused
#endif
#define _DONT_DEFINE_UNUSED
#include_next <unistd.h>
#undef _DONT_DEFINE_UNUSED

#include <sys/cdefs.h>

#ifndef BOOTSTRAP_UNISTD_H
#define BOOTSTRAP_UNISTD_H

__BEGIN_DECLS

int	pledge(const char *, const char *);
int	unveil(const char *, const char *);
extern int optreset;

__END_DECLS

#endif /* BOOTSTRAP_UNISTD_H */
