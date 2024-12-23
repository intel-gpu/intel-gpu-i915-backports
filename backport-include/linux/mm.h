/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __BACKPORT_MM_H
#define __BACKPORT_MM_H
#include_next <linux/mm.h>
#include <linux/page_ref.h>
#include <linux/sched.h>
#include <linux/overflow.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/pagevec.h>
#include <linux/kref.h>

#ifdef BPM_UNPIN_USER_PAGES_DIRTY_LOCK_NOT_PRESENT
void unpin_user_page_range_dirty_lock(struct page *page, unsigned long npages,
						bool make_dirty);
#endif

#ifdef BPM_PTE_OFFSET_MAP_NOT_PRESENT

#ifdef BPM_BAD_UNLOCK_PTE_OFFSET_MAP
#if defined(CONFIG_GUP_GET_PXX_LOW_HIGH) && \
        (defined(CONFIG_SMP) || defined(CONFIG_PREEMPT_RCU))

static unsigned long pmdp_get_lockless_start(void)
{
	unsigned long irqflags;

	local_irq_save(irqflags);
	return irqflags;
}
static void pmdp_get_lockless_end(unsigned long irqflags)
{
	local_irq_restore(irqflags);
}
#else
static unsigned long pmdp_get_lockless_start(void) { return 0; }
static void pmdp_get_lockless_end(unsigned long irqflags) { }
#endif
#endif

static inline void i915bkpt_pmd_clear_bad(pmd_t *pmd)
{
	pmd_ERROR(*pmd);
	pmd_clear(pmd);
}

#undef pte_offset_map
#define pte_offset_map LINUX_I915_BACKPORT(pte_offset_map)

static inline pte_t *pte_offset_map(pmd_t *pmd, unsigned long addr)
{
	pmd_t pmdval;
	pmd_t *pmdvalp = NULL;

#ifdef BPM_BAD_UNLOCK_PTE_OFFSET_MAP
	unsigned long irqflags;

	rcu_read_lock();
	irqflags = pmdp_get_lockless_start();
#endif
	pmdval = pmdp_get_lockless(pmd);
#ifdef BPM_BAD_UNLOCK_PTE_OFFSET_MAP
	pmdp_get_lockless_end(irqflags);
#endif

	if (pmdvalp)
		*pmdvalp = pmdval;
	if (unlikely(pmd_none(pmdval)))
		goto nomap;
	if (unlikely(pmd_trans_huge(pmdval) || pmd_devmap(pmdval)))
		goto nomap;
	if (unlikely(pmd_bad(pmdval))) {
		i915bkpt_pmd_clear_bad(pmd);
		goto nomap;
	}
	return __pte_map(&pmdval, addr);
nomap:
#ifdef BPM_BAD_UNLOCK_PTE_OFFSET_MAP
	rcu_read_unlock();
#endif
	return NULL;
}
#endif

#ifdef BPM_CANCEL_DIRTY_PAGE_NOT_PRESENT
#define cancel_dirty_page(X) folio_cancel_dirty(page_folio(X))
#endif

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
#endif /* < 4.12 */

#if LINUX_VERSION_IS_LESS(4,18,0)
#define kvcalloc LINUX_I915_BACKPORT(kvcalloc)
static inline void *kvcalloc(size_t n, size_t size, gfp_t flags)
{
	return kvmalloc_array(n, size, flags | __GFP_ZERO);
}
#endif /* < 4.18 */

#ifdef BPM_VMA_SET_FILE_NOT_PRESENT
#define vma_set_file LINUX_DMABUF_BACKPORT(vma_set_file)
void vma_set_file(struct vm_area_struct *vma, struct file *file);
#endif

#ifdef BPM_FOLIO_ADDRESS_PRESENT

#if defined(HASHED_PAGE_VIRTUAL)
void *page_address(const struct page *page);
void set_page_address(struct page *page, void *virtual);
void page_address_init(void);
#endif

#if !defined(HASHED_PAGE_VIRTUAL) && !defined(WANT_PAGE_VIRTUAL)
#define page_address(page) lowmem_page_address(page)
#define set_page_address(page, address)  do { } while(0)
#define page_address_init()  do { } while(0)
#endif

#define folio_address LINUX_I915_BACKPORT(folio_address)
static inline void *folio_address(const struct folio *folio)
{
	return page_address(&folio->page);
}

#ifdef BPM_REGISTER_SHRINKER_SECOND_ARG_NOT_PRESENT
int backport_register_shrinker(struct shrinker *shrinker);
#define register_shrinker backport_register_shrinker
#endif

#endif /* BPM_FOLIO_ADDRESS_PRESENT */

#ifdef BPM_PIN_USER_PAGES_FAST_NOT_PRESENT
#define pin_user_pages_fast get_user_pages_fast
#endif

#ifdef BPM_FOLL_FAST_ONLY_NOT_PRESENT
#define FOLL_FAST_ONLY 0x80000 /* gup_fast: prevent fall-back to slow gup */
#endif

#ifdef BPM_TOTALRAM_PAGES_FUNC_NOT_PRESENT
#define totalram_pages() totalram_pages
#endif

#ifdef BPM_IS_COW_MAPPING_NOT_PRESENT
static inline bool is_cow_mapping(vm_flags_t flags)
{
	return (flags & (VM_SHARED | VM_MAYWRITE)) == VM_MAYWRITE;
}
#endif

#ifdef BPM_WANT_INIT_ON_ALLOC_NOT_PRESENT

#ifdef CONFIG_INIT_ON_ALLOC_DEFAULT_ON
DECLARE_STATIC_KEY_TRUE(init_on_alloc);
#else
DECLARE_STATIC_KEY_FALSE(init_on_alloc);
#endif
static inline bool want_init_on_alloc(gfp_t flags)
{
       if (static_branch_unlikely(&init_on_alloc) &&
           !page_poisoning_enabled())
               return true;
       return flags & __GFP_ZERO;
}
#endif


#ifdef BPM_PIN_OR_UNPIN_USER_PAGE_NOT_PRESENT
#ifdef BPM_PUT_USER_PAGES_DIRTY_LOCK_ARG_NOT_PRESENT
#define unpin_user_pages_dirty_lock(X,Y,Z) put_user_pages_dirty_lock(X,Y)
#else
#define unpin_user_pages_dirty_lock(X,Y,Z) put_user_pages_dirty_lock(X,Y,Z)
#endif
#define unpin_user_page(X) put_user_page(X)
#define unpin_user_pages(X,Y) put_user_pages(X,Y)
#endif

#endif /* __BACKPORT_MM_H */
