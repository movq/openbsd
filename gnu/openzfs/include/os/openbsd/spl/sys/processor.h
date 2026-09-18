// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS processor identifiers for OpenBSD. */

#ifndef _SPL_SYS_PROCESSOR_H
#define _SPL_SYS_PROCESSOR_H

#include <sys/types.h>
#include <machine/cpu.h>

typedef uint16_t lgrpid_t;
typedef int processorid_t;
typedef int chipid_t;

#define	getcpuid()	CPU_INFO_UNIT(curcpu())

#endif /* _SPL_SYS_PROCESSOR_H */
