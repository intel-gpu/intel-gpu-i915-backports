#ifndef __BACKPORT_LIST_H
#define __BACKPORT_LIST_H
#include_next <linux/list.h>
#include <linux/version.h>

#if LINUX_VERSION_IS_LESS(3,9,0)
/**
 * backport:
 *
 * commit 0bbacca7c3911451cea923b0ad6389d58e3d9ce9
 * Author: Sasha Levin <sasha.levin@oracle.com>
 * Date:   Thu Feb 7 12:32:18 2013 +1100
 *
 *     hlist: drop the node parameter from iterators
 */
#include <backport/magic.h>

#undef hlist_entry_safe
#define hlist_entry_safe(ptr, type, member) \
	({ typeof(ptr) ____ptr = (ptr); \
	   ____ptr ? hlist_entry(____ptr, type, member) : NULL; \
	})

#define hlist_for_each_entry4(tpos, pos, head, member)			\
	for (pos = (head)->first;					\
	     pos &&							\
		({ tpos = hlist_entry(pos, typeof(*tpos), member); 1;});\
	     pos = pos->next)

#define hlist_for_each_entry_safe5(tpos, pos, n, head, member)		\
	for (pos = (head)->first;					\
	     pos && ({ n = pos->next; 1; }) &&				\
		({ tpos = hlist_entry(pos, typeof(*tpos), member); 1;});\
	     pos = n)

#define hlist_for_each_entry3(pos, head, member)				\
	for (pos = hlist_entry_safe((head)->first, typeof(*(pos)), member);	\
	     pos;								\
	     pos = hlist_entry_safe((pos)->member.next, typeof(*(pos)), member))

#define hlist_for_each_entry_safe4(pos, n, head, member) 			\
	for (pos = hlist_entry_safe((head)->first, typeof(*pos), member);	\
	     pos && ({ n = pos->member.next; 1; });				\
	     pos = hlist_entry_safe(n, typeof(*pos), member))

#undef hlist_for_each_entry
#define hlist_for_each_entry(...) \
	macro_dispatcher(hlist_for_each_entry, __VA_ARGS__)(__VA_ARGS__)
#undef hlist_for_each_entry_safe
#define hlist_for_each_entry_safe(...) \
	macro_dispatcher(hlist_for_each_entry_safe, __VA_ARGS__)(__VA_ARGS__)

#endif

#ifndef list_first_entry_or_null
/**
 * list_first_entry_or_null - get the first element from a list
 * @ptr:	the list head to take the element from.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the list_struct within the struct.
 *
 * Note that if the list is empty, it returns NULL.
 */
#define list_first_entry_or_null(ptr, type, member) \
	(!list_empty(ptr) ? list_first_entry(ptr, type, member) : NULL)
#endif /* list_first_entry_or_null */

#ifndef list_next_entry
/**
 * list_next_entry - get the next element in list
 * @pos:	the type * to cursor
 * @member:	the name of the list_struct within the struct.
 */
#define list_next_entry(pos, member) \
	list_entry((pos)->member.next, typeof(*(pos)), member)
#endif /* list_next_entry */

#ifndef list_last_entry
/**
 * list_last_entry - get the last element from a list
 * @ptr:	the list head to take the element from.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the list_struct within the struct.
 *
 * Note, that list is expected to be not empty.
 */
#define list_last_entry(ptr, type, member) \
	list_entry((ptr)->prev, type, member)
#endif

#if LINUX_VERSION_IS_LESS(5,1,0)
/**
 * list_is_first -- tests whether @list is the first entry in list @head
 * @list: the entry to test
 * @head: the head of the list
 */
static inline int list_is_first(const struct list_head *list,
					const struct list_head *head)
{
	return list->prev == head;
}

/*
 * Delete a list entry and clear the 'prev' pointer.
 *
 * This is a special-purpose list clearing method used in the networking code
 * for lists allocated as per-cpu, where we don't want to incur the extra
 * WRITE_ONCE() overhead of a regular list_del_init(). The code that uses this
 * needs to check the node 'prev' pointer instead of calling list_empty().
 */
static inline void __list_del_clearprev(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
	entry->prev = NULL;
}

/**
 * list_swap - replace entry1 with entry2 and re-add entry1 at entry2's position
 * @entry1: the location to place entry2
 * @entry2: the location to place entry1
 */
static inline void list_swap(struct list_head *entry1,
			     struct list_head *entry2)
{
	struct list_head *pos = entry2->prev;

	list_del(entry2);
	list_replace(entry1, entry2);
	if (pos == entry1)
		pos = entry2;
	list_add(entry1, pos);
}

/**
 * list_rotate_to_front() - Rotate list to specific item.
 * @list: The desired new front of the list.
 * @head: The head of the list.
 *
 * Rotates list so that @list becomes the new front of the list.
 */
static inline void list_rotate_to_front(struct list_head *list,
					struct list_head *head)
{
	/*
	 * Deletes the list head from the list denoted by @head and
	 * places it as the tail of @list, this effectively rotates the
	 * list so that @list is at the front.
	 */
	list_move_tail(head, list);
}


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
   
#endif /* __BACKPORT_LIST_H */
