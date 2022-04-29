// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include <linux/mm_types.h>
#include <linux/sched/mm.h>

#include "i915_svm.h"
#include "intel_memory_region.h"
#include "gem/i915_gem_context.h"

struct svm_notifier {
	struct mmu_interval_notifier notifier;
	struct i915_svm *svm;
};

static struct i915_svm *vm_get_svm(struct i915_address_space *vm)
{
	struct i915_svm *svm = vm->svm;

	mutex_lock(&vm->svm_mutex);
	if (svm && !kref_get_unless_zero(&svm->ref))
		svm = NULL;

	mutex_unlock(&vm->svm_mutex);
	return svm;
}

static void release_svm(struct kref *ref)
{
	struct i915_svm *svm = container_of(ref, typeof(*svm), ref);
	struct i915_address_space *vm = svm->vm;

	mmu_notifier_unregister(&svm->notifier, svm->notifier.mm);
	mutex_destroy(&svm->mutex);
	vm->svm = NULL;
	kfree(svm);
}

static void vm_put_svm(struct i915_address_space *vm)
{
	mutex_lock(&vm->svm_mutex);
	if (vm->svm)
		kref_put(&vm->svm->ref, release_svm);
	mutex_unlock(&vm->svm_mutex);
}

static u32 i915_svm_build_sg(struct i915_address_space *vm,
			     struct hmm_range *range,
			     struct sg_table *st)
{
	struct scatterlist *sg;
	u32 sg_page_sizes = 0;
	u64 i, npages;

	sg = NULL;
	st->nents = 0;
	npages = (range->end - range->start) / PAGE_SIZE;

	/*
	 * No need to dma map the host pages and later unmap it, as
	 * GPU is not allowed to access it with SVM.
	 * XXX: Need to dma map host pages for integrated graphics while
	 * extending SVM support there.
	 */
	for (i = 0; i < npages; i++) {
		unsigned long addr = range->hmm_pfns[i];

		if (sg && (addr == (sg_dma_address(sg) + sg->length))) {
			sg->length += PAGE_SIZE;
			sg_dma_len(sg) += PAGE_SIZE;
			continue;
		}

		if (sg)
			sg_page_sizes |= sg->length;

		sg =  sg ? __sg_next(sg) : st->sgl;
		sg_dma_address(sg) = addr;
		sg_dma_len(sg) = PAGE_SIZE;
		sg->length = PAGE_SIZE;
		st->nents++;
	}

	sg_page_sizes |= sg->length;
	sg_mark_end(sg);
	return sg_page_sizes;
}

static bool i915_svm_range_invalidate(struct mmu_interval_notifier *mni,
				      const struct mmu_notifier_range *range,
				      unsigned long cur_seq)
{
	struct svm_notifier *sn =
		container_of(mni, struct svm_notifier, notifier);

	/*
	 * serializes the update to mni->invalidate_seq done by caller and
	 * prevents invalidation of the PTE from progressing while HW is being
	 * programmed. This is very hacky and only works because the normal
	 * notifier that does invalidation is always called after the range
	 * notifier.
	 */
	if (mmu_notifier_range_blockable(range))
		mutex_lock(&sn->svm->mutex);
	else if (!mutex_trylock(&sn->svm->mutex))
		return false;
	mmu_interval_set_seq(mni, cur_seq);
	mutex_unlock(&sn->svm->mutex);
	return true;
}

static const struct mmu_interval_notifier_ops i915_svm_mni_ops = {
	.invalidate = i915_svm_range_invalidate,
};

static int i915_hmm_convert_pfn(struct drm_i915_private *dev_priv,
				struct hmm_range *range)
{
	unsigned long i, npages;
	int regions = 0;

