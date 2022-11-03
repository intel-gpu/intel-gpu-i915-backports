/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_GT_TYPES__
#define __INTEL_GT_TYPES__

#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/llist.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/seqlock.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include <drm/drm_mm.h>

#include "iov/intel_iov_types.h"
#include "uc/intel_uc.h"
#include "intel_gsc.h"

#include "i915_perf_types.h"
#include "intel_engine_types.h"
#include "intel_flat_ppgtt_pool_types.h"
#include "intel_gt_buffer_pool_types.h"
#include "intel_hwconfig.h"
#include "intel_llc_types.h"
#include "intel_memory_region.h"
#include "intel_reset_types.h"
#include "intel_rc6_types.h"
#include "intel_rps_types.h"
#include "intel_migrate_types.h"
#include "intel_wakeref.h"
#include "pxp/intel_pxp_types.h"
#include "intel_wopcm.h"

#include "intel_gt_defines.h"

struct drm_i915_private;
struct i915_ggtt;
struct i915_vma;
struct intel_engine_cs;
struct intel_uncore;

/* Count of GT Correctable and FATAL HW ERRORS */
enum intel_gt_hw_errors {
	INTEL_GT_HW_ERROR_COR_L3_SNG = 0,
	INTEL_GT_HW_ERROR_COR_GUC,
	INTEL_GT_HW_ERROR_COR_SAMPLER,
	INTEL_GT_HW_ERROR_COR_SLM,
	INTEL_GT_HW_ERROR_COR_EU_IC,
	INTEL_GT_HW_ERROR_COR_EU_GRF,
	INTEL_GT_HW_ERROR_FAT_ARR_BIST,
	INTEL_GT_HW_ERROR_FAT_FPU,
	INTEL_GT_HW_ERROR_FAT_L3_DOUB,
	INTEL_GT_HW_ERROR_FAT_L3_ECC_CHK,
	INTEL_GT_HW_ERROR_FAT_GUC,
	INTEL_GT_HW_ERROR_FAT_IDI_PAR,
	INTEL_GT_HW_ERROR_FAT_SQIDI,
	INTEL_GT_HW_ERROR_FAT_SAMPLER,
	INTEL_GT_HW_ERROR_FAT_SLM,
	INTEL_GT_HW_ERROR_FAT_EU_IC,
	INTEL_GT_HW_ERROR_FAT_EU_GRF,
	INTEL_GT_HW_ERROR_FAT_TLB,
	INTEL_GT_HW_ERROR_FAT_L3_FABRIC,
	INTEL_GT_HW_ERROR_COUNT
};

enum intel_soc_num_ieh {
	INTEL_GT_SOC_IEH0 = 0,
	INTEL_GT_SOC_IEH1,
	INTEL_GT_SOC_NUM_IEH
};

enum intel_soc_ieh_reg_type {
	INTEL_SOC_REG_LOCAL = 0,
	INTEL_SOC_REG_GLOBAL
};

enum intel_gt_driver_errors {
	INTEL_GT_DRIVER_ERROR_GGTT = 0,
	INTEL_GT_DRIVER_ERROR_ENGINE_OTHER,
	INTEL_GT_DRIVER_ERROR_GUC_COMMUNICATION,
	INTEL_GT_DRIVER_ERROR_RPS,
	INTEL_GT_DRIVER_ERROR_GT_OTHER,
	INTEL_GT_DRIVER_ERROR_INTERRUPT,
	INTEL_GT_DRIVER_ERROR_COUNT
};

struct intel_mmio_range {
	u32 start;
	u32 end;
};

/*
 * The hardware has multiple kinds of multicast register ranges that need
 * special register steering (and future platforms are expected to add
 * additional types).
 *
 * During driver startup, we initialize the steering control register to
 * direct reads to a slice/subslice that are valid for the 'subslice' class
 * of multicast registers.  If another type of steering does not have any
 * overlap in valid steering targets with 'subslice' style registers, we will
 * need to explicitly re-steer reads of registers of the other type.
 *
 * Only the replication types that may need additional non-default steering
 * are listed here.
 */
enum intel_steering_type {
	L3BANK,
	MSLICE,
	LNCF,

	/*
	 * On some platforms there are multiple types of MCR registers that
	 * will always return a non-terminated value at instance (0, 0).  We'll
	 * lump those all into a single category to keep things simple.
	 */
	INSTANCE0,

	NUM_STEERING_TYPES
};

