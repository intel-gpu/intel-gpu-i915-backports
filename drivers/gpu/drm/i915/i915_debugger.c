// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include <drm/drm_cache.h>

#include <linux/anon_inodes.h>
#include <linux/minmax.h>
#include <linux/mman.h>
#include <linux/ptrace.h>
#include <linux/dma-buf.h>

#include "gem/i915_gem_context.h"
#include "gem/i915_gem_mman.h"
#include "gem/i915_gem_vm_bind.h"
#include "gt/intel_context_types.h"
#include "gt/intel_engine_pm.h"
#include "gt/intel_engine.h"
#include "gt/intel_engine_pm.h"
#include "gt/intel_engine_regs.h"
#include "gt/intel_engine_user.h"
#include "gt/intel_engine_heartbeat.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_debug.h"
#include "gt/intel_gt_mcr.h"
#include "gt/intel_gt_pm.h"
#include "gt/intel_gt_regs.h"
#include "gt/uc/intel_guc_submission.h"
#include "gt/intel_workarounds.h"

#include "i915_debugger.h"
#include "i915_driver.h"
#include "i915_drm_client.h"
#include "i915_drv.h"
#include "i915_gpu_error.h"

#define from_event(T, event) container_of((event), typeof(*(T)), base)
#define to_event(e) (&(e)->base)

enum {
	DISCONNECT_CLIENT_CLOSE   = 1,
	DISCONNECT_SEND_TIMEOUT   = 2,
	DISCONNECT_INTERNAL_ERR   = 3,
};

static const char *disconnect_reason_to_str(const int reason)
{
	switch (reason) {
	case DISCONNECT_CLIENT_CLOSE:
		return "client closed";
	case DISCONNECT_SEND_TIMEOUT:
		return "send timeout";
	case DISCONNECT_INTERNAL_ERR:
		return "internal error";
	}

	return "unknown";
}

static void __i915_debugger_print(const struct i915_debugger * const debugger,
				  const int level,
				  const char * const prefix,
				  const char * const format, ...)
{
	struct drm_printer p;
	struct va_format vaf;
	va_list	args;

	if (level > 2)
		p = drm_debug_printer("i915_debugger");
	else if (level > 1)
		p = drm_info_printer(debugger->i915->drm.dev);
	else
		p = drm_err_printer("i915_debugger");

	va_start(args, format);

	vaf.fmt = format;
	vaf.va = &args;

	drm_printf(&p, "%s(%d/%d:%lld:%d/%d): %pV", prefix,
		   current->pid, task_tgid_nr(current),
		   debugger->session,
		   debugger->target_task->pid,
		   task_tgid_nr(debugger->target_task),
		   &vaf);

	va_end(args);
}

#define i915_debugger_print(debugger, level, prefix, fmt, ...) do { \
		if ((debugger)->debug_lvl >= (level)) {	\
			__i915_debugger_print((debugger), (level), prefix, fmt, ##__VA_ARGS__); \
		} \
	} while (0)

#define __DD(debugger, level, fmt, ...) i915_debugger_print(debugger, level, __func__, fmt, ##__VA_ARGS__)

#define DD_DEBUG_LEVEL_NONE 0
#define DD_DEBUG_LEVEL_ERR  1
#define DD_DEBUG_LEVEL_WARN 2
#define DD_DEBUG_LEVEL_INFO 3
#define DD_DEBUG_LEVEL_VERBOSE 4

/* With verbose raw addresses are seen */
#define I915_DEBUGGER_BUILD_DEBUG_LEVEL DD_DEBUG_LEVEL_VERBOSE

#define DD_INFO(debugger, fmt, ...) __DD(debugger, DD_DEBUG_LEVEL_INFO, fmt, ##__VA_ARGS__)
#define DD_WARN(debugger, fmt, ...) __DD(debugger, DD_DEBUG_LEVEL_WARN, fmt, ##__VA_ARGS__)
#define DD_ERR(debugger, fmt, ...) __DD(debugger, DD_DEBUG_LEVEL_ERR, fmt, ##__VA_ARGS__)

#if I915_DEBUGGER_BUILD_DEBUG_LEVEL >= DD_DEBUG_LEVEL_VERBOSE
#define ND_VERBOSE(i915, fmt, ...) DRM_DEV_DEBUG_DRIVER((i915)->drm.dev, fmt, ##__VA_ARGS__)
#define DD_VERBOSE(debugger, fmt, ...) __DD(debugger, DD_DEBUG_LEVEL_VERBOSE, fmt, ##__VA_ARGS__)
#else
#define ND_VERBOSE(i915, fmt, ...)
#define DD_VERBOSE(debugger, fmt, ...)
#endif

#define DEBUG_ACK_EVENT(debugger, prefix, _e) do { \
		DD_INFO(debugger, "%s: type=%u, flags=0x%08x, seqno=%llu", \
			prefix, \
			(_e)->type, \
			(_e)->flags, \
			(_e)->seqno); \
	} while(0)

#define DEBUG_ACK(d, _a) DEBUG_ACK_EVENT(d, "ack", &((_a)->event))

static const char *event_type_to_str(u32 type)
{
	static const char * const type_str[] = {
		"none",
		"read",
		"client",
		"context",
		"uuid",
		"vm",
		"vm-bind",
		"context-param",
		"eu-attention",
		"engines",
		"unknown",
	};

	if (type > ARRAY_SIZE(type_str) - 1)
		type = ARRAY_SIZE(type_str) - 1;

	return type_str[type];
}

static const char *event_flags_to_str(const u32 flags)
{
	if (flags & PRELIM_DRM_I915_DEBUG_EVENT_CREATE) {
		if (flags & PRELIM_DRM_I915_DEBUG_EVENT_NEED_ACK)
			return "create-need-ack";
		else
			return "create";
	} else if (flags & PRELIM_DRM_I915_DEBUG_EVENT_DESTROY)
		return "destroy";
	else if (flags & PRELIM_DRM_I915_DEBUG_EVENT_STATE_CHANGE)
		return "state-change";

	return "unknown";
}

#define EVENT_PRINT_MEMBER(d, p, s, m, fmt, type) do { \
		BUILD_BUG_ON(sizeof(s->m) != sizeof(type)); \
		__i915_debugger_print(d, DD_DEBUG_LEVEL_INFO, p, \
				      "  %s->%s = " fmt, #s, #m, (type)s->m); \
	} while(0)

#define EVENT_PRINT_MEMBER_U64(d, p, s, n) EVENT_PRINT_MEMBER(d, p, s, n, "%llu", u64)
#define EVENT_PRINT_MEMBER_U32(d, p, s, n) EVENT_PRINT_MEMBER(d, p, s, n, "%u", u32)
#define EVENT_PRINT_MEMBER_U16(d, p, s, n) EVENT_PRINT_MEMBER(d, p, s, n, "%u", u16)
#define EVENT_PRINT_MEMBER_U64X(d, p, s, n) EVENT_PRINT_MEMBER(d, p, s, n, "0x%llx", u64)
#define EVENT_PRINT_MEMBER_U32X(d, p, s, n) EVENT_PRINT_MEMBER(d, p, s, n, "0x%x", u32)
#define EVENT_PRINT_MEMBER_HANDLE(d, p, s, n) EVENT_PRINT_MEMBER_U64(d, p, s, n)

typedef void (*debug_event_printer_t)(const struct i915_debugger * const debugger,
				      const char * const prefix,
				      const struct i915_debug_event * const event);

static void event_printer_client(const struct i915_debugger * const debugger,
				 const char * const prefix,
				 const struct i915_debug_event * const event)
{
	const struct i915_debug_event_client * const client =
		from_event(client, event);

	EVENT_PRINT_MEMBER_HANDLE(debugger, prefix, client, handle);
}

static void event_printer_context(const struct i915_debugger * const debugger,
				  const char * const prefix,
				  const struct i915_debug_event * const event)
{
	const struct i915_debug_event_context * const context =
		from_event(context, event);

	EVENT_PRINT_MEMBER_HANDLE(debugger, prefix, context, client_handle);
	EVENT_PRINT_MEMBER_HANDLE(debugger, prefix, context, handle);
}

static void event_printer_uuid(const struct i915_debugger * const debugger,
			       const char * const prefix,
			       const struct i915_debug_event * const event)
{
	const struct i915_debug_event_uuid * const uuid =
		from_event(uuid, event);

	EVENT_PRINT_MEMBER_HANDLE(debugger, prefix, uuid, client_handle);
	EVENT_PRINT_MEMBER_HANDLE(debugger, prefix, uuid, handle);
	EVENT_PRINT_MEMBER_HANDLE(debugger, prefix, uuid, class_handle);
	EVENT_PRINT_MEMBER_U64(debugger, prefix, uuid, payload_size);
}

static void event_printer_vm(const struct i915_debugger * const debugger,
			     const char * const prefix,
			     const struct i915_debug_event * const event)
{
	const struct i915_debug_event_vm * const vm = from_event(vm, event);

	EVENT_PRINT_MEMBER_HANDLE(debugger, prefix, vm, client_handle);
	EVENT_PRINT_MEMBER_HANDLE(debugger, prefix, vm, handle);
}

static void event_printer_vma(const struct i915_debugger * const debugger,
			      const char * const prefix,
			      const struct i915_debug_event * const event)
{
	const struct i915_debug_event_vm_bind * const ev = from_event(ev, event);
	unsigned i;

	EVENT_PRINT_MEMBER_HANDLE(debugger, prefix, ev, client_handle);
	EVENT_PRINT_MEMBER_HANDLE(debugger, prefix, ev, vm_handle);
	EVENT_PRINT_MEMBER_U64X(debugger, prefix, ev, va_start);
	EVENT_PRINT_MEMBER_U64X(debugger, prefix, ev, va_length);
	EVENT_PRINT_MEMBER_U32(debugger, prefix, ev, num_uuids);
	EVENT_PRINT_MEMBER_U32(debugger, prefix, ev, flags);

	for (i = 0; i < ev->num_uuids; i++)
		i915_debugger_print(debugger, DD_DEBUG_LEVEL_INFO, prefix,
				    "  vma->uuids[%u] = %lld",
				    i, ev->uuids[i]);
}

static void event_printer_context_param(const struct i915_debugger * const debugger,
					const char * const prefix,
					const struct i915_debug_event * const event)
{
	const struct i915_debug_event_context_param * const context_param =
		from_event(context_param, event);
	const struct drm_i915_gem_context_param * const context_param_param =
		&context_param->param;

	EVENT_PRINT_MEMBER_HANDLE(debugger, prefix, context_param, client_handle);
	EVENT_PRINT_MEMBER_HANDLE(debugger, prefix, context_param, ctx_handle);
	EVENT_PRINT_MEMBER_U32(debugger, prefix, context_param_param, ctx_id);
	EVENT_PRINT_MEMBER_U64(debugger, prefix, context_param_param, param);
	EVENT_PRINT_MEMBER_U64(debugger, prefix, context_param_param, value);
}

static void event_printer_eu_attention(const struct i915_debugger * const debugger,
				       const char * const prefix,
				       const struct i915_debug_event * const event)
{
	const struct i915_debug_event_eu_attention * const eu_attention =
		from_event(eu_attention, event);
	const struct i915_engine_class_instance * const eu_attention_ci =
		&eu_attention->ci;
	unsigned int i, count;

	EVENT_PRINT_MEMBER_HANDLE(debugger, prefix, eu_attention, client_handle);
	EVENT_PRINT_MEMBER_HANDLE(debugger, prefix, eu_attention, ctx_handle);
	EVENT_PRINT_MEMBER_HANDLE(debugger, prefix, eu_attention, lrc_handle);
	EVENT_PRINT_MEMBER_U32X(debugger, prefix, eu_attention, flags);
	EVENT_PRINT_MEMBER_U16(debugger, prefix, eu_attention_ci, engine_class);
	EVENT_PRINT_MEMBER_U16(debugger, prefix, eu_attention_ci, engine_instance);
	EVENT_PRINT_MEMBER_U32(debugger, prefix, eu_attention, bitmask_size);

	for (count = 0, i = 0; i < eu_attention->bitmask_size; i++) {
		if (eu_attention->bitmask[i]) {
			i915_debugger_print(debugger, DD_DEBUG_LEVEL_INFO, prefix,
					    "  eu_attention->bitmask[%u] = 0x%x",
					    i, eu_attention->bitmask[i]);
			count++;
		}

		if (debugger->debug_lvl < DD_DEBUG_LEVEL_VERBOSE && count >= 8) {
			i915_debugger_print(debugger, DD_DEBUG_LEVEL_INFO, prefix,
					    "  eu_attention->bitmask[%u]++ <snipped>", i);
			break;
		}
	}
}

static void event_printer_engines(const struct i915_debugger * const debugger,
				  const char * const prefix,
				  const struct i915_debug_event * const event)
{
	const struct i915_debug_event_engines * const engines = from_event(engines, event);
	u64 i;

	EVENT_PRINT_MEMBER_HANDLE(debugger, prefix, engines, ctx_handle);
	EVENT_PRINT_MEMBER_U64(debugger, prefix, engines, num_engines);

	for (i = 0; i < engines->num_engines; i++) {
		const struct i915_debug_engine_info * const ei = &engines->engines[i];

		i915_debugger_print(debugger, DD_DEBUG_LEVEL_INFO, prefix,
				    "  engines->engines[%lld] = engine_class=%u, engine_instance=%u, lrc_handle = %lld",
				    i, ei->engine.engine_class,
				    ei->engine.engine_instance, ei->lrc_handle);
	}
}

static void i915_debugger_print_event(const struct i915_debugger * const debugger,
				      const char * const prefix,
				      const struct i915_debug_event * const event)
{
	static const debug_event_printer_t event_printers[] = {
		NULL,
		NULL,
		event_printer_client,
		event_printer_context,
		event_printer_uuid,
		event_printer_vm,
		event_printer_vma,
		event_printer_context_param,
		event_printer_eu_attention,
		event_printer_engines,
	};
	debug_event_printer_t event_printer = NULL;

	if (likely(debugger->debug_lvl < DD_DEBUG_LEVEL_VERBOSE))
		return;

	__i915_debugger_print(debugger, DD_DEBUG_LEVEL_VERBOSE, prefix,
			      "%s:%s type=%u, flags=0x%08x, seqno=%llu, size=%llu\n",
			      event_type_to_str(event->type),
			      event_flags_to_str(event->flags),
			      event->type,
			      event->flags,
			      event->seqno,
			      event->size);

	if (event->type < ARRAY_SIZE(event_printers))
		event_printer = event_printers[event->type];

	if (event_printer)
		event_printer(debugger, prefix, event);
	else
		DD_VERBOSE(debugger, "no event printer found for type=%u\n", event->type);
}

static inline struct i915_debug_event *
event_fifo_pending(struct i915_debugger *debugger)
{
	struct i915_debug_event *event;

	if (kfifo_peek(&debugger->event_fifo, &event))
		return event;

	return NULL;
}

