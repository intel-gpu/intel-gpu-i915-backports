/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2016 Intel Corporation
 */

#ifndef __I915_TIMELINE_TYPES_H__
#define __I915_TIMELINE_TYPES_H__

#include <linux/list.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/types.h>

#include "i915_active_types.h"

struct i915_vma;
struct i915_syncmap;
struct intel_gt;

/*
 * Every context has a flow of requests that we track using breadcrumbs
 * written by the individual requests that show their current status:
 * whether they have finished waiting for all other requests and have
 * started the user payload, or wheter that request has finished
 * the user payload and has signaled its completion. This sequence of
 * requests and their breadcrumbs forms the timeline.
 *
 * Each context is independent of any other context, and we wish to
 * easily reorder the execution of the contexts, so we want to
 * store the breadcrumb of each context in a separate location. The most
 * flexible approach is to allocate each timeline a slot in a common
 * page (that we reallocate upon demand), as we can then move the
 * timeline whenever we need (such as restarting the breadcrumb sequence
 * after a wrap). Sometimes we do not need the flexibilty to reallocate
 * upon demand, and can use a static slot, for which we can utilise
 * the ppHWSP inside logical ring contexts (gen8+). For perma-pinned
 * kernel contexts, we cannot reallocate a timeline / status page
 * on demand, and so must always use a static slot. Here, we use
 * the per-engine global HWSP available on all generations.
 */
enum intel_timeline_mode {
	INTEL_TIMELINE_ABSOLUTE = 0, /* stored in a common page */
	INTEL_TIMELINE_RELATIVE_CONTEXT = BIT(0), /* stored in ppHWSP */
	INTEL_TIMELINE_RELATIVE_ENGINE  = BIT(1), /* stored in the HWSP */
};

struct intel_timeline {
	u64 fence_context;
	u32 seqno;

	struct mutex mutex; /* protects the flow of requests */

	/*
	 * pin_count and active_count track essentially the same thing:
	 * How many requests are in flight or may be under construction.
	 *
	 * We need two distinct counters so that we can assign different
	 * lifetimes to the events for different use-cases. For example,
	 * we want to permanently keep the timeline pinned for the kernel
	 * context so that we can issue requests at any time without having
	 * to acquire space in the GGTT. However, we want to keep tracking
	 * the activity (to be able to detect when we become idle) along that
	 * permanently pinned timeline and so end up requiring two counters.
	 *
	 * Note that the active_count is protected by the intel_timeline.mutex,
	 * but the pin_count is protected by a combination of serialisation
	 * from the intel_context caller plus internal atomicity.
	 */
	atomic_t pin_count;
	atomic_t active_count;

	enum intel_timeline_mode mode;

	void *hwsp_map;
	const u32 *hwsp_seqno;
	struct i915_vma *hwsp_ggtt;
	u32 hwsp_offset;

	/**
	 * List of breadcrumbs associated with GPU requests currently
	 * outstanding.
	 */
	struct list_head requests;

	/*
	 * Contains an RCU guarded pointer to the last request. No reference is
	 * held to the request, users must carefully acquire a reference to
	 * the request using i915_active_fence_get(), or manage the RCU
	 * protection themselves (cf the i915_active_fence API).
	 */
	struct i915_active_fence last_request;

	struct i915_active active;

	/** A chain of completed timelines ready for early retirement. */
	struct intel_timeline *retire;

	/**
	 * We track the most recent seqno that we wait on in every context so
	 * that we only have to emit a new await and dependency on a more
	 * recent sync point. As the contexts may be executed out-of-order, we
	 * have to track each individually and can not rely on an absolute
	 * global_seqno. When we know that all tracked fences are completed
	 * (i.e. when the driver is idle), we know that the syncmap is
	 * redundant and we can discard it without loss of generality.
	 */
	struct i915_syncmap *sync;

	struct list_head link;
	struct intel_gt *gt;

	struct list_head engine_link;

	struct kref kref;
	struct rcu_head rcu;
};

#endif /* __I915_TIMELINE_TYPES_H__ */
