/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _BACKPORT_LINUX_BITMAP_H
#define _BACKPORT_LINUX_BITMAP_H
#include_next <linux/bitmap.h>

/* bitmap_for_each_clear_region introduced in 5.6 version */
#ifdef BPM_BITMAP_CLEAR_REGION_NOT_PRESENT

static inline void bitmap_next_clear_region(unsigned long *bitmap,
                                            unsigned int *rs, unsigned int *re,
                                            unsigned int end)
{
        *rs = find_next_zero_bit(bitmap, end, *rs);
        *re = find_next_bit(bitmap, end, *rs + 1);
}

/*
 * Bitmap region iterators.  Iterates over the bitmap between [@start, @end).
 * @rs and @re should be integer variables and will be set to start and end
 * index of the current clear or set region.
 */
#define bitmap_for_each_clear_region(bitmap, rs, re, start, end)             \
        for ((rs) = (start),                                                 \
             bitmap_next_clear_region((bitmap), &(rs), &(re), (end));        \
             (rs) < (re);                                                    \
             (rs) = (re) + 1,                                                \
             bitmap_next_clear_region((bitmap), &(rs), &(re), (end)))

#endif
#endif /* _BACKPORT_LINUX_BITMAP_H */
