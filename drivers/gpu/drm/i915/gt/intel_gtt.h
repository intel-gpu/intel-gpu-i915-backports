/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2020 Intel Corporation
 *
 * Please try to maintain the following order within this file unless it makes
 * sense to do otherwise. From top to bottom:
 * 1. typedefs
 * 2. #defines, and macros
 * 3. structure definitions
 * 4. function prototypes
 *
 * Within each section, please try to order by generation in ascending order,
 * from top to bottom (ie. gen6 on the top, gen8 on the bottom).
 */

#ifndef __INTEL_GTT_H__
#define __INTEL_GTT_H__

#include <linux/io-mapping.h>
#include <linux/kref.h>
#include <linux/mm.h>
#include <linux/pagevec.h>
#include <linux/scatterlist.h>
#include <linux/workqueue.h>

#include <drm/drm_mm.h>

#include "gt/intel_reset.h"
#include "i915_scatterlist.h"
#include "i915_selftest.h"
#include "i915_vma_types.h"
#include "i915_params.h"
#include "intel_memory_region.h"

#if IS_ENABLED(CPTCFG_DRM_I915_TRACE_GTT)
#define DBG(...) trace_printk(__VA_ARGS__)
#else
#define DBG(...)
#endif

#define I915_GTT_PAGE_SIZE_4K	BIT_ULL(12)
#define I915_GTT_PAGE_SIZE_64K	BIT_ULL(16)
#define I915_GTT_PAGE_SIZE_2M	BIT_ULL(21)
#define I915_GTT_PAGE_SIZE_1G	BIT_ULL(30)

#define I915_GTT_PAGE_SIZE I915_GTT_PAGE_SIZE_4K
#define I915_GTT_MAX_PAGE_SIZE I915_GTT_PAGE_SIZE_1G

#define I915_GTT_PAGE_MASK -I915_GTT_PAGE_SIZE

#define I915_GTT_MIN_ALIGNMENT I915_GTT_PAGE_SIZE

typedef u32 gen6_pte_t;
typedef u64 gen8_pte_t;

#define ggtt_total_entries(ggtt) ((ggtt)->vm.total >> PAGE_SHIFT)

#define GEN12_PPGTT_PTE_PAT3	BIT_ULL(62)
#define GEN12_PPGTT_PTE_LM	BIT_ULL(11)
#define GEN12_USM_PPGTT_PTE_AE	BIT_ULL(10)
#define GEN12_PPGTT_PTE_PAT2	BIT_ULL(7)
#define GEN12_PPGTT_PTE_NC	BIT_ULL(5)
#define GEN12_PPGTT_PTE_PAT1	BIT_ULL(4)
#define GEN12_PPGTT_PTE_PAT0	BIT_ULL(3)
#define GEN12_PPGTT_PTE_FF	BIT_ULL(2)

/*
 *  DOC: GEN12 GGTT Table Entry format
 *
 * TGL:
 *
 * +----------+---------+---------+-----------------+--------------+---------+
 * |    63:46 |   45:12 |    11:5 |             4:2 |            1 |       0 |
 * +==========+=========+=========+=================+==============+=========+
 * |  Ignored | Address | Ignored | Function Number | Local Memory | Present |
 * +----------+---------+---------+-----------------+--------------+---------+
 *
 * ADL-P/S:
 * +----------+--------------+-------------------+---------+---------+----------+--------+---------+
 * |    63:46 |        45:42 |             41:39 |   38:12 |   11:5  |      4:2 |      1 |       0 |
 * +==========+==============+===================+=========+=========+==========+========+=========+
 * |  Ignored | MKTME key ID | 2LM Far Memory    | Address | Ignored | Function | Local  | Present |
 * |          |              | address extension |         |         | Number   | Memory |         |
 * +----------+--------------+-------------------+---------+---------+----------+--------+---------+
 *
 * Platforms supporting more than 7 VFs (XEHPSDV and later):
 *
 * +----------+---------+-----------------+--------------+---------+
 * |    63:46 |   45:12 |            11:2 |            1 |       0 |
 * +==========+=========+=================+==============+=========+
 * |  Ignored | Address | Function Number | Local Memory | Present |
 * +----------+---------+-----------------+--------------+---------+
 */

