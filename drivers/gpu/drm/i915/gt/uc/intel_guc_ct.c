// SPDX-License-Identifier: MIT
/*
 * Copyright © 2016-2019 Intel Corporation
 */

#include <linux/circ_buf.h>
#include <linux/ktime.h>
#include <linux/time64.h>
#include <linux/string_helpers.h>
#include <linux/timekeeping.h>

#include "i915_drv.h"
#include "i915_irq.h"
#include "intel_guc_ct.h"
#include "intel_guc_print.h"
#include "gt/intel_gt.h"
#include "gt/intel_pagefault.h"
#include "gt/intel_tlb.h"
#include "gt/iov/intel_iov_event.h"
#include "gt/iov/intel_iov_relay.h"
#include "gt/iov/intel_iov_service.h"
#include "gt/iov/intel_iov_state.h"

#if IS_ENABLED(CPTCFG_DRM_I915_DEBUG_GEM)
enum {
	CT_DEAD_ALIVE = 0,
	CT_DEAD_SETUP,
	CT_DEAD_WRITE,
	CT_DEAD_DEADLOCK,
	CT_DEAD_H2G_HAS_ROOM,
	CT_DEAD_READ,
	CT_DEAD_PROCESS_FAILED,
};

static void ct_dead_ct_worker_func(struct work_struct *w);

#define CT_DEAD(ct, reason)	\
	do { \
		if (!(ct)->dead_ct_reported && !i915_error_injected()) { \
			(ct)->dead_ct_reason |= 1 << CT_DEAD_##reason; \
			queue_work(system_unbound_wq, &(ct)->dead_ct_worker); \
		} \
	} while (0)
#else
#define CT_DEAD(ct, reason)	do { } while (0)
#endif

static inline struct intel_guc *ct_to_guc(struct intel_guc_ct *ct)
{
	return container_of(ct, struct intel_guc, ct);
}

