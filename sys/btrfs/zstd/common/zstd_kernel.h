/*	$OpenBSD$	*/

#ifndef ZSTD_KERNEL_H
#define ZSTD_KERNEL_H

#define DEBUGLEVEL 0
#define DYNAMIC_BMI2 0
#define ZSTD_COMPRESS_HEAPMODE 1
#define ZSTD_DISABLE_ASM 1
#define ZSTD_LEGACY_SUPPORT 0
#define ZSTD_NO_UNUSED_FUNCTIONS
#define ZSTD_STRIP_ERROR_STRINGS
#define ZSTD_TRACE 0

#include <sys/types.h>
#include <sys/param.h>
#include <sys/limits.h>
#include <sys/systm.h>

#ifndef _PTRDIFF_T_DEFINED_
#define _PTRDIFF_T_DEFINED_
typedef __ptrdiff_t ptrdiff_t;
#endif

#endif
