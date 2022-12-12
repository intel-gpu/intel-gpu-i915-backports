/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_CONTEXT_H__
#define __INTEL_CONTEXT_H__

#include <linux/bitops.h>
#include <linux/lockdep.h>
#include <linux/types.h>

#include "i915_active.h"
#include "i915_drv.h"
#include "i915_suspend_fence.h"
#include "intel_context_types.h"
#include "intel_engine_types.h"
#include "intel_gt_pm.h"
#include "intel_ring_types.h"
#include "intel_timeline_types.h"
#include "i915_trace.h"

#define CE_TRACE(ce, fmt, ...) do {					\
	const struct intel_context *ce__ = (ce);			\
	ENGINE_TRACE(ce__->engine, "context:%llx " fmt,			\
		     ce__->timeline->fence_context,			\
		     ##__VA_ARGS__);					\
} while (0)

struct i915_address_space;
struct i915_gem_ww_ctx;

void intel_context_update_schedule_policy(struct intel_context *ce);
void intel_context_init_schedule_policy(struct intel_context *ce);
void intel_context_reset_preemption_timeout(struct intel_context *ce);
void intel_context_disable_preemption_timeout(struct intel_context *ce);

void intel_context_init(struct intel_context *ce,
			struct intel_engine_cs *engine);
void intel_context_fini(struct intel_context *ce);

void i915_context_module_exit(void);
int i915_context_module_init(void);

struct intel_context *
intel_context_create(struct intel_engine_cs *engine);

int intel_context_alloc_state(struct intel_context *ce);

void intel_context_free(struct intel_context *ce);

int intel_context_reconfigure_sseu(struct intel_context *ce,
				   const struct intel_sseu sseu);
int intel_context_reconfigure_vm(struct intel_context *ce,
				 struct i915_address_space *vm);

#define PARENT_SCRATCH_SIZE	PAGE_SIZE

static inline bool intel_context_is_child(struct intel_context *ce)
{
	return !!ce->parallel.parent;
}

static inline bool intel_context_is_parent(struct intel_context *ce)
{
	return !!ce->parallel.number_children;
}

static inline bool intel_context_is_pinned(struct intel_context *ce);

static inline struct intel_context *
intel_context_to_parent(struct intel_context *ce)
{
	if (intel_context_is_child(ce)) {
		/*
		 * The parent holds ref count to the child so it is always safe
		 * for the parent to access the child, but the child has a
		 * pointer to the parent without a ref. To ensure this is safe
		 * the child should only access the parent pointer while the
		 * parent is pinned.
		 */
		GEM_BUG_ON(!intel_context_is_pinned(ce->parallel.parent));

		return ce->parallel.parent;
	} else {
		return ce;
	}
}

static inline bool intel_context_is_parallel(struct intel_context *ce)
{
	return intel_context_is_child(ce) || intel_context_is_parent(ce);
}

void intel_context_bind_parent_child(struct intel_context *parent,
				     struct intel_context *child);

#define for_each_child(parent, ce)\
	list_for_each_entry(ce, &(parent)->parallel.child_list,\
			    parallel.child_link)
#define for_each_child_safe(parent, ce, cn)\
	list_for_each_entry_safe(ce, cn, &(parent)->parallel.child_list,\
				 parallel.child_link)

/**
 * intel_context_lock_pinned - Stablises the 'pinned' status of the HW context
 * @ce - the context
 *
 * Acquire a lock on the pinned status of the HW context, such that the context
 * can neither be bound to the GPU or unbound whilst the lock is held, i.e.
 * intel_context_is_pinned() remains stable.
 */
static inline int intel_context_lock_pinned(struct intel_context *ce)
	__acquires(ce->pin_mutex)
{
	return mutex_lock_interruptible(&ce->pin_mutex);
}

/**
 * intel_context_is_pinned - Reports the 'pinned' status
 * @ce - the context
 *
 * While in use by the GPU, the context, along with its ring and page
 * tables is pinned into memory and the GTT.
 *
 * Returns: true if the context is currently pinned for use by the GPU.
 */
static inline bool
intel_context_is_pinned(struct intel_context *ce)
{
	return atomic_read(&ce->pin_count);
}

static inline void intel_context_cancel_request(struct intel_context *ce,
						struct i915_request *rq)
{
	GEM_BUG_ON(!ce->ops->cancel_request);
	return ce->ops->cancel_request(ce, rq);
}

/**
 * intel_context_unlock_pinned - Releases the earlier locking of 'pinned' status
 * @ce - the context
 *
 * Releases the lock earlier acquired by intel_context_unlock_pinned().
 */
static inline void intel_context_unlock_pinned(struct intel_context *ce)
	__releases(ce->pin_mutex)
{
	mutex_unlock(&ce->pin_mutex);
}

int __intel_context_do_pin(struct intel_context *ce);
int __intel_context_do_pin_ww(struct intel_context *ce,
			      struct i915_gem_ww_ctx *ww);

