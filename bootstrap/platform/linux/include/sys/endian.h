/*
 * Shim <sys/endian.h> - provide OpenBSD-style byte-order names on glibc.
 */
#ifndef BOOTSTRAP_LINUX_SYS_ENDIAN_H
#define BOOTSTRAP_LINUX_SYS_ENDIAN_H

#include <stdint.h>
#include <endian.h>
#include <byteswap.h>

#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN __LITTLE_ENDIAN
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN __BIG_ENDIAN
#endif
#ifndef PDP_ENDIAN
#define PDP_ENDIAN __PDP_ENDIAN
#endif
#ifndef BYTE_ORDER
#define BYTE_ORDER __BYTE_ORDER
#endif

#ifndef htobe16
#if __BYTE_ORDER == __LITTLE_ENDIAN
#define htobe16(x) bswap_16(x)
#define htobe32(x) bswap_32(x)
#define htobe64(x) bswap_64(x)
#define htole16(x) (uint16_t)(x)
#define htole32(x) (uint32_t)(x)
#define htole64(x) (uint64_t)(x)
#else
#define htobe16(x) (uint16_t)(x)
#define htobe32(x) (uint32_t)(x)
#define htobe64(x) (uint64_t)(x)
#define htole16(x) bswap_16(x)
#define htole32(x) bswap_32(x)
#define htole64(x) bswap_64(x)
#endif
#endif

#ifndef be16toh
#define be16toh(x) htobe16(x)
#define be32toh(x) htobe32(x)
#define be64toh(x) htobe64(x)
#define le16toh(x) htole16(x)
#define le32toh(x) htole32(x)
#define le64toh(x) htole64(x)
#endif

#ifndef swap16
#define swap16(x) bswap_16(x)
#define swap32(x) bswap_32(x)
#define swap64(x) bswap_64(x)
#endif

#endif /* BOOTSTRAP_LINUX_SYS_ENDIAN_H */
