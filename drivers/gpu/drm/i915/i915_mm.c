/*
 * Copyright © 2014 Intel Corporation
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

#include <linux/mm.h>
#include <linux/huge_mm.h>

#include <asm/pgalloc.h>

#include "i915_drv.h"
#include "i915_mm.h"

#define EXPECTED_FLAGS (VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP)

struct remap_pfn {
#if ALLOC_SPLIT_PTLOCKS || !IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_HUGEFAULT)
	struct mm_struct *mm;
#endif
	struct vm_area_struct *vma;
	unsigned long pfn;
	pgprot_t prot;

	struct sgt_iter sgt;
	resource_size_t iobase;
	bool write:1;
};

#define use_dma(io) ((io) != -1)

static inline unsigned long sgt_pfn(const struct remap_pfn *r)
{
	if (use_dma(r->iobase))
		return (r->sgt.dma + r->sgt.curr + r->iobase) >> PAGE_SHIFT;
	else
		return r->sgt.pfn + (r->sgt.curr >> PAGE_SHIFT);
}

#if ARCH_PAGE_TABLE_SYNC_MASK || ALLOC_SPLIT_PTLOCKS || !IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_HUGEFAULT)
static int remap_sg_pfn(pte_t *pte, unsigned long addr, void *data)
{
	struct remap_pfn *r = data;

	if (GEM_WARN_ON(!r->sgt.sgp))
		return -EINVAL;

	/* Special PTE are not associated with any struct page */
	set_pte_at(r->mm, addr, pte,
		   pte_mkspecial(pfn_pte(sgt_pfn(r), r->prot)));
	r->pfn++; /* track insertions in case we need to unwind later */

	r->sgt.curr += PAGE_SIZE;
	if (r->sgt.curr >= r->sgt.max)
		r->sgt = __sgt_iter(__sg_next(r->sgt.sgp), use_dma(r->iobase));

	return 0;
}

int remap_io_sg(struct vm_area_struct *vma,
		unsigned long addr, unsigned long size,
		struct scatterlist *sgl, unsigned long offset,
		resource_size_t iobase, bool write)
{
	struct remap_pfn r = {
		.mm = vma->vm_mm,
		.prot = vma->vm_page_prot,
		.sgt = __sgt_iter(sgl, use_dma(iobase)),
		.iobase = iobase,
		.write = write,
		.vma = vma,
	};
	int err;

	/* We rely on prevalidation of the io-mapping to skip track_pfn(). */
	GEM_BUG_ON((vma->vm_flags & EXPECTED_FLAGS) != EXPECTED_FLAGS);

	while (offset >= r.sgt.max >> PAGE_SHIFT) {
		offset -= r.sgt.max >> PAGE_SHIFT;
		r.sgt = __sgt_iter(__sg_next(r.sgt.sgp), use_dma(iobase));
		if (!r.sgt.sgp)
			return -EINVAL;
	}
	r.sgt.curr = offset << PAGE_SHIFT;

	if (!use_dma(iobase))
		flush_cache_range(vma, addr, size);

	err = apply_to_page_range(r.mm, addr, size, remap_sg_pfn, &r);
	if (unlikely(err)) {
		zap_vma_ptes(vma, addr, r.pfn << PAGE_SHIFT);
		return err;
	}

	return 0;
}

#else

typedef int (*__pmd_fn_t)(struct mm_struct *mm, pmd_t *pmd, unsigned long addr, void *data);
typedef int (*__pte_fn_t)(struct mm_struct *mm, pte_t *pmd, unsigned long addr, void *data);

static int remap_sg_pmd(struct mm_struct *mm, pmd_t *pmd, unsigned long addr, void *data)
{
	struct remap_pfn *r = data;
	unsigned long v;
	pmd_t entry;

	if (GEM_WARN_ON(!r->sgt.sgp))
		return -EINVAL;

	if (r->sgt.max - r->sgt.curr < SZ_2M)
		return -EINVAL;

	v = sgt_pfn(r);
	if (!IS_ALIGNED(v, SZ_2M >> PAGE_SHIFT))
		return -EINVAL;

	entry = pfn_pmd(v, r->prot);
	entry = pmd_mkhuge(entry);
	if (r->write)
#ifdef BPM_PMD_PTE_MKWRITE_VMA_ARG_NOT_PRESENT
		entry = pmd_mkwrite(pmd_mkyoung(pmd_mkdirty(entry)), r->vma);
#else
		entry = pmd_mkwrite(pmd_mkyoung(pmd_mkdirty(entry)));
#endif
	entry = pmd_mkdevmap(entry);

	set_pmd_at(mm, addr, pmd, entry);
	r->pfn += SZ_2M >> PAGE_SHIFT;/* track insertions in case we need to unwind later */

	r->sgt.curr += SZ_2M;
	if (r->sgt.curr >= r->sgt.max)
		r->sgt = __sgt_iter(__sg_next(r->sgt.sgp), use_dma(r->iobase));

	return 0;
}

