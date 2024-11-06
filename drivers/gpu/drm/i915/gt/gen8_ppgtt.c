// SPDX-License-Identifier: MIT
/*
 * Copyright © 2020 Intel Corporation
 */

#include <linux/log2.h>

#include "gem/i915_gem_lmem.h"

#include "gen8_engine_cs.h"
#include "gen8_ppgtt.h"
#include "i915_scatterlist.h"
#include "i915_trace.h"
#include "intel_engine_pm.h"
#include "intel_gpu_commands.h"
#include "intel_gt.h"
#include "intel_gtt.h"
#include "intel_gt_pm.h"
#include "intel_lrc.h"
#include "i915_drv.h"

inline u64 gen8_pde_encode(const dma_addr_t addr, const enum i915_cache_level level)
{
	u64 pde = addr | GEN8_PAGE_PRESENT | GEN8_PAGE_RW;

	if (level != I915_CACHE_NONE)
		pde |= PPAT_CACHED_PDE;
	else
		pde |= PPAT_UNCACHED;

	return pde;
}

static u64 pde_encode(const struct i915_page_table *pt)
{
	u64 encode;

	encode = gen8_pde_encode(px_dma(pt), I915_CACHE_LLC);
	if (pt->is_compact)
		encode |= GEN12_PDE_64K;

	return encode;
}

static u64 gen12_pte_encode(dma_addr_t addr, unsigned int pat_index, u32 flags)
{
	gen8_pte_t pte = addr | GEN8_PAGE_PRESENT | GEN8_PAGE_RW;

	if (unlikely(flags & PTE_READ_ONLY))
		pte &= ~GEN8_PAGE_RW;

	if (flags & PTE_LM)
		pte |= GEN12_PPGTT_PTE_LM | GEN12_PPGTT_PTE_NC;
	if (flags & PTE_AE)
		pte |= GEN12_USM_PPGTT_PTE_AE;

	pte |= (pat_index & (BIT(0) | BIT(1))) << (3 - 0);
	pte |= (pat_index & BIT_ULL(2)) << (7 - 2);
	pte |= (pat_index & BIT_ULL(3)) << (62 - 3);

	return pte;
}

static inline void
write_pte(struct i915_page_table *pt, int idx, gen8_pte_t pte)
{
	u64 * const vaddr = px_vaddr(pt);

	WRITE_ONCE(vaddr[idx], pte);
}

/* Index shifts into the pagetable are offset by GEN8_PTE_SHIFT [12] */
#define GEN8_PAGE_SIZE (SZ_4K) /* page and page-directory sizes are the same */
#define GEN8_PTE_SHIFT (ilog2(GEN8_PAGE_SIZE))
#define GEN8_PDES (GEN8_PAGE_SIZE / sizeof(u64))

static inline u32 gen8_pde_index(u64 addr, u32 shift)
{
	return (addr >> shift) & (GEN8_PDES - 1);
}

#define gen8_pd_shift(lvl) ((lvl) * ilog2(GEN8_PDES))
#define gen8_pd_index(i, lvl) gen8_pde_index((i), gen8_pd_shift(lvl))
#define __gen8_pte_shift(lvl) (GEN8_PTE_SHIFT + gen8_pd_shift(lvl))
#define __gen8_pte_index(a, lvl) gen8_pde_index((a), __gen8_pte_shift(lvl))

#define as_pd(x) container_of((x), typeof(struct i915_page_directory), pt)

static unsigned int
gen8_pd_range(u64 start, u64 end, int lvl, unsigned int *idx)
{
	const int shift = gen8_pd_shift(lvl);
	const u64 mask = ~0ull << gen8_pd_shift(lvl + 1);

	GEM_BUG_ON(start >= end);
	end += ~mask >> gen8_pd_shift(1);

	*idx = gen8_pde_index(start, shift);
	if ((start ^ end) & mask)
		return GEN8_PDES - *idx;
	else
		return gen8_pde_index(end, shift) - *idx;
}

static bool gen8_pd_contains(u64 start, u64 end, int lvl)
{
	const u64 mask = ~0ull << gen8_pd_shift(lvl + 1);

	GEM_BUG_ON(start >= end);
	return (start ^ end) & mask && (start & ~mask) == 0;
}

static unsigned int gen8_pd_count(u64 start, u64 end)
{
	if ((start ^ end) >> gen8_pd_shift(1))
		return GEN8_PDES - (start & (GEN8_PDES - 1));
	else
		return end - start + 1;
}

static unsigned int gen8_pd_top_count(const struct i915_address_space *vm)
{
	unsigned int shift = __gen8_pte_shift(vm->top);

	return (vm->total + (1ull << shift) - 1) >> shift;
}

struct freelist {
	struct llist_head head;
	struct llist_node *tail;
};

static inline void
free_px_f(struct i915_address_space *vm, struct i915_page_table *pt, int lvl, struct freelist *f)
{
	if (__llist_add(&pt->base->freed, &f->head))
		f->tail = &pt->base->freed;

	pt->base = NULL;
	free_px(vm, pt, lvl);
}

