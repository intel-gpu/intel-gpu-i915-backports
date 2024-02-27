/*
 * Copyright (c) 2013  Luis R. Rodriguez <mcgrof@do-not-panic.com>
 *
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
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/proc_fs.h>
#include <linux/random.h>
#include <linux/tty.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/of.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/idr.h>
#include <linux/radix-tree.h>

void proc_set_size(struct proc_dir_entry *de, loff_t size)
{
	de->size = size;
}
EXPORT_SYMBOL_GPL(proc_set_size);

void proc_set_user(struct proc_dir_entry *de, kuid_t uid, kgid_t gid)
{
	de->uid = uid;
	de->gid = gid;
}
EXPORT_SYMBOL_GPL(proc_set_user);

/* get_random_int() was not exported for module use until 3.10-rc.
   Implement it here in terms of the more expensive get_random_bytes()
 */
unsigned int get_random_int(void)
{
	unsigned int r;
	get_random_bytes(&r, sizeof(r));

	return r;
}
EXPORT_SYMBOL_GPL(get_random_int);

#ifdef CONFIG_TTY
/**
 * tty_port_tty_wakeup - helper to wake up a tty
 *
 * @port: tty port
 */
void tty_port_tty_wakeup(struct tty_port *port)
{
	struct tty_struct *tty = tty_port_tty_get(port);

	if (tty) {
		tty_wakeup(tty);
		tty_kref_put(tty);
	}
}
EXPORT_SYMBOL_GPL(tty_port_tty_wakeup);

/**
 * tty_port_tty_hangup - helper to hang up a tty
 *
 * @port: tty port
 * @check_clocal: hang only ttys with CLOCAL unset?
 */
void tty_port_tty_hangup(struct tty_port *port, bool check_clocal)
{
	struct tty_struct *tty = tty_port_tty_get(port);

	if (tty && (!check_clocal || !C_CLOCAL(tty)))
		tty_hangup(tty);
	tty_kref_put(tty);
}
EXPORT_SYMBOL_GPL(tty_port_tty_hangup);
#endif /* CONFIG_TTY */

#ifdef CONFIG_PCI_IOV
/*
 * pci_vfs_assigned - returns number of VFs are assigned to a guest
 * @dev: the PCI device
 *
 * Returns number of VFs belonging to this device that are assigned to a guest.
 * If device is not a physical function returns -ENODEV.
 */
int pci_vfs_assigned(struct pci_dev *dev)
{
	struct pci_dev *vfdev;
	unsigned int vfs_assigned = 0;
	unsigned short dev_id;

	/* only search if we are a PF */
	if (!dev->is_physfn)
		return 0;

	/*
	 * determine the device ID for the VFs, the vendor ID will be the
	 * same as the PF so there is no need to check for that one
	 */
	pci_read_config_word(dev, dev->sriov->pos + PCI_SRIOV_VF_DID, &dev_id);

	/* loop through all the VFs to see if we own any that are assigned */
	vfdev = pci_get_device(dev->vendor, dev_id, NULL);
	while (vfdev) {
		/*
		 * It is considered assigned if it is a virtual function with
		 * our dev as the physical function and the assigned bit is set
		 */
		if (vfdev->is_virtfn && (vfdev->physfn == dev) &&
		    (vfdev->dev_flags & PCI_DEV_FLAGS_ASSIGNED))
			vfs_assigned++;

		vfdev = pci_get_device(dev->vendor, dev_id, vfdev);
	}

	return vfs_assigned;
}
EXPORT_SYMBOL_GPL(pci_vfs_assigned);
#endif /* CONFIG_PCI_IOV */

#ifdef CONFIG_OF
/**
 * of_property_read_u32_index - Find and read a u32 from a multi-value property.
 *
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @index:	index of the u32 in the list of values
 * @out_value:	pointer to return value, modified only if no error.
 *
 * Search for a property in a device node and read nth 32-bit value from
 * it. Returns 0 on success, -EINVAL if the property does not exist,
 * -ENODATA if property does not have a value, and -EOVERFLOW if the
 * property data isn't large enough.
 *
 * The out_value is modified only if a valid u32 value can be decoded.
 */
int of_property_read_u32_index(const struct device_node *np,
				       const char *propname,
				       u32 index, u32 *out_value)
{
	const u32 *val = of_find_property_value_of_size(np, propname,
					((index + 1) * sizeof(*out_value)));

	if (IS_ERR(val))
		return PTR_ERR(val);

	*out_value = be32_to_cpup(((__be32 *)val) + index);
	return 0;
}
EXPORT_SYMBOL_GPL(of_property_read_u32_index);
#endif /* CONFIG_OF */

static inline void set_page_count(struct page *page, int v)
{
	atomic_set(&page->_count, v);
}

/*
 * Turn a non-refcounted page (->_count == 0) into refcounted with
 * a count of one.
 */
static inline void set_page_refcounted(struct page *page)
{
	VM_BUG_ON(PageTail(page));
	VM_BUG_ON(atomic_read(&page->_count));
	set_page_count(page, 1);
}

/*
 * split_page takes a non-compound higher-order page, and splits it into
 * n (1<<order) sub-pages: page[0..n]
 * Each sub-page must be freed individually.
 *
 * Note: this is probably too low level an operation for use in drivers.
 * Please consult with lkml before using this in your driver.
 */
void split_page(struct page *page, unsigned int order)
{
	int i;

	VM_BUG_ON(PageCompound(page));
	VM_BUG_ON(!page_count(page));

#ifdef CONFIG_KMEMCHECK
	/*
	 * Split shadow pages too, because free(page[0]) would
	 * otherwise free the whole shadow.
	 */
	if (kmemcheck_page_is_tracked(page))
		split_page(virt_to_page(page[0].shadow), order);
#endif

	for (i = 1; i < (1 << order); i++)
		set_page_refcounted(page + i);
}
EXPORT_SYMBOL_GPL(split_page);

struct action_devres {
	void *data;
	void (*action)(void *);
};

static void devm_action_release(struct device *dev, void *res)
{
	struct action_devres *devres = res;

	devres->action(devres->data);
}

/**
 * devm_add_action() - add a custom action to list of managed resources
 * @dev: Device that owns the action
 * @action: Function that should be called
 * @data: Pointer to data passed to @action implementation
 *
 * This adds a custom action to the list of managed resources so that
 * it gets executed as part of standard resource unwinding.
 */
int devm_add_action(struct device *dev, void (*action)(void *), void *data)
{
	struct action_devres *devres;

	devres = devres_alloc(devm_action_release,
			      sizeof(struct action_devres), GFP_KERNEL);
	if (!devres)
		return -ENOMEM;

	devres->data = data;
	devres->action = action;

	devres_add(dev, devres);
	return 0;
}
EXPORT_SYMBOL_GPL(devm_add_action);

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
 *               * Repeat on the address that fired VM_FAULT_RETRY
 *                               * without FAULT_FLAG_ALLOW_RETRY but with
 *                                               * FAULT_FLAG_TRIED.
 *                                                               */
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
 *               * We must let the caller know we temporarily dropped the lock
 *                               * and so the critical section protected by it was lost.
 *                                               */
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