#define CT_ERROR(_ct, _fmt, ...) \
	intel_gt_log_driver_error(guc_to_gt(ct_to_guc(_ct)), INTEL_GT_DRIVER_ERROR_GUC_COMMUNICATION, \
				  "CT: " _fmt, ##__VA_ARGS__)
#ifdef CPTCFG_DRM_I915_DEBUG_GUC
#define CT_DEBUG(_ct, _fmt, ...) \
	guc_dbg(ct_to_guc(_ct), "CT: " _fmt, ##__VA_ARGS__)
#else
#define CT_DEBUG(...)	do { } while (0)
#endif
#define CT_PROBE_ERROR(_ct, _fmt, ...) \
	guc_probe_error(ct_to_guc(ct), "CT: " _fmt, ##__VA_ARGS__)

/**
 * DOC: CTB Blob
 *
 * We allocate single blob to hold both CTB descriptors and buffers:
 *
 *      +--------+-----------------------------------------------+------+
 *      | offset | contents                                      | size |
 *      +========+===============================================+======+
 *      | 0x0000 | H2G `CTB Descriptor`_ (send)                  |      |
 *      +--------+-----------------------------------------------+  4K  |
 *      | 0x0800 | G2H `CTB Descriptor`_ (recv)                  |      |
 *      +--------+-----------------------------------------------+------+
 *      | 0x1000 | H2G `CT Buffer`_ (send)                       | n*4K |
 *      |        |                                               |      |
 *      +--------+-----------------------------------------------+------+
 *      | 0x1000 | G2H `CT Buffer`_ (recv)                       | m*4K |
 *      | + n*4K |                                               |      |
 *      +--------+-----------------------------------------------+------+
 *
 * Size of each `CT Buffer`_ must be multiple of 4K.
 * We don't expect too many messages in flight at any time, unless we are
 * using the GuC submission. In that case each request requires a minimum
 * 2 dwords which gives us a maximum 256 queue'd requests. Hopefully this
 * enough space to avoid backpressure on the driver. We increase the size
 * of the receive buffer (relative to the send) to ensure a G2H response
 * CTB has a landing spot.
 */
#define CTB_DESC_SIZE		ALIGN(sizeof(struct guc_ct_buffer_desc), SZ_2K)
#define CTB_H2G_BUFFER_SIZE	(SZ_4K)
#define PVC_CTB_H2G_BUFFER_SIZE	(SZ_32K) /* concurrent pagefault replies */

struct ct_request {
	struct list_head link;
	struct task_struct *tsk;
	u32 fence;
	u32 status;
	u32 response_len;
	u32 *response_buf;
};

struct ct_incoming_msg {
	struct llist_node link;
	u32 msg[];
};

static inline int __ct_msg_size(u32 hdr)
{
        return FIELD_GET(GUC_CTB_MSG_0_NUM_DWORDS, hdr) + GUC_CTB_MSG_MIN_LEN;
}

static inline int ct_msg_size(const struct ct_incoming_msg *msg)
{
	return __ct_msg_size(msg->msg[0]);
}

enum { CTB_SEND = 0, CTB_RECV = 1 };

enum { CTB_OWNER_HOST = 0 };

static noinline  void ct_incoming_request_worker_func(struct work_struct *w);

/**
 * intel_guc_ct_init_early - Initialize CT state without requiring device access
 * @ct: pointer to CT struct
 */
void intel_guc_ct_init_early(struct intel_guc_ct *ct)
{
	spin_lock_init(&ct->ctbs.send.lock);
	spin_lock_init(&ct->ctbs.recv.lock);
	spin_lock_init(&ct->requests.lock);
	INIT_LIST_HEAD(&ct->requests.pending);
	init_llist_head(&ct->requests.incoming);
#if IS_ENABLED(CPTCFG_DRM_I915_DEBUG_GEM)
	INIT_WORK(&ct->dead_ct_worker, ct_dead_ct_worker_func);
#endif
	INIT_WORK(&ct->requests.worker, ct_incoming_request_worker_func);
	init_waitqueue_head(&ct->wq);
}

static void guc_ct_buffer_desc_init(struct guc_ct_buffer_desc *desc)
{
	memset(desc, 0, sizeof(*desc));
}

static void guc_ct_buffer_reset(struct intel_guc_ct_buffer *ctb)
{
	u32 space;

	ctb->broken = false;
	ctb->tail = 0;
	ctb->head = 0;
	space = CIRC_SPACE(ctb->tail, ctb->head, ctb->size) - ctb->resv_space;
	atomic_set(&ctb->space, space);

	guc_ct_buffer_desc_init(ctb->desc);
}

static void guc_ct_buffer_init(struct intel_guc_ct_buffer *ctb,
			       struct guc_ct_buffer_desc *desc,
			       u32 *cmds, u32 size_in_bytes, u32 resv_space)
{
	GEM_BUG_ON(size_in_bytes % 4);

	ctb->desc = desc;
	ctb->cmds = cmds;
	ctb->size = size_in_bytes / 4;
	ctb->resv_space = resv_space / 4;

	guc_ct_buffer_reset(ctb);
}

static int guc_action_control_ctb(struct intel_guc *guc, u32 control)
{
	u32 request[HOST2GUC_CONTROL_CTB_REQUEST_MSG_LEN] = {
		FIELD_PREP(GUC_HXG_MSG_0_ORIGIN, GUC_HXG_ORIGIN_HOST) |
		FIELD_PREP(GUC_HXG_MSG_0_TYPE, GUC_HXG_TYPE_REQUEST) |
		FIELD_PREP(GUC_HXG_REQUEST_MSG_0_ACTION, GUC_ACTION_HOST2GUC_CONTROL_CTB),
		FIELD_PREP(HOST2GUC_CONTROL_CTB_REQUEST_MSG_1_CONTROL, control),
	};
	int ret;

	GEM_BUG_ON(control != GUC_CTB_CONTROL_DISABLE && control != GUC_CTB_CONTROL_ENABLE);

	/* CT control must go over MMIO */
	ret = intel_guc_send_mmio(guc, request, ARRAY_SIZE(request), NULL, 0);

	return ret > 0 ? -EPROTO : ret;
}

static int ct_control_enable(struct intel_guc_ct *ct, bool enable)
{
	int err;

	err = guc_action_control_ctb(ct_to_guc(ct), enable ?
				     GUC_CTB_CONTROL_ENABLE : GUC_CTB_CONTROL_DISABLE);
	if (unlikely(err))
		CT_PROBE_ERROR(ct, "Failed to control/%s CTB (%pe)\n",
			       str_enable_disable(enable), ERR_PTR(err));

	return err;
}

static int ct_register_buffer(struct intel_guc_ct *ct, bool send,
			      u32 desc_addr, u32 buff_addr, u32 size)
{
	int err;

	err = intel_guc_self_cfg64(ct_to_guc(ct), send ?
				   GUC_KLV_SELF_CFG_H2G_CTB_DESCRIPTOR_ADDR_KEY :
				   GUC_KLV_SELF_CFG_G2H_CTB_DESCRIPTOR_ADDR_KEY,
				   desc_addr);
	if (unlikely(err))
		goto failed;

	err = intel_guc_self_cfg64(ct_to_guc(ct), send ?
				   GUC_KLV_SELF_CFG_H2G_CTB_ADDR_KEY :
				   GUC_KLV_SELF_CFG_G2H_CTB_ADDR_KEY,
				   buff_addr);
	if (unlikely(err))
		goto failed;

	err = intel_guc_self_cfg32(ct_to_guc(ct), send ?
				   GUC_KLV_SELF_CFG_H2G_CTB_SIZE_KEY :
				   GUC_KLV_SELF_CFG_G2H_CTB_SIZE_KEY,
				   size);
	if (unlikely(err))
failed:
		CT_PROBE_ERROR(ct, "Failed to register %s buffer (%pe)\n",
			       send ? "SEND" : "RECV", ERR_PTR(err));

	return err;
}

/**
 * intel_guc_ct_init - Init buffer-based communication
 * @ct: pointer to CT struct
 *
 * Allocate memory required for buffer-based communication.
 *
 * Return: 0 on success, a negative errno code on failure.
 */
int intel_guc_ct_init(struct intel_guc_ct *ct)
{
	struct intel_guc *guc = ct_to_guc(ct);
	struct guc_ct_buffer_desc *desc;
	u32 h2g_bufsz, g2h_bufsz;
	u32 blob_size;
	u32 cmds_size;
	u32 resv_space;
	void *blob;
	u32 *cmds;
	int err;

	err = i915_inject_probe_error(guc_to_gt(guc)->i915, -ENXIO);
	if (err)
		return err;

	GEM_BUG_ON(ct->vma);

	h2g_bufsz = CTB_H2G_BUFFER_SIZE;
	if (HAS_RECOVERABLE_PAGE_FAULT(guc_to_gt(guc)->i915))
		h2g_bufsz = max_t(u32, h2g_bufsz, PVC_CTB_H2G_BUFFER_SIZE);
	g2h_bufsz = 4 * h2g_bufsz; /* expect each h2g to generate a reply */

	blob_size = 2 * CTB_DESC_SIZE + h2g_bufsz + g2h_bufsz;
	err = __intel_guc_allocate_and_map_vma(guc, blob_size, true, &ct->vma, &blob);
	if (unlikely(err)) {
		CT_PROBE_ERROR(ct, "Failed to allocate %u for CTB data (%pe)\n",
			       blob_size, ERR_PTR(err));
		return err;
	}

	CT_DEBUG(ct, "base=%#x size=%u\n", intel_guc_ggtt_offset(guc, ct->vma), blob_size);

	/* store pointers to desc and cmds for send ctb */
	desc = blob;
	cmds = blob + 2 * CTB_DESC_SIZE;
	cmds_size = h2g_bufsz;
	resv_space = 0;
	CT_DEBUG(ct, "%s desc %#tx cmds %#tx size %u/%u\n", "send",
		 ptrdiff(desc, blob), ptrdiff(cmds, blob), cmds_size,
		 resv_space);

	guc_ct_buffer_init(&ct->ctbs.send, desc, cmds, cmds_size, resv_space);

	/* store pointers to desc and cmds for recv ctb */
	desc = blob + CTB_DESC_SIZE;
	cmds = blob + 2 * CTB_DESC_SIZE + h2g_bufsz;
	cmds_size = g2h_bufsz;
	resv_space = cmds_size / 4;
	CT_DEBUG(ct, "%s desc %#tx cmds %#tx size %u/%u\n", "recv",
		 ptrdiff(desc, blob), ptrdiff(cmds, blob), cmds_size,
		 resv_space);

	guc_ct_buffer_init(&ct->ctbs.recv, desc, cmds, cmds_size, resv_space);

	return 0;
}

/**
 * intel_guc_ct_fini - Fini buffer-based communication
 * @ct: pointer to CT struct
 *
 * Deallocate memory required for buffer-based communication.
 */
void intel_guc_ct_fini(struct intel_guc_ct *ct)
{
	GEM_BUG_ON(ct->enabled);

	i915_vma_unpin_and_release(&ct->vma, I915_VMA_RELEASE_MAP);
	memset(ct, 0, sizeof(*ct));
}

/**
 * intel_guc_ct_enable - Enable buffer based command transport.
 * @ct: pointer to CT struct
 *
 * Return: 0 on success, a negative errno code on failure.
 */
int intel_guc_ct_enable(struct intel_guc_ct *ct)
{
	struct intel_guc *guc = ct_to_guc(ct);
	u32 base, desc, cmds, size;
	void *blob;
	int err;

	GEM_BUG_ON(ct->enabled);

	/* vma should be already allocated and map'ed */
	GEM_BUG_ON(!ct->vma);
	GEM_BUG_ON(!i915_gem_object_has_pinned_pages(ct->vma->obj));
	base = intel_guc_ggtt_offset(guc, ct->vma);

	/* blob should start with send descriptor */
	blob = __px_vaddr(ct->vma->obj);
	GEM_BUG_ON(blob != ct->ctbs.send.desc);

	/* (re)initialize descriptors */
	guc_ct_buffer_reset(&ct->ctbs.send);
	guc_ct_buffer_reset(&ct->ctbs.recv);

	/*
	 * Register both CT buffers starting with RECV buffer.
	 * Descriptors are in first half of the blob.
	 */
	desc = base + ptrdiff(ct->ctbs.recv.desc, blob);
	cmds = base + ptrdiff(ct->ctbs.recv.cmds, blob);
	size = ct->ctbs.recv.size * 4;
	err = ct_register_buffer(ct, false, desc, cmds, size);
	if (unlikely(err))
		goto err_out;

	desc = base + ptrdiff(ct->ctbs.send.desc, blob);
	cmds = base + ptrdiff(ct->ctbs.send.cmds, blob);
	size = ct->ctbs.send.size * 4;
	err = ct_register_buffer(ct, true, desc, cmds, size);
	if (unlikely(err))
		goto err_out;

	err = ct_control_enable(ct, true);
	if (unlikely(err))
		goto err_out;

	ct->enabled = true;
#if IS_ENABLED(CPTCFG_DRM_I915_DEBUG_GEM)
	ct->dead_ct_reported = false;
	ct->dead_ct_reason = CT_DEAD_ALIVE;
#endif

	return 0;

err_out:
	CT_PROBE_ERROR(ct, "Failed to enable CTB (%pe)\n", ERR_PTR(err));
	CT_DEAD(ct, SETUP);
	return err;
}

/**
 * intel_guc_ct_disable - Disable buffer based command transport.
 * @ct: pointer to CT struct
 */
void intel_guc_ct_disable(struct intel_guc_ct *ct)
{
	struct intel_guc *guc = ct_to_guc(ct);

	GEM_BUG_ON(!ct->enabled);

	ct->enabled = false;

	if (intel_guc_is_fw_running(guc))
		ct_control_enable(ct, false);

	if (!list_empty(&ct->requests.pending)) {
		struct ct_request *rq;
		unsigned long flags;

		spin_lock_irqsave(&ct->requests.lock, flags);
		list_for_each_entry(rq, &ct->requests.pending, link)
			wake_up_process(rq->tsk);
		spin_unlock_irqrestore(&ct->requests.lock, flags);
	}

	if (waitqueue_active(&ct->wq))
		wake_up_all(&ct->wq);
}

#if IS_ENABLED(CPTCFG_DRM_I915_DEBUG_GEM)
static void ct_track_lost_and_found(struct intel_guc_ct *ct, u32 fence, u32 action)
{
	unsigned int lost = fence % ARRAY_SIZE(ct->requests.lost_and_found);
#if IS_ENABLED(CPTCFG_DRM_I915_DEBUG_GUC)
	unsigned long entries[SZ_32];
	unsigned int n;

	n = stack_trace_save(entries, ARRAY_SIZE(entries), 1);

	/* May be called under spinlock, so avoid sleeping */
	ct->requests.lost_and_found[lost].stack = stack_depot_save(entries, n, GFP_NOWAIT);
#endif
	ct->requests.lost_and_found[lost].fence = fence;
	ct->requests.lost_and_found[lost].action = action;
}
#endif

static u32 ct_get_next_fence(struct intel_guc_ct *ct)
{
	/* For now it's trivial */
	return ++ct->requests.last_fence;
}

static int ct_write(struct intel_guc_ct *ct,
		    const u32 *action,
		    u32 len /* in dwords */,
		    u32 fence, u32 flags)
{
	struct intel_guc_ct_buffer *ctb = &ct->ctbs.send;
	struct guc_ct_buffer_desc *desc = ctb->desc;
	u32 tail = ctb->tail;
	u32 size = ctb->size;
	u32 header;
	u32 hxg;
	u32 type;
	u32 *cmds = ctb->cmds;
	unsigned int i;

	if (unlikely(desc->status))
		goto corrupted;

	GEM_BUG_ON(tail > size);

#ifdef CPTCFG_DRM_I915_DEBUG_GUC
	if (unlikely(tail != READ_ONCE(desc->tail))) {
		CT_ERROR(ct, "Tail was modified %u != %u\n",
			 desc->tail, tail);
		desc->status |= GUC_CTB_STATUS_MISMATCH;
		goto corrupted;
	}
	if (unlikely(READ_ONCE(desc->head) >= size)) {
		CT_ERROR(ct, "Invalid head offset %u >= %u)\n",
			 desc->head, size);
		desc->status |= GUC_CTB_STATUS_OVERFLOW;
		goto corrupted;
	}
#endif

	/*
	 * dw0: CT header (including fence)
	 * dw1: HXG header (including action code)
	 * dw2+: action data
	 */
	header = FIELD_PREP(GUC_CTB_MSG_0_FORMAT, GUC_CTB_FORMAT_HXG) |
		 FIELD_PREP(GUC_CTB_MSG_0_NUM_DWORDS, len) |
		 FIELD_PREP(GUC_CTB_MSG_0_FENCE, fence);

/* Disable fast request temporarily as it is exposing a bug */
#undef GUC_HXG_TYPE_FAST_REQUEST
#define GUC_HXG_TYPE_FAST_REQUEST	GUC_HXG_TYPE_EVENT

	type = (flags & INTEL_GUC_CT_SEND_NB) ? GUC_HXG_TYPE_FAST_REQUEST :
		GUC_HXG_TYPE_REQUEST;
	hxg = FIELD_PREP(GUC_HXG_MSG_0_TYPE, type) |
		FIELD_PREP(GUC_HXG_REQUEST_MSG_0_ACTION |
			   GUC_HXG_REQUEST_MSG_0_DATA0, action[0]);

	CT_DEBUG(ct, "writing (tail %u) %*ph %*ph %*ph\n",
		 tail, 4, &header, 4, &hxg, 4 * (len - 1), &action[1]);

	cmds[tail] = header;
	tail = (tail + 1) % size;

	cmds[tail] = hxg;
	tail = (tail + 1) % size;

	for (i = 1; i < len; i++) {
		cmds[tail] = action[i];
		tail = (tail + 1) % size;
	}
	GEM_BUG_ON(tail > size);

#if IS_ENABLED(CPTCFG_DRM_I915_DEBUG_GEM)
	ct_track_lost_and_found(ct, fence,
				FIELD_GET(GUC_HXG_EVENT_MSG_0_ACTION, action[0]));
#endif

	/*
	 * make sure H2G buffer update and LRC tail update (if this triggering a
	 * submission) are visible before updating the descriptor tail
	 */
	wmb();

	/* now update descriptor */
	WRITE_ONCE(desc->tail, tail);

	/* update local copies */
	ctb->tail = tail;
	atomic_set(&ctb->space, CIRC_SPACE(tail, READ_ONCE(desc->head), size));

	/* Wa_22016122933: Theoretically write combining buffer flush is
	 * needed here to make the tail update visible to GuC right away,
	 * but ct_write is always followed by a MMIO write which triggers
	 * the interrupt to GuC, so an explicit flush is not required.
	 * Just leave a comment here for now.
	 */
	/* i915_write_barrier(guc_to_gt(ct_to_guc(ct))->i915); */

	return 0;

corrupted:
	CT_ERROR(ct, "Corrupted descriptor head=%u tail=%u status=%#x\n",
		 desc->head, desc->tail, desc->status);
	CT_DEAD(ct, WRITE);
	ctb->broken = true;
	return -EPIPE;
}

/*
 * wait_for_ct_request_update - Wait for CT request state update.
 * @ct:		pointer to CT
 * @req:	pointer to pending request
 * @status:	placeholder for status
 *
 * For each sent request, GuC shall send back CT response message.
 * Our message handler will update status of tracked request once
 * response message with given fence is received. Wait here and
 * check for valid response status value.
 *
 * Return:
 * *	0 response received (status is valid)
 * *	-ETIMEDOUT no response within hardcoded timeout
 */
static int wait_for_ct_request_update(struct intel_guc_ct *ct, struct ct_request *req)
{
	long timeout = 10 * HZ;
	int err = 0;

	intel_boost_fake_int_timer(guc_to_gt(ct_to_guc(ct)), true);

	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);

		intel_guc_ct_receive(ct);

		if (FIELD_GET(GUC_HXG_MSG_0_ORIGIN, READ_ONCE(req->status)) == GUC_HXG_ORIGIN_GUC)
			break;

		if (!intel_guc_ct_enabled(ct)) {
			err = -ENODEV;
			break;
		}

		if (signal_pending(current)) {
			err = -ERESTARTSYS;
			break;
		}

		if (!timeout) {
			CT_ERROR(ct, "CT: fence %u timed out\n", req->fence);
			err = -ETIME;
			break;
		}

		timeout = io_schedule_timeout(timeout);
	}
	__set_current_state(TASK_RUNNING);

	intel_boost_fake_int_timer(guc_to_gt(ct_to_guc(ct)), false);
	return err;
}

static inline bool g2h_has_room(struct intel_guc_ct *ct, u32 g2h_len_dw)
{
	struct intel_guc_ct_buffer *ctb = &ct->ctbs.recv;

	/*
	 * We leave a certain amount of space in the G2H CTB buffer for
	 * unexpected G2H CTBs (e.g. logging, engine hang, etc...)
	 */
	return !g2h_len_dw || atomic_read(&ctb->space) >= g2h_len_dw;
}

static inline void g2h_reserve_space(struct intel_guc_ct *ct, u32 g2h_len_dw)
{
	lockdep_assert_held(&ct->ctbs.send.lock);

	GEM_BUG_ON(!g2h_has_room(ct, g2h_len_dw));

	if (g2h_len_dw)
		atomic_sub(g2h_len_dw, &ct->ctbs.recv.space);
}

static inline void g2h_release_space(struct intel_guc_ct *ct, u32 g2h_len_dw)
{
	atomic_add(g2h_len_dw, &ct->ctbs.recv.space);
}

static inline bool h2g_has_room(struct intel_guc_ct *ct, u32 len_dw)
{
	struct intel_guc_ct_buffer *ctb = &ct->ctbs.send;
	struct guc_ct_buffer_desc *desc = ctb->desc;
	u32 space;

	if (atomic_read(&ctb->space) >= len_dw)
		return true;

	space = CIRC_SPACE(ctb->tail, READ_ONCE(desc->head), ctb->size);
	return space >= len_dw;
}

static bool has_room_nb(struct intel_guc_ct *ct, u32 h2g_dw, u32 g2h_dw)
{
	bool h2g = h2g_has_room(ct, h2g_dw);
	bool g2h = g2h_has_room(ct, g2h_dw);

	return h2g && g2h;
}

#define G2H_LEN_DW(f) ({ \
	typeof(f) f_ = (f); \
	FIELD_GET(INTEL_GUC_CT_SEND_G2H_DW_MASK, f_) ? \
	FIELD_GET(INTEL_GUC_CT_SEND_G2H_DW_MASK, f_) + \
	GUC_CTB_HXG_MSG_MIN_LEN : 0; \
})
static int ct_send_nb(struct intel_guc_ct *ct,
		      const u32 *action,
		      u32 len,
		      u32 flags)
{
	struct intel_guc_ct_buffer *ctb = &ct->ctbs.send;
	u32 g2h_len_dw = G2H_LEN_DW(flags);
	unsigned long spin_flags;
	u32 fence;
	int ret = -EBUSY;

	if (!has_room_nb(ct, len + GUC_CTB_HDR_LEN, g2h_len_dw))
		return ret;

	spin_lock_irqsave(&ctb->lock, spin_flags);

	if (!has_room_nb(ct, len + GUC_CTB_HDR_LEN, g2h_len_dw))
		goto out;

	fence = ct_get_next_fence(ct);
	ret = ct_write(ct, action, len, fence, flags);
	if (unlikely(ret))
		goto out;

	g2h_reserve_space(ct, g2h_len_dw);
out:
	spin_unlock_irqrestore(&ctb->lock, spin_flags);
	intel_guc_notify(ct_to_guc(ct));

	return ret;
}