static inline void free_px_ll(struct i915_address_space *vm, struct freelist *f)
{
	if (llist_empty(&f->head))
		return;

	GEM_BUG_ON(!f->tail);

	if (likely(vm->gt->px_cache)) {
		preempt_disable();
		__llist_add_batch(f->head.first, f->tail, this_cpu_ptr(vm->gt->px_cache));
		preempt_enable();
	} else {
		struct drm_i915_gem_object *pt, *pn;

		llist_for_each_entry_safe(pt, pn, __llist_del_all(&f->head), freed)
			i915_gem_object_put(pt);
	}
}

static inline void init_px_ll(struct freelist *f)
{
	init_llist_head(&f->head);
}

static void __gen8_ppgtt_cleanup(struct i915_address_space *vm,
				 struct i915_page_directory *pd,
				 int count, int lvl,
				 struct freelist *f)
{
	if (lvl) {
		void **pde = pd->entry;

		do {
			void *pt = *pde++;

			if (!pt)
				continue;

			__gen8_ppgtt_cleanup(vm, pt, GEN8_PDES, lvl - 1, f);
		} while (--count);
	}

	free_px_f(vm, &pd->pt, lvl, f);
}

static void gen8_ppgtt_cleanup(struct i915_address_space *vm)
{
	struct i915_ppgtt *ppgtt = i915_vm_to_ppgtt(vm);

	if (ppgtt->pd) {
		struct freelist f;

		init_px_ll(&f);
		__gen8_ppgtt_cleanup(vm, ppgtt->pd,
				     gen8_pd_top_count(vm), vm->top, &f);
		free_px_ll(vm, &f);
	}

	i915_vm_free_scratch(vm);
}

static u64 __ppgtt_clear(struct i915_address_space * const vm,
			 struct i915_page_directory * const pd,
			 u64 start, const u64 end, const u64 fail,
			 int lvl, struct freelist *f)
{
	u64 scratch_encode = i915_vm_scratch_encode(vm, lvl);
	unsigned int idx, len;

	GEM_BUG_ON(end > vm->total >> GEN8_PTE_SHIFT);
	GEM_BUG_ON(start > end);

	len = gen8_pd_range(start, fail ?: end, lvl--, &idx);
	DBG("%s(%p):{ lvl:%d, start:%llx, last:%llx, fail:%llx, idx:%d, len:%d, used:%d }\n",
	    __func__, vm, lvl + 1,
	    start >> gen8_pd_shift(lvl + 1),
	    (end - 1) >> gen8_pd_shift(lvl + 1),
	    fail >> gen8_pd_shift(lvl + 1),
	    idx, len, atomic_read(px_used(pd)));
	GEM_BUG_ON(!len);

	do {
		struct i915_page_table *pt = pd->entry[idx];
		int used;

		GEM_BUG_ON(start > end);

		if (!pt) { /* restore huge pages, which leave the entry blank */
			DBG("%s(%p):{ lvl:%d, start:%llx, last:%llx, idx:%d } empty pd (huge page)\n",
			    __func__, vm, lvl + 1,
			    start >> gen8_pd_shift(lvl + 1),
			    (end - 1) >> gen8_pd_shift(lvl + 1),
			    idx);
skip:			write_pte(&pd->pt, idx, scratch_encode);
			start += (u64)GEN8_PDES << gen8_pd_shift(lvl);
			continue;
		}

		if (gen8_pd_contains(start, end, lvl)) {
			DBG("%s(%p):{ lvl:%d, start:%llx, last:%llx, idx:%d } removing pd\n",
			    __func__, vm, lvl + 1,
			    start >> gen8_pd_shift(lvl + 1),
			    (end - 1) >> gen8_pd_shift(lvl + 1),
			    idx);
			WRITE_ONCE(pd->entry[idx], NULL);
			__gen8_ppgtt_cleanup(vm, as_pd(pt), GEN8_PDES, lvl, f);
			goto skip;
		}

		used = gen8_pd_count(start >> gen8_pd_shift(lvl),
				     (end - 1) >> gen8_pd_shift(lvl));
		DBG("%s(%p):{ lvl:%d, start:%llx, last:%llx } used %d of %d%s\n",
		    __func__, vm, lvl,
		    start >> gen8_pd_shift(lvl),
		    (end - 1) >> gen8_pd_shift(lvl),
		    used, atomic_read(px_used(pt)),
		    atomic_read(px_used(pt)) < used ? "!***" :"");
		GEM_BUG_ON(atomic_read(px_used(pt)) < used);
		if (lvl) {
			start = __ppgtt_clear(vm, as_pd(pt), start, end, fail, lvl, f);
		} else {
			unsigned int pte = gen8_pd_index(start, 0);
			unsigned int count = used;
			u64 *vaddr;

			DBG("%s(%p):{ lvl:%d, start:%llx, last:%llx, idx:%d, len:%d, used:%d, compact?:%d } removing pte\n",
			    __func__, vm, lvl,
			    start, end - 1,
			    pte, count,
			    atomic_read(px_used(pt)),
			    pt->is_compact);

			start += count;
			if (pt->is_compact) {
				GEM_BUG_ON(!IS_ALIGNED(count | pte, 16));
				count /= 16;
				pte /= 16;
			}

			vaddr = px_vaddr(pt);
			memset64(vaddr + pte,
				 i915_vm_scratch0_encode(vm),
				 count);
		}

		if (atomic_sub_and_test(used, px_used(pt))) {
			DBG("%s(%p):{ lvl:%d, idx:%d } freeing pd:%p\n",
			    __func__, vm, lvl + 1, idx, pt);
			write_pte(&pd->pt, idx, scratch_encode);
			smp_wmb(); /* order the PTE update with pt_alloc() */
			WRITE_ONCE(pd->entry[idx], NULL);
			free_px_f(vm, pt, lvl, f);
		}
	} while (idx++, --len);

	return start;
}

