// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS RFC-1950 helpers backed by OpenBSD's in-kernel zlib. */

#ifndef _SPL_SYS_ZMOD_H
#define	_SPL_SYS_ZMOD_H

#include <sys/types.h>
#include <lib/libz/zlib.h>

int	 z_uncompress(void *, size_t *, const void *, size_t);
int	 z_compress(void *, size_t *, const void *, size_t);
int	 z_compress_level(void *, size_t *, const void *, size_t, int);
const char *z_strerror(int);

#endif /* _SPL_SYS_ZMOD_H */
