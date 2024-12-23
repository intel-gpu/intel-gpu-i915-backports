// SPDX-License-Identifier: MIT
/*
 * Copyright © 2014 Intel Corporation
 */

#include <linux/circ_buf.h>

#include "gem/i915_gem_context.h"
#include "gt/gen8_engine_cs.h"
#include "gt/intel_breadcrumbs.h"
#include "gt/intel_context.h"
#include "gt/intel_engine_heartbeat.h"
#include "gt/intel_engine_pm.h"
#include "gt/intel_engine_regs.h"
#include "gt/intel_gpu_commands.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_clock_utils.h"
#include "gt/intel_gt_irq.h"
#include "gt/intel_gt_pm.h"
#include "gt/intel_gt_regs.h"
#include "gt/intel_gt_requests.h"
#include "gt/intel_lrc.h"
#include "gt/intel_lrc_reg.h"
#include "gt/intel_mocs.h"
#include "gt/intel_ring.h"

#include "intel_guc_ads.h"
#include "intel_guc_capture.h"
#include "intel_guc_print.h"
#include "intel_guc_submission.h"

#include "i915_drv.h"
#include "i915_trace.h"

/**
 * DOC: GuC-based command submission
 *
 * The Scratch registers:
 * There are 16 MMIO-based registers start from 0xC180. The kernel driver writes
 * a value to the action register (SOFT_SCRATCH_0) along with any data. It then
 * triggers an interrupt on the GuC via another register write (0xC4C8).
 * Firmware writes a success/fail code back to the action register after
 * processes the request. The kernel driver polls waiting for this update and
 * then proceeds.
 *
 * Command Transport buffers (CTBs):
 * Covered in detail in other sections but CTBs (Host to GuC - H2G, GuC to Host
 * - G2H) are a message interface between the i915 and GuC.
 *
 * Context registration:
 * Before a context can be submitted it must be registered with the GuC via a
 * H2G. A unique guc_id is associated with each context. The context is either
 * registered at request creation time (normal operation) or at submission time
 * (abnormal operation, e.g. after a reset).
 *
 * Context submission:
 * The i915 updates the LRC tail value in memory. The i915 must enable the
 * scheduling of the context within the GuC for the GuC to actually consider it.
 * Therefore, the first time a disabled context is submitted we use a schedule
 * enable H2G, while follow up submissions are done via the context submit H2G,
 * which informs the GuC that a previously enabled context has new work
 * available.
 *
 * Context unpin:
 * To unpin a context a H2G is used to disable scheduling. When the
 * corresponding G2H returns indicating the scheduling disable operation has
 * completed it is safe to unpin the context. While a disable is in flight it
 * isn't safe to resubmit the context so a fence is used to stall all future
 * requests of that context until the G2H is returned. Because this interaction
 * with the GuC takes a non-zero amount of time we delay the disabling of
 * scheduling after the pin count goes to zero by a configurable period of time
 * (see SCHED_DISABLE_DELAY_MS). The thought is this gives the user a window of
 * time to resubmit something on the context before doing this costly operation.
 * This delay is only done if the context isn't closed and the guc_id usage is
 * less than a threshold (see NUM_SCHED_DISABLE_GUC_IDS_THRESHOLD).
 *
 * Context deregistration:
 * Before a context can be destroyed or if we steal its guc_id we must
 * deregister the context with the GuC via H2G. If stealing the guc_id it isn't
 * safe to submit anything to this guc_id until the deregister completes so a
 * fence is used to stall all requests associated with this guc_id until the
 * corresponding G2H returns indicating the guc_id has been deregistered.
 *
 * submission_state.guc_ids:
 * Unique number associated with private GuC context data passed in during
 * context registration / submission / deregistration. 64k available. Simple ida
 * is used for allocation.
 *
 * Stealing guc_ids:
 * If no guc_ids are available they can be stolen from another context at
 * request creation time if that context is unpinned. If a guc_id can't be found
 * we punt this problem to the user as we believe this is near impossible to hit
 * during normal use cases.
 *
 * Locking:
 * In the GuC submission code we have 3 basic spin locks which protect
 * everything. Details about each below.
 *
 * sched_engine->lock
 * This is the submission lock for all contexts that share an i915 schedule
 * engine (sched_engine), thus only one of the contexts which share a
 * sched_engine can be submitting at a time. Currently only one sched_engine is
 * used for all of GuC submission but that could change in the future.
 *
 * guc->submission_state.lock
 * Global lock for GuC submission state. Protects guc_ids and destroyed contexts
 * list.
 *
 * ce->guc_state.lock
 * Protects everything under ce->guc_state. Ensures that a context is in the
 * correct state before issuing a H2G. e.g. We don't issue a schedule disable
 * on a disabled context (bad idea), we don't issue a schedule enable when a
 * schedule disable is in flight, etc... Also protects list of inflight requests
 * on the context and the priority management state. Lock is individual to each
 * context.
 *
 * Lock ordering rules:
 * sched_engine->lock -> ce->guc_state.lock
 * guc->submission_state.lock -> ce->guc_state.lock
 *
 * Reset races:
 * When a full GT reset is triggered it is assumed that some G2H responses to
 * H2Gs can be lost as the GuC is also reset. Losing these G2H can prove to be
 * fatal as we do certain operations upon receiving a G2H (e.g. destroy
 * contexts, release guc_ids, etc...). When this occurs we can scrub the
 * context state and cleanup appropriately, however this is quite racey.
 * To avoid races, the reset code must disable submission before scrubbing for
 * the missing G2H, while the submission code must check for submission being
 * disabled and skip sending H2Gs and updating context states when it is. Both
 * sides must also make sure to hold the relevant locks.
 */

/* GuC Virtual Engine */
struct guc_virtual_engine {
	struct intel_context context;
	struct intel_engine_cs base;
};

static struct intel_context *
guc_create_virtual(struct intel_engine_cs **siblings, unsigned int count,
		   unsigned long flags);

static struct intel_context *
guc_create_parallel(struct intel_engine_cs **engines,
		    unsigned int num_siblings,
		    unsigned int width);

static void add_to_context(struct i915_request *rq);

#define GUC_REQUEST_SIZE 64 /* bytes */

/*
 * Below is a set of functions which control the GuC scheduling state which
 * require a lock.
 */
#define SCHED_STATE_WAIT_FOR_DEREGISTER_TO_REGISTER	BIT(0)
#define SCHED_STATE_DESTROYED				BIT(1)
#define SCHED_STATE_PENDING_DISABLE			BIT(2)
#define SCHED_STATE_BANNED				BIT(3)
#define SCHED_STATE_ENABLED				BIT(4)
#define SCHED_STATE_PENDING_ENABLE			BIT(5)
#define SCHED_STATE_REGISTERED				BIT(6)
#define SCHED_STATE_POLICY_REQUIRED			BIT(7)
#define SCHED_STATE_CLOSED				BIT(8)
#define SCHED_STATE_BLOCKED_SHIFT			9
#define SCHED_STATE_BLOCKED		BIT(SCHED_STATE_BLOCKED_SHIFT)
#define SCHED_STATE_BLOCKED_MASK	(0xfff << SCHED_STATE_BLOCKED_SHIFT)

static inline void init_sched_state(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state &= SCHED_STATE_BLOCKED_MASK;
}

/*
 * Kernel contexts can have SCHED_STATE_REGISTERED after suspend.
 * A context close can race with the submission path, so SCHED_STATE_CLOSED
 * can be set immediately before we try to register.
 */
#define SCHED_STATE_VALID_INIT \
	(SCHED_STATE_BLOCKED_MASK | \
	 SCHED_STATE_CLOSED | \
	 SCHED_STATE_REGISTERED)

__maybe_unused
static bool sched_state_is_init(struct intel_context *ce)
{
	return !(ce->guc_state.sched_state & ~SCHED_STATE_VALID_INIT);
}

static inline bool
context_wait_for_deregister_to_register(struct intel_context *ce)
{
	return ce->guc_state.sched_state &
		SCHED_STATE_WAIT_FOR_DEREGISTER_TO_REGISTER;
}

static inline void
set_context_wait_for_deregister_to_register(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state |=
		SCHED_STATE_WAIT_FOR_DEREGISTER_TO_REGISTER;
}

static inline void
clr_context_wait_for_deregister_to_register(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state &=
		~SCHED_STATE_WAIT_FOR_DEREGISTER_TO_REGISTER;
}

static inline bool
context_destroyed(struct intel_context *ce)
{
	return ce->guc_state.sched_state & SCHED_STATE_DESTROYED;
}

static inline void
set_context_destroyed(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state |= SCHED_STATE_DESTROYED;
}

static inline bool context_pending_disable(struct intel_context *ce)
{
	return ce->guc_state.sched_state & SCHED_STATE_PENDING_DISABLE;
}

static inline void set_context_pending_disable(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state |= SCHED_STATE_PENDING_DISABLE;
}

static inline void clr_context_pending_disable(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state &= ~SCHED_STATE_PENDING_DISABLE;
}

static inline bool context_banned(struct intel_context *ce)
{
	return ce->guc_state.sched_state & SCHED_STATE_BANNED;
}

static inline void set_context_banned(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state |= SCHED_STATE_BANNED;
}

static inline void clr_context_banned(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state &= ~SCHED_STATE_BANNED;
}

static inline bool context_enabled(struct intel_context *ce)
{
	return ce->guc_state.sched_state & SCHED_STATE_ENABLED;
}

static inline void set_context_enabled(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state |= SCHED_STATE_ENABLED;
}

static inline void clr_context_enabled(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state &= ~SCHED_STATE_ENABLED;
}

static inline bool context_pending_enable(struct intel_context *ce)
{
	return ce->guc_state.sched_state & SCHED_STATE_PENDING_ENABLE;
}

static inline void set_context_pending_enable(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state |= SCHED_STATE_PENDING_ENABLE;
}

static inline void clr_context_pending_enable(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state &= ~SCHED_STATE_PENDING_ENABLE;
}

static inline bool context_registered(struct intel_context *ce)
{
	return ce->guc_state.sched_state & SCHED_STATE_REGISTERED;
}

static inline void set_context_registered(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state |= SCHED_STATE_REGISTERED;
}

static inline void clr_context_registered(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state &= ~SCHED_STATE_REGISTERED;
}

static inline bool context_policy_required(struct intel_context *ce)
{
	return ce->guc_state.sched_state & SCHED_STATE_POLICY_REQUIRED;
}

static inline void set_context_policy_required(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state |= SCHED_STATE_POLICY_REQUIRED;
}

static inline void clr_context_policy_required(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state &= ~SCHED_STATE_POLICY_REQUIRED;
}

static inline bool context_close_done(struct intel_context *ce)
{
	return ce->guc_state.sched_state & SCHED_STATE_CLOSED;
}

static inline void set_context_close_done(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state |= SCHED_STATE_CLOSED;
}

static inline u32 context_blocked(struct intel_context *ce)
{
	return (ce->guc_state.sched_state & SCHED_STATE_BLOCKED_MASK) >>
		SCHED_STATE_BLOCKED_SHIFT;
}

static inline void incr_context_blocked(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);

	ce->guc_state.sched_state += SCHED_STATE_BLOCKED;

	GEM_BUG_ON(!context_blocked(ce));	/* Overflow check */
}

static inline void decr_context_blocked(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);

	GEM_BUG_ON(!context_blocked(ce));	/* Underflow check */

	ce->guc_state.sched_state -= SCHED_STATE_BLOCKED;
}

static struct intel_context *
request_to_scheduling_context(struct i915_request *rq)
{
	return intel_context_to_parent(rq->context);
}

static inline bool context_guc_id_invalid(struct intel_context *ce)
{
	return ce->guc_id.id == GUC_INVALID_CONTEXT_ID;
}

static inline void set_context_guc_id_invalid(struct intel_context *ce)
{
	ce->guc_id.id = GUC_INVALID_CONTEXT_ID;
}

static inline struct intel_guc *ce_to_guc(struct intel_context *ce)
{
	return &ce->engine->gt->uc.guc;
}

static inline struct i915_priolist *to_priolist(struct rb_node *rb)
{
	return rb_entry(rb, struct i915_priolist, node);
}

/*
 * When using multi-lrc submission a scratch memory area is reserved in the
 * parent's context state for the process descriptor, work queue, and handshake
 * between the parent + children contexts to insert safe preemption points
 * between each of the BBs. Currently the scratch area is sized to a page.
 *
 * The layout of this scratch area is below:
 * 0						guc_process_desc
 * + sizeof(struct guc_process_desc)		child go
 * + CACHELINE_BYTES				child join[0]
 * ...
 * + CACHELINE_BYTES				child join[n - 1]
 * ...						unused
 * PARENT_SCRATCH_SIZE / 2			work queue start
 * ...						work queue
 * PARENT_SCRATCH_SIZE - 1			work queue end
 */
#define WQ_SIZE			(PARENT_SCRATCH_SIZE / 2)
#define WQ_OFFSET		(PARENT_SCRATCH_SIZE - WQ_SIZE)

struct sync_semaphore {
	u32 semaphore;
	u8 unused[CACHELINE_BYTES - sizeof(u32)];
};

struct parent_scratch {
	union guc_descs {
		struct guc_sched_wq_desc wq_desc;
		struct guc_process_desc_v69 pdesc;
	} descs;

	struct sync_semaphore go;
	struct sync_semaphore join[MAX_ENGINE_INSTANCE + 1];

	u8 unused[WQ_OFFSET - sizeof(union guc_descs) -
		sizeof(struct sync_semaphore) * (MAX_ENGINE_INSTANCE + 2)];

	u32 wq[WQ_SIZE / sizeof(u32)];
};

static u32 __get_parent_scratch_offset(struct intel_context *ce)
{
	GEM_BUG_ON(!ce->parallel.guc.parent_page);

	return ce->parallel.guc.parent_page * PAGE_SIZE;
}

static u32 __get_wq_offset(struct intel_context *ce)
{
	BUILD_BUG_ON(offsetof(struct parent_scratch, wq) != WQ_OFFSET);

	return __get_parent_scratch_offset(ce) + WQ_OFFSET;
}

static struct parent_scratch *
__get_parent_scratch(struct intel_context *ce)
{
	BUILD_BUG_ON(sizeof(struct parent_scratch) != PARENT_SCRATCH_SIZE);
	BUILD_BUG_ON(sizeof(struct sync_semaphore) != CACHELINE_BYTES);

	/*
	 * Need to subtract LRC_STATE_OFFSET here as the
	 * parallel.guc.parent_page is the offset into ce->state while
	 * ce->lrc_reg_reg is ce->state + LRC_STATE_OFFSET.
	 */
	return (struct parent_scratch *)
		(ce->lrc_reg_state +
		 ((__get_parent_scratch_offset(ce) -
		   LRC_STATE_OFFSET) / sizeof(u32)));
}

static struct guc_process_desc_v69 *
__get_process_desc_v69(struct intel_context *ce)
{
	struct parent_scratch *ps = __get_parent_scratch(ce);

	return &ps->descs.pdesc;
}

static struct guc_sched_wq_desc *
__get_wq_desc_v70(struct intel_context *ce)
{
	struct parent_scratch *ps = __get_parent_scratch(ce);

	return &ps->descs.wq_desc;
}

static u32 *get_wq_pointer(struct intel_context *ce, u32 wqi_size)
{
	/*
	 * Check for space in work queue. Caching a value of head pointer in
	 * intel_context structure in order reduce the number accesses to shared
	 * GPU memory which may be across a PCIe bus.
	 */
#define AVAILABLE_SPACE	\
	CIRC_SPACE(ce->parallel.guc.wqi_tail, ce->parallel.guc.wqi_head, WQ_SIZE)
	if (wqi_size > AVAILABLE_SPACE) {
		ce->parallel.guc.wqi_head = READ_ONCE(*ce->parallel.guc.wq_head);

		if (wqi_size > AVAILABLE_SPACE)
			return NULL;
	}
#undef AVAILABLE_SPACE

	return &__get_parent_scratch(ce)->wq[ce->parallel.guc.wqi_tail / sizeof(u32)];
}

static inline struct intel_context *__get_context(struct intel_guc *guc, u32 id)
{
	struct intel_context *ce = xa_load(&guc->context_lookup, id);

	GEM_BUG_ON(id >= GUC_MAX_CONTEXT_ID);

	return ce;
}

static struct guc_lrc_desc_v69 *__get_lrc_desc_v69(struct intel_guc *guc, u32 index)
{
	struct guc_lrc_desc_v69 *base = guc->lrc_desc_pool_vaddr_v69;

	if (!base)
		return NULL;

	GEM_BUG_ON(index >= GUC_MAX_CONTEXT_ID);

	return &base[index];
}

static int guc_lrc_desc_pool_create_v69(struct intel_guc *guc)
{
	u32 size;
	int ret;

	size = PAGE_ALIGN(sizeof(struct guc_lrc_desc_v69) *
			  GUC_MAX_CONTEXT_ID);
	ret = intel_guc_allocate_and_map_vma(guc, size, &guc->lrc_desc_pool_v69,
					     (void **)&guc->lrc_desc_pool_vaddr_v69);
	if (ret)
		return ret;

	return 0;
}

static void guc_lrc_desc_pool_destroy_v69(struct intel_guc *guc)
{
	if (!guc->lrc_desc_pool_vaddr_v69)
		return;

	guc->lrc_desc_pool_vaddr_v69 = NULL;
	i915_vma_unpin_and_release(&guc->lrc_desc_pool_v69, I915_VMA_RELEASE_MAP);
}

static inline bool guc_submission_initialized(struct intel_guc *guc)
{
	return guc->submission_initialized;
}

static inline void _reset_lrc_desc_v69(struct intel_guc *guc, u32 id)
{
	struct guc_lrc_desc_v69 *desc = __get_lrc_desc_v69(guc, id);

	if (desc)
		memset(desc, 0, sizeof(*desc));
}

static inline bool ctx_id_mapped(struct intel_guc *guc, u32 id)
{
	return __get_context(guc, id);
}

static inline struct intel_context *
set_ctx_id_mapping(struct intel_guc *guc, u32 id, struct intel_context *ce)
{
	unsigned long flags;

	/*
	 * xarray API doesn't have xa_save_irqsave wrapper, so calling the
	 * lower level functions directly.
	 */
	xa_lock_irqsave(&guc->context_lookup, flags);
	ce = __xa_store(&guc->context_lookup, id, ce, GFP_ATOMIC);
	xa_unlock_irqrestore(&guc->context_lookup, flags);

	return ce;
}

static inline void clr_ctx_id_mapping(struct intel_guc *guc, u32 id)
{
	unsigned long flags;

	if (unlikely(!guc_submission_initialized(guc)))
		return;

	_reset_lrc_desc_v69(guc, id);

	/*
	 * xarray API doesn't have xa_erase_irqsave wrapper, so calling
	 * the lower level functions directly.
	 */
	xa_lock_irqsave(&guc->context_lookup, flags);
	__xa_erase(&guc->context_lookup, id);
	xa_unlock_irqrestore(&guc->context_lookup, flags);
}

static void incr_outstanding_submission_g2h(struct intel_guc *guc)
{
	if (atomic_fetch_inc(&guc->outstanding_submission_g2h))
		return;

	__intel_gt_pm_get(guc_to_gt(guc));
	intel_boost_fake_int_timer(guc_to_gt(guc), true);
}

static void decr_outstanding_submission_g2h(struct intel_guc *guc)
{
	if (!atomic_dec_and_test(&guc->outstanding_submission_g2h))
		return;

	wake_up_all(&guc->ct.wq);
	intel_boost_fake_int_timer(guc_to_gt(guc), false);
	intel_gt_pm_put_async_untracked(guc_to_gt(guc));
}

static int guc_submission_send_busy_loop(struct intel_guc *guc,
					 const u32 *action,
					 u32 len,
					 u32 g2h_len_dw,
					 bool loop)
{
	int ret;

	/*
	 * We always loop when a send requires a reply (i.e. g2h_len_dw > 0),
	 * so we don't handle the case where we don't get a reply because we
	 * aborted the send due to the channel being busy.
	 */
	GEM_BUG_ON(g2h_len_dw && !loop);

	if (g2h_len_dw)
		incr_outstanding_submission_g2h(guc);

	ret = intel_guc_send_busy_loop(guc, action, len, g2h_len_dw, loop);
	if (unlikely(ret && g2h_len_dw))
		decr_outstanding_submission_g2h(guc);

	return ret;
}

static int guc_context_policy_init_v70(struct intel_context *ce, bool loop);
static int try_context_registration(struct intel_context *ce, bool loop);

static int __guc_add_request(struct intel_guc *guc, struct i915_request *rq)
{
	int err = 0;
	struct intel_context *ce = request_to_scheduling_context(rq);
	u32 action[3];
	int len = 0;
	u32 g2h_len_dw = 0;
	bool enabled;

	lockdep_assert_held(&rq->sched_engine->lock);

	GEM_BUG_ON(!atomic_read(&ce->guc_id.ref));
	GEM_BUG_ON(context_guc_id_invalid(ce));

	if (context_policy_required(ce)) {
		err = guc_context_policy_init_v70(ce, false);
		if (err)
			return err;
	}

	spin_lock(&ce->guc_state.lock);

	/*
	 * The request / context will be run on the hardware when scheduling
	 * gets enabled in the unblock. For multi-lrc we still submit the
	 * context to move the LRC tails.
	 */
	if (unlikely(context_blocked(ce) && !intel_context_is_parent(ce))) {
		spin_unlock(&ce->guc_state.lock);
		return err;
	}

	enabled = context_enabled(ce) || context_blocked(ce) || context_pending_enable(ce);
	if (!enabled) {
		action[len++] = INTEL_GUC_ACTION_SCHED_CONTEXT_MODE_SET;
		action[len++] = ce->guc_id.id;
		action[len++] = GUC_CONTEXT_ENABLE;
		intel_context_get(ce);
		set_context_enabled(ce);
		set_context_pending_enable(ce);
		incr_outstanding_submission_g2h(guc);
		g2h_len_dw = G2H_LEN_DW_SCHED_CONTEXT_MODE_SET;
	} else {
		action[len++] = INTEL_GUC_ACTION_SCHED_CONTEXT;
		action[len++] = ce->guc_id.id;
	}
	spin_unlock(&ce->guc_state.lock);

	err = intel_guc_send_nb(guc, action, len, g2h_len_dw);
	if (!enabled && !err) {
		trace_intel_context_sched_enable(ce);
		/*
		 * Without multi-lrc KMD does the submission step (moving the
		 * lrc tail) so enabling scheduling is sufficient to submit the
		 * context. This isn't the case in multi-lrc submission as the
		 * GuC needs to move the tails, hence the need for another H2G
		 * to submit a multi-lrc context after enabling scheduling.
		 */
		if (intel_context_is_parent(ce)) {
			action[0] = INTEL_GUC_ACTION_SCHED_CONTEXT;
			err = intel_guc_send_nb(guc, action, len - 1, 0);
		}
	} else if (!enabled) {
		decr_outstanding_submission_g2h(guc);
		spin_lock(&ce->guc_state.lock);
		clr_context_pending_enable(ce);
		clr_context_enabled(ce);
		spin_unlock(&ce->guc_state.lock);
		intel_context_put(ce);
	}
	if (likely(!err))
		trace_i915_request_guc_submit(rq);

	return err;
}

static int guc_add_request(struct intel_guc *guc, struct i915_request *rq)
{
	int ret = __guc_add_request(guc, rq);

	if (unlikely(ret == -EBUSY)) {
		guc->stalled_request = rq;
		guc->submission_stall_reason = STALL_ADD_REQUEST;
	}

	return ret;
}

static inline void guc_set_lrc_tail(struct i915_request *rq)
{
	wmb(); /* Ensure writes to ring are pushed before tail pointer is updated */
	WRITE_ONCE(rq->context->lrc_reg_state[CTX_RING_TAIL],
		   intel_ring_set_tail(rq->ring, rq->tail));
}

static inline int rq_prio(const struct i915_request *rq)
{
	return rq->sched.attr.priority;
}

static bool is_multi_lrc_rq(struct i915_request *rq)
{
	return intel_context_is_parallel(rq->context);
}

static bool can_merge_rq(struct i915_request *rq,
			 struct i915_request *last)
{
	return request_to_scheduling_context(rq) ==
		request_to_scheduling_context(last);
}

static u32 wq_space_until_wrap(struct intel_context *ce)
{
	return (WQ_SIZE - ce->parallel.guc.wqi_tail);
}

static void write_wqi(struct intel_context *ce, u32 wqi_size)
{
	BUILD_BUG_ON(!is_power_of_2(WQ_SIZE));

	/*
	 * Ensure WQI are visible before updating tail
	 */
	i915_write_barrier(ce->engine->i915);

	ce->parallel.guc.wqi_tail = (ce->parallel.guc.wqi_tail + wqi_size) &
		(WQ_SIZE - 1);
	WRITE_ONCE(*ce->parallel.guc.wq_tail, ce->parallel.guc.wqi_tail);
}

static int guc_wq_noop_append(struct intel_context *ce)
{
	u32 *wqi = get_wq_pointer(ce, wq_space_until_wrap(ce));
	u32 len_dw = wq_space_until_wrap(ce) / sizeof(u32) - 1;

	if (!wqi)
		return -EBUSY;

	GEM_BUG_ON(!FIELD_FIT(WQ_LEN_MASK, len_dw));

	*wqi = FIELD_PREP(WQ_TYPE_MASK, WQ_TYPE_NOOP) |
		FIELD_PREP(WQ_LEN_MASK, len_dw);
	ce->parallel.guc.wqi_tail = 0;

	return 0;
}

static int __guc_wq_item_append(struct i915_request *rq)
{
	struct intel_context *ce = request_to_scheduling_context(rq);
	struct intel_context *child;
	unsigned int wqi_size = (ce->parallel.number_children + 4) *
		sizeof(u32);
	u32 *wqi;
	u32 len_dw = (wqi_size / sizeof(u32)) - 1;
	int ret;

	/* Ensure context is in correct state updating work queue */
	GEM_BUG_ON(!atomic_read(&ce->guc_id.ref));
	GEM_BUG_ON(context_guc_id_invalid(ce));
	GEM_BUG_ON(context_wait_for_deregister_to_register(ce));
	GEM_BUG_ON(!ctx_id_mapped(ce_to_guc(ce), ce->guc_id.id));

	/* Insert NOOP if this work queue item will wrap the tail pointer. */
	if (wqi_size > wq_space_until_wrap(ce)) {
		ret = guc_wq_noop_append(ce);
		if (ret)
			return ret;
	}

	wqi = get_wq_pointer(ce, wqi_size);
	if (!wqi)
		return -EBUSY;

	GEM_BUG_ON(!FIELD_FIT(WQ_LEN_MASK, len_dw));

	*wqi++ = FIELD_PREP(WQ_TYPE_MASK, WQ_TYPE_MULTI_LRC) |
		FIELD_PREP(WQ_LEN_MASK, len_dw);
	*wqi++ = ce->lrc.lrca;
	*wqi++ = FIELD_PREP(WQ_GUC_ID_MASK, ce->guc_id.id) |
	       FIELD_PREP(WQ_RING_TAIL_MASK, ce->ring->tail / sizeof(u64));
	*wqi++ = 0;	/* fence_id */
	for_each_child(ce, child)
		*wqi++ = child->ring->tail / sizeof(u64);

	write_wqi(ce, wqi_size);

	return 0;
}

static int guc_wq_item_append(struct intel_guc *guc,
			      struct i915_request *rq)
{
	int ret;

	ret = __guc_wq_item_append(rq);
	if (unlikely(ret == -EBUSY)) {
		guc->stalled_request = rq;
		guc->submission_stall_reason = STALL_MOVE_LRC_TAIL;
	}

	return ret;
}

static bool multi_lrc_submit(struct i915_request *rq)
{
	struct intel_context *ce = request_to_scheduling_context(rq);

	intel_ring_set_tail(rq->ring, rq->tail);

	/*
	 * We expect the front end (execbuf IOCTL) to set this flag on the last
	 * request generated from a multi-BB submission. This indicates to the
	 * backend (GuC interface) that we should submit this context thus
	 * submitting all the requests generated in parallel.
	 */
	return test_bit(I915_FENCE_FLAG_SUBMIT_PARALLEL, &rq->fence.flags) ||
		intel_context_is_banned(ce);
}

