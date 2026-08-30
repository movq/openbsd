/*
 * sys_signame[] - BSD array mapping signal numbers to short names.
 * glibc provides sys_siglist (long descriptions), not sys_signame.
 */
#include <signal.h>
#include <stddef.h>

#ifndef NSIG
#define NSIG 65
#endif

const char *const sys_signame[NSIG] = {
	[0]		= NULL,
	[SIGHUP]	= "HUP",
	[SIGINT]	= "INT",
	[SIGQUIT]	= "QUIT",
	[SIGILL]	= "ILL",
	[SIGTRAP]	= "TRAP",
	[SIGABRT]	= "ABRT",
	[SIGBUS]	= "BUS",
	[SIGFPE]	= "FPE",
	[SIGKILL]	= "KILL",
	[SIGUSR1]	= "USR1",
	[SIGSEGV]	= "SEGV",
	[SIGUSR2]	= "USR2",
	[SIGPIPE]	= "PIPE",
	[SIGALRM]	= "ALRM",
	[SIGTERM]	= "TERM",
#ifdef SIGSTKFLT
	[SIGSTKFLT]	= "UNK",
#endif
	[SIGCHLD]	= "CHLD",
	[SIGCONT]	= "CONT",
	[SIGSTOP]	= "STOP",
	[SIGTSTP]	= "TSTP",
	[SIGTTIN]	= "TTIN",
	[SIGTTOU]	= "TTOU",
	[SIGURG]	= "URG",
	[SIGXCPU]	= "XCPU",
	[SIGXFSZ]	= "XFSZ",
	[SIGVTALRM]	= "VTALRM",
	[SIGPROF]	= "PROF",
	[SIGWINCH]	= "WINCH",
#ifdef SIGIO
	[SIGIO]		= "IO",
#elif defined(SIGPOLL)
	[SIGPOLL]	= "IO",
#endif
#ifdef SIGPWR
	[SIGPWR]	= "UNK",
#endif
	[SIGSYS]	= "SYS",
};
