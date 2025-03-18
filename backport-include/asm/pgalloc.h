/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __BACKPORT_ASM_PGALLOC_H
#define __BACKPORT_ASM_PGALLOC_H
#include_next <asm/pgalloc.h>

#ifdef BPM_PTE_ALLOC_ONE_NOT_PRESENT
#define GFP_PGTABLE_KERNEL      (GFP_KERNEL | __GFP_ZERO)
#define GFP_PGTABLE_USER        (GFP_PGTABLE_KERNEL | __GFP_ACCOUNT)

static inline pgtable_t __pte_alloc_one(struct mm_struct *mm, gfp_t gfp)
{
        struct page *pte;

        pte = alloc_page(gfp);
        if (!pte)
                return NULL;
        if (!pgtable_page_ctor(pte)) {
                __free_page(pte);
                return NULL;
        }

        return pte;
}
#endif
#endif /*  __BACKPORT_ASM_PGALLOC_H */
