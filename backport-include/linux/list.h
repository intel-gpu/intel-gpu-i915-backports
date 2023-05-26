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
#endif /* _BACKPORT_LINUX_LIST_H */

