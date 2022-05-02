#ifndef __BACKPORT_MM_H
#define __BACKPORT_MM_H
#include_next <linux/mm.h>
#include <linux/page_ref.h>
#include <linux/sched.h>
#include <linux/overflow.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>

#if LINUX_VERSION_IS_LESS(3,15,0)
#define kvfree LINUX_I915_BACKPORT(kvfree)
void kvfree(const void *addr);
#endif /* < 3.15 */

#if LINUX_VERSION_IS_LESS(4,12,0)
#define kvmalloc LINUX_I915_BACKPORT(kvmalloc)
static inline void *kvmalloc(size_t size, gfp_t flags)
{
	gfp_t kmalloc_flags = flags;
	void *ret;

	if ((flags & GFP_KERNEL) != GFP_KERNEL)
		return kmalloc(size, flags);

	if (size > PAGE_SIZE)
		kmalloc_flags |= __GFP_NOWARN | __GFP_NORETRY;

	ret = kmalloc(size, flags);
	if (ret || size < PAGE_SIZE)
		return ret;

	return vmalloc(size);
}

#define kvmalloc_array LINUX_I915_BACKPORT(kvmalloc_array)
static inline void *kvmalloc_array(size_t n, size_t size, gfp_t flags)
{
	size_t bytes;

	if (unlikely(check_mul_overflow(n, size, &bytes)))
		return NULL;

	return kvmalloc(bytes, flags);
}

#define kvzalloc LINUX_I915_BACKPORT(kvzalloc)
static inline void *kvzalloc(size_t size, gfp_t flags)
{
	return kvmalloc(size, flags | __GFP_ZERO);
}
#endif

#if LINUX_VERSION_IS_LESS(5,10,0)
#define vma_set_file LINUX_DMABUF_BACKPORT(vma_set_file)
void vma_set_file(struct vm_area_struct *vma, struct file *file);
#endif

#endif /* __BACKPORT_MM_H */
