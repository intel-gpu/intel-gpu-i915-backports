/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2016 Intel Corporation
 */

#ifndef __I915_GEM_OBJECT_TYPES_H__
#define __I915_GEM_OBJECT_TYPES_H__

#ifdef BPM_MMU_INTERVAL_NOTIFIER_NOTIFIER_NOT_PRESENT
#include <linux/i915_gem_mmu_notifier.h>
#else
#include <linux/mmu_notifier.h>
#endif

#include <drm/drm_gem.h>
#include <uapi/drm/i915_drm.h>

#include "i915_active.h"
#include "i915_scatterlist.h"
#include "i915_selftest.h"

#include "gt/intel_gt_defines.h"

struct drm_i915_gem_object;
struct intel_fronbuffer;

/*
 * struct i915_lut_handle tracks the fast lookups from handle to vma used
 * for execbuf. Although we use a radixtree for that mapping, in order to
 * remove them as the object or context is closed, we need a secondary list
 * and a translation entry (i915_lut_handle).
 */
struct i915_lut_handle {
	struct list_head obj_link;
	struct i915_gem_context *ctx;
	u32 handle;
};

struct drm_i915_gem_object_ops {
	unsigned int flags;
#define I915_GEM_OBJECT_HAS_STRUCT_PAGE	BIT(0)
#define I915_GEM_OBJECT_HAS_IOMEM	BIT(1)
#define I915_GEM_OBJECT_IS_PROXY	BIT(2)
#define I915_GEM_OBJECT_NO_MMAP		BIT(3)

	/* Interface between the GEM object and its backing storage.
	 * get_pages() is called once prior to the use of the associated set
	 * of pages before to binding them into the GTT, and put_pages() is
	 * called after we no longer need them. As we expect there to be
	 * associated cost with migrating pages between the backing storage
	 * and making them available for the GPU (e.g. clflush), we may hold
	 * onto the pages after they are no longer referenced by the GPU
	 * in case they may be used again shortly (for example migrating the
	 * pages to a different memory domain within the GTT). put_pages()
	 * will therefore most likely be called when the object itself is
	 * being released or under memory pressure (where we attempt to
	 * reap pages for the shrinker).
	 */
	int (*get_pages)(struct drm_i915_gem_object *obj);
	int (*put_pages)(struct drm_i915_gem_object *obj,
			 struct scatterlist *pages);
	void (*truncate)(struct drm_i915_gem_object *obj);

	int (*dmabuf_export)(struct drm_i915_gem_object *obj);

	void (*release)(struct drm_i915_gem_object *obj);

	const char *name; /* friendly name for debug, e.g. lockdep classes */
};

/**
 * enum i915_cache_level - The supported GTT caching values for system memory
 * pages.
 *
 * These translate to some special GTT PTE bits when binding pages into some
 * address space. It also determines whether an object, or rather its pages are
 * coherent with the GPU, when also reading or writing through the CPU cache
 * with those pages.
 *
 * Userspace can also control this through struct drm_i915_gem_caching.
 */
enum i915_cache_level {
	/**
	 * @I915_CACHE_NONE:
	 *
	 * GPU access is not coherent with the CPU cache. If the cache is dirty
	 * and we need the underlying pages to be coherent with some later GPU
	 * access then we need to manually flush the pages.
	 *
	 * On shared LLC platforms reads and writes through the CPU cache are
	 * still coherent even with this setting. See also
	 * &drm_i915_gem_object.cache_coherent for more details. Due to this we
	 * should only ever use uncached for scanout surfaces, otherwise we end
	 * up over-flushing in some places.
	 *
	 * This is the default on non-LLC platforms.
	 */
	I915_CACHE_NONE = 0,
	/**
	 * @I915_CACHE_LLC:
	 *
	 * GPU access is coherent with the CPU cache. If the cache is dirty,
	 * then the GPU will ensure that access remains coherent, when both
	 * reading and writing through the CPU cache. GPU writes can dirty the
	 * CPU cache.
	 *
	 * Not used for scanout surfaces.
	 *
	 * Applies to both platforms with shared LLC(HAS_LLC), and snooping
	 * based platforms(HAS_SNOOP).
	 *
	 * This is the default on shared LLC platforms.  The only exception is
	 * scanout objects, where the display engine is not coherent with the
	 * CPU cache. For such objects I915_CACHE_NONE or I915_CACHE_WT is
	 * automatically applied by the kernel in pin_for_display, if userspace
	 * has not done so already.
	 */
	I915_CACHE_LLC,
	/**
	 * @I915_CACHE_L3_LLC:
	 *
	 * Explicitly enable the Gfx L3 cache, with coherent LLC.
	 *
	 * The Gfx L3 sits between the domain specific caches, e.g
	 * sampler/render caches, and the larger LLC. LLC is coherent with the
	 * GPU, but L3 is only visible to the GPU, so likely needs to be flushed
	 * when the workload completes.
	 *
	 * Not used for scanout surfaces.
	 *
	 * Only exposed on some gen7 + GGTT. More recent hardware has dropped
	 * this explicit setting, where it should now be enabled by default.
	 */
	I915_CACHE_L3_LLC,
	/**
	 * @I915_CACHE_WT:
	 *
	 * Write-through. Used for scanout surfaces.
	 *
	 * The GPU can utilise the caches, while still having the display engine
	 * be coherent with GPU writes, as a result we don't need to flush the
	 * CPU caches when moving out of the render domain. This is the default
	 * setting chosen by the kernel, if supported by the HW, otherwise we
	 * fallback to I915_CACHE_NONE. On the CPU side writes through the CPU
	 * cache still need to be flushed, to remain coherent with the display
	 * engine.
	 */
	I915_CACHE_WT,
	I915_MAX_CACHE_LEVEL,
};