static void ppgtt_clear(struct i915_address_space *vm,
			     u64 start, u64 end, u64 fail)
{
	struct freelist f;

	DBG("%s(%p):{ start:%llx, end:%llx, fail:%llx }\n",
	    __func__, vm, start, end, fail);

	GEM_BUG_ON(!IS_ALIGNED(start | end, BIT_ULL(GEN8_PTE_SHIFT)));

	start >>= GEN8_PTE_SHIFT;
	end >>= GEN8_PTE_SHIFT;
	fail >>= GEN8_PTE_SHIFT;

	init_px_ll(&f);
	__ppgtt_clear(vm, i915_vm_to_ppgtt(vm)->pd, start, end, fail, vm->top, &f);
	free_px_ll(vm, &f);
}

static void gen8_ppgtt_clear(struct i915_address_space *vm, u64 start, u64 length)
{
	intel_wakeref_t wf;

	DBG("%s(%p):{ start:%llx, length:%llx }\n",
	    __func__, vm, start, length);

	GEM_BUG_ON(!IS_ALIGNED(start, BIT_ULL(GEN8_PTE_SHIFT)));
	GEM_BUG_ON(!IS_ALIGNED(length, BIT_ULL(GEN8_PTE_SHIFT)));
	GEM_BUG_ON(range_overflows(start, length, vm->total));
	GEM_BUG_ON(length == 0);

	with_intel_gt_pm_delay(vm->gt, wf, 2)
		ppgtt_clear(vm, start, start + length, 0);
}

struct pt_insert {
	struct i915_address_space *vm;
	struct i915_gem_ww_ctx *ww;
	gen8_pte_t pte_encode;
	u64 addr, end, fail;
	struct sgt_dma it;
	int error;
};

static gen8_pte_t pt_advance(struct pt_insert *arg, int sz, int count)
{
	gen8_pte_t pte = arg->it.dma | arg->pte_encode;

	GEM_BUG_ON(!IS_ALIGNED(arg->it.dma, sz));
	GEM_BUG_ON(!IS_ALIGNED(arg->addr, sz));
	GEM_BUG_ON(!count);

	sz *= count;

	arg->addr += sz;
	if (unlikely(arg->addr >= arg->end)) {
		arg->it.sg = NULL;
		return pte;
	}

	arg->it.dma += sz;
	if (arg->it.dma >= arg->it.max) {
		arg->it.sg  = __sg_next(arg->it.sg);
		arg->it.dma = sg_dma_address(arg->it.sg);
		arg->it.max = arg->it.dma +
			min_t(u64, sg_dma_len(arg->it.sg), arg->end - arg->addr);
		if (unlikely(arg->it.dma >= arg->it.max))
			arg->it.sg = NULL;
	}

	return pte;
}

static struct i915_page_table *
pt_alloc(struct pt_insert *arg, int lvl,
	 struct i915_page_directory *pd, void **pde,
	 gen8_pte_t *encode)
{
	struct i915_page_table *pt;
	int used;

	used = gen8_pd_count(arg->addr >> __gen8_pte_shift(lvl),
			     (arg->end - 1) >> __gen8_pte_shift(lvl));
	GEM_BUG_ON(!used);

	DBG("%s(%p):{ lvl:%d, addr:%llx, start:%llx, last:%llx } adding used:%d\n",
	    __func__, arg->vm, lvl, arg->addr,
	    arg->addr >> __gen8_pte_shift(lvl),
	    (arg->end - 1) >> __gen8_pte_shift(lvl),
	    used);

	rcu_read_lock();
	GEM_BUG_ON(!atomic_read(px_used(pd))); /* Must be pinned! */
	pt = READ_ONCE(*pde);
	if (!pt) {
replace:	rcu_read_unlock();

		pt = lvl ? &alloc_pd(arg->vm)->pt : alloc_pt(arg->vm, SZ_4K);
		if (IS_ERR(pt)) {
			arg->error = PTR_ERR(pt);
			goto err;
		}

		arg->error = map_pt_dma(arg->vm, arg->ww, pt->base);
		if (arg->error) {
			free_px(arg->vm, pt, lvl);
			goto err;
		}

		atomic_set(px_used(pt), used);
		pt->is_compact = !lvl && arg->pte_encode & arg->vm->pt_compact;
		pt->is_64k = true;

		if (used < GEN8_PDES) {
			struct i915_page_table *old;

			fill_px(pt, i915_vm_scratch_encode(arg->vm, lvl));

			rcu_read_lock();
			old = cmpxchg(pde, NULL, pt);
			if (unlikely(old)) {
				do {
					if (atomic_add_unless(px_used(old), used, 0)) {
						free_px(arg->vm, pt, lvl);
						pt = old;
						break;
					}
					DBG("%s(%p):{ lvl:%d, addr:%llx, idx:%d } waiting for freed pde:%p\n",
					    __func__, arg->vm, lvl, arg->addr, __gen8_pte_index(arg->addr, lvl), old);
					while (READ_ONCE(*pde) == old)
						cpu_relax();
				} while ((old = cmpxchg(pde, NULL, pt)));
			}
			if (!old) {
				DBG("%s(%p):{ lvl:%d, addr:%llx, idx:%d, used:%d } inserting pde:%p\n",
				    __func__, arg->vm, lvl, arg->addr, __gen8_pte_index(arg->addr, lvl), used, pt);
			}
			rcu_read_unlock();
		} else {
			DBG("%s(%p):{ lvl:%d, addr:%llx, idx:%d, used:%d } inserting pde:%p (whole)\n",
			    __func__, arg->vm, lvl, arg->addr, __gen8_pte_index(arg->addr, lvl), used, pt);
			*pde = pt;
		}
	} else {
		if (!lvl) {
			bool is_compact = arg->pte_encode & arg->vm->pt_compact;

			/* Wait for the prior owner to remove conflicting PD. */
			if (unlikely(is_compact != pt->is_compact)) {
				while (READ_ONCE(*pde) == pt && is_compact != pt->is_compact) {
					cpu_relax();
					barrier();
				}

				pt = READ_ONCE(*pde);
				if (!pt)
					goto replace;
			}
		}

		if (!atomic_add_unless(px_used(pt), used, 0))
			goto replace;

		rcu_read_unlock();
	}

	*encode = pde_encode(pt);
	return pt;

err:
	arg->fail = arg->addr;
	arg->it.sg = NULL;
	return NULL;
}

