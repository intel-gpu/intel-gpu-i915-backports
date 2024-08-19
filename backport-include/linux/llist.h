/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _BACKPORT_LINUX_LLIST_H
#define _BACKPORT_LINUX_LLIST_H
#include <linux/version.h>
#include_next <linux/llist.h>

#ifdef BPM_LLIST_ADD_NOT_PRESENT

static inline bool __llist_add_batch(struct llist_node *new_first,
                                     struct llist_node *new_last,
                                     struct llist_head *head)
{
        new_last->next = head->first;
        head->first = new_first;
        return new_last->next == NULL;
}

static inline bool __llist_add(struct llist_node *new, struct llist_head *head)
{
        return __llist_add_batch(new, new, head);
}

static inline struct llist_node *__llist_del_all(struct llist_head *head)
{
	        struct llist_node *first = head->first;

		        head->first = NULL;
			        return first;
}
#endif
#endif /* _BACKPORT_LINUX_LLIST_H */