enum intel_submission_method {
	INTEL_SUBMISSION_RING,
	INTEL_SUBMISSION_ELSP,
	INTEL_SUBMISSION_GUC,
};

struct intel_mem_sparing_event {
	struct work_struct mem_health_work;
	u32    cause;
	enum {
		MEM_HEALTH_OKAY = 0,
		MEM_HEALTH_ALARM,
		MEM_HEALTH_EC_PENDING,
		MEM_HEALTH_DEGRADED,
		MEM_HEALTH_UNKNOWN
	} health_status;
};

struct intel_rps_defaults {
	u32 min_freq;
	u32 max_freq;
	u32 boost_freq;
	u32 media_ratio_mode;
	u32 base_freq_factor;
};

enum intel_gt_type {
	GT_PRIMARY,
	GT_TILE,
	GT_MEDIA,
};

struct intel_gt {
	struct drm_i915_private *i915;
	const char *name;
	enum intel_gt_type type;

	struct intel_uncore *uncore;
	struct i915_ggtt *ggtt;

	struct intel_uc uc;
	struct intel_gsc gsc;
	struct intel_wopcm wopcm;
	struct intel_iov iov;
	enum intel_engine_id rsvd_bcs;

	struct {
		/* Serialize global tlb invalidations */
		struct mutex invalidate_lock;

		/*
		 * Batch TLB invalidations
		 *
		 * After unbinding the PTE, we need to ensure the TLB
		 * are invalidated prior to releasing the physical pages.
		 * But we only need one such invalidation for all unbinds,
		 * so we track how many TLB invalidations have been
		 * performed since unbind the PTE and only emit an extra
		 * invalidate if no full barrier has been passed.
		 */
		seqcount_mutex_t seqno;
	} tlb;

	struct i915_wa_list wa_list;

	struct intel_gt_timelines {
		spinlock_t lock; /* protects active_list */
		struct list_head active_list;
	} timelines;

	struct intel_gt_requests {
		/**
		 * We leave the user IRQ off as much as possible,
		 * but this means that requests will finish and never
		 * be retired once the system goes idle. Set a timer to
		 * fire periodically while the ring is running. When it
		 * fires, go retire requests.
		 */
		struct delayed_work retire_work;
	} requests;

	struct {
		struct llist_head list;
		struct work_struct work;
	} watchdog;

	struct {
		bool enabled;
		struct hrtimer timer;
		atomic_t boost;
		u32 delay;
		u32 delay_fast, delay_slow;
		bool int_enabled;
	} fake_int;

	/* Maintain a per-gt pool */
	struct intel_flat_ppgtt_pool fpp;

	struct intel_wakeref wakeref;
	atomic_t user_wakeref;

	struct list_head closed_vma;
	spinlock_t closed_lock; /* guards the list of closed_vma */

	ktime_t last_init_time;
	struct intel_reset reset;

	/**
	 * Is the GPU currently considered idle, or busy executing
	 * userspace requests? Whilst idle, we allow runtime power
	 * management to power down the hardware and display clocks.
	 * In order to reduce the effect on performance, there
	 * is a slight delay before we do so.
	 */
	intel_wakeref_t awake;

	u32 clock_frequency;
	u32 clock_period_ns;

#define GEN12_ENGINE_SEMAPHORE_TOKEN_MAX       27
#define XEHPSDV_ENGINE_SEMAPHORE_TOKEN_MAX         256
	/*
	 * Used for gen12+ semaphore tokens.
	 * This value is used to initialize our contexts, and is
	 * free to overflow.
	 */
	atomic_t next_token;

	struct intel_llc llc;
	struct intel_rc6 rc6;
	struct intel_rps rps;

	struct i915_vma *dbg;

	spinlock_t *irq_lock;
	u32 gt_imr;
	u32 pm_ier;
	u32 pm_imr;

	u32 pm_guc_events;

	struct {
		/**
		 * @total: Total time this engine was busy.
		 *
		 * Accumulated time not counting the most recent block in cases
		 * where engine is currently busy (active > 0).
		 */
		ktime_t total;

		/**
		 * @start: Timestamp of the last idle to active transition.
		 *
		 * Idle is defined as active == 0, active is active > 0.
		 */
		ktime_t start;
	} stats;

	struct intel_engine_cs *engine[I915_NUM_ENGINES];
	struct intel_engine_cs *engine_class[MAX_ENGINE_CLASS + 1]
					    [MAX_ENGINE_INSTANCE + 1];
	struct {
		/* Serialize CCS mode access */
		struct mutex mutex;
		/* Active CCS engines */
		intel_engine_mask_t active;
		/* CCS context -> C-slice */
		intel_engine_mask_t config;
	} ccs;

