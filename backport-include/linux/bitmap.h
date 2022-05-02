/*
 * Copyright © 2021 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 *
 */

#ifndef _BACKPORT_LINUX_BITMAP_H
#define _BACKPORT_LINUX_BITMAP_H
#include_next <linux/bitmap.h>

/* bitmap_for_each_clear_region introduced in 5.6 version */
#if LINUX_VERSION_IS_LESS(5,6,0)

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

#endif /* LINUX_VERSION_IS_LESS (5,6,0) */
#endif /* _BACKPORT_LINUX_BITMAP_H  */
