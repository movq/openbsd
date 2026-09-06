#ifndef BTRFS_COMPAT_H
#define BTRFS_COMPAT_H

#include <stdint.h>

typedef uint8_t __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int8_t __s8;
typedef int16_t __s16;
typedef int32_t __s32;
typedef int64_t __s64;

typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef uint16_t __be16;
typedef uint32_t __be32;
typedef uint64_t __be64;

#ifndef __packed
#define __packed __attribute__((__packed__))
#endif
#ifndef __attribute_const__
#define __attribute_const__ __attribute__((__const__))
#endif
#ifndef __aligned_u64
#define __aligned_u64 __u64 __attribute__((aligned(8)))
#endif
#ifndef __DECLARE_FLEX_ARRAY
#define __DECLARE_FLEX_ARRAY(TYPE, NAME) \
	struct { struct {} __empty_ ## NAME; TYPE NAME[]; }
#endif
#ifndef __bitwise
#define __bitwise
#endif

#endif
