/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Written by Mark Hemment, 1996 (markhe@nextd.demon.co.uk).
 *
 * (C) SGI 2006, Christoph Lameter
 *      Cleaned up and restructured to ease the addition of alternative
 *      implementations of SLAB allocators.
 * (C) Linux Foundation 2008-2013
 *      Unified interface for all slab allocators
 */

#ifndef _BACKPORT_LINUX_SWAP_H
#define _BACKPORT_LINUX_SWAP_H
#include_next<linux/swap.h>
#ifdef BPM_LRU_CACHE_ADD_API_NOT_PRESENT
void lru_cache_add(struct page *page);
#endif
#endif