static inline bool
event_fifo_has_events(const struct i915_debugger * const debugger)
{
	return !kfifo_is_empty(&debugger->event_fifo);
}

static inline struct i915_debug_event *
event_fifo_get(struct i915_debugger *debugger)
{
	struct i915_debug_event *event;

	if (kfifo_get(&debugger->event_fifo, &event))
		return event;

	return NULL;
}

static inline bool event_fifo_put(struct i915_debugger *debugger,
				  const struct i915_debug_event *event)
{
	return kfifo_put(&debugger->event_fifo, event);
}

static inline bool event_fifo_full(struct i915_debugger *debugger)
{
	return kfifo_is_full(&debugger->event_fifo);
}

static void event_fifo_drain(struct i915_debugger *debugger)
{
	struct i915_debug_event *event;

	while (kfifo_get(&debugger->event_fifo, &event))
		kfree(event);
}

static void _i915_debugger_free(struct kref *ref)
{
	struct i915_debugger *debugger = container_of(ref, typeof(*debugger), ref);

	event_fifo_drain(debugger);

	put_task_struct(debugger->target_task);
	xa_destroy(&debugger->resources_xa);
	kfree_rcu(debugger, rcu);
}

static void i915_debugger_put(struct i915_debugger *debugger)
{
	kref_put(&debugger->ref, _i915_debugger_free);
}

static inline bool
is_debugger_closed(const struct i915_debugger * const debugger)
{
	return list_empty(&debugger->connection_link);
}

static void i915_debugger_detach(struct i915_debugger *debugger)
{
	struct drm_i915_private * const i915 = debugger->i915;
	unsigned long flags;

	spin_lock_irqsave(&i915->debuggers.lock, flags);
	if (!is_debugger_closed(debugger)) {
		DD_INFO(debugger, "session %lld detached", debugger->session);
		list_del_init(&debugger->connection_link);
	}
	spin_unlock_irqrestore(&i915->debuggers.lock, flags);
}

static const char *get_driver_name(struct dma_fence *fence)
{
	return "[" DRIVER_NAME "]";
}

static const char *get_timeline_name(struct dma_fence *fence)
{
	return "debugger";
}

static const struct dma_fence_ops debugger_fence_ops = {
	.get_driver_name = get_driver_name,
	.get_timeline_name = get_timeline_name,
};

struct debugger_fence {
	struct dma_fence base;
	struct i915_sw_fence chain;
	struct i915_sw_dma_fence_cb cb;
	spinlock_t lock;
};

static int
fence_notify(struct i915_sw_fence *fence, enum i915_sw_fence_notify state)
{
	struct debugger_fence *f = container_of(fence, typeof(*f), chain);

	switch (state) {
	case FENCE_COMPLETE:
		dma_fence_signal(&f->base);
		break;

	case FENCE_FREE:
		i915_sw_fence_fini(&f->chain);
		dma_fence_put(&f->base);
		break;
	}

	return NOTIFY_DONE;
}

static struct debugger_fence *create_debugger_fence(gfp_t gfp)
{
	struct debugger_fence *f;

	f = kzalloc(sizeof(*f), gfp);
	if (!f)
		return NULL;

	spin_lock_init(&f->lock);
	dma_fence_init(&f->base, &debugger_fence_ops, &f->lock, 0, 0);
	i915_sw_fence_init(&f->chain, fence_notify);

	return f;
}

#define fetch_ack(x) \
	rb_entry(READ_ONCE(x), typeof(struct i915_debug_ack), rb_node)

static inline int compare_ack(const u64 a, const u64 b)
{
	if (a < b)
		return -1;
	else if (a > b)
		return 1;

	return 0;
}

static struct i915_debug_ack *
find_ack(struct i915_debugger *debugger, u64 seqno)
{
	struct rb_node *node = debugger->ack_tree.rb_node;

	lockdep_assert_held(&debugger->lock);

	while (node) {
		struct i915_debug_ack * const ack = fetch_ack(node);
		const int result = compare_ack(seqno, ack->event.seqno);

		if (result < 0)
			node = node->rb_left;
		else if (result > 0)
			node = node->rb_right;
		else
			return ack;
	}
	return NULL;
}

static bool
insert_ack(struct i915_debugger *debugger, struct i915_debug_ack *ack)
{
	struct rb_root *root = &debugger->ack_tree;
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;

	lockdep_assert_held(&debugger->lock);

	while (*p) {
		struct i915_debug_ack *__ack;
		int result;

		parent = *p;

		__ack = fetch_ack(parent);
		result = compare_ack(ack->event.seqno, __ack->event.seqno);
		if (result < 0)
			p = &parent->rb_left;
		else if (result > 0)
			p = &parent->rb_right;
		else
			return false;
	}

	rb_link_node(&ack->rb_node, parent, p);
	rb_insert_color(&ack->rb_node, root);

	DEBUG_ACK(debugger, ack);
	return true;
}

static struct dma_fence *
vma_await_ack(struct i915_vma *vma, struct dma_fence *fence)
{
	return i915_active_set_exclusive(&vma->active, fence) ?: i915_active_fence_get_or_error(&vma->obj->mm.migrate);
}

static void *
prepare_vm_bind_ack(const struct i915_debug_ack *ack,
		    struct i915_vma *vma,
		    gfp_t gfp)
{
	struct debugger_fence *f;
	struct dma_fence *prev;

	if (!(ack->event.flags & PRELIM_DRM_I915_DEBUG_EVENT_CREATE))
		return ERR_PTR(-EINVAL);

	if (!vma)
		return ERR_PTR(-EINVAL);

	f = create_debugger_fence(gfp);
	if (!f)
		return ERR_PTR(-ENOMEM);

	prev = vma_await_ack(vma, &f->base);
	if (IS_ERR(prev)) {
		i915_sw_fence_set_error_once(&f->chain, PTR_ERR(prev));
	} else if (prev) {
		__i915_sw_fence_await_dma_fence(&f->chain, prev, &f->cb);
		dma_fence_put(prev);
	}

	i915_sw_fence_await(&f->chain);
	i915_sw_fence_commit(&f->chain);

	return &f->chain;
}

static void handle_vm_bind_ack(struct i915_debug_ack *ack)
{
	i915_sw_fence_complete(ack->event.ack_data);
}

static struct i915_debug_ack *
remove_ack(struct i915_debugger *debugger, u64 seqno)
{
	struct rb_root * const root = &debugger->ack_tree;
	struct i915_debug_ack *ack;

	lockdep_assert_held(&debugger->lock);

	ack = find_ack(debugger, seqno);
	if (!ack)
		return NULL;

	rb_erase(&ack->rb_node, root);
	DEBUG_ACK(debugger, ack);
	return ack;
}

static void
handle_ack(struct i915_debugger *debugger, struct i915_debug_ack *ack)
{
	switch (ack->event.type) {
	case PRELIM_DRM_I915_DEBUG_EVENT_VM_BIND:
		handle_vm_bind_ack(ack);
		break;
	}

	DEBUG_ACK(debugger, ack);
}

static struct i915_debug_ack *
create_ack(struct i915_debugger *debugger,
	   const struct i915_debug_event *event,
	   void *data,
	   gfp_t gfp)
{
	struct i915_debug_ack *ack;

	ack = kzalloc(sizeof(*ack), gfp);
	if (!ack)
		return ERR_PTR(-ENOMEM);

	ack->event.type = event->type;
	ack->event.flags = event->flags;
	ack->event.seqno = event->seqno;

	switch (ack->event.type) {
	case PRELIM_DRM_I915_DEBUG_EVENT_VM_BIND:
		data = prepare_vm_bind_ack(ack, data, gfp);
		break;
	default:
		GEM_WARN_ON(ack->event.type);
		data = ERR_PTR(-EINVAL);
		break;
	}
	if (IS_ERR(data)) {
		kfree(ack);
		return data;
	}

	ack->event.ack_data = data;
	return ack;
}

static void release_acks(struct i915_debugger *debugger)
{
	struct rb_root *root = &debugger->ack_tree;
	struct i915_debug_ack *ack, *n;

	lockdep_assert_held(&debugger->lock);

	rbtree_postorder_for_each_entry_safe(ack, n, root, rb_node) {
		handle_ack(debugger, ack);
		kfree(ack);
	}

	*root = RB_ROOT;
}

static void
i915_debugger_ctx_process_callback(const struct i915_gem_context *ctx,
				   void (*func)(struct intel_context *ce))
{
	struct i915_gem_engines_iter it;
	struct intel_context *ce;

	for_each_gem_engine(ce, ctx->engines, it)
		if (i915_debugger_active_on_context(ce))
			func(ce);
}

static void
i915_debugger_restore_ctx_schedule_params(struct i915_debugger *debugger)
{
	struct i915_drm_client *client;
	unsigned long idx;

	rcu_read_lock();
	xa_for_each(&debugger->i915->clients.xarray, idx, client) {
		struct i915_gem_context *ctx;

		client = i915_drm_client_get_rcu(client);
		if (!client)
			continue;

		list_for_each_entry_rcu(ctx, &client->ctx_list, client_link) {
			rcu_read_unlock();
			i915_debugger_ctx_process_callback(ctx,
					intel_context_reset_preemption_timeout);
			rcu_read_lock();
		}

		i915_drm_client_put(client);
	}
	rcu_read_unlock();
}

static void i915_debugger_disconnect__locked(struct i915_debugger *debugger,
					    int reason)
{
	GEM_WARN_ON(!reason);
	lockdep_assert_held(&debugger->lock);

	i915_debugger_detach(debugger);

	if (!debugger->disconnect_reason) {
		debugger->disconnect_reason = reason;
		release_acks(debugger);
		i915_debugger_restore_ctx_schedule_params(debugger);
		DD_INFO(debugger, "disconnected: %s",
			disconnect_reason_to_str(reason));
	} else {
		DD_INFO(debugger, "earlier disconnected with %s (now %d)",
			disconnect_reason_to_str(debugger->disconnect_reason),
			reason);
	}

	complete_all(&debugger->discovery);
	wake_up_all(&debugger->write_done);
	complete_all(&debugger->read_done);
}

static void i915_debugger_disconnect_timeout(struct i915_debugger *debugger)
{
	i915_debugger_disconnect__locked(debugger, DISCONNECT_SEND_TIMEOUT);
}

static void i915_debugger_disconnect_err(struct i915_debugger *debugger)
{
	mutex_lock(&debugger->lock);
	i915_debugger_disconnect__locked(debugger, DISCONNECT_INTERNAL_ERR);
	mutex_unlock(&debugger->lock);
}

static void i915_debugger_client_close(struct i915_debugger *debugger)
{
	mutex_lock(&debugger->lock);
	i915_debugger_disconnect__locked(debugger, DISCONNECT_CLIENT_CLOSE);
	mutex_unlock(&debugger->lock);
}

static int i915_debugger_disconnect_retcode(struct i915_debugger *debugger)
{
	GEM_WARN_ON(!debugger->disconnect_reason);

	if (debugger->disconnect_reason == DISCONNECT_SEND_TIMEOUT)
		return -ENXIO;

	return -ENODEV;
}

static bool was_debugger_disconnected(const struct i915_debugger *debugger)
{
	GEM_BUG_ON(!debugger->disconnect_reason);

	return debugger->disconnect_reason != DISCONNECT_CLIENT_CLOSE;
}

static __poll_t i915_debugger_poll(struct file *file, poll_table *wait)
{
	struct i915_debugger * const debugger = file->private_data;
	__poll_t ret = 0;

	poll_wait(file, &debugger->write_done, wait);

	if (is_debugger_closed(debugger)) {
		ret |= EPOLLHUP;
		if (was_debugger_disconnected(debugger))
			ret |= EPOLLERR;
	}

	if (event_fifo_has_events(debugger))
		ret |= EPOLLIN;

	return ret;
}

static ssize_t i915_debugger_read(struct file *file,
				  char __user *buf,
				  size_t count,
				  loff_t *ppos)
{
	return 0;
}

static inline u64 client_session(const struct i915_drm_client *client)
{
	return client ? READ_ONCE(client->debugger_session) : 0;
}

#define for_each_debugger(debugger, head) \
	list_for_each_entry(debugger, head, connection_link)

static struct i915_debugger *
i915_debugger_get(const struct i915_drm_client * const client)
{
	struct drm_i915_private *i915;
	const u64 session = client_session(client);
	struct i915_debugger *debugger, *iter;
	unsigned long flags;

	if (likely(!session))
		return NULL;

	i915 = client->clients->i915;
	debugger = NULL;

	spin_lock_irqsave(&i915->debuggers.lock, flags);
	for_each_debugger(iter, &i915->debuggers.list) {
		if (iter->session != session)
			continue;

		kref_get(&iter->ref);
		debugger = iter;
		break;
	}
	spin_unlock_irqrestore(&i915->debuggers.lock, flags);

	return debugger;
}

static struct i915_debugger *
i915_debugger_find_task_get(struct drm_i915_private *i915,
			    struct task_struct *task)
{
	struct i915_debugger *debugger, *iter;
	unsigned long flags;

	debugger = NULL;

	spin_lock_irqsave(&i915->debuggers.lock, flags);
	for_each_debugger(iter, &i915->debuggers.list) {
		if (!same_thread_group(iter->target_task, task))
			continue;

		kref_get(&iter->ref);
		debugger = iter;
		break;
	}
	spin_unlock_irqrestore(&i915->debuggers.lock, flags);

	return debugger;
}

static inline bool client_debugged(const struct i915_drm_client * const client)
{
	struct i915_debugger *debugger;

	if (likely(!client_session(client)))
		return false;

	debugger = i915_debugger_get(client);
	if (debugger)
		i915_debugger_put(debugger);

	return debugger != NULL;
}