#ifdef BPM_TASKLET_STRUCT_CALLBACK_NOT_PRESENT
#define set_tasklet_fn(t, fn) (t)->func = (fn)
typedef unsigned long tasklet_data_t;
#else
#define set_tasklet_fn(t, fn) (t)->callback = (fn)
typedef struct tasklet_struct *tasklet_data_t;
#endif

static void nop_submission_tasklet(tasklet_data_t t)
{
}

static int guc_dequeue_one_context(struct intel_guc *guc)
{
	struct i915_sched_engine * const sched_engine = guc->sched_engine;
	struct i915_request *last = NULL;
	bool submit = false;
	struct rb_node *rb;
	int ret;

	lockdep_assert_held(&sched_engine->lock);

	GEM_BUG_ON(intel_gt_is_wedged(guc_to_gt(guc)));

	if (guc->stalled_request) {
		submit = true;
		last = guc->stalled_request;

		switch (guc->submission_stall_reason) {
		case STALL_REGISTER_CONTEXT:
			goto register_context;
		case STALL_MOVE_LRC_TAIL:
			goto move_lrc_tail;
		case STALL_ADD_REQUEST:
			goto add_request;
		default:
			MISSING_CASE(guc->submission_stall_reason);
		}
	}

	while ((rb = rb_first_cached(&sched_engine->queue))) {
		struct i915_priolist *p = to_priolist(rb);
		struct i915_request *rq, *rn;

		priolist_for_each_request_consume(rq, rn, p) {
			if (last && !can_merge_rq(rq, last))
				goto register_context;

			if (unlikely(intel_context_is_banned(rq->context)))
				i915_request_put(i915_request_mark_eio(rq));

			if (!__i915_request_submit(rq))
				continue;

			add_to_context(rq);
			last = rq;

			if (is_multi_lrc_rq(rq)) {
				/*
				 * We need to coalesce all multi-lrc requests in
				 * a relationship into a single H2G. We are
				 * guaranteed that all of these requests will be
				 * submitted sequentially.
				 */
				if (multi_lrc_submit(rq)) {
					submit = true;
					goto register_context;
				}
			} else {
				submit = true;
			}
		}

		rb_erase_cached(&p->node, &sched_engine->queue);
		i915_priolist_free(p);
	}

register_context:
	if (submit) {
		struct intel_context *ce = request_to_scheduling_context(last);

		if (unlikely(!ctx_id_mapped(guc, ce->guc_id.id))) {
			ret = try_context_registration(ce, false);
			if (unlikely(ret == -EPIPE)) {
				goto deadlk;
			} else if (ret == -EBUSY) {
				guc->stalled_request = last;
				guc->submission_stall_reason =
					STALL_REGISTER_CONTEXT;
				goto schedule_tasklet;
			} else if (ret != 0) {
				GEM_WARN_ON(ret);	/* Unexpected */
				goto deadlk;
			}
		}

move_lrc_tail:
		if (is_multi_lrc_rq(last)) {
			ret = guc_wq_item_append(guc, last);
			if (ret == -EBUSY) {
				goto schedule_tasklet;
			} else if (ret != 0) {
				GEM_WARN_ON(ret);	/* Unexpected */
				goto deadlk;
			}
		} else {
			guc_set_lrc_tail(last);
		}

add_request:
		ret = guc_add_request(guc, last);
		if (unlikely(ret == -EPIPE)) {
			goto deadlk;
		} else if (ret == -EBUSY) {
			goto schedule_tasklet;
		} else if (ret != 0) {
			GEM_WARN_ON(ret);	/* Unexpected */
			goto deadlk;
		}
	}

	guc->stalled_request = NULL;
	guc->submission_stall_reason = STALL_NONE;
	return submit;

deadlk:
	set_tasklet_fn(&sched_engine->tasklet, nop_submission_tasklet);
	tasklet_disable_nosync(&sched_engine->tasklet);
	return false;

schedule_tasklet:
	tasklet_schedule(&sched_engine->tasklet);
	return false;
}

static void guc_submission_tasklet(tasklet_data_t t)
{
#ifdef BPM_TASKLET_STRUCT_CALLBACK_NOT_PRESENT
	struct intel_guc *guc = (struct intel_guc *)t;
	struct i915_sched_engine *sched_engine = guc->sched_engine;
#else
	struct i915_sched_engine *sched_engine =
		from_tasklet(sched_engine, t, tasklet);
	struct intel_guc *guc = sched_engine->private_data;
#endif
	unsigned long flags;

	spin_lock_irqsave(&sched_engine->lock, flags);

	while (guc_dequeue_one_context(guc))
		;
	i915_sched_engine_reset_on_empty(sched_engine);

	spin_unlock_irqrestore(&sched_engine->lock, flags);
}

static void cs_irq_handler(struct intel_engine_cs *engine, u16 iir)
{
	if (iir & GT_RENDER_USER_INTERRUPT)
		intel_engine_signal_breadcrumbs_irq(engine);

	if (iir & GT_RENDER_PIPECTL_NOTIFY_INTERRUPT)
		wake_up_all(&engine->breadcrumbs->wq);
}

static void __guc_context_destroy(struct intel_context *ce);
static void release_guc_id(struct intel_guc *guc, struct intel_context *ce);
static void guc_signal_context_fence(struct intel_context *ce);
static void guc_cancel_context_requests(struct intel_context *ce);
static void guc_blocked_fence_complete(struct intel_context *ce);

static void scrub_guc_desc_for_outstanding_g2h(struct intel_guc *guc)
{
	struct intel_context *ce;
	unsigned long index, flags;
	bool pending_disable, pending_enable, deregister, destroyed, banned;

	rcu_read_lock();
	xa_for_each(&guc->context_lookup, index, ce) {
		/*
		 * Corner case where the ref count on the object is zero but and
		 * deregister G2H was lost. In this case we don't touch the ref
		 * count and finish the destroy of the context.
		 */
		bool do_put = kref_get_unless_zero(&ce->ref);

		rcu_read_unlock();

		if (test_bit(CONTEXT_GUC_INIT, &ce->flags) &&
		    (cancel_delayed_work(&ce->guc_state.sched_disable_delay_work))) {
			/* successful cancel so jump straight to close it */
			intel_context_sched_disable_unpin(ce);
		}

		spin_lock_irqsave(&ce->guc_state.lock, flags);

		/*
		 * Once we are at this point submission_disabled() is guaranteed
		 * to be visible to all callers who set the below flags (see above
		 * flush and flushes in reset_prepare). If submission_disabled()
		 * is set, the caller shouldn't set these flags.
		 */

		destroyed = context_destroyed(ce);
		pending_enable = context_pending_enable(ce);
		pending_disable = context_pending_disable(ce);
		deregister = context_wait_for_deregister_to_register(ce);
		banned = context_banned(ce);
		init_sched_state(ce);

		spin_unlock_irqrestore(&ce->guc_state.lock, flags);

		if (pending_enable || destroyed || deregister) {
			decr_outstanding_submission_g2h(guc);
			if (deregister)
				guc_signal_context_fence(ce);
			if (destroyed) {
				intel_gt_pm_put_async_untracked(guc_to_gt(guc));
				release_guc_id(guc, ce);
				__guc_context_destroy(ce);
			}
			if (pending_enable || deregister)
				intel_context_put(ce);
		}

		/* Not mutualy exclusive with above if statement. */
		if (pending_disable) {
			guc_signal_context_fence(ce);
			if (banned)
				guc_cancel_context_requests(ce);
			intel_context_sched_disable_unpin(ce);
			decr_outstanding_submission_g2h(guc);

			spin_lock_irqsave(&ce->guc_state.lock, flags);
			guc_blocked_fence_complete(ce);
			spin_unlock_irqrestore(&ce->guc_state.lock, flags);

			intel_context_put(ce);
		}

		if (do_put)
			intel_context_put(ce);
		rcu_read_lock();
	}
	rcu_read_unlock();
}

static bool busy_type_is_v1(struct intel_guc *guc)
{
	if (GUC_SUBMIT_VER(guc) < MAKE_GUC_VER(1, 14, 1))
		return true;

	return false;
}

static bool busy_type_is_v2(struct intel_guc *guc)
{
	/* Must not call this before the submit version is determined! */
	GEM_BUG_ON(guc->submission_version.major == 0);

	/*
	 * GuC Busyness v2 is deprecated. Adding this function to allow
	 * separation of v1 and v2. This enables adding support for V3
	 * logic easier.
	 */
	return false;
}

static bool busy_type_is_v3(struct intel_guc *guc)
{
	/* Must not call this before the submit version is determined! */
	GEM_BUG_ON(guc->submission_version.major == 0);

	if (IS_SRIOV_VF(guc_to_gt(guc)->i915))
		return false;

	if (GUC_SUBMIT_VER(guc) >= MAKE_GUC_VER(1, 14, 1))
		return true;

	return false;
}

static int guc_busy_v3_alloc_activity_groups(struct intel_guc *guc)
{
	struct drm_i915_private *i915 = guc_to_gt(guc)->i915;
	/*
	 * Two additional activity groups are allocated one for global
	 * engine busyness and one for PF when SRIOV is enabled
	 */
	u32 num_ags = IS_SRIOV_PF(i915) ?
		      i915_sriov_pf_get_totalvfs(i915) + 2 :
		      1;

	guc->busy.v3.ag = kmalloc_array(num_ags, sizeof(struct activity_group),
					GFP_KERNEL);
	if (!guc->busy.v3.ag)
		return -ENOMEM;

	memset(guc->busy.v3.ag, 0, num_ags * sizeof(struct activity_group));
	guc->busy.v3.num_ags = num_ags;

	return 0;
}

static int guc_busy_v3_alloc_activity_data(struct intel_guc *guc,
					   struct activity_buffer *ab,
					   unsigned int count)
{
	size_t size = sizeof(struct guc_engine_activity_data) * count;
	void *ptr;
	int ret;

	ret = __intel_guc_allocate_and_map_vma(guc, size, false,
					       &ab->activity_vma, &ptr);
	if (ret)
		return ret;

	if (i915_gem_object_is_lmem(ab->activity_vma->obj))
		iosys_map_set_vaddr_iomem(&ab->activity_map, (void __iomem *)ptr);
	else
		iosys_map_set_vaddr(&ab->activity_map, ptr);

	return 0;
}

static void guc_busy_v3_free_activity_data(struct intel_guc *guc,
					   struct activity_buffer *ab)
{
	if (!ab->activity_vma)
		return;

	i915_vma_unpin_and_release(&ab->activity_vma, I915_VMA_RELEASE_MAP);
	iosys_map_clear(&ab->activity_map);

	ab->activity_vma = NULL;
}

static int guc_busy_v3_alloc_metadata(struct intel_guc *guc,
				      struct activity_buffer *ab,
				      unsigned int count)
{
	size_t size = sizeof(struct guc_engine_activity_metadata) * count;
	void *ptr;
	int ret;

	ret = __intel_guc_allocate_and_map_vma(guc, size, true,
					       &ab->metadata_vma, &ptr);
	if (ret)
		return ret;

	iosys_map_set_vaddr(&ab->metadata_map, ptr);

	return 0;
}

static void guc_busy_v3_free_metadata(struct intel_guc *guc,
				      struct activity_buffer *ab)
{
	if (!ab->metadata_vma)
		return;

	i915_vma_unpin_and_release(&ab->metadata_vma, I915_VMA_RELEASE_MAP);
	iosys_map_clear(&ab->metadata_map);

	ab->metadata_vma = NULL;
}

static void guc_busy_v3_free_function_array(struct intel_guc *guc)
{
	guc_busy_v3_free_activity_data(guc, &guc->busy.v3.function);
	guc_busy_v3_free_metadata(guc, &guc->busy.v3.function);
}

static int guc_busy_v3_alloc_function_array(struct intel_guc *guc)
{
	int ret;

	ret = guc_busy_v3_alloc_activity_data(guc, &guc->busy.v3.function,
					      guc->busy.v3.num_functions);
	if (ret)
		return ret;

	ret = guc_busy_v3_alloc_metadata(guc, &guc->busy.v3.function,
					 guc->busy.v3.num_functions);
	if (ret)
		guc_busy_v3_free_activity_data(guc, &guc->busy.v3.function);

	return ret;
}

/*
 * GuC < 70.11.1 stores busyness stats for each engine at context in/out boundaries.
 * A context 'in' logs execution start time, 'out' adds in -> out delta to total.
 * i915/kmd accesses 'start', 'total' and 'context id' from memory shared with
 * GuC.
 *
 * __i915_pmu_event_read samples engine busyness. When sampling, if context id
 * is valid (!= ~0) and start is non-zero, the engine is considered to be
 * active. For an active engine total busyness = total + (now - start), where
 * 'now' is the time at which the busyness is sampled. For inactive engine,
 * total busyness = total.
 *
 * All times are captured from GUCPMTIMESTAMP reg and are in gt clock domain.
 *
 * The start and total values provided by GuC are 32 bits and wrap around in a
 * few minutes. Since perf pmu provides busyness as 64 bit monotonically
 * increasing ns values, there is a need for this implementation to account for
 * overflows and extend the GuC provided values to 64 bits before returning
 * busyness to the user. In order to do that, a worker runs periodically at
 * frequency = 1/8th the time it takes for the timestamp to wrap (i.e. once in
 * 27 seconds for a gt clock frequency of 19.2 MHz).
 */

#define BUSY_V1_WRAP_TIME_CLKS U32_MAX
#define BUSY_V1_POLL_TIME_CLKS (BUSY_V1_WRAP_TIME_CLKS >> 3)

static void
__busy_v1_extend_last_switch(struct intel_guc *guc, u64 *prev_start, u32 new_start)
{
	u32 gt_stamp_hi = upper_32_bits(guc->busy.v1.gt_stamp);
	u32 gt_stamp_last = lower_32_bits(guc->busy.v1.gt_stamp);

	if (new_start == lower_32_bits(*prev_start))
		return;

	/*
	 * When gt is unparked, we update the gt timestamp and start the ping
	 * worker that updates the gt_stamp every BUSY_V1_POLL_TIME_CLKS. As long as gt
	 * is unparked, all switched in contexts will have a start time that is
	 * within +/- BUSY_V1_POLL_TIME_CLKS of the most recent gt_stamp.
	 *
	 * If neither gt_stamp nor new_start has rolled over, then the
	 * gt_stamp_hi does not need to be adjusted, however if one of them has
	 * rolled over, we need to adjust gt_stamp_hi accordingly.
	 *
	 * The below conditions address the cases of new_start rollover and
	 * gt_stamp_last rollover respectively.
	 */
	if (new_start < gt_stamp_last &&
	    (new_start - gt_stamp_last) <= BUSY_V1_POLL_TIME_CLKS)
		gt_stamp_hi++;

	if (new_start > gt_stamp_last &&
	    (gt_stamp_last - new_start) <= BUSY_V1_POLL_TIME_CLKS && gt_stamp_hi)
		gt_stamp_hi--;

	*prev_start = make_u64(gt_stamp_hi, new_start);
}

/*
 * GuC updates shared memory and KMD reads it. Since this is not synchronized,
 * we run into a race where the value read is inconsistent. Sometimes the
 * inconsistency is in reading the upper MSB bytes of the last_in value when
 * this race occurs. 2 types of cases are seen - upper 8 bits are zero and upper
 * 24 bits are zero. Since these are non-zero values, it is non-trivial to
 * determine validity of these values. Instead we read the values multiple times
 * until they are consistent. In test runs, 3 attempts results in consistent
 * values. The upper bound is set to 6 attempts and may need to be tuned as per
 * any new occurences.
 */
static void __busy_v1_get_engine_usage_record(struct intel_engine_cs *engine,
					      u32 *last_in, u32 *id, u32 *total)
{
	struct iosys_map rec_map = intel_guc_engine_usage_record_map_v1(engine);
	int i = 0;

#define record_read(map_, field_) \
	iosys_map_rd_field(map_, 0, struct guc_engine_usage_record, field_)

	do {
		*last_in = record_read(&rec_map, last_switch_in_stamp);
		*id = record_read(&rec_map, current_context_index);
		*total = record_read(&rec_map, total_runtime);

		if (record_read(&rec_map, last_switch_in_stamp) == *last_in &&
		    record_read(&rec_map, current_context_index) == *id &&
		    record_read(&rec_map, total_runtime) == *total)
			break;
	} while (++i < 6);

#undef record_read
}

static void busy_v1_guc_update_engine_gt_clks(struct intel_engine_cs *engine)
{
	struct intel_engine_guc_stats_v1 *stats = &engine->stats.guc_v1;
	struct intel_guc *guc = &engine->gt->uc.guc;
	u32 last_switch, ctx_id, total;

	lockdep_assert_held(&guc->busy.lock);

	__busy_v1_get_engine_usage_record(engine, &last_switch, &ctx_id, &total);

	stats->running = ctx_id != ~0U && last_switch;
	if (stats->running)
		__busy_v1_extend_last_switch(guc, &stats->start_gt_clk, last_switch);

	/*
	 * Instead of adjusting the total for overflow, just add the
	 * difference from previous sample stats->total_gt_clks
	 */
	if (total && total != ~0U) {
		stats->total_gt_clks += (u32)(total - stats->prev_total);
		stats->prev_total = total;
	}
}

static u32 gpm_timestamp_shift(struct intel_gt *gt)
{
	intel_wakeref_t wakeref;
	u32 reg, shift;

	with_intel_runtime_pm(gt->uncore->rpm, wakeref)
		reg = intel_uncore_read(gt->uncore, RPM_CONFIG0);

	shift = (reg & GEN10_RPM_CONFIG0_CTC_SHIFT_PARAMETER_MASK) >>
		GEN10_RPM_CONFIG0_CTC_SHIFT_PARAMETER_SHIFT;

	return 3 - shift;
}

static void busy_v1_guc_update_pm_timestamp(struct intel_guc *guc, ktime_t *now)
{
	struct intel_gt *gt = guc_to_gt(guc);
	u32 gt_stamp_lo, gt_stamp_hi;
	u64 gpm_ts;

	lockdep_assert_held(&guc->busy.lock);

	gt_stamp_hi = upper_32_bits(guc->busy.v1.gt_stamp);
	gpm_ts = intel_uncore_read64_2x32(gt->uncore, MISC_STATUS0,
					  MISC_STATUS1) >> guc->gpm_timestamp_shift;
	gt_stamp_lo = lower_32_bits(gpm_ts);
	if (now)
		*now = ktime_get();

	if (gt_stamp_lo < lower_32_bits(guc->busy.v1.gt_stamp))
		gt_stamp_hi++;

	guc->busy.v1.gt_stamp = make_u64(gt_stamp_hi, gt_stamp_lo);
}

static u64 __busy_v1_guc_engine_busyness_ticks(struct intel_engine_cs *engine, ktime_t *now_out)
{
	struct intel_engine_guc_stats_v1 stats_saved, *stats = &engine->stats.guc_v1;
	struct i915_gpu_error *gpu_error = &engine->i915->gpu_error;
	struct intel_gt *gt = engine->gt;
	struct intel_guc *guc = &gt->uc.guc;
	u64 total, gt_stamp_saved;
	unsigned long flags;
	u32 reset_count;
	bool in_reset;
	intel_wakeref_t wakeref;
	ktime_t now;

	spin_lock_irqsave(&guc->busy.lock, flags);

	/*
	 * If a reset happened, we risk reading partially updated engine
	 * busyness from GuC, so we just use the driver stored copy of busyness.
	 * Synchronize with gt reset using reset_count and the
	 * I915_RESET_BACKOFF flag. Note that reset flow updates the reset_count
	 * after I915_RESET_BACKOFF flag, so ensure that the reset_count is
	 * usable by checking the flag afterwards.
	 */
	reset_count = i915_reset_count(gpu_error);
	in_reset = test_bit(I915_RESET_BACKOFF, &gt->reset.flags);

	now = ktime_get();

	/*
	 * The active busyness depends on start_gt_clk and gt_stamp.
	 * gt_stamp is updated by i915 only when gt is awake and the
	 * start_gt_clk is derived from GuC state. To get a consistent
	 * view of activity, we query the GuC state only if gt is awake.
	 */
	if (!in_reset && !IS_SRIOV_VF(gt->i915) &&
	    (wakeref = intel_gt_pm_get_if_awake(gt))) {
		stats_saved = *stats;
		gt_stamp_saved = guc->busy.v1.gt_stamp;
		/*
		 * Update gt_clks, then gt timestamp to simplify the 'gt_stamp -
		 * start_gt_clk' calculation below for active engines.
		 */
		busy_v1_guc_update_engine_gt_clks(engine);
		busy_v1_guc_update_pm_timestamp(guc, &now);
		intel_gt_pm_put_async(gt, wakeref);
		if (i915_reset_count(gpu_error) != reset_count) {
			*stats = stats_saved;
			guc->busy.v1.gt_stamp = gt_stamp_saved;
		}
	}

	total = stats->total_gt_clks;
	if (stats->running) {
		u64 clk = guc->busy.v1.gt_stamp - stats->start_gt_clk;

		total += clk;
	}

	spin_unlock_irqrestore(&guc->busy.lock, flags);

	if (now_out)
		*now_out = now;

	return total;
}

/*
 * Unlike the execlist mode of submission total and active times are in terms of
 * gt clocks. The *now parameter is retained to return the cpu time at which the
 * busyness was sampled.
 */
static ktime_t busy_v1_guc_engine_busyness(struct intel_engine_cs *engine,
					   unsigned int vf_id,
					   ktime_t *now)
{
	u64 ticks = __busy_v1_guc_engine_busyness_ticks(engine, now);
	return intel_gt_clock_interval_to_ns(engine->gt, ticks);
}

static u64 busy_v1_guc_engine_busyness_ticks(struct intel_engine_cs *engine,
					     unsigned int vf_id)
{
	if (vf_id > 1) {
		/*
		 * VF specific counter is not available with v1 interface, but
		 * PF specific counter is available. Since 0 is global and 1 is
		 * PF, we support those values of vf_id here.
		 */
		return 0;
	}

	return __busy_v1_guc_engine_busyness_ticks(engine, NULL);
}

static void busy_v1_guc_enable_worker(struct intel_guc *guc)
{
	queue_delayed_work(system_highpri_wq, &guc->busy.work, guc->busy.v1.ping_delay);
}

static void busy_v1_guc_cancel_worker(struct intel_guc *guc)
{
	cancel_delayed_work(&guc->busy.work);
}

static void __busy_v1_reset_guc_busyness_stats(struct intel_guc *guc)
{
	struct intel_gt *gt = guc_to_gt(guc);
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	unsigned long flags;

	busy_v1_guc_cancel_worker(guc);

	spin_lock_irqsave(&guc->busy.lock, flags);

	busy_v1_guc_update_pm_timestamp(guc, NULL);
	for_each_engine(engine, gt, id) {
		busy_v1_guc_update_engine_gt_clks(engine);
		engine->stats.guc_v1.prev_total = 0;
	}

	spin_unlock_irqrestore(&guc->busy.lock, flags);
}

static void __busy_v1_update_guc_busyness_stats(struct intel_guc *guc)
{
	struct intel_gt *gt = guc_to_gt(guc);
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	unsigned long flags;

	guc->busy.v1.last_stat_jiffies = jiffies;

	spin_lock_irqsave(&guc->busy.lock, flags);

	busy_v1_guc_update_pm_timestamp(guc, NULL);
	for_each_engine(engine, gt, id)
		busy_v1_guc_update_engine_gt_clks(engine);

	spin_unlock_irqrestore(&guc->busy.lock, flags);
}

static void busy_v1_guc_timestamp_ping(struct work_struct *wrk)
{
	struct intel_guc *guc = container_of(wrk, typeof(*guc), busy.work.work);
	struct intel_uc *uc = container_of(guc, typeof(*uc), guc);
	struct intel_gt *gt = guc_to_gt(guc);
	intel_wakeref_t wakeref;
	int srcu, ret;

	wakeref = intel_gt_pm_get_if_awake(gt);
	if (!wakeref)
		return;

	/*
	 * Synchronize with gt reset to make sure the worker does not
	 * corrupt the engine/guc stats. NB: can't actually block waiting
	 * for a reset to complete as the reset requires flushing out
	 * this worker thread if started. So waiting would deadlock.
	 */
	ret = intel_gt_reset_trylock(gt, &srcu);
	if (ret)
		goto err_trylock;

	__busy_v1_update_guc_busyness_stats(guc);

	intel_gt_reset_unlock(gt, srcu);

	busy_v1_guc_enable_worker(guc);

err_trylock:
	intel_gt_pm_put(gt, wakeref);
}

static int busy_v1_guc_action_enable_usage_stats(struct intel_guc *guc)
{
	u32 offset = intel_guc_engine_usage_offset_global(guc);
	u32 action[] = {
		INTEL_GUC_ACTION_SET_ENG_UTIL_BUFF_V1,
		offset,
		0,
	};

	return intel_guc_send(guc, action, ARRAY_SIZE(action));
}

/*
 * GuC >= 70.11.1 maintains busyness counters in a shared memory buffer for each
 * engine on a continuous basis. The counters are all 64bits and count in clock
 * ticks. The values are updated on context switch events and periodically on a
 * timer internal to GuC. The update rate is guaranteed to be at least 2Hz (but
 * with the caveat that GuC is not a real-time OS so best effort only).
 *
 * In addition to an engine active time count, there is also a total time count.
 * For native, this is only a free-running GT timestamp counter. For PF/VF,
 * there is also a function active counter - how many ticks the VF or PF has had
 * available for execution.
 *
 * Note that the counters should only be used as ratios of each other for
 * a calculating a percentage. No guarantees are made about frequencies for
 * conversions to wall time, etc.
 *
 * ticks_engine:   clock ticks for which engine was active
 * ticks_function: clock ticks owned by this VF
 * ticks_gt:       total clock ticks
 *
 * native engine busyness: ticks_engine / ticks_gt
 * VF/PF engine busyness:  ticks_engine / ticks_function
 * VF/PF engine ownership: ticks_function / ticks_gt
 */

static u32 guc_engine_usage_offset_v2_device(struct intel_guc *guc)
{
	return intel_guc_ggtt_offset(guc, guc->busy.v2.device_vma);
}

static int guc_busy_v2_alloc_device(struct intel_guc *guc)
{
	size_t size = sizeof(struct guc_engine_observation_data);
	void *busy_v2_ptr;
	int ret;

	ret = __intel_guc_allocate_and_map_vma(guc, size, true,
					       &guc->busy.v2.device_vma, &busy_v2_ptr);
	if (ret)
		return ret;

	if (i915_gem_object_is_lmem(guc->busy.v2.device_vma->obj))
		iosys_map_set_vaddr_iomem(&guc->busy.v2.device_map, (void __iomem *)busy_v2_ptr);
	else
		iosys_map_set_vaddr(&guc->busy.v2.device_map, busy_v2_ptr);

	return 0;
}

static void guc_busy_v2_free_device(struct intel_guc *guc)
{
	i915_vma_unpin_and_release(&guc->busy.v2.device_vma, I915_VMA_RELEASE_MAP);
	iosys_map_clear(&guc->busy.v2.device_map);

	guc->busy.v2.device_vma = NULL;
}

static void __busy_v2_get_engine_usage_record(struct intel_guc *guc,
					      struct intel_engine_cs *engine,
					      u32 guc_vf,
					      u64 *_ticks_engine, u64 *_ticks_function,
					      u64 *_ticks_gt)
{
	struct iosys_map rec_map_engine, rec_map_global;
	u64 ticks_engine, ticks_function, ticks_gt;
	int i = 0, ret;

	ret = intel_guc_engine_usage_record_map_v2(guc, engine, guc_vf,
						   &rec_map_engine, &rec_map_global);
	if (ret) {
		ticks_engine = 0;
		ticks_function = 0;
		ticks_gt = 0;
		goto done;
	}

#define record_read_engine(map_, field_) \
	iosys_map_rd_field(map_, 0, struct guc_engine_data, field_)
#define record_read_global(map_, field_) \
	iosys_map_rd_field(map_, 0, struct guc_engine_observation_data, field_)

	do {
		if (engine)
			ticks_engine = record_read_engine(&rec_map_engine, total_execution_ticks);
		ticks_function = record_read_global(&rec_map_global, total_active_ticks);
		ticks_gt = record_read_global(&rec_map_global, gt_timestamp);

		if (engine && (record_read_engine(&rec_map_engine, total_execution_ticks) !=
			       ticks_engine))
			continue;

		if (record_read_global(&rec_map_global, total_active_ticks) == ticks_function &&
		    record_read_global(&rec_map_global, gt_timestamp) == ticks_gt)
			break;
	} while (++i < 6);

#undef record_read_engine
#undef record_read_global

done:
	if (_ticks_engine)
		*_ticks_engine = ticks_engine;
	if (_ticks_function)
		*_ticks_function = ticks_function;
	if (_ticks_gt)
		*_ticks_gt = ticks_gt;
}