#define GEN12_GGTT_PTE_LM		BIT_ULL(1)
#define MTL_GGTT_PTE_PAT0		BIT_ULL(52)
#define MTL_GGTT_PTE_PAT1		BIT_ULL(53)
#define TGL_GGTT_PTE_VFID_MASK		GENMASK_ULL(4, 2)
#define XEHPSDV_GGTT_PTE_VFID_MASK	GENMASK_ULL(11, 2)
#define GEN12_GGTT_PTE_ADDR_MASK	GENMASK_ULL(45, 12)
#define ADL_GGTT_PTE_ADDR_MASK		GENMASK_ULL(38, 12)
#define MTL_GGTT_PTE_PAT_MASK		GENMASK_ULL(53, 52)

#define GEN12_PDE_64K BIT(6)
#define GEN12_PTE_PS64 BIT(8)

/*
 * Cacheability Control is a 4-bit value. The low three bits are stored in bits
 * 3:1 of the PTE, while the fourth bit is stored in bit 11 of the PTE.
 */
#define HSW_CACHEABILITY_CONTROL(bits)	((((bits) & 0x7) << 1) | \
					 (((bits) & 0x8) << (11 - 3)))
#define HSW_WB_LLC_AGE3			HSW_CACHEABILITY_CONTROL(0x2)
#define HSW_WB_LLC_AGE0			HSW_CACHEABILITY_CONTROL(0x3)
#define HSW_WB_ELLC_LLC_AGE3		HSW_CACHEABILITY_CONTROL(0x8)
#define HSW_WB_ELLC_LLC_AGE0		HSW_CACHEABILITY_CONTROL(0xb)
#define HSW_WT_ELLC_LLC_AGE3		HSW_CACHEABILITY_CONTROL(0x7)
#define HSW_WT_ELLC_LLC_AGE0		HSW_CACHEABILITY_CONTROL(0x6)
#define HSW_PTE_UNCACHED		(0)
#define HSW_GTT_ADDR_ENCODE(addr)	((addr) | (((addr) >> 28) & 0x7f0))
#define HSW_PTE_ADDR_ENCODE(addr)	HSW_GTT_ADDR_ENCODE(addr)

/*
 * GEN8 32b style address is defined as a 3 level page table:
 * 31:30 | 29:21 | 20:12 |  11:0
 * PDPE  |  PDE  |  PTE  | offset
 * The difference as compared to normal x86 3 level page table is the PDPEs are
 * programmed via register.
 *
 * GEN8 48b style address is defined as a 4 level page table:
 * 47:39 | 38:30 | 29:21 | 20:12 |  11:0
 * PML4E | PDPE  |  PDE  |  PTE  | offset
 */
#define GEN8_3LVL_PDPES			4

#define PPAT_UNCACHED			(_PAGE_PWT | _PAGE_PCD)
#define PPAT_CACHED_PDE			0 /* WB LLC */
#define PPAT_CACHED			_PAGE_PAT /* WB LLCeLLC */
#define PPAT_DISPLAY_ELLC		_PAGE_PCD /* WT eLLC */

#define CHV_PPAT_SNOOP			REG_BIT(6)
#define GEN12_PPAT_CLOS(x)              ((x)<<2)
#define GEN8_PPAT_AGE(x)		((x)<<4)
#define GEN8_PPAT_LLCeLLC		(3<<2)
#define GEN8_PPAT_LLCELLC		(2<<2)
#define GEN8_PPAT_LLC			(1<<2)
#define GEN8_PPAT_WB			(3<<0)
#define GEN8_PPAT_WT			(2<<0)
#define GEN8_PPAT_WC			(1<<0)
#define GEN8_PPAT_UC			(0<<0)
#define GEN8_PPAT_ELLC_OVERRIDE		(0<<2)
#define GEN8_PPAT(i, x)			((u64)(x) << ((i) * 8))

#define GEN8_PAGE_PRESENT		BIT_ULL(0)
#define GEN8_PAGE_RW			BIT_ULL(1)
#define PTE_NULL_PAGE			BIT_ULL(9)