	npages = (range->end - range->start) >> PAGE_SHIFT;
	for (i = 0; i < npages; ++i) {
		struct page *page;
		unsigned long addr;

		if (!(range->hmm_pfns[i] & HMM_PFN_VALID)) {
			range->hmm_pfns[i] = 0;
			continue;
		}

		page = hmm_pfn_to_page(range->hmm_pfns[i]);
		if (!page)
			continue;

		if (is_device_private_page(page)) {
			struct i915_buddy_block *block = page->zone_device_data;
			struct intel_memory_region *mem = block->private;

			regions |= REGION_LMEM;
			addr = mem->region.start + i915_buddy_block_offset(block);
			addr += (page_to_pfn(page) - block->pfn_first) << PAGE_SHIFT;
		} else {
			regions |= REGION_SMEM;
			addr = page_to_phys(page);
		}

		range->hmm_pfns[i] = addr;
	}

	return regions;
}

static int i915_range_fault(struct svm_notifier *sn,
			    struct prelim_drm_i915_gem_vm_bind *va,
			    struct sg_table *st, unsigned long *pfns)
{
	unsigned long timeout =
		jiffies + msecs_to_jiffies(HMM_RANGE_DEFAULT_TIMEOUT);
	struct i915_svm *svm = sn->svm;
	struct i915_address_space *vm = svm->vm;
	/* Have HMM fault pages within the fault window to the GPU. */
	struct hmm_range range = {
		.notifier = &sn->notifier,
		.start = sn->notifier.interval_tree.start,
		.end = sn->notifier.interval_tree.last + 1,
		.pfn_flags_mask = HMM_PFN_REQ_FAULT | HMM_PFN_REQ_WRITE,
		.hmm_pfns = pfns,
		.dev_private_owner = vm->i915->drm.dev,
	};
	struct mm_struct *mm = sn->notifier.mm;
	struct i915_vm_pt_stash stash = { 0 };
	struct i915_gem_ww_ctx ww;
	u32 sg_page_sizes;
	int regions;
	u64 flags;
	long ret;

	while (true) {
		if (time_after(jiffies, timeout))
			return -EBUSY;

		range.notifier_seq = mmu_interval_read_begin(range.notifier);
		mmap_read_lock(mm);
		ret = hmm_range_fault(&range);
		mmap_read_unlock(mm);
		if (ret) {
			if (ret == -EBUSY)
				continue;
			return ret;
		}

		/* Ensure the range is in one memory region */
		regions = i915_hmm_convert_pfn(vm->i915, &range);
		if (!regions ||
		    ((regions & REGION_SMEM) && (regions & REGION_LMEM)))
			return -EINVAL;

		sg_page_sizes = i915_svm_build_sg(vm, &range, st);

		/* XXX: Not an elegant solution, revisit */
		i915_gem_ww_ctx_init(&ww, true);
		ret = svm_bind_addr_prepare(vm, &stash, &ww, va->start, va->length);
		if (ret)
			goto fault_done;

		mutex_lock(&svm->mutex);
		if (mmu_interval_read_retry(range.notifier,
					    range.notifier_seq)) {
			svm_unbind_addr(vm, va->start, va->length);
			mutex_unlock(&svm->mutex);
			i915_vm_free_pt_stash(vm, &stash);
			i915_gem_ww_ctx_fini(&ww);
			continue;
		}
		break;
	}

	flags = (regions & REGION_LMEM) ? I915_GTT_SVM_LMEM : 0;
	flags |= (va->flags & PRELIM_I915_GEM_VM_BIND_READONLY) ?
		 I915_GTT_SVM_READONLY : 0;
	ret = svm_bind_addr_commit(vm, va->start, va->length,
				   flags, st, sg_page_sizes);
	mutex_unlock(&svm->mutex);
	i915_vm_free_pt_stash(vm, &stash);
fault_done:
	i915_gem_ww_ctx_fini(&ww);
	return ret;
}

static void __i915_gem_vm_unbind_svm_buffer(struct i915_address_space *vm,
					    u64 start, u64 length)
{
	struct i915_svm *svm = vm->svm;

	mutex_lock(&svm->mutex);
	/* FIXME: Need to flush the TLB */
	svm_unbind_addr(vm, start, length);
	mutex_unlock(&svm->mutex);
}

int i915_gem_vm_unbind_svm_buffer(struct i915_address_space *vm,
				  struct prelim_drm_i915_gem_vm_bind *va)
{
	struct i915_svm *svm;
	struct mm_struct *mm;
	int ret = 0;