static struct activity_engine *to_activity_engine(struct intel_engine_cs *engine, u32 idx)
{
	struct intel_guc *guc = &engine->gt->uc.guc;
	struct activity_group *ag = &guc->busy.v3.ag[idx];
	u8 guc_class = engine_class_to_guc_class(engine->class);
	u32 instance = ilog2(engine->logical_mask);

	return &ag->engine[guc_class][instance];
}

static u64 cpu_ns_to_guc_tsc_tick(ktime_t ns, u32 freq)
{
	return mul_u64_u32_div(ns, freq, NSEC_PER_SEC);
}

static u64 __busy_v3_get_engine_activity(struct intel_guc *guc,
					 struct intel_engine_cs *engine, u32 idx)
{
	struct activity_engine *ae = to_activity_engine(engine, idx);
	struct guc_engine_activity *cached_counter = &ae->counter;
	struct guc_engine_activity_metadata *cached_meta = &ae->metadata;
	struct iosys_map rec_map_activity, rec_map_metadata;
	struct intel_gt *gt = engine->gt;
	u32 global_change_num, last_update_tick;
	u16 change_num, quanta_ratio;
	u64 numerator, active_ticks, gpm_ts;
	ktime_t now, cpu_delta;

	rec_map_activity = intel_guc_engine_activity_map(guc, engine, idx);
	rec_map_metadata = intel_guc_engine_metadata_map(guc, idx);

#define record_read_activity(map_, field_) \
	iosys_map_rd_field(map_, 0, struct guc_engine_activity, field_)
#define record_read_metadata(map_, field_) \
	iosys_map_rd_field(map_, 0, struct guc_engine_activity_metadata, field_)

	global_change_num = record_read_metadata(&rec_map_metadata,
						 global_change_num);

	/* GuC has not initialized activity data yet, return 0 */
	if (!global_change_num)
		goto update;

	if (!cached_meta->guc_tsc_frequency_hz) {
		cached_meta->guc_tsc_frequency_hz = record_read_metadata(&rec_map_metadata,
									 guc_tsc_frequency_hz);
		cached_meta->lag_latency_usec = record_read_metadata(&rec_map_metadata,
								     lag_latency_usec);
	}

	if (global_change_num == cached_meta->global_change_num)
		goto update;
	else
		cached_meta->global_change_num = global_change_num;

	change_num = record_read_activity(&rec_map_activity, change_num);
	if (!change_num)
		goto update;

	if (change_num == cached_counter->change_num)
		goto update;

	/* read the engine stats */
	quanta_ratio = record_read_activity(&rec_map_activity, quanta_ratio);
	last_update_tick = record_read_activity(&rec_map_activity, last_update_tick);
	active_ticks = record_read_activity(&rec_map_activity, active_ticks);

	/* activity calculations */
	ae->running = !!last_update_tick;
	ae->total += active_ticks - cached_counter->active_ticks;
	ae->active = 0;

	/* cache the counter */
	cached_counter->change_num = change_num;
	cached_counter->quanta_ratio = quanta_ratio;
	cached_counter->last_update_tick = last_update_tick;
	cached_counter->active_ticks = active_ticks;

#undef record_read_activity
#undef record_read_metadata

update:
	if (ae->running) {
		gpm_ts = intel_uncore_read64_2x32(gt->uncore, MISC_STATUS0,
						  MISC_STATUS1) >>
						  guc->gpm_timestamp_shift;
		ae->active = lower_32_bits(gpm_ts) - cached_counter->last_update_tick;
	}

	/* quanta calculations */
	now = ktime_get();
	cpu_delta = now - ae->last_cpu_ts;
	ae->last_cpu_ts = now;
	numerator = (ae->quanta_remainder_ns + cpu_delta) * cached_counter->quanta_ratio;
	ae->quanta_ns += numerator / 0x8000;
	ae->quanta_remainder_ns = numerator % 0x8000;
	ae->quanta = cpu_ns_to_guc_tsc_tick(ae->quanta_ns, cached_meta->guc_tsc_frequency_hz);

	return ae->total + ae->active;
}

static ktime_t busy_v2_guc_engine_busyness(struct intel_engine_cs *engine,
					   unsigned int vf_id,
					   ktime_t *now)
{
	struct intel_gt *gt = engine->gt;
	struct intel_guc *guc = &gt->uc.guc;
	u64 ticks;

	if (now)
		*now = ktime_get();

	__busy_v2_get_engine_usage_record(guc, engine, GUC_BUSYNESS_VF_GLOBAL,
					  &ticks, NULL, NULL);

	if (now)
		*now += (ktime_get() - *now) >> 1;

	return intel_gt_clock_interval_to_ns(gt, ticks);
}

static u32 pmu_vfid_to_guc_vfid(unsigned int vf_id)
{
	/*
	 * PMU vf_id is VF# + 1, i.e. zero => global, 1 => PF, 2+ => VF 1+
	 * So substract 1 and ~0U => global, else it is the GuC VF#
	 * (where the PF is VF#0)
	 */

	if (vf_id > GUC_MAX_VF_COUNT)
		return GUC_MAX_VF_COUNT;

	return vf_id - 1;
}

static u64 busy_v2_guc_engine_busyness_ticks(struct intel_engine_cs *engine,
					     unsigned int vf_id)
{
	struct intel_guc *guc = &engine->gt->uc.guc;
	u64 ticks_engine;
	u32 guc_vf;

	guc_vf = pmu_vfid_to_guc_vfid(vf_id);
	if (guc_vf == GUC_MAX_VF_COUNT)
		return 0;

	__busy_v2_get_engine_usage_record(guc, engine, guc_vf, &ticks_engine, NULL, NULL);

	return ticks_engine;
}

static bool busy_v3_vf_id_valid(struct drm_i915_private *i915, unsigned int vf_id)
{
	struct pci_dev *pdev = to_pci_dev(i915->drm.dev);

	if (!IS_SRIOV(i915)) {
		if (vf_id)
			return false;
	} else if (vf_id >= (2 + pci_num_vf(pdev))) {
		return false;
	}

	return true;
}

static u64 busy_v3_guc_engine_activity_ticks(struct intel_engine_cs *engine,
					     unsigned int vf_id)
{
	struct intel_guc *guc = &engine->gt->uc.guc;
	struct drm_i915_private *i915 = engine->gt->i915;

	if (!busy_v3_vf_id_valid(i915, vf_id))
		return 0;

	return __busy_v3_get_engine_activity(guc, engine, vf_id);
}

static ktime_t busy_v3_guc_engine_busyness(struct intel_engine_cs *engine,
					   unsigned int vf_id,
					   ktime_t *now)
{
	u64 ticks;

	if (now)
		*now = ktime_get();

	ticks = busy_v3_guc_engine_activity_ticks(engine, vf_id);

	if (now)
		*now += (ktime_get() - *now) >> 1;

	return intel_gt_clock_interval_to_ns(engine->gt, ticks);
}

static u64 busy_v1_intel_guc_total_active_ticks(struct intel_engine_cs *engine,
						unsigned int vf_id)
{
	struct intel_guc *guc = &engine->gt->uc.guc;
	struct intel_gt *gt = guc_to_gt(guc);
	intel_wakeref_t wakeref;
	unsigned long flags;

	if (!guc_submission_initialized(guc))
		return 0;

	if (vf_id > 1) {
		/*
		 * VF specific counter is not available with v1 interface, but
		 * PF specific counter is available. Since 0 is global and 1 is
		 * PF, we support those values of vf_id here.
		 */
		return 0;
	}

	with_intel_gt_pm_if_awake(gt, wakeref) {
		spin_lock_irqsave(&guc->busy.lock, flags);
		busy_v1_guc_update_pm_timestamp(guc, NULL);
		spin_unlock_irqrestore(&guc->busy.lock, flags);
	}

	return guc->busy.v1.gt_stamp;
}

static u64 busy_v2_intel_guc_total_active_ticks(struct intel_engine_cs *engine,
						unsigned int vf_id)
{
	struct intel_guc *guc = &engine->gt->uc.guc;
	u64 ticks_function, ticks_gt;
	u32 guc_vf;

	if (!guc_submission_initialized(guc))
		return 0;

	guc_vf = pmu_vfid_to_guc_vfid(vf_id);
	if (guc_vf == GUC_MAX_VF_COUNT)
		return 0;

	__busy_v2_get_engine_usage_record(guc, NULL, guc_vf, NULL, &ticks_function, &ticks_gt);

	if (IS_SRIOV(guc_to_gt(guc)->i915))
		return ticks_function;

	return ticks_gt;
}

static u64 busy_v3_intel_guc_total_active_ticks(struct intel_engine_cs *engine,
						unsigned int vf_id)
{
	struct activity_engine *ae = to_activity_engine(engine, vf_id);

	busy_v3_guc_engine_activity_ticks(engine, vf_id);

	return ae->quanta;
}

/*
 * Provide total active ticks counter for backwards compatibility with busy v1.
 * This is just the gt timestamp and will only work on native/PF. For VF, this
 * will be 0. Note that this counter does not specifically rely on GuC, so we
 * just use the v1 helper.
 */
u64 intel_guc_total_active_ticks(struct intel_gt *gt, unsigned int vf_id)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;

	/* Get any engine that belongs to this gt */
	for_each_engine(engine, gt, id)
		break;

	return busy_v1_intel_guc_total_active_ticks(engine, vf_id);
}

static u64 __busy_v2_busy_free_ticks(struct intel_gt *gt, unsigned int vf_id, u32 counter)
{
	struct intel_guc *guc = &gt->uc.guc;
	struct iosys_map rec_map_global;
	u64 ticks_busy_free;
	int i = 0, ret;
	u32 guc_vf;

	if (!guc_submission_initialized(guc))
		return 0;

	guc_vf = pmu_vfid_to_guc_vfid(vf_id);
	if (guc_vf == GUC_MAX_VF_COUNT)
		return 0;

	ret = intel_guc_engine_usage_record_map_v2(guc, NULL, guc_vf, NULL,
						   &rec_map_global);
	if (ret)
		return 0;

#define record_read_global(map_, field_) \
	iosys_map_rd_field(map_, 0, struct guc_engine_observation_data, field_)

	do {
		ticks_busy_free = record_read_global(&rec_map_global, oag_busy_free_data[counter]);

		if (record_read_global(&rec_map_global, oag_busy_free_data[counter]) == ticks_busy_free)
			break;
	} while (++i < 6);

#undef record_read_global

	return ticks_busy_free;
}

static u64 busy_v2_busy_free_ticks(struct intel_gt *gt, u64 config, unsigned int vf_id)
{
	u64 val;

	switch (config) {
	case PRELIM_I915_PMU_RENDER_GROUP_BUSY:
	case PRELIM_I915_PMU_RENDER_GROUP_BUSY_TICKS:
		val = __busy_v2_busy_free_ticks(gt, vf_id, OAG_RENDER_BUSY_COUNTER_INDEX);
		break;
	case PRELIM_I915_PMU_COPY_GROUP_BUSY:
	case PRELIM_I915_PMU_COPY_GROUP_BUSY_TICKS:
		val = __busy_v2_busy_free_ticks(gt, vf_id, OAG_BLT_BUSY_COUNTER_INDEX);
		break;
	case PRELIM_I915_PMU_MEDIA_GROUP_BUSY:
	case PRELIM_I915_PMU_MEDIA_GROUP_BUSY_TICKS:
		val = __busy_v2_busy_free_ticks(gt, vf_id, OAG_ANY_MEDIA_FF_BUSY_COUNTER_INDEX);
		break;
	case PRELIM_I915_PMU_ANY_ENGINE_GROUP_BUSY:
	case PRELIM_I915_PMU_ANY_ENGINE_GROUP_BUSY_TICKS:
		val = __busy_v2_busy_free_ticks(gt, vf_id, OAG_RC0_ANY_ENGINE_BUSY_COUNTER_INDEX);
		break;
	default:
		MISSING_CASE(config);
		return 0;
	}

	/*
	 * These counters ignore some lower bits compared to standard timestamp
	 * TSC. Adjust for that using a multiplier.
	 */
	return val << 4;
}

static u64 busy_v2_busy_free_ns(struct intel_gt *gt, u64 config, unsigned int vf_id)
{
	u64 val = busy_v2_busy_free_ticks(gt, config, vf_id);

	return intel_gt_clock_interval_to_ns(gt, val);
}

void intel_guc_init_busy_free(struct intel_gt *gt)
{
	struct intel_guc *guc = &gt->uc.guc;

	if (!guc_submission_initialized(guc))
		return;

	/* v1 is implemented at i915_pmu level */
	if (busy_type_is_v1(guc)) {
		return;
	} else if (busy_type_is_v2(guc)) {
		gt->stats.busy_free = busy_v2_busy_free_ns;
		gt->stats.busy_free_ticks = busy_v2_busy_free_ticks;

		/*
		 * In busyness v2, a periodic timer updates the group busy counters, so
		 * we don't need to save the last value of the counter on gt park.
		 * Instead a query will fetch the latest value from the GuC interface.
		 */
		gt->stats.busy_free_park = NULL;
	} else if (busy_type_is_v3(guc)) {
		/*
		 * v3 does away with the support for busy free counters. User is
		 * supposed to use the single engine busyness to create groups
		 * and accumulate busy free data for a group.
		 *
		 * non-GuC related support (reading HW registers directly) is
		 * retained to avoid breaking existing uApi. This means that
		 * whatever worked on PF and Native will continue to work.
		 */
	}
}

static inline int __prepare_busy_v2_guc_action_enable_usage_stats_device(struct intel_guc *guc, u32 *action)
{
	u32 offset = guc_engine_usage_offset_v2_device(guc);
	int len = 0;

	action[len++] = INTEL_GUC_ACTION_SET_DEVICE_ENGINE_UTILIZATION_V2;
	action[len++] = offset;
	action[len++] = 0;

	return len;
}

static int busy_v2_guc_action_enable_usage_stats_device(struct intel_guc *guc)
{
	bool not_atomic = !in_atomic() && !rcu_preempt_depth() && !irqs_disabled();
	unsigned int sleep_period_us = 1;
	int srcu, len, err;
	u32 action[3];

	/* No sleeping with spin locks, just busy loop */
	might_sleep_if(not_atomic);

retry:
	err = gt_ggtt_address_read_lock_interruptible(guc_to_gt(guc), &srcu);
	if (unlikely(err))
		return err;

	len = __prepare_busy_v2_guc_action_enable_usage_stats_device(guc, action);

	GEM_BUG_ON(len > ARRAY_SIZE(action));

	err = intel_guc_send_nb(guc, action, len, 0);
	gt_ggtt_address_read_unlock(guc_to_gt(guc), srcu);
	if (unlikely(err == -EBUSY)) {
		intel_guc_send_wait(&sleep_period_us, not_atomic);
		goto retry;
	}

	return err;
}

static void busy_v3_set_activity_engine_cpu_ts(struct intel_guc *guc, u32 idx)
{
	struct activity_group *ag = &guc->busy.v3.ag[idx];
	int i, j;

	for (i = 0; i < GUC_MAX_ENGINE_CLASSES; i++)
		for (j = 0; j < GUC_MAX_INSTANCES_PER_CLASS; j++)
			ag->engine[i][j].last_cpu_ts = ktime_get();
}

static int
__prepare_busy_v3_guc_action_set_device_engine_activity(struct intel_guc *guc,
							u32 *action,
							bool enable)
{
	struct activity_buffer *ab = &guc->busy.v3.device;
	u32 activity_offset, metadata_offset;
	int len = 0;

	if (enable) {
		activity_offset = intel_guc_ggtt_offset(guc, ab->activity_vma);
		metadata_offset = intel_guc_ggtt_offset(guc, ab->metadata_vma);
	} else {
		activity_offset = 0;
		metadata_offset = 0;
	}

	action[len++] = INTEL_GUC_ACTION_SET_DEVICE_ENGINE_ACTIVITY_BUFFER;
	action[len++] = metadata_offset;
	action[len++] = 0;
	action[len++] = activity_offset;
	action[len++] = 0;

	return len;
}

static inline int
__prepare_busy_v3_guc_action_set_function_engine_activity(struct intel_guc *guc,
							  u32 *action,
							  bool enable)
{
	struct activity_buffer *ab = &guc->busy.v3.function;
	u32 activity_offset, metadata_offset, num_functions;
	int len = 0;

	if (enable) {
		activity_offset = intel_guc_ggtt_offset(guc, ab->activity_vma);
		metadata_offset = intel_guc_ggtt_offset(guc, ab->metadata_vma);
		num_functions = guc->busy.v3.num_functions;
	} else {
		activity_offset = 0;
		metadata_offset = 0;
		num_functions = 0;
	}

	action[len++] = INTEL_GUC_ACTION_SET_FUNCTION_ENGINE_ACTIVITY_BUFFER;
	action[len++] = num_functions;
	action[len++] = metadata_offset;
	action[len++] = 0;
	action[len++] = activity_offset;
	action[len++] = 0;

	return len;
}

static int busy_v3_guc_action_set_engine_activity(struct intel_guc *guc,
						  bool is_device, bool enable)
{
	u32 action[6];
	int len;

	if (is_device)
		len = __prepare_busy_v3_guc_action_set_device_engine_activity(guc,
									      action, enable);
	else
		len = __prepare_busy_v3_guc_action_set_function_engine_activity(guc,
										action, enable);

	GEM_BUG_ON(len > ARRAY_SIZE(action));

	return intel_guc_send(guc, action, ARRAY_SIZE(action));
}

static int busy_v2_guc_action_enable_usage_stats_function(struct intel_guc *guc)
{
	u32 offset = intel_guc_engine_usage_offset_global(guc);
	u32 action[] = {
		INTEL_GUC_ACTION_SET_FUNCTION_ENGINE_UTILIZATION_V2,
		offset,
		0,
	};

	return intel_guc_send(guc, action, ARRAY_SIZE(action));
}

/**
 * intel_guc_enable_activity_stats_functions - Enable function activity stats
 * @guc: intel_guc struct
 * @num_vfs: num of vfs
 *
 * Enable v3 engine activity stats for pf and vfs
 *
 * Return: 0 on success, negative error code otherwise
 */
int intel_guc_enable_activity_stats_functions(struct intel_guc *guc, int num_vfs)
{
	int ret, i;

	if (!busy_type_is_v3(guc))
		return 0;

	guc->busy.v3.num_functions = num_vfs + 1;

	ret = guc_busy_v3_alloc_function_array(guc);
	if (ret)
		return ret;

	ret = busy_v3_guc_action_set_engine_activity(guc, false, true);
	if (ret) {
		guc_busy_v3_free_function_array(guc);
		guc->busy.v3.num_functions = 0;
		return ret;
	}

	for (i = 0; i < guc->busy.v3.num_functions; i++)
		busy_v3_set_activity_engine_cpu_ts(guc, i + 1);

	return ret;
}

/**
 * intel_guc_disable_activity_stats_functions - disable function activity stats
 * @guc: intel_guc struct
 *
 * Disable v3 engine activity stats for pf and vfs
 *
 * Return: 0 on success, negative error code otherwise
 */
int intel_guc_disable_activity_stats_functions(struct intel_guc *guc)
{
	int ret;

	if (!busy_type_is_v3(guc))
		return 0;

	ret = busy_v3_guc_action_set_engine_activity(guc, false, false);

	guc_busy_v3_free_function_array(guc);

	guc->busy.v3.num_functions = 0;

	return ret;
}

/**
 * intel_guc_reset_activity_stats_functions - reset activity stats
 * @guc: intel_guc struct
 *
 * reset engine activity stats for pf and vfs
 *
 * Return: 0 on success, negative error code otherwise
 */
int intel_guc_reset_activity_stats_functions(struct intel_guc *guc)
{
	int ret = 0;

	if (!busy_type_is_v3(guc))
		return 0;

	if (!guc->busy.v3.num_functions)
		return 0;

	ret = busy_v3_guc_action_set_engine_activity(guc, false, false);
	if (ret)
		return ret;

	ret = busy_v3_guc_action_set_engine_activity(guc, false, true);
	if (ret)
		return  busy_v3_guc_action_set_engine_activity(guc, false, false);

	return ret;
}

static int guc_init_engine_stats(struct intel_guc *guc)
{
	struct intel_gt *gt = guc_to_gt(guc);
	intel_wakeref_t wakeref;
	int ret = 0;

	if (busy_type_is_v1(guc)) {
		if (!IS_SRIOV_VF(gt->i915)) {
			with_intel_gt_pm(gt, wakeref)
				ret = busy_v1_guc_action_enable_usage_stats(guc);

			if (ret == 0)
				busy_v1_guc_enable_worker(guc);
		}

		if (ret)
			guc_probe_error(guc, "Failed to enable v1 usage stats: %pe\n",
					ERR_PTR(ret));

	} else if (busy_type_is_v2(guc)) {
		with_intel_gt_pm(gt, wakeref) {
			ret = busy_v2_guc_action_enable_usage_stats_device(guc);
			if (ret == 0 && !IS_SRIOV_VF(gt->i915))
				ret = busy_v2_guc_action_enable_usage_stats_function(guc);
		}
		if (ret)
			guc_probe_error(guc, "Failed to enable v2 usage stats: %pe\n",
					ERR_PTR(ret));
	} else if (busy_type_is_v3(guc)) {
		with_intel_gt_pm(gt, wakeref)
			ret = busy_v3_guc_action_set_engine_activity(guc, true, true);

		if (ret)
			guc_probe_error(guc, "Failed to enable v3 usage stats: %pe\n",
					ERR_PTR(ret));
		else
			busy_v3_set_activity_engine_cpu_ts(guc, 0);
	}

	return ret;
}

static void guc_fini_engine_stats(struct intel_guc *guc)
{
	busy_v1_guc_cancel_worker(guc);
}

void intel_guc_busyness_park(struct intel_gt *gt)
{
	struct intel_guc *guc = &gt->uc.guc;

	if (IS_SRIOV_VF(gt->i915))
		return;

	if (!guc_submission_initialized(guc))
		return;

	if (busy_type_is_v1(guc)) {
		busy_v1_guc_cancel_worker(guc);

		/*
		 * Before parking, we should sample engine busyness stats if we need to.
		 * We can skip it if we are less than half a ping from the last time we
		 * sampled the busyness stats.
		 */
		if (guc->busy.v1.last_stat_jiffies &&
		    !time_after(jiffies, guc->busy.v1.last_stat_jiffies +
				(guc->busy.v1.ping_delay / 2)))
			return;

		__busy_v1_update_guc_busyness_stats(guc);
	}
}

void intel_guc_busyness_unpark(struct intel_gt *gt)
{
	struct intel_guc *guc = &gt->uc.guc;

	if (IS_SRIOV_VF(gt->i915))
		return;

	if (!guc_submission_initialized(guc))
		return;

	if (busy_type_is_v1(guc)) {
		busy_v1_guc_enable_worker(guc);
	}
}

static inline bool
submission_disabled(struct intel_guc *guc)
{
	struct i915_sched_engine * const sched_engine = guc->sched_engine;

	return unlikely(!sched_engine ||
			!__tasklet_is_enabled(&sched_engine->tasklet) ||
			intel_gt_is_wedged(guc_to_gt(guc)));
}

static void disable_submission(struct intel_guc *guc)
{
	struct i915_sched_engine * const sched_engine = guc->sched_engine;

	if (__tasklet_is_enabled(&sched_engine->tasklet)) {
		GEM_BUG_ON(!guc->ct.enabled);
		__tasklet_disable_sync_once(&sched_engine->tasklet);
		set_tasklet_fn(&sched_engine->tasklet, nop_submission_tasklet);

	}
}

static bool
__enable_submission_tasklet(struct i915_sched_engine * const sched_engine)
{
	return !__tasklet_is_enabled(&sched_engine->tasklet) &&
	       __tasklet_enable(&sched_engine->tasklet);
}

static void enable_submission(struct intel_guc *guc)
{
	struct i915_sched_engine * const sched_engine = guc->sched_engine;

	set_tasklet_fn(&sched_engine->tasklet, guc_submission_tasklet);
	smp_wmb();	/* Make sure callback visible */

	if (__enable_submission_tasklet(sched_engine))
		GEM_BUG_ON(!guc->ct.enabled);

	/* And kick in case we missed a new request submission. */
	tasklet_hi_schedule(&sched_engine->tasklet);
}

static void guc_flush_submissions(struct intel_guc *guc)
{
	struct i915_sched_engine * const sched_engine = guc->sched_engine;
	unsigned long flags;

	spin_lock_irqsave(&sched_engine->lock, flags);
	spin_unlock_irqrestore(&sched_engine->lock, flags);
}

static void guc_flush_destroyed_contexts(struct intel_guc *guc);

static struct i915_request *
__i915_sched_rewind_requests(struct i915_sched_engine *se,
			     intel_engine_mask_t stalled)
{
	struct i915_request *rq, *rn, *active = NULL;
	u64 prio = I915_PRIORITY_INVALID;
	struct list_head *pl;

	lockdep_assert_held(&se->lock);

	list_for_each_entry_safe_reverse(rq, rn, &se->requests, sched.link) {
		if (__i915_request_is_complete(rq)) {
			list_del_init(&rq->sched.link);
			continue;
		}

		__i915_request_unsubmit(rq);

		if (__i915_request_has_started(rq)) {
			struct intel_context *ce = rq->context;
			u32 head = rq->infix;
			int srcu;

			__i915_request_reset(rq, rq->execution_mask & stalled);
			gt_ggtt_address_read_lock(rq->engine->gt, &srcu);
			if (rq->execution_mask & stalled) {
				lrc_init_regs(ce, rq->engine, true);
				head = rq->postfix;
			}
			ce->lrc.lrca = lrc_update_regs(ce, rq->engine, intel_ring_wrap(ce->ring, head));
			gt_ggtt_address_read_unlock(rq->engine->gt, srcu);
		}

		GEM_BUG_ON(rq_prio(rq) == I915_PRIORITY_INVALID);
		if (rq_prio(rq) != prio) {
			prio = rq_prio(rq);
			pl = i915_sched_lookup_priolist(se, prio);
		}
		GEM_BUG_ON(i915_request_in_priority_queue(rq));
		list_move(&rq->sched.link, pl);
		set_bit(I915_FENCE_FLAG_PQUEUE, &rq->fence.flags);

		active = rq;
	}

	return active;
}

void intel_guc_submission_reset_prepare(struct intel_guc *guc)
{
	if (unlikely(!guc_submission_initialized(guc))) {
		/* Reset called during driver load? GuC not yet initialised! */
		return;
	}

	intel_gt_park_heartbeats(guc_to_gt(guc));
	disable_submission(guc);
	guc->interrupts.disable(guc);

	if (busy_type_is_v1(guc))
		if (!IS_SRIOV_VF(guc_to_gt(guc)->i915))
			__busy_v1_reset_guc_busyness_stats(guc);

	guc_flush_submissions(guc);
	guc_flush_destroyed_contexts(guc);
}

static void guc_submission_refresh_request_ring_content(struct i915_request *rq)
{
	u32 rhead, remit, rspace;
	int err;

	if (!test_bit(I915_FENCE_FLAG_GGTT_EMITTED, &rq->fence.flags))
		return;

	/*
	 * Pretend we have an empty, uninitialized request, being added at
	 * end of the ring. This allows us to re-use the emit callbacks,
	 * despite them being designed for exec only during request creation.
	 */
	rhead = rq->ring->head;
	remit = rq->ring->emit;
	rspace = rq->ring->space;
	rq->ring->emit = get_init_breadcrumb_pos(rq);
	rq->ring->head = rq->head;
	intel_ring_update_space(rq->ring);
	rq->reserved_space =
		2 * rq->engine->emit_fini_breadcrumb_dw * sizeof(u32);

	err = reemit_init_breadcrumb(rq);
	if (err)
		DRM_DEBUG_DRIVER("Request prefix ring content not recognized, fence %llx:%lld, err=%pe\n",
				 rq->fence.context, rq->fence.seqno, ERR_PTR(err));

	err = reemit_bb_start(rq);
	if (err)
		DRM_DEBUG_DRIVER("Request infix ring content not recognized, fence %llx:%lld, err=%pe\n",
				 rq->fence.context, rq->fence.seqno, ERR_PTR(err));

	rq->ring->head = rhead;
	rq->ring->emit = remit;
	rq->ring->space = rspace;
	rq->reserved_space = 0;

	if (test_bit(I915_FENCE_FLAG_ACTIVE, &rq->fence.flags))
		rq->engine->emit_fini_breadcrumb(rq, rq->ring->vaddr + rq->postfix);
}