static inline bool intel_context_pin_if_active(struct intel_context *ce)
{
	return atomic_inc_not_zero(&ce->pin_count);
}

static inline int intel_context_pin(struct intel_context *ce)
{
	if (likely(intel_context_pin_if_active(ce)))
		return 0;

	return __intel_context_do_pin(ce);
}

static inline int intel_context_pin_ww(struct intel_context *ce,
				       struct i915_gem_ww_ctx *ww)
{
	if (likely(intel_context_pin_if_active(ce)))
		return 0;

	return __intel_context_do_pin_ww(ce, ww);
}

static inline void __intel_context_pin(struct intel_context *ce)
{
	GEM_BUG_ON(!intel_context_is_pinned(ce));
	atomic_inc(&ce->pin_count);
}

void __intel_context_do_unpin(struct intel_context *ce, int sub);

static inline void intel_context_sched_disable_unpin(struct intel_context *ce)
{
	__intel_context_do_unpin(ce, 2);
}

static inline void intel_context_unpin(struct intel_context *ce)
{
	if (!ce->ops->sched_disable) {
		__intel_context_do_unpin(ce, 1);
	} else {
		/*
		 * Move ownership of this pin to the scheduling disable which is
		 * an async operation. When that operation completes the above
		 * intel_context_sched_disable_unpin is called potentially
		 * unpinning the context.
		 */
		while (!atomic_add_unless(&ce->pin_count, -1, 1)) {
			if (atomic_cmpxchg(&ce->pin_count, 1, 2) == 1) {
				ce->ops->sched_disable(ce);
				break;
			}
		}
	}
}

void intel_context_enter_engine(struct intel_context *ce);
void intel_context_exit_engine(struct intel_context *ce);

static inline void intel_context_enter(struct intel_context *ce)
{
	lockdep_assert_held(&ce->timeline->mutex);
	if (ce->active_count++)
		return;

	ce->ops->enter(ce);
	ce->wakeref = intel_gt_pm_get(ce->vm->gt);
}

static inline void intel_context_mark_active(struct intel_context *ce)
{
	lockdep_assert_held(&ce->timeline->mutex);
	++ce->active_count;
}

static inline void intel_context_exit(struct intel_context *ce)
{
	lockdep_assert_held(&ce->timeline->mutex);
	GEM_BUG_ON(!ce->active_count);
	if (--ce->active_count)
		return;

	intel_gt_pm_put_async(ce->vm->gt, ce->wakeref);
	ce->ops->exit(ce);
}

static inline bool intel_context_is_active(const struct intel_context *ce)
{
	return !i915_active_is_idle(&ce->active);
}

static inline void intel_context_suspend_fence_set(struct intel_context *ce,
						   struct dma_fence *fence)
{
	struct i915_suspend_fence *sfence =
		container_of(fence, typeof(*sfence), base.dma);

	lockdep_assert_held(&ce->timeline->mutex);

	GEM_BUG_ON(ce->sfence);
	dma_fence_get(fence);
	ce->sfence = sfence;
}

static inline void
intel_context_suspend_fence_replace(struct intel_context *ce,
				    struct dma_fence *fence)
{
	struct dma_fence *prev;
	struct i915_suspend_fence *sfence =
		container_of(fence, typeof(*sfence), base.dma);

	lockdep_assert_held(&ce->timeline->mutex);
	GEM_BUG_ON(!ce->sfence);

	prev = &ce->sfence->base.dma;
	dma_fence_get(fence);
	ce->sfence = sfence;
	dma_fence_put(prev);
}

static inline struct intel_context *intel_context_get(struct intel_context *ce)
{
	kref_get(&ce->ref);
	return ce;
}

static inline void intel_context_put(struct intel_context *ce)
{
	kref_put(&ce->ref, ce->ops->destroy);
}

static inline struct intel_timeline *__must_check
intel_context_timeline_lock(struct intel_context *ce)
	__acquires(&ce->timeline->mutex)
{
	struct intel_timeline *tl = ce->timeline;
	int err;

	err = mutex_lock_interruptible(&tl->mutex);
	if (err)
		return ERR_PTR(err);

	return tl;
}

static inline void intel_context_timeline_unlock(struct intel_timeline *tl)
	__releases(&tl->mutex)
{
	mutex_unlock(&tl->mutex);
}

int intel_context_prepare_remote_request(struct intel_context *ce,
					 struct i915_request *rq);

struct i915_request *intel_context_create_request(struct intel_context *ce);

struct i915_request *
__intel_context_find_active_request(struct intel_context *ce,
				    bool rq_get_ref);

static inline struct i915_request *
intel_context_find_active_request(struct intel_context *ce)
{
	return __intel_context_find_active_request(ce, false);
}

static inline struct i915_request *
intel_context_get_active_request(struct intel_context *ce)
{
	return __intel_context_find_active_request(ce, true);
}

static inline bool intel_context_has_error(const struct intel_context *ce)
{
	return test_bit(CONTEXT_ERROR, &ce->flags);
}

