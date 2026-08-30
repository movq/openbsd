/*
 * err(3) / warn(3) family — BSD error-reporting functions.
 * Simple implementations for GNU/Linux hosts.
 */
#include <err.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
vwarn_common(const char *fmt, va_list ap, const char *prefix)
{
	int saved_errno = errno;

	fprintf(stderr, "%s", prefix);
	if (fmt) {
		vfprintf(stderr, fmt, ap);
		fprintf(stderr, ": ");
	}
	fprintf(stderr, "%s\n", strerror(saved_errno));
}

void
warn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vwarn_common(fmt, ap, "");
	va_end(ap);
}

void
warnx(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (fmt)
		vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
}

void
err(int eval, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vwarn_common(fmt, ap, "");
	va_end(ap);
	exit(eval);
}

void
errx(int eval, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (fmt)
		vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(eval);
}

void
warnc(int code, const char *fmt, ...)
{
	va_list ap;
	int saved_errno = errno;

	errno = code;
	va_start(ap, fmt);
	vwarn_common(fmt, ap, "");
	va_end(ap);
	errno = saved_errno;
}

void
errc(int eval, int code, const char *fmt, ...)
{
	va_list ap;

	errno = code;
	va_start(ap, fmt);
	vwarn_common(fmt, ap, "");
	va_end(ap);
	exit(eval);
}