static int ct_send(struct intel_guc_ct *ct,
		   const u32 *action,
		   u32 len,
		   u32 *response_buf,
		   u32 response_buf_size)
{
	struct intel_guc_ct_buffer *ctb = &ct->ctbs.send;
	struct ct_request request;
	bool send_again;
	int err;

	GEM_BUG_ON(!ct->enabled);
	GEM_BUG_ON(!len);
	GEM_BUG_ON(len > GUC_CTB_HXG_MSG_MAX_LEN - GUC_CTB_HDR_LEN);
	GEM_BUG_ON(!response_buf && response_buf_size);
	might_sleep();

	request.status = 0;
	request.response_len = response_buf_size;
	request.response_buf = response_buf;
	request.tsk = current;

resend:
	send_again = false;
	err = ___wait_event(ct->wq,
			    has_room_nb(ct, len + GUC_CTB_HDR_LEN, GUC_CTB_HXG_MSG_MAX_LEN),
			    TASK_INTERRUPTIBLE, true, 0,
			    intel_guc_ct_receive(ct);
			    schedule());
	if (unlikely(err))
		return err;

	spin_lock_irq(&ctb->lock);
	if (!has_room_nb(ct, len + GUC_CTB_HDR_LEN, GUC_CTB_HXG_MSG_MAX_LEN)) {
		spin_unlock_irq(&ctb->lock);
		goto resend;
	}

	request.fence = ct_get_next_fence(ct);

	spin_lock(&ct->requests.lock);
	list_add_tail(&request.link, &ct->requests.pending);
	spin_unlock(&ct->requests.lock);

	err = ct_write(ct, action, len, request.fence, 0);
	g2h_reserve_space(ct, GUC_CTB_HXG_MSG_MAX_LEN);

	spin_unlock_irq(&ctb->lock);

	if (unlikely(err))
		goto unlink;

	intel_guc_notify(ct_to_guc(ct));
	err = wait_for_ct_request_update(ct, &request);

	if (unlikely(err)) {
		if (err == -ENODEV)
			/* wait_for_ct_request_update returns -ENODEV on reset/suspend in progress.
			 * In this case, output is debug rather than error info
			 */
			CT_DEBUG(ct, "Request %#x (fence %u) cancelled as CTB is disabled\n",
				 action[0], request.fence);
		else
			CT_ERROR(ct, "No response for request %#x (fence %u)\n",
				 action[0], request.fence);
		goto unlink;
	}

	if (FIELD_GET(GUC_HXG_MSG_0_TYPE, request.status) == GUC_HXG_TYPE_NO_RESPONSE_RETRY) {
		CT_DEBUG(ct, "retrying request %#x (%u)\n", *action,
			 FIELD_GET(GUC_HXG_RETRY_MSG_0_REASON, request.status));
		send_again = true;
		goto unlink;
	}

	if (FIELD_GET(GUC_HXG_MSG_0_TYPE, request.status) != GUC_HXG_TYPE_RESPONSE_SUCCESS) {
		CT_DEBUG(ct, "Sending action %#x failed (%u)\n", *action,
			 FIELD_GET(GUC_HXG_RETRY_MSG_0_REASON, request.status));
		err = -EIO;
		goto unlink;
	}

	if (response_buf) {
		/* There shall be no data in the status */
		GEM_BUG_ON(FIELD_GET(GUC_HXG_RESPONSE_MSG_0_DATA0, request.status));
		/* Return actual response len */
		err = request.response_len;
	} else {
		/* There shall be no response payload */
		GEM_BUG_ON(request.response_len);
		/* Return data decoded from the status dword */
		err = FIELD_GET(GUC_HXG_RESPONSE_MSG_0_DATA0, request.status);
	}

unlink:
	/* kick the next waiter on clearing our response from the CT buffer */
	g2h_release_space(ct, GUC_CTB_HXG_MSG_MAX_LEN);
	if (waitqueue_active(&ct->wq))
		wake_up(&ct->wq);

	spin_lock_irq(&ct->requests.lock);
	list_del(&request.link);
	spin_unlock_irq(&ct->requests.lock);

	if (unlikely(send_again))
		goto resend;

	return err;
}