static void guc_submission_noop_request_ring_content(struct i915_request *rq)
{
	ring_range_emit_noop(rq->ring, rq->head, rq->tail);
}

void guc_submission_refresh_ctx_rings_content(struct intel_context *ce)
{
	struct intel_timeline *tl;
	struct i915_request *rq;

	if (unlikely(!test_bit(CONTEXT_ALLOC_BIT, &ce->flags)))
		return;

	tl = ce->timeline;

	list_for_each_entry_rcu(rq, &tl->requests, link) {
		if (i915_request_completed(rq))
			guc_submission_noop_request_ring_content(rq);
		else
			guc_submission_refresh_request_ring_content(rq);
	}
}

/*
 * guc_submission_unwind_all - stop waiting for unfinished requests,
 *    add them back to scheduled requests list instead
 *
 * If hardware reset, or migration, prevents any submitted requests from
 * completing, this function can be used to un-submit the requests in
 * flight, and schedule them to be later submitted again.
 */
static void
guc_submission_unwind_all(struct intel_guc *guc, intel_engine_mask_t stalled)
{
	struct i915_sched_engine * const se = guc->sched_engine;
	unsigned long flags;

	spin_lock_irqsave(&se->lock, flags);
	__i915_sched_rewind_requests(se, stalled);
	spin_unlock_irqrestore(&se->lock, flags);
}

/*
 * intel_guc_submission_pause - temporarily stop GuC submission mechanics
 * @guc: intel_guc struct instance for the target tile
 */
void intel_guc_submission_pause(struct intel_guc *guc)
{
	struct i915_sched_engine * const sched_engine = guc->sched_engine;

	tasklet_disable_nosync(&sched_engine->tasklet);
}

/*
 * intel_guc_submission_restore - unpause GuC submission mechanics
 * @guc: intel_guc struct instance for the target tile
 */
void intel_guc_submission_restore(struct intel_guc *guc)
{
	/*
	 * If the submissions were only paused, there should be no need
	 * to perform all the enabling operations; but since other threads
	 * could have disabled the submissions fully, we need a full enable.
	 */
	enable_submission(guc);
}

static struct intel_engine_cs *
guc_virtual_get_sibling(struct intel_engine_cs *ve, unsigned int sibling)
{
	struct intel_engine_cs *engine;
	intel_engine_mask_t tmp, mask = ve->mask;
	unsigned int num_siblings = 0;

	for_each_engine_masked(engine, ve->gt, mask, tmp)
		if (num_siblings++ == sibling)
			return engine;

	return NULL;
}

static inline struct intel_engine_cs *
__context_to_physical_engine(struct intel_context *ce)
{
	struct intel_engine_cs *engine = ce->engine;

	if (intel_engine_is_virtual(engine))
		engine = guc_virtual_get_sibling(engine, 0);

	return engine;
}

static void guc_reset_state(struct intel_context *ce, u32 head, bool scrub)
{
	struct intel_engine_cs *engine = __context_to_physical_engine(ce);
	int srcu;

	if (intel_context_is_banned(ce))
		return;

	GEM_BUG_ON(!intel_context_is_pinned(ce));

	gt_ggtt_address_read_lock(ce->engine->gt, &srcu);
	/*
	 * We want a simple context + ring to execute the breadcrumb update.
	 * We cannot rely on the context being intact across the GPU hang,
	 * so clear it and rebuild just what we need for the breadcrumb.
	 * All pending requests for this context will be zapped, and any
	 * future request will be after userspace has had the opportunity
	 * to recreate its own state.
	 */
	if (scrub)
		lrc_init_regs(ce, engine, true);

	/* Rerun the request; its payload has been neutered (if guilty). */
	ce->lrc.lrca = lrc_update_regs(ce, engine, head);
	gt_ggtt_address_read_unlock(ce->engine->gt, srcu);
}

static void guc_engine_reset_prepare(struct intel_engine_cs *engine)
{
	if (!IS_GRAPHICS_VER(engine->i915, 11, 12))
		return;

	intel_engine_stop_cs(engine);

	/*
	 * Wa_22011802037: In addition to stopping the cs, we need
	 * to wait for any pending mi force wakeups
	 */
	intel_engine_wait_for_pending_mi_fw(engine);
}

static void guc_reset_nop(struct intel_engine_cs *engine)
{
}

static void guc_rewind_nop(struct intel_engine_cs *engine, bool stalled)
{
}

static void
__unwind_incomplete_requests(struct intel_context *ce)
{
	struct i915_sched_engine * const sched_engine = ce->engine->sched_engine;
	int prio = I915_PRIORITY_INVALID;
	struct i915_request *rq;
	struct list_head *pl;
	unsigned long flags;

	spin_lock_irqsave(&sched_engine->lock, flags);
	list_for_each_entry_reverse(rq, &ce->timeline->requests, link) {
		if (__i915_request_is_complete(rq))
			break;

		if (!i915_request_is_active(rq))
			continue;

		__i915_request_unsubmit(rq);

		GEM_BUG_ON(rq_prio(rq) == I915_PRIORITY_INVALID);
		if (rq_prio(rq) != prio) {
			prio = rq_prio(rq);
			pl = i915_sched_lookup_priolist(sched_engine, prio);
		}

		GEM_BUG_ON(i915_request_in_priority_queue(rq));
		list_move(&rq->sched.link, pl);
		set_bit(I915_FENCE_FLAG_PQUEUE, &rq->fence.flags);
	}
	spin_unlock_irqrestore(&sched_engine->lock, flags);
}

static void __guc_reset_context(struct intel_context *ce, intel_engine_mask_t stalled)
{
	bool guilty;
	struct i915_request *rq;
	unsigned long flags;
	u32 head;
	int i, number_children = ce->parallel.number_children;
	bool skip = false;
	struct intel_context *parent = ce;

	GEM_BUG_ON(intel_context_is_child(ce));

	intel_context_get(ce);

	/*
	 * GuC will implicitly mark the context as non-schedulable when it sends
	 * the reset notification. Make sure our state reflects this change. The
	 * context will be marked enabled on resubmission.
	 *
	 * XXX: If the context is reset as a result of the request cancellation
	 * this G2H is received after the schedule disable complete G2H which is
	 * wrong as this creates a race between the request cancellation code
	 * re-submitting the context and this G2H handler. This is a bug in the
	 * GuC but can be worked around in the meantime but converting this to a
	 * NOP if a pending enable is in flight as this indicates that a request
	 * cancellation has occurred.
	 */
	spin_lock_irqsave(&ce->guc_state.lock, flags);
	if (likely(!context_pending_enable(ce)))
		clr_context_enabled(ce);
	else
		skip = true;
	spin_unlock_irqrestore(&ce->guc_state.lock, flags);
	if (unlikely(skip))
		goto out_put;

	/*
	 * For each context in the relationship find the hanging request
	 * resetting each context / request as needed
	 */
	for (i = 0; i < number_children + 1; ++i) {
		if (!intel_context_is_pinned(ce))
			goto next_context;

		guilty = false;
		rq = intel_context_find_active_request(ce);
		if (!rq) {
			head = ce->ring->tail;
			goto out_replay;
		}

		if (__i915_request_has_started(rq))
			guilty = stalled & rq->execution_mask;

		GEM_BUG_ON(i915_active_is_idle(&ce->active));
		head = intel_ring_wrap(ce->ring, rq->head);

		__i915_request_reset(rq, guilty);
out_replay:
		guc_reset_state(ce, head, guilty);
next_context:
		if (i != number_children)
			ce = list_next_entry(ce, parallel.child_link);
	}

	__unwind_incomplete_requests(parent);
out_put:
	intel_context_put(parent);
}

static void clear_context_state(struct intel_guc *guc)
{
	scrub_guc_desc_for_outstanding_g2h(guc);
	while (atomic_read(&guc->outstanding_submission_g2h) > 0)
		decr_outstanding_submission_g2h(guc);
	wake_up_all(&guc->ct.wq);

	/* GuC is blown away, drop all references to contexts */
	xa_destroy(&guc->context_lookup);
}

void intel_guc_submission_reset(struct intel_guc *guc, intel_engine_mask_t stalled)
{
	if (unlikely(!guc_submission_initialized(guc))) {
		/* Reset called during driver load? GuC not yet initialised! */
		return;
	}

	clear_context_state(guc);
	guc_submission_unwind_all(guc, stalled);
}

static void guc_cancel_context_requests(struct intel_context *ce)
{
	struct intel_guc *guc = ce_to_guc(ce);
	struct i915_sched_engine *se = ce->engine->sched_engine;
	struct i915_request *rq;
	unsigned long flags;
	bool retire = false;

	spin_lock_irqsave(&se->lock, flags);
	list_for_each_entry(rq, &se->requests, sched.link) {
		if (rq->context != ce)
			continue;

		if (rq->sched.semaphores &&
		    !i915_sw_fence_signaled(&rq->semaphore))
			break;

		if (rq == guc->stalled_request)
			guc->stalled_request = NULL;

		if (i915_request_mark_eio(rq)) {
			i915_request_put(rq);
			retire = true;
		}
	}
	spin_unlock_irqrestore(&se->lock, flags);

	if (retire)
		intel_engine_add_retire(__context_to_physical_engine(ce),
					ce->timeline);
}

static void
guc_cancel_sched_engine_requests(struct i915_sched_engine *sched_engine)
{
	struct i915_request *rq, *rn;
	struct rb_node *rb;
	unsigned long flags;

	/* Can be called during boot if GuC fails to load */
	if (!sched_engine)
		return;

	/*
	 * Before we call engine->cancel_requests(), we should have exclusive
	 * access to the submission state. This is arranged for us by the
	 * caller disabling the interrupt generation, the tasklet and other
	 * threads that may then access the same state, giving us a free hand
	 * to reset state. However, we still need to let lockdep be aware that
	 * we know this state may be accessed in hardirq context, so we
	 * disable the irq around this manipulation and we want to keep
	 * the spinlock focused on its duties and not accidentally conflate
	 * coverage to the submission's irq state. (Similarly, although we
	 * shouldn't need to disable irq around the manipulation of the
	 * submission's irq state, we also wish to remind ourselves that
	 * it is irq state.)
	 */
	spin_lock_irqsave(&sched_engine->lock, flags);

	list_for_each_entry(rq, &sched_engine->requests, sched.link)
		i915_request_put(i915_request_mark_eio(rq));

	/* Flush the queued requests to the timeline list (for retiring). */
	while ((rb = rb_first_cached(&sched_engine->queue))) {
		struct i915_priolist *p = to_priolist(rb);

		priolist_for_each_request_consume(rq, rn, p) {
			if (i915_request_mark_eio(rq)) {
				__i915_request_submit(rq);
				i915_request_put(rq);
			}
		}

		rb_erase_cached(&p->node, &sched_engine->queue);
		i915_priolist_free(p);
	}

	/* Remaining _unready_ requests will be nop'ed when submitted */

	sched_engine->queue_priority_hint = INT_MIN;
	sched_engine->queue = RB_ROOT_CACHED;

	spin_unlock_irqrestore(&sched_engine->lock, flags);
}

void intel_guc_submission_cancel_requests(struct intel_guc *guc)
{
	guc_cancel_sched_engine_requests(guc->sched_engine);
	clear_context_state(guc);
}

void intel_guc_submission_reset_finish(struct intel_guc *guc)
{
	/* Reset called during driver load */
	if (unlikely(!guc_submission_initialized(guc)))
		return;

	/*
	 * if the device is wedged, we still need to re-enable the tasklet to
	 * allow for it to run, otherwise it won't be killable if there is a
	 * pending scheduled run.
	 */
	if (intel_gt_is_wedged(guc_to_gt(guc)) || !intel_guc_is_fw_running(guc)) {
		set_tasklet_fn(&guc->sched_engine->tasklet, nop_submission_tasklet);
		smp_wmb(); /* Make sure callback visible */
		__enable_submission_tasklet(guc->sched_engine);
		return;
	}

	intel_guc_global_policies_update(guc);
	enable_submission(guc);
	intel_gt_unpark_heartbeats(guc_to_gt(guc));

	if (waitqueue_active(&guc->ct.wq))
		wake_up_all(&guc->ct.wq);
}

static void destroyed_worker_func(struct work_struct *w);
static int number_mlrc_guc_id(struct intel_guc *guc);

/*
 * Set up the memory resources to be shared with the GuC (via the GGTT)
 * at firmware loading time.
 */
int intel_guc_submission_init(struct intel_guc *guc)
{
	struct intel_gt *gt = guc_to_gt(guc);
	int ret;

	if (guc->submission_initialized)
		return 0;

	if (GUC_SUBMIT_VER(guc) < MAKE_GUC_VER(1, 0, 0)) {
		ret = guc_lrc_desc_pool_create_v69(guc);
		if (ret)
			return ret;
	}

	guc->submission_state.guc_ids_bitmap =
		bitmap_zalloc(number_mlrc_guc_id(guc), GFP_KERNEL);
	if (!guc->submission_state.guc_ids_bitmap) {
		ret = -ENOMEM;
		goto destroy_pool;
	}

	guc->gpm_timestamp_shift = gpm_timestamp_shift(gt);
	if (busy_type_is_v1(guc)) {
		guc->busy.v1.ping_delay = (BUSY_V1_POLL_TIME_CLKS / gt->clock_frequency + 1) * HZ;
	} else if (busy_type_is_v2(guc)) {
		ret = guc_busy_v2_alloc_device(guc);
		if (ret)
			goto destroy_bitmap;
	} else if (busy_type_is_v3(guc)) {
		ret = guc_busy_v3_alloc_activity_groups(guc);
		if (ret)
			goto destroy_bitmap;

		ret = guc_busy_v3_alloc_activity_data(guc, &guc->busy.v3.device, 1);
		if (ret)
			goto destroy_activity_groups;

		ret = guc_busy_v3_alloc_metadata(guc, &guc->busy.v3.device, 1);
		if (ret)
			goto destroy_activity_data;
	}

	guc->submission_initialized = true;

	return 0;
destroy_activity_data:
	guc_busy_v3_free_activity_data(guc, &guc->busy.v3.device);
destroy_activity_groups:
	kfree(guc->busy.v3.ag);
destroy_bitmap:
	bitmap_free(guc->submission_state.guc_ids_bitmap);
destroy_pool:
	guc_lrc_desc_pool_destroy_v69(guc);
	return ret;
}

void intel_guc_submission_fini(struct intel_guc *guc)
{
	if (!guc->submission_initialized)
		return;

	guc_flush_destroyed_contexts(guc);
	guc_lrc_desc_pool_destroy_v69(guc);
	i915_sched_engine_put(fetch_and_zero(&guc->sched_engine));
	bitmap_free(guc->submission_state.guc_ids_bitmap);
	if (busy_type_is_v2(guc)) {
		guc_busy_v2_free_device(guc);
	} else if (busy_type_is_v3(guc)) {
		guc_busy_v3_free_activity_data(guc, &guc->busy.v3.device);
		guc_busy_v3_free_metadata(guc, &guc->busy.v3.device);
		kfree(guc->busy.v3.ag);
	}
	guc->submission_initialized = false;
}

static inline void queue_request(struct i915_sched_engine *sched_engine,
				 struct i915_request *rq,
				 int prio)
{
	GEM_BUG_ON(!list_empty(&rq->sched.link));
	list_add_tail(&rq->sched.link,
		      i915_sched_lookup_priolist(sched_engine, prio));
	set_bit(I915_FENCE_FLAG_PQUEUE, &rq->fence.flags);
	tasklet_hi_schedule(&sched_engine->tasklet);
}

static int guc_bypass_tasklet_submit(struct intel_guc *guc,
				     struct i915_request *rq)
{
	int ret = 0;

	__i915_request_submit(rq);
	add_to_context(rq);

	if (is_multi_lrc_rq(rq)) {
		if (multi_lrc_submit(rq)) {
			ret = guc_wq_item_append(guc, rq);
			if (!ret)
				ret = guc_add_request(guc, rq);
		}
	} else {
		guc_set_lrc_tail(rq);
		ret = guc_add_request(guc, rq);
	}

	if (unlikely(ret == -EPIPE))
		disable_submission(guc);

	return ret;
}

static bool need_tasklet(struct intel_guc *guc, struct i915_request *rq)
{
	struct i915_sched_engine *sched_engine = rq->sched_engine;
	struct intel_context *ce = request_to_scheduling_context(rq);

	return submission_disabled(guc) || guc->stalled_request ||
		!i915_sched_engine_is_empty(sched_engine) ||
		!ctx_id_mapped(guc, ce->guc_id.id);
}

static void guc_submit_request(struct i915_request *rq)
{
	struct i915_sched_engine *sched_engine = rq->sched_engine;
	struct intel_guc *guc = &rq->engine->gt->uc.guc;
	unsigned long flags;

	/* Will be called from irq-context when using foreign fences. */
	spin_lock_irqsave(&sched_engine->lock, flags);

	if (need_tasklet(guc, rq))
		queue_request(sched_engine, rq, rq_prio(rq));
	else if (guc_bypass_tasklet_submit(guc, rq) == -EBUSY)
		tasklet_hi_schedule(&sched_engine->tasklet);

	spin_unlock_irqrestore(&sched_engine->lock, flags);
}

/*
 * We reserve 1/16 of the guc_ids for multi-lrc as these need to be contiguous
 * per the GuC submission interface. A different allocation algorithm is used
 * (bitmap vs. ida) between multi-lrc and single-lrc hence the reason to
 * partition the guc_id space. We believe the number of multi-lrc contexts in
 * use should be low and 1/16 should be sufficient.
 */
#define MLRC_GUC_ID_RATIO	16

static int number_mlrc_guc_id(struct intel_guc *guc)
{
	return guc->submission_state.num_guc_ids / MLRC_GUC_ID_RATIO;
}

static int number_slrc_guc_id(struct intel_guc *guc)
{
	return guc->submission_state.num_guc_ids - number_mlrc_guc_id(guc);
}

static int mlrc_guc_id_base(struct intel_guc *guc)
{
	return number_slrc_guc_id(guc);
}

static int new_mlrc_guc_id(struct intel_guc *guc, struct intel_context *ce)
{
	int ret;

	GEM_BUG_ON(!intel_context_is_parent(ce));
	GEM_BUG_ON(!guc->submission_state.guc_ids_bitmap);

	ret =  bitmap_find_free_region(guc->submission_state.guc_ids_bitmap,
				       number_mlrc_guc_id(guc),
				       order_base_2(ce->parallel.number_children
						    + 1));
	if (unlikely(ret < 0))
		return ret;

	return ret + mlrc_guc_id_base(guc);
}

static int new_slrc_guc_id(struct intel_guc *guc, struct intel_context *ce)
{
	GEM_BUG_ON(intel_context_is_parent(ce));

	return ida_simple_get(&guc->submission_state.guc_ids,
			      0, number_slrc_guc_id(guc),
			      I915_GFP_ALLOW_FAIL);
}

int intel_guc_submission_limit_ids(struct intel_guc *guc, u32 limit)
{
	if (limit > GUC_MAX_CONTEXT_ID)
		return -E2BIG;

	if (!ida_is_empty(&guc->submission_state.guc_ids))
		return -ETXTBSY;

	guc->submission_state.num_guc_ids = limit;
	return 0;
}

static int new_guc_id(struct intel_guc *guc, struct intel_context *ce)
{
	int ret;

	GEM_BUG_ON(intel_context_is_child(ce));

	if (intel_context_is_parent(ce))
		ret = new_mlrc_guc_id(guc, ce);
	else
		ret = new_slrc_guc_id(guc, ce);

	if (unlikely(ret < 0))
		return ret;

	if (!intel_context_is_parent(ce))
		++guc->submission_state.guc_ids_in_use;

	ce->guc_id.id = ret;
	return 0;
}

static void __release_guc_id(struct intel_guc *guc, struct intel_context *ce)
{
	GEM_BUG_ON(intel_context_is_child(ce));

	if (!context_guc_id_invalid(ce)) {
		if (intel_context_is_parent(ce)) {
			bitmap_release_region(guc->submission_state.guc_ids_bitmap,
					      ce->guc_id.id - mlrc_guc_id_base(guc),
					      order_base_2(ce->parallel.number_children
							   + 1));
		} else {
			--guc->submission_state.guc_ids_in_use;
			ida_simple_remove(&guc->submission_state.guc_ids,
					  ce->guc_id.id);
		}
		clr_ctx_id_mapping(guc, ce->guc_id.id);
		set_context_guc_id_invalid(ce);
	}
	if (!list_empty(&ce->guc_id.link))
		list_del_init(&ce->guc_id.link);
}

static void release_guc_id(struct intel_guc *guc, struct intel_context *ce)
{
	unsigned long flags;

	spin_lock_irqsave(&guc->submission_state.lock, flags);
	__release_guc_id(guc, ce);
	spin_unlock_irqrestore(&guc->submission_state.lock, flags);
}

static int steal_guc_id(struct intel_guc *guc, struct intel_context *ce)
{
	struct intel_context *cn;

	lockdep_assert_held(&guc->submission_state.lock);
	GEM_BUG_ON(intel_context_is_child(ce));
	GEM_BUG_ON(intel_context_is_parent(ce));

	if (!list_empty(&guc->submission_state.guc_id_list)) {
		cn = list_first_entry(&guc->submission_state.guc_id_list,
				      struct intel_context,
				      guc_id.link);

		GEM_BUG_ON(atomic_read(&cn->guc_id.ref));
		GEM_BUG_ON(context_guc_id_invalid(cn));
		GEM_BUG_ON(intel_context_is_child(cn));
		GEM_BUG_ON(intel_context_is_parent(cn));

		list_del_init(&cn->guc_id.link);
		ce->guc_id.id = cn->guc_id.id;

		spin_lock(&cn->guc_state.lock);
		clr_context_registered(cn);
		spin_unlock(&cn->guc_state.lock);

		set_context_guc_id_invalid(cn);

#ifdef CPTCFG_DRM_I915_SELFTEST
		guc->number_guc_id_stolen++;
#endif

		return 0;
	} else {
		return -EAGAIN;
	}
}

static int assign_guc_id(struct intel_guc *guc, struct intel_context *ce)
{
	int ret;

	lockdep_assert_held(&guc->submission_state.lock);
	GEM_BUG_ON(intel_context_is_child(ce));

	ret = new_guc_id(guc, ce);
	if (unlikely(ret < 0)) {
		if (intel_context_is_parent(ce))
			return -ENOSPC;

		ret = steal_guc_id(guc, ce);
		if (ret < 0)
			return ret;
	}

	if (intel_context_is_parent(ce)) {
		struct intel_context *child;
		int i = 1;

		for_each_child(ce, child)
			child->guc_id.id = ce->guc_id.id + i++;
	}

	return 0;
}

#define PIN_GUC_ID_TRIES	4
static int pin_guc_id(struct intel_guc *guc, struct intel_context *ce)
{
	int ret = 0;
	unsigned long flags, tries = PIN_GUC_ID_TRIES;

	GEM_BUG_ON(atomic_read(&ce->guc_id.ref));

try_again:
	spin_lock_irqsave(&guc->submission_state.lock, flags);

	might_lock(&ce->guc_state.lock);

	if (context_guc_id_invalid(ce)) {
		ret = assign_guc_id(guc, ce);
		if (ret)
			goto out_unlock;
		ret = 1;	/* Indidcates newly assigned guc_id */
	}
	if (!list_empty(&ce->guc_id.link))
		list_del_init(&ce->guc_id.link);
	atomic_inc(&ce->guc_id.ref);

out_unlock:
	spin_unlock_irqrestore(&guc->submission_state.lock, flags);

	/*
	 * -EAGAIN indicates no guc_id are available, let's retire any
	 * outstanding requests to see if that frees up a guc_id. If the first
	 * retire didn't help, insert a sleep with the timeslice duration before
	 * attempting to retire more requests. Double the sleep period each
	 * subsequent pass before finally giving up. The sleep period has max of
	 * 100ms and minimum of 1ms.
	 */
	if (ret == -EAGAIN && --tries) {
		if (PIN_GUC_ID_TRIES - tries > 1) {
			unsigned int timeslice_shifted =
				ce->schedule_policy.timeslice_duration_ms <<
				(PIN_GUC_ID_TRIES - tries - 2);
			unsigned int max = min_t(unsigned int, 100,
						 timeslice_shifted);

			msleep(max_t(unsigned int, max, 1));
		}
		intel_gt_retire_requests(guc_to_gt(guc));
		goto try_again;
	}

	return ret;
}

static void unpin_guc_id(struct intel_guc *guc, struct intel_context *ce)
{
	unsigned long flags;

	GEM_BUG_ON(atomic_read(&ce->guc_id.ref) < 0);
	GEM_BUG_ON(intel_context_is_child(ce));

	if (unlikely(context_guc_id_invalid(ce) ||
		     intel_context_is_parent(ce)))
		return;

	spin_lock_irqsave(&guc->submission_state.lock, flags);
	if (!context_guc_id_invalid(ce) && list_empty(&ce->guc_id.link) &&
	    !atomic_read(&ce->guc_id.ref))
		list_add_tail(&ce->guc_id.link,
			      &guc->submission_state.guc_id_list);
	spin_unlock_irqrestore(&guc->submission_state.lock, flags);
}

static int __guc_action_register_multi_lrc_v69(struct intel_guc *guc,
					       struct intel_context *ce,
					       u32 guc_id,
					       u32 offset,
					       bool loop)
{
	struct intel_context *child;
	u32 action[4 + MAX_ENGINE_INSTANCE];
	int len = 0;

	GEM_BUG_ON(ce->parallel.number_children > MAX_ENGINE_INSTANCE);

	action[len++] = INTEL_GUC_ACTION_REGISTER_CONTEXT_MULTI_LRC;
	action[len++] = guc_id;
	action[len++] = ce->parallel.number_children + 1;
	action[len++] = offset;
	for_each_child(ce, child) {
		offset += sizeof(struct guc_lrc_desc_v69);
		action[len++] = offset;
	}

	return guc_submission_send_busy_loop(guc, action, len, 0, loop);
}

static void prepare_context_registration_info_v69(struct intel_context *ce);
static void prepare_context_registration_info_v70(struct intel_context *ce,
						  struct guc_ctxt_registration_info *info);

static int __prepare_context_registration_action_multi_lrc_v70(struct intel_context *ce, u32 *action)
{
	struct guc_ctxt_registration_info info;
	struct intel_context *child;
	int len = 0;
	u32 next_id;

	GEM_BUG_ON(ce->parallel.number_children > MAX_ENGINE_INSTANCE);

	prepare_context_registration_info_v70(ce, &info);

	action[len++] = INTEL_GUC_ACTION_REGISTER_CONTEXT_MULTI_LRC;
	action[len++] = info.flags;
	action[len++] = info.context_idx;
	action[len++] = info.engine_class;
	action[len++] = info.engine_submit_mask;
	action[len++] = info.wq_desc_lo;
	action[len++] = info.wq_desc_hi;
	action[len++] = info.wq_base_lo;
	action[len++] = info.wq_base_hi;
	action[len++] = info.wq_size;
	action[len++] = ce->parallel.number_children + 1;
	action[len++] = info.hwlrca_lo;
	action[len++] = info.hwlrca_hi;

	next_id = info.context_idx + 1;
	for_each_child(ce, child) {
		GEM_BUG_ON(next_id++ != child->guc_id.id);

		/*
		 * NB: GuC interface supports 64 bit LRCA even though i915/HW
		 * only supports 32 bit currently.
		 */
		action[len++] = lower_32_bits(child->lrc.lrca);
		action[len++] = upper_32_bits(child->lrc.lrca);
	}

	return len;
}

static int __guc_action_register_context_v69(struct intel_guc *guc,
					     u32 guc_id,
					     u32 offset,
					     bool loop)
{
	u32 action[] = {
		INTEL_GUC_ACTION_REGISTER_CONTEXT,
		guc_id,
		offset,
	};

	return guc_submission_send_busy_loop(guc, action, ARRAY_SIZE(action),
					     0, loop);
}