static int _i915_debugger_queue_event(struct i915_debugger * const debugger,
				      const struct i915_debug_event *event,
				      void *ack_data,
				      gfp_t gfp)
{
	struct drm_i915_private * const i915 = debugger->i915;
	const unsigned long user_ms = i915->params.debugger_timeout_ms;
	const unsigned long retry_timeout_ms = 100;
	struct i915_debug_ack *ack = NULL;
	ktime_t disconnect_ts, now;
	unsigned long timeout;
	bool expired;

	/* No need to send base events */
	if (event->size <= sizeof(struct prelim_drm_i915_debug_event) ||
	    !event->type ||
	    event->type == PRELIM_DRM_I915_DEBUG_EVENT_READ) {
		GEM_WARN_ON(event->size <= sizeof(struct prelim_drm_i915_debug_event));
		GEM_WARN_ON(!event->type);
		GEM_WARN_ON(event->type == PRELIM_DRM_I915_DEBUG_EVENT_READ);

		return -EINVAL;
	}

	if (event->flags & PRELIM_DRM_I915_DEBUG_EVENT_NEED_ACK)
		ack = create_ack(debugger, event, ack_data, gfp);

	disconnect_ts = ktime_add_ms(ktime_get_raw(), user_ms);
	mutex_lock(&debugger->lock);

	do {
		const struct i915_debug_event *blocking_event;
		u64 blocking_seqno;

		if (is_debugger_closed(debugger)) {
			DD_INFO(debugger, "send: debugger was closed\n");
			goto closed;
		}

		if (!event_fifo_full(debugger))
			break;

		/*
		 * If we did not get access to event, there might be stuck
		 * reader or other writer have raced us. Take a snapshot
		 * of that event seqno.
		 */
		blocking_event = event_fifo_pending(debugger);
		if (GEM_WARN_ON(!blocking_event))
			goto disconnect_err;

		blocking_seqno = blocking_event->seqno;

		mutex_unlock(&debugger->lock);

		now = ktime_get_raw();
		if (user_ms == 0)
			disconnect_ts = ktime_add_ms(now, retry_timeout_ms);

		if (ktime_sub(disconnect_ts, now) > 0) {
			timeout = min_t(unsigned long,
					retry_timeout_ms,
					ktime_to_ms(ktime_sub(disconnect_ts, now)));

			wait_for_completion_timeout(&debugger->read_done,
						    msecs_to_jiffies(timeout));

			now = ktime_get_raw();
		}

		expired = user_ms ? ktime_after(now, disconnect_ts) : false;

		mutex_lock(&debugger->lock);

		/* Postpone expiration if some other writer made progress */
		blocking_event = is_debugger_closed(debugger) ?
			NULL : event_fifo_pending(debugger);
		if (!blocking_event)
			expired = true;
		else if (blocking_event->seqno != blocking_seqno)
			expired = false;
	} while (!expired);

	if (is_debugger_closed(debugger))
		goto closed;

	if (event_fifo_full(debugger)) {
		DD_INFO(debugger, "send: fifo full (no readers?). disconnecting");
		i915_debugger_disconnect_timeout(debugger);
		goto closed;
	}

	reinit_completion(&debugger->read_done);
	if (!event_fifo_put(debugger, event)) {
		DD_ERR(debugger, "disconnect: fifo put fail\n");
		goto disconnect_err;
	}
	/* Transfer of ownership into the fifo from caller */
	event = NULL;

	if (ack) {
		if (IS_ERR(ack)) {
			DD_ERR(debugger, "disconnect: ack not created %ld", PTR_ERR(ack));
			goto disconnect_err;
		}

		if (!insert_ack(debugger, ack)) {
			DD_ERR(debugger, "disconnect: duplicate ack found for %llu",
			       ack->event.seqno);
			handle_ack(debugger, ack);
			kfree(ack);
			goto disconnect_err;
		}
	}

	mutex_unlock(&debugger->lock);

	wake_up_all(&debugger->write_done);

	return 0;

disconnect_err:
	i915_debugger_disconnect__locked(debugger, DISCONNECT_INTERNAL_ERR);
closed:
	mutex_unlock(&debugger->lock);

	/* If ownership was transferred, kfree(NULL) is valid */
	kfree(event);

	return -ENODEV;
}

static int i915_debugger_queue_event(struct i915_debugger * const debugger,
				     const struct i915_debug_event *event)
{
	return _i915_debugger_queue_event(debugger, event, NULL, GFP_KERNEL);
}

static struct i915_debug_event *
__i915_debugger_create_event(struct i915_debugger * const debugger,
			     u32 type, u32 flags, u32 size, gfp_t gfp)
{
	struct i915_debug_event *event;

	GEM_WARN_ON(size <= sizeof(*event));

	event = kzalloc(size, gfp);
	if (!event) {
		DD_ERR(debugger, "unable to create event 0x%08x (ENOMEM), disconnecting", type);
		i915_debugger_disconnect_err(debugger);
		return NULL;
	}

	event->type = type;
	event->flags = flags;
	event->size = size;

	return event;
}

static struct i915_debug_event *
i915_debugger_create_event(struct i915_debugger * const debugger,
			   u32 type, u32 flags, u32 size, gfp_t gfp)
{
	struct i915_debug_event *event;

	event = __i915_debugger_create_event(debugger, type, flags, size, gfp);
	if (event)
		event->seqno = atomic_long_inc_return(&debugger->event_seqno);

	return event;
}

static long i915_debugger_read_event(struct i915_debugger *debugger,
				     const unsigned long arg,
				     const bool nonblock)
{
	struct prelim_drm_i915_debug_event __user * const user_orig =
		(void __user *)(arg);
	struct prelim_drm_i915_debug_event user_event;
	const struct i915_debug_event *event;
	unsigned int waits;
	long ret;

	if (copy_from_user(&user_event, user_orig, sizeof(user_event)))
		return -EFAULT;

	if (!user_event.type)
		return -EINVAL;

	if (user_event.type > PRELIM_DRM_I915_DEBUG_EVENT_MAX_EVENT)
		return -EINVAL;

	if (user_event.type != PRELIM_DRM_I915_DEBUG_EVENT_READ)
		return -EINVAL;

	if (user_event.size < sizeof(*user_orig))
		return -EINVAL;

	if (user_event.flags)
		return -EINVAL;

	waits = 0;
	event = NULL;
	mutex_lock(&debugger->lock);
	do {
		event = event_fifo_pending(debugger);
		if (event)
			break;

		mutex_unlock(&debugger->lock);
		if (nonblock) {
			ret = -EAGAIN;
			goto out;
		}

		ret = wait_event_interruptible_timeout(debugger->write_done,
						       event_fifo_has_events(debugger),
						       msecs_to_jiffies(100));
		if (ret < 0)
			goto out;

		mutex_lock(&debugger->lock);
	} while (waits++ < 10);

	if (!event) {
		if (is_debugger_closed(debugger))
			ret = i915_debugger_disconnect_retcode(debugger);
		else
			ret = -ETIMEDOUT;

		complete(&debugger->read_done);
		goto unlock;
	}

	if (unlikely(user_event.size < event->size)) {
		ret = -EMSGSIZE;
		goto unlock;
	}

	if (!access_ok(user_orig, event->size)) {
		ret = -EFAULT;
		goto unlock;
	}

	event = event_fifo_get(debugger);

	complete(&debugger->read_done);
	mutex_unlock(&debugger->lock);

	ret = __copy_to_user(user_orig, event, event->size);
	if (ret)
		ret = -EFAULT;

	i915_debugger_print_event(debugger, "read", event);

out:
	kfree(event);
	return ret;

unlock:
	mutex_unlock(&debugger->lock);
	goto out;
}

static long i915_debugger_read_uuid_ioctl(struct i915_debugger *debugger,
					  unsigned int cmd,
					  const u64 arg)
{
	struct prelim_drm_i915_debug_read_uuid read_arg;
	struct i915_uuid_resource *uuid;
	struct i915_drm_client *client;
	long ret = 0;

	if (is_debugger_closed(debugger))
		return i915_debugger_disconnect_retcode(debugger);

	if (_IOC_SIZE(cmd) < sizeof(read_arg))
		return -EINVAL;

	if (!(_IOC_DIR(cmd) & _IOC_WRITE))
		return -EINVAL;

	if (!(_IOC_DIR(cmd) & _IOC_READ))
		return -EINVAL;

	if (copy_from_user(&read_arg, u64_to_user_ptr(arg), sizeof(read_arg)))
		return -EFAULT;

	if (read_arg.flags)
		return -EINVAL;

	if (!access_ok(u64_to_user_ptr(read_arg.payload_ptr),
		       read_arg.payload_size))
		return -EFAULT;

	DD_INFO(debugger, "read_uuid: client_handle=%lld, handle=%lld, flags=0x%x",
		read_arg.client_handle, read_arg.handle, read_arg.flags);

	uuid = NULL;
	rcu_read_lock();
	client = xa_load(&debugger->i915->clients.xarray,
			 read_arg.client_handle);
	if (client) {
		xa_lock(&client->uuids_xa);
		uuid = xa_load(&client->uuids_xa, read_arg.handle);
		if (uuid)
			i915_uuid_get(uuid);
		xa_unlock(&client->uuids_xa);
	}
	rcu_read_unlock();
	if (!uuid)
		return -ENOENT;

	if (read_arg.payload_size) {
		if (read_arg.payload_size < uuid->size) {
			ret = -EINVAL;
			goto out_uuid;
		}

		/* This limits us to a maximum payload size of 2G */
		if (copy_to_user(u64_to_user_ptr(read_arg.payload_ptr),
				 uuid->ptr, uuid->size)) {
			ret = -EFAULT;
			goto out_uuid;
		}
	}

	read_arg.payload_size = uuid->size;
	memcpy(read_arg.uuid, uuid->uuid, sizeof(read_arg.uuid));

	if (copy_to_user(u64_to_user_ptr(arg), &read_arg, sizeof(read_arg)))
		ret = -EFAULT;

	DD_INFO(debugger, "read_uuid: payload delivery of %lld bytes returned %lld\n", uuid->size, ret);

out_uuid:
	i915_uuid_put(uuid);
	return ret;
}

static void gen12_invalidate_inst_cache(struct drm_i915_private *i915)
{
	const u32 bit = GEN12_INST_STATE_CACHE_INVALIDATE;
	intel_wakeref_t wakeref;
	struct intel_gt *gt;
	int id;

	for_each_gt(gt, i915, id) {
		with_intel_gt_pm_if_awake(gt, wakeref) {
			intel_uncore_write(gt->uncore, GEN9_CS_DEBUG_MODE2,
					   _MASKED_BIT_ENABLE(bit));
		}
	}
}

static int engine_rcu_async_flush(struct intel_engine_cs *engine,
				  u32 mask,
				  unsigned int timeout_ms)
{
	struct drm_i915_private *i915 = engine->i915;
	struct intel_uncore * const uncore = engine->uncore;
	const i915_reg_t psmi_addr = RING_PSMI_CTL(engine->mmio_base);
	const enum forcewake_domains fw = FORCEWAKE_GT | FORCEWAKE_RENDER;
	u32 psmi_ctrl;
	u32 id = 0;
	int ret;

	if (engine->class == COMPUTE_CLASS)
		id = engine->instance + 1;
	else if (engine->class == RENDER_CLASS)
		id = 0;
	else
		GEM_WARN_ON(engine->class);

	if (!intel_engine_pm_get_if_awake(engine))
		return 0;

	intel_uncore_forcewake_get(uncore, fw);
	mutex_lock(&i915->debuggers.eu_flush_lock);

	psmi_ctrl = intel_uncore_read_fw(uncore, psmi_addr);
	if (!(psmi_ctrl & GEN6_PSMI_SLEEP_MSG_DISABLE))
		intel_uncore_write_fw(uncore, psmi_addr,
				      _MASKED_BIT_ENABLE(GEN6_PSMI_SLEEP_MSG_DISABLE));

	/* We dont track time spent here so worst case is 2 * timeout_ms */
	ret = intel_wait_for_register_fw(uncore, GEN12_RCU_ASYNC_FLUSH,
					 GEN12_RCU_ASYNC_FLUSH_IN_PROGRESS, 0,
					 timeout_ms);
	if (ret)
		goto out;

	if (id < 8)
		mask |= id << GEN12_RCU_ASYNC_FLUSH_ENGINE_ID_SHIFT;
	else
		mask |= (id - 8) << GEN12_RCU_ASYNC_FLUSH_ENGINE_ID_SHIFT |
			GEN12_RCU_ASYNC_FLUSH_ENGINE_ID_DECODE1;

	intel_uncore_write_fw(uncore, GEN12_RCU_ASYNC_FLUSH, mask);

	ret = intel_wait_for_register_fw(uncore, GEN12_RCU_ASYNC_FLUSH,
					 GEN12_RCU_ASYNC_FLUSH_IN_PROGRESS, 0,
					 timeout_ms);

out:
	if (!(psmi_ctrl & GEN6_PSMI_SLEEP_MSG_DISABLE))
		intel_uncore_write_fw(uncore, psmi_addr,
				      _MASKED_BIT_DISABLE(GEN6_PSMI_SLEEP_MSG_DISABLE));

	mutex_unlock(&i915->debuggers.eu_flush_lock);
	intel_uncore_forcewake_put(uncore, fw);
	intel_engine_pm_put(engine);

	return ret;
}

static void dg2_flush_engines(struct drm_i915_private *i915, u32 mask)
{
	const unsigned int timeout_ms = 500;
	struct intel_engine_cs *engine;

	for_each_uabi_engine(engine, i915) {
		if (!(engine->class == COMPUTE_CLASS ||
		      engine->class == RENDER_CLASS))
			continue;

		if (engine_rcu_async_flush(engine, mask, timeout_ms))
			drm_warn(&i915->drm,
				 "debugger: EU invalidation timeout for engine %s\n",
				 engine->name);
	}
}

static void gen12_flush_l3(struct drm_i915_private *i915)
{
	intel_wakeref_t wakeref;
	struct intel_gt *gt;
	int id, ret;

	for_each_gt(gt, i915, id) {
		with_intel_gt_pm_if_awake(gt, wakeref) {
			ret = intel_gt_invalidate_l3_mmio(gt);
			if (ret)
				drm_notice_once(&gt->i915->drm,
						"debugger: gt%d l3 invalidation fail: %s(%d). "
						"Surfaces need to be declared uncached to avoid coherency issues!\n",
						id, ret == -EACCES ? "incompatible bios" : "timeout",
						ret);
		}
	}
}

static void gpu_flush_engines(struct drm_i915_private *i915, u32 mask)
{
	const bool flush_in_debug_mode2 = IS_ALDERLAKE_P(i915) ||
		IS_ALDERLAKE_S(i915) ||
		IS_DG1(i915) ||
		IS_ROCKETLAKE(i915) ||
		IS_TIGERLAKE(i915);

	if (GRAPHICS_VER(i915) < 12) {
		drm_WARN_ON_ONCE(&i915->drm, GRAPHICS_VER(i915));
		return;
	}

	if (flush_in_debug_mode2)
		return gen12_invalidate_inst_cache(i915);

	dg2_flush_engines(i915, mask);
}

static void gpu_invalidate_l3(struct drm_i915_private *i915)
{
	gen12_flush_l3(i915);
}

