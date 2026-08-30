/*
 * Shim <sys/types.h> -- on glibc >= 2.28, makedev/major/minor
 * are no longer in <sys/types.h> but were moved to <sys/sysmacros.h>.
 *
 * Include the system <sys/types.h> first, then pull in the extra
 * macros so that OpenBSD code expecting them here works unchanged.
 */
#ifndef BOOTSTRAP_LINUX_SYS_TYPES_H
#define BOOTSTRAP_LINUX_SYS_TYPES_H

#include_next <sys/types.h>

/*
 * On glibc, major/minor/makedev require <sys/sysmacros.h>.
 * Use a direct #include (no sysmacros.h shim) to get the
 * real system header that sits beyond our bootstrap include
 * directory.
 */
#if defined(__GLIBC__)
# include <sys/sysmacros.h>
#endif

#endif /* BOOTSTRAP_LINUX_SYS_TYPES_H */