static inline u64 pt_len(const struct pt_insert *arg)
{
	return arg->it.max - arg->it.dma;
}

static inline bool pt_aligned(const struct pt_insert *arg, unsigned int sz)
{
	return IS_ALIGNED(arg->it.dma | arg->addr, sz) && pt_len(arg) >= sz;
}

static gen8_pte_t
pt_insert(struct pt_insert *arg, int lvl,
	  struct i915_page_directory *pd, void **pde)
{
	struct i915_page_table *pt;
	gen8_pte_t pte;

	DBG("%s(%p):{ lvl:%d, start:%llx, last:%llx }\n",
	    __func__, arg->vm, lvl,
	    arg->addr >> __gen8_pte_shift(lvl),
	    (arg->end - 1) >> __gen8_pte_shift(lvl));

	pt = pt_alloc(arg, lvl, pd, pde, &pte);
	if (!pt)
		return 0;

	return arg->vm->pt_insert(arg, pt) | pte;
}

static gen8_pte_t
pd_insert(struct pt_insert *arg, int lvl,
	  struct i915_page_directory *pd, void **pde)
{
	struct i915_page_directory *pt;
	unsigned int idx;
	gen8_pte_t ret;

	DBG("%s(%p):{ lvl:%d, addr:%llx, start:%llx, last:%llx }\n",
	    __func__, arg->vm, lvl, arg->addr,
	    arg->addr >> __gen8_pte_shift(lvl),
	    (arg->end - 1) >> __gen8_pte_shift(lvl));

	pt = as_pd(pt_alloc(arg, lvl, pd, pde, &ret));
	if (!pt)
		return 0;

	idx = __gen8_pte_index(arg->addr, lvl--);
	do {
		gen8_pte_t pte;

		if (lvl == 1 && pt_aligned(arg, SZ_1G)) {
			DBG("%s(%p):{ lvl:%d, addr:%llx, start:%llx, last:%llx, idx:%d } 1G PTE\n",
			    __func__, arg->vm, lvl + 1, arg->addr,
			    arg->addr >> __gen8_pte_shift(lvl + 1),
			    (arg->end - 1) >> __gen8_pte_shift(lvl + 1),
			    idx);
			pte = pt_advance(arg, SZ_1G, 1) | GEN8_PDPE_PS_1G;
		} else if (lvl == 0 && pt_aligned(arg, SZ_2M)) {
			DBG("%s(%p):{ lvl:%d, addr:%llx, start:%llx, last:%llx, idx:%d } 2M PTE\n",
			    __func__, arg->vm, lvl + 1, arg->addr,
			    arg->addr >> __gen8_pte_shift(lvl + 1),
			    (arg->end - 1) >> __gen8_pte_shift(lvl + 1),
			    idx);
			pte = pt_advance(arg, SZ_2M, 1) | GEN8_PDE_PS_2M;
		} else {
			DBG("%s(%p):{ lvl:%d, addr:%llx, start:%llx, last:%llx, idx:%d } leaf\n",
			    __func__, arg->vm, lvl + 1, arg->addr,
			    arg->addr >> __gen8_pte_shift(lvl + 1),
			    (arg->end - 1) >> __gen8_pte_shift(lvl + 1),
			    idx);
			pte = (lvl ? pd_insert : pt_insert)(arg, lvl, pt, &pt->entry[idx]);
		}
		if (unlikely(pte)) {
			DBG("%s(%p):{ lvl:%d, idx:%d } PDE update: %llx\n",
			    __func__, arg->vm, lvl + 1,
			    idx, pte);
			write_pte(&pt->pt, idx, pte);
		}
	} while (++idx < GEN8_PDES && arg->it.sg);

	wmb();
	return ret;
}