static int access_page_in_obj(struct drm_i915_gem_object * const obj,
			      const unsigned long vma_offset,
			      void * const buf,
			      const size_t len,
			      const bool write)
{
	const pgoff_t pn = vma_offset >> PAGE_SHIFT;
	const size_t offset = offset_in_page(vma_offset);

	if (i915_gem_object_is_lmem(obj)) {
		void __iomem *vaddr;

		vaddr = i915_gem_object_lmem_io_map_page(obj, pn);
		mb();

		if (write)
			memcpy_toio(vaddr + offset, buf, len);
		else
			memcpy_fromio(buf, vaddr + offset, len);

		mb();
		io_mapping_unmap(vaddr);

		return 0;
	}

	if (i915_gem_object_has_struct_page(obj)) {
		struct page *page;
		void *vaddr;

		page = i915_gem_object_get_page(obj, pn);
		vaddr = kmap(page);

		drm_clflush_virt_range(vaddr + offset, len);

		if (write)
			memcpy(vaddr + offset, buf, len);
		else
			memcpy(buf, vaddr + offset, len);

		drm_clflush_virt_range(vaddr + offset, len);

		mark_page_accessed(page);
		if (write)
			set_page_dirty(page);

		kunmap(page);

		return 0;
	}

	if (obj->base.import_attach) {
		struct dma_buf * const b =
			obj->base.import_attach->dmabuf;
		struct iosys_map map;
		int ret;

		ret = dma_buf_vmap(b, &map);
		if (ret)
			return ret;

		/*
		 * There is no dma_buf_[begin|end]_cpu_access. The
		 * fence_wait inside of begin would deadlock if the
		 * signal is after the breakpointed kernel.
		 *
		 * For now, we just need to give up on coherency
		 * guarantees on remote dmabufs and leave it to the
		 * debugger to coordinate access wrt to active surfaces
		 * to avoid racing against the client.
		 */
		if (write)
			iosys_map_memcpy_to(&map, vma_offset, buf, len);
		else
			iosys_map_memcpy_from(buf, &map, vma_offset, len);

		dma_buf_vunmap(b, &map);

		return ret;
	}

	return -EINVAL;
}

static ssize_t access_page_in_vm(struct i915_address_space *vm,
				 const u64 vm_offset,
				 void *buf,
				 ssize_t len,
				 bool write)
{
	struct i915_vma *vma;
	struct i915_gem_ww_ctx ww;
	struct drm_i915_gem_object *obj;
	u64 vma_offset;
	ssize_t ret;

	if (len == 0)
		return 0;

	if (len < 0)
		return -EINVAL;

	if (GEM_WARN_ON(range_overflows_t(u64, vm_offset, len, vm->total)))
		return -EINVAL;

	ret = i915_gem_vm_bind_lock_interruptible(vm);
	if (ret)
		return ret;

	vma = i915_gem_vm_bind_lookup_vma(vm, vm_offset);
	if (!vma) {
		i915_gem_vm_bind_unlock(vm);
		return 0;
	}

	obj = vma->obj;

	for_i915_gem_ww(&ww, ret, true) {
		ret = i915_gem_object_lock(obj, &ww);
		if (ret)
			continue;

		ret = i915_gem_object_pin_pages_sync(obj);
		if (ret)
			continue;

		vma_offset = vm_offset - vma->start;

		len = min_t(ssize_t, len, PAGE_SIZE - offset_in_page(vma_offset));

		ret = access_page_in_obj(obj, vma_offset, buf, len, write);
		i915_gem_object_unpin_pages(obj);
	}

	i915_gem_vm_bind_unlock(vm);

	if (GEM_WARN_ON(ret > 0))
		return 0;

	return ret ?: len;
}

static ssize_t __vm_read_write(struct i915_address_space *vm,
			       char __user *r_buffer,
			       const char __user *w_buffer,
			       size_t count, loff_t *__pos, bool write)
{
	void *bounce_buf;
	ssize_t copied = 0;
	ssize_t bytes_left = count;
	loff_t pos = *__pos;
	ssize_t ret = 0;

	if (bytes_left <= 0)
		return 0;

	bounce_buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!bounce_buf)
		return -ENOMEM;

	do {
		ssize_t len = min_t(ssize_t, bytes_left, PAGE_SIZE);

		if (write) {
			ret = copy_from_user(bounce_buf, w_buffer + copied, len);
			if (ret < 0)
				break;

			len = len - ret;
			if (len > 0) {
				ret = access_page_in_vm(vm, pos + copied, bounce_buf, len, true);
				if (ret <= 0)
					break;

				len = ret;
			}
		} else {
			ret = access_page_in_vm(vm, pos + copied, bounce_buf, len, false);
			if (ret <= 0)
				break;

			len = ret;

			ret = copy_to_user(r_buffer + copied, bounce_buf, len);
			if (ret < 0)
				break;

			len = len - ret;
		}

		if (GEM_WARN_ON(len < 0))
			break;

		if (len == 0)
			break;

		bytes_left -= len;
		copied += len;
	} while(bytes_left >= 0);

	kfree(bounce_buf);

	/* pread/pwrite ignore this increment */
	if (copied > 0)
		*__pos += copied;

	return copied ?: ret;
}

#define debugger_vm_write(pd, b, c, p)	\
				__vm_read_write(pd, NULL, b, c, p, true)
#define debugger_vm_read(pd, b, c, p)	\
				__vm_read_write(pd, b, NULL, c, p, false)

static ssize_t i915_debugger_vm_write(struct file *file,
				      const char __user *buffer,
				      size_t count, loff_t *pos)
{
	struct i915_address_space *vm = file->private_data;
	ssize_t s;

	gpu_flush_engines(vm->i915, GEN12_RCU_ASYNC_FLUSH_AND_INVALIDATE_ALL);
	gpu_invalidate_l3(vm->i915);

	s = debugger_vm_write(vm, buffer, count, pos);

	gpu_invalidate_l3(vm->i915);
	gpu_flush_engines(vm->i915, GEN12_RCU_ASYNC_FLUSH_AND_INVALIDATE_ALL);

	return s;
}

static ssize_t i915_debugger_vm_read(struct file *file, char __user *buffer,
				     size_t count, loff_t *pos)
{
	struct i915_address_space *vm = file->private_data;
	ssize_t s;

	gpu_flush_engines(vm->i915, GEN12_RCU_ASYNC_FLUSH_AND_INVALIDATE_ALL);
	gpu_invalidate_l3(vm->i915);

	s = debugger_vm_read(file->private_data, buffer, count, pos);

	return s;
}

static vm_fault_t vm_mmap_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *area = vmf->vma;
	struct i915_address_space *vm = area->vm_private_data;
	struct i915_gem_ww_ctx ww;
	struct i915_vma *vma;
	unsigned long n;
	vm_fault_t ret;
	int err;

	err = i915_gem_vm_bind_lock_interruptible(vm);
	if (err)
		return i915_error_to_vmf_fault(err);

	vma = i915_gem_vm_bind_lookup_vma(vm, vmf->pgoff << PAGE_SHIFT);
	if (!vma) {
		i915_gem_vm_bind_unlock(vm);
		return VM_FAULT_SIGBUS;
	}

	n = vmf->pgoff - (vma->node.start >> PAGE_SHIFT);

	ret = VM_FAULT_SIGBUS;
	for_i915_gem_ww(&ww, err, true) {
		struct drm_i915_gem_object *obj = vma->obj;
		pgprot_t prot = pgprot_decrypted(area->vm_page_prot);
		unsigned long pfn;

		err = i915_gem_object_lock(obj, &ww);
		if (err)
			continue;

		if (!i915_gem_object_has_pages(obj)) {
			err = ____i915_gem_object_get_pages(obj);
			if (err)
				continue;
		}

		if (i915_gem_object_has_struct_page(obj)) {
			pfn = page_to_pfn(i915_gem_object_get_page(obj, n));
		} else if (i915_gem_object_is_lmem(obj)) {
			const dma_addr_t region_offset =
				(obj->mm.region->iomap.base -
				 obj->mm.region->region.start);
			const dma_addr_t page_start_addr =
				i915_gem_object_get_dma_address(obj, n);

			pfn = PHYS_PFN(page_start_addr + region_offset);
			prot = pgprot_writecombine(prot);
		} else {
			err = -EFAULT;
			continue;
		}

		ret = vmf_insert_pfn_prot(area, vmf->address, pfn, prot);
	}

	i915_gem_vm_bind_unlock(vm);

	if (err)
		ret = i915_error_to_vmf_fault(err);

	return ret;
}

static const struct vm_operations_struct vm_mmap_ops = {
	.fault = vm_mmap_fault,
};

static int i915_debugger_vm_mmap(struct file *file, struct vm_area_struct *area)
{
	struct i915_address_space *vm = file->private_data;
	pgoff_t len = (area->vm_end - area->vm_start) >> PAGE_SHIFT;
	pgoff_t sz = vm->total >> PAGE_SHIFT;

	if (area->vm_pgoff > sz - len)
	       return -EINVAL;

	area->vm_ops = &vm_mmap_ops;
	area->vm_private_data = vm;
	area->vm_flags |= VM_PFNMAP;

	gpu_invalidate_l3(vm->i915);
	gpu_flush_engines(vm->i915, GEN12_RCU_ASYNC_FLUSH_AND_INVALIDATE_ALL);

	return 0;
}

static int i915_debugger_vm_release(struct inode *inode, struct file *file)
{
	struct i915_address_space *vm = file->private_data;
	struct drm_device *dev = &vm->i915->drm;

	gpu_invalidate_l3(vm->i915);
	gpu_flush_engines(vm->i915, GEN12_RCU_ASYNC_FLUSH_AND_INVALIDATE_ALL);

	i915_vm_put(vm);
	drm_dev_put(dev);

	return 0;
}

static const struct file_operations vm_fops = {
	.owner   = THIS_MODULE,
	.llseek  = generic_file_llseek,
	.read    = i915_debugger_vm_read,
	.write   = i915_debugger_vm_write,
	.mmap    = i915_debugger_vm_mmap,
	.release = i915_debugger_vm_release,
};

static bool context_runalone_is_active(struct intel_engine_cs *engine)
{
	u32 val, engine_status, engine_shift;
	int id;

	val = intel_uncore_read(engine->gt->uncore, GEN12_RCU_DEBUG_1);

	if (engine->class == RENDER_CLASS)
		id = 0;
	else if (engine->class == COMPUTE_CLASS)
		id = engine->instance + 1;
	else
		GEM_BUG_ON(engine->class);

	if (GEM_WARN_ON(id > 4))
		return false;

	/* 3 status bits per engine, starting from bit 7 */
	engine_shift = 3 * id + 7;
	engine_status = val >> engine_shift & 0x7;

	/*
	 * On earlier gen12 the context status seems to be idle when
	 * it has raised attention. We have to omit the active bit.
	 */
	if (IS_DGFX(engine->i915))
		return (engine_status & GEN12_RCU_DEBUG_1_RUNALONE_ACTIVE) &&
			(engine_status & GEN12_RCU_DEBUG_1_CONTEXT_ACTIVE);

	return engine_status & GEN12_RCU_DEBUG_1_RUNALONE_ACTIVE;
}

static bool context_lrc_match(struct intel_engine_cs *engine,
			      struct intel_context *ce)
{
	u32 lrc_ggtt, lrc_reg, lrc_hw;

	lrc_ggtt = ce->lrc.lrca & GENMASK(31, 12);
	lrc_reg = ENGINE_READ(engine, RING_CURRENT_LRCA);
	lrc_hw = lrc_reg & GENMASK(31, 12);

	if (lrc_reg & CURRENT_LRCA_VALID)
		return lrc_ggtt == lrc_hw;

	return false;
}

static bool context_verify_active(struct intel_engine_cs *engine,
				  struct intel_context *ce)
{
	if (!ce)
		return false;

	/* We can't do better than this on older gens */
	if (GRAPHICS_VER(engine->i915) < 11)
		return true;

	if (!context_lrc_match(engine, ce))
		return false;

	if (GRAPHICS_VER(engine->i915) < 12)
		return true;

	if (!context_runalone_is_active(engine))
		return false;

	return true;
}

static struct intel_context *execlists_active_context_get(struct intel_engine_cs *engine)
{
	struct intel_context *ce = NULL;
	struct i915_request * const *port, *rq;

	rcu_read_lock();
	for (port = engine->execlists.active; (rq = *port); port++) {
		if (!__i915_request_is_complete(rq)) {
			ce = intel_context_get(rq->context);
			break;
		}
	}
	rcu_read_unlock();

	return ce;
}

static struct intel_context *engine_active_context_get(struct intel_engine_cs *engine)
{
	struct intel_context *ce, *active_ce = NULL;

	if (!intel_engine_pm_get_if_awake(engine))
		return NULL;

	i915_sched_engine_active_lock_bh(engine->sched_engine);
	spin_lock_irq(&engine->sched_engine->lock);

	if (intel_uc_uses_guc_submission(&engine->gt->uc))
		ce = intel_guc_active_context_get(engine);
	else
		ce = execlists_active_context_get(engine);

	if (ce && context_verify_active(engine, ce))
		active_ce = ce;

	spin_unlock_irq(&engine->sched_engine->lock);
	i915_sched_engine_active_unlock_bh(engine->sched_engine);

	intel_engine_pm_put(engine);

	if (active_ce)
		return active_ce;

	if (ce)
		intel_context_put(ce);

	return active_ce;
}

static bool client_has_vm(struct i915_drm_client *client,
			  struct i915_address_space *vm)
{
	struct i915_address_space *__vm;
	unsigned long idx;

	xa_for_each(&client->file->vm_xa, idx, __vm)
		if (__vm == vm)
			return true;

	return false;
}

static void *__i915_debugger_load_handle(struct i915_debugger *debugger,
					 u32 handle)
{
	return xa_load(&debugger->resources_xa, handle);
}

static struct i915_address_space *
__get_vm_from_handle(struct i915_debugger *debugger,
		     struct i915_debug_vm_open *vmo)
{
	struct i915_drm_client *client;
	struct i915_address_space *vm;

	if (upper_32_bits(vmo->handle))
		return ERR_PTR(-EINVAL);

	rcu_read_lock();

	vm = __i915_debugger_load_handle(debugger, lower_32_bits(vmo->handle));

	client = xa_load(&debugger->i915->clients.xarray, vmo->client_handle);
	if (client && client_has_vm(client, vm))
		vm = i915_vm_tryget(vm);
	else
		vm = NULL;

	rcu_read_unlock();

	return vm ?: ERR_PTR(-ENOENT);
}

static long
i915_debugger_vm_open_ioctl(struct i915_debugger *debugger, unsigned long arg)
{
	struct i915_debug_vm_open vmo;
	struct i915_address_space *vm;
	struct file *file;
	int ret;
	int fd;

	if (is_debugger_closed(debugger))
		return i915_debugger_disconnect_retcode(debugger);

	if (_IOC_SIZE(PRELIM_I915_DEBUG_IOCTL_VM_OPEN) != sizeof(vmo))
		return -EINVAL;

	if (!(_IOC_DIR(PRELIM_I915_DEBUG_IOCTL_VM_OPEN) & _IOC_WRITE))
		return -EINVAL;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		return fd;

	if (copy_from_user(&vmo, (void __user *) arg, sizeof(vmo))) {
		ret = -EFAULT;
		goto err_fd;
	}

	vm = __get_vm_from_handle(debugger, &vmo);
	if (IS_ERR(vm)) {
		ret = PTR_ERR(vm);
		goto err_fd;
	}

	file = anon_inode_getfile(DRIVER_NAME ".vm", &vm_fops,
				  vm, vmo.flags & O_ACCMODE);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto err_vm;
	}

	switch (vmo.flags & O_ACCMODE) {
	case O_RDONLY:
		file->f_mode |= FMODE_PREAD | FMODE_READ | FMODE_LSEEK;
		break;
	case O_WRONLY:
		file->f_mode |= FMODE_PWRITE | FMODE_WRITE| FMODE_LSEEK;
		break;
	case O_RDWR:
		file->f_mode |= FMODE_PREAD | FMODE_PWRITE |
				FMODE_READ | FMODE_WRITE | FMODE_LSEEK;
		break;
	}

	file->f_mapping = vm->inode->i_mapping;
	fd_install(fd, file);

	drm_dev_get(&vm->i915->drm);

	DD_VERBOSE(debugger, "vm_open: client_handle=%lld, handle=%lld, flags=0x%llx, fd=%d vm_address=%px",
		   vmo.client_handle, vmo.handle, vmo.flags, fd, vm);

	return fd;

