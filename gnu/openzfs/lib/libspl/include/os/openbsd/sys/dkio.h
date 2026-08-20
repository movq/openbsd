// SPDX-License-Identifier: CDDL-1.0

#ifndef _LIBSPL_OPENBSD_SYS_DKIO_H
#define _LIBSPL_OPENBSD_SYS_DKIO_H

/*
 * Preserve the illumos definitions expected by common OpenZFS code, then add
 * the native OpenBSD ioctl used by the OS adapter.  The generic SPL header is
 * the next include-directory entry, ahead of the system include directory.
 */
#include_next <sys/dkio.h>
#include <sys/ioccom.h>

#ifndef DIOCCACHESYNC
#define	DIOCCACHESYNC	_IOW('d', 120, int)
#endif

#endif /* _LIBSPL_OPENBSD_SYS_DKIO_H */
