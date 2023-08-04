/*
 * Copyright © 2015 Intel Corporation
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

#ifndef _BACKPORT_LINUX_KERNEL_H
#define _BACKPORT_LINUX_KERNEL_H

#include <linux/version.h>

#include_next <linux/kernel.h>

#ifdef BPM_LIMITS_H_NOT_PRESENT
#define SIZE_MAX        (~(size_t)0)
#endif

#ifndef DIV_ROUND_CLOSEST_ULL
/*
 * Same as above but for u64 dividends. divisor must be a 32-bit
 * number.
 */
#define DIV_ROUND_CLOSEST_ULL(x, divisor)(		\
{							\
	typeof(divisor) __d = divisor;			\
	unsigned long long _tmp = (x) + (__d) / 2;	\
	do_div(_tmp, __d);				\
	_tmp;						\
}							\
)
#endif

#ifndef U64_MAX
#define U64_MAX         ((u64)~0ULL)
#endif

#ifdef BPM_TYPEOF_MEMBER_NOT_PRESENT
#define typeof_member(T, m)     typeof(((T*)0)->m)
#endif

#ifdef BPM_PTR_ALIGN_DOWN_NOT_PRESENT
#define PTR_ALIGN_DOWN(p, a)    ((typeof(p))ALIGN_DOWN((unsigned long)(p), (a)))
#endif

#endif