static void __ppgtt_insert(struct pt_insert *arg)
{
	struct i915_page_directory *pd = i915_vm_to_ppgtt(arg->vm)->pd;
	const int top = arg->vm->top;
	int idx = __gen8_pte_index(arg->addr, top);
	u64 start = arg->addr;

	do {
		gen8_pte_t pte;

		pte = pd_insert(arg, top - 1, pd, &pd->entry[idx]);
		if (unlikely(pte))
			write_pte(&pd->pt, idx, pte);
	} while (idx++, unlikely(arg->it.sg));

	if (unlikely(arg->error && arg->fail > start))
		ppgtt_clear(arg->vm, start, arg->end, arg->fail);
}

static int ppgtt_insert(struct i915_address_space *vm,
			struct i915_vma *vma,
			struct i915_gem_ww_ctx *ww,
			unsigned int pat_index,
			u32 flags)
{
	struct pt_insert arg = {
		.vm = vm,
		.it = sgt_dma(vma),
		.pte_encode = gen12_pte_encode(0, pat_index, flags),
		.addr = i915_vma_offset(vma),
		.end = i915_vma_offset(vma) + min(i915_vma_size(vma), vma->size),
		.ww = ww,
	};
	intel_wakeref_t wf;

	DBG("%s(%p):{ start:%llx, end:%llx }\n",
	    __func__, vm, arg.addr, arg.end);

	with_intel_gt_pm_delay(vm->gt, wf, 2)
		__ppgtt_insert(&arg);

	return arg.error;
}

static gen8_pte_t
gen8_pt_insert(struct pt_insert *arg, struct i915_page_table *pt)
{
	u64 * const vaddr = px_vaddr(pt);
	int idx = __gen8_pte_index(arg->addr, 0);

	do {
		gen8_pte_t pte;
		int len;

		len = min_t(u64, GEN8_PDES - idx, pt_len(arg) >> 12);
		if (pt->is_64k && !pt_aligned(arg, SZ_64K))
			pt->is_64k = false;

		GEM_BUG_ON(!len);
		pte = pt_advance(arg, SZ_4K, len);
		do
			vaddr[idx++] = pte;
		while (pte += SZ_4K, --len);
	} while (idx < GEN8_PDES && arg->it.sg);

	return pt->is_64k ? GEN8_PDE_IPS_64K : 0;
}

static void
dg2_ppgtt_color_adjust(const struct drm_mm_node *node,
		       unsigned long color,
		       u64 *start, u64 *end)
{
	if (i915_node_color_differs(node, color))
		*start = round_up(*start, SZ_2M);

	node = list_next_entry(node, node_list);
	if (i915_node_color_differs(node, color))
		*end = round_down(*end, SZ_2M);
}

static gen8_pte_t
ps64_pt_insert(struct pt_insert *arg, struct i915_page_table *pt)
{
	u64 *vaddr = px_vaddr(pt);

	vaddr += __gen8_pte_index(arg->addr, 0);
	do {
		int len = min_t(u64, GEN8_PDES - offset_in_page(vaddr) / sizeof(*vaddr), pt_len(arg) >> 12);
		gen8_pte_t pte;

		GEM_BUG_ON(!len);
		if (pt_aligned(arg, SZ_64K)) {
			int count = len / 16;

			DBG("%s(%p):{ lvl:%d, start:%llx, last:%llx, len:%d, used:%d } 64K PTE x %d, dma:%llx, max:%llx\n",
			    __func__, arg->vm, 0,
			    arg->addr >> __gen8_pte_shift(0),
			    (arg->end - 1) >> __gen8_pte_shift(0),
			    len, atomic_read(px_used(pt)), count,
			    arg->it.dma | arg->pte_encode, arg->it.max);

			pte = pt_advance(arg, SZ_64K, count) | GEN12_PTE_PS64;
			len -= 16 * count;
			do {
				*vaddr++ = pte +  0 * SZ_4K;
				*vaddr++ = pte +  1 * SZ_4K;
				*vaddr++ = pte +  2 * SZ_4K;
				*vaddr++ = pte +  3 * SZ_4K;
				*vaddr++ = pte +  4 * SZ_4K;
				*vaddr++ = pte +  5 * SZ_4K;
				*vaddr++ = pte +  6 * SZ_4K;
				*vaddr++ = pte +  7 * SZ_4K;
				*vaddr++ = pte +  8 * SZ_4K;
				*vaddr++ = pte +  9 * SZ_4K;
				*vaddr++ = pte + 10 * SZ_4K;
				*vaddr++ = pte + 11 * SZ_4K;
				*vaddr++ = pte + 12 * SZ_4K;
				*vaddr++ = pte + 13 * SZ_4K;
				*vaddr++ = pte + 14 * SZ_4K;
				*vaddr++ = pte + 15 * SZ_4K;
			} while (pte += SZ_64K, --count);
		}
		if (len) {
			DBG("%s(%p):{ lvl:%d, start:%llx, last:%llx, len:%d, used:%d } 4K PTE x %d, dma:%llx, max:%llx\n",
			    __func__, arg->vm, 0,
			    arg->addr >> __gen8_pte_shift(0),
			    (arg->end - 1) >> __gen8_pte_shift(0),
			    len, atomic_read(px_used(pt)), len,
			    arg->it.dma | arg->pte_encode, arg->it.max);

			pte = pt_advance(arg, SZ_4K, len);
			do
				*vaddr++ = pte;
			while (pte += SZ_4K, --len);
		}
	} while (offset_in_page(vaddr) && arg->it.sg);

	return 0;
}

