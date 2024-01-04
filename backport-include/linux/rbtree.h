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

#ifndef _BACKPORT_LINUX_RBTREE_H
#define _BACKPORT_LINUX_RBTREE_H
#include <linux/version.h>
#include_next <linux/rbtree.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,18,0)
struct rb_root_cached {
        struct rb_root rb_root;
        struct rb_node *rb_leftmost;
};
#endif
#define RB_ROOT_CACHED (struct rb_root_cached) { {NULL, }, NULL }

#ifdef RB_FIND_NOT_PRESENT
#define rb_find_add LINUX_I915_BACKPORT(rb_find_add)
/**
 * rb_find_add() - find equivalent @node in @tree, or add @node
 * @node: node to look-for / insert
 * @tree: tree to search / modify
 * @cmp: operator defining the node order
 *
 * Returns the rb_node matching @node, or NULL when no match is found and @node
 * is inserted.
 */
static __always_inline struct rb_node *
rb_find_add(struct rb_node *node, struct rb_root *tree,
            int (*cmp)(struct rb_node *, const struct rb_node *))
{
        struct rb_node **link = &tree->rb_node;
        struct rb_node *parent = NULL;
        int c;

        while (*link) {
                parent = *link;
                c = cmp(node, parent);

                if (c < 0)
                        link = &parent->rb_left;
                else if (c > 0)
                        link = &parent->rb_right;
                else
                        return parent;
        }

        rb_link_node(node, parent, link);
        rb_insert_color(node, tree);
        return NULL;
}

#define rb_find LINUX_I915_BACKPORT(rb_find)
/**
 * rb_find() - find @key in tree @tree
 * @key: key to match
 * @tree: tree to search
 * @cmp: operator defining the node order
 *
 * Returns the rb_node matching @key or NULL.
 */
static __always_inline struct rb_node *
rb_find(const void *key, const struct rb_root *tree,
        int (*cmp)(const void *key, const struct rb_node *))
{
        struct rb_node *node = tree->rb_node;

        while (node) {
                int c = cmp(key, node);

                if (c < 0)
                        node = node->rb_left;
                else if (c > 0)
                        node = node->rb_right;
                else
                        return node;
        }

        return NULL;
}

#endif /* RB_FIND_NOT_PRESENT */
#endif
