// SPDX-License-Identifier: GPL-2.0-only
#include <linux/mm.h>

#ifdef BPM_UNPIN_USER_PAGES_DIRTY_LOCK_NOT_PRESENT
void unpin_user_page_range_dirty_lock(struct page *page, unsigned long npages,
                bool make_dirty)
{
        do {
                unpin_user_pages_dirty_lock(&page, 1, make_dirty);
        } while(page++, --npages);
}
EXPORT_SYMBOL(unpin_user_page_range_dirty_lock);
#endif