static int __prepare_context_registration_action_single_v70(struct intel_context *ce, u32 *action)
{
	struct guc_ctxt_registration_info info;
	int len = 0;

	GEM_BUG_ON(ce->parallel.number_children > MAX_ENGINE_INSTANCE);

	prepare_context_registration_info_v70(ce, &info);

	action[len++] = INTEL_GUC_ACTION_REGISTER_CONTEXT;
	action[len++] = info.flags;
	action[len++] = info.context_idx;
	action[len++] = info.engine_class;
	action[len++] = info.engine_submit_mask;
	action[len++] = info.wq_desc_lo;
	action[len++] = info.wq_desc_hi;
	action[len++] = info.wq_base_lo;
	action[len++] = info.wq_base_hi;
	action[len++] = info.wq_size;
	action[len++] = info.hwlrca_lo;
	action[len++] = info.hwlrca_hi;

	return len;
}

static int
register_context_v69(struct intel_guc *guc, struct intel_context *ce, bool loop)
{
	u32 offset = intel_guc_ggtt_offset(guc, guc->lrc_desc_pool_v69) +
		ce->guc_id.id * sizeof(struct guc_lrc_desc_v69);

	prepare_context_registration_info_v69(ce);

	if (intel_context_is_parent(ce))
		return __guc_action_register_multi_lrc_v69(guc, ce, ce->guc_id.id,
							   offset, loop);
	else
		return __guc_action_register_context_v69(guc, ce->guc_id.id,
							 offset, loop);
}

static int
register_context_v70(struct intel_guc *guc, struct intel_context *ce, bool loop)
{
	u32 action[13 + (MAX_ENGINE_INSTANCE * 2)];
	bool not_atomic = !in_atomic() && !rcu_preempt_depth() && !irqs_disabled();
	unsigned int sleep_period_us = 1;
	int srcu, len, err;

	/* No sleeping with spin locks, just busy loop */
	might_sleep_if(loop && not_atomic);

retry:
	err = gt_ggtt_address_read_lock_interruptible(guc_to_gt(guc), &srcu);
	if (unlikely(err))
		return err;

	if (intel_context_is_parent(ce))
		len = __prepare_context_registration_action_multi_lrc_v70(ce, action);
	else
		len = __prepare_context_registration_action_single_v70(ce, action);

	GEM_BUG_ON(len > ARRAY_SIZE(action));

	err = intel_guc_send_nb(guc, action, len, 0);
	gt_ggtt_address_read_unlock(guc_to_gt(guc), srcu);
	if (unlikely(err == -EBUSY && loop)) {
		intel_guc_send_wait(&sleep_period_us, not_atomic);
		goto retry;
	}

	return err;
}

static int register_context(struct intel_context *ce, bool loop)
{
	struct intel_guc *guc = ce_to_guc(ce);
	int ret;

	GEM_BUG_ON(intel_context_is_child(ce));
	trace_intel_context_register(ce);

	if (GUC_SUBMIT_VER(guc) >= MAKE_GUC_VER(1, 0, 0))
		ret = register_context_v70(guc, ce, loop);
	else
		ret = register_context_v69(guc, ce, loop);

	if (likely(!ret)) {
		unsigned long flags;

		spin_lock_irqsave(&ce->guc_state.lock, flags);
		set_context_registered(ce);
		spin_unlock_irqrestore(&ce->guc_state.lock, flags);

		if (GUC_SUBMIT_VER(guc) >= MAKE_GUC_VER(1, 0, 0))
			guc_context_policy_init_v70(ce, loop);
	}

	return ret;
}

static int __guc_action_deregister_context(struct intel_guc *guc,
					   u32 guc_id)
{
	u32 action[] = {
		INTEL_GUC_ACTION_DEREGISTER_CONTEXT,
		guc_id,
	};

	return guc_submission_send_busy_loop(guc, action, ARRAY_SIZE(action),
					     G2H_LEN_DW_DEREGISTER_CONTEXT,
					     true);
}

static int deregister_context(struct intel_context *ce, u32 guc_id)
{
	struct intel_guc *guc = ce_to_guc(ce);

	GEM_BUG_ON(intel_context_is_child(ce));
	trace_intel_context_deregister(ce);

	return __guc_action_deregister_context(guc, guc_id);
}

static inline void clear_children_join_go_memory(struct intel_context *ce)
{
	struct parent_scratch *ps = __get_parent_scratch(ce);
	int i;

	ps->go.semaphore = 0;
	for (i = 0; i < ce->parallel.number_children + 1; ++i)
		ps->join[i].semaphore = 0;
}

static inline u32 get_children_go_value(struct intel_context *ce)
{
	return __get_parent_scratch(ce)->go.semaphore;
}

static inline u32 get_children_join_value(struct intel_context *ce,
					  u8 child_index)
{
	return __get_parent_scratch(ce)->join[child_index].semaphore;
}

struct context_policy {
	u32 count;
	struct guc_update_context_policy h2g;
};

static u32 __guc_context_policy_action_size(struct context_policy *policy)
{
	size_t bytes = sizeof(policy->h2g.header) +
		       (sizeof(policy->h2g.klv[0]) * policy->count);

	return bytes / sizeof(u32);
}

static void __guc_context_policy_start_klv(struct context_policy *policy, u16 guc_id)
{
	policy->h2g.header.action = INTEL_GUC_ACTION_HOST2GUC_UPDATE_CONTEXT_POLICIES;
	policy->h2g.header.ctx_id = guc_id;
	policy->count = 0;
}

#define MAKE_CONTEXT_POLICY_ADD(func, id) \
static void __guc_context_policy_add_##func(struct context_policy *policy, u32 data) \
{ \
	GEM_BUG_ON(policy->count >= GUC_CONTEXT_POLICIES_KLV_NUM_IDS); \
	policy->h2g.klv[policy->count].kl = \
		FIELD_PREP(GUC_KLV_0_KEY, GUC_CONTEXT_POLICIES_KLV_ID_##id) | \
		FIELD_PREP(GUC_KLV_0_LEN, 1); \
	policy->h2g.klv[policy->count].value = data; \
	policy->count++; \
}

MAKE_CONTEXT_POLICY_ADD(execution_quantum, EXECUTION_QUANTUM)
MAKE_CONTEXT_POLICY_ADD(preemption_timeout, PREEMPTION_TIMEOUT)
MAKE_CONTEXT_POLICY_ADD(priority, SCHEDULING_PRIORITY)
MAKE_CONTEXT_POLICY_ADD(preempt_to_idle, PREEMPT_TO_IDLE_ON_QUANTUM_EXPIRY)

#undef MAKE_CONTEXT_POLICY_ADD

static int __guc_context_set_context_policies(struct intel_guc *guc,
					      struct context_policy *policy,
					      bool loop)
{
	return guc_submission_send_busy_loop(guc, (u32 *)&policy->h2g,
					__guc_context_policy_action_size(policy),
					0, loop);
}

static int guc_context_policy_init_v70(struct intel_context *ce, bool loop)
{
	struct intel_engine_cs *engine = ce->engine;
	struct intel_guc *guc = &engine->gt->uc.guc;
	struct context_policy policy;
	u32 execution_quantum;
	u32 preemption_timeout;
	unsigned long flags;
	int ret;

	/* Refresh the context's scheduling policies before applying */
	intel_context_update_schedule_policy(ce);

	/* NB: For both of these, zero means disabled. */
	GEM_BUG_ON(overflows_type(ce->schedule_policy.timeslice_duration_ms * 1000,
				  execution_quantum));
	GEM_BUG_ON(overflows_type(ce->schedule_policy.preempt_timeout_ms * 1000,
				  preemption_timeout));
	execution_quantum = ce->schedule_policy.timeslice_duration_ms * 1000;
	preemption_timeout = ce->schedule_policy.preempt_timeout_ms * 1000;

	__guc_context_policy_start_klv(&policy, ce->guc_id.id);

	__guc_context_policy_add_priority(&policy, ce->guc_state.prio);
	__guc_context_policy_add_execution_quantum(&policy, execution_quantum);
	__guc_context_policy_add_preemption_timeout(&policy, preemption_timeout);

	if (engine->flags & I915_ENGINE_WANT_FORCED_PREEMPTION)
		__guc_context_policy_add_preempt_to_idle(&policy, 1);

	ret = __guc_context_set_context_policies(guc, &policy, loop);

	spin_lock_irqsave(&ce->guc_state.lock, flags);
	if (ret != 0)
		set_context_policy_required(ce);
	else
		clr_context_policy_required(ce);
	spin_unlock_irqrestore(&ce->guc_state.lock, flags);

	return ret;
}

static void guc_context_policy_init_v69(struct intel_context *ce,
					struct guc_lrc_desc_v69 *desc)
{
	struct intel_engine_cs *engine = ce->engine;

	desc->policy_flags = 0;

	if (engine->flags & I915_ENGINE_WANT_FORCED_PREEMPTION)
		desc->policy_flags |= CONTEXT_POLICY_FLAG_PREEMPT_TO_IDLE_V69;

	/* NB: For both of these, zero means disabled. */
	GEM_BUG_ON(overflows_type(ce->schedule_policy.timeslice_duration_ms * 1000,
				  desc->execution_quantum));
	GEM_BUG_ON(overflows_type(ce->schedule_policy.preempt_timeout_ms * 1000,
				  desc->preemption_timeout));
	desc->execution_quantum = ce->schedule_policy.timeslice_duration_ms * 1000;
	desc->preemption_timeout = ce->schedule_policy.preempt_timeout_ms * 1000;
}

static u32 map_guc_prio_to_lrc_desc_prio(u8 prio)
{
	/*
	 * this matches the mapping we do in map_i915_prio_to_guc_prio()
	 * (e.g. prio < I915_PRIORITY_NORMAL maps to GUC_CLIENT_PRIORITY_NORMAL)
	 */
	switch (prio) {
	default:
		MISSING_CASE(prio);
		fallthrough;
	case GUC_CLIENT_PRIORITY_KMD_NORMAL:
		return GEN12_CTX_PRIORITY_NORMAL;
	case GUC_CLIENT_PRIORITY_NORMAL:
		return GEN12_CTX_PRIORITY_LOW;
	case GUC_CLIENT_PRIORITY_HIGH:
	case GUC_CLIENT_PRIORITY_KMD_HIGH:
		return GEN12_CTX_PRIORITY_HIGH;
	}
}

static inline void update_um_queues_regs(struct intel_context *ce)
{
	u32 asid;

	asid = ce->vm->asid;
	if (!asid)
		return;

	if (rcu_access_pointer(ce->gem_context)) {
		struct i915_gem_context *ctx;

		rcu_read_lock();
		ctx = rcu_dereference(ce->gem_context);
		if (ctx->acc_trigger) {
			ce->lrc_reg_state[PVC_CTX_ACC_CTR_THOLD] =
				ctx->acc_notify << ACC_NOTIFY_S |
				ctx->acc_trigger;
			asid |= ctx->acc_granularity << ACC_GRANULARITY_S;
		}
		rcu_read_unlock();
	}
	ce->lrc_reg_state[PVC_CTX_ASID] = asid;
}

static void prepare_context_registration_info_v69(struct intel_context *ce)
{
	struct intel_engine_cs *engine = ce->engine;
	struct intel_guc *guc = &engine->gt->uc.guc;
	u32 ctx_id = ce->guc_id.id;
	struct guc_lrc_desc_v69 *desc;
	struct intel_context *child;

	GEM_BUG_ON(!engine->mask);

	update_um_queues_regs(ce);

	desc = __get_lrc_desc_v69(guc, ctx_id);
	GEM_BUG_ON(!desc);
	desc->engine_class = engine_class_to_guc_class(engine->class);
	desc->engine_submit_mask = engine->logical_mask;
	desc->hw_context_desc = ce->lrc.lrca;
	desc->priority = ce->guc_state.prio;
	desc->context_flags = CONTEXT_REGISTRATION_FLAG_KMD;
	guc_context_policy_init_v69(ce, desc);

	/*
	 * If context is a parent, we need to register a process descriptor
	 * describing a work queue and register all child contexts.
	 */
	if (intel_context_is_parent(ce)) {
		struct guc_process_desc_v69 *pdesc;

		ce->parallel.guc.wqi_tail = 0;
		ce->parallel.guc.wqi_head = 0;

		desc->process_desc = i915_ggtt_offset(ce->state) +
			__get_parent_scratch_offset(ce);
		desc->wq_addr = i915_ggtt_offset(ce->state) +
			__get_wq_offset(ce);
		desc->wq_size = WQ_SIZE;

		pdesc = __get_process_desc_v69(ce);
		memset(pdesc, 0, sizeof(*(pdesc)));
		pdesc->stage_id = ce->guc_id.id;
		pdesc->wq_base_addr = desc->wq_addr;
		pdesc->wq_size_bytes = desc->wq_size;
		pdesc->wq_status = WQ_STATUS_ACTIVE;

		ce->parallel.guc.wq_head = &pdesc->head;
		ce->parallel.guc.wq_tail = &pdesc->tail;
		ce->parallel.guc.wq_status = &pdesc->wq_status;

		for_each_child(ce, child) {
			desc = __get_lrc_desc_v69(guc, child->guc_id.id);

			desc->engine_class =
				engine_class_to_guc_class(engine->class);
			desc->hw_context_desc = child->lrc.lrca;
			desc->priority = ce->guc_state.prio;
			desc->context_flags = CONTEXT_REGISTRATION_FLAG_KMD;
			guc_context_policy_init_v69(ce, desc);
		}

		clear_children_join_go_memory(ce);
	}
}

static void prepare_context_registration_info_v70(struct intel_context *ce,
						  struct guc_ctxt_registration_info *info)
{
	struct intel_engine_cs *engine = ce->engine;
	u32 ctx_id = ce->guc_id.id;

	GEM_BUG_ON(!engine->mask);

	update_um_queues_regs(ce);

	memset(info, 0, sizeof(*info));
	info->context_idx = ctx_id;
	info->engine_class = engine_class_to_guc_class(engine->class);
	info->engine_submit_mask = engine->logical_mask;
	/*
	 * NB: GuC interface supports 64 bit LRCA even though i915/HW
	 * only supports 32 bit currently.
	 */
	info->hwlrca_lo = lower_32_bits(ce->lrc.lrca);
	info->hwlrca_hi = upper_32_bits(ce->lrc.lrca);
	if (engine->flags & I915_ENGINE_HAS_EU_PRIORITY)
		info->hwlrca_lo |= map_guc_prio_to_lrc_desc_prio(ce->guc_state.prio);
	info->flags = CONTEXT_REGISTRATION_FLAG_KMD;

	/*
	 * If context is a parent, we need to register a process descriptor
	 * describing a work queue and register all child contexts.
	 */
	if (intel_context_is_parent(ce)) {
		struct guc_sched_wq_desc *wq_desc;
		u64 wq_desc_offset, wq_base_offset;
		struct intel_context *child;

		ce->parallel.guc.wqi_tail = 0;
		ce->parallel.guc.wqi_head = 0;

		wq_desc_offset = i915_ggtt_offset(ce->state) +
				 __get_parent_scratch_offset(ce);
		wq_base_offset = i915_ggtt_offset(ce->state) +
				 __get_wq_offset(ce);
		info->wq_desc_lo = lower_32_bits(wq_desc_offset);
		info->wq_desc_hi = upper_32_bits(wq_desc_offset);
		info->wq_base_lo = lower_32_bits(wq_base_offset);
		info->wq_base_hi = upper_32_bits(wq_base_offset);
		info->wq_size = WQ_SIZE;

		wq_desc = __get_wq_desc_v70(ce);
		memset(wq_desc, 0, sizeof(*wq_desc));
		wq_desc->wq_status = WQ_STATUS_ACTIVE;

		ce->parallel.guc.wq_head = &wq_desc->head;
		ce->parallel.guc.wq_tail = &wq_desc->tail;
		ce->parallel.guc.wq_status = &wq_desc->wq_status;

		for_each_child(ce, child)
			update_um_queues_regs(child);

		clear_children_join_go_memory(ce);
	}
}

static int try_context_registration(struct intel_context *ce, bool loop)
{
	struct intel_guc *guc = ce_to_guc(ce);
	u32 ctx_id = ce->guc_id.id;
	struct intel_context *old;
	int ret;

	GEM_BUG_ON(!sched_state_is_init(ce));

	if (__test_and_clear_bit(GUC_INVALIDATE_TLB, &guc->flags)) {
		ret = intel_guc_invalidate_tlb_guc(guc, INTEL_GUC_TLB_INVAL_MODE_HEAVY);
		if (unlikely(ret))
			return ret;
	}

	old = set_ctx_id_mapping(guc, ctx_id, ce);
	if (IS_ERR(old))
		return PTR_ERR(old);

	/*
	 * The context_lookup xarray is used to determine if the hardware
	 * context is currently registered. There are two cases in which it
	 * could be registered either the guc_id has been stolen from another
	 * context or the lrc descriptor address of this context has changed. In
	 * either case the context needs to be deregistered with the GuC before
	 * registering this context.
	 */
	if (old) {
		unsigned long flags;
		bool disabled;

		trace_intel_context_steal_guc_id(ce);
		GEM_BUG_ON(!loop);

		/* Seal race with Reset */
		spin_lock_irqsave(&ce->guc_state.lock, flags);
		disabled = submission_disabled(guc);
		if (likely(!disabled)) {
			set_context_wait_for_deregister_to_register(ce);
			intel_context_get(ce);
		}
		spin_unlock_irqrestore(&ce->guc_state.lock, flags);
		if (unlikely(disabled)) {
			clr_ctx_id_mapping(guc, ctx_id);
			return 0;	/* Will get registered later */
		}

		/*
		 * If stealing the guc_id, this ce has the same guc_id as the
		 * context whose guc_id was stolen.
		 */
		ret = deregister_context(ce, ctx_id);
	} else {
		ret = register_context(ce, loop);
	}
	if (ret) {
		set_ctx_id_mapping(guc, ctx_id, old);

		if (context_wait_for_deregister_to_register(ce)) {
			clr_context_wait_for_deregister_to_register(ce);
			intel_context_put(ce);
		}

		if (unlikely(ret == -ENODEV))
			ret = 0; /* Will get registered later */
	}

	return ret;
}

static int __guc_context_pre_pin(struct intel_context *ce,
				 struct intel_engine_cs *engine,
				 struct i915_gem_ww_ctx *ww,
				 void **vaddr)
{
	return lrc_pre_pin(ce, engine, ww, vaddr);
}

static int __guc_context_pin(struct intel_context *ce,
			     struct intel_engine_cs *engine,
			     void *vaddr)
{
	int ret, srcu;

	ret = gt_ggtt_address_read_lock_sync(engine->gt, &srcu);
	if (unlikely(ret))
		return ret;
	if (i915_ggtt_offset(ce->state) !=
	    (ce->lrc.lrca & CTX_GTT_ADDRESS_MASK))
		set_bit(CONTEXT_LRCA_DIRTY, &ce->flags);

	/*
	 * GuC context gets pinned in guc_request_alloc. See that function for
	 * explaination of why.
	 */

	ret = lrc_pin(ce, engine, vaddr);

	gt_ggtt_address_read_unlock(engine->gt, srcu);
	return ret;
}

static int guc_context_pre_pin(struct intel_context *ce,
			       struct i915_gem_ww_ctx *ww,
			       void **vaddr)
{
	return __guc_context_pre_pin(ce, ce->engine, ww, vaddr);
}

static int guc_context_pin(struct intel_context *ce, void *vaddr)
{
	int ret = __guc_context_pin(ce, ce->engine, vaddr);

	if (likely(!ret && !intel_context_is_barrier(ce)))
		intel_engine_pm_get(ce->engine);

	return ret;
}

static void guc_context_unpin(struct intel_context *ce)
{
	struct intel_guc *guc = ce_to_guc(ce);

	unpin_guc_id(guc, ce);
	lrc_unpin(ce);

	if (likely(!intel_context_is_barrier(ce)))
		intel_engine_pm_put_async(ce->engine);
}

static void guc_context_post_unpin(struct intel_context *ce)
{
	lrc_post_unpin(ce);
}

static void __guc_context_sched_enable(struct intel_guc *guc,
				       struct intel_context *ce)
{
	u32 action[] = {
		INTEL_GUC_ACTION_SCHED_CONTEXT_MODE_SET,
		ce->guc_id.id,
		GUC_CONTEXT_ENABLE
	};

	trace_intel_context_sched_enable(ce);

	guc_submission_send_busy_loop(guc, action, ARRAY_SIZE(action),
				      G2H_LEN_DW_SCHED_CONTEXT_MODE_SET, true);
}

static void __guc_context_sched_disable(struct intel_guc *guc,
					struct intel_context *ce,
					u16 guc_id)
{
	u32 action[] = {
		INTEL_GUC_ACTION_SCHED_CONTEXT_MODE_SET,
		guc_id,	/* ce->guc_id.id not stable */
		GUC_CONTEXT_DISABLE
	};

	GEM_BUG_ON(guc_id == GUC_INVALID_CONTEXT_ID);

	GEM_BUG_ON(intel_context_is_child(ce));
	trace_intel_context_sched_disable(ce);

	guc_submission_send_busy_loop(guc, action, ARRAY_SIZE(action),
				      G2H_LEN_DW_SCHED_CONTEXT_MODE_SET, true);
}

static void guc_blocked_fence_complete(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);

	if (!i915_sw_fence_done(&ce->guc_state.blocked))
		i915_sw_fence_complete(&ce->guc_state.blocked);
}

static void guc_blocked_fence_reinit(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	GEM_BUG_ON(!i915_sw_fence_done(&ce->guc_state.blocked));

	/*
	 * This fence is always complete unless a pending schedule disable is
	 * outstanding. We arm the fence here and complete it when we receive
	 * the pending schedule disable complete message.
	 */
	i915_sw_fence_fini(&ce->guc_state.blocked);
	i915_sw_fence_reinit(&ce->guc_state.blocked);
	i915_sw_fence_await(&ce->guc_state.blocked);
	i915_sw_fence_commit(&ce->guc_state.blocked);
}

static u16 prep_context_pending_disable(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	GEM_BUG_ON(context_guc_id_invalid(ce));

	set_context_pending_disable(ce);
	clr_context_enabled(ce);
	guc_blocked_fence_reinit(ce);
	intel_context_get(ce);

	return ce->guc_id.id;
}

static struct i915_sw_fence *guc_context_block(struct intel_context *ce)
{
	struct intel_guc *guc = ce_to_guc(ce);
	unsigned long flags;
	intel_wakeref_t wakeref;
	u16 guc_id;
	bool enabled;

	GEM_BUG_ON(intel_context_is_child(ce));

	spin_lock_irqsave(&ce->guc_state.lock, flags);

	incr_context_blocked(ce);

	enabled = context_enabled(ce);
	if (unlikely(!enabled || submission_disabled(guc))) {
		if (enabled)
			clr_context_enabled(ce);
		spin_unlock_irqrestore(&ce->guc_state.lock, flags);
		return &ce->guc_state.blocked;
	}

	/*
	 * We add +2 here as the schedule disable complete CTB handler calls
	 * intel_context_sched_disable_unpin (-2 to pin_count).
	 */
	atomic_add(2, &ce->pin_count);

	guc_id = prep_context_pending_disable(ce);

	spin_unlock_irqrestore(&ce->guc_state.lock, flags);

	with_intel_gt_pm(guc_to_gt(guc), wakeref)
		__guc_context_sched_disable(guc, ce, guc_id);

	return &ce->guc_state.blocked;
}

#define SCHED_STATE_MULTI_BLOCKED_MASK \
	(SCHED_STATE_BLOCKED_MASK & ~SCHED_STATE_BLOCKED)
#define SCHED_STATE_NO_UNBLOCK \
	(SCHED_STATE_MULTI_BLOCKED_MASK | \
	 SCHED_STATE_PENDING_DISABLE | \
	 SCHED_STATE_BANNED)

static bool context_cant_unblock(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);

	return (ce->guc_state.sched_state & SCHED_STATE_NO_UNBLOCK) ||
		context_guc_id_invalid(ce) ||
		!ctx_id_mapped(ce_to_guc(ce), ce->guc_id.id) ||
		!intel_context_is_pinned(ce);
}

static void guc_context_unblock(struct intel_context *ce)
{
	struct intel_guc *guc = ce_to_guc(ce);
	unsigned long flags;
	intel_wakeref_t wakeref;
	bool enable;

	GEM_BUG_ON(context_enabled(ce));
	GEM_BUG_ON(intel_context_is_child(ce));

	spin_lock_irqsave(&ce->guc_state.lock, flags);

	if (unlikely(submission_disabled(guc) ||
		     context_cant_unblock(ce))) {
		enable = false;
	} else {
		enable = true;
		set_context_pending_enable(ce);
		set_context_enabled(ce);
		intel_context_get(ce);
	}

	decr_context_blocked(ce);

	spin_unlock_irqrestore(&ce->guc_state.lock, flags);

	if (enable) {
		with_intel_gt_pm(guc_to_gt(guc), wakeref)
			__guc_context_sched_enable(guc, ce);
	}
}

static void guc_context_cancel_request(struct intel_context *ce,
				       struct i915_request *rq)
{
	struct intel_context *block_context =
		request_to_scheduling_context(rq);

	if (i915_sw_fence_signaled(&rq->submit)) {
		struct i915_sw_fence *fence;

		intel_context_get(ce);
		fence = guc_context_block(block_context);
		i915_sw_fence_wait(fence);
		if (!i915_request_completed(rq)) {
			__i915_request_skip(rq);
			guc_reset_state(ce, intel_ring_wrap(ce->ring, rq->head),
					true);
		}

		/*
		 * XXX: Racey if context is reset, see comment in
		 * __guc_reset_context().
		 */
		flush_work(&ce_to_guc(ce)->ct.requests.worker);

		guc_context_unblock(block_context);
		intel_context_put(ce);
	}
}

static void __guc_context_set_preemption_timeout(struct intel_guc *guc,
						 u16 guc_id,
						 u32 preemption_timeout)
{
	if (GUC_SUBMIT_VER(guc) >= MAKE_GUC_VER(1, 0, 0)) {
		struct context_policy policy;

		__guc_context_policy_start_klv(&policy, guc_id);
		__guc_context_policy_add_preemption_timeout(&policy, preemption_timeout);
		__guc_context_set_context_policies(guc, &policy, true);
	} else {
		u32 action[] = {
			INTEL_GUC_ACTION_V69_SET_CONTEXT_PREEMPTION_TIMEOUT,
			guc_id,
			preemption_timeout
		};

		intel_guc_send_busy_loop(guc, action, ARRAY_SIZE(action), 0, true);
	}
}

static void guc_context_ban(struct intel_context *ce, struct i915_request *rq)
{
	struct intel_guc *guc = ce_to_guc(ce);
	u16 guc_id = GUC_INVALID_CONTEXT_ID;
	unsigned long flags;
	intel_wakeref_t wf;

	if (!ce->timeline)
		return;

	if (GEM_WARN_ON(intel_context_is_barrier(ce)))
		return;

	GEM_BUG_ON(intel_context_is_child(ce));
	if (!submission_disabled(guc)) {
		with_intel_gt_pm_if_awake(guc_to_gt(guc), wf) {
			spin_lock_irqsave(&ce->guc_state.lock, flags);
			set_context_banned(ce);
			if (context_enabled(ce) && intel_context_is_active(ce)) {
				atomic_add(2, &ce->pin_count);
				guc_id = prep_context_pending_disable(ce);
			}
			spin_unlock_irqrestore(&ce->guc_state.lock, flags);
			if (guc_id != GUC_INVALID_CONTEXT_ID) {
				__guc_context_set_preemption_timeout(guc, guc_id, 1);
				__guc_context_sched_disable(guc, ce, guc_id);
			}
		}
	}

	if (rq && !i915_request_is_active(rq))
		return;

	guc_cancel_context_requests(ce);
}

static void do_sched_disable(struct intel_guc *guc, struct intel_context *ce,
			     unsigned long flags)
	__releases(ce->guc_state.lock)
{
	intel_wakeref_t wakeref;
	u16 guc_id;

	lockdep_assert_held(&ce->guc_state.lock);
	guc_id = prep_context_pending_disable(ce);

	spin_unlock_irqrestore(&ce->guc_state.lock, flags);

	with_intel_gt_pm_async(guc_to_gt(guc), wakeref)
		__guc_context_sched_disable(guc, ce, guc_id);
}