/*
 * Command Transport (CT) buffer based GuC send function.
 */
int intel_guc_ct_send(struct intel_guc_ct *ct, const u32 *action, u32 len,
		      u32 *response_buf, u32 response_buf_size, u32 flags)
{
	struct intel_guc *guc = ct_to_guc(ct);
	struct intel_gt *gt = guc_to_gt(guc);
	int ret;

	ret = i915_inject_probe_error(gt->i915, -ENXIO);
	if (ret)
		return ret;

	ret = i915_inject_probe_error(gt->i915, -EBUSY);
	if (ret)
		return ret;

	if (unlikely(!ct->enabled))
		return -ENODEV;

	if (flags & INTEL_GUC_CT_SEND_NB)
		return ct_send_nb(ct, action, len, flags);

	if (unlikely(ct->ctbs.send.broken))
		return -EPIPE;

	return ct_send(ct, action, len, response_buf, response_buf_size);
}
ALLOW_ERROR_INJECTION(intel_guc_ct_send, ERRNO);

static struct ct_incoming_msg *ct_alloc_msg(u32 num_dwords)
{
	struct ct_incoming_msg *msg;

	return kmalloc(struct_size(msg, msg, num_dwords), GFP_ATOMIC);
}

static void ct_free_msg(struct ct_incoming_msg *msg)
{
	kfree(msg);
}

