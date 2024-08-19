/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2020 Intel Corporation
 */

#ifndef __INTEL_MIGRATE_TYPES__
#define __INTEL_MIGRATE_TYPES__

#include <drm/drm_mm.h>

struct drm_i915_gem_object;
struct intel_context;

enum {
	CLEAR_ALLOC = 0,
	CLEAR_FREE,
	CLEAR_IDLE,
	__N_CLEAR,
};

struct intel_migrate {
	union {
		struct intel_context *context;
		struct intel_context *clear[__N_CLEAR];
	};

	struct intel_migrate_window {
		struct intel_context *context;

		struct intel_migrate_node {
			struct drm_i915_gem_object *obj;
			struct drm_mm_node node;
			u64 pd_offset;
		} ps64, pde64, ps2M;

		unsigned long clear_chunk;
		unsigned long swap_chunk;
	} swapin[2], swapout[4], smem;
	atomic_t next_swapin, next_swapout;
};

#endif /* __INTEL_MIGRATE_TYPES__ */
