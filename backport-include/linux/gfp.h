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

#ifndef _BACKPORT_LINUX_GFP_H
#define _BACKPORT_LINUX_GFP_H
#include <linux/version.h>
#include_next <linux/gfp.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,18,0)
#define ___GFP_DIRECT_RECLAIM   0x400000u
#define __GFP_DIRECT_RECLAIM    ((__force gfp_t)___GFP_DIRECT_RECLAIM) /* Caller can reclaim */
#define ___GFP_RETRY_MAYFAIL   0x400u
#define __GFP_RETRY_MAYFAIL     ((__force gfp_t)___GFP_RETRY_MAYFAIL)

#if RHEL_RELEASE_CODE != RHEL_RELEASE_VERSION(7,5)
static inline bool gfpflags_allow_blocking(const gfp_t gfp_flags)
{
        return !!(gfp_flags & __GFP_DIRECT_RECLAIM);
}
#endif

#ifndef __GFP_RECLAIM
#define __GFP_RECLAIM __GFP_WAIT
#endif
#endif
#endif
