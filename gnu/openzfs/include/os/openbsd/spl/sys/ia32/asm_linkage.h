// SPDX-License-Identifier: BSD-2-Clause
/* x86 assembler linkage definitions used by OpenZFS declarations. */

#ifndef _IA32_SYS_ASM_LINKAGE_H
#define _IA32_SYS_ASM_LINKAGE_H

#define RET ret
#define ENDBR

#undef ASMABI
#define ASMABI __attribute__((sysv_abi))

#define SECTION_TEXT .text
#define SECTION_STATIC .section .rodata

#endif /* _IA32_SYS_ASM_LINKAGE_H */