#define GEN8_PDE_IPS_64K BIT_ULL(11)
#define GEN8_PDE_PS_2M   BIT_ULL(7)
#define GEN8_PDPE_PS_1G  BIT_ULL(7)

#define MTL_PPAT_L4_CACHE_POLICY_MASK	REG_GENMASK(3, 2)
#define MTL_PAT_INDEX_COH_MODE_MASK	REG_GENMASK(1, 0)
#define MTL_PPAT_L4_3_UC	REG_FIELD_PREP(MTL_PPAT_L4_CACHE_POLICY_MASK, 3)
#define MTL_PPAT_L4_1_WT	REG_FIELD_PREP(MTL_PPAT_L4_CACHE_POLICY_MASK, 1)
#define MTL_PPAT_L4_0_WB	REG_FIELD_PREP(MTL_PPAT_L4_CACHE_POLICY_MASK, 0)
#define MTL_3_COH_2W	REG_FIELD_PREP(MTL_PAT_INDEX_COH_MODE_MASK, 3)
#define MTL_2_COH_1W	REG_FIELD_PREP(MTL_PAT_INDEX_COH_MODE_MASK, 2)
#define MTL_0_COH_NON	REG_FIELD_PREP(MTL_PAT_INDEX_COH_MODE_MASK, 0)

enum i915_cache_level;

struct drm_i915_gem_object;
struct i915_drm_client;
struct i915_fence_reg;
struct i915_vma;
struct intel_gt;

#define for_each_sgt_daddr(__dp, __iter, __sgt) \
	__for_each_sgt_daddr(__dp, __iter, __sgt, I915_GTT_PAGE_SIZE)

/* iterate through those GTs which contain a unique GGTT reference */
#define for_each_ggtt(gt__, i915__, __id__) \
	for_each_gt(gt__, i915__, __id__) for_each_if((gt__)->type != GT_MEDIA)

struct i915_page_table {
	struct drm_i915_gem_object *base;
	atomic_t used;
	bool is_compact:1;
	bool is_64k:1;
};

struct i915_page_directory {
	struct i915_page_table pt;
	void **entry;
};

#define __px_choose_expr(x, type, expr, other) \
	__builtin_choose_expr( \
	__builtin_types_compatible_p(typeof(x), type) || \
	__builtin_types_compatible_p(typeof(x), const type), \
	({ type __x = (type)(x); expr; }), \
	other)

#define px_base(px) \
	__px_choose_expr(px, struct drm_i915_gem_object *, __x, \
	__px_choose_expr(px, struct i915_page_table *, __x->base, \
	__px_choose_expr(px, struct i915_page_directory *, __x->pt.base, \
	(void)0)))

#define px_dma(px) (__px_dma(px_base(px)))
static inline dma_addr_t __px_dma(struct drm_i915_gem_object *p)
{
	return sg_dma_address(p->mm.pages);
}

#define px_vaddr(px) (__px_vaddr(px_base(px)))
static inline void *__px_vaddr(struct drm_i915_gem_object *p)
{
	return page_mask_bits(p->mm.mapping);
}

static inline void
fill_page_dma(struct drm_i915_gem_object *p, const u64 val, unsigned int count)
{
	memset64(__px_vaddr(p), val, count);
}

#define px_pt(px) \
	__px_choose_expr(px, struct i915_page_table *, __x, \
	__px_choose_expr(px, struct i915_page_directory *, &__x->pt, \
	(void)0))
#define px_used(px) (&px_pt(px)->used)

struct i915_vma_ops {
	/* Map an object into an address space with the given cache flags. */
	int (*bind_vma)(struct i915_address_space *vm,
			struct i915_vma *vma,
			struct i915_gem_ww_ctx *ww,
			unsigned int pat_index,
			u32 flags);
	/*
	 * Unmap an object from an address space. This usually consists of
	 * setting the valid PTE entries to a reserved scratch page.
	 */
	void (*unbind_vma)(struct i915_address_space *vm,
			   struct i915_vma *vma);

	int (*set_pages)(struct i915_vma *vma);
	void (*clear_pages)(struct i915_vma *vma);
};

struct pt_insert;

struct i915_address_space {
	struct kref ref;
	struct rcu_work rcu;