static void ct_read(struct intel_guc_ct *ct, struct llist_head *msg)
{
	struct intel_guc_ct_buffer *ctb = &ct->ctbs.recv;
	struct guc_ct_buffer_desc *desc = ctb->desc;
	struct ct_incoming_msg *m;
	u32 head = ctb->head;
	u32 tail = READ_ONCE(desc->tail);
	u32 size = ctb->size;
	u32 *cmds = ctb->cmds;
	s32 available;

	if (tail == head)
		return;

	if (unlikely(desc->status)) {
		u32 status = desc->status;

		if (status & GUC_CTB_STATUS_UNUSED) {
			/*
			 * Potentially valid if a CLIENT_RESET request resulted in
			 * contexts/engines being reset. But should never happen as
			 * no contexts should be active when CLIENT_RESET is sent.
			 */
			CT_ERROR(ct, "Unexpected G2H after GuC has stopped!\n");
			status &= ~GUC_CTB_STATUS_UNUSED;
		}

		if (status)
			goto corrupted;
	}

	GEM_BUG_ON(head > size);

#ifdef CPTCFG_DRM_I915_DEBUG_GUC
	if (unlikely(head != READ_ONCE(desc->head))) {
		CT_ERROR(ct, "Head was modified %u != %u\n",
			 desc->head, head);
		desc->status |= GUC_CTB_STATUS_MISMATCH;
		goto corrupted;
	}
#endif
	if (unlikely(tail >= size)) {
		CT_ERROR(ct, "Invalid tail offset %u >= %u)\n",
			 tail, size);
		desc->status |= GUC_CTB_STATUS_OVERFLOW;
		goto corrupted;
	}

	/* beware of buffer wrap case */
	available = tail - head;
	if (unlikely(available < 0))
		available += size;
	CT_DEBUG(ct, "available %d (%u:%u:%u)\n", available, head, tail, size);
	GEM_BUG_ON(available < 0);

	do {
		u32 header = cmds[head];
		unsigned int len;
		unsigned int i;

		head = (head + 1) % size;

		/* message len with header */
		len = __ct_msg_size(header);
		if (unlikely(len > (u32)available)) {
			CT_ERROR(ct, "Incomplete message %*ph %*ph %*ph\n",
				 4, &header,
				 4 * (head + available - 1 > size ?
				      size - head : available - 1), &cmds[head],
				 4 * (head + available - 1 > size ?
				      available - 1 - size + head : 0), &cmds[0]);
			desc->status |= GUC_CTB_STATUS_UNDERFLOW;
			goto corrupted;
		}

		m = ct_alloc_msg(len);
		if (!m) {
			CT_ERROR(ct, "No memory for message %*ph\n", 4, &header);
			head = (head - 1) % size;
			break;
		}

		__llist_add(&m->link, msg);
		m->msg[0] = header;
		for (i = 1; i < len; i++) {
			m->msg[i] = cmds[head];
			head = (head + 1) % size;
		}
		CT_DEBUG(ct, "received %*ph\n", 4 * len, m->msg);
		available -= len;
	} while (available);

	/* update local copies */
	WRITE_ONCE(ctb->head, head);

	/* now update descriptor */
	WRITE_ONCE(desc->head, head);
	return;

corrupted:
	CT_ERROR(ct, "Corrupted descriptor head=%u tail=%u status=%#x\n",
		 desc->head, desc->tail, desc->status);
	WRITE_ONCE(ctb->head, desc->tail);
	ctb->broken = true;
	CT_DEAD(ct, READ);
}

#if IS_ENABLED(CPTCFG_DRM_I915_DEBUG_GEM)
static bool ct_check_lost_and_found(struct intel_guc_ct *ct, u32 fence)
{
	unsigned int n;
	char *buf = NULL;
	bool found = false;

	lockdep_assert_held(&ct->requests.lock);

	for (n = 0; n < ARRAY_SIZE(ct->requests.lost_and_found); n++) {
		if (ct->requests.lost_and_found[n].fence != fence)
			continue;
		found = true;

#if IS_ENABLED(CPTCFG_DRM_I915_DEBUG_GUC)
		buf = kmalloc(SZ_4K, GFP_NOWAIT);
		if (buf && stack_depot_snprint(ct->requests.lost_and_found[n].stack,
					       buf, SZ_4K, 0)) {
			CT_ERROR(ct, "Fence %u was used by action %#04x sent at\n%s",
				 fence, ct->requests.lost_and_found[n].action, buf);
			break;
		}
#endif
		CT_ERROR(ct, "Fence %u was used by action %#04x\n",
			 fence, ct->requests.lost_and_found[n].action);
		break;
	}
	kfree(buf);
	return found;
}
#else
static bool ct_check_lost_and_found(struct intel_guc_ct *ct, u32 fence)
{
	return false;
}
#endif

