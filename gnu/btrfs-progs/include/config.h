#ifndef BTRFS_PROGS_CONFIG_H
#define BTRFS_PROGS_CONFIG_H

#include <sys/param.h>

#define BTRFS_DISABLE_BACKTRACE 1
#define COMPRESSION_LZO 0
#define COMPRESSION_ZSTD 0
#define CRYPTOPROVIDER "builtin"
#define CRYPTOPROVIDER_BUILTIN 1
#define EXPERIMENTAL 0
#define HAVE___BUILTIN_ADD_OVERFLOW 1
#define HAVE___BUILTIN_MUL_OVERFLOW 1
#define HAVE___BUILTIN_SUB_OVERFLOW 1
#define HAVE___BUILTIN_CPU_SUPPORTS__PCLMUL 0
#define HAVE_CFLAG_mavx2 0
#define HAVE_CFLAG_msha 0
#define HAVE_CFLAG_msse2 0
#define HAVE_CFLAG_msse41 0
#define PACKAGE_NAME "btrfs-progs"
#define PACKAGE_STRING "btrfs-progs v7.1 (OpenBSD port)"
#define PACKAGE_URL "https://btrfs.readthedocs.io/"
#define PACKAGE_VERSION "7.1"

#ifndef O_DIRECT
#define O_DIRECT 0
#endif
#ifndef O_NOATIME
#define O_NOATIME 0
#endif
#ifndef KERNEL_VERSION
#define KERNEL_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + (c))
#endif
#ifndef XATTR_NAME_MAX
#define XATTR_NAME_MAX 255
#endif
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
#ifndef _static_assert
#define _static_assert(expr) _Static_assert(expr, #expr)
#endif

#endif