static gen8_pte_t
dg2_pt_insert(struct pt_insert *arg, struct i915_page_table *pt)
{
	u64 * const vaddr = px_vaddr(pt);
	int idx;

	if (!pt->is_compact)
		return ps64_pt_insert(arg, pt);

	GEM_BUG_ON(!(arg->pte_encode & GEN12_PPGTT_PTE_LM));
	idx = __gen8_pte_index(arg->addr, 0) / 16;
	do {
		int len = min_t(u64, GEN8_PDES / 16 - idx, pt_len(arg) >> 16);
		gen8_pte_t pte;

		DBG("%s(%p):{ lvl:%d, start:%llx, last:%llx, idx:%d, len:%d, used:%d } 64K PTE, dma:%llx, max:%llx\n",
		    __func__, arg->vm, 0,
		    arg->addr >> __gen8_pte_shift(0),
		    (arg->end - 1) >> __gen8_pte_shift(0),
		    16 * idx, 16 * len, atomic_read(px_used(pt)),
		    arg->it.dma | arg->pte_encode, arg->it.max);

		GEM_BUG_ON(!len);
		pte = pt_advance(arg, SZ_64K, len);
		do
			vaddr[idx++] = pte;
		while (pte += SZ_64K, --len);
	} while (idx < GEN8_PDES / 16 && arg->it.sg);

	return 0;
}

int pvc_ppgtt_fault(struct i915_address_space *vm,
		    u64 start, u64 length, bool valid)
{
	struct pt_insert arg = {
		.vm = vm,
		.it = { .max = length, .sg = ERR_PTR(-1) },
		.pte_encode = PTE_NULL_PAGE | valid,
		.addr = start,
		.end = start + length,
	};

	DBG("%s(%p):{ start:%llx, end:%llx }\n",
	    __func__, vm, arg.addr, arg.end);

	__ppgtt_insert(&arg);

	return arg.error;
}

static inline bool can_share_scratch(struct i915_address_space const *vm)
{
	struct i915_address_space *src = vm->gt->vm;

	/*
	 * Reuse scratch page for all vm
	 *
	 * The writes are dropped because the page is either read-only or null page.
	 * This helps to reduce memory pressure, and reduce startup latency.
	 *
	 */
	if (src && !i915_is_ggtt(src) &&
	    (has_null_page(src) || vm->has_read_only)) {

		if (!has_null_page(src))
			GEM_BUG_ON(!src->has_read_only);
		return true;
	}
	return false;
}

static int ww_map_pt_dma(struct i915_address_space *vm, struct drm_i915_gem_object *px)
{
	struct i915_gem_ww_ctx ww;
	int err;

	for_i915_gem_ww(&ww, err, true)
		err = map_pt_dma(vm, &ww, px);

	return err;
}

static int gen8_init_scratch(struct i915_address_space *vm)
{
	int ret;
	int i;

	if (can_share_scratch(vm)) {
		struct i915_address_space *clone = vm->gt->vm;

		for (i = 0; i <= vm->top; i++) {
			if (clone->scratch[i])
				vm->scratch[i] = i915_gem_object_get(clone->scratch[i]);
		}

		vm->poison = clone->poison;
		return 0;
	}

	for (i = 1; i <= vm->top; i++) {
		struct drm_i915_gem_object *obj;

		obj = i915_vm_alloc_px(vm);
		if (IS_ERR(obj)) {
			ret = PTR_ERR(obj);
			goto free_scratch;
		}

		ret = ww_map_pt_dma(vm, obj);
		if (ret) {
			i915_gem_object_put(obj);
			goto free_scratch;
		}

		fill_px(obj, i915_vm_scratch_encode(vm, i - 1));
		vm->scratch[i] = obj;
	}

	return 0;

free_scratch:
	i915_vm_free_scratch(vm);
	return ret;
}

static void
set_pd_entry(struct i915_page_directory * const pd,
	     const unsigned short idx,
	     struct i915_page_table *pt)
{
	atomic_inc(px_used(pd));
	pd->entry[idx] = pt;
	write_pte(&pd->pt, idx, pde_encode(pt));
}

static struct i915_page_directory *
gen8_alloc_top_pd(struct i915_address_space *vm)
{
	const unsigned int count = gen8_pd_top_count(vm);
	struct i915_page_directory *pd;
	int err;

	GEM_BUG_ON(count > GEN8_PDES);

	pd = __alloc_pd(count);
	if (unlikely(!pd))
		return ERR_PTR(-ENOMEM);

	pd->pt.base = i915_vm_alloc_px(vm);
	if (IS_ERR(pd->pt.base)) {
		err = PTR_ERR(pd->pt.base);
		pd->pt.base = NULL;
		goto err_pd;
	}

	err = ww_map_pt_dma(vm, pd->pt.base);
	if (err)
		goto err_pd;

	fill_page_dma(px_base(pd), i915_vm_scratch_encode(vm, vm->top), count);

	atomic_inc(px_used(pd)); /* mark as pinned */
	return pd;

err_pd:
	free_pd(vm, pd);
	return ERR_PTR(err);
}

