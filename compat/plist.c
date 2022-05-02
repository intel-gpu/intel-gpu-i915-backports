#include <linux/bug.h>
#include <linux/plist.h>

#ifdef CONFIG_DEBUG_PLIST
static void plist_check_head(struct plist_head *head)
{
        if (!plist_head_empty(head))
                plist_check_list(&plist_first(head)->prio_list);
        plist_check_list(&head->node_list);
}

#else
#define plist_check_head(h)    do { } while (0)
#endif


/**
 * plist_add - add @node to @head
 *
 * @node:       &struct plist_node pointer
 * @head:       &struct plist_head pointer
 */
void plist_add(struct plist_node *node, struct plist_head *head)
{
        struct plist_node *first, *iter, *prev = NULL;
        struct list_head *node_next = &head->node_list;

        plist_check_head(head);
        WARN_ON(!plist_node_empty(node));
        WARN_ON(!list_empty(&node->prio_list));

        if (plist_head_empty(head))
                goto ins_node;

        first = iter = plist_first(head);

        do {
                if (node->prio < iter->prio) {
                        node_next = &iter->node_list;
                        break;
                }

                prev = iter;
                iter = list_entry(iter->prio_list.next,
                                struct plist_node, prio_list);
        } while (iter != first);

        if (!prev || prev->prio != node->prio)
                list_add_tail(&node->prio_list, &iter->prio_list);
ins_node:
        list_add_tail(&node->node_list, node_next);

        plist_check_head(head);
}
EXPORT_SYMBOL_GPL(plist_add);

/**
 * plist_del - Remove a @node from plist.
 *
 * @node:       &struct plist_node pointer - entry to be removed
 * @head:       &struct plist_head pointer - list head
 */
void plist_del(struct plist_node *node, struct plist_head *head)
{
        plist_check_head(head);

        if (!list_empty(&node->prio_list)) {
                if (node->node_list.next != &head->node_list) {
                        struct plist_node *next;

                        next = list_entry(node->node_list.next,
                                        struct plist_node, node_list);

                        /* add the next plist_node into prio_list */
                        if (list_empty(&next->prio_list))
                                list_add(&next->prio_list, &node->prio_list);
                }
                list_del_init(&node->prio_list);
        }

        list_del_init(&node->node_list);

        plist_check_head(head);
}
EXPORT_SYMBOL_GPL(plist_del);