err_vm:
	i915_vm_put(vm);
err_fd:
	put_unused_fd(fd);

	DD_WARN(debugger, "vm_open: client_handle=%lld, handle=%lld, flags=0x%llx, ret=%lld",
		vmo.client_handle, vmo.handle, vmo.flags, ret);

	return ret;
}

static int eu_control_interrupt_all(struct i915_debugger *debugger,
				    u64 client_handle,
				    struct intel_engine_cs *engine,
				    u8 *bits,
				    unsigned int bitmask_size)
{
	struct intel_gt *gt = engine->gt;
	struct intel_uncore *uncore = gt->uncore;
	struct i915_drm_client *client;
	struct intel_context *active_ctx;
	u32 context_lrca, lrca;
	u64 client_id;
	u32 td_ctl;

	/* Make sure we dont promise anything but interrupting all */
	if (bitmask_size)
		return -EINVAL;

	active_ctx = engine_active_context_get(engine);
	if (!active_ctx)
		return -ENOENT;

	if (!active_ctx->client) {
		intel_context_put(active_ctx);
		return -ENOENT;
	}

	client = i915_drm_client_get(active_ctx->client);
	client_id = client->id;
	i915_drm_client_put(client);
	context_lrca = active_ctx->lrc.lrca & GENMASK(31, 12);
	intel_context_put(active_ctx);

	if (client_id != client_handle)
		return -EBUSY;

	/* Additional check just before issuing MMIO writes */
	lrca = ENGINE_READ(engine, RING_CURRENT_LRCA);

	/* LRCA is not valid anymore */
	if (!(lrca & 0x1))
		return -ENOENT;

	lrca &= GENMASK(31, 12);

	if (context_lrca != lrca)
		return -EBUSY;

	td_ctl = intel_uncore_read(uncore, TD_CTL);

	/* Halt on next thread dispatch */
	if (!(td_ctl & TD_CTL_FORCE_EXTERNAL_HALT))
		intel_gt_mcr_multicast_write(gt, TD_CTL,
					     td_ctl | TD_CTL_FORCE_EXTERNAL_HALT);

	/*
	 * The sleep is needed because some interrupts are ignored
	 * by the HW, hence we allow the HW some time to acknowledge
	 * that.
	 */
	usleep_range(100, 200);

	/* Halt regardless of thread dependancies */
	if (!(td_ctl & TD_CTL_FORCE_EXCEPTION))
		intel_gt_mcr_multicast_write(gt, TD_CTL,
					     td_ctl | TD_CTL_FORCE_EXCEPTION);

	usleep_range(100, 200);

	intel_gt_mcr_multicast_write(gt, TD_CTL, td_ctl &
				     ~(TD_CTL_FORCE_EXTERNAL_HALT | TD_CTL_FORCE_EXCEPTION));

	/*
	 * In case of stopping wrong ctx emit warning.
	 * Nothing else we can do for now.
	 */
	lrca = ENGINE_READ(engine, RING_CURRENT_LRCA);
	if (!(lrca & 0x1) || context_lrca != (lrca & GENMASK(31, 12)))
		dev_warn(gt->i915->drm.dev,
			 "i915 debugger: interrupted wrong context.");

	intel_engine_schedule_heartbeat(engine);

	return 0;
}

struct ss_iter {
	struct i915_debugger *debugger;
	unsigned int i;

	unsigned int size;
	u8 *bits;
};

static int check_attn_ss_fw(struct intel_gt *gt, void *data,
			    unsigned int group, unsigned int instance,
			    bool present)
{
	struct ss_iter *iter = data;
	struct i915_debugger *debugger = iter->debugger;
	unsigned int row;

	for (row = 0; row < TD_EU_ATTENTION_MAX_ROWS; row++) {
		u32 val, cur = 0;

		if (iter->i >= iter->size)
			return 0;

		if (GEM_WARN_ON((iter->i + sizeof(val)) >
				(intel_gt_eu_attention_bitmap_size(gt))))
			return -EIO;

		memcpy(&val, &iter->bits[iter->i], sizeof(val));
		iter->i += sizeof(val);

		if (present)
			cur = intel_gt_mcr_read_fw(gt, TD_ATT(row), group, instance);

		if ((val | cur) != cur) {
			DD_INFO(debugger,
				"WRONG CLEAR (%d:%d:%d) TD_CRL: 0x%08x; TD_ATT: 0x%08x\n",
				group, instance, row, val, cur);
			return -EINVAL;
		}
	}

	return 0;
}

static int clear_attn_ss_fw(struct intel_gt *gt, void *data,
			    unsigned int group, unsigned int instance,
			    bool present)
{
	struct ss_iter *iter = data;
	struct i915_debugger *debugger = iter->debugger;
	unsigned int row;

	for (row = 0; row < TD_EU_ATTENTION_MAX_ROWS; row++) {
		u32 val;

		if (iter->i >= iter->size)
			return 0;

		if (GEM_WARN_ON((iter->i + sizeof(val)) >
				(intel_gt_eu_attention_bitmap_size(gt))))
			return -EIO;

		memcpy(&val, &iter->bits[iter->i], sizeof(val));
		iter->i += sizeof(val);

		if (!val)
			continue;

		if (present) {
			intel_gt_mcr_unicast_write_fw(gt, TD_CLR(row), val,
						      group, instance);

			DD_INFO(debugger,
				"TD_CLR: (%d:%d:%d): 0x%08x\n",
				group, instance, row, val);
		} else {
			DD_WARN(debugger,
				"TD_CLR: (%d:%d:%d): 0x%08x write to fused off subslice\n",
				group, instance, row, val);
		}
	}

	return 0;
}

static int eu_control_resume(struct i915_debugger *debugger,
			      struct intel_engine_cs *engine,
			      u8 *bits,
			      unsigned int bitmask_size)
{
	struct ss_iter iter = {
		.debugger = debugger,
		.i = 0,
		.size = bitmask_size,
		.bits = bits
	};
	int ret = 0;

	/*
	 * hsdes: 18021122357
	 * We need to avoid clearing attention bits that are not set
	 * in order to avoid the EOT hang on PVC.
	 */
	if (GRAPHICS_VER_FULL(engine->i915) == IP_VER(12, 60)) {
		ret = intel_gt_for_each_compute_slice_subslice(engine->gt,
							       check_attn_ss_fw,
							       &iter);
		if (ret)
			return ret;

		iter.i = 0;
	}


	intel_gt_for_each_compute_slice_subslice(engine->gt,
						 clear_attn_ss_fw, &iter);
	return 0;
}

static int do_eu_control(struct i915_debugger * debugger,
			 const struct prelim_drm_i915_debug_eu_control * const arg,
			 struct prelim_drm_i915_debug_eu_control __user * const user_ptr)
{
	void __user * const bitmask_ptr = u64_to_user_ptr(arg->bitmask_ptr);
	struct intel_engine_cs *engine;
	unsigned int hw_attn_size, attn_size;
	u8* bits = NULL;
	u64 seqno;
	int ret;

	if (is_debugger_closed(debugger))
		return i915_debugger_disconnect_retcode(debugger);

	/* Accept only hardware reg granularity mask */
	if (!IS_ALIGNED(arg->bitmask_size, sizeof(u32)))
		return -EINVAL;

	/* XXX Do we need to limit to these types? */
	if (arg->ci.engine_class != I915_ENGINE_CLASS_RENDER &&
	    arg->ci.engine_class != I915_ENGINE_CLASS_COMPUTE)
		return -EINVAL;

	engine = intel_engine_lookup_user(debugger->i915,
					  arg->ci.engine_class,
					  arg->ci.engine_instance);
	if (!engine)
		return -EINVAL;

	hw_attn_size = intel_gt_eu_attention_bitmap_size(engine->gt);
	attn_size = arg->bitmask_size;

	if (attn_size > hw_attn_size)
		attn_size = hw_attn_size;

	if (attn_size > 0) {
		bits = kmalloc(attn_size, GFP_KERNEL);
		if (!bits)
			return -ENOMEM;

		if (copy_from_user(bits, bitmask_ptr, attn_size)) {
			ret = -EFAULT;
			goto out_free;
		}

		if (debugger->debug_lvl > DD_DEBUG_LEVEL_INFO) {
			unsigned long i;

			for (i = 0; i < attn_size; i++) {
				if (!bits[i])
					continue;

				i915_debugger_print(debugger, DD_DEBUG_LEVEL_VERBOSE, "eu_control",
						    "from_user.bitmask[%u:%u] = 0x%x",
						    i, attn_size, bits[i]);
			}
		}
	}

	if (!intel_engine_pm_get_if_awake(engine)) {
		ret = -EIO;
		goto out_free;
	}