enum i915_map_type {
	I915_MAP_WB = 0,
	I915_MAP_WC,
#define I915_MAP_OVERRIDE BIT(31)
	I915_MAP_FORCE_WB = I915_MAP_WB | I915_MAP_OVERRIDE,
	I915_MAP_FORCE_WC = I915_MAP_WC | I915_MAP_OVERRIDE,
};

enum i915_mmap_type {
	I915_MMAP_TYPE_GTT = 0,
	I915_MMAP_TYPE_WC,
	I915_MMAP_TYPE_WB,
	I915_MMAP_TYPE_UC,
};

struct i915_mmap_offset {
	struct drm_vma_offset_node vma_node;
	struct drm_i915_gem_object *obj;
	enum i915_mmap_type mmap_type;

	struct rb_node offset;
};

struct i915_gem_object_page_iter {
	struct scatterlist *sg_pos;
	unsigned int sg_idx; /* in pages, but 32bit eek! */

	struct radix_tree_root radix;
	struct mutex lock; /* protects this cache */
};

struct i915_resv {
	struct dma_resv base;
	union {
		struct kref refcount;
		struct rcu_head rcu;
	};
};

#define I915_BO_MIN_CHUNK_SIZE	SZ_64K

struct drm_i915_gem_object {
	struct drm_gem_object base;
	const struct drm_i915_gem_object_ops *ops;

	unsigned long *_nodes;
	unsigned long mempol;
	int maxnode;

	struct rb_root_cached segments;
	struct rb_node segment_node;
	unsigned long segment_offset;
	struct drm_i915_gem_object *parent;

	/* VM pointer if the object is private to a VM; NULL otherwise */
	struct i915_address_space *vm;
	struct list_head priv_obj_link;

	struct {
		/**
		 * @vma.lock: protect the list/tree of vmas
		 */
		spinlock_t lock;

		/**
		 * @vma.list: List of VMAs backed by this object
		 *
		 * The VMA on this list are ordered by type, all GGTT vma are
		 * placed at the head and all ppGTT vma are placed at the tail.
		 * The different types of GGTT vma are unordered between
		 * themselves, use the @vma.tree (which has a defined order
		 * between all VMA) to quickly find an exact match.
		 */
		struct list_head list;

		/**
		 * @vma.tree: Ordered tree of VMAs backed by this object
		 *
		 * All VMA created for this object are placed in the @vma.tree
		 * for fast retrieval via a binary search in
		 * i915_vma_instance(). They are also added to @vma.list for
		 * easy iteration.
		 */
		struct rb_root tree;
	} vma;

	/**
	 * @lut_list: List of vma lookup entries in use for this object.
	 *
	 * If this object is closed, we need to remove all of its VMA from
	 * the fast lookup index in associated contexts; @lut_list provides
	 * this translation from object to context->handles_vma.
	 */
	struct list_head lut_list;
	spinlock_t lut_lock; /* guards lut_list */

	/**
	 * @obj_link: Link into @i915_gem_ww_ctx.obj_list
	 *
	 * When we lock this object through i915_gem_object_lock() with a
	 * context, we add it to the list to ensure we can unlock everything
	 * when i915_gem_ww_ctx_backoff() or i915_gem_ww_ctx_fini() are called.
	 */
	struct list_head obj_link;
	struct i915_resv *shares_resv;

	union {
		struct rcu_head rcu;
		struct llist_node freed;
	};

	struct {
		spinlock_t lock; /* Protects access to mmo offsets */
		struct rb_root offsets;
	} mmo;

	I915_SELFTEST_DECLARE(struct list_head st_link);

