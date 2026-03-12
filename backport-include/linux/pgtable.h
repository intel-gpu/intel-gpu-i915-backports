/* SPDX-License-Identifier: GPL-2.0 */
/*
 * include/linux/pgtable.h
 *
 * In KV(5,8,0), include/asm-generic/pgtable.h is moved
 * to include/linux/pgtable.h. Add a header to include
 * asm/pgtable.h
 *
 * commit detail: ca5999f mm: introduce include/linux/pgtable.h
 *
 */

#ifndef _BACKPORT_LINUX_PGTABLE_H
#define _BACKPORT_LINUX_PGTABLE_H

#ifdef BPM_ASM_PGTABLE_H_NOT_PRESENT
#include <asm/pgtable.h>
#else
#include_next <linux/pgtable.h>
#endif

#ifdef BPM_PXD_DEVMAP_NOT_PRESENT
static inline int pmd_devmap(pmd_t pmd) { return 0; }
static inline int pte_devmap(pte_t pte) { return 0; }
static inline int pud_devmap(pud_t pud) { return 0; }
#endif

#ifdef BPM_PMD_MKDEVMAP_NOT_PRESENT
#define _PAGE_BIT_SOFTW4        57      /* available for programmer */
#define _PAGE_BIT_DEVMAP        _PAGE_BIT_SOFTW4

#if defined(CONFIG_X86_64) || defined(CONFIG_X86_PAE)
#define _PAGE_DEVMAP    (_AT(u64, 1) << _PAGE_BIT_DEVMAP)
#else
#define _PAGE_DEVMAP    (_AT(pteval_t, 0))
#endif
static inline pmd_t pmd_mkdevmap(pmd_t pmd)
{
	return pmd_set_flags(pmd, _PAGE_DEVMAP);
}
#endif

#endif /* _LINUX_PGTABLE_H */