	ret = 0;
	mutex_lock(&debugger->lock);
	switch (arg->cmd) {
	case PRELIM_I915_DEBUG_EU_THREADS_CMD_INTERRUPT_ALL:
		ret = eu_control_interrupt_all(debugger, arg->client_handle,
					       engine, bits, attn_size);
		break;
	case PRELIM_I915_DEBUG_EU_THREADS_CMD_STOPPED:
		intel_gt_eu_attention_bitmap(engine->gt, bits, attn_size);
		break;
	case PRELIM_I915_DEBUG_EU_THREADS_CMD_RESUME:
		ret = eu_control_resume(debugger, engine, bits, attn_size);
		break;
	case PRELIM_I915_DEBUG_EU_THREADS_CMD_INTERRUPT:
		/* We cant interrupt individual threads */
		ret = -EINVAL;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (ret == 0)
		seqno = atomic_long_inc_return(&debugger->event_seqno);

	mutex_unlock(&debugger->lock);
	intel_engine_pm_put(engine);

	if (ret)
		goto out_free;

	if (put_user(seqno, &user_ptr->seqno)) {
		ret = -EFAULT;
		goto out_free;
	}

	if (copy_to_user(bitmask_ptr, bits, attn_size)) {
		ret = -EFAULT;
		goto out_free;
	}

	if (debugger->debug_lvl > DD_DEBUG_LEVEL_INFO) {
		unsigned long i;

		for (i = 0; i < attn_size; i++) {
			if (!bits[i])
				continue;

			i915_debugger_print(debugger, DD_DEBUG_LEVEL_VERBOSE, "eu_control",
					    "to_user.bitmask[%u:%u] = 0x%x",
					    i, attn_size, bits[i]);
		}
	}

	if (hw_attn_size != arg->bitmask_size)
		if (put_user(hw_attn_size, &user_ptr->bitmask_size))
			ret = -EFAULT;

out_free:
	kfree(bits);

	return ret;
}

static struct i915_drm_client *
find_client_get(struct i915_debugger *debugger, unsigned long handle)
{
	struct i915_drm_client *client;

	rcu_read_lock();
	client = xa_load(&debugger->i915->clients.xarray, handle);
	if (client) {
		if (client_session(client) == debugger->session)
			client = i915_drm_client_get_rcu(client);
		else
			client = NULL;
	}

	rcu_read_unlock();

	return client;
}

static long i915_debugger_eu_control(struct i915_debugger *debugger,
				     const unsigned int cmd,
				     const u64 arg)
{
	struct prelim_drm_i915_debug_eu_control __user * const user_ptr =
		u64_to_user_ptr(arg);
	struct prelim_drm_i915_debug_eu_control user_arg;
	struct i915_drm_client *client;
	int ret;

	if (_IOC_SIZE(cmd) < sizeof(user_arg))
		return -EINVAL;

	/* Userland write */
	if (!(_IOC_DIR(cmd) & _IOC_WRITE))
		return -EINVAL;

	/* Userland read */
	if (!(_IOC_DIR(cmd) & _IOC_READ))
		return -EINVAL;

	if (copy_from_user(&user_arg,
			   user_ptr,
			   sizeof(user_arg)))
		return -EFAULT;

	if (user_arg.flags)
		return -EINVAL;

	if (!access_ok(u64_to_user_ptr(user_arg.bitmask_ptr),
		       user_arg.bitmask_size))
		return -EFAULT;

	DD_INFO(debugger,
		"eu_control: client_handle=%lld, cmd=%u, flags=0x%x, ci.engine_class=%u, ci.engine_instance=%u, bitmask_size=%u\n",
		user_arg.client_handle, user_arg.cmd, user_arg.flags, user_arg.ci.engine_class,
		user_arg.ci.engine_instance, user_arg.bitmask_size);

	client = find_client_get(debugger, user_arg.client_handle);
	if (!client) {
		DD_INFO(debugger, "eu_control: no client found for %lld\n", user_arg.client_handle);
		return -EINVAL;
	}

	GEM_BUG_ON(client->id != user_arg.client_handle);

	ret = do_eu_control(debugger, &user_arg, user_ptr);

	DD_INFO(debugger,
		"eu_control: client_handle=%lld, cmd=%u, flags=0x%x, ci.engine_class=%u, ci.engine_instance=%u, bitmask_size=%u, ret=%lld\n",
		user_arg.client_handle, user_arg.cmd, user_arg.flags, user_arg.ci.engine_class, user_arg.ci.engine_instance,
		user_arg.bitmask_size, ret);

	i915_drm_client_put(client);

	return ret;
}

static long
i915_debugger_ack_event_ioctl(struct i915_debugger *debugger,
			      const unsigned int cmd,
			      const u64 arg)
{
	struct prelim_drm_i915_debug_event_ack __user * const user_ptr =
		u64_to_user_ptr(arg);
	struct prelim_drm_i915_debug_event_ack user_arg;
	struct i915_debug_ack *ack;

	if (_IOC_SIZE(cmd) < sizeof(user_arg))
		return -EINVAL;

	/* Userland write */
	if (!(_IOC_DIR(cmd) & _IOC_WRITE))
		return -EINVAL;

	if (copy_from_user(&user_arg,
			   user_ptr,
			   sizeof(user_arg)))
		return -EFAULT;

	if (user_arg.flags)
		return -EINVAL;

	mutex_lock(&debugger->lock);
	ack = remove_ack(debugger, user_arg.seqno);
	mutex_unlock(&debugger->lock);
	if (!ack)
		return -EINVAL;

	handle_ack(debugger, ack);
	kfree(ack);
	return 0;
}

static long i915_debugger_ioctl(struct file *file,
				unsigned int cmd,
				unsigned long arg)
{
	struct i915_debugger * const debugger = file->private_data;
	long ret;

	switch(cmd) {
	case PRELIM_I915_DEBUG_IOCTL_READ_EVENT:
		ret = i915_debugger_read_event(debugger, arg,
					       file->f_flags & O_NONBLOCK);
		DD_VERBOSE(debugger, "ioctl cmd=READ_EVENT ret=%lld\n", ret);
		break;
	case PRELIM_I915_DEBUG_IOCTL_READ_UUID:
		ret = i915_debugger_read_uuid_ioctl(debugger, cmd, arg);
		DD_VERBOSE(debugger, "ioctl cmd=READ_UUID ret = %lld\n", ret);
		break;
	case PRELIM_I915_DEBUG_IOCTL_VM_OPEN:
		ret = i915_debugger_vm_open_ioctl(debugger, arg);
		DD_VERBOSE(debugger, "ioctl cmd=VM_OPEN ret = %lld\n", ret);
		break;
	case PRELIM_I915_DEBUG_IOCTL_EU_CONTROL:
		ret = i915_debugger_eu_control(debugger, cmd, arg);
		DD_VERBOSE(debugger, "ioctl cmd=EU_CONTROL ret=%lld\n", ret);
		break;
	case PRELIM_I915_DEBUG_IOCTL_ACK_EVENT:
		ret = i915_debugger_ack_event_ioctl(debugger, cmd, arg);
		DD_VERBOSE(debugger, "ioctl cmd=ACK_EVENT ret=%lld\n", ret);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (ret < 0)
		DD_INFO(debugger, "ioctl cmd=0x%x arg=0x%llx ret=%lld\n", cmd, arg, ret);

	return ret;
}

static void
i915_debugger_discover_uuids(struct i915_drm_client *client)
{
	unsigned long idx;
	struct i915_uuid_resource *uuid;

	/*
	 * Lock not needed since i915_debugger_wait_in_discovery
	 * prevents from changing the set.
	 */
	xa_for_each(&client->uuids_xa, idx, uuid)
		i915_debugger_uuid_create(client, uuid);
}

static void
__i915_debugger_vm_send_event(struct i915_debugger *debugger,
			      const struct i915_drm_client *client,
			      u32 flags, u64 handle, gfp_t gfp)
{
	struct i915_debug_event_vm *vm_event;
	struct i915_debug_event *event;

	event = i915_debugger_create_event(debugger,
					   PRELIM_DRM_I915_DEBUG_EVENT_VM,
					   flags,
					   sizeof(*vm_event),
					   gfp);
	if (event) {
		vm_event = from_event(vm_event, event);
		vm_event->client_handle = client->id;
		vm_event->handle = handle;

		i915_debugger_queue_event(debugger, event);
	}
}

static int __i915_debugger_alloc_handle(struct i915_debugger *debugger,
					void *data, u32 *handle)
{
	int ret;

	ret = xa_alloc_cyclic(&debugger->resources_xa, handle, data,
			      xa_limit_32b, &debugger->next_handle,
			      GFP_KERNEL);
	if (ret == 1)
		ret = 0;

	if (ret) {
		DD_ERR(debugger, "xa_alloc_cyclic failed %d, disconnecting\n", ret);
		i915_debugger_disconnect_err(debugger);
	}

	return ret;
}

static int __i915_debugger_get_handle(struct i915_debugger *debugger,
				      const void *data, u32 *handle)
{
	unsigned long idx;
	int ret = -ENOENT;
	void *entry;

	xa_lock(&debugger->resources_xa);
	xa_for_each(&debugger->resources_xa, idx, entry) {
		if (entry == data) {
			if (handle)
				*handle = idx;
			ret = 0;
			break;
		}
	}
	xa_unlock(&debugger->resources_xa);
	return ret;
}

static inline bool
__i915_debugger_has_resource(struct i915_debugger *debugger, const void *data)
{
	return __i915_debugger_get_handle(debugger, data, NULL) == 0;
}

static int __i915_debugger_del_handle(struct i915_debugger *debugger, u32 id)
{
	return xa_erase(&debugger->resources_xa, id) ? 0 : -ENOENT;
}

static void __i915_debugger_vm_create(struct i915_debugger *debugger,
				      struct i915_drm_client *client,
				      struct i915_address_space *vm)
{
	u32 handle;

	if (__i915_debugger_alloc_handle(debugger, vm, &handle)) {
		DD_ERR(debugger,
		       "unable to allocate vm handle for client %u, disconnecting\n",
		       client->id);
		i915_debugger_disconnect_err(debugger);
		return;
	}

	__i915_debugger_vm_send_event(debugger, client,
				      PRELIM_DRM_I915_DEBUG_EVENT_CREATE,
				      handle,
				      GFP_KERNEL);
}

static void i915_debugger_discover_vma(struct i915_debugger *debugger,
				       struct i915_address_space *vm)
{
	unsigned long count;
	void *ev = NULL, *__ev;
	u32 vm_handle;
	size_t size;

	if (__i915_debugger_get_handle(debugger, vm, &vm_handle)) {
		DD_WARN(debugger, "discover_vm did not found handle for vm %p\n", vm);
		return;
	}

	size = 0;
	do {
		struct drm_mm_node *node;
		size_t used = 0;

		count = 0;
		__ev = ev;
		mutex_lock(&vm->mutex);
		drm_mm_for_each_node(node, &vm->mm) {
			struct i915_vma *vma = container_of(node, typeof(*vma), node);
			struct i915_debug_event_vm_bind *e = __ev;
			struct i915_vma_metadata *metadata;

			if (!i915_vma_is_persistent(vma))
				continue;

			used += sizeof(*e);
			list_for_each_entry(metadata, &vma->metadata_list, vma_link)
				used += sizeof(e->uuids[0]);

			if (used <= size) {
				e->base.type     = PRELIM_DRM_I915_DEBUG_EVENT_VM_BIND;
				e->base.flags    = PRELIM_DRM_I915_DEBUG_EVENT_CREATE;
				e->base.size     = sizeof(*e);
				e->client_handle = vm->client->id;
				e->vm_handle     = vm_handle;
				e->va_start      = i915_vma_offset(vma);
				e->va_length     = i915_vma_size(vma);
				e->num_uuids	 = 0;
				e->flags         = 0;

				list_for_each_entry(metadata,
						    &vma->metadata_list, vma_link) {
					e->uuids[e->num_uuids++] = metadata->uuid->handle;
					e->base.size += sizeof(e->uuids[0]);
				}

				__ev += e->base.size;
				count++;
			}
		}
		mutex_unlock(&vm->mutex);
		if (size >= used)
			break;

		__ev = krealloc(ev, used, GFP_KERNEL);
		if (!__ev) {
			DD_ERR(debugger, "could not allocate bind event, disconnecting\n");
			goto err;
		}
		ev = __ev;
		size = used;
	} while (1);

	for (__ev = ev; count--; ) {
		struct i915_debug_event_vm_bind *e = __ev;
		struct i915_debug_event *event;

		event = kmemdup(e, e->base.size, GFP_KERNEL);
		if (!event) {
			DD_ERR(debugger, "could not allocate bind event, disconnecting\n");
			goto err;
		}

		event->seqno = atomic_long_inc_return(&debugger->event_seqno);
		i915_debugger_queue_event(debugger, event);

		__ev += e->base.size;
	}
	goto out;

err:
	i915_debugger_disconnect_err(debugger);
out:
	kfree(ev);
}

static void i915_debugger_discover_vm(struct i915_debugger *debugger,
				      struct i915_drm_client *client)
{
	struct i915_address_space *vm;
	unsigned long i;

	if (!client->file) /* protect kernel internals */
		return;

	if (GEM_WARN_ON(client->debugger_session &&
			debugger->session != client->debugger_session))
		return;

	xa_for_each(&client->file->vm_xa, i, vm) {
		if (__i915_debugger_has_resource(debugger, vm))
			continue;

		__i915_debugger_vm_create(debugger, client, vm);
		i915_debugger_discover_vma(debugger, vm);
	}
}

static void i915_debugger_ctx_vm_def(struct i915_debugger *debugger,
				     const struct i915_drm_client *client,
				     u32 ctx_id,
				     const struct i915_address_space *vm)
{
	struct i915_debug_event *event;
	struct i915_debug_event_context_param *ep;
	u32 vm_handle;

	if (__i915_debugger_get_handle(debugger, vm, &vm_handle))
		return;

	event = i915_debugger_create_event(debugger,
					   PRELIM_DRM_I915_DEBUG_EVENT_CONTEXT_PARAM,
					   PRELIM_DRM_I915_DEBUG_EVENT_CREATE,
					   sizeof(*ep),
					   GFP_KERNEL);
	if (!event)
		return;

	ep = from_event(ep, event);
	ep->client_handle = client->id;
	ep->ctx_handle = ctx_id;
	ep->param.ctx_id = ctx_id;
	ep->param.param = I915_CONTEXT_PARAM_VM;
	ep->param.value = vm_handle;

	i915_debugger_queue_event(debugger, event);
}

static void i915_debugger_ctx_vm_create(struct i915_debugger *debugger,
					struct i915_gem_context *ctx)
{
	struct i915_address_space *vm = i915_gem_context_get_eb_vm(ctx);
	bool vm_found;

	vm_found = __i915_debugger_has_resource(debugger, vm);
	if (!vm_found)
		__i915_debugger_vm_create(debugger, ctx->client, vm);

	i915_debugger_ctx_vm_def(debugger, ctx->client, ctx->id, vm);

	if (!vm_found)
		i915_debugger_discover_vma(debugger, vm);

	i915_vm_put(vm);
}

static void
i915_debugger_discover_contexts(struct i915_debugger *debugger,
				struct i915_drm_client *client)
{
	struct i915_gem_context *ctx;

	rcu_read_lock();
	list_for_each_entry_rcu(ctx, &client->ctx_list, client_link) {
		if (!i915_gem_context_get_rcu(ctx))
			continue;

		if (!i915_gem_context_is_closed(ctx)) {
			rcu_read_unlock();

			i915_debugger_ctx_process_callback(ctx,
				intel_context_disable_preemption_timeout);

			i915_debugger_context_create(ctx);
			i915_debugger_ctx_vm_create(debugger, ctx);
			i915_debugger_context_param_engines(ctx);

			rcu_read_lock();
		}

		i915_gem_context_put(ctx);
	}
	rcu_read_unlock();
}

static bool
i915_debugger_client_task_register(const struct i915_debugger * const debugger,
				   struct i915_drm_client * const client,
				   struct task_struct * const task)
{
	bool registered = false;

	rcu_read_lock();
	if (!READ_ONCE(client->closed) &&
	    !is_debugger_closed(debugger) &&
	    same_thread_group(debugger->target_task, task)) {
		GEM_WARN_ON(client->debugger_session >= debugger->session);
		WRITE_ONCE(client->debugger_session, debugger->session);
		registered = true;
	}
	rcu_read_unlock();

	return registered;
}

static bool
i915_debugger_register_client(const struct i915_debugger * const debugger,
			      struct i915_drm_client * const client)
{
	const struct i915_drm_client_name *name;
	struct task_struct *client_task = NULL;
	bool registered;

	rcu_read_lock();
	name = __i915_drm_client_name(client);
	if (name) {
		client_task = get_pid_task(name->pid, PIDTYPE_PID);
	} else {
		/* XXX: clients->xarray can contain unregistered clients, should we wait or lock? */
		DD_WARN(debugger, "client %d with no pid, will not be found by discovery\n",
			 client->id);
	}
	rcu_read_unlock();

	if (!client_task)
		return false;

	registered = i915_debugger_client_task_register(debugger, client, client_task);
	DD_INFO(debugger, "client %d, pid %d, session %lld, %s registered\n",
		client->id, client_task->pid, client_session(client), registered ? "was" : "not");

	put_task_struct(client_task);

	return registered;
}

static void
i915_debugger_client_discovery(struct i915_debugger *debugger)
{
	struct i915_drm_client *client;
	unsigned long idx;

	rcu_read_lock();
	xa_for_each(&debugger->i915->clients.xarray, idx, client) {
		if (READ_ONCE(client->closed))
			continue;

		client = i915_drm_client_get_rcu(client);
		if (!client)
			continue;

		rcu_read_unlock();

		if (i915_debugger_register_client(debugger, client)) {
			DD_INFO(debugger, "client %u registered, discovery start", client->id);

			i915_debugger_client_create(client);
			i915_debugger_discover_uuids(client);
			i915_debugger_discover_contexts(debugger, client);
			i915_debugger_discover_vm(debugger, client);

			DD_INFO(debugger, "client %u discovery done", client->id);
		}

		i915_drm_client_put(client);

		rcu_read_lock();
	}

	rcu_read_unlock();
}

static void
compute_engines_reschedule_heartbeat(struct i915_debugger *debugger)
{
	struct drm_i915_private *i915 = debugger->i915;
	intel_wakeref_t wakeref;
	struct intel_gt *gt;
	int gt_id;

	for_each_gt(gt, i915, gt_id) {
		with_intel_gt_pm_if_awake(gt, wakeref) {
			struct intel_engine_cs *engine;
			int engine_id;

			for_each_engine(engine, gt, engine_id)
				if (intel_engine_has_eu_attention(engine))
					intel_engine_schedule_heartbeat(engine);
		}
	}
}

static int i915_debugger_discovery_worker(void *data)
{
	struct i915_debugger *debugger = data;

	if (kthread_should_stop())
		goto out;

	if (is_debugger_closed(debugger))
		goto out;

	i915_debugger_client_discovery(debugger);

out:
	complete_all(&debugger->discovery);
	i915_debugger_put(debugger);
	return 0;
}

static int i915_debugger_release(struct inode *inode, struct file *file)
{
	struct i915_debugger *debugger = file->private_data;

	i915_debugger_client_close(debugger);
	i915_debugger_put(debugger);
	return 0;
}

static const struct file_operations fops = {
	.owner		= THIS_MODULE,
	.llseek		= no_llseek,
	.release	= i915_debugger_release,
	.poll		= i915_debugger_poll,
	.read		= i915_debugger_read,
	.unlocked_ioctl	= i915_debugger_ioctl,
};

static struct task_struct *find_get_target(const pid_t nr)
{
	struct task_struct *task;

	rcu_read_lock();
	task = pid_task(find_pid_ns(nr, task_active_pid_ns(current)), PIDTYPE_PID);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();

	return task;
}

static int discovery_thread_stop(struct task_struct *task)
{
	int ret;

	ret = kthread_stop(task);

	GEM_WARN_ON(ret != -EINTR);
	return ret;
}

static int
i915_debugger_open(struct drm_i915_private * const i915,
		   struct prelim_drm_i915_debugger_open_param * const param)
{
	const u64 known_open_flags = PRELIM_DRM_I915_DEBUG_FLAG_FD_NONBLOCK;
	struct i915_debugger *debugger, *iter;
	struct task_struct *discovery_task;
	unsigned long f_flags = 0;
	unsigned long flags;
	int debug_fd;
	bool allowed;
	int ret;

	if (!param->pid)
		return -EINVAL;

	if (param->flags & ~known_open_flags)
		return -EINVAL;

	if (param->version && param->version != PRELIM_DRM_I915_DEBUG_VERSION)
		return -EINVAL;

	/* XXX: You get all for now */
	if (param->events)
		return -EINVAL;

	if (param->extensions)
		return -EINVAL;

	debugger = kzalloc(sizeof(*debugger), GFP_KERNEL);
	if (!debugger)
		return -ENOMEM;

	kref_init(&debugger->ref);
	mutex_init(&debugger->lock);
	INIT_LIST_HEAD(&debugger->connection_link);
	atomic_long_set(&debugger->event_seqno, 0);
	debugger->ack_tree = RB_ROOT;
	init_completion(&debugger->read_done);
	init_waitqueue_head(&debugger->write_done);
	init_completion(&debugger->discovery);
	xa_init_flags(&debugger->resources_xa, XA_FLAGS_ALLOC1);
	INIT_KFIFO(debugger->event_fifo);

	debugger->target_task = find_get_target(param->pid);
	if (!debugger->target_task) {
		ret = -ENOENT;
		goto err_free;
	}

	allowed = ptrace_may_access(debugger->target_task, PTRACE_MODE_READ_REALCREDS);
	if (!allowed) {
		ret = -EACCES;
		goto err_put_task;
	}

	kref_get(&debugger->ref); /* +1 for worker thread */
	discovery_task = kthread_create(i915_debugger_discovery_worker, debugger,
					"[i915_debugger_discover]");
	if (IS_ERR(discovery_task)) {
		ret = PTR_ERR(discovery_task);
		goto err_put_task;
	}

	if (param->flags & PRELIM_DRM_I915_DEBUG_FLAG_FD_NONBLOCK)
		f_flags |= O_NONBLOCK;

	spin_lock_irqsave(&i915->debuggers.lock, flags);

	for_each_debugger(iter, &i915->debuggers.list) {
		if (same_thread_group(iter->target_task, debugger->target_task)) {
			drm_info(&i915->drm, "pid %llu already debugged\n", param->pid);
			ret = -EBUSY;
			goto err_unlock;
		}
	}

	/* XXX handle the overflow without bailing out */
	if (i915->debuggers.session_count + 1 == 0) {
		drm_err(&i915->drm, "debugger connections exhausted. (you need module reload)\n");
		ret = -EBUSY;
		goto err_unlock;
	}

	if (i915->params.debugger_log_level < 0)
		debugger->debug_lvl = DD_DEBUG_LEVEL_WARN;
	else
		debugger->debug_lvl = min_t(int, i915->params.debugger_log_level,
					    DD_DEBUG_LEVEL_VERBOSE);

	debugger->i915 = i915;
	debugger->session = ++i915->debuggers.session_count;
	list_add_tail(&debugger->connection_link, &i915->debuggers.list);
	spin_unlock_irqrestore(&i915->debuggers.lock, flags);

	debug_fd = anon_inode_getfd("[i915_debugger]", &fops, debugger, f_flags);
	if (debug_fd < 0) {
		ret = debug_fd;
		spin_lock_irqsave(&i915->debuggers.lock, flags);
		list_del_init(&debugger->connection_link);
		goto err_unlock;
	}

	complete(&debugger->read_done);
	wake_up_process(discovery_task);

	compute_engines_reschedule_heartbeat(debugger);

	DD_INFO(debugger, "connected session %lld, debug level = %d",
		debugger->session, debugger->debug_lvl);

	if (debugger->debug_lvl >= DD_DEBUG_LEVEL_VERBOSE)
		printk(KERN_WARNING "i915_debugger: verbose debug level exposing raw addresses!\n");

	param->version = PRELIM_DRM_I915_DEBUG_VERSION;

	return debug_fd;

err_unlock:
	spin_unlock_irqrestore(&i915->debuggers.lock, flags);
	discovery_thread_stop(discovery_task);
err_put_task:
	put_task_struct(debugger->target_task);
err_free:
	xa_destroy(&debugger->resources_xa);
	kfree(debugger);

	return ret;
}

int i915_debugger_open_ioctl(struct drm_device *dev,
			     void *data,
			     struct drm_file *file)
{
	struct drm_i915_private *i915 = to_i915(dev);
	struct prelim_drm_i915_debugger_open_param * const param = data;
	int ret = 0;

	/*
	 * Use lock to avoid the debugger getting disabled via sysfs during
	 * session creation.
	 */
	mutex_lock(&i915->debuggers.enable_eu_debug_lock);
	if (!i915->debuggers.enable_eu_debug) {
		drm_err(&i915->drm,
			"i915_debugger: prelim_enable_eu_debug not set (is %d)\n",
			i915->debuggers.enable_eu_debug);
		mutex_unlock(&i915->debuggers.enable_eu_debug_lock);
		return -ENODEV;
	}

	ret = i915_debugger_open(i915, param);
	mutex_unlock(&i915->debuggers.enable_eu_debug_lock);
	return ret;
}

void i915_debugger_init(struct drm_i915_private *i915)
{
	mutex_init(&i915->debuggers.eu_flush_lock);

	spin_lock_init(&i915->debuggers.lock);
	INIT_LIST_HEAD(&i915->debuggers.list);
	mutex_init(&i915->debuggers.enable_eu_debug_lock);

	i915->debuggers.enable_eu_debug = !!i915->params.debug_eu;
	if (IS_SRIOV_VF(i915) && i915->params.debug_eu) {
		drm_notice(&i915->drm,
			   "i915_debugger: ignoring 'debug_eu=1' in VF mode\n");
		i915->debuggers.enable_eu_debug = false;
	}

	i915->debuggers.allow_eu_debug = !IS_SRIOV_VF(i915);
}

void i915_debugger_fini(struct drm_i915_private *i915)
{
	GEM_WARN_ON(!list_empty(&i915->debuggers.list));

	mutex_destroy(&i915->debuggers.eu_flush_lock);
}

void i915_debugger_wait_on_discovery(struct drm_i915_private *i915,
				     struct i915_drm_client *client)
{
	const unsigned long waitjiffs = msecs_to_jiffies(5000);
	struct i915_debugger *debugger;
	long timeleft;
	u64 session;

	session = client_session(client);
	if (!session)
		return;

	debugger = i915_debugger_find_task_get(i915, current);
	if (!debugger)
		return;

	GEM_WARN_ON(!same_thread_group(debugger->target_task, current));
	GEM_WARN_ON(debugger->session != session);

	timeleft = wait_for_completion_interruptible_timeout(&debugger->discovery,
							     waitjiffs);
	if (timeleft == -ERESTARTSYS) {
		DD_WARN(debugger,
			"task %d interrupted while waited during debugger discovery process\n",
			task_pid_nr(current));
	} else if (!timeleft) {
		DD_WARN(debugger,
			"task %d waited too long for discovery to complete. Ignoring barrier.\n",
			task_pid_nr(current));
	}

	i915_debugger_put(debugger);
}

void i915_debugger_client_register(struct i915_drm_client *client,
				   struct task_struct *task)
{
	struct drm_i915_private * const i915 = client->clients->i915;
	struct i915_debugger *iter;
	unsigned long flags;

	/*
	 * Session count only grows and we cannot connect to
	 * the same pid twice.
	 */
	spin_lock_irqsave(&i915->debuggers.lock, flags);
	for_each_debugger(iter, &i915->debuggers.list) {
		if (!same_thread_group(iter->target_task, task))
			continue;

		WRITE_ONCE(client->debugger_session, iter->session);
		break;
	}
	spin_unlock_irqrestore(&i915->debuggers.lock, flags);
}

void i915_debugger_client_release(struct i915_drm_client *client)
{
	WRITE_ONCE(client->debugger_session, 0);
}

static int send_engine_attentions(struct i915_debugger *debugger,
				  struct intel_engine_cs *engine,
				  struct i915_drm_client *client,
				  struct intel_context *ce)
{
	struct i915_debug_event_eu_attention *ea;
	struct i915_debug_event *event;
	unsigned int size;

	if (is_debugger_closed(debugger))
		return -ENODEV;

	/* XXX test for CONTEXT_DEBUG when igt/umd is there */

	size = struct_size(ea, bitmask, intel_gt_eu_attention_bitmap_size(engine->gt));
	event = __i915_debugger_create_event(debugger,
					     PRELIM_DRM_I915_DEBUG_EVENT_EU_ATTENTION,
					     PRELIM_DRM_I915_DEBUG_EVENT_STATE_CHANGE,
					     size,
					     GFP_KERNEL);
	if (!event)
		return -ENOMEM;

	ea = from_event(ea, event);
	ea->client_handle = client->id;

	ea->ci.engine_class = engine->uabi_class;
	ea->ci.engine_instance = engine->uabi_instance;
	ea->bitmask_size = intel_gt_eu_attention_bitmap_size(engine->gt);
	ea->ctx_handle = ce->dbg_id.gem_context_id;
	ea->lrc_handle = ce->dbg_id.lrc_id;

	mutex_lock(&debugger->lock);
	intel_gt_eu_attention_bitmap(engine->gt, &ea->bitmask[0], ea->bitmask_size);
	event->seqno = atomic_long_inc_return(&debugger->event_seqno);
	mutex_unlock(&debugger->lock);

	return i915_debugger_queue_event(debugger, event);
}

static int i915_debugger_send_engine_attention(struct intel_engine_cs *engine)
{
	struct i915_debugger *debugger;
	struct i915_drm_client *client;
	struct intel_context *ce;
	int ret;

	/* Anybody listening out for an event? */
	if (list_empty_careful(&engine->i915->debuggers.list))
		return -ENOTCONN;

	/* Find the client seeking attention */
	ce = engine_active_context_get(engine);
	if (!ce)
		return -ENOENT;

	if (!ce->client) {
		intel_context_put(ce);
		return -ENOENT;
	}

	client = i915_drm_client_get(ce->client);
	/*
	 * There has been attention, thus the engine on which the
	 * request resides can't proceed with said context as the
	 * shader is 'stuck'.
	 *
	 * Second, the lrca matches what the hardware has lastly
	 * executed (CURRENT_LRCA) and the RunAlone is set guaranteeing
	 * that the EU's did belong only to the current context.
	 *
	 * So the context that did raise the attention, has to
	 * be the correct one.
	 */
	debugger = i915_debugger_get(client);
	if (!debugger) {
		ret = -ENOTCONN;
	} else if (!completion_done(&debugger->discovery)) {
		DD_INFO(debugger, "%s: discovery not yet done\n", engine->name);
		ret = -EBUSY;
	} else {
		ret = send_engine_attentions(debugger, engine, client, ce);
	}

	if (debugger) {
		DD_INFO(debugger, "%s: i915_send_engine_attention: %d\n", engine->name, ret);
		i915_debugger_put(debugger);
	}

	i915_drm_client_put(client);
	intel_context_put(ce);

	return ret;
}

static void
i915_debugger_send_client_event_ctor(const struct i915_drm_client *client,
				     u32 type, u32 flags, u64 size,
				     void (*constructor)(struct i915_debug_event *,
							 const void *),
				     const void *data,
				     gfp_t gfp)
{
	struct i915_debugger *debugger;
	struct i915_debug_event *event;

	debugger = i915_debugger_get(client);
	if (!debugger)
		return;

	event = i915_debugger_create_event(debugger, type, flags, size, gfp);
	if (event) {
		constructor(event, data);

		i915_debugger_queue_event(debugger, event);
	}

	i915_debugger_put(debugger);
}

#define write_member(T_out, ptr, member, value) { \
	BUILD_BUG_ON(sizeof(*ptr) != sizeof(T_out)); \
	BUILD_BUG_ON(offsetof(typeof(*ptr), member) != \
		     offsetof(typeof(T_out), member)); \
	BUILD_BUG_ON(sizeof(ptr->member) != sizeof(value)); \
	BUILD_BUG_ON(sizeof(struct_member(T_out, member)) != sizeof(value)); \
	BUILD_BUG_ON(!typecheck(typeof((ptr)->member), value));	\
	memcpy(&ptr->member, &value, sizeof(ptr->member)); \
}

struct client_event_param {
	u64 handle;
};

static void client_event_ctor(struct i915_debug_event *event, const void *data)
{
	const struct client_event_param *p = data;
	struct i915_debug_event_client *ec = from_event(ec, event);

	write_member(struct prelim_drm_i915_debug_event_client, ec, handle, p->handle);
}

static void send_client_event(const struct i915_drm_client *client, u32 flags)
{
	const struct client_event_param p = {
		.handle = client->id,
	};

	i915_debugger_send_client_event_ctor(client,
					     PRELIM_DRM_I915_DEBUG_EVENT_CLIENT,
					     flags,
					     sizeof(struct prelim_drm_i915_debug_event_client),
					     client_event_ctor, &p,
					     GFP_KERNEL);
}

void i915_debugger_client_create(const struct i915_drm_client *client)
{
	if (!client_debugged(client))
		return;

	send_client_event(client, PRELIM_DRM_I915_DEBUG_EVENT_CREATE);
}

void i915_debugger_client_destroy(struct i915_drm_client *client)
{
	struct i915_uuid_resource *uuid_res;
	unsigned long idx;

	if (!client_debugged(client))
		return;

	xa_for_each(&client->uuids_xa, idx, uuid_res)
		i915_debugger_uuid_destroy(client, uuid_res);

	send_client_event(client, PRELIM_DRM_I915_DEBUG_EVENT_DESTROY);

	i915_debugger_client_release(client);
}

struct ctx_event_param {
	u64 client_handle;
	u64 handle;
};

static void ctx_event_ctor(struct i915_debug_event *event, const void *data)
{
	const struct ctx_event_param *p = data;
	struct i915_debug_event_context *ec = from_event(ec, event);

	write_member(struct prelim_drm_i915_debug_event_context, ec, client_handle, p->client_handle);
	write_member(struct prelim_drm_i915_debug_event_context, ec, handle, p->handle);
}

static void send_context_event(const struct i915_gem_context *ctx, u32 flags)
{
	const struct ctx_event_param p = {
		.client_handle = ctx->client->id,
		.handle = ctx->id
	};

	i915_debugger_send_client_event_ctor(ctx->client,
					     PRELIM_DRM_I915_DEBUG_EVENT_CONTEXT,
					     flags,
					     sizeof(struct prelim_drm_i915_debug_event_context),
					     ctx_event_ctor, &p,
					     GFP_KERNEL);
}

void i915_debugger_context_create(const struct i915_gem_context *ctx)
{
	if (!client_debugged(ctx->client))
		return;

	send_context_event(ctx, PRELIM_DRM_I915_DEBUG_EVENT_CREATE);
}

void i915_debugger_context_destroy(const struct i915_gem_context *ctx)
{
	if (!client_debugged(ctx->client))
		return;

	send_context_event(ctx, PRELIM_DRM_I915_DEBUG_EVENT_DESTROY);
}

struct uuid_event_param {
	u64 client_handle;
	u64 handle;
	u64 class_handle;
	u64 payload_size;
};

static void uuid_event_ctor(struct i915_debug_event *event, const void *data)
{
	const struct uuid_event_param *p = data;
	struct i915_debug_event_uuid *ec = from_event(ec, event);

	write_member(struct prelim_drm_i915_debug_event_uuid, ec, client_handle, p->client_handle);
	write_member(struct prelim_drm_i915_debug_event_uuid, ec, handle, p->handle);
	write_member(struct prelim_drm_i915_debug_event_uuid, ec, class_handle, p->class_handle);
	write_member(struct prelim_drm_i915_debug_event_uuid, ec, payload_size, p->payload_size);
}

static void send_uuid_event(const struct i915_drm_client *client,
			    const struct i915_uuid_resource *uuid,
			    u32 flags)
{
	struct uuid_event_param p = {
		.client_handle = client->id,
		.handle = uuid->handle,
		.class_handle = uuid->uuid_class,
		.payload_size = 0,
	};

	if (flags & PRELIM_DRM_I915_DEBUG_EVENT_CREATE)
		p.payload_size = uuid->size;

	i915_debugger_send_client_event_ctor(client,
					     PRELIM_DRM_I915_DEBUG_EVENT_UUID,
					     flags,
					     sizeof(struct prelim_drm_i915_debug_event_uuid),
					     uuid_event_ctor, &p,
					     GFP_KERNEL);
}

void i915_debugger_uuid_create(const struct i915_drm_client *client,
			       const struct i915_uuid_resource *uuid)
{
	if (!client_debugged(client))
		return;

	send_uuid_event(client, uuid, PRELIM_DRM_I915_DEBUG_EVENT_CREATE);
}

void i915_debugger_uuid_destroy(const struct i915_drm_client *client,
				const struct i915_uuid_resource *uuid)
{
	if (!client_debugged(client))
		return;

	send_uuid_event(client, uuid, PRELIM_DRM_I915_DEBUG_EVENT_DESTROY);
}

static void __i915_debugger_vma_send_event(struct i915_debugger *debugger,
					   struct i915_drm_client *client,
					   struct i915_vma *vma,
					   u32 flags,
					   gfp_t gfp)
{
	struct i915_vma_metadata *metadata;
	struct i915_debug_event_vm_bind *ev;
	struct i915_debug_event *event;
	u32 vm_handle;
	u64 size;

	if (GEM_WARN_ON(!vma))
		return;

	if (__i915_debugger_get_handle(debugger, vma->vm, &vm_handle))
		return;

	size = sizeof(*ev);
	list_for_each_entry(metadata, &vma->metadata_list, vma_link)
		size += sizeof(ev->uuids[0]);

	if (flags & PRELIM_DRM_I915_DEBUG_EVENT_CREATE)
		flags |= PRELIM_DRM_I915_DEBUG_EVENT_NEED_ACK;

	event = i915_debugger_create_event(debugger,
					   PRELIM_DRM_I915_DEBUG_EVENT_VM_BIND,
					   flags,
					   size,
					   gfp);
	if (!event) {
		DD_ERR(debugger, "debugger: vma: alloc fail, bailing out\n");
		return;
	}

	ev = from_event(ev, event);

	ev->client_handle = client->id;
	ev->vm_handle     = vm_handle;
	ev->va_start      = i915_vma_offset(vma);
	ev->va_length     = i915_vma_size(vma);
	ev->flags         = 0;
	ev->num_uuids     = 0;

	list_for_each_entry(metadata, &vma->metadata_list, vma_link)
		ev->uuids[ev->num_uuids++] = metadata->uuid->handle;

	_i915_debugger_queue_event(debugger, event, vma, gfp);
}

void i915_debugger_vma_insert(struct i915_drm_client *client,
			      struct i915_vma *vma)
{
	struct i915_debugger *debugger;

