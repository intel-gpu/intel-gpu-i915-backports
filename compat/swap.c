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

#ifdef BPM_LRU_CACHE_ADD_API_NOT_PRESENT
void lru_cache_add(struct page *page)
{
       folio_add_lru(page_folio(page));
}
EXPORT_SYMBOL(lru_cache_add);
#endif