int intel_flat_lmem_ppgtt_init(struct i915_address_space *vm,
			       struct drm_mm_node *node)
{
	struct i915_page_directory *pd = i915_vm_to_ppgtt(vm)->pd;
	unsigned int idx, count;
	u64 start, end, head;
	gen8_pte_t *vaddr;
	gen8_pte_t encode;
	u32 pte_flags;
	int lvl;
	int err;

	/*
	 * Map all of LMEM in a kernel internal vm(could be cloned?). This gives
	 * us the useful property where the va == pa, which lets us touch any
	 * part of LMEM, from the gpu without having to dynamically bind
	 * anything. We map the entries as 1G GTT entries, such that we only
	 * need one pdpe for every 1G of LMEM, i.e a single pdp can cover 512G
	 * of LMEM.
	 */
	GEM_BUG_ON(!IS_ALIGNED(node->start | node->size, SZ_1G));
	GEM_BUG_ON(node->size > SZ_1G * GEN8_PDES);

	pte_flags = PTE_LM;
	if (GRAPHICS_VER_FULL(vm->i915) >= IP_VER(12, 60))
		pte_flags |= PTE_AE;

	start = node->start >> GEN8_PTE_SHIFT;
	end = start + (node->size >> GEN8_PTE_SHIFT);
	encode = GEN8_PDPE_PS_1G |
		 gen12_pte_encode(node->start,
				  i915_gem_get_pat_index(vm->i915,
							 I915_CACHE_NONE),
				  pte_flags);

	/* The vm->mm may be hiding the first page already */
	head = vm->mm.head_node.start + vm->mm.head_node.size;
	if (node->start < head) {
		GEM_BUG_ON(node->size < head - node->start);
		node->size -= head - node->start;
		node->start = head;
	}

	err = drm_mm_reserve_node(&vm->mm, node);
	if (err) {
		struct drm_printer p = drm_err_printer(__func__);

		drm_printf(&p,
			   "flat node:[%llx + %llx] already taken\n",
			   node->start, node->size);
		drm_mm_print(&vm->mm, &p);

		return err;
	}

	lvl = vm->top;
	while (lvl >= 3) { /* allocate everything up to and including the pdp */
		struct i915_page_directory *pde;

		/* Check we don't cross into the next page directory */
		GEM_BUG_ON(gen8_pd_range(start, end, lvl, &idx) != 1);

		idx = gen8_pd_index(start, lvl);
		pde = pd->entry[idx];
		if (!pde) {
			pde = alloc_pd(vm);
			if (IS_ERR(pde)) {
				err = PTR_ERR(pde);
				goto err_out;
			}

			err = ww_map_pt_dma(vm, pde->pt.base);
			if (err) {
				free_pd(vm, pde);
				goto err_out;
			}

			fill_px(pde, i915_vm_scratch_encode(vm, lvl));
			wmb();
		}

		set_pd_entry(pd, idx, &pde->pt);
		pd = pde;
		lvl--;
	}

	vaddr = px_vaddr(pd);
	count = gen8_pd_range(start, end, lvl, &idx);
	atomic_set(px_used(pd), count);
	do {
		vaddr[idx++] = encode;
		encode += SZ_1G;
	} while (--count);

	i915_write_barrier(vm->i915);
	return 0;

err_out:
	drm_mm_remove_node(node);
	return err;
}

int intel_flat_lmem_ppgtt_insert_window(struct i915_address_space *vm,
					struct drm_i915_gem_object *obj,
					struct drm_mm_node *node,
					int leaf,
					bool is_compact)
{
	struct i915_page_directory *pd = i915_vm_to_ppgtt(vm)->pd;
	gen8_pte_t *vaddr, encode;
	unsigned int idx, count;
	struct scatterlist *sg;
	u64 start, end;
	int lvl, err;

	if (!i915_gem_object_has_pinned_pages(obj))
		return -EINVAL;

	sg = obj->mm.pages;
	if (!sg_is_last(sg))
		return -EINVAL;

	node->size = mul_u32_u32(sg_dma_len(sg) >> 3,
				 leaf ? SZ_2M : is_compact ? SZ_64K : SZ_4K);
	node->size = min_t(u64, node->size, leaf ? 512ull * SZ_1G : SZ_1G);
	if (GEM_WARN_ON(node->size < SZ_2M))
		return -EINVAL;

	err = drm_mm_insert_node_in_range(&vm->mm, node,
					  node->size, roundup_pow_of_two(node->size),
					  I915_COLOR_UNEVICTABLE,
					  0, U64_MAX,
					  DRM_MM_INSERT_LOW);
	if (err)
		return err;

	start = node->start >> GEN8_PTE_SHIFT;
	end = start + (node->size >> GEN8_PTE_SHIFT);

	lvl = vm->top;
	while (lvl >= leaf + 2) {
		struct i915_page_directory *pde;

		/* Check we don't cross into the next page directory */
		GEM_BUG_ON(gen8_pd_range(start, end, lvl, &idx) != 1);

		idx = gen8_pd_index(start, lvl);
		pde = pd->entry[idx];
		if (!pde) {
			pde = alloc_pd(vm);
			if (IS_ERR(pde)) {
				err = PTR_ERR(pde);
				goto err_out;
			}

			err = ww_map_pt_dma(vm, pde->pt.base);
			if (err) {
				free_pd(vm, pde);
				goto err_out;
			}

			fill_px(pde, i915_vm_scratch_encode(vm, lvl));
			wmb();
		}

		set_pd_entry(pd, idx, &pde->pt);
		pd = pde;
		lvl--;
	}

	encode = gen8_pde_encode(sg_dma_address(sg), I915_CACHE_LLC);
	if (is_compact)
		encode |= GEN12_PDE_64K;
	vaddr = px_vaddr(pd);
	count = gen8_pd_range(start, end, lvl, &idx);
	atomic_set(px_used(pd), count);
	do {
		vaddr[idx++] = encode;
		encode += is_compact ? SZ_256 : SZ_4K;
	} while (--count);

	i915_write_barrier(vm->i915);
	return 0;

err_out:
	drm_mm_remove_node(node);
	return err;
}

