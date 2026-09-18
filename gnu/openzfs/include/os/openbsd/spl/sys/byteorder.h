// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS byte-order compatibility for OpenBSD. */

#ifndef _SPL_SYS_BYTEORDER_H
#define _SPL_SYS_BYTEORDER_H

#include <sys/types.h>
#include <sys/endian.h>
#include <sys/isa_defs.h>

#define	BSWAP_8(x)	((uint8_t)(x))
#define	BSWAP_16(x)	swap16((uint16_t)(x))
#define	BSWAP_32(x)	swap32((uint32_t)(x))
#define	BSWAP_64(x)	swap64((uint64_t)(x))

#define	BMASK_8(x)	((uint8_t)(x))
#define	BMASK_16(x)	((uint16_t)(x))
#define	BMASK_32(x)	((uint32_t)(x))
#define	BMASK_64(x)	((uint64_t)(x))

#define	BE_8(x)		BMASK_8(x)
#define	BE_16(x)	htobe16((uint16_t)(x))
#define	BE_32(x)	htobe32((uint32_t)(x))
#define	BE_64(x)	htobe64((uint64_t)(x))
#define	LE_8(x)		BMASK_8(x)
#define	LE_16(x)	htole16((uint16_t)(x))
#define	LE_32(x)	htole32((uint32_t)(x))
#define	LE_64(x)	htole64((uint64_t)(x))

#define	htonll(x)	BE_64(x)
#define	ntohll(x)	BE_64(x)

#define	BE_IN8(p)	(((const uint8_t *)(p))[0])
#define	BE_IN16(p)	((uint16_t)((uint16_t)BE_IN8(p) << 8) | \
	BE_IN8((const uint8_t *)(p) + 1))
#define	BE_IN32(p)	((uint32_t)((uint32_t)BE_IN16(p) << 16) | \
	BE_IN16((const uint8_t *)(p) + 2))

#endif /* _SPL_SYS_BYTEORDER_H */
