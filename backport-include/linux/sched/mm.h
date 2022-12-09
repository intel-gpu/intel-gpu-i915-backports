/*
 * Copyright © 2022 Intel Corporation
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

#ifndef _BACKPORT_LINUX_SCHED_MM_H
#define _BACKPORT_LINUX_SCHED_MM_H
#include <linux/version.h>

#include_next <linux/sched/mm.h>

#ifdef BPM_MIGHT_ALLOC_NOT_PRESENT
#define might_alloc LINUX_I915_BACKPORT(might_alloc)
/**
 * might_alloc - Mark possible allocation sites
 * @gfp_mask: gfp_t flags that would be used to allocate
 *
 * Similar to might_sleep() and other annotations, this can be used in functions
 * that might allocate, but often don't. Compiles to nothing without
 * CONFIG_LOCKDEP. Includes a conditional might_sleep() if @gfp allows blocking.
 */

static inline void might_alloc(gfp_t gfp_mask)
{
        fs_reclaim_acquire(gfp_mask);
        fs_reclaim_release(gfp_mask);

        might_sleep_if(gfpflags_allow_blocking(gfp_mask));
}
#endif
#endif /* _BACKPORT_LINUX_SCHED_MM_H */