	debugger = i915_debugger_get(client);
	if (!debugger)
		return;

	if (i915_vma_is_persistent(vma))
		__i915_debugger_vma_send_event(debugger, client, vma,
					       PRELIM_DRM_I915_DEBUG_EVENT_CREATE,
					       GFP_ATOMIC);

	i915_debugger_put(debugger);
}

void i915_debugger_vma_evict(struct i915_drm_client *client,
			     struct i915_vma *vma)
{
	struct i915_debugger *debugger;

	debugger = i915_debugger_get(client);
	if (!debugger)
		return;

	unmap_mapping_range(vma->vm->inode->i_mapping,
			    vma->node.start, vma->node.size,
			    1);

	if (i915_vma_is_persistent(vma))
		__i915_debugger_vma_send_event(debugger, client, vma,
					       PRELIM_DRM_I915_DEBUG_EVENT_DESTROY,
					       GFP_ATOMIC);

	i915_debugger_put(debugger);
}

void i915_debugger_vm_create(struct i915_drm_client *client,
			     struct i915_address_space *vm)
{
	struct i915_debugger *debugger;

	if (!client)
		return;

	if (GEM_WARN_ON(!vm))
		return;

	debugger = i915_debugger_get(client);
	if (!debugger)
		return;

	if (!__i915_debugger_has_resource(debugger, vm))
		__i915_debugger_vm_create(debugger, client, vm);

