#ifndef BOOTSTRAP_ERR_H
#define BOOTSTRAP_ERR_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

__BEGIN_DECLS

void	warn(const char *, ...)
	    __attribute__((__format__(__printf__, 1, 2)));
void	warnc(int, const char *, ...)
	    __attribute__((__format__(__printf__, 2, 3)));
void	warnx(const char *, ...)
	    __attribute__((__format__(__printf__, 1, 2)));
void	err(int, const char *, ...)
	    __attribute__((__noreturn__, __format__(__printf__, 2, 3)));
void	errc(int, int, const char *, ...)
	    __attribute__((__noreturn__, __format__(__printf__, 3, 4)));
void	errx(int, const char *, ...)
	    __attribute__((__noreturn__, __format__(__printf__, 2, 3)));

__END_DECLS

#endif /* BOOTSTRAP_ERR_H */
