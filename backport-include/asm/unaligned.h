#ifndef __BACKPORT_ASM_X86_UNALIGNED_H
#define __BACKPORT_ASM_X86_UNALIGNED_H

#ifdef BPM_ASM_UNALIGNED_HEADER_NOT_PRESENT
#include <linux/unaligned.h>
#else
#include_next "asm/unaligned.h"
#endif

#endif /* __BACKPORT_ASM_X86_UNALIGNED_H */
