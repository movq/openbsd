#define sigset libc_sigset
#include_next <signal.h>
#undef sigset

#ifndef BOOTSTRAP_LINUX_SIGNAL_H
#define BOOTSTRAP_LINUX_SIGNAL_H

#include <sys/cdefs.h>

#ifndef NSIG
#define NSIG 65
#endif

#ifndef SIGINFO
#define SIGINFO 32
#endif

__BEGIN_DECLS

extern const char *const sys_signame[];

__END_DECLS

#endif /* BOOTSTRAP_LINUX_SIGNAL_H */