	i915_debugger_put(debugger);
}

void i915_debugger_vm_destroy(struct i915_drm_client *client,
			      struct i915_address_space *vm)
{
	struct i915_debugger *debugger;
	u32 handle;

	if (!client)
		return;

	if (GEM_WARN_ON(!vm))
		return;

	debugger = i915_debugger_get(client);
	if (!debugger)
		return;

	if (atomic_read(&vm->open) > 1)
		goto out;

	if (__i915_debugger_get_handle(debugger, vm, &handle))
		goto out;

	__i915_debugger_del_handle(debugger, handle);
	__i915_debugger_vm_send_event(debugger, client,
				      PRELIM_DRM_I915_DEBUG_EVENT_DESTROY,
				      handle,
				      GFP_KERNEL);

out:
	i915_debugger_put(debugger);
}

void i915_debugger_context_param_vm(const struct i915_drm_client *client,
				    struct i915_gem_context *ctx,
				    struct i915_address_space *vm)
{
	struct i915_debugger *debugger;

	if (!client)
		return;

	if (!ctx) {
		GEM_WARN_ON(!ctx);
		return;
	}

	if (!vm) {
		GEM_WARN_ON(!vm);
		return;
	}

	debugger = i915_debugger_get(client);
	if (!debugger)
		return;

	i915_debugger_ctx_vm_def(debugger, client, ctx->id, vm);
	i915_debugger_put(debugger);
}

void i915_debugger_context_param_engines(struct i915_gem_context *ctx)
{
	struct i915_debugger *debugger;
	struct i915_context_param_engines *e;
	struct i915_gem_engines *gem_engines;
	struct i915_debug_event_engines *event_engine;
	struct i915_debug_event_context_param *event_param;
	struct i915_debug_event *event;
	struct i915_drm_client *client = ctx->client;
	size_t event_size, count, n;

	/* Can land here during the i915_gem_context_create_ioctl twice:
	 * during the extension phase and later on in gem_context_register.
	 * In gem_context_register ctx->client will be set and previous
	 * events were sent (context create, vm create, ...).
	 */
	if (!client)
		return;

	debugger = i915_debugger_get(client);
	if (!debugger)
		return;

	gem_engines = i915_gem_context_engines_get(ctx, NULL);
	if (!gem_engines) {
		i915_debugger_put(debugger);
		return;
	}

	count = gem_engines->num_engines;

	if (!check_struct_size(e, engines, count, &event_size)) {
		i915_gem_context_engines_put(gem_engines);
		i915_debugger_put(debugger);
		return;
	}

	/* param.value is like data[] thus don't count it */
	event_size += (sizeof(*event_param) - sizeof(event_param->param.value));

	event = i915_debugger_create_event(debugger,
					   PRELIM_DRM_I915_DEBUG_EVENT_CONTEXT_PARAM,
					   PRELIM_DRM_I915_DEBUG_EVENT_CREATE,
					   event_size,
					   GFP_KERNEL);
	if (!event) {
		i915_gem_context_engines_put(gem_engines);
		i915_debugger_put(debugger);
		return;
	}

	event_param = from_event(event_param, event);
	event_param->client_handle = client->id;
	event_param->ctx_handle = ctx->id;

	event_param->param.ctx_id = ctx->id;
	event_param->param.param = I915_CONTEXT_PARAM_ENGINES;
	event_param->param.size = struct_size(e, engines, count);

	if (count) {
		event_size = sizeof(*event_engine) +
			     count * sizeof(struct i915_debug_engine_info);

		event = i915_debugger_create_event(debugger,
					PRELIM_DRM_I915_DEBUG_EVENT_ENGINES,
					PRELIM_DRM_I915_DEBUG_EVENT_CREATE,
					event_size,
					GFP_KERNEL);
		if (!event) {
			i915_gem_context_engines_put(gem_engines);
			i915_debugger_put(debugger);
			kfree(event_param);

			return;
		}

		event_engine = from_event(event_engine, event);
		event_engine->client_handle = client->id;
		event_engine->ctx_handle    = ctx->id;
		event_engine->num_engines   = count;
	} else {
		event_engine = NULL;
	}

	e = (struct i915_context_param_engines *)&event_param->param.value;

	for (n = 0; n < count; n++) {
		struct i915_engine_class_instance *ci = &e->engines[n];
		struct i915_debug_engine_info *engines =
			&event_engine->engines[n];

		if (gem_engines->engines[n]) {
			ci->engine_class =
				gem_engines->engines[n]->engine->uabi_class;
			ci->engine_instance =
				gem_engines->engines[n]->engine->uabi_instance;

			engines->engine.engine_class = ci->engine_class;
			engines->engine.engine_instance = ci->engine_instance;
			engines->lrc_handle = gem_engines->engines[n]->dbg_id.lrc_id;
		} else {
			ci->engine_class = I915_ENGINE_CLASS_INVALID;
			ci->engine_instance = I915_ENGINE_CLASS_INVALID_NONE;
		}
	}
	i915_gem_context_engines_put(gem_engines);

	i915_debugger_queue_event(debugger, to_event(event_param));

	if (event_engine)
		i915_debugger_queue_event(debugger, to_event(event_engine));

	i915_debugger_put(debugger);
}

/**
 * i915_debugger_handle_engine_attention() - handle attentions if any
 * @engine: engine
 *
 * Check if there are eu thread attentions in engine and if so
 * pass a message to debugger to handle them.
 *
 * Returns: number of attentions present or negative on error
 */
int i915_debugger_handle_engine_attention(struct intel_engine_cs *engine)
{
	int ret, attentions;

	if (!intel_engine_has_eu_attention(engine))
		return 0;

	ret = intel_gt_eu_threads_needing_attention(engine->gt);
	if (ret <= 0)
		return ret;

	attentions = ret;

	atomic_inc(&engine->gt->reset.eu_attention_count);

	/* We dont care if it fails reach this debugger at this time */
	ret = i915_debugger_send_engine_attention(engine);
	if (ret == -EBUSY)
		return attentions; /* Discovery in progress, fake it */

	return ret ?: attentions;
}

static bool i915_debugger_active_on_client(struct i915_drm_client *client)
{
	struct i915_debugger *debugger = i915_debugger_get(client);

	if (debugger)
		i915_debugger_put(debugger);

	return !!debugger; /* implict casting */
}

bool i915_debugger_prevents_hangcheck(struct intel_engine_cs *engine)
{
	if (!intel_engine_has_eu_attention(engine))
		return false;

	return !list_empty(&engine->i915->debuggers.list);
}

bool i915_debugger_active_on_context(struct intel_context *context)
{
	struct i915_drm_client *client;
	bool active;

	rcu_read_lock();
	client = i915_drm_client_get_rcu(context->client);
	rcu_read_unlock();
	if (!client)
		return false;

	active = i915_debugger_active_on_client(client);
	i915_drm_client_put(client);

	return active;
}

bool i915_debugger_context_guc_debugged(struct intel_context *context)
{
	if (!intel_engine_uses_guc(context->engine))
		return false;

	if (!i915_debugger_active_on_context(context))
		return false;

	return true;
}

#define i915_DEBUGGER_ATTENTION_INTERVAL 100
long i915_debugger_attention_poll_interval(struct intel_engine_cs *engine)
{
	long delay = 0;

	GEM_BUG_ON(!engine);

	if (intel_engine_has_eu_attention(engine) &&
	    !list_empty(&engine->i915->debuggers.list))
		delay = i915_DEBUGGER_ATTENTION_INTERVAL;

	return delay;
}

int i915_debugger_enable(struct drm_i915_private *i915, bool enable)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	struct intel_gt *gt;
	unsigned int i;

	mutex_lock(&i915->debuggers.enable_eu_debug_lock);
	if (!i915->debuggers.allow_eu_debug) {
		mutex_unlock(&i915->debuggers.enable_eu_debug_lock);
		return -EPERM;
	}

	if (!enable && !list_empty(&i915->debuggers.list)) {
		mutex_unlock(&i915->debuggers.enable_eu_debug_lock);
		return -EBUSY;
	}

	if (enable == i915->debuggers.enable_eu_debug) {
		mutex_unlock(&i915->debuggers.enable_eu_debug_lock);
		return 0;
	}

	for_each_gt(gt, i915, i) {
		/* XXX suspend current activity */
		for_each_engine(engine, gt, id) {
			if (enable)
				intel_engine_debug_enable(engine);
			else
				intel_engine_debug_disable(engine);
		}
		intel_gt_handle_error(gt, ALL_ENGINES, 0, NULL);
	}

	i915->debuggers.enable_eu_debug = enable;
	mutex_unlock(&i915->debuggers.enable_eu_debug_lock);

	return 0;
}

static int __i915_debugger_allow(struct drm_i915_private *i915, bool allow)
{
	if (IS_SRIOV_VF(i915) && allow)
		return -EPERM;

	mutex_lock(&i915->debuggers.enable_eu_debug_lock);
	if (!allow && i915->debuggers.enable_eu_debug) {
		mutex_unlock(&i915->debuggers.enable_eu_debug_lock);
		return -EBUSY;
	}

	i915->debuggers.allow_eu_debug = allow;
	mutex_unlock(&i915->debuggers.enable_eu_debug_lock);

	return 0;
}

int i915_debugger_allow(struct drm_i915_private *i915)
{
	return __i915_debugger_allow(i915, true);
}

int i915_debugger_disallow(struct drm_i915_private *i915)
{
	return __i915_debugger_allow(i915, false);
}

#if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST)
#include "selftests/i915_debugger.c"
#endif
