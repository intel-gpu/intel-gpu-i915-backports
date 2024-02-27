/*
 * Copyright _ 2018 Intel Corporation
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
#ifndef __BACKPORT_LINUX_WAIT_H
#define __BACKPORT_LINUX_WAIT_H
#include_next <linux/wait.h>

#if LINUX_VERSION_IS_LESS(3,17,0)
extern int bit_wait(void *);
extern int bit_wait_io(void *);

static inline int
backport_wait_on_bit(void *word, int bit, unsigned mode)
{
	return wait_on_bit(word, bit, bit_wait, mode);
}

static inline int
backport_wait_on_bit_io(void *word, int bit, unsigned mode)
{
	return wait_on_bit(word, bit, bit_wait_io, mode);
}

#define wait_on_bit LINUX_I915_BACKPORT(wait_on_bit)
#define wait_on_bit_io LINUX_I915_BACKPORT(wait_on_bit_io)

#endif

#if LINUX_VERSION_IS_LESS(3,18,12)
#define WQ_FLAG_WOKEN		0x02

#define wait_woken LINUX_I915_BACKPORT(wait_woken)
long wait_woken(wait_queue_t *wait, unsigned mode, long timeout);
#define wait_woken LINUX_I915_BACKPORT(wait_woken)
int woken_wake_function(wait_queue_t *wait, unsigned mode, int sync, void *key);
#endif

/**
 * For wait_on_bit_timeout() an extra member in struct wait_bit_key is needed.
 * This was introuced in kernel 3.17 and we are only able to backport this
 * function on these kernel versions.
 */
#if LINUX_VERSION_IS_GEQ(3,17,0)
#if LINUX_VERSION_IS_LESS(3,18,0)
#define out_of_line_wait_on_bit_timeout LINUX_I915_BACKPORT(out_of_line_wait_on_bit_timeout)
int out_of_line_wait_on_bit_timeout(void *, int, wait_bit_action_f *, unsigned, unsigned long);

#define bit_wait_timeout LINUX_I915_BACKPORT(bit_wait_timeout)
extern int bit_wait_timeout(struct wait_bit_key *);
#endif

#if LINUX_VERSION_IS_LESS(3,20,0)
#define wait_on_bit_timeout LINUX_I915_BACKPORT(wait_on_bit_timeout)
/**
 * wait_on_bit_timeout - wait for a bit to be cleared or a timeout elapses
 * @word: the word being waited on, a kernel virtual address
 * @bit: the bit of the word being waited on
 * @mode: the task state to sleep in
 * @timeout: timeout, in jiffies
 *
 * Use the standard hashed waitqueue table to wait for a bit
 * to be cleared. This is similar to wait_on_bit(), except also takes a
 * timeout parameter.
 *
 * Returned value will be zero if the bit was cleared before the
 * @timeout elapsed, or non-zero if the @timeout elapsed or process
 * received a signal and the mode permitted wakeup on that signal.
 */
static inline int
wait_on_bit_timeout(void *word, int bit, unsigned mode, unsigned long timeout)
{
	might_sleep();
	if (!test_bit(bit, word))
		return 0;
	return out_of_line_wait_on_bit_timeout(word, bit,
					       bit_wait_timeout,
					       mode, timeout);
}
#endif
#endif

#if LINUX_VERSION_IS_LESS(4,13,0)
#define wait_queue_entry_t wait_queue_t

#define wait_event_killable_timeout(wq_head, condition, timeout)	\
({									\
	long __ret = timeout;						\
	might_sleep();							\
	if (!___wait_cond_timeout(condition))				\
		__ret = __wait_event_killable_timeout(wq_head,		\
						condition, timeout);	\
	__ret;								\
})

#define __wait_event_killable_timeout(wq_head, condition, timeout)	\
	___wait_event(wq_head, ___wait_cond_timeout(condition),		\
		      TASK_KILLABLE, 0, timeout,			\
		      __ret = schedule_timeout(__ret))
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,18,0)
/*
 * A single wait-queue entry structure:
 */
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
#endif /* __BACKPORT_LINUX_WAIT_H */