	struct drm_mm mm;
	struct intel_gt *gt;
	struct drm_i915_private *i915;

	/*
	 * Every address space belongs to a struct file, a single client -
	 * except for the global GTT that is owned by the driver (and so @file
	 * is set to NULL). In principle, no information should leak from one
	 * context to another (or between files/processes etc) unless
	 * explicitly shared by the owner. Tracking the owner is important in
	 * order to free up per-file objects along with the file, to aide
	 * resource tracking, and to assign blame.
	 */
	struct i915_drm_client *client;

	struct inode *inode;

	u32 asid;
	u32 poison; /* value used to fill the scratch page */

	struct i915_vm_tlb {
		spinlock_t lock;
		struct rb_root_cached range;
		u32 last;
		bool has_error:1;
	} tlb[I915_MAX_GT];

	u64 total;		/* size addr space maps (ex. 2GB for ggtt) */
	u64 reserved;		/* size addr space reserved */
	u64 min_alignment[INTEL_REGION_UNKNOWN];
	u64 fault_start, fault_end;

	/*
	 * Each active user context has its own address space (in full-ppgtt).
	 * Since the vm may be shared between multiple contexts, we count how
	 * many contexts keep us "open". Once open hits zero, we are closed
	 * and do not allow any new attachments, and proceed to shutdown our
	 * vma and page directories.
	 */
	atomic_t open;
	struct work_struct close_work;

	struct mutex mutex; /* protects vma and our lists */
	seqcount_t seqlock;

#define VM_CLASS_GGTT 0
#define VM_CLASS_PPGTT 1
#define VM_CLASS_DPT 2

#define I915_MAX_PD_LVL 5
	struct drm_i915_gem_object *scratch[I915_MAX_PD_LVL];

	/**
	 * List of vma currently bound.
	 */
	struct list_head bound_list;

	/**
	 * List of VM_BIND objects.
	 */
	struct mutex vm_bind_lock;  /* Protects vm_bind lists */
	struct list_head vm_bind_list;
	struct list_head vm_bound_list;
	struct list_head vm_capture_list;
	spinlock_t vm_capture_lock;  /* Protects vm_capture_list */
	/* va tree of persistent vmas */
	struct rb_root_cached va;
	struct drm_i915_gem_object *root_obj;

	spinlock_t priv_obj_lock;
	struct list_head priv_obj_list;
	struct i915_active_fence user_fence;

	unsigned long flags;
#define I915_VM_HAS_PERSISTENT_BINDS 0

	/* Global GTT */
	bool is_ggtt:1;

	/* Display page table */
	bool is_dpt:1;

	/* Some systems support read-only mappings for GGTT and/or PPGTT */
	bool has_read_only:1;

	/* Does address space maps to a valid scratch page */
	bool has_scratch:1;

	/* Is address space enabled for recoverable page faults? */
	bool page_fault_enabled:1;

	unsigned int pt_compact;

	u8 top;
	u8 pd_shift;

	struct drm_i915_gem_object *
		(*alloc_pt_dma)(struct i915_address_space *vm, int sz);
	struct drm_i915_gem_object *
		(*alloc_scratch_dma)(struct i915_address_space *vm, int sz);

	u64 (*pte_encode)(dma_addr_t addr,
			  unsigned int pat_index,
			  u32 flags); /* Create a valid PTE */
#define PTE_READ_ONLY	BIT(0)
#define PTE_LM		BIT(1)
#define PTE_AE		BIT(2)
#define PTE_FF		BIT(3)
	gen8_pte_t (*pt_insert)(struct pt_insert *arg,
				struct i915_page_table *pt);

	void (*clear_range)(struct i915_address_space *vm,
			    u64 start, u64 length);
	void (*scratch_range)(struct i915_address_space *vm,
			    u64 start, u64 length);
	void (*insert_page)(struct i915_address_space *vm,
			    dma_addr_t addr,
			    u64 offset,
			    unsigned int pat_index,
			    u32 flags);
	int (*insert_entries)(struct i915_address_space *vm,
			      struct i915_vma *vma,
			      struct i915_gem_ww_ctx *ww,
			      unsigned int pat_index,
			      u32 flags);
	void (*cleanup)(struct i915_address_space *vm);

