// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS large-allocation compatibility for the OpenBSD kernel. */

#ifndef _SPL_SYS_VMEM_H
#define	_SPL_SYS_VMEM_H

#include <sys/kmem.h>

#define	VMEM_ALLOC	0x01
#define	VMEM_FREE	0x02

/* OpenZFS does not use illumos vmem arenas through this interface. */
#define	vmem_alloc(size, flags)	kmem_alloc((size), (flags))
#define	vmem_zalloc(size, flags)	kmem_zalloc((size), (flags))
#define	vmem_free(buf, size)		kmem_free((buf), (size))

#endif /* _SPL_SYS_VMEM_H */
