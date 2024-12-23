/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BACKPORT_LINUX_PAGEMAP_H
#define _BACKPORT_LINUX_PAGEMAP_H

#include_next <linux/pagemap.h>

#ifdef BPM_PAGE_MAPPING_NOT_PRESENT
static inline struct address_space *page_mapping(struct page *page)
{
       return folio_mapping(page_folio(page));
}
#endif /* BPM_PAGE_MAPPING_NOT_PRESENT */
#endif