	struct i915_vma_ops vma_ops;

	I915_SELFTEST_DECLARE(struct fault_attr fault_attr);
	I915_SELFTEST_DECLARE(bool scrub_64K);

	struct i915_active active;

	/* Per tile active users of this VM */
	atomic_t active_contexts[I915_MAX_GT];
};

/*
 * The Graphics Translation Table is the way in which GEN hardware translates a
 * Graphics Virtual Address into a Physical Address. In addition to the normal
 * collateral associated with any va->pa translations GEN hardware also has a
 * portion of the GTT which can be mapped by the CPU and remain both coherent
 * and correct (in cases like swizzling). That region is referred to as GMADR in
 * the spec.
 */
struct i915_ggtt {
	struct i915_address_space vm;

	/** "Graphics Stolen Memory" holds the global PTEs */
	void __iomem *gsm;
	void (*invalidate)(struct i915_ggtt *ggtt);

	u32 pin_bias;

	struct drm_mm_node uc_fw;

	/** List of GTs mapping this GGTT */
	struct list_head gt_list;

	/* Sleepable RCU for blocking on address computations. */
	struct srcu_struct blocked_srcu;
	unsigned long flags;
#define GGTT_ADDRESS_COMPUTE_BLOCKED	0
	/** Waitqueue to signal when the blocking has completed. */
	wait_queue_head_t queue;
};

struct i915_ppgtt {
	struct i915_address_space vm;

	struct i915_page_directory *pd;
};

#define i915_is_ggtt(vm) ((vm)->is_ggtt)
#define i915_is_dpt(vm) ((vm)->is_dpt)
#define i915_is_ggtt_or_dpt(vm) (i915_is_ggtt(vm) || i915_is_dpt(vm))

/* lock the vm into the current ww, if we lock one, we lock all */
int i915_vm_lock_objects(const struct i915_address_space *vm,
			 struct i915_gem_ww_ctx *ww);

static inline unsigned int
i915_vm_lvl(const struct i915_address_space * const vm)
{
	return vm->top + 1;
}

static inline u64 i915_vm_min_alignment(struct i915_address_space *vm,
					enum intel_memory_type type)
{
	return vm->min_alignment[type];
}

static inline bool
i915_vm_has_memory_coloring(struct i915_address_space *vm)
{
       return vm->mm.color_adjust;
}

static inline bool
i915_vm_page_fault_enabled(struct i915_address_space *vm)
{
	return vm->page_fault_enabled;
}

static inline struct i915_ggtt *
i915_vm_to_ggtt(struct i915_address_space *vm)
{
	BUILD_BUG_ON(offsetof(struct i915_ggtt, vm));
	GEM_BUG_ON(!i915_is_ggtt(vm));
	return container_of(vm, struct i915_ggtt, vm);
}

static inline struct i915_ppgtt *
i915_vm_to_ppgtt(struct i915_address_space *vm)
{
	BUILD_BUG_ON(offsetof(struct i915_ppgtt, vm));
	GEM_BUG_ON(i915_is_ggtt_or_dpt(vm));
	return container_of(vm, struct i915_ppgtt, vm);
}

static inline struct i915_address_space *
i915_vm_get(struct i915_address_space *vm)
{
	kref_get(&vm->ref);
	return vm;
}

static inline struct i915_address_space *
i915_vm_tryget(struct i915_address_space *vm)
{
	if (likely(kref_get_unless_zero(&vm->ref)))
		return vm;

	return NULL;
}

void i915_vm_release(struct kref *kref);

static inline void i915_vm_put(struct i915_address_space *vm)
{
	kref_put(&vm->ref, i915_vm_release);
}

static inline struct i915_address_space *
i915_vm_open(struct i915_address_space *vm)
{
	GEM_BUG_ON(!atomic_read(&vm->open));
	atomic_inc(&vm->open);
	return i915_vm_get(vm);
}

static inline bool
i915_vm_tryopen(struct i915_address_space *vm)
{
	if (atomic_add_unless(&vm->open, 1, 0))
		return i915_vm_get(vm);

	return false;
}

