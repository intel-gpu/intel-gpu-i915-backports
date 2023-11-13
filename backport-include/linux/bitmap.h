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

#endif /* __BACKPORT_BITMAP_H */