	enum intel_submission_method submission_method;

	/*
	 * Default address space (either GGTT or ppGTT depending on arch).
	 *
	 * Reserved for exclusive use by the kernel.
	 */
	struct i915_address_space *vm;
	struct drm_mm_node flat; /* 1:1 mapping of lmem reserved in vm */

	/*
	 * A pool of objects to use as shadow copies of client batch buffers
	 * when the command parser is enabled. Prevents the client from
	 * modifying the batch contents after software parsing.
	 *
	 * Buffers older than 1s are periodically reaped from the pool,
	 * or may be reclaimed by the shrinker before then.
	 */
	struct intel_gt_buffer_pool buffer_pool;

	struct i915_vma *scratch;

	const struct intel_mmio_range *steering_table[NUM_STEERING_TYPES];
	struct intel_migrate migrate;

	struct {
		u8 groupid;
		u8 instanceid;
	} default_steering;

	/*
	 * Base of per-tile GTTMMADR where we can derive the MMIO and the GGTT.
	 */
	phys_addr_t phys_addr;

	struct intel_memory_region *lmem;

	int iaf_irq;

	struct intel_hw_errors {
		unsigned long hw[INTEL_GT_HW_ERROR_COUNT];
		struct xarray soc;
		unsigned long sgunit[HARDWARE_ERROR_MAX];
		unsigned long driver[INTEL_GT_DRIVER_ERROR_COUNT];
	} errors;

	struct intel_gt_info {
		unsigned int id;

		intel_engine_mask_t engine_mask;

		u8 num_engines;

		/* General presence of SFC units */
		u8 sfc_mask;

		/* Media engine access to SFC per instance */
		u8 vdbox_sfc_access;

		/* Slice/subslice/EU info */
		struct sseu_dev_info sseu;

		union {
			unsigned long mslice_mask;
			unsigned long l3bank_mask;
		};

		/** @hwconfig: hardware configuration data */
		struct intel_hwconfig hwconfig;
	} info;

	struct {
		u8 uc_index;
		u8 wb_index; /* Only used on HAS_L3_CCS_READ() platforms */
	} mocs;

	struct intel_pxp pxp;

	/* sysfs defaults per gt */
	struct intel_rps_defaults rps_defaults;
	struct kobject *sysfs_defaults;

	/* Memory sparing data structure for errors reporting on root tile */
	struct intel_mem_sparing_event mem_sparing;

	struct i915_perf_gt perf;

	struct i915_eu_stall_cntr_gt eu_stall_cntr;

	/** link: &ggtt.gt_list */
	struct list_head ggtt_link;
};

struct intel_gt_definition {
	enum intel_gt_type type;
	char *name;
	u32 mapping_base;
	u32 gsi_offset;
	intel_engine_mask_t engine_mask;
};

enum intel_gt_scratch_field {
	/* 8 bytes */
	INTEL_GT_SCRATCH_FIELD_DEFAULT = 0,

	/* 8 bytes */
	INTEL_GT_SCRATCH_FIELD_RENDER_FLUSH = 128,

	/* 8 bytes */
	INTEL_GT_SCRATCH_FIELD_COHERENTL3_WA = 256,
};

#define SOC_HW_ERR_SHIFT	ilog2(SOC_HW_ERR_MAX_BITS)
#define SOC_ERR_BIT		BIT(IEH_SHIFT + 1)
#define IEH_SHIFT		(REG_GROUP_SHIFT + REG_GROUP_BITS)
#define IEH_MASK		(0x1)
#define REG_GROUP_SHIFT		(HW_ERR_TYPE_BITS + SOC_HW_ERR_SHIFT)
#define REG_GROUP_BITS		(1)
#define HW_ERR_TYPE_BITS	(2)
#define SOC_ERR_INDEX(IEH, REG_GROUP, HW_ERR, ERRBIT) \
	(SOC_ERR_BIT | \
	 (IEH) << IEH_SHIFT | \
	 (REG_GROUP) << REG_GROUP_SHIFT | \
	 (HW_ERR) << SOC_HW_ERR_SHIFT | \
	 (ERRBIT))

__printf(3, 4)
void intel_gt_log_driver_error(struct intel_gt *gt,
			       const enum intel_gt_driver_errors error,
			       const char *fmt, ...);

#endif /* __INTEL_GT_TYPES_H__ */