static int ct_handle_response(struct intel_guc_ct *ct, struct ct_incoming_msg *response)
{
	u32 len = FIELD_GET(GUC_CTB_MSG_0_NUM_DWORDS, response->msg[0]);
	u32 fence = FIELD_GET(GUC_CTB_MSG_0_FENCE, response->msg[0]);
	const u32 *hxg = &response->msg[GUC_CTB_MSG_MIN_LEN];
	const u32 *data = &hxg[GUC_HXG_MSG_MIN_LEN];
	u32 datalen = len - GUC_HXG_MSG_MIN_LEN;
	struct ct_request *req;
	unsigned long flags;
	int err = 0;

	GEM_BUG_ON(len < GUC_HXG_MSG_MIN_LEN);
	GEM_BUG_ON(FIELD_GET(GUC_HXG_MSG_0_ORIGIN, hxg[0]) != GUC_HXG_ORIGIN_GUC);
	GEM_BUG_ON(FIELD_GET(GUC_HXG_MSG_0_TYPE, hxg[0]) != GUC_HXG_TYPE_RESPONSE_SUCCESS &&
		   FIELD_GET(GUC_HXG_MSG_0_TYPE, hxg[0]) != GUC_HXG_TYPE_NO_RESPONSE_RETRY &&
		   FIELD_GET(GUC_HXG_MSG_0_TYPE, hxg[0]) != GUC_HXG_TYPE_RESPONSE_FAILURE);

	CT_DEBUG(ct, "response fence %u status %#x\n", fence, hxg[0]);

	spin_lock_irqsave(&ct->requests.lock, flags);
	list_for_each_entry(req, &ct->requests.pending, link) {
		if (unlikely(fence != req->fence)) {
			CT_DEBUG(ct, "request %u awaits response\n",
				 req->fence);
			continue;
		}
		if (unlikely(datalen > req->response_len)) {
			CT_ERROR(ct, "Response %u too long (datalen %u > %u)\n",
				 req->fence, datalen, req->response_len);
			datalen = min(datalen, req->response_len);
			err = -EMSGSIZE;
		}
		if (datalen)
			memcpy(req->response_buf, data, 4 * datalen);
		req->response_len = datalen;
		WRITE_ONCE(req->status, hxg[0]);
		wake_up_process(req->tsk);
		break;
	}
	if (list_is_head(&req->link, &ct->requests.pending)) {
		CT_ERROR(ct, "Unsolicited response message: len %u, data %#x (fence %u, last %u)\n",
			 len, hxg[0], fence, ct->requests.last_fence);
		if (!ct_check_lost_and_found(ct, fence)) {
			list_for_each_entry(req, &ct->requests.pending, link)
				CT_ERROR(ct, "request %u awaits response\n",
					 req->fence);
		}
		err = -ENOKEY;
	}
	spin_unlock_irqrestore(&ct->requests.lock, flags);

	if (unlikely(err))
		return err;

	ct_free_msg(response);
	return 0;
}

static void ct_process_request(struct intel_guc_ct *ct, struct ct_incoming_msg *request)
{
	struct intel_guc *guc = ct_to_guc(ct);
	struct intel_gt *gt = guc_to_gt(guc);
	struct intel_iov *iov = &gt->iov;
	const u32 *hxg;
	const u32 *payload;
	u32 hxg_len, action, len;
	int ret;

	hxg = &request->msg[GUC_CTB_MSG_MIN_LEN];
	hxg_len = ct_msg_size(request) - GUC_CTB_MSG_MIN_LEN;
	payload = &hxg[GUC_HXG_MSG_MIN_LEN];
	action = FIELD_GET(GUC_HXG_EVENT_MSG_0_ACTION, hxg[0]);
	len = hxg_len - GUC_HXG_MSG_MIN_LEN;

	CT_DEBUG(ct, "request %x %*ph\n", action, 4 * len, payload);

	switch (action) {
	case INTEL_GUC_ACTION_DEFAULT:
		ret = intel_guc_to_host_process_recv_msg(guc, payload, len);
		break;
	case INTEL_GUC_ACTION_DEREGISTER_CONTEXT_DONE:
		ret = intel_guc_deregister_done_process_msg(guc, payload,
							    len);
		break;
	case INTEL_GUC_ACTION_SCHED_CONTEXT_MODE_DONE:
		ret = intel_guc_sched_done_process_msg(guc, payload, len);
		break;
	case INTEL_GUC_ACTION_SCHED_ENGINE_MODE_DONE:
		ret = intel_guc_engine_sched_done_process_msg(guc, payload,
							      len);
		if (unlikely(ret))
			CT_ERROR(ct, "engine schedule context failed %x %*ph\n",
				  action, 4 * len, payload);
		break;
	case INTEL_GUC_ACTION_CONTEXT_RESET_NOTIFICATION:
		ret = intel_guc_context_reset_process_msg(guc, payload, len);
		break;
	case GUC_ACTION_GUC2HOST_NOTIFY_PAGE_FAULT:
		ret = intel_gt_pagefault_process_page_fault_msg(gt, hxg, hxg_len);
		break;
	case INTEL_GUC_ACTION_STATE_CAPTURE_NOTIFICATION:
		ret = intel_guc_error_capture_process_msg(guc, payload, len);
		if (unlikely(ret))
			CT_ERROR(ct, "error capture notification failed %x %*ph\n",
				 action, 4 * len, payload);
		break;
	case INTEL_GUC_ACTION_ENGINE_FAILURE_NOTIFICATION:
		ret = intel_guc_engine_failure_process_msg(guc, payload, len);
		break;
	case GUC_ACTION_GUC2PF_VF_STATE_NOTIFY:
		ret = intel_iov_state_process_guc2pf(iov, hxg, hxg_len);
		break;
	case GUC_ACTION_GUC2PF_ADVERSE_EVENT:
		ret = intel_iov_event_process_guc2pf(iov, hxg, hxg_len);
		break;
	case GUC_ACTION_GUC2PF_RELAY_FROM_VF:
		ret = intel_iov_relay_process_guc2pf(&iov->relay, hxg, hxg_len);
		break;
	case GUC_ACTION_GUC2VF_RELAY_FROM_PF:
		ret = intel_iov_relay_process_guc2vf(&iov->relay, hxg, hxg_len);
		break;
	case GUC_ACTION_GUC2PF_MMIO_RELAY_SERVICE:
		ret = intel_iov_service_process_mmio_relay(iov, hxg, hxg_len);
		break;
	case INTEL_GUC_ACTION_REPORT_PAGE_FAULT_REQ_DESC:
		ret = intel_pagefault_req_process_msg(guc, payload, len);
		break;
	case INTEL_GUC_ACTION_NOTIFY_FLUSH_LOG_BUFFER_TO_FILE:
		intel_guc_log_handle_flush_event(&guc->log);
		break;
	case INTEL_GUC_ACTION_NOTIFY_CRASH_DUMP_POSTED:
	case INTEL_GUC_ACTION_NOTIFY_EXCEPTION:
		ret = intel_guc_crash_process_msg(guc, action);
		break;
	case INTEL_GUC_ACTION_ACCESS_COUNTER_NOTIFY:
		ret = intel_access_counter_req_process_msg(guc, payload, len);
		break;
	default:
		break;
	}
}

static noinline void ct_incoming_request_worker_func(struct work_struct *w)
{
	struct intel_guc_ct *ct =
		container_of(w, struct intel_guc_ct, requests.worker);
	struct ct_incoming_msg *request, *n;

	llist_for_each_entry_safe(request, n, llist_reverse_order(llist_del_all(&ct->requests.incoming)), link) {
		ct_process_request(ct, request);
		ct_free_msg(request);
		cond_resched();
	}
}