static int remap_sg_pfn(struct mm_struct *mm, pte_t *pte, unsigned long addr, void *data)
{
	struct remap_pfn *r = data;
	pte_t entry;

	if (GEM_WARN_ON(!r->sgt.sgp))
		return -EINVAL;

	/* Special PTE are not associated with any struct page */
	entry = pfn_pte(sgt_pfn(r), r->prot);
	if (r->write)
#ifdef BPM_PMD_PTE_MKWRITE_VMA_ARG_NOT_PRESENT
		entry = pte_mkwrite(pte_mkyoung(pte_mkdirty(entry)), r->vma);
#else
		entry = pte_mkwrite(pte_mkyoung(pte_mkdirty(entry)));
#endif
	entry = pte_mkspecial(entry);
	set_pte_at(mm, addr, pte, entry);
	r->pfn++; /* track insertions in case we need to unwind later */

	r->sgt.curr += PAGE_SIZE;
	if (r->sgt.curr >= r->sgt.max)
		r->sgt = __sgt_iter(__sg_next(r->sgt.sgp), use_dma(r->iobase));

	return 0;
}

static int apply_to_pte_range(struct mm_struct *mm, pmd_t *pmd,
			      unsigned long addr, unsigned long end,
			      __pte_fn_t fn, void *data)
{
	pte_t *pte, *mapped_pte;
	spinlock_t *ptl;
	int err = 0;

	if (unlikely(pmd_none(*pmd))) {
		pgtable_t new;

		new = __pte_alloc_one(mm, GFP_PGTABLE_USER);
		if (!new)
			return -ENOMEM;

		smp_wmb(); /* Could be smp_wmb__xxx(before|after)_spin_lock */

		ptl = pmd_lock(mm, pmd);
		if (likely(pmd_none(*pmd))) {	/* Has another populated it ? */
			mm_inc_nr_ptes(mm);
			pmd_populate(mm, pmd, new);
			new = NULL;
		}
		spin_unlock(ptl);
		if (new)
			pte_free(mm, new);
	}
	mapped_pte = pte = pte_offset_map_lock(mm, pmd, addr, &ptl);

	arch_enter_lazy_mmu_mode();

	do {
		err = fn(mm, pte, addr, data);
		if (err)
			break;
	} while (pte++, addr += PAGE_SIZE, addr != end);

	arch_leave_lazy_mmu_mode();

	pte_unmap_unlock(mapped_pte, ptl);
	return err;
}

static int apply_to_pmd_range(struct mm_struct *mm, pud_t *pud,
				     unsigned long addr,
				     unsigned long end,
				     __pmd_fn_t pmd_fn,
				     __pte_fn_t pte_fn,
				     void *data)
{
	pmd_t *pmd;
	unsigned long next;
	int err = 0;

	if (unlikely(pud_none(*pud))) {
		struct page *page;
		spinlock_t *ptl;

		page = alloc_pages(GFP_PGTABLE_USER, 0);
		if (!page)
			return -ENOMEM;

		if (!pgtable_pmd_page_ctor(page)) {
			__free_pages(page, 0);
			return -ENOMEM;
		}
		pmd = (pmd_t *)page_address(page);
		smp_wmb(); /* See comment in __pte_alloc */

		ptl = pud_lock(mm, pud);
		if (!pud_present(*pud)) {
			mm_inc_nr_pmds(mm);
			pud_populate(mm, pud, pmd);
		} else  /* Another has populated it */
			pmd_free(mm, pmd);
		spin_unlock(ptl);
	}
        pmd = pmd_offset(pud, addr);

	do {
		next = pmd_addr_end(addr, end);

		if (!pmd_none(*pmd) && pmd_bad(*pmd))
			WRITE_ONCE(*pmd, __pmd(0));

		if (IS_ALIGNED(addr | next, SZ_2M) && !pmd_fn(mm, pmd, addr, data))
			continue;

		if (GEM_WARN_ON(pmd_leaf(*pmd)))
			return -EINVAL;

		err = apply_to_pte_range(mm, pmd, addr, next, pte_fn, data);
		if (err)
			break;
	} while (pmd++, addr = next, addr != end);

	return err;
}

static int apply_to_pud_range(struct mm_struct *mm, p4d_t *p4d,
				     unsigned long addr, unsigned long end,
				     __pmd_fn_t pmd_fn,
				     __pte_fn_t pte_fn,
				     void *data)
{
	unsigned long next;
	pud_t *pud;
	int err = 0;

	if (unlikely(p4d_none(*p4d))) {
		pud = (pud_t *)get_zeroed_page(GFP_PGTABLE_USER);
		if (!pud)
			return -ENOMEM;

		smp_wmb(); /* See comment in __pte_alloc */

		spin_lock(&mm->page_table_lock);
		if (!p4d_present(*p4d)) {
			mm_inc_nr_puds(mm);
			paravirt_alloc_pud(mm, __pa(pud) >> PAGE_SHIFT);
			WRITE_ONCE(*p4d, __p4d(_PAGE_TABLE | __pa(pud)));
		} else  /* Another has populated it */
			pud_free(mm, pud);
		spin_unlock(&mm->page_table_lock);
	}
        pud = pud_offset(p4d, addr);

	do {
		next = pud_addr_end(addr, end);
		if (GEM_WARN_ON(pud_leaf(*pud)))
			return -EINVAL;
		if (!pud_none(*pud) && pud_bad(*pud))
			WRITE_ONCE(*pud, __pud(0));

		err = apply_to_pmd_range(mm, pud, addr, next, pmd_fn, pte_fn, data);
		if (err)
			break;
	} while (pud++, addr = next, addr != end);

	return err;
}

