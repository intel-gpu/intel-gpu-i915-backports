/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BACKPORT_LINUX_LIST_H
#define _BACKPORT_LINUX_LIST_H
#include <linux/types.h>
#include <linux/kernel.h>
#include_next <linux/list.h>

#ifdef BPM_LIST_FOR_EACH_CONTINUE_NOT_PRESENT

/**
 * list_for_each_continue - continue iteration over a list
 * @pos:        the &struct list_head to use as a loop cursor.
 * @head:       the head for your list.
 *
 * Continue to iterate over a list, continuing after the current position.
 */
#define list_for_each_continue(pos, head) \
        for (pos = pos->next; pos != (head); pos = pos->next)

#endif 

#ifdef BPM_LIST_IS_HEAD_NOT_PRESENT

/**
 * list_is_head - tests whether @list is the list @head
 * @list: the entry to test
 * @head: the head of the list
 */
static inline int list_is_head(const struct list_head *list, const struct list_head *head)
{
	return list == head;
}
#endif

#ifdef BPM_LIST_ENTRY_IS_HEAD_NOT_PRESENT
#define list_entry_is_head(pos, head, member)                           \
	(&pos->member == (head))
#endif

#endif /* _BACKPORT_LINUX_LIST_H */

