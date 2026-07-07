/*
 * Shim <string.h> for building OpenBSD host tools on non-BSD systems.
 *
 * On OpenBSD, <string.h> declares strlcpy(3) and strlcat(3).
 * On glibc Linux they are missing; declare them here.
 */
#include_next <string.h>

#ifndef BOOTSTRAP_STRING_H
#define BOOTSTRAP_STRING_H

#include <sys/types.h>

__BEGIN_DECLS
size_t strlcpy(char *, const char *, size_t);
size_t strlcat(char *, const char *, size_t);
__END_DECLS

#endif /* BOOTSTRAP_STRING_H */