static bool bypass_sched_disable(struct intel_guc *guc,
				 struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	GEM_BUG_ON(intel_context_is_child(ce));

	if (!intel_gt_pm_is_awake(guc_to_gt(guc)) ||
	    submission_disabled(guc) ||
	    context_guc_id_invalid(ce) ||
	    !ctx_id_mapped(guc, ce->guc_id.id)) {
		clr_context_enabled(ce);
		return true;
	}

	return !context_enabled(ce);
}

static void __delay_sched_disable(struct work_struct *wrk)
{
	struct intel_context *ce =
		container_of(wrk, typeof(*ce), guc_state.sched_disable_delay_work.work);
	struct intel_guc *guc = ce_to_guc(ce);
	unsigned long flags;

	spin_lock_irqsave(&ce->guc_state.lock, flags);

	if (bypass_sched_disable(guc, ce)) {
		spin_unlock_irqrestore(&ce->guc_state.lock, flags);
		intel_context_sched_disable_unpin(ce);
	} else {
		do_sched_disable(guc, ce, flags);
	}
}

static bool guc_id_pressure(struct intel_guc *guc, struct intel_context *ce)
{
	/*
	 * parent contexts are perma-pinned, if we are unpinning do schedule
	 * disable immediately.
	 */
	if (intel_context_is_parent(ce))
		return true;

	/*
	 * If we are beyond the threshold for avail guc_ids, do schedule disable immediately.
	 */
	return guc->submission_state.guc_ids_in_use >
		guc->submission_state.sched_disable_gucid_threshold;
}

static void guc_context_sched_disable(struct intel_context *ce)
{
	struct intel_guc *guc = ce_to_guc(ce);
	u64 delay = guc->submission_state.sched_disable_delay_ms;
	unsigned long flags;

	spin_lock_irqsave(&ce->guc_state.lock, flags);

	if (bypass_sched_disable(guc, ce)) {
		spin_unlock_irqrestore(&ce->guc_state.lock, flags);
		intel_context_sched_disable_unpin(ce);
	} else if (!intel_context_is_closed(ce) && !guc_id_pressure(guc, ce) &&
		   delay) {
		spin_unlock_irqrestore(&ce->guc_state.lock, flags);
		mod_delayed_work(system_unbound_wq,
				 &ce->guc_state.sched_disable_delay_work,
				 msecs_to_jiffies(delay));
	} else {
		do_sched_disable(guc, ce, flags);
	}
}

static void guc_context_close(struct intel_context *ce)
{
	unsigned long flags;

	if (test_bit(CONTEXT_GUC_INIT, &ce->flags) &&
	    cancel_delayed_work(&ce->guc_state.sched_disable_delay_work))
		__delay_sched_disable(&ce->guc_state.sched_disable_delay_work.work);

	spin_lock_irqsave(&ce->guc_state.lock, flags);
	set_context_close_done(ce);
	spin_unlock_irqrestore(&ce->guc_state.lock, flags);
}

static struct i915_sw_fence *guc_context_suspend(struct intel_context *ce,
						 bool atomic)
{
	/*
	 * Need to sort out pm sleeping and locking around
	 * __guc_context_sched_disable / enable
	 */
	if (atomic)
		return ERR_PTR(-EBUSY);

	return guc_context_block(ce);
}

static void guc_context_resume(struct intel_context *ce)
{
	GEM_BUG_ON(!i915_sw_fence_done(&ce->guc_state.blocked));

	guc_context_unblock(ce);
}

static inline void guc_lrc_desc_unpin(struct intel_context *ce)
{
	struct intel_guc *guc = ce_to_guc(ce);
	struct intel_gt *gt = guc_to_gt(guc);
	unsigned long flags;
	bool disabled;

	GEM_BUG_ON(!intel_gt_pm_is_awake(gt));
	GEM_BUG_ON(!ctx_id_mapped(guc, ce->guc_id.id));
	GEM_BUG_ON(ce != __get_context(guc, ce->guc_id.id));
	GEM_BUG_ON(context_enabled(ce));

	/* Seal race with Reset */
	spin_lock_irqsave(&ce->guc_state.lock, flags);
	disabled = submission_disabled(guc);
	if (likely(!disabled)) {
		__intel_gt_pm_get(gt);
		set_context_destroyed(ce);
		clr_context_registered(ce);
	}
	spin_unlock_irqrestore(&ce->guc_state.lock, flags);
	if (unlikely(disabled)) {
		release_guc_id(guc, ce);
		__guc_context_destroy(ce);
		return;
	}

	deregister_context(ce, ce->guc_id.id);
}

static void __guc_context_destroy(struct intel_context *ce)
{
	GEM_BUG_ON(ce->guc_state.prio_count[GUC_CLIENT_PRIORITY_KMD_HIGH] ||
		   ce->guc_state.prio_count[GUC_CLIENT_PRIORITY_HIGH] ||
		   ce->guc_state.prio_count[GUC_CLIENT_PRIORITY_KMD_NORMAL] ||
		   ce->guc_state.prio_count[GUC_CLIENT_PRIORITY_NORMAL]);

	lrc_fini(ce);
	intel_context_fini(ce);

	if (intel_engine_is_virtual(ce->engine)) {
		struct guc_virtual_engine *ve =
			container_of(ce, typeof(*ve), context);

		if (ve->base.breadcrumbs)
			intel_breadcrumbs_put(ve->base.breadcrumbs);

		kfree_rcu(ce, rcu);
	} else {
		intel_context_free(ce);
	}
}

static void guc_flush_destroyed_contexts(struct intel_guc *guc)
{
	struct intel_context *ce;
	unsigned long flags;

	GEM_BUG_ON(!submission_disabled(guc) &&
		   guc_submission_initialized(guc));

	while (!list_empty(&guc->submission_state.destroyed_contexts)) {
		spin_lock_irqsave(&guc->submission_state.lock, flags);
		ce = list_first_entry_or_null(&guc->submission_state.destroyed_contexts,
					      struct intel_context,
					      destroyed_link);
		if (ce)
			list_del_init(&ce->destroyed_link);
		spin_unlock_irqrestore(&guc->submission_state.lock, flags);

		if (!ce)
			break;

		release_guc_id(guc, ce);
		__guc_context_destroy(ce);
	}
}

static void deregister_destroyed_contexts(struct intel_guc *guc)
{
	struct intel_context *ce;
	unsigned long flags;

	while (!list_empty(&guc->submission_state.destroyed_contexts)) {
		spin_lock_irqsave(&guc->submission_state.lock, flags);
		ce = list_first_entry_or_null(&guc->submission_state.destroyed_contexts,
					      struct intel_context,
					      destroyed_link);
		if (ce)
			list_del_init(&ce->destroyed_link);
		spin_unlock_irqrestore(&guc->submission_state.lock, flags);

		if (!ce)
			break;

		guc_lrc_desc_unpin(ce);
	}
}

static void destroyed_worker_func(struct work_struct *w)
{
	struct intel_guc *guc = container_of(w, struct intel_guc,
					     submission_state.destroyed_worker);
	struct intel_gt *gt = guc_to_gt(guc);
	intel_wakeref_t wakeref;

	with_intel_gt_pm(gt, wakeref)
		deregister_destroyed_contexts(guc);
}

static void guc_context_destroy(struct kref *kref)
{
	struct intel_context *ce = container_of(kref, typeof(*ce), ref);
	struct intel_guc *guc = ce_to_guc(ce);
	unsigned long flags;
	bool destroy;

	/*
	 * If the guc_id is invalid this context has been stolen and we can free
	 * it immediately. Also can be freed immediately if the context is not
	 * registered with the GuC or the GuC is in the middle of a reset.
	 */
	spin_lock_irqsave(&guc->submission_state.lock, flags);
	destroy = submission_disabled(guc) || context_guc_id_invalid(ce) ||
		!ctx_id_mapped(guc, ce->guc_id.id);
	if (likely(!destroy)) {
		if (!list_empty(&ce->guc_id.link))
			list_del_init(&ce->guc_id.link);
		list_add_tail(&ce->destroyed_link,
			      &guc->submission_state.destroyed_contexts);
	} else {
		__release_guc_id(guc, ce);
	}
	spin_unlock_irqrestore(&guc->submission_state.lock, flags);
	if (unlikely(destroy)) {
		__guc_context_destroy(ce);
		return;
	}

	/*
	 * We use a worker to issue the H2G to deregister the context as we can
	 * take the GT PM for the first time which isn't allowed from an atomic
	 * context.
	 */
	intel_gt_queue_work(guc_to_gt(guc), &guc->submission_state.destroyed_worker);
}

static int guc_context_alloc(struct intel_context *ce)
{
	return lrc_alloc(ce, ce->engine);
}

static void __guc_context_set_prio(struct intel_guc *guc,
				   struct intel_context *ce)
{
	if (GUC_SUBMIT_VER(guc) >= MAKE_GUC_VER(1, 0, 0)) {
		struct context_policy policy;

		__guc_context_policy_start_klv(&policy, ce->guc_id.id);
		__guc_context_policy_add_priority(&policy, ce->guc_state.prio);
		__guc_context_set_context_policies(guc, &policy, true);
	} else {
		u32 action[] = {
			INTEL_GUC_ACTION_V69_SET_CONTEXT_PRIORITY,
			ce->guc_id.id,
			ce->guc_state.prio,
		};

		guc_submission_send_busy_loop(guc, action, ARRAY_SIZE(action), 0, true);
	}
}

static bool __context_is_available(struct intel_guc *guc,
				   struct intel_context *ce)
{
	return !submission_disabled(guc) &&
	       context_registered(ce) &&
	       !context_wait_for_deregister_to_register(ce);
}

static void guc_context_set_prio(struct intel_guc *guc,
				 struct intel_context *ce,
				 u8 prio)
{
	GEM_BUG_ON(prio < GUC_CLIENT_PRIORITY_KMD_HIGH ||
		   prio > GUC_CLIENT_PRIORITY_NORMAL);
	lockdep_assert_held(&ce->guc_state.lock);

	if (ce->guc_state.prio == prio || !__context_is_available(guc, ce)) {
		ce->guc_state.prio = prio;
		return;
	}

	ce->guc_state.prio = prio;
	__guc_context_set_prio(guc, ce);

	trace_intel_context_set_prio(ce);
}

static inline u8 map_i915_prio_to_guc_prio(int prio)
{
	if (prio == I915_PRIORITY_NORMAL)
		return GUC_CLIENT_PRIORITY_KMD_NORMAL;
	else if (prio < I915_PRIORITY_NORMAL)
		return GUC_CLIENT_PRIORITY_NORMAL;
	else if (prio < I915_PRIORITY_DISPLAY)
		return GUC_CLIENT_PRIORITY_HIGH;
	else
		return GUC_CLIENT_PRIORITY_KMD_HIGH;
}

static inline void add_context_inflight_prio(struct intel_context *ce,
					     u8 guc_prio)
{
	lockdep_assert_held(&ce->guc_state.lock);
	GEM_BUG_ON(guc_prio >= ARRAY_SIZE(ce->guc_state.prio_count));

	++ce->guc_state.prio_count[guc_prio];

	/* Overflow protection */
	GEM_WARN_ON(!ce->guc_state.prio_count[guc_prio]);
}

static inline void sub_context_inflight_prio(struct intel_context *ce,
					     u8 guc_prio)
{
	lockdep_assert_held(&ce->guc_state.lock);
	GEM_BUG_ON(guc_prio >= ARRAY_SIZE(ce->guc_state.prio_count));

	/* Underflow protection */
	GEM_WARN_ON(!ce->guc_state.prio_count[guc_prio]);

	--ce->guc_state.prio_count[guc_prio];
}

static inline void update_context_prio(struct intel_context *ce)
{
	struct intel_guc *guc = &ce->engine->gt->uc.guc;
	int i;

	BUILD_BUG_ON(GUC_CLIENT_PRIORITY_KMD_HIGH != 0);
	BUILD_BUG_ON(GUC_CLIENT_PRIORITY_KMD_HIGH > GUC_CLIENT_PRIORITY_NORMAL);

	lockdep_assert_held(&ce->guc_state.lock);

	for (i = 0; i < ARRAY_SIZE(ce->guc_state.prio_count); ++i) {
		if (ce->guc_state.prio_count[i]) {
			guc_context_set_prio(guc, ce, i);
			break;
		}
	}
}

static inline bool new_guc_prio_higher(u8 old_guc_prio, u8 new_guc_prio)
{
	/* Lower value is higher priority */
	return new_guc_prio < old_guc_prio;
}

static void add_to_context(struct i915_request *rq)
{
	struct intel_context *ce = request_to_scheduling_context(rq);
	u8 new_guc_prio = map_i915_prio_to_guc_prio(rq_prio(rq));

	GEM_BUG_ON(intel_context_is_child(ce));
	GEM_BUG_ON(READ_ONCE(rq->guc_prio) == GUC_PRIO_FINI);

	trace_i915_request_in(rq, 0);

	spin_lock(&ce->guc_state.lock);
	if (rq->guc_prio == GUC_PRIO_INIT) {
		rq->guc_prio = new_guc_prio;
		add_context_inflight_prio(ce, rq->guc_prio);
	} else if (new_guc_prio_higher(rq->guc_prio, new_guc_prio)) {
		sub_context_inflight_prio(ce, rq->guc_prio);
		rq->guc_prio = new_guc_prio;
		add_context_inflight_prio(ce, rq->guc_prio);
	}
	update_context_prio(ce);

	spin_unlock(&ce->guc_state.lock);
}

static void guc_prio_fini(struct i915_request *rq, struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);

	if (rq->guc_prio != GUC_PRIO_INIT &&
	    rq->guc_prio != GUC_PRIO_FINI) {
		sub_context_inflight_prio(ce, rq->guc_prio);
		update_context_prio(ce);
	}
	rq->guc_prio = GUC_PRIO_FINI;
}

static void remove_from_context(struct i915_request *rq)
{
	struct intel_context *ce = request_to_scheduling_context(rq);

	GEM_BUG_ON(intel_context_is_child(ce));

	spin_lock_irq(&ce->guc_state.lock);

	guc_prio_fini(rq, ce);

	spin_unlock_irq(&ce->guc_state.lock);

	atomic_dec(&ce->guc_id.ref);
}

static const struct intel_context_ops guc_context_ops = {
	.flags = COPS_RUNTIME_CYCLES,
	.alloc = guc_context_alloc,

	.close = guc_context_close,

	.pre_pin = guc_context_pre_pin,
	.pin = guc_context_pin,
	.unpin = guc_context_unpin,
	.post_unpin = guc_context_post_unpin,

	.ban = guc_context_ban,

	.cancel_request = guc_context_cancel_request,

	.suspend = guc_context_suspend,
	.resume = guc_context_resume,

	.enter = intel_context_enter_engine,
	.exit = intel_context_exit_engine,

	.sched_disable = guc_context_sched_disable,

	.reset = lrc_reset,
	.destroy = guc_context_destroy,

	.create_virtual = guc_create_virtual,
	.create_parallel = guc_create_parallel,
};

static void submit_work_cb(struct irq_work *wrk)
{
	struct i915_request *rq = container_of(wrk, typeof(*rq), submit_work);

	might_lock(&rq->sched_engine->lock);
	i915_sw_fence_complete(&rq->submit);
}

static void __guc_signal_context_fence(struct intel_context *ce)
{
	struct i915_request *rq, *rn;

	lockdep_assert_held(&ce->guc_state.lock);

	if (!list_empty(&ce->guc_state.fences))
		trace_intel_context_fence_release(ce);

	/*
	 * Use an IRQ to ensure locking order of sched_engine->lock ->
	 * ce->guc_state.lock is preserved.
	 */
	list_for_each_entry_safe(rq, rn, &ce->guc_state.fences,
				 guc_fence_link) {
		list_del(&rq->guc_fence_link);
		irq_work_queue(&rq->submit_work);
	}

	INIT_LIST_HEAD(&ce->guc_state.fences);
}

static void guc_signal_context_fence(struct intel_context *ce)
{
	unsigned long flags;

	GEM_BUG_ON(intel_context_is_child(ce));

	spin_lock_irqsave(&ce->guc_state.lock, flags);
	clr_context_wait_for_deregister_to_register(ce);
	__guc_signal_context_fence(ce);
	spin_unlock_irqrestore(&ce->guc_state.lock, flags);
}

static bool context_needs_register(struct intel_context *ce, bool new_guc_id)
{
	return (new_guc_id || test_bit(CONTEXT_LRCA_DIRTY, &ce->flags) ||
		!ctx_id_mapped(ce_to_guc(ce), ce->guc_id.id)) &&
		!submission_disabled(ce_to_guc(ce));
}

static void guc_context_init(struct intel_context *ce)
{
	const struct i915_gem_context *ctx;
	int prio = I915_CONTEXT_DEFAULT_PRIORITY;

	rcu_read_lock();
	ctx = rcu_dereference(ce->gem_context);
	if (ctx)
		prio = ctx->sched.priority;
	rcu_read_unlock();

	ce->guc_state.prio = map_i915_prio_to_guc_prio(prio);

	INIT_DELAYED_WORK(&ce->guc_state.sched_disable_delay_work,
			  __delay_sched_disable);

	set_bit(CONTEXT_GUC_INIT, &ce->flags);
}

static int guc_request_alloc(struct i915_request *rq)
{
	struct intel_context *ce = request_to_scheduling_context(rq);
	struct intel_guc *guc = ce_to_guc(ce);
	unsigned long flags;
	int ret;

	GEM_BUG_ON(!intel_context_is_pinned(rq->context));

	if (unlikely(!test_bit(CONTEXT_GUC_INIT, &ce->flags)))
		guc_context_init(ce);

	/*
	 * If the context gets closed while the execbuf is ongoing, the context
	 * close code will race with the below code to cancel the delayed work.
	 * If the context close wins the race and cancels the work, it will
	 * immediately call the sched disable (see guc_context_close), so there
	 * is a chance we can get past this check while the sched_disable code
	 * is being executed. To make sure that code completes before we check
	 * the status further down, we wait for the close process to complete.
	 * Else, this code path could send a request down thinking that the
	 * context is still in a schedule-enable mode while the GuC ends up
	 * dropping the request completely because the disable did go from the
	 * context_close path right to GuC just prior. In the event the CT is
	 * full, we could potentially need to wait up to 1.5 seconds.
	 */
	if (cancel_delayed_work_sync(&ce->guc_state.sched_disable_delay_work))
		intel_context_sched_disable_unpin(ce);
	else if (intel_context_is_closed(ce))
		if (wait_for(context_close_done(ce), 1500))
			guc_warn(guc, "timed out waiting on context sched close before realloc\n");
	/*
	 * Call pin_guc_id here rather than in the pinning step as with
	 * dma_resv, contexts can be repeatedly pinned / unpinned trashing the
	 * guc_id and creating horrible race conditions. This is especially bad
	 * when guc_id are being stolen due to over subscription. By the time
	 * this function is reached, it is guaranteed that the guc_id will be
	 * persistent until the generated request is retired. Thus, sealing these
	 * race conditions. It is still safe to fail here if guc_id are
	 * exhausted and return -EAGAIN to the user indicating that they can try
	 * again in the future.
	 *
	 * There is no need for a lock here as the timeline mutex ensures at
	 * most one context can be executing this code path at once. The
	 * guc_id_ref is incremented once for every request in flight and
	 * decremented on each retire. When it is zero, a lock around the
	 * increment (in pin_guc_id) is needed to seal a race with unpin_guc_id.
	 */
	if (atomic_add_unless(&ce->guc_id.ref, 1, 0))
		goto out;

	ret = pin_guc_id(guc, ce);	/* returns 1 if new guc_id assigned */
	if (unlikely(ret < 0))
		return ret;
	if (context_needs_register(ce, !!ret)) {
		ret = try_context_registration(ce, true);
		if (unlikely(ret)) {	/* unwind */
			if (ret == -EPIPE) {
				disable_submission(guc);
				goto out;	/* GPU will be reset */
			}
			atomic_dec(&ce->guc_id.ref);
			unpin_guc_id(guc, ce);
			return ret;
		}
	}

	clear_bit(CONTEXT_LRCA_DIRTY, &ce->flags);

out:
	/*
	 * We block all requests on this context if a G2H is pending for a
	 * schedule disable or context deregistration as the GuC will fail a
	 * schedule enable or context registration if either G2H is pending
	 * respectfully. Once a G2H returns, the fence is released that is
	 * blocking these requests (see guc_signal_context_fence).
	 */
	spin_lock_irqsave(&ce->guc_state.lock, flags);
	if (context_wait_for_deregister_to_register(ce) ||
	    context_pending_disable(ce)) {
		init_irq_work(&rq->submit_work, submit_work_cb);
		i915_sw_fence_await(&rq->submit);

		list_add_tail(&rq->guc_fence_link, &ce->guc_state.fences);
	}
	spin_unlock_irqrestore(&ce->guc_state.lock, flags);

	return 0;
}

static int guc_virtual_context_pre_pin(struct intel_context *ce,
				       struct i915_gem_ww_ctx *ww,
				       void **vaddr)
{
	struct intel_engine_cs *engine = guc_virtual_get_sibling(ce->engine, 0);

	return __guc_context_pre_pin(ce, engine, ww, vaddr);
}

static int guc_virtual_context_pin(struct intel_context *ce, void *vaddr)
{
	struct intel_engine_cs *engine = guc_virtual_get_sibling(ce->engine, 0);
	int ret = __guc_context_pin(ce, engine, vaddr);
	intel_engine_mask_t tmp, mask = ce->engine->mask;

	if (likely(!ret))
		for_each_engine_masked(engine, ce->engine->gt, mask, tmp)
			intel_engine_pm_get(engine);

	return ret;
}

static void guc_virtual_context_unpin(struct intel_context *ce)
{
	intel_engine_mask_t tmp, mask = ce->engine->mask;
	struct intel_engine_cs *engine;
	struct intel_guc *guc = ce_to_guc(ce);

	GEM_BUG_ON(context_enabled(ce));
	GEM_BUG_ON(intel_context_is_barrier(ce));

	unpin_guc_id(guc, ce);
	lrc_unpin(ce);

	for_each_engine_masked(engine, ce->engine->gt, mask, tmp)
		intel_engine_pm_put_async(engine);
}

static void guc_virtual_context_enter(struct intel_context *ce)
{
	intel_engine_mask_t tmp, mask = ce->engine->mask;
	struct intel_engine_cs *engine;

	for_each_engine_masked(engine, ce->engine->gt, mask, tmp)
		intel_engine_pm_get(engine);

	intel_timeline_enter(ce->timeline);
}

static void guc_virtual_context_exit(struct intel_context *ce)
{
	intel_engine_mask_t tmp, mask = ce->engine->mask;
	struct intel_engine_cs *engine;

	for_each_engine_masked(engine, ce->engine->gt, mask, tmp)
		intel_engine_pm_put(engine);

	intel_timeline_exit(ce->timeline);
}

static int guc_virtual_context_alloc(struct intel_context *ce)
{
	struct intel_engine_cs *engine = guc_virtual_get_sibling(ce->engine, 0);

	return lrc_alloc(ce, engine);
}

static struct intel_context *guc_clone_virtual(struct intel_engine_cs *src)
{
	struct intel_engine_cs *siblings[GUC_MAX_INSTANCES_PER_CLASS], *engine;
	intel_engine_mask_t tmp, mask = src->mask;
	unsigned int num_siblings = 0;

	for_each_engine_masked(engine, src->gt, mask, tmp)
		siblings[num_siblings++] = engine;

	return guc_create_virtual(siblings, num_siblings, 0);
}

static const struct intel_context_ops virtual_guc_context_ops = {
	.flags = COPS_RUNTIME_CYCLES,
	.alloc = guc_virtual_context_alloc,

	.close = guc_context_close,

	.pre_pin = guc_virtual_context_pre_pin,
	.pin = guc_virtual_context_pin,
	.unpin = guc_virtual_context_unpin,
	.post_unpin = guc_context_post_unpin,

	.ban = guc_context_ban,

	.cancel_request = guc_context_cancel_request,

	.suspend = guc_context_suspend,
	.resume = guc_context_resume,

	.enter = guc_virtual_context_enter,
	.exit = guc_virtual_context_exit,

	.sched_disable = guc_context_sched_disable,

	.destroy = guc_context_destroy,

	.clone_virtual = guc_clone_virtual,
	.get_sibling = guc_virtual_get_sibling,
};

static int guc_parent_context_pin(struct intel_context *ce, void *vaddr)
{
	struct intel_engine_cs *engine = guc_virtual_get_sibling(ce->engine, 0);
	struct intel_guc *guc = ce_to_guc(ce);
	int ret;

	GEM_BUG_ON(!intel_context_is_parent(ce));
	GEM_BUG_ON(!intel_engine_is_virtual(ce->engine));

	ret = pin_guc_id(guc, ce);
	if (unlikely(ret < 0))
		return ret;

	return __guc_context_pin(ce, engine, vaddr);
}

static int guc_child_context_pin(struct intel_context *ce, void *vaddr)
{
	struct intel_engine_cs *engine = guc_virtual_get_sibling(ce->engine, 0);

	GEM_BUG_ON(!intel_context_is_child(ce));
	GEM_BUG_ON(!intel_engine_is_virtual(ce->engine));

	__intel_context_pin(ce->parallel.parent);
	return __guc_context_pin(ce, engine, vaddr);
}

static void guc_parent_context_unpin(struct intel_context *ce)
{
	struct intel_guc *guc = ce_to_guc(ce);

	GEM_BUG_ON(context_enabled(ce));
	GEM_BUG_ON(intel_context_is_barrier(ce));
	GEM_BUG_ON(!intel_context_is_parent(ce));
	GEM_BUG_ON(!intel_engine_is_virtual(ce->engine));

	unpin_guc_id(guc, ce);
	lrc_unpin(ce);
}

static void guc_child_context_unpin(struct intel_context *ce)
{
	GEM_BUG_ON(context_enabled(ce));
	GEM_BUG_ON(intel_context_is_barrier(ce));
	GEM_BUG_ON(!intel_context_is_child(ce));
	GEM_BUG_ON(!intel_engine_is_virtual(ce->engine));

	lrc_unpin(ce);
}

static void guc_child_context_post_unpin(struct intel_context *ce)
{
	GEM_BUG_ON(!intel_context_is_child(ce));
	GEM_BUG_ON(!intel_context_is_pinned(ce->parallel.parent));
	GEM_BUG_ON(!intel_engine_is_virtual(ce->engine));

	lrc_post_unpin(ce);
	intel_context_unpin(ce->parallel.parent);
}

static void guc_child_context_destroy(struct kref *kref)
{
	struct intel_context *ce = container_of(kref, typeof(*ce), ref);

	__guc_context_destroy(ce);
}

static const struct intel_context_ops virtual_parent_context_ops = {
	.flags = COPS_RUNTIME_CYCLES,
	.alloc = guc_virtual_context_alloc,

	.close = guc_context_close,

	.pre_pin = guc_context_pre_pin,
	.pin = guc_parent_context_pin,
	.unpin = guc_parent_context_unpin,
	.post_unpin = guc_context_post_unpin,

	.ban = guc_context_ban,

	.cancel_request = guc_context_cancel_request,

	.suspend = guc_context_suspend,
	.resume = guc_context_resume,

	.enter = guc_virtual_context_enter,
	.exit = guc_virtual_context_exit,

	.sched_disable = guc_context_sched_disable,

	.destroy = guc_context_destroy,

	.get_sibling = guc_virtual_get_sibling,
};

static const struct intel_context_ops virtual_child_context_ops = {
	.flags = COPS_RUNTIME_CYCLES,
	.alloc = guc_virtual_context_alloc,

	.pre_pin = guc_context_pre_pin,
	.pin = guc_child_context_pin,
	.unpin = guc_child_context_unpin,
	.post_unpin = guc_child_context_post_unpin,

	.cancel_request = guc_context_cancel_request,

	.suspend = guc_context_suspend,
	.resume = guc_context_resume,

	.enter = guc_virtual_context_enter,
	.exit = guc_virtual_context_exit,

	.destroy = guc_child_context_destroy,

	.get_sibling = guc_virtual_get_sibling,
};

/*
 * The below override of the breadcrumbs is enabled when the user configures a
 * context for parallel submission (multi-lrc, parent-child).
 *
 * The overridden breadcrumbs implements an algorithm which allows the GuC to
 * safely preempt all the hw contexts configured for parallel submission
 * between each BB. The contract between the i915 and GuC is if the parent
 * context can be preempted, all the children can be preempted, and the GuC will
 * always try to preempt the parent before the children. A handshake between the
 * parent / children breadcrumbs ensures the i915 holds up its end of the deal
 * creating a window to preempt between each set of BBs.
 */
