// SPDX-License-Identifier: CDDL-1.0

#ifndef _LIBSPL_OPENBSD_LIBINTL_H
#define _LIBSPL_OPENBSD_LIBINTL_H

/* OpenBSD base does not provide gettext. */
#define gettext(message)		(message)
#define dgettext(domain, message)	(message)
#define textdomain(domain)		(domain)

#endif /* _LIBSPL_OPENBSD_LIBINTL_H */