	unsigned long flags;
#define I915_BO_ALLOC_CONTIGUOUS BIT(0)
#define I915_BO_ALLOC_VOLATILE   BIT(1)
#define I915_BO_ALLOC_USER       BIT(2)
#define I915_BO_ALLOC_IGNORE_MIN_PAGE_SIZE     BIT(3)
#define I915_BO_ALLOC_CHUNK_4K   BIT(4)
#define I915_BO_ALLOC_CHUNK_64K  BIT(5)
#define I915_BO_ALLOC_CHUNK_2M   BIT(6)
#define I915_BO_ALLOC_CHUNK_1G   BIT(7)
#define I915_BO_ALLOC_FLAGS (I915_BO_ALLOC_CONTIGUOUS | \
			     I915_BO_ALLOC_VOLATILE | \
			     I915_BO_ALLOC_USER | \
			     I915_BO_ALLOC_IGNORE_MIN_PAGE_SIZE | \
			     I915_BO_ALLOC_CHUNK_4K | \
			     I915_BO_ALLOC_CHUNK_64K | \
			     I915_BO_ALLOC_CHUNK_2M | \
			     I915_BO_ALLOC_CHUNK_1G)
#define I915_BO_READONLY	BIT(9)
#define I915_BO_HAS_BACKING_STORE	BIT(10)
#define I915_BO_PROTECTED	BIT(11)
#define I915_BO_SKIP_CLEAR	BIT(12)
#define I915_BO_CPU_CLEAR	BIT(13)
#define I915_BO_FAULT_CLEAR	BIT(14)
#define I915_BO_SYNC_HINT	BIT(15)
#define I915_BO_FABRIC		BIT(16)
#define I915_BO_MMAP_BIT	17
#define I915_BO_FAST_GUP_BIT	18

	/*
	 * Track whether the pages are coherent with the GPU if reading or
	 * writing through the CPU caches. The largely depends on the
	 * @cache_level setting.
	 *
	 * On platforms which don't have the shared LLC(HAS_SNOOP), like on Atom
	 * platforms, coherency must be explicitly requested with some special
	 * GTT caching bits(see enum i915_cache_level). When enabling coherency
	 * it does come at a performance and power cost on such platforms. On
	 * the flip side the kernel does not need to manually flush any buffers
	 * which need to be coherent with the GPU, if the object is not coherent
	 * i.e @cache_coherent is zero.
	 *
	 * On platforms that share the LLC with the CPU(HAS_LLC), all GT memory
	 * access will automatically snoop the CPU caches(even with CACHE_NONE).
	 * The one exception is when dealing with the display engine, like with
	 * scanout surfaces. To handle this the kernel will always flush the
	 * surface out of the CPU caches when preparing it for scanout.  Also
	 * note that since scanout surfaces are only ever read by the display
	 * engine we only need to care about flushing any writes through the CPU
	 * cache, reads on the other hand will always be coherent.
	 *
	 * Something strange here is why @cache_coherent is not a simple
	 * boolean, i.e coherent vs non-coherent. The reasoning for this is back
	 * to the display engine not being fully coherent. As a result scanout
	 * surfaces will either be marked as I915_CACHE_NONE or I915_CACHE_WT.
	 * In the case of seeing I915_CACHE_NONE the kernel makes the assumption
	 * that this is likely a scanout surface, and will set @cache_coherent
	 * as only I915_BO_CACHE_COHERENT_FOR_READ, on platforms with the shared
	 * LLC. The kernel uses this to always flush writes through the CPU
	 * cache as early as possible, where it can, in effect keeping
	 * @cache_dirty clean, so we can potentially avoid stalling when
	 * flushing the surface just before doing the scanout.  This does mean
	 * we might unnecessarily flush non-scanout objects in some places, but
	 * the default assumption is that all normal objects should be using
	 * I915_CACHE_LLC, at least on platforms with the shared LLC.
	 *
	 * Supported values:
	 *
	 * I915_BO_CACHE_COHERENT_FOR_READ:
	 *
	 * On shared LLC platforms, we use this for special scanout surfaces,
	 * where the display engine is not coherent with the CPU cache. As such
	 * we need to ensure we flush any writes before doing the scanout. As an
	 * optimisation we try to flush any writes as early as possible to avoid
	 * stalling later.
	 *
	 * Thus for scanout surfaces using I915_CACHE_NONE, on shared LLC
	 * platforms, we use:
	 *
	 *	cache_coherent = I915_BO_CACHE_COHERENT_FOR_READ
	 *
	 * While for normal objects that are fully coherent, including special
	 * scanout surfaces marked as I915_CACHE_WT, we use:
	 *
	 *	cache_coherent = I915_BO_CACHE_COHERENT_FOR_READ |
	 *			 I915_BO_CACHE_COHERENT_FOR_WRITE
	 *
	 * And then for objects that are not coherent at all we use:
	 *
	 *	cache_coherent = 0
	 *
	 * I915_BO_CACHE_COHERENT_FOR_WRITE:
	 *
	 * When writing through the CPU cache, the GPU is still coherent. Note
	 * that this also implies I915_BO_CACHE_COHERENT_FOR_READ.
	 */
