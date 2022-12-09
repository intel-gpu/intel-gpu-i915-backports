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

#include <linux/module.h>
#include <linux/scatterlist.h>

#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/idr.h>
#include <linux/radix-tree.h>

#ifdef CONFIG_DEBUG_OBJECTS_WORK
void destroy_delayed_work_on_stack(struct delayed_work *work)
{
        destroy_timer_on_stack(&work->timer);
        debug_object_free(&work->work, &work_debug_descr);
}
EXPORT_SYMBOL_GPL(destroy_delayed_work_on_stack);
#endif

static __always_inline long __get_user_pages_locked(struct task_struct *tsk,
						struct mm_struct *mm,
						unsigned long start,
						unsigned long nr_pages,
						struct page **pages,
						struct vm_area_struct **vmas,
						int *locked, bool notify_drop,
						unsigned int flags)
{
	long ret, pages_done;
	bool lock_dropped;

	if (locked) {
		/* if VM_FAULT_RETRY can be returned, vmas become invalid */
		BUG_ON(vmas);
		/* check caller initialized locked */
		BUG_ON(*locked != 1);
	}

	if (pages)
		flags |= FOLL_GET;

	pages_done = 0;
	lock_dropped = false;
	for (;;) {
		ret = __get_user_pages(tsk, mm, start, nr_pages, flags, pages,
				       vmas, locked);
		if (!locked)
			/* VM_FAULT_RETRY couldn't trigger, bypass */
			return ret;

		/* VM_FAULT_RETRY cannot return errors */
		if (!*locked) {
			BUG_ON(ret < 0);
			BUG_ON(ret >= nr_pages);
		}

		if (!pages)
			/* If it's a prefault don't insist harder */
			return ret;

		if (ret > 0) {
			nr_pages -= ret;
			pages_done += ret;
			if (!nr_pages)
				break;
		}
		if (*locked) {
			/* VM_FAULT_RETRY didn't trigger */
			if (!pages_done)
				pages_done = ret;
			break;
		}
		/* VM_FAULT_RETRY triggered, so seek to the faulting offset */
		pages += ret;
		start += ret << PAGE_SHIFT;

		/*
 * 		 * Repeat on the address that fired VM_FAULT_RETRY
 * 		 		 * without FAULT_FLAG_ALLOW_RETRY but with
 * 		 		 		 * FAULT_FLAG_TRIED.
 * 		 		 		 		 */
		*locked = 1;
		lock_dropped = true;
		down_read(&mm->mmap_sem);
		ret = __get_user_pages(tsk, mm, start, 1, flags | FOLL_TRIED,
				       pages, NULL, NULL);
		if (ret != 1) {
			BUG_ON(ret > 1);
			if (!pages_done)
				pages_done = ret;
			break;
		}
		nr_pages--;
		pages_done++;
		if (!nr_pages)
			break;
		pages++;
		start += PAGE_SIZE;
	}
	if (notify_drop && lock_dropped && *locked) {
		/*
 * 		 * We must let the caller know we temporarily dropped the lock
 * 		 		 * and so the critical section protected by it was lost.
 * 		 		 		 */
		up_read(&mm->mmap_sem);
		*locked = 0;
	}
	return pages_done;
}
#if REDHAT_RELEASE_VERSION_IS_NOT_EQL(7,5)
long get_user_pages_remote(struct task_struct *tsk, struct mm_struct *mm,
		unsigned long start, unsigned long nr_pages,
		unsigned int gup_flags, struct page **pages,
		struct vm_area_struct **vmas, int *locked)
{
	return __get_user_pages_locked(tsk, mm, start, nr_pages, pages, vmas,
				       locked, true,
				       gup_flags | FOLL_TOUCH | FOLL_REMOTE);
}
EXPORT_SYMBOL(get_user_pages_remote);
#endif

/**
 *  * radix_tree_iter_delete - delete the entry at this iterator position
 *   * @root: radix tree root
 *    * @iter: iterator state
 *     * @slot: pointer to slot
 *      *
 *       * Delete the entry at the position currently pointed to by the iterator.
 *        * This may result in the current node being freed; if it is, the iterator
 *         * is advanced so that it will not reference the freed memory.  This
 *          * function may be called without any locking if there are no other threads
 *           * which can access this tree.
 *            */
void radix_tree_iter_delete(struct radix_tree_root *root,
                                struct radix_tree_iter *iter, void __rcu **slot)
{
 	void *entry;
	entry = radix_tree_delete(root, iter->index);
	return;

}
EXPORT_SYMBOL(radix_tree_iter_delete);

void init_wait_entry(struct __wait_queue *wq_entry, int flags)
{
        wq_entry->flags = flags;
        wq_entry->private = current;
        wq_entry->func = autoremove_wake_function;
        INIT_LIST_HEAD(&wq_entry->task_list);
}
EXPORT_SYMBOL(init_wait_entry);

void *__vmalloc_node_flags_caller(unsigned long size, int node, gfp_t flags,
				  void *caller)
{
	/*return __vmalloc_node(size, 1, flags, PAGE_KERNEL, node, caller);*/
	return __vmalloc(size,flags,PAGE_KERNEL);
}

void *kvmalloc_node(size_t size, gfp_t flags, int node)
{
	gfp_t kmalloc_flags = flags;
	void *ret;

	/*
  	 * vmalloc uses GFP_KERNEL for some internal allocations (e.g page tables)
  	 * so the given set of flags has to be compatible.
   	 */
	WARN_ON_ONCE((flags & GFP_KERNEL) != GFP_KERNEL);

	/*
 	 * We want to attempt a large physically contiguous block first because
 	 * it is less likely to fragment multiple larger blocks and therefore
 	 * contribute to a long term fragmentation less than vmalloc fallback.
 	 * However make sure that larger requests are not too disruptive - no
	 * OOM killer and no allocation failure warnings as we have a fallback.
 	 */
	if (size > PAGE_SIZE) {
		kmalloc_flags |= __GFP_NOWARN;

		if (!(kmalloc_flags & __GFP_RETRY_MAYFAIL))
			kmalloc_flags |= __GFP_NORETRY;
	}

	ret = kmalloc_node(size, kmalloc_flags, node);

	/*
 	 * It doesn't really make sense to fallback to vmalloc for sub page
 	 * requests
 	 */
	if (ret || size <= PAGE_SIZE)
		return ret;

	return __vmalloc_node_flags_caller(size, node, flags,
			__builtin_return_address(0));
}
#if REDHAT_RELEASE_VERSION_IS_NOT_EQL(7,5)
EXPORT_SYMBOL(kvmalloc_node);
#endif