static int emit_bb_start_parent_no_preempt_mid_batch(struct i915_request *rq,
						     u64 offset, u32 len,
						     const unsigned int flags);
static int emit_bb_start_child_no_preempt_mid_batch(struct i915_request *rq,
						    u64 offset, u32 len,
						    const unsigned int flags);
static u32 *
emit_fini_breadcrumb_parent_no_preempt_mid_batch(struct i915_request *rq,
						 u32 *cs);
static u32 *
emit_fini_breadcrumb_child_no_preempt_mid_batch(struct i915_request *rq,
						u32 *cs);

static struct intel_context *
guc_create_parallel(struct intel_engine_cs **engines,
		    unsigned int num_siblings,
		    unsigned int width)
{
	struct intel_engine_cs **siblings = NULL;
	struct intel_context *parent = NULL, *ce, *err;
	int i, j;

	siblings = kmalloc_array(num_siblings,
				 sizeof(*siblings),
				 GFP_KERNEL);
	if (!siblings)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < width; ++i) {
		for (j = 0; j < num_siblings; ++j)
			siblings[j] = engines[i * num_siblings + j];

		ce = intel_engine_create_virtual(siblings, num_siblings,
						 FORCE_VIRTUAL);
		if (IS_ERR(ce)) {
			err = ERR_CAST(ce);
			goto unwind;
		}

		if (i == 0) {
			parent = ce;
			parent->ops = &virtual_parent_context_ops;
		} else {
			ce->ops = &virtual_child_context_ops;
			intel_context_bind_parent_child(parent, ce);
		}
	}

	parent->parallel.fence_context = dma_fence_context_alloc(1);

	parent->engine->emit_bb_start =
		emit_bb_start_parent_no_preempt_mid_batch;
	parent->engine->emit_fini_breadcrumb =
		emit_fini_breadcrumb_parent_no_preempt_mid_batch;
	parent->engine->emit_fini_breadcrumb_dw =
		12 + 4 * parent->parallel.number_children;
	for_each_child(parent, ce) {
		ce->engine->emit_bb_start =
			emit_bb_start_child_no_preempt_mid_batch;
		ce->engine->emit_fini_breadcrumb =
			emit_fini_breadcrumb_child_no_preempt_mid_batch;
		ce->engine->emit_fini_breadcrumb_dw = 16;
	}

	kfree(siblings);
	return parent;

unwind:
	if (parent)
		intel_context_put(parent);
	kfree(siblings);
	return err;
}

static bool
guc_irq_enable_breadcrumbs(struct intel_breadcrumbs *b)
{
	struct intel_engine_cs *sibling;
	intel_engine_mask_t tmp, mask = b->engine_mask;
	bool result = false;

	for_each_engine_masked(sibling, b->irq_engine->gt, mask, tmp)
		result |= intel_engine_irq_enable(sibling);

	return result;
}

static void
guc_irq_disable_breadcrumbs(struct intel_breadcrumbs *b)
{
	struct intel_engine_cs *sibling;
	intel_engine_mask_t tmp, mask = b->engine_mask;

	for_each_engine_masked(sibling, b->irq_engine->gt, mask, tmp)
		intel_engine_irq_disable(sibling);
}

static void guc_init_breadcrumbs(struct intel_engine_cs *engine)
{
	int i;

	/*
	 * In GuC submission mode we do not know which physical engine a request
	 * will be scheduled on, this creates a problem because the breadcrumb
	 * interrupt is per physical engine. To work around this we attach
	 * requests and direct all breadcrumb interrupts to the first instance
	 * of an engine per class. In addition all breadcrumb interrupts are
	 * enabled / disabled across an engine class in unison.
	 */
	for (i = 0; i < MAX_ENGINE_INSTANCE; ++i) {
		struct intel_engine_cs *sibling =
			engine->gt->engine_class[engine->class][i];

		if (sibling) {
			if (engine->breadcrumbs != sibling->breadcrumbs) {
				intel_breadcrumbs_put(engine->breadcrumbs);
				engine->breadcrumbs =
					intel_breadcrumbs_get(sibling->breadcrumbs);
			}
			break;
		}
	}

	if (engine->breadcrumbs) {
		engine->breadcrumbs->engine_mask |= engine->mask;
		engine->breadcrumbs->irq_enable = guc_irq_enable_breadcrumbs;
		engine->breadcrumbs->irq_disable = guc_irq_disable_breadcrumbs;
	}
}

static void guc_bump_inflight_request_prio(struct i915_request *rq, int prio)
{
	struct intel_context *ce = request_to_scheduling_context(rq);
	u8 new_guc_prio = map_i915_prio_to_guc_prio(prio);
	u8 old_guc_prio = READ_ONCE(rq->guc_prio);
	unsigned long flags;

	/* Short circuit function */
	if (prio < I915_PRIORITY_NORMAL ||
	    old_guc_prio == GUC_PRIO_FINI ||
	    (old_guc_prio != GUC_PRIO_INIT &&
	     !new_guc_prio_higher(old_guc_prio, new_guc_prio)))
		return;

	spin_lock_irqsave(&ce->guc_state.lock, flags);
	if (rq->guc_prio == GUC_PRIO_FINI)
		goto unlock;

	if (rq->guc_prio != GUC_PRIO_INIT) {
		if (!new_guc_prio_higher(rq->guc_prio, new_guc_prio))
			goto unlock;

		sub_context_inflight_prio(ce, rq->guc_prio);
	}

	rq->guc_prio = new_guc_prio;
	add_context_inflight_prio(ce, rq->guc_prio);

	update_context_prio(ce);
unlock:
	spin_unlock_irqrestore(&ce->guc_state.lock, flags);
}

static void guc_retire_inflight_request_prio(struct i915_request *rq)
{
	struct intel_context *ce = request_to_scheduling_context(rq);

	spin_lock(&ce->guc_state.lock);
	guc_prio_fini(rq, ce);
	spin_unlock(&ce->guc_state.lock);
}

static void setup_hwsp(struct intel_engine_cs *engine)
{
	intel_engine_set_hwsp_writemask(engine, ~0u); /* HWSTAM */

	ENGINE_WRITE_FW(engine,
			RING_HWS_PGA,
			i915_ggtt_offset(engine->status_page.vma));
}

static void start_engine(struct intel_engine_cs *engine)
{
	ENGINE_WRITE_FW(engine,
			RING_MODE_GEN7,
			_MASKED_BIT_ENABLE(GEN11_GFX_DISABLE_LEGACY_MODE));

	ENGINE_WRITE_FW(engine, RING_MI_MODE, _MASKED_BIT_DISABLE(STOP_RING));
	ENGINE_POSTING_READ(engine, RING_MI_MODE);
}

static int guc_resume(struct intel_engine_cs *engine)
{
	assert_forcewakes_active(engine->uncore, FORCEWAKE_ALL);

	intel_mocs_init_engine(engine);

	intel_breadcrumbs_reset(engine->breadcrumbs);

	setup_hwsp(engine);
	start_engine(engine);

	if (engine->flags & I915_ENGINE_FIRST_RENDER_COMPUTE)
		xehp_enable_ccs_engines(engine);

	return 0;
}

static bool guc_sched_engine_disabled(struct i915_sched_engine *sched_engine)
{
#ifdef BPM_TASKLET_STRUCT_CALLBACK_NOT_PRESENT
	return sched_engine->tasklet.func != guc_submission_tasklet;
#else
	return sched_engine->tasklet.callback != guc_submission_tasklet;
#endif
}

static int vf_guc_resume(struct intel_engine_cs *engine)
{
	intel_breadcrumbs_reset(engine->breadcrumbs);
	return 0;
}

static void guc_set_default_submission(struct intel_engine_cs *engine)
{
	engine->submit_request = guc_submit_request;
}

static inline int guc_kernel_context_pin(struct intel_guc *guc,
					 struct intel_context *ce)
{
	intel_wakeref_t wf;
	int ret;

	/*
	 * Note: we purposefully do not check the returns below because
	 * the registration can only fail if a reset is just starting.
	 * This is called at the end of reset so presumably another reset
	 * isn't happening and even it did this code would be run again.
	 */

	if (context_guc_id_invalid(ce)) {
		ret = pin_guc_id(guc, ce);

		if (ret < 0)
			return ret;
	}

	if (!test_bit(CONTEXT_GUC_INIT, &ce->flags))
		guc_context_init(ce);

	with_intel_gt_pm_async(guc_to_gt(guc), wf) {
		ret = try_context_registration(ce, true);
		if (ret)
			unpin_guc_id(guc, ce);
	}

	return ret;
}

static inline int guc_init_submission(struct intel_guc *guc)
{
	struct intel_gt *gt = guc_to_gt(guc);
	struct intel_context *ce;

	/* make sure all descriptors are clean... */
	xa_destroy(&guc->context_lookup);

	/*
	 * A reset might have occurred while we had a pending stalled request,
	 * so make sure we clean that up.
	 */
	guc->stalled_request = NULL;
	guc->submission_stall_reason = STALL_NONE;

	/*
	 * Some contexts might have been pinned before we enabled GuC
	 * submission, so we need to add them to the GuC bookeeping.
	 * Also, after a reset the of the GuC we want to make sure that the
	 * information shared with GuC is properly reset. The kernel LRCs are
	 * not attached to the gem_context, so they need to be added separately.
	 */

	list_for_each_entry(ce, &gt->pinned_contexts, pinned_contexts_link) {
		int ret = guc_kernel_context_pin(guc, ce);
		if (ret) {
			/* No point in trying to clean up as i915 will wedge on failure */
			return ret;
		}
	}

	return 0;
}

static void guc_release(struct intel_engine_cs *engine)
{
	tasklet_kill(&engine->sched_engine->tasklet);

	intel_engine_cleanup_common(engine);
}

static void virtual_guc_bump_serial(struct intel_engine_cs *engine)
{
	struct intel_engine_cs *e;
	intel_engine_mask_t tmp, mask = engine->mask;

	for_each_engine_masked(e, engine->gt, mask, tmp)
		e->serial++;
}

static void guc_fake_irq_enable(struct intel_engine_cs *engine)
{
	struct intel_gt *gt = engine->gt;

	lockdep_assert_held(gt->irq_lock);

	if (!gt->fake_int.int_enabled) {
		gt->fake_int.int_enabled = true;
		intel_boost_fake_int_timer(gt, true);
	}
}

static void guc_fake_irq_disable(struct intel_engine_cs *engine)
{
	struct intel_gt *gt = engine->gt;

	lockdep_assert_held(gt->irq_lock);

	if (gt->fake_int.int_enabled) {
		gt->fake_int.int_enabled = false;
		intel_boost_fake_int_timer(gt, false);
	}
}

static void guc_default_vfuncs(struct intel_engine_cs *engine)
{
	/* Default vfuncs which can be overridden by each engine. */

	engine->resume = guc_resume;

	engine->cops = &guc_context_ops;
	engine->request_alloc = guc_request_alloc;
	engine->remove_active_request = remove_from_context;

	/*
	 * guc_engine_reset_prepare causes media workload hang for PVC
	 * A0. Disable this for PVC A0 steppings.
	 */
	if (IS_SRIOV_VF(engine->i915) ||
	    IS_PVC_BD_STEP(engine->gt->i915, STEP_A0, STEP_B0))
		engine->reset.prepare = guc_reset_nop;
	else
		engine->reset.prepare = guc_engine_reset_prepare;

	engine->reset.rewind = guc_rewind_nop;
	engine->reset.cancel = guc_reset_nop;
	engine->reset.finish = guc_reset_nop;

	engine->emit_flush = gen8_emit_flush_xcs;
	engine->emit_init_breadcrumb = gen8_emit_init_breadcrumb;
	engine->emit_fini_breadcrumb = gen8_emit_fini_breadcrumb_xcs;
	if (GRAPHICS_VER(engine->i915) >= 12) {
		engine->emit_fini_breadcrumb = gen12_emit_fini_breadcrumb_xcs;
		engine->emit_flush = gen12_emit_flush_xcs;
	}
	engine->set_default_submission = guc_set_default_submission;
	if (busy_type_is_v1(&engine->gt->uc.guc)) {
		/*
		 * v1 busyness in VF is not supported, so prevent the counters
		 * from getting created in sysfs.
		 */
		if (!IS_SRIOV_VF(engine->i915)) {
			engine->busyness = busy_v1_guc_engine_busyness;
			engine->busyness_ticks = busy_v1_guc_engine_busyness_ticks;
			engine->total_active_ticks = busy_v1_intel_guc_total_active_ticks;
		}
	} else if (busy_type_is_v2(&engine->gt->uc.guc)) {
		engine->busyness = busy_v2_guc_engine_busyness;
		engine->busyness_ticks = busy_v2_guc_engine_busyness_ticks;
		engine->total_active_ticks = busy_v2_intel_guc_total_active_ticks;
	} else if (busy_type_is_v3(&engine->gt->uc.guc)) {
		engine->busyness = busy_v3_guc_engine_busyness;
		engine->busyness_ticks = busy_v3_guc_engine_activity_ticks;
		engine->total_active_ticks = busy_v3_intel_guc_total_active_ticks;
	}

	/* Wa:16014207253 */
	if (engine->gt->fake_int.enabled) {
		engine->irq_enable = guc_fake_irq_enable;
		engine->irq_disable = guc_fake_irq_disable;
	}

	engine->flags |= I915_ENGINE_HAS_SCHEDULER;
	engine->flags |= I915_ENGINE_HAS_PREEMPTION;
	engine->flags |= I915_ENGINE_HAS_TIMESLICES;
	engine->flags |= I915_ENGINE_SUPPORTS_STATS;

	/* Wa_14014475959:dg2 */
	if (engine->class == COMPUTE_CLASS)
		if (IS_MTL_GRAPHICS_STEP(engine->i915, M, STEP_A0, STEP_B0) ||
		    IS_DG2(engine->i915))
			engine->flags |= I915_ENGINE_USES_WA_HOLD_CCS_SWITCHOUT;

	/*
	 * TODO: GuC supports timeslicing and semaphores as well, but they're
	 * handled by the firmware so some minor tweaks are required before
	 * enabling.
	 *
	 * engine->flags |= I915_ENGINE_HAS_SEMAPHORES;
	 */

	engine->emit_bb_start = gen8_emit_bb_start;
	if (GRAPHICS_VER_FULL(engine->i915) >= IP_VER(12, 50))
		engine->emit_bb_start = xehp_emit_bb_start;
}

static void rcs_submission_override(struct intel_engine_cs *engine)
{
	switch (GRAPHICS_VER(engine->i915)) {
	case 12:
		engine->emit_flush = gen12_emit_flush_rcs;
		engine->emit_fini_breadcrumb = gen12_emit_fini_breadcrumb_rcs;
		break;
	case 11:
		engine->emit_flush = gen11_emit_flush_rcs;
		engine->emit_fini_breadcrumb = gen11_emit_fini_breadcrumb_rcs;
		break;
	default:
		engine->emit_flush = gen8_emit_flush_rcs;
		engine->emit_fini_breadcrumb = gen8_emit_fini_breadcrumb_rcs;
		break;
	}
}

static inline void guc_default_irqs(struct intel_engine_cs *engine)
{
	engine->irq_keep_mask = GT_RENDER_USER_INTERRUPT;
	intel_engine_set_irq_handler(engine, cs_irq_handler);
}

static void guc_sched_engine_destroy(struct kref *kref)
{
	struct i915_sched_engine *sched_engine =
		container_of(kref, typeof(*sched_engine), ref);
#ifdef BPM_TASKLET_STRUCT_CALLBACK_NOT_PRESENT
	struct intel_guc *guc = (struct intel_guc *)sched_engine->tasklet.data;
#else
	struct intel_guc *guc = sched_engine->private_data;
#endif

	guc->sched_engine = NULL;
	tasklet_kill(&sched_engine->tasklet); /* flush the callback */
	kfree(sched_engine);
}

int intel_guc_submission_setup(struct intel_engine_cs *engine)
{
	struct drm_i915_private *i915 = engine->i915;
	struct intel_guc *guc = &engine->gt->uc.guc;

	/*
	 * The setup relies on several assumptions (e.g. irqs always enabled)
	 * that are only valid on gen11+
	 */
	GEM_BUG_ON(GRAPHICS_VER(i915) < 11);

	if (!guc->sched_engine) {
		guc->sched_engine = i915_sched_engine_create(ENGINE_VIRTUAL);
		if (!guc->sched_engine)
			return -ENOMEM;

		guc->sched_engine->disabled = guc_sched_engine_disabled;
#ifndef BPM_TASKLET_STRUCT_CALLBACK_NOT_PRESENT
		guc->sched_engine->private_data = guc;
#endif
		guc->sched_engine->destroy = guc_sched_engine_destroy;
		guc->sched_engine->bump_inflight_request_prio =
			guc_bump_inflight_request_prio;
		guc->sched_engine->retire_inflight_request_prio =
			guc_retire_inflight_request_prio;
#ifdef BPM_TASKLET_STRUCT_CALLBACK_NOT_PRESENT
		guc->sched_engine->tasklet.func = guc_submission_tasklet;
		guc->sched_engine->tasklet.data = (uintptr_t) guc;
#else
		tasklet_setup(&guc->sched_engine->tasklet,
			      guc_submission_tasklet);
#endif
	}
	i915_sched_engine_put(engine->sched_engine);
	engine->sched_engine = i915_sched_engine_get(guc->sched_engine);

	guc_default_vfuncs(engine);
	guc_default_irqs(engine);
	guc_init_breadcrumbs(engine);

	if (engine->flags & I915_ENGINE_HAS_RCS_REG_STATE)
		rcs_submission_override(engine);

	if (IS_SRIOV_VF(engine->i915))
		engine->resume = vf_guc_resume;

	/* Finally, take ownership and responsibility for cleanup! */
	engine->release = guc_release;

	return 0;
}

struct scheduling_policy {
	/* internal data */
	u32 max_words, num_words;
	u32 count;
	/* API data */
	struct guc_update_scheduling_policy h2g;
};

static u32 __guc_scheduling_policy_action_size(struct scheduling_policy *policy)
{
	u32 *start = (void *)&policy->h2g;
	u32 *end = policy->h2g.data + policy->num_words;
	size_t delta = end - start;

	return delta;
}

static struct scheduling_policy *__guc_scheduling_policy_start_klv(struct scheduling_policy *policy)
{
	policy->h2g.header.action = INTEL_GUC_ACTION_UPDATE_SCHEDULING_POLICIES_KLV;
	policy->max_words = ARRAY_SIZE(policy->h2g.data);
	policy->num_words = 0;
	policy->count = 0;

	return policy;
}

static void __guc_scheduling_policy_add_klv(struct scheduling_policy *policy,
					    u32 action, u32 *data, u32 len)
{
	u32 *klv_ptr = policy->h2g.data + policy->num_words;

	GEM_BUG_ON((policy->num_words + 1 + len) > policy->max_words);
	*(klv_ptr++) = FIELD_PREP(GUC_KLV_0_KEY, action) |
		       FIELD_PREP(GUC_KLV_0_LEN, len);
	memcpy(klv_ptr, data, sizeof(u32) * len);
	policy->num_words += 1 + len;
	policy->count++;
}

static int __guc_action_set_scheduling_policies(struct intel_guc *guc,
						struct scheduling_policy *policy)
{
	int ret;

	ret = intel_guc_send(guc, (u32 *)&policy->h2g,
			     __guc_scheduling_policy_action_size(policy));
	if (ret < 0) {
		guc_probe_error(guc, "Failed to configure global scheduling policies: %pe!\n",
				ERR_PTR(ret));
		return ret;
	}

	if (ret != policy->count) {
		guc_warn(guc, "global scheduler policy processed %d of %d KLVs!",
			 ret, policy->count);
		if (ret > policy->count)
			return -EPROTO;
	}

	return 0;
}

static int guc_init_global_schedule_policy(struct intel_guc *guc)
{
	struct scheduling_policy policy;
	intel_wakeref_t wakeref;
	int ret;

	if (GUC_SUBMIT_VER(guc) < MAKE_GUC_VER(1, 1, 0))
		return 0;

	__guc_scheduling_policy_start_klv(&policy);

	with_intel_gt_pm(guc_to_gt(guc), wakeref) {
		u32 yield[] = {
			GLOBAL_SCHEDULE_POLICY_RC_YIELD_DURATION,
			GLOBAL_SCHEDULE_POLICY_RC_YIELD_RATIO,
		};

		__guc_scheduling_policy_add_klv(&policy,
						GUC_SCHEDULING_POLICIES_KLV_ID_RENDER_COMPUTE_YIELD,
						yield, ARRAY_SIZE(yield));

		ret = __guc_action_set_scheduling_policies(guc, &policy);
	}

	return ret;
}

static void guc_route_semaphores(struct intel_guc *guc, bool to_guc)
{
	struct intel_gt *gt = guc_to_gt(guc);
	u32 val, val2;

	if (GRAPHICS_VER(gt->i915) < 12)
		return;

	if (to_guc) {
		val = GUC_SEM_INTR_ROUTE_TO_GUC | GUC_SEM_INTR_ENABLE_ALL;
		val2 = GUC_SEM_INTR_MASK_NONE;
	} else {
		val = 0;
		val2 = GUC_SEM_INTR_MASK_ALL;
	}

	intel_uncore_write(gt->uncore, GEN12_GUC_SEM_INTR_ENABLES, val);

	if (HAS_SEMAPHORE_XEHPSDV(gt->i915))
		intel_uncore_write(gt->uncore, XEHP_GUC_SEM_INTR_MASK,
				   val2);
}

int intel_guc_submission_enable(struct intel_guc *guc)
{
	int ret;

	/* Semaphore interrupt enable and route to GuC */
	guc_route_semaphores(guc, true);

	ret = guc_init_submission(guc);
	if (ret)
		goto fail_sem;

	ret = guc_init_engine_stats(guc);
	if (ret)
		goto fail_sem;

	ret = guc_init_global_schedule_policy(guc);
	if (ret)
		goto fail_stats;

	return 0;

fail_stats:
	guc_fini_engine_stats(guc);
fail_sem:
	guc_route_semaphores(guc, false);
	return ret;
}

/* Note: By the time we're here, GuC may have already been reset */
void intel_guc_submission_disable(struct intel_guc *guc)
{
	if (guc_to_gt(guc)->i915->quiesce_gpu)
		return;

	guc_fini_engine_stats(guc);

	/* Semaphore interrupt disable and route to host */
	guc_route_semaphores(guc, false);
}

static bool __guc_submission_supported(struct intel_guc *guc)
{
	/* GuC submission is unavailable for pre-Gen11 */
	return intel_guc_is_supported(guc) &&
	       GRAPHICS_VER(guc_to_gt(guc)->i915) >= 11;
}

static bool __guc_submission_selected(struct intel_guc *guc)
{
	struct drm_i915_private *i915 = guc_to_gt(guc)->i915;

	if (!intel_guc_submission_is_supported(guc))
		return false;

	return i915->params.enable_guc & ENABLE_GUC_SUBMISSION;
}

int intel_guc_sched_disable_gucid_threshold_max(struct intel_guc *guc)
{
	return guc->submission_state.num_guc_ids - number_mlrc_guc_id(guc);
}

static void reset_fail_worker_func(struct work_struct *w);

/*
 * This default value of 33 milisecs (+1 milisec round up) ensures 30fps or higher
 * workloads are able to enjoy the latency reduction when delaying the schedule-disable
 * operation. This matches the 30fps game-render + encode (real world) workload this
 * knob was tested against.
 */
#define SCHED_DISABLE_DELAY_MS	34

/*
 * A threshold of 75% is a reasonable starting point considering that real world apps
 * generally don't get anywhere near this.
 */
#define NUM_SCHED_DISABLE_GUCIDS_DEFAULT_THRESHOLD(__guc) \
	(((intel_guc_sched_disable_gucid_threshold_max(guc)) * 3) / 4)

void intel_guc_submission_init_early(struct intel_guc *guc)
{
	xa_init_flags(&guc->context_lookup, XA_FLAGS_LOCK_IRQ);

	spin_lock_init(&guc->submission_state.lock);
	INIT_LIST_HEAD(&guc->submission_state.guc_id_list);
	ida_init(&guc->submission_state.guc_ids);
	INIT_LIST_HEAD(&guc->submission_state.destroyed_contexts);
	INIT_WORK(&guc->submission_state.destroyed_worker,
		  destroyed_worker_func);
	INIT_WORK(&guc->submission_state.reset_fail_worker,
		  reset_fail_worker_func);

	guc->submission_state.sched_disable_delay_ms = SCHED_DISABLE_DELAY_MS;
	guc->submission_state.num_guc_ids = GUC_MAX_CONTEXT_ID;
	guc->submission_state.sched_disable_gucid_threshold =
		NUM_SCHED_DISABLE_GUCIDS_DEFAULT_THRESHOLD(guc);
	guc->submission_supported = __guc_submission_supported(guc);
	guc->submission_selected = __guc_submission_selected(guc);

	spin_lock_init(&guc->busy.lock);
	INIT_DELAYED_WORK(&guc->busy.work, busy_v1_guc_timestamp_ping);
}

static inline struct intel_context *
g2h_context_lookup(struct intel_guc *guc, u32 ctx_id)
{
	struct intel_context *ce;

	if (unlikely(ctx_id >= GUC_MAX_CONTEXT_ID)) {
		guc_err(guc, "Invalid ctx_id %u\n", ctx_id);
		return NULL;
	}

	ce = __get_context(guc, ctx_id);
	if (unlikely(!ce)) {
		guc_err(guc, "Context is NULL, ctx_id %u\n", ctx_id);
		return NULL;
	}

	if (unlikely(intel_context_is_child(ce))) {
		guc_err(guc, "Context is child, ctx_id %u\n", ctx_id);
		return NULL;
	}

	return ce;
}

int intel_guc_deregister_done_process_msg(struct intel_guc *guc,
					  const u32 *msg,
					  u32 len)
{
	struct intel_context *ce;
	u32 ctx_id;

	if (unlikely(len < 1)) {
		guc_err(guc, "Invalid length %u\n", len);
		return -EPROTO;
	}
	ctx_id = msg[0];

	ce = g2h_context_lookup(guc, ctx_id);
	if (unlikely(!ce))
		return -EPROTO;

	trace_intel_context_deregister_done(ce);
	WRITE_ONCE(ce->engine->stats.irq.count,
		   READ_ONCE(ce->engine->stats.irq.count) + 1);

#ifdef CPTCFG_DRM_I915_SELFTEST
	if (unlikely(ce->drop_deregister)) {
		ce->drop_deregister = false;
		return 0;
	}
#endif

	if (context_wait_for_deregister_to_register(ce)) {
		/*
		 * Previous owner of this guc_id has been deregistered, now safe
		 * register this context.
		 */
		register_context(ce, true);
		guc_signal_context_fence(ce);
		intel_context_put(ce);
	} else if (context_destroyed(ce)) {
		/* Context has been destroyed */
		intel_gt_pm_put_async_untracked(guc_to_gt(guc));
		release_guc_id(guc, ce);
		__guc_context_destroy(ce);
	}

	decr_outstanding_submission_g2h(guc);

	return 0;
}

int intel_guc_engine_sched_done_process_msg(struct intel_guc *guc,
					    const u32 *msg,
					    u32 len)
{
	if (unlikely(len < 2)) {
		guc_dbg(guc, "Invalid length %u\n", len);
		return -EPROTO;
	}

	decr_outstanding_submission_g2h(guc);

	return 0;
}

