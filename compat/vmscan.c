// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 *  Swap reorganised 29.12.95, Stephen Tweedie.
 *  kswapd added: 7.1.96  sct
 *  Removed kswapd_ctl limits, and swap out as many pages as needed
 *  to bring the system back to freepages.high: 2.4.97, Rik van Riel.
 *  Zone aware kswapd started 02/00, Kanoj Sarcar (kanoj@sgi.com).
 *  Multiqueue VM started 5.8.00, Rik van Riel.
 */

#include<linux/mm.h>
#ifdef BPM_REGISTER_SHRINKER_SECOND_ARG_NOT_PRESENT
#undef register_shrinker
int backport_register_shrinker(struct shrinker *shrinker)
{
         return register_shrinker(shrinker,"drm-i915_gem");
}
#define register_shrinker backport_register_shrinker
EXPORT_SYMBOL(register_shrinker);
#endif

#ifdef BPM_CHECK_MOVE_UNEVICTABLE_PAGES_NOT_PRESENT
#include<linux/swap.h>
void check_move_unevictable_pages(struct pagevec *pvec)
{
       struct folio_batch fbatch;
       unsigned i;

       folio_batch_init(&fbatch);
       for (i = 0; i < pvec->nr; i++) {
               struct page *page = pvec->pages[i];

               if (PageTransTail(page))
                       continue;
               folio_batch_add(&fbatch, page_folio(page));
       }
       check_move_unevictable_folios(&fbatch);
}
EXPORT_SYMBOL_GPL(check_move_unevictable_pages);
#endif
