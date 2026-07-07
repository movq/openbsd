/*
 * Shim <stdio.h> — adds BSD extensions missing on glibc Linux.
 */
#include_next <stdio.h>

#ifndef BOOTSTRAP_STDIO_H
#define BOOTSTRAP_STDIO_H

__BEGIN_DECLS

char	*fgetln(FILE *, size_t *);

__END_DECLS

#endif /* BOOTSTRAP_STDIO_H */
