#ifndef _BACKPORT_LINUX_WAIT_H
#define _BACKPORT_LINUX_WAIT_H

#include_next <linux/wait.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,18,0)
/*
 *  * A single wait-queue entry structure:
 *   */
struct wait_queue_entry {
	unsigned int		flags;
	void			*private;
	wait_queue_func_t	func;
	struct list_head	entry;
};
typedef struct __wait_queue wait_queue_entry_t;

static inline void __add_wait_queue_entry_tail( struct __wait_queue_head *wq_head, struct __wait_queue *wq_entry)
{
	list_add_tail(&wq_entry->task_list, &wq_head->task_list);
}

void init_wait_entry(struct __wait_queue *wq_entry, int flags);

long prepare_to_wait_event(wait_queue_head_t *q, wait_queue_t *wait, int state);

#endif
#endif
