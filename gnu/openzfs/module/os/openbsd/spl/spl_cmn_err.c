// SPDX-License-Identifier: CDDL-1.0

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/cmn_err.h>

void
vcmn_err(int severity, const char *fmt, va_list ap)
{
	char buf[256];
	const char *prefix;

	switch (severity) {
	case CE_CONT:
		prefix = "zfs: ";
		break;
	case CE_NOTE:
		prefix = "zfs: NOTICE: ";
		break;
	case CE_WARN:
		prefix = "zfs: WARNING: ";
		break;
	case CE_PANIC:
		prefix = "zfs: PANIC: ";
		break;
	case CE_IGNORE:
		return;
	default:
		panic("zfs: unknown cmn_err severity %d", severity);
	}

	if (severity == CE_PANIC) {
		(void)vsnprintf(buf, sizeof (buf), fmt, ap);
		panic("%s%s", prefix, buf);
	}

	printf("%s", prefix);
	(void)vprintf(fmt, ap);
	printf("\n");
}

void
cmn_err(int severity, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vcmn_err(severity, fmt, ap);
	va_end(ap);
}
