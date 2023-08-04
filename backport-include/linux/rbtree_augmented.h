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

#ifndef _BACKPORT_LINUX_RBTREE_AUGMENTED_H
#define _BACKPORT_LINUX_RBTREE_AUGMENTED_H
#include <linux/version.h>
#include_next <linux/rbtree_augmented.h>

#ifdef BPM_RB_DECLARE_CALLBACKS_MAX_NOT_PRESENT
#define RB_DECLARE_CALLBACKS_MAX RB_DECLARE_CALLBACKS
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,18,0)
static inline void
rb_insert_augmented_cached(struct rb_node *node,
                           struct rb_root_cached *root, bool newleft,
                           const struct rb_augment_callbacks *augment)
{
	/* to do: implemented the function with full parameters */
	/* __rb_insert_augmented(node, &root->rb_root,
                                 newleft, &root->rb_leftmost, augment->rotate);*/
        __rb_insert_augmented(node, &root->rb_root, augment->rotate);
}
#endif
#endif
