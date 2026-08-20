// SPDX-License-Identifier: BSD-2-Clause
/* Small Solaris DDI compatibility surface used by OpenZFS. */

#ifndef _SPL_SYS_SUNDDI_H
#define	_SPL_SYS_SUNDDI_H

#include <sys/types.h>
#include <sys/u8_textprep.h>

typedef int ddi_devid_t;

#define	DDI_DEV_T_NONE			((dev_t)-1)
#define	DDI_DEV_T_ANY			((dev_t)-2)
#define	DI_MAJOR_T_UNKNOWN		((major_t)0)

#define	DDI_PROP_DONTPASS		0x0001
#define	DDI_PROP_CANSLEEP		0x0002

#define	DDI_SUCCESS			0
#define	DDI_FAILURE			-1

#define	ddi_prop_lookup_string(x1, x2, x3, x4, x5)	(*(x5) = NULL)
#define	ddi_prop_free(x)				((void)0)
#define	ddi_root_node()					((void *)0)

int	ddi_strtol(const char *, char **, int, long *);
int	ddi_strtoll(const char *, char **, int, long long *);
int	ddi_strtoull(const char *, char **, int, unsigned long long *);
int	ddi_copyin(const void *, void *, size_t, int);
int	ddi_copyout(const void *, void *, size_t, int);

#endif /* _SPL_SYS_SUNDDI_H */
