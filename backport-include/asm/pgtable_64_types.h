/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __BACKPORT_ASM_PGTABLE_64_TYPES_H
#define __BACKPORT_ASM_PGTABLE_64_TYPES_H
#include_next <asm/pgtable_64_types.h>

#ifdef ARCH_PAGE_TABLE_SYNC_MASK
#undef ARCH_PAGE_TABLE_SYNC_MASK
#define ARCH_PAGE_TABLE_SYNC_MASK 0
#endif

#endif /* __BACKPORT_ASM_PGTABLE_64_TYPES_H */
