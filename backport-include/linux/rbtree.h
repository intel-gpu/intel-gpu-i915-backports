#ifndef __BACKPORT_RBTREE_H
#define __BACKPORT_RBTREE_H
#include_next <linux/rbtree.h>

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
#endif /* __BACKPORT_RBTREE_H */