	if (unlikely(!i915_vm_is_svm_enabled(vm)))
		return -ENOTSUPP;

	svm = vm_get_svm(vm);
	if (!svm)
		return -EINVAL;

	mm = svm->notifier.mm;
	if (mm != current->mm) {
		ret = -EPERM;
		goto unbind_done;
	}

	va->length += (va->start & ~PAGE_MASK);
	va->start &= PAGE_MASK;
	__i915_gem_vm_unbind_svm_buffer(vm, va->start, va->length);

unbind_done:
	vm_put_svm(vm);
	return ret;
}

int i915_gem_vm_bind_svm_buffer(struct i915_address_space *vm,
				struct prelim_drm_i915_gem_vm_bind *va)
{
	unsigned long *pfns, flags = HMM_PFN_REQ_FAULT;
	struct svm_notifier sn;
	struct i915_svm *svm;
	struct mm_struct *mm;
	struct sg_table st;
	int ret = 0;
	u64 npages;

	if (unlikely(!i915_vm_is_svm_enabled(vm)))
		return -ENOTSUPP;

	svm = vm_get_svm(vm);
	if (!svm)
		return -EINVAL;

	mm = svm->notifier.mm;
	if (mm != current->mm) {
		ret = -EPERM;
		goto bind_done;
	}

	va->length += (va->start & ~PAGE_MASK);
	va->start &= PAGE_MASK;
	npages = va->length / PAGE_SIZE;
	if (unlikely(sg_alloc_table(&st, npages, GFP_KERNEL))) {
		ret = -ENOMEM;
		goto bind_done;
	}

	pfns = kvmalloc_array(npages, sizeof(*pfns), GFP_KERNEL);
	if (unlikely(!pfns)) {
		ret = -ENOMEM;
		goto range_done;
	}

	if (!(va->flags & PRELIM_I915_GEM_VM_BIND_READONLY))
		flags |= HMM_PFN_REQ_WRITE;

	memset64((u64 *)pfns, (u64)flags, npages);

	sn.svm = svm;
	ret = mmu_interval_notifier_insert(&sn.notifier, mm,
					   va->start, va->length,
					   &i915_svm_mni_ops);
	if (!ret) {
		ret = i915_range_fault(&sn, va, &st, pfns);
		mmu_interval_notifier_remove(&sn.notifier);
	}

	kvfree(pfns);
range_done:
	sg_free_table(&st);
bind_done:
	vm_put_svm(vm);
	return ret;
}

static int
i915_svm_invalidate_range_start(struct mmu_notifier *mn,
				const struct mmu_notifier_range *update)
{
	struct i915_svm *svm = container_of(mn, struct i915_svm, notifier);
	unsigned long length = update->end - update->start;

	DRM_DEBUG_DRIVER("start 0x%lx length 0x%lx\n", update->start, length);
	if (!mmu_notifier_range_blockable(update))
		return -EAGAIN;

	__i915_gem_vm_unbind_svm_buffer(svm->vm, update->start, length);
	return 0;
}

static const struct mmu_notifier_ops i915_mn_ops = {
	.invalidate_range_start = i915_svm_invalidate_range_start,
};

void i915_svm_unbind_mm(struct i915_address_space *vm)
{
	vm_put_svm(vm);
}

int i915_svm_bind_mm(struct i915_address_space *vm)
{
	struct mm_struct *mm = current->mm;
	struct i915_svm *svm;
	int ret = 0;

	mmap_write_lock(mm);
	mutex_lock(&vm->svm_mutex);
	if (vm->svm)
		goto bind_out;

	svm = kzalloc(sizeof(*svm), GFP_KERNEL);
	if (!svm) {
		ret = -ENOMEM;
		goto bind_out;
	}
	mutex_init(&svm->mutex);
	kref_init(&svm->ref);
	svm->vm = vm;

	svm->notifier.ops = &i915_mn_ops;
	ret = __mmu_notifier_register(&svm->notifier, mm);
	if (ret) {
		kfree(svm);
		goto bind_out;
	}

	vm->svm = svm;
bind_out:
	mutex_unlock(&vm->svm_mutex);
	mmap_write_unlock(mm);
	return ret;
}