void __i915_vm_close(struct i915_address_space *vm, bool imm);
static inline void i915_vm_close(struct i915_address_space *vm)
{
	return __i915_vm_close(vm, false);
}
static inline void i915_vm_close_imm(struct i915_address_space *vm)
{
	return __i915_vm_close(vm, true);
}

int i915_address_space_init(struct i915_address_space *vm, int subclass);
void i915_address_space_fini(struct i915_address_space *vm);

static inline dma_addr_t
i915_page_dir_dma_addr(const struct i915_ppgtt *ppgtt, const unsigned int n)
{
	struct i915_page_table *pt = ppgtt->pd->entry[n];

	return __px_dma(pt ? px_base(pt) : ppgtt->vm.scratch[ppgtt->vm.top]);
}

int ppgtt_init(struct i915_ppgtt *ppgtt, struct intel_gt *gt);

int intel_ggtt_bind_vma(struct i915_address_space *vm,
			struct i915_vma *vma,
			struct i915_gem_ww_ctx *ww,
			unsigned int pat_index,
			u32 flags);
void intel_ggtt_unbind_vma(struct i915_address_space *vm, struct i915_vma *vma);

int i915_ggtt_probe_hw(struct drm_i915_private *i915);
int i915_init_ggtt(struct drm_i915_private *i915);
void i915_ggtt_driver_release(struct drm_i915_private *i915);
void i915_ggtt_driver_late_release(struct drm_i915_private *i915);

int i915_ggtt_balloon(struct i915_ggtt *ggtt, u64 start, u64 end,
		      struct drm_mm_node *node);
void i915_ggtt_deballoon(struct i915_ggtt *ggtt, struct drm_mm_node *node);

bool i915_ggtt_has_xehpsdv_pte_vfid_mask(struct i915_ggtt *ggtt);

void i915_ggtt_set_space_owner(struct i915_ggtt *ggtt, u16 vfid,
			       const struct drm_mm_node *node);

#define I915_GGTT_SAVE_PTES_NO_VFID BIT(31)

int i915_ggtt_save_ptes(struct i915_ggtt *ggtt, const struct drm_mm_node *node, void *buf,
			unsigned int size, unsigned int flags);

#define I915_GGTT_RESTORE_PTES_NEW_VFID  BIT(31)
#define I915_GGTT_RESTORE_PTES_VFID_MASK GENMASK(19, 0)

int i915_ggtt_restore_ptes(struct i915_ggtt *ggtt, const struct drm_mm_node *node, const void *buf,
			   unsigned int size, unsigned int flags);

struct i915_ppgtt *i915_ppgtt_create(struct intel_gt *gt, u32 flags);

void i915_ggtt_suspend_vm(struct i915_address_space *vm);
void i915_ggtt_resume_vm(struct i915_address_space *vm);
void i915_ggtt_suspend(struct i915_ggtt *gtt);
void i915_ggtt_resume(struct i915_ggtt *ggtt);

void
fill_page_dma(struct drm_i915_gem_object *p, const u64 val, unsigned int count);

#define fill_px(px, v) fill_page_dma(px_base(px), (v), PAGE_SIZE / sizeof(u64))
#define fill32_px(px, v) do {						\
	u64 v__ = lower_32_bits(v);					\
	fill_px((px), v__ << 32 | v__);					\
} while (0)

u64 gen8_pde_encode(const dma_addr_t addr, const enum i915_cache_level level);
void i915_vm_free_scratch(struct i915_address_space *vm);
u64 i915_vm_scratch_encode(struct i915_address_space *vm, int lvl);
#define i915_vm_scratch0_encode(vm) i915_vm_scratch_encode(vm, 0)
#define i915_vm_ggtt_scratch0_encode(vm) i915_vm_scratch0_encode(vm)
#define has_null_page(vm) (i915_vm_scratch0_encode(vm) & PTE_NULL_PAGE)

struct drm_i915_gem_object *alloc_pt_dma(struct i915_address_space *vm, int sz);
struct drm_i915_gem_object *alloc_pt_lmem(struct i915_address_space *vm, int sz);
struct i915_page_table *alloc_pt(struct i915_address_space *vm, int sz);
struct i915_page_directory *alloc_pd(struct i915_address_space *vm);
struct i915_page_directory *__alloc_pd(int npde);