void intel_flat_lmem_ppgtt_fini(struct i915_address_space *vm,
				struct drm_mm_node *node)
{
	if (!drm_mm_node_allocated(node))
		return;

	GEM_BUG_ON(node->mm != &vm->mm);
	drm_mm_remove_node(node);
}

/*
 * GEN8 legacy ppgtt programming is accomplished through a max 4 PDP registers
 * with a net effect resembling a 2-level page table in normal x86 terms. Each
 * PDP represents 1GB of memory 4 * 512 * 512 * 4096 = 4GB legacy 32b address
 * space.
 *
 */
struct i915_ppgtt *gen8_ppgtt_create(struct intel_gt *gt, u32 flags)
{
	struct i915_page_directory *pd;
	struct i915_ppgtt *ppgtt;
	int err;

	ppgtt = kzalloc(sizeof(*ppgtt), GFP_KERNEL);
	if (!ppgtt)
		return ERR_PTR(-ENOMEM);

	err = ppgtt_init(ppgtt, gt);
	if (err) {
		kfree(ppgtt);
		return ERR_PTR(err);
	}

	ppgtt->vm.pd_shift = ilog2(SZ_4K * SZ_4K / sizeof(gen8_pte_t));
	ppgtt->vm.has_read_only = true;

	if (HAS_LMEM(gt->i915))
		ppgtt->vm.alloc_pt_dma = alloc_pt_lmem;
	else
		ppgtt->vm.alloc_pt_dma = alloc_pt_dma;

	/*
	 * On some platforms the hw has dropped support for 4K GTT pages
	 * when dealing with LMEM, and due to the design of 64K GTT
	 * pages in the hw, we can only mark the *entire* page-table as
	 * operating in 64K GTT mode, since the enable bit is still on
	 * the pde, and not the pte. And since we still need to allow
	 * 4K GTT pages for SMEM objects, we can't have a "normal" 4K
	 * page-table with scratch pointing to LMEM, since that's
	 * undefined from the hw pov. The simplest solution is to just
	 * move the 64K scratch page to SMEM on all platforms and call
	 * it a day, since that should work for all configurations.
	 *
	 * Using SMEM instead of LMEM has the additional advantage of
	 * not reserving high performance memory for a "never" used
	 * filler page. It also removes the device access that would
	 * be required to initialise the scratch page, reducing pressure
	 * on an even scarcer resource.
	 */
	ppgtt->vm.alloc_scratch_dma = alloc_pt_dma;

	ppgtt->vm.pte_encode = gen12_pte_encode;
	ppgtt->vm.insert_entries = ppgtt_insert;
	if (GRAPHICS_VER_FULL(gt->i915) >= IP_VER(12, 60)) {
		ppgtt->vm.pt_insert = ps64_pt_insert;
	} else if (GRAPHICS_VER_FULL(gt->i915) >= IP_VER(12, 50)) {
		ppgtt->vm.pt_compact = GEN12_PPGTT_PTE_LM;
		ppgtt->vm.pt_insert = dg2_pt_insert;
		ppgtt->vm.mm.color_adjust = dg2_ppgtt_color_adjust;
	} else {
		ppgtt->vm.pt_insert = gen8_pt_insert;
	}
	ppgtt->vm.clear_range = gen8_ppgtt_clear;
	ppgtt->vm.cleanup = gen8_ppgtt_cleanup;

	if (flags & PRELIM_I915_VM_CREATE_FLAGS_DISABLE_SCRATCH)
		ppgtt->vm.has_scratch = false;
	if (flags & PRELIM_I915_VM_CREATE_FLAGS_ENABLE_PAGE_FAULT)
		ppgtt->vm.page_fault_enabled = true;

	err = gen8_init_scratch(&ppgtt->vm);
	if (err)
		goto err_put;

	pd = gen8_alloc_top_pd(&ppgtt->vm);
	if (IS_ERR(pd)) {
		err = PTR_ERR(pd);
		goto err_put;
	}
	ppgtt->pd = pd;

	/* Exclude the last page for wabb scratch */
	ppgtt->vm.total -= SZ_64K;
	if (!(i915_vm_scratch0_encode(&ppgtt->vm) & GEN8_PAGE_PRESENT)) {
		err = pvc_ppgtt_fault(&ppgtt->vm, ppgtt->vm.total, SZ_64K, true);
		if (err)
			goto err_put;
	}

	return ppgtt;

err_put:
	i915_vm_put(&ppgtt->vm);
	return ERR_PTR(err);
}
