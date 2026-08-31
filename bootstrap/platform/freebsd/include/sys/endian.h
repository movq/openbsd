#include_next <sys/endian.h>

#ifndef BOOTSTRAP_FREEBSD_SYS_ENDIAN_H
#define BOOTSTRAP_FREEBSD_SYS_ENDIAN_H

#ifndef swap16
#define swap16(x) __bswap16(x)
#define swap32(x) __bswap32(x)
#define swap64(x) __bswap64(x)
#endif

#endif