static inline void intel_context_set_error(struct intel_context *ce)
{
	set_bit(CONTEXT_ERROR, &ce->flags);
}

static inline bool intel_context_is_barrier(const struct intel_context *ce)
{
	return test_bit(CONTEXT_BARRIER_BIT, &ce->flags);
}

static inline void intel_context_close(struct intel_context *ce)
{
	set_bit(CONTEXT_CLOSED_BIT, &ce->flags);

	if (ce->ops->close)
		ce->ops->close(ce);
}

static inline bool intel_context_is_closed(const struct intel_context *ce)
{
	return test_bit(CONTEXT_CLOSED_BIT, &ce->flags);
}

static inline bool intel_context_has_inflight(const struct intel_context *ce)
{
	return test_bit(COPS_HAS_INFLIGHT_BIT, &ce->ops->flags);
}

static inline bool intel_context_use_semaphores(const struct intel_context *ce)
{
	return test_bit(CONTEXT_USE_SEMAPHORES, &ce->flags);
}

static inline void intel_context_set_use_semaphores(struct intel_context *ce)
{
	set_bit(CONTEXT_USE_SEMAPHORES, &ce->flags);
}

static inline void intel_context_clear_use_semaphores(struct intel_context *ce)
{
	clear_bit(CONTEXT_USE_SEMAPHORES, &ce->flags);
}

static inline bool intel_context_is_banned(const struct intel_context *ce)
{
	return test_bit(CONTEXT_BANNED, &ce->flags);
}

static inline bool intel_context_set_banned(struct intel_context *ce)
{
	return test_and_set_bit(CONTEXT_BANNED, &ce->flags);
}

static inline bool intel_context_ban(struct intel_context *ce,
				     struct i915_request *rq)
{
	bool ret = intel_context_set_banned(ce);

	trace_intel_context_ban(ce);
	if (ce->ops->ban)
		ce->ops->ban(ce, rq);

	return ret;
}

/**
 * intel_context_suspend - suspend a context
 * @ce: The context to suspend
 * @atomic: Perform the suspend without sleeping.
 *
 * Return: A pointer to a struct i915_sw_fence that, when signaled,
 * indicates that the suspension is complete. If the function is called
 * with @atomic == true, and the suspend can't be performed
 * without sleeping, return ERR_PTR(-EBUSY);
 *
 * The function may be called from reclaim.
 *
 * It is safe to recursively suspend the context multiple times.
 * In that case a corresponding number of calls to
 * intel_context_resume is needed to resume it.
 *
 * The returned i915_sw_fence is guaranteed to be valid until
 * a paired i915_context_resume is called. In addition the
 * paired i915_context_resume may not be called unless the
 * returned fence is complete.
 */
static inline struct i915_sw_fence *
intel_context_suspend(struct intel_context *ce, bool atomic)
{
	GEM_BUG_ON(!ce->ops->suspend);
	return ce->ops->suspend(ce, atomic);
}

/**
 * intel_context_resume - Resume a context
 * @ce: The context to resume
 *
 * Resume the context previously suspended using intel_context_suspend().
 * The i915_sw_fence returned from intel_context_suspend() must be
 * complete.
 */
static inline void intel_context_resume(struct intel_context *ce)
{
	GEM_BUG_ON(!ce->ops->resume);
	ce->ops->resume(ce);
}

static inline bool
intel_context_force_single_submission(const struct intel_context *ce)
{
	return test_bit(CONTEXT_FORCE_SINGLE_SUBMISSION, &ce->flags);
}

static inline void
intel_context_set_single_submission(struct intel_context *ce)
{
	__set_bit(CONTEXT_FORCE_SINGLE_SUBMISSION, &ce->flags);
}

static inline bool
intel_context_nopreempt(const struct intel_context *ce)
{
	return test_bit(CONTEXT_NOPREEMPT, &ce->flags);
}

static inline void
intel_context_set_nopreempt(struct intel_context *ce)
{
	set_bit(CONTEXT_NOPREEMPT, &ce->flags);
}

static inline void
intel_context_clear_nopreempt(struct intel_context *ce)
{
	clear_bit(CONTEXT_NOPREEMPT, &ce->flags);
}

static inline bool
intel_context_debug(const struct intel_context *ce)
{
	return test_bit(CONTEXT_DEBUG, &ce->flags);
}

static inline void
intel_context_set_debug(struct intel_context *ce)
{
	set_bit(CONTEXT_DEBUG, &ce->flags);
}

static inline void
intel_context_clear_debug(struct intel_context *ce)
{
	clear_bit(CONTEXT_DEBUG, &ce->flags);
}

u64 intel_context_get_total_runtime_ns(const struct intel_context *ce);
u64 intel_context_get_avg_runtime_ns(struct intel_context *ce);

static inline u64 intel_context_clock(void)
{
	/* As we mix CS cycles with CPU clocks, use the raw monotonic clock. */
	return ktime_get_raw_fast_ns();
}

#endif /* __INTEL_CONTEXT_H__ */
