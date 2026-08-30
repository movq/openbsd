#ifdef __unused
#undef __unused
#endif
#define _DONT_DEFINE_UNUSED
#include_next <unistd.h>
#undef _DONT_DEFINE_UNUSED

#define __unused __attribute__((__unused__))

#ifndef BOOTSTRAP_LINUX_UNISTD_H
#define BOOTSTRAP_LINUX_UNISTD_H

__BEGIN_DECLS

int	pledge(const char *, const char *);
int	unveil(const char *, const char *);
extern int optreset;

__END_DECLS

#endif /* BOOTSTRAP_LINUX_UNISTD_H */
