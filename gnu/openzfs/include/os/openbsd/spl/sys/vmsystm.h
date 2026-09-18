// SPDX-License-Identifier: BSD-2-Clause
/* Minimal illumos VM-system compatibility used by shared OpenZFS code. */

#ifndef _SPL_SYS_VMSYSTM_H
#define _SPL_SYS_VMSYSTM_H

#include <sys/systm.h>

#define xcopyout(src, dst, len) copyout((src), (dst), (len))

#endif /* _SPL_SYS_VMSYSTM_H */