struct drm_i915_gem_object *
i915_vm_alloc_px(struct i915_address_space *vm);

void free_px(struct i915_address_space *vm,
	     struct i915_page_table *pt, int lvl);
#define free_pt(vm, px) free_px(vm, px, 0)
#define free_pd(vm, px) free_px(vm, px_pt(px), 1)

int map_pt_dma(struct i915_address_space *vm, struct i915_gem_ww_ctx *ww, struct drm_i915_gem_object *obj);

void gen8_set_pte(void __iomem *addr, gen8_pte_t pte);
gen8_pte_t gen8_get_pte(void __iomem *addr);

u64 ggtt_addr_to_pte_offset(u64 ggtt_addr);

void i915_ggtt_address_lock_init(struct i915_ggtt *ggtt);
void i915_ggtt_address_lock_fini(struct i915_ggtt *ggtt);
int gt_ggtt_address_read_lock_sync(struct intel_gt *gt, int *srcu);
int gt_ggtt_address_read_lock_interruptible(struct intel_gt *gt, int *srcu);
void gt_ggtt_address_read_lock(struct intel_gt *gt, int *srcu);
void gt_ggtt_address_read_unlock(struct intel_gt *gt, int srcu);
void i915_ggtt_address_write_lock(struct drm_i915_private *i915);
void i915_ggtt_address_write_unlock(struct drm_i915_private *i915);

int ggtt_set_pages(struct i915_vma *vma);
void ggtt_clear_pages(struct i915_vma *vma);
int ppgtt_set_pages(struct i915_vma *vma);
void ppgtt_clear_pages(struct i915_vma *vma);

int ppgtt_bind_vma(struct i915_address_space *vm,
		   struct i915_vma *vma,
		   struct i915_gem_ww_ctx *ww,
		   unsigned int pat_index,
		   u32 flags);
void ppgtt_unbind_vma(struct i915_address_space *vm,
		      struct i915_vma *vma);

u32 ppgtt_tlb_range(struct i915_address_space *vm, struct intel_gt *gt, u64 start, u64 end);
void ppgtt_tlb_cleanup(struct i915_address_space *vm);

void setup_private_pat(struct intel_gt *gt);

u64 i915_vm_estimate_pt_size(struct i915_address_space *vm, u64 size);

int i915_px_cache_init(struct intel_gt *gt);
bool i915_px_cache_release(struct intel_gt *gt);
void i915_px_cache_fini(struct intel_gt *gt);

struct i915_vma *
__vm_create_scratch_for_read(struct i915_address_space *vm, unsigned long size);

struct i915_vma *
__vm_create_scratch_for_read_pinned(struct i915_address_space *vm, unsigned long size);

static inline struct sgt_dma {
	struct scatterlist *sg;
	dma_addr_t dma, max;
	u64 rem;
} sgt_dma(struct i915_vma *vma) {
	struct scatterlist *sg = vma->pages;
	u64 max, offset = 0;
	dma_addr_t addr;

	/* For partial binding, skip until specified offset */
	if (vma->ggtt_view.type == I915_GGTT_VIEW_PARTIAL) {
		offset = vma->ggtt_view.partial.offset << PAGE_SHIFT;
		while (offset >= sg_dma_len(sg)) {
			offset -= sg_dma_len(sg);
			sg = __sg_next(sg);
		}
	}

	addr = sg_dma_address(sg) + offset;
	max = addr + min_t(u64, (sg_dma_len(sg) - offset), vma->size);
	return (struct sgt_dma) { sg, addr, max, vma->size };
}

static inline void
i915_vm_heal_scratch(struct i915_address_space *vm, u64 start, u64 end)
{
	/* Try to heal the edges of the scratch */
	if (start <= vm->fault_start)
		vm->fault_start = start;
	if (end >= vm->fault_end)
		vm->fault_end = start;

	/* Reset for tight bounds on the next invalid fault */
	if (vm->fault_end <= vm->fault_start)
		vm->fault_end = 0, vm->fault_start = U64_MAX;
}

int __init intel_ppgtt_module_init(void);
void intel_ppgtt_module_exit(void);

#endif