#define I915_BO_CACHE_COHERENT_FOR_READ BIT(19)
#define I915_BO_CACHE_COHERENT_FOR_WRITE BIT(20)

	/**
	 * @pat_index: The desired PAT index.
	 *
	 * See hardware specification for valid PAT indices for each platform.
	 * This field used to contain a value of enum i915_cache_level. It's
	 * changed to an unsigned int because PAT indices are being used by
	 * both UMD and KMD for caching policy control after GEN12.
	 * For backward compatibility, this field will continue to contain
	 * value of i915_cache_level for pre-GEN12 platforms so that the PTE
	 * encode functions for these legacy platforms can stay the same.
	 * In the meantime platform specific tables are created to translate
	 * i915_cache_level into pat index, for more details check the macros
	 * defined i915/i915_pci.c, e.g. PVC_CACHELEVEL.
	 */
#define I915_BO_PAT_INDEX GENMASK(24, 21)

	unsigned long eviction;

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
	struct intel_frontbuffer __rcu *frontbuffer;
#endif

	struct {
		/*
		 * Protects the pages and their use. Do not use directly, but
		 * instead go through the pin/unpin interfaces.
		 */
		atomic_t pages_pin_count;

		struct intel_memory_region_link {
			/**
			 * Memory region for this object.
			 */
			struct intel_memory_region *mem;

			/**
			 * Element within memory_region->objects or region->purgeable
			 * if the object is marked as DONTNEED. Access is protected by
			 * region->obj_lock.
			 */
			struct list_head link;

			unsigned long age;
		} region;

		/**
		 * Priority list of potential placements for this object.
		 */
		struct intel_memory_region **placements;
		struct intel_memory_region *preferred_region;
		int n_placements;

		struct list_head blocks;

		struct scatterlist *pages;
		void *mapping;

		I915_SELFTEST_DECLARE(unsigned int page_mask);

		struct i915_gem_object_page_iter get_page;
		struct i915_gem_object_page_iter get_dma_page;

		/**
		 * Advice: are the backing pages purgeable, atomics enabled?
		 */
		unsigned int madv:2;
		unsigned int madv_atomic:2;
#define I915_BO_ATOMIC_NONE	0
#define I915_BO_ATOMIC_SYSTEM	1
#define I915_BO_ATOMIC_DEVICE	2

		/*
		 * Track the completion of the page construction if using the
		 * blitter for swapin/swapout and for clears. Following
		 * completion, it holds a persistent ERR_PTR should the
		 * GPU operation to instantiate the pages fail and all
		 * attempts to utilise the backing store must be prevented
		 * (as the backing store is in undefined state) until the
		 * taint is removed. All operations on the backing store
		 * must wait for the fence to be signaled, be it asynchronously
		 * as part of the scheduling pipeline or synchronously before
		 * CPU access.
		 */
		struct i915_active_fence migrate;

		u32 tlb[I915_MAX_GT];
	} mm;

	/*
	 * Record which PXP key instance this object was created against (if
	 * any), so we can use it to determine if the encryption is valid by
	 * comparing against the current key instance.
	 */
	u32 pxp_key_instance;

	union {
		struct i915_gem_userptr {
			uintptr_t ptr;
			struct mm_struct *mm;
		} userptr;

		struct drm_mm_node *stolen;

		struct sg_table *dmabuf_sgt;
		unsigned long scratch;
	};

	struct drm_i915_gem_object *swapto;

	/*
	 * To store the memory mask which represents the user preference about
	 * which memory region the object should reside in
	 */
	u32 memory_mask;

	struct {
		spinlock_t lock;

		/* list of clients which allocated/imported this object */
		struct rb_root rb;

		/* Whether this object currently resides in local memory */
		bool resident:1;
	} client;

	/*
	 * Implicity scaling uses two objects, allow them to be connected
	 */
	struct drm_i915_gem_object *pair;
};

static inline struct drm_i915_gem_object *
to_intel_bo(struct drm_gem_object *gem)
{
	/* Assert that to_intel_bo(NULL) == NULL */
	BUILD_BUG_ON(offsetof(struct drm_i915_gem_object, base));

	return container_of(gem, struct drm_i915_gem_object, base);
}

static inline const unsigned long *get_obj_nodes(const struct drm_i915_gem_object *obj)
{
	if (likely(obj->maxnode <= BITS_PER_TYPE(obj->_nodes)))
		return (unsigned long *)&obj->_nodes;
	else
		return obj->_nodes;
}

static inline unsigned int i915_gem_object_pat_index(const struct drm_i915_gem_object *obj)
{
	return FIELD_GET(I915_BO_PAT_INDEX, obj->flags);
}

#endif
