/*
 * OpenBSD make uses sigset as a variable name; hide NetBSD's legacy
 * sigset(3) declaration while including the native signal definitions.
 */
#define sigset bootstrap_libc_sigset
#include_next <signal.h>
#undef sigset
