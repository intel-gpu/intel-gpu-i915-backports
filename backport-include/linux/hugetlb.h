/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BACKPORT_LINUX_HUGETLB_H
#define _BACKPORT_LINUX_HUGETLB_H

#include_next <linux/hugetlb.h>

#ifdef BPM_PTEP_GET_LOCKLESS_NOT_PRESENT
#ifdef CONFIG_GUP_GET_PTE_LOW_HIGH
/*
 * WARNING: only to be used in the get_user_pages_fast() implementation.
 *
 * With get_user_pages_fast(), we walk down the pagetables without taking any
 * locks.  For this we would like to load the pointers atomically, but sometimes
 * that is not possible (e.g. without expensive cmpxchg8b on x86_32 PAE).  What
 * we do have is the guarantee that a PTE will only either go from not present
 * to present, or present to not present or both -- it will not switch to a
 * completely different present page without a TLB flush in between; something
 * that we are blocking by holding interrupts off.
 *
 * Setting ptes from not present to present goes:
 *
 *   ptep->pte_high = h;
 *   smp_wmb();
 *   ptep->pte_low = l;
 *
 * And present to not present goes:
 *
 *   ptep->pte_low = 0;
 *   smp_wmb();
 *   ptep->pte_high = 0;
 *
 * We must ensure here that the load of pte_low sees 'l' IFF pte_high sees 'h'.
 * We load pte_high *after* loading pte_low, which ensures we don't see an older
 * value of pte_high.  *Then* we recheck pte_low, which ensures that we haven't
 * picked up a changed pte high. We might have gotten rubbish values from
 * pte_low and pte_high, but we are guaranteed that pte_low will not have the
 * present bit set *unless* it is 'l'. Because get_user_pages_fast() only
 * operates on present ptes we're safe.
 */
static inline pte_t ptep_get_lockless(pte_t *ptep)
{
        pte_t pte;

        do {
                pte.pte_low = ptep->pte_low;
                smp_rmb();
                pte.pte_high = ptep->pte_high;
                smp_rmb();
        } while (unlikely(pte.pte_low != ptep->pte_low));

        return pte;
}
#else /* CONFIG_GUP_GET_PTE_LOW_HIGH */
/*
 * We require that the PTE can be read atomically.
 */
static inline pte_t ptep_get_lockless(pte_t *ptep)
{
        return READ_ONCE(*ptep);
}
#endif /* CONFIG_GUP_GET_PTE_LOW_HIGH */

#endif /* BPM_PTEP_GET_LOCKLESS_NOT_PRESENT */
#endif /* _BACKPORT_LINUX_HUGETLB_H */

