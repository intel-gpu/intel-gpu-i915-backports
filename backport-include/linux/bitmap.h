/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __BACKPORT_BITMAP_H
#define __BACKPORT_BITMAP_H
#include_next <linux/bitmap.h>
#include <linux/version.h>

#ifdef BPM_BITMAP_WEIGHT_RETURN_TYPE_CHANGED
#define bitmap_weight LINUX_I915_BACKPORT(bitmap_weight)
static __always_inline
unsigned int bitmap_weight(const unsigned long *src, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return hweight_long(*src & BITMAP_LAST_WORD_MASK(nbits));
	return __bitmap_weight(src, nbits);
}
#endif

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

#ifdef BPM_BITMAP_FOR_REGION_NOT_PRESENT
#define bitmap_for_each_clear_region(bitmap, rs, re, start, end) \
	for_each_clear_bitrange(rs, re, bitmap, end)
#endif

#endif /* __BACKPORT_BITMAP_H */