static int guc_action_tlb_invalidation_done(struct intel_guc *guc, const u32 *msg, u32 len)
{
	intel_tlb_invalidation_done(guc_to_gt(guc), msg[0]);
	return 0;
}

static int guc_action_cat_error(struct intel_guc *guc, const u32 *msg, u32 len)
{
	intel_gt_pagefault_process_cat_error_msg(guc_to_gt(guc), msg[0]);
	return 0;
}

static int ct_handle_event(struct intel_guc_ct *ct, struct ct_incoming_msg *request)
{
	const u32 *hxg = &request->msg[GUC_CTB_MSG_MIN_LEN];
	u32 action = FIELD_GET(GUC_HXG_EVENT_MSG_0_ACTION, hxg[0]);
	int (*fn)(struct intel_guc *guc, const u32 *msg, u32 len) = NULL;

	GEM_BUG_ON(FIELD_GET(GUC_HXG_MSG_0_TYPE, hxg[0]) != GUC_HXG_TYPE_EVENT);

	/*
	 * Adjusting the space must be done in IRQ or deadlock can occur as the
	 * CTB processing in the below workqueue can send CTBs which creates a
	 * circular dependency if the space was returned there.
	 */
	switch (action) {
	case INTEL_GUC_ACTION_SCHED_ENGINE_MODE_DONE:
	case INTEL_GUC_ACTION_SCHED_CONTEXT_MODE_DONE:
	case INTEL_GUC_ACTION_DEREGISTER_CONTEXT_DONE:
	case INTEL_GUC_ACTION_TLB_INVALIDATION_DONE:
		g2h_release_space(ct, ct_msg_size(request));
		break;
	}
	switch (action) {
	case INTEL_GUC_ACTION_SCHED_ENGINE_MODE_DONE:
		fn = intel_guc_engine_sched_done_process_msg;
		break;
#if 0
	case INTEL_GUC_ACTION_SCHED_CONTEXT_MODE_DONE:
		fn = intel_guc_sched_done_process_msg;
		break;
	case INTEL_GUC_ACTION_DEREGISTER_CONTEXT_DONE:
		fn = intel_guc_deregister_done_process_msg;
		break;
#endif
	case INTEL_GUC_ACTION_TLB_INVALIDATION_DONE:
		fn = guc_action_tlb_invalidation_done;
		break;
	case GUC_ACTION_GUC2HOST_NOTIFY_MEMORY_CAT_ERROR:
		fn = guc_action_cat_error;
		break;
	}
	if (fn) { /* Handle tlb invalidation response in interrupt context */
		u32 hxg_len, len;

		hxg_len = ct_msg_size(request) - GUC_CTB_MSG_MIN_LEN;
		len = hxg_len - GUC_HXG_MSG_MIN_LEN;
		if (unlikely(len < 1))
			return -EPROTO;

		fn(ct_to_guc(ct), &hxg[GUC_HXG_MSG_MIN_LEN], len);
		ct_free_msg(request);
		return 0;
	}

	if (llist_add(&request->link, &ct->requests.incoming))
		intel_gt_queue_work(guc_to_gt(ct_to_guc(ct)), &ct->requests.worker);

	return 0;
}

static int ct_handle_hxg(struct intel_guc_ct *ct, struct ct_incoming_msg *msg)
{
	u32 *hxg = &msg->msg[GUC_CTB_MSG_MIN_LEN];
	u32 origin, type;
	int err;

	origin = FIELD_GET(GUC_HXG_MSG_0_ORIGIN, hxg[0]);
	if (unlikely(origin != GUC_HXG_ORIGIN_GUC)) {
		err = -EPROTO;
		goto failed;
	}

	type = FIELD_GET(GUC_HXG_MSG_0_TYPE, hxg[0]);
	switch (type) {
	case GUC_HXG_TYPE_EVENT:
		err = ct_handle_event(ct, msg);
		break;
	case GUC_HXG_TYPE_RESPONSE_SUCCESS:
	case GUC_HXG_TYPE_RESPONSE_FAILURE:
	case GUC_HXG_TYPE_NO_RESPONSE_RETRY:
		err = ct_handle_response(ct, msg);
		break;
	default:
		err = -EOPNOTSUPP;
	}

	if (unlikely(err)) {
failed:
		CT_ERROR(ct, "Failed to handle HXG message (%pe) %*ph\n",
			 ERR_PTR(err), 4 * GUC_HXG_MSG_MIN_LEN, hxg);
	}
	return err;
}

static void ct_handle_msg(struct intel_guc_ct *ct, struct ct_incoming_msg *msg)
{
	u32 format = FIELD_GET(GUC_CTB_MSG_0_FORMAT, msg->msg[0]);
	int err;

#if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST)
	if (unlikely(ct->rcv_override && ct->rcv_override(ct, msg->msg) != -ENOTSUPP)) {
		ct_free_msg(msg);
		return;
	}
#endif

	if (format == GUC_CTB_FORMAT_HXG)
		err = ct_handle_hxg(ct, msg);
	else
		err = -EOPNOTSUPP;

	if (unlikely(err)) {
		CT_ERROR(ct, "Failed to process CT message (%pe) %*ph\n",
			 ERR_PTR(err), 4 * ct_msg_size(msg), msg->msg);
		ct_free_msg(msg);
	}
}

/*
 * Return: number available remaining dwords to read (0 if empty)
 *         or a negative error code on failure
 */
void intel_guc_ct_receive(struct intel_guc_ct *ct)
{
	struct intel_guc_ct_buffer *ctb = &ct->ctbs.recv;
	struct ct_incoming_msg *msg, *n;
	LLIST_HEAD(mq);

	if (READ_ONCE(ctb->head) == READ_ONCE(ctb->desc->tail))
		return;

	rcu_read_lock(); /* lightweight serialisation with full GT resets */

	if (!spin_trylock(&ctb->lock))
		goto out;

	ct_read(ct, &mq);
	spin_unlock(&ctb->lock);
	if (llist_empty(&mq))
		goto out;

	/*
	 * Lazily make the HEAD update visible to the GuC, we do not need to
	 * force it until there is a new send which has its own explicit
	 * barriers.
	 */

	llist_for_each_entry_safe(msg, n, llist_reverse_order(mq.first), link)
		ct_handle_msg(ct, msg);

out:
	rcu_read_unlock();
}

void intel_guc_ct_reset(struct intel_guc_ct *ct)
{
	if (!ct->ctbs.recv.desc)
		return;

	/* Flush the CT interrupt handlers */
	intel_synchronize_hardirq(guc_to_gt(ct_to_guc(ct))->i915);

	/* Drain any remaining messages */
	intel_guc_ct_receive(ct);

	/* And wait for any other threads to finish processing messages */
	synchronize_rcu_expedited();

	/* Finish processing the messages */
	cancel_work_sync(&ct->requests.worker);
	ct_incoming_request_worker_func(&ct->requests.worker);
}

/*
 * When we're communicating with the GuC over CT, GuC uses events
 * to notify us about new messages being posted on the RECV buffer.
 */
