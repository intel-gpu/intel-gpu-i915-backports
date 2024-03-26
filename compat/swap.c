// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/mm/swap.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 */

/*
 * This file contains the default values for the operation of the
 * Linux VM subsystem. Fine-tuning documentation can be found in
 * Documentation/admin-guide/sysctl/vm.rst.
 * Started 18.12.91
 * Swap aging added 23.2.95, Stephen Tweedie.
 * Buffermem limits added 12.3.98, Rik van Riel.
 */

#include <linux/pagemap.h>
#include <linux/swap.h>

#ifdef BPM_LRU_CACHE_ADD_WRAPPER_NOT_PRESENT
void lru_cache_add(struct page *page)
{
	folio_add_lru(page_folio(page));
}
EXPORT_SYMBOL_GPL(lru_cache_add);
#endif

#ifdef BPM_LRU_CACHE_ADD_EXPORT_NOT_PRESENT
static DEFINE_PER_CPU(struct pagevec, lru_add_pvec);
void lru_cache_add(struct page *page)
{
	VM_BUG_ON_PAGE(PageActive(page) && PageUnevictable(page), page);
	VM_BUG_ON_PAGE(PageLRU(page), page);
	struct pagevec *pvec = &get_cpu_var(lru_add_pvec);
	get_page(page);
	if (!pagevec_add(pvec, page) || PageCompound(page))
		__pagevec_lru_add(pvec);
	put_cpu_var(lru_add_pvec);
}
EXPORT_SYMBOL_GPL(lru_cache_add);
#endif
#ifdef BPM_PAGEVEC_NOT_PRESENT
void __pagevec_release(struct pagevec *pvec)
{
	__folio_batch_release((struct folio_batch*)pvec);
}
EXPORT_SYMBOL(__pagevec_release);
#endif
