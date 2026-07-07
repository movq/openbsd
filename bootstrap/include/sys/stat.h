/*
 * Shim <sys/stat.h> - map OpenBSD stat fields used by makefs onto
 * glibc's reserved fields so they default to zero after stat(2).
 */
#include_next <sys/stat.h>

#ifndef BOOTSTRAP_SYS_STAT_H
#define BOOTSTRAP_SYS_STAT_H

#if defined(__GLIBC__)
#ifndef st_flags
#define st_flags __glibc_reserved[0]
#endif
#ifndef st_gen
#define st_gen __glibc_reserved[1]
#endif
#endif

#endif /* BOOTSTRAP_SYS_STAT_H */