void intel_guc_ct_event_handler(struct intel_guc_ct *ct)
{
	if (unlikely(!ct->enabled)) {
		/*
		 * We are unable to mask memory based interrupt from GuC,
		 * so there is a chance that an GuC CT event for VF will come
		 * just as CT will be already disabled. As we are not able to
		 * handle such an event properly, we should abandon it.
		 * In this case, calling WARN is not recommended.
		 */
		WARN(!HAS_MEMORY_IRQ_STATUS(guc_to_gt(ct_to_guc(ct))->i915),
		     "Unexpected GuC event received while CT disabled!\n");
		return;
	}

	intel_guc_ct_receive(ct);
	if (waitqueue_active(&ct->wq))
		wake_up(&ct->wq);
}

/*
 * ct_update_addresses_in_message - Shift any GGTT addresses within
 * a single message left within CTB from before post-migration recovery.
 * @ct: pointer to CT struct of the target GuC
 * @cmds: the buffer containing CT messages
 * @head: start of the target message within the buffer
 * @len: length of the target message
 * @size: size of the commands buffer
 * @shift: the address shift to be added to each GGTT reference
 */
static void ct_update_addresses_in_message(struct intel_guc_ct *ct,
					    u32 *cmds, u32 head, u32 len,
					    u32 size, s64 shift)
{
	u32 action, i, n;
	u64 offset;

#define msg(p) cmds[(head + (p)) % size]
#define fixup64(p)				\
	offset = make_u64(msg(p+1), msg(p+0));	\
	offset += shift;			\
	msg(p+0) = lower_32_bits(offset);	\
	msg(p+1) = upper_32_bits(offset)

	action = FIELD_GET(GUC_HXG_REQUEST_MSG_0_ACTION, msg(0));
	switch (action)
	{
	case INTEL_GUC_ACTION_SET_DEVICE_ENGINE_UTILIZATION_V2:
		fixup64(1);
		break;
	case INTEL_GUC_ACTION_REGISTER_CONTEXT:
	case INTEL_GUC_ACTION_REGISTER_CONTEXT_MULTI_LRC:
		/* field wq_desc */
		fixup64(5);
		/* field wq_base */
		fixup64(7);
		if (action == INTEL_GUC_ACTION_REGISTER_CONTEXT_MULTI_LRC) {
			/* field number_children */
			n = msg(10);
			/* field hwlrca and child lrcas */
			for (i = 0; i < n; i++) {
				fixup64(11 + 2 * i);
			}
		} else {
			/* field hwlrca */
			fixup64(10);
		}
		break;
	default:
		break;
	}
#undef fixup64
#undef msg
}

static int ct_update_addresses_in_buffer(struct intel_guc_ct *ct,
					 struct intel_guc_ct_buffer *ctb,
					 s64 shift, u32 *mhead, s32 available)
{
	u32 head = *mhead;
	u32 size = ctb->size;
	u32 *cmds = ctb->cmds;
	u32 header, len;

	header = cmds[head];
	head = (head + 1) % size;

	/* message len with header */
	len = __ct_msg_size(header);
	if (unlikely(len > (u32)available)) {
		CT_ERROR(ct, "Incomplete message %*ph %*ph %*ph\n",
			 4, &header,
			 4 * (head + available - 1 > size ?
			      size - head : available - 1), &cmds[head],
			 4 * (head + available - 1 > size ?
			      available - 1 - size + head : 0), &cmds[0]);
		return 0;
	}
	ct_update_addresses_in_message(ct, cmds, head, len - 1, size, shift);
	*mhead = (head + len - 1) % size;

	return available - len;
}

/**
 * intel_guc_ct_update_addresses - Shifts any GGTT addresses left
 * within CTB from before post-migration recovery.
 * @ct: pointer to CT struct of the target GuC
 */
int intel_guc_ct_update_addresses(struct intel_guc_ct *ct)
{
	struct intel_guc *guc = ct_to_guc(ct);
	struct intel_gt *gt = guc_to_gt(guc);
	struct intel_guc_ct_buffer *ctb = &ct->ctbs.send;
	struct guc_ct_buffer_desc *desc = ctb->desc;
	u32 head = ctb->head;
	u32 tail = READ_ONCE(desc->tail);
	u32 size = ctb->size;
	s32 available;
	s64 ggtt_shift;

	if (unlikely(ctb->broken))
		return -EPIPE;

	GEM_BUG_ON(head > size);

	if (unlikely(tail >= size)) {
		CT_ERROR(ct, "Invalid tail offset %u >= %u)\n",
			 tail, size);
		desc->status |= GUC_CTB_STATUS_OVERFLOW;
		goto corrupted;
	}

	available = tail - head;

	/* beware of buffer wrap case */
	if (unlikely(available < 0))
		available += size;
	CT_DEBUG(ct, "available %d (%u:%u:%u)\n", available, head, tail, size);
	GEM_BUG_ON(available < 0);

	ggtt_shift = gt->iov.vf.config.ggtt_shift;

	while (available > 0)
		available = ct_update_addresses_in_buffer(ct, ctb, ggtt_shift, &head, available);

	return 0;

corrupted:
	CT_ERROR(ct, "Corrupted descriptor head=%u tail=%u status=%#x\n",
		 head, tail, desc->status);
	ctb->broken = true;
	CT_DEAD(ct, READ);
	return -EPIPE;
}

void intel_guc_ct_print_info(struct intel_guc_ct *ct,
			     struct drm_printer *p,
			     int indent)
{
	i_printf(p, indent, "CT: %s\n", str_enabled_disabled(ct->enabled));

	if (!ct->enabled)
		return;

	indent += 2;

	i_printf(p, indent, "H2G: { Head: %u, Tail: %u, Space: %u [%u] }\n",
		 ct->ctbs.send.desc->head,
		 ct->ctbs.send.desc->tail,
		 atomic_read(&ct->ctbs.send.space) * 4,
		 CIRC_SPACE(ct->ctbs.send.desc->tail,
			    ct->ctbs.send.desc->head,
			    ct->ctbs.send.size) * 4);
	i_printf(p, indent, "G2H: { Head: %u, Tail: %u, Space: %u [%u] }\n",
		 ct->ctbs.recv.desc->head,
		 ct->ctbs.recv.desc->tail,
		 atomic_read(&ct->ctbs.recv.space) * 4,
		 CIRC_SPACE(ct->ctbs.recv.desc->tail,
			    ct->ctbs.recv.desc->head,
			    ct->ctbs.recv.size) * 4);
	i_printf(p, indent, "Requests: { pending: %s, incoming: %s, work: %s }\n",
		 str_yes_no(!list_empty(&ct->requests.pending)),
		 str_yes_no(!llist_empty(&ct->requests.incoming)),
		 str_yes_no(work_busy(&ct->requests.worker)));
}

#if IS_ENABLED(CPTCFG_DRM_I915_DEBUG_GEM)
static void ct_dead_ct_worker_func(struct work_struct *w)
{
	struct intel_guc_ct *ct = container_of(w, struct intel_guc_ct, dead_ct_worker);
	struct intel_guc *guc = ct_to_guc(ct);

	if (ct->dead_ct_reported)
		return;

	ct->dead_ct_reported = true;

	guc_info(guc, "CTB is dead - reason=0x%X\n", ct->dead_ct_reason);
	intel_klog_error_capture(guc_to_gt(guc), (intel_engine_mask_t)~0U);
}
#endif
