/*
 * Shim <signal.h> — adds BSD extensions missing on glibc Linux.
 */
#include_next <signal.h>

#ifndef BOOTSTRAP_SIGNAL_H
#define BOOTSTRAP_SIGNAL_H

/* NSIG is defined on glibc but ensure a reasonable value. */
#ifndef NSIG
#define NSIG 65
#endif

/* SIGINFO is BSD-specific.  Use a value that doesn't conflict. */
#ifndef SIGINFO
#define SIGINFO 32
#endif

__BEGIN_DECLS

extern const char *const sys_signame[];

__END_DECLS

#endif /* BOOTSTRAP_SIGNAL_H */
