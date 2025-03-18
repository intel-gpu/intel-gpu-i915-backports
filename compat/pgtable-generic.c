// SPDX-License-Identifier: GPL-2.0
/*
 *  mm/pgtable-generic.c
 *
 *  Generic pgtable methods declared in linux/pgtable.h
 *
 *  Copyright (C) 2010  Linus Torvalds
 */

#include <linux/pagemap.h>
#include <linux/hugetlb.h>
#include <linux/pgtable.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/mm_inline.h>
#include <linux/mm.h>

#ifdef BPM_PTE_OFFSET_MAP_LOCK_NOT_PRESENT
pte_t *__pte_offset_map_lock(struct mm_struct *mm, pmd_t *pmd,
                             unsigned long addr, spinlock_t **ptlp)
{
        spinlock_t *ptl;
        pmd_t pmdval;
        pte_t *pte;
again:
	pmdval = pmdp_get_lockless(pmd);
        pte = pte_offset_map(pmd, addr);
        if (unlikely(!pte))
                return pte;
        ptl = pte_lockptr(mm, &pmdval);
        spin_lock(ptl);
        if (likely(pmd_same(pmdval, pmdp_get_lockless(pmd)))) {
                *ptlp = ptl;
                return pte;
        }
        pte_unmap_unlock(pte, ptl);
        goto again;
}
EXPORT_SYMBOL(__pte_offset_map_lock);
#endif
