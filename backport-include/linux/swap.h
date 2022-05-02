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
 * Authors:
 *    Owen Zhang <owen.zhang@intel.com>
 *
 */

#ifndef _BACKPORT_LINUX_SWAP_H
#define _BACKPORT_LINUX_SWAP_H
#include <linux/version.h>
#include <linux/pagemap.h>
#include <linux/pagevec.h>
#include <backport/backport.h>

#include_next <linux/swap.h>

#define check_move_unevictable_pages LINUX_I915_BACKPORT(check_move_unevictable_pages)
#if RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(8,5)
void check_move_unevictable_pages(struct page **pages, int nr_pages);
#else
void check_move_unevictable_pages(struct pagevec *pvec, int nr_pages);
#endif /* RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(8,5) */
#endif /* _BACKPORT_LINUX_SWAP_H */