int intel_guc_sched_done_process_msg(struct intel_guc *guc,
				     const u32 *msg,
				     u32 len)
{
	struct intel_context *ce;
	unsigned long flags;
	u32 ctx_id, state;

	if (unlikely(len < 2)) {
		guc_err(guc, "Invalid length %u\n", len);
		return -EPROTO;
	}
	ctx_id = msg[0];
	state = msg[1];

	ce = g2h_context_lookup(guc, ctx_id);
	if (unlikely(!ce))
		return -EPROTO;

	if (unlikely(context_destroyed(ce) ||
		     (!context_pending_enable(ce) &&
		     !context_pending_disable(ce)))) {
		guc_err(guc, "Bad context sched_state 0x%x, ctx_id %u, state %u\n",
			ce->guc_state.sched_state, ctx_id, state);
		return -EPROTO;
	}

	trace_intel_context_sched_done(ce);
	WRITE_ONCE(ce->engine->stats.irq.count,
		   READ_ONCE(ce->engine->stats.irq.count) + 1);

	if (state == GUC_CONTEXT_ENABLE) {
		if (!context_pending_enable(ce)) {
			guc_err(guc, "Unexpected context enable done: sched_state 0x%x, ctx_id %u\n",
				ce->guc_state.sched_state, ctx_id);
			return -EPROTO;
		}

#ifdef CPTCFG_DRM_I915_SELFTEST
		if (unlikely(ce->drop_schedule_enable)) {
			ce->drop_schedule_enable = false;
			return 0;
		}
#endif

		spin_lock_irqsave(&ce->guc_state.lock, flags);
		clr_context_pending_enable(ce);
		spin_unlock_irqrestore(&ce->guc_state.lock, flags);
	} else if (state == GUC_CONTEXT_DISABLE) {
		bool banned;

		if (!context_pending_disable(ce)) {
			guc_err(guc, "Unexpected context disable done: sched_state 0x%x, ctx_id %u\n",
				ce->guc_state.sched_state, ctx_id);
			return -EPROTO;
		}

#ifdef CPTCFG_DRM_I915_SELFTEST
		if (unlikely(ce->drop_schedule_disable)) {
			ce->drop_schedule_disable = false;
			return 0;
		}
#endif

		/*
		 * Unpin must be done before __guc_signal_context_fence,
		 * otherwise a race exists between the requests getting
		 * submitted + retired before this unpin completes resulting in
		 * the pin_count going to zero and the context still being
		 * enabled.
		 */
		intel_context_sched_disable_unpin(ce);

		spin_lock_irqsave(&ce->guc_state.lock, flags);
		banned = context_banned(ce);
		clr_context_banned(ce);
		clr_context_pending_disable(ce);
		__guc_signal_context_fence(ce);
		guc_blocked_fence_complete(ce);
		spin_unlock_irqrestore(&ce->guc_state.lock, flags);

		if (banned)
			guc_cancel_context_requests(ce);
	} else {
		guc_err(guc, "Unexpected context state done: sched_state 0x%x, ctx_id %u, state %u\n",
			ce->guc_state.sched_state, ctx_id, state);
		return -EPROTO;
	}

	decr_outstanding_submission_g2h(guc);
	intel_context_put(ce);

	return 0;
}

static void capture_error_state(struct intel_guc *guc,
				struct intel_context *ce)
{
	struct intel_gt *gt = guc_to_gt(guc);
	struct i915_page_compress *compress;
	struct i915_gpu_coredump *error;
	struct intel_engine_cs *e;
	struct i915_request *rq;
	intel_engine_mask_t tmp;

	rcu_read_lock();
	rq = intel_context_find_active_request(ce);
	rq = rq && __i915_request_has_started(rq) ? i915_request_get_rcu(rq) : NULL;
	rcu_read_unlock();
	if (!rq)
		return;

	if (!rcu_access_pointer(rq->context->gem_context))
		goto out;

	error = i915_gpu_coredump_alloc(gt->i915, GFP_KERNEL);
	if (!error)
		goto out;

	error->gt = intel_gt_coredump_alloc(gt, GFP_KERNEL, CORE_DUMP_FLAG_IS_GUC_CAPTURE);
	if (!error->gt)
		goto put_coredump;

	compress = i915_vma_capture_prepare(error->gt);

	for_each_engine_masked(e, gt, rq->execution_mask, tmp) {
		struct intel_engine_capture_vma *capture = NULL;
		struct intel_engine_coredump *ee;

		/* Capture all register state on any engine this request may have run on */
		ee = intel_engine_coredump_alloc(e, GFP_KERNEL, CORE_DUMP_FLAG_IS_GUC_CAPTURE);
		if (!ee)
			continue;

		capture = intel_engine_coredump_add_request(ee, rq, capture, GFP_KERNEL, compress);
		intel_engine_coredump_add_vma(ee, capture, compress);

		ee->hung = is_power_of_2(rq->execution_mask);
		if (intel_guc_capture_is_matching_engine(gt, ce, e)) {
			intel_guc_capture_get_matching_node(gt, ee, ce);
			ee->hung = true;
		}

		ee->next = error->gt->engine;
		error->gt->engine = ee;
	}

	if (compress)
		i915_vma_capture_finish(error->gt, compress);

	i915_error_state_store(error);
put_coredump:
	i915_gpu_coredump_put(error);
out:
	i915_request_put(rq);
}

static void guc_context_replay(struct intel_context *ce)
{
	struct i915_sched_engine *sched_engine = ce->engine->sched_engine;

	__guc_reset_context(ce, ce->engine->mask);
	tasklet_hi_schedule(&sched_engine->tasklet);
}

static void guc_handle_context_reset(struct intel_guc *guc,
				     struct intel_context *ce)
{
	trace_intel_context_reset(ce);

	guc_dbg(guc, "Got context reset notification: 0x%04X on %s, blocked = %s, banned = %s, closed = %s\n",
		ce->guc_id.id, ce->engine->name,
		str_yes_no(context_blocked(ce)),
		str_yes_no(intel_context_is_banned(ce)),
		str_yes_no(intel_context_is_closed(ce)));

	/*
	 * XXX: Racey if request cancellation has occurred, see comment in
	 * __guc_reset_context().
	 */
	if (likely(!intel_context_is_banned(ce) && !context_blocked(ce))) {
		atomic_inc(&guc_to_gt(guc)->reset.engines_reset_count);
		if (intel_context_set_coredump(ce))
			capture_error_state(guc, ce);
		guc_context_replay(ce);
	}
}

int intel_guc_context_reset_process_msg(struct intel_guc *guc,
					const u32 *msg, u32 len)
{
	struct intel_context *ce;
	int ctx_id;

	if (unlikely(len != 1)) {
		guc_err(guc, "Invalid length %u", len);
		return -EPROTO;
	}

	ctx_id = msg[0];

	/*
	 * The context lookup uses the xarray but lookups only require an RCU lock
	 * not the full spinlock. So take the lock explicitly and keep it until the
	 * context has been reference count locked to ensure it can't be destroyed
	 * asynchronously until the reset is done.
	 */
	rcu_read_lock();
	ce = g2h_context_lookup(guc, ctx_id);
	if (ce)
		ce = intel_context_get_rcu(ce);
	rcu_read_unlock();
	if (unlikely(!ce))
		return -EPROTO;

	guc_handle_context_reset(guc, ce);
	intel_context_put(ce);

	return 0;
}

int intel_guc_error_capture_process_msg(struct intel_guc *guc,
					const u32 *msg, u32 len)
{
	u32 status;

	if (unlikely(len != 1)) {
		guc_dbg(guc, "Invalid length %u", len);
		return -EPROTO;
	}

	status = msg[0] & INTEL_GUC_STATE_CAPTURE_EVENT_STATUS_MASK;
	if (status == INTEL_GUC_STATE_CAPTURE_EVENT_STATUS_NOSPACE)
		guc_warn(guc, "No space for error capture");

	intel_guc_capture_process(guc);

	return 0;
}

struct intel_engine_cs *
intel_guc_lookup_engine(struct intel_guc *guc, u8 guc_class, u8 instance)
{
	struct intel_gt *gt = guc_to_gt(guc);
	u8 engine_class = guc_class_to_engine_class(guc_class);

	/* Class index is checked in class converter */
	GEM_BUG_ON(instance > MAX_ENGINE_INSTANCE);

	return gt->engine_class[engine_class][instance];
}

static void reset_fail_worker_func(struct work_struct *w)
{
	struct intel_guc *guc = container_of(w, struct intel_guc,
					submission_state.reset_fail_worker);
	struct intel_gt *gt = guc_to_gt(guc);
	intel_engine_mask_t reset_fail_mask;
	unsigned long flags;

	spin_lock_irqsave(&guc->submission_state.lock, flags);
	reset_fail_mask = guc->submission_state.reset_fail_mask;
	guc->submission_state.reset_fail_mask = 0;
	spin_unlock_irqrestore(&guc->submission_state.lock, flags);

	if (likely(reset_fail_mask)) {
		struct intel_engine_cs *engine;
		enum intel_engine_id id;

		/*
		 * GuC is toast at this point - it dead loops after sending the failed
		 * reset notification. So need to manually determine the guilty context.
		 * Note that it should be reliable to do this here because the GuC is
		 * toast and will not be scheduling behind the KMD's back.
		 */
		for_each_engine_masked(engine, gt, reset_fail_mask, id)
			intel_engine_reset_failed_uevent(engine);

		intel_gt_handle_error(gt, reset_fail_mask,
				      I915_ERROR_CAPTURE,
				      "GuC failed to reset engine mask=0x%x",
				      reset_fail_mask);
	}
}

int intel_guc_engine_failure_process_msg(struct intel_guc *guc,
					 const u32 *msg, u32 len)
{
	struct intel_engine_cs *engine;
	unsigned long flags;
	u8 guc_class, instance;
	u32 reason, gdrst;

	if (unlikely(len != 3)) {
		guc_err(guc, "Invalid length %u", len);
		return -EPROTO;
	}

	guc_class = msg[0];
	instance = msg[1];
	reason = msg[2];

	engine = intel_guc_lookup_engine(guc, guc_class, instance);
	if (unlikely(!engine)) {
		guc_err(guc, "Invalid engine %d:%d", guc_class, instance);
		return -EPROTO;
	}

	/*
	 * This is an unexpected failure of a hardware feature. So, log a real
	 * error message not just the informational that comes with the reset.
	 */
	gdrst = intel_uncore_read_fw(engine->uncore, GEN6_GDRST);
	guc_err(guc, "Engine reset request failed on %d:%d (%s) because 0x%X, GDRST = 0x%08X\n",
		guc_class, instance, engine->name, reason, gdrst);

	if (gdrst) {
		int err = __intel_wait_for_register_fw(engine->uncore, GEN6_GDRST, ~0U, 0, 500, 0, NULL);
		if (err)
			guc_err(guc, "i915 wait for GDRST also failed: %d [on %d:%d (%s)]\n",
				err, guc_class, instance, engine->name);
	}

	spin_lock_irqsave(&guc->submission_state.lock, flags);
	guc->submission_state.reset_fail_mask |= engine->mask;
	spin_unlock_irqrestore(&guc->submission_state.lock, flags);

	/*
	 * A GT reset flushes this worker queue (G2H handler) so we must use
	 * another worker to trigger a GT reset.
	 */
	queue_work(system_unbound_wq, &guc->submission_state.reset_fail_worker);

	return 0;
}

void intel_guc_submission_print_info(struct intel_guc *guc,
				     struct drm_printer *p,
				     int indent)
{
	struct i915_sched_engine *sched_engine = guc->sched_engine;
	struct rb_node *rb;
	unsigned long flags;

	if (!sched_engine)
		return;

	i_printf(p, indent, "Submission API Version: %d.%d.%d\n",
		 guc->submission_version.major, guc->submission_version.minor,
		 guc->submission_version.patch);
	i_printf(p, indent, "Outstanding G2H: %u\n",
		 atomic_read(&guc->outstanding_submission_g2h));

	if (guc->stalled_request || !RB_EMPTY_ROOT(&sched_engine->queue.rb_root)) {
		spin_lock_irqsave(&sched_engine->lock, flags);
		if (guc->stalled_request)
			i915_request_show(p, guc->stalled_request, "Stalled: ", indent);
		i_printf(p, indent, "Tasklet:\n");
		for (rb = rb_first_cached(&sched_engine->queue); rb; rb = rb_next(rb)) {
			struct i915_request *rq;
			int skip = 0;

			priolist_for_each_request(rq, to_priolist(rb)) {
				if (skip++ < 8)
					i915_request_show(p, rq, "", indent + 2);
			}
			if (skip > 8)
				i_printf(p, indent, "... skipped %d requests\n", skip - 8);
		}
		spin_unlock_irqrestore(&sched_engine->lock, flags);
	}
}

void intel_guc_submission_print_context_info(struct intel_guc *guc,
					     struct drm_printer *p,
					     int indent)
{
	struct intel_context *ce;
	unsigned long index;

	rcu_read_lock();
	xa_for_each(&guc->context_lookup, index, ce) {
		GEM_BUG_ON(intel_context_is_child(ce));

		intel_context_show(ce, p, indent);

		if (intel_context_is_parent(ce)) {
			struct intel_context *child;
			int i;

			i_printf(p, indent + 2, "Number children: %u\n",
				 ce->parallel.number_children);

			if (ce->parallel.guc.wq_status) {
				i_printf(p, indent + 2, "WQI: { Head: %x, Tail: %x, Status: %x }\n",
					 READ_ONCE(*ce->parallel.guc.wq_head),
					 READ_ONCE(*ce->parallel.guc.wq_tail),
					 READ_ONCE(*ce->parallel.guc.wq_status));
			}

			if (ce->engine->emit_bb_start ==
			    emit_bb_start_parent_no_preempt_mid_batch) {
				i_printf(p, indent + 2, "Children Go: %u\n",
					 get_children_go_value(ce));
				for (i = 0; i < ce->parallel.number_children; ++i)
					i_printf(p, indent + 2, "Children Join: %u\n",
						 get_children_join_value(ce, i));
			}

			i = 0;
			for_each_child(ce, child) {
				i_printf(p, indent + 2, "- child %d:\n", i++);
				intel_context_show(child, p, indent + 4);
			}
		}
	}
	rcu_read_unlock();
}

static inline u32 get_children_go_addr(struct intel_context *ce)
{
	GEM_BUG_ON(!intel_context_is_parent(ce));

	return i915_ggtt_offset(ce->state) +
		__get_parent_scratch_offset(ce) +
		offsetof(struct parent_scratch, go.semaphore);
}

static inline u32 get_children_join_addr(struct intel_context *ce,
					 u8 child_index)
{
	GEM_BUG_ON(!intel_context_is_parent(ce));

	return i915_ggtt_offset(ce->state) +
		__get_parent_scratch_offset(ce) +
		offsetof(struct parent_scratch, join[child_index].semaphore);
}

#define PARENT_GO_BB			1
#define PARENT_GO_FINI_BREADCRUMB	0
#define CHILD_GO_BB			1
#define CHILD_GO_FINI_BREADCRUMB	0
static int emit_bb_start_parent_no_preempt_mid_batch(struct i915_request *rq,
						     u64 offset, u32 len,
						     const unsigned int flags)
{
	struct intel_context *ce = rq->context;
	int srcu;
	u32 *cs;
	u8 i;

	GEM_BUG_ON(!intel_context_is_parent(ce));

	cs = intel_ring_begin_ggtt(rq, &srcu, 10 + 4 * ce->parallel.number_children);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	/* Turn off preemption */
	*cs++ = MI_ARB_ON_OFF | MI_ARB_DISABLE;
	*cs++ = MI_NOOP;

	/* Wait on children */
	for (i = 0; i < ce->parallel.number_children; ++i) {
		*cs++ = (MI_SEMAPHORE_WAIT |
			 MI_SEMAPHORE_GLOBAL_GTT |
			 MI_SEMAPHORE_POLL |
			 MI_SEMAPHORE_SAD_EQ_SDD);
		*cs++ = PARENT_GO_BB;
		*cs++ = get_children_join_addr(ce, i);
		*cs++ = 0;
	}

	/* Tell children go */
	cs = gen8_emit_ggtt_write(cs,
				  CHILD_GO_BB,
				  get_children_go_addr(ce),
				  0);

	/* Jump to batch */
	*cs++ = MI_BATCH_BUFFER_START_GEN8 |
		(flags & I915_DISPATCH_SECURE ? 0 : BIT(8));
	*cs++ = lower_32_bits(offset);
	*cs++ = upper_32_bits(offset);
	*cs++ = MI_NOOP;

	intel_ring_advance_ggtt(rq, srcu, cs);

	return 0;
}

static int emit_bb_start_child_no_preempt_mid_batch(struct i915_request *rq,
						    u64 offset, u32 len,
						    const unsigned int flags)
{
	struct intel_context *ce = rq->context;
	struct intel_context *parent = intel_context_to_parent(ce);
	int srcu;
	u32 *cs;

	GEM_BUG_ON(!intel_context_is_child(ce));

	cs = intel_ring_begin_ggtt(rq, &srcu, 12);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	/* Signal parent */
	cs = gen8_emit_ggtt_write(cs,
				  PARENT_GO_BB,
				  get_children_join_addr(parent,
							 ce->parallel.child_index),
				  0);

	/* Wait on parent for go */
	*cs++ = (MI_SEMAPHORE_WAIT |
		 MI_SEMAPHORE_GLOBAL_GTT |
		 MI_SEMAPHORE_POLL |
		 MI_SEMAPHORE_SAD_EQ_SDD);
	*cs++ = CHILD_GO_BB;
	*cs++ = get_children_go_addr(parent);
	*cs++ = 0;

	/* Turn off preemption */
	*cs++ = MI_ARB_ON_OFF | MI_ARB_DISABLE;

	/* Jump to batch */
	*cs++ = MI_BATCH_BUFFER_START_GEN8 |
		(flags & I915_DISPATCH_SECURE ? 0 : BIT(8));
	*cs++ = lower_32_bits(offset);
	*cs++ = upper_32_bits(offset);

	intel_ring_advance_ggtt(rq, srcu, cs);

	return 0;
}

static u32 *
__emit_fini_breadcrumb_parent_no_preempt_mid_batch(struct i915_request *rq,
						   u32 *cs)
{
	struct intel_context *ce = rq->context;
	u8 i;

	GEM_BUG_ON(!intel_context_is_parent(ce));

	/* Wait on children */
	for (i = 0; i < ce->parallel.number_children; ++i) {
		*cs++ = (MI_SEMAPHORE_WAIT |
			 MI_SEMAPHORE_GLOBAL_GTT |
			 MI_SEMAPHORE_POLL |
			 MI_SEMAPHORE_SAD_EQ_SDD);
		*cs++ = PARENT_GO_FINI_BREADCRUMB;
		*cs++ = get_children_join_addr(ce, i);
		*cs++ = 0;
	}

	/* Turn on preemption */
	*cs++ = MI_ARB_ON_OFF | MI_ARB_ENABLE;
	*cs++ = MI_NOOP;

	/* Tell children go */
	cs = gen8_emit_ggtt_write(cs,
				  CHILD_GO_FINI_BREADCRUMB,
				  get_children_go_addr(ce),
				  0);

	return cs;
}

/*
 * If this true, a submission of multi-lrc requests had an error and the
 * requests need to be skipped. The front end (execuf IOCTL) should've called
 * i915_request_skip which squashes the BB but we still need to emit the fini
 * breadrcrumbs seqno write. At this point we don't know how many of the
 * requests in the multi-lrc submission were generated so we can't do the
 * handshake between the parent and children (e.g. if 4 requests should be
 * generated but 2nd hit an error only 1 would be seen by the GuC backend).
 * Simply skip the handshake, but still emit the breadcrumbd seqno, if an error
 * has occurred on any of the requests in submission / relationship.
 */
static inline bool skip_handshake(struct i915_request *rq)
{
	return test_bit(I915_FENCE_FLAG_SKIP_PARALLEL, &rq->fence.flags);
}

#define NON_SKIP_LEN	6
static u32 *
emit_fini_breadcrumb_parent_no_preempt_mid_batch(struct i915_request *rq,
						 u32 *cs)
{
	struct intel_context *ce = rq->context;
	__maybe_unused u32 *before_fini_breadcrumb_user_interrupt_cs;
	__maybe_unused u32 *start_fini_breadcrumb_cs = cs;
	int srcu;

	GEM_BUG_ON(!intel_context_is_parent(ce));

	intel_ring_fini_begin_ggtt(rq, &srcu);

	if (unlikely(skip_handshake(rq))) {
		/*
		 * NOP everything in __emit_fini_breadcrumb_parent_no_preempt_mid_batch,
		 * the NON_SKIP_LEN comes from the length of the emits below.
		 */
		memset(cs, 0, sizeof(u32) *
		       (ce->engine->emit_fini_breadcrumb_dw - NON_SKIP_LEN));
		cs += ce->engine->emit_fini_breadcrumb_dw - NON_SKIP_LEN;
	} else {
		cs = __emit_fini_breadcrumb_parent_no_preempt_mid_batch(rq, cs);
	}

	/* Emit fini breadcrumb */
	before_fini_breadcrumb_user_interrupt_cs = cs;
	cs = gen8_emit_ggtt_write(cs,
				  rq->fence.seqno,
				  i915_request_active_timeline(rq)->hwsp_offset,
				  0);

	/* User interrupt */
	*cs++ = MI_USER_INTERRUPT;
	*cs++ = MI_NOOP;

	/* Ensure our math for skip + emit is correct */
	GEM_BUG_ON(before_fini_breadcrumb_user_interrupt_cs + NON_SKIP_LEN !=
		   cs);
	GEM_BUG_ON(start_fini_breadcrumb_cs +
		   ce->engine->emit_fini_breadcrumb_dw != cs);

	intel_ring_fini_advance_ggtt(rq, srcu, cs);

	return cs;
}

static u32 *
__emit_fini_breadcrumb_child_no_preempt_mid_batch(struct i915_request *rq,
						  u32 *cs)
{
	struct intel_context *ce = rq->context;
	struct intel_context *parent = intel_context_to_parent(ce);

	GEM_BUG_ON(!intel_context_is_child(ce));

	/* Turn on preemption */
	*cs++ = MI_ARB_ON_OFF | MI_ARB_ENABLE;
	*cs++ = MI_NOOP;

	/* Signal parent */
	cs = gen8_emit_ggtt_write(cs,
				  PARENT_GO_FINI_BREADCRUMB,
				  get_children_join_addr(parent,
							 ce->parallel.child_index),
				  0);

	/* Wait parent on for go */
	*cs++ = (MI_SEMAPHORE_WAIT |
		 MI_SEMAPHORE_GLOBAL_GTT |
		 MI_SEMAPHORE_POLL |
		 MI_SEMAPHORE_SAD_EQ_SDD);
	*cs++ = CHILD_GO_FINI_BREADCRUMB;
	*cs++ = get_children_go_addr(parent);
	*cs++ = 0;

	return cs;
}

static u32 *
emit_fini_breadcrumb_child_no_preempt_mid_batch(struct i915_request *rq,
						u32 *cs)
{
	struct intel_context *ce = rq->context;
	__maybe_unused u32 *before_fini_breadcrumb_user_interrupt_cs;
	__maybe_unused u32 *start_fini_breadcrumb_cs = cs;
	int srcu;

	GEM_BUG_ON(!intel_context_is_child(ce));

	intel_ring_fini_begin_ggtt(rq, &srcu);

	if (unlikely(skip_handshake(rq))) {
		/*
		 * NOP everything in __emit_fini_breadcrumb_child_no_preempt_mid_batch,
		 * the NON_SKIP_LEN comes from the length of the emits below.
		 */
		memset(cs, 0, sizeof(u32) *
		       (ce->engine->emit_fini_breadcrumb_dw - NON_SKIP_LEN));
		cs += ce->engine->emit_fini_breadcrumb_dw - NON_SKIP_LEN;
	} else {
		cs = __emit_fini_breadcrumb_child_no_preempt_mid_batch(rq, cs);
	}

	/* Emit fini breadcrumb */
	before_fini_breadcrumb_user_interrupt_cs = cs;
	cs = gen8_emit_ggtt_write(cs,
				  rq->fence.seqno,
				  i915_request_active_timeline(rq)->hwsp_offset,
				  0);

	/* User interrupt */
	*cs++ = MI_USER_INTERRUPT;
	*cs++ = MI_NOOP;

	/* Ensure our math for skip + emit is correct */
	GEM_BUG_ON(before_fini_breadcrumb_user_interrupt_cs + NON_SKIP_LEN !=
		   cs);
	GEM_BUG_ON(start_fini_breadcrumb_cs +
		   ce->engine->emit_fini_breadcrumb_dw != cs);

	intel_ring_fini_advance_ggtt(rq, srcu, cs);

	return cs;
}

#undef NON_SKIP_LEN

static struct intel_context *
guc_create_virtual(struct intel_engine_cs **siblings, unsigned int count,
		   unsigned long flags)
{
	struct guc_virtual_engine *ve;
	struct intel_guc *guc;
	unsigned int n;
	int err;

	ve = kzalloc(sizeof(*ve), GFP_KERNEL);
	if (!ve)
		return ERR_PTR(-ENOMEM);

	guc = &siblings[0]->gt->uc.guc;

	ve->base.i915 = siblings[0]->i915;
	ve->base.gt = siblings[0]->gt;
	ve->base.uncore = siblings[0]->uncore;
	ve->base.id = -1;

	ve->base.uabi_class = I915_ENGINE_CLASS_INVALID;
	ve->base.instance = I915_ENGINE_CLASS_INVALID_VIRTUAL;
	ve->base.uabi_instance = I915_ENGINE_CLASS_INVALID_VIRTUAL;
	ve->base.saturated = ALL_ENGINES;

	snprintf(ve->base.name, sizeof(ve->base.name), "virtual");

	ve->base.sched_engine = i915_sched_engine_get(guc->sched_engine);

	ve->base.cops = &virtual_guc_context_ops;
	ve->base.request_alloc = guc_request_alloc;
	ve->base.bump_serial = virtual_guc_bump_serial;

	ve->base.submit_request = guc_submit_request;

	ve->base.flags = I915_ENGINE_IS_VIRTUAL;
	ve->base.mask = VIRTUAL_ENGINES;

	intel_context_init(&ve->context, &ve->base);

	for (n = 0; n < count; n++) {
		struct intel_engine_cs *sibling = siblings[n];

		GEM_BUG_ON(!is_power_of_2(sibling->mask));
		if (sibling->mask & ve->base.mask) {
			guc_dbg(guc, "duplicate %s entry in load balancer\n",
				sibling->name);
			err = -EINVAL;
			goto err_put;
		}

		ve->base.mask |= sibling->mask;
		ve->base.logical_mask |= sibling->logical_mask;

		if (n != 0 && ve->base.class != sibling->class) {
			guc_dbg(guc, "invalid mixing of engine class, sibling %d, already %d\n",
				sibling->class, ve->base.class);
			err = -EINVAL;
			goto err_put;
		} else if (n == 0) {
			ve->base.class = sibling->class;
			ve->base.uabi_class = sibling->uabi_class;
			snprintf(ve->base.name, sizeof(ve->base.name),
				 "v%dx%d", ve->base.class, count);
			ve->base.context_size = sibling->context_size;

			ve->base.remove_active_request =
				sibling->remove_active_request;
			ve->base.emit_bb_start = sibling->emit_bb_start;
			ve->base.emit_flush = sibling->emit_flush;
			ve->base.emit_init_breadcrumb =
				sibling->emit_init_breadcrumb;
			ve->base.emit_fini_breadcrumb =
				sibling->emit_fini_breadcrumb;
			ve->base.emit_fini_breadcrumb_dw =
				sibling->emit_fini_breadcrumb_dw;
			ve->base.breadcrumbs =
				intel_breadcrumbs_get(sibling->breadcrumbs);

			ve->base.flags |= sibling->flags;

			ve->base.props.timeslice_duration_ms =
				sibling->props.timeslice_duration_ms;
			ve->base.props.preempt_timeout_ms =
				sibling->props.preempt_timeout_ms;
		}
	}

	return &ve->context;

err_put:
	intel_context_put(&ve->context);
	return ERR_PTR(err);
}

bool intel_guc_virtual_engine_has_heartbeat(const struct intel_engine_cs *ve)
{
	struct intel_engine_cs *engine;
	intel_engine_mask_t tmp, mask = ve->mask;

	for_each_engine_masked(engine, ve->gt, mask, tmp)
		if (READ_ONCE(engine->props.heartbeat_interval_ms))
			return true;

	return false;
}

void intel_guc_context_set_preemption_timeout(struct intel_context *ce)
{
	u32 preempt_timeout_ms = ce->schedule_policy.preempt_timeout_ms;
	struct intel_guc *guc = ce_to_guc(ce);
	intel_wakeref_t wakeref;

	if (!__context_is_available(guc, ce))
		return;

	with_intel_gt_pm(guc_to_gt(guc), wakeref)
		__guc_context_set_preemption_timeout(guc, ce->guc_id.id,
						     preempt_timeout_ms * 1000);
}

#if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST)
#include "selftest_guc.c"
#include "selftest_guc_multi_lrc.c"
#include "selftest_guc_hangcheck.c"
#include "selftest_doorbells.c"
#endif