static int apply_to_p4d_range(struct mm_struct *mm, pgd_t *pgd,
				     unsigned long addr, unsigned long end,
				     __pmd_fn_t pmd_fn,
				     __pte_fn_t pte_fn,
				     void *data)
{
	unsigned long next;
	p4d_t *p4d;
	int err = 0;

	if (unlikely(pgd_none(*pgd))) {
		p4d =  (p4d_t *)get_zeroed_page(GFP_PGTABLE_USER);
		if (!p4d)
			return -ENOMEM;

		smp_wmb(); /* See comment in __pte_alloc */

		spin_lock(&mm->page_table_lock);
		if (!pgd_present(*pgd)) { /* Another has populated it */
			paravirt_alloc_p4d(mm, __pa(p4d) >> PAGE_SHIFT);
			WRITE_ONCE(*pgd, __pgd(_PAGE_TABLE | __pa(p4d)));
		} else
			p4d_free(mm, p4d);
		spin_unlock(&mm->page_table_lock);
	}
        p4d = p4d_offset(pgd, addr);

	do {
		next = p4d_addr_end(addr, end);
		if (GEM_WARN_ON(p4d_leaf(*p4d)))
			return -EINVAL;
		if (!p4d_none(*p4d) && p4d_bad(*p4d))
			WRITE_ONCE(*p4d, __p4d(0));

		err = apply_to_pud_range(mm, p4d, addr, next, pmd_fn, pte_fn, data);
		if (err)
			break;
	} while (p4d++, addr = next, addr != end);

	return err;
}

static int __apply_to_page_range(struct mm_struct *mm,
				 unsigned long addr,
				 unsigned long size,
				 __pmd_fn_t pmd_fn,
				 __pte_fn_t pte_fn,
				 void *data)
{
	unsigned long end = addr + size, next;
	pgd_t *pgd;
	int err;

	if (GEM_WARN_ON(addr >= end))
		return -EINVAL;

	pgd = pgd_offset(mm, addr);
	do {
		next = pgd_addr_end(addr, end);
		if (GEM_WARN_ON(pgd_leaf(*pgd)))
			return -EINVAL;
		if (!pgd_none(*pgd) && pgd_bad(*pgd))
			WRITE_ONCE(*pgd, __pgd(0));

		err = apply_to_p4d_range(mm, pgd, addr, next, pmd_fn, pte_fn, data);
		if (err)
			break;
	} while (pgd++, addr = next, addr != end);

	return err;
}

/**
 * remap_io_sg - remap an IO mapping to userspace
 * @vma: user vma to map to
 * @addr: target user address to start at
 * @size: size of map area
 * @sgl: Start sg entry
 * @offset: offset into scatterlist
 * @iobase: Use stored dma address offset by this address or pfn if -1
 * @write: mark the PTE for writing
 *
 *  Note: this is only safe if the mm semaphore is held when called.
 */
int remap_io_sg(struct vm_area_struct *vma,
		unsigned long addr, unsigned long size,
		struct scatterlist *sgl, unsigned long offset,
		resource_size_t iobase, bool write)
{
	struct remap_pfn r = {
		.prot = vma->vm_page_prot,
		.sgt = __sgt_iter(sgl, use_dma(iobase)),
		.iobase = iobase,
		.write = write,
		.vma = vma,
	};
	int err;

	/* We rely on prevalidation of the io-mapping to skip track_pfn(). */
	GEM_BUG_ON((vma->vm_flags & EXPECTED_FLAGS) != EXPECTED_FLAGS);

	while (offset >= r.sgt.max >> PAGE_SHIFT) {
		offset -= r.sgt.max >> PAGE_SHIFT;
		r.sgt = __sgt_iter(__sg_next(r.sgt.sgp), use_dma(iobase));
		if (!r.sgt.sgp)
			return -EINVAL;
	}
	r.sgt.curr = offset << PAGE_SHIFT;

	if (!use_dma(iobase))
		flush_cache_range(vma, addr, size);

	err = __apply_to_page_range(vma->vm_mm, addr, size, remap_sg_pmd, remap_sg_pfn, &r);
	if (unlikely(err)) {
		zap_vma_ptes(vma, addr, r.pfn << PAGE_SHIFT);
		return err;
	}

	return 0;
}
#endif
