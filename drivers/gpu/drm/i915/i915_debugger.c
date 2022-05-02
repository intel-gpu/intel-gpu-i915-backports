/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */
#include <drm/drm_cache.h>

#include <linux/anon_inodes.h>
#include <linux/minmax.h>
#include <linux/mman.h>
#include <linux/ptrace.h>
#include <linux/delay.h>

#include "i915_drv.h"
#include "i915_debugger.h"
#include "i915_gpu_error.h"
#include "i915_sw_fence.h"
#include "i915_drm_client.h"
#include "gem/i915_gem_context.h"
#include "gem/i915_gem_mman.h"
#include "gem/i915_gem_vm_bind.h"
#include "gt/intel_context_types.h"
#include "gt/intel_engine_pm.h"
#include "gt/intel_engine.h"
#include "gt/intel_engine_user.h"
#include "gt/intel_engine_heartbeat.h"

#include "gt/intel_gt.h"
#include "gt/intel_gt_pm.h"
#include "gt/intel_engine_pm.h"
#include "gt/intel_engine_regs.h"
#include "gt/intel_gt_regs.h"
#include "gt/intel_gt_debug.h"
#include "gt/uc/intel_guc_submission.h"

#define from_event(T, event) container_of((event), typeof(*(T)), base)
#define to_event(e) (&(e)->base)

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

	drm_printf(&p, "%s(%lld:%d): %pV", prefix, debugger->session, current->pid, &vaf);

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

static void event_printer_vm_bind(const struct i915_debugger * const debugger,
				  const char * const prefix,
				  const struct i915_debug_event * const event)
{
	const struct i915_debug_event_vm_bind * const vm_bind =
		from_event(vm_bind, event);
	unsigned i;

	EVENT_PRINT_MEMBER_HANDLE(debugger, prefix, vm_bind, client_handle);
	EVENT_PRINT_MEMBER_HANDLE(debugger, prefix, vm_bind, vm_handle);
	EVENT_PRINT_MEMBER_U64X(debugger, prefix, vm_bind, va_start);
	EVENT_PRINT_MEMBER_U64X(debugger, prefix, vm_bind, va_length);
	EVENT_PRINT_MEMBER_U32(debugger, prefix, vm_bind, num_uuids);
	EVENT_PRINT_MEMBER_U32(debugger, prefix, vm_bind, flags);

	for (i = 0; i < vm_bind->num_uuids; i++)
		i915_debugger_print(debugger, DD_DEBUG_LEVEL_INFO, prefix,
				    "  vm_bind->uuids[%u] = %lld",
				    i, vm_bind->uuids[i]);
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
		event_printer_vm_bind,
		event_printer_context_param,
		event_printer_eu_attention,
		event_printer_engines,
	};
	debug_event_printer_t event_printer = NULL;

	if (likely(debugger->debug_lvl < DD_DEBUG_LEVEL_INFO))
		return;

	__i915_debugger_print(debugger, DD_DEBUG_LEVEL_INFO, prefix,
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

static void _i915_debugger_free(struct kref *ref)
{
	struct i915_debugger *debugger = container_of(ref, typeof(*debugger), ref);

	put_task_struct(debugger->target_task);
	xa_destroy(&debugger->resources_xa);
	kfree_rcu(debugger, rcu);
}

static void i915_debugger_put(struct i915_debugger *debugger)
{
	kref_put(&debugger->ref, _i915_debugger_free);
}

static inline bool
__is_debugger_closed(const struct drm_i915_private * const i915,
		     const struct i915_debugger * const debugger)
{
	return debugger != rcu_access_pointer(i915->debug.debugger);
}


static inline bool
is_debugger_closed(const struct i915_debugger * const debugger)
{
	return __is_debugger_closed(debugger->i915, debugger);
}

static void i915_debugger_detach(struct i915_debugger *debugger)
{
	struct drm_i915_private * const i915 = debugger->i915;

	mutex_lock(&i915->debug.mutex);
	if (!__is_debugger_closed(i915, debugger)) {
		rcu_replace_pointer(i915->debug.debugger, NULL, true);
		DD_INFO(debugger, "detached");
	}
	mutex_unlock(&i915->debug.mutex);
}

static inline const struct i915_debug_event *
event_pending(const struct i915_debugger * const debugger)
{
	return READ_ONCE(debugger->event);
}

static inline bool is_client_connected(const struct i915_debugger *debugger,
				       const struct i915_drm_client *client)
{
	return READ_ONCE(client->debugger_session) == debugger->session;
}

#define fetch_ack(x) rb_entry_safe(READ_ONCE(x), \
			      typeof(struct i915_debug_ack), rb_node)

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
	struct i915_debug_ack *__ack;
	int result;

	lockdep_assert_held(&debugger->lock);

	while (*p) {
		parent = *p;

		__ack = fetch_ack(parent);
		result = compare_ack(ack->event.seqno, __ack->event.seqno);

		if (result < 0)
			p = &(*p)->rb_left;
		else if (result > 0)
			p = &(*p)->rb_right;
		else
			return false;
	}

	rb_link_node(&ack->rb_node, parent, p);
	rb_insert_color(&ack->rb_node, root);

	DEBUG_ACK(debugger, ack);

	return true;
}

static int prepare_vm_bind_ack(const struct i915_debug_ack *ack)
{
	struct i915_vma *vma = u64_to_ptr(struct i915_vma, ack->event.ack_data);

	if (!(ack->event.flags & PRELIM_DRM_I915_DEBUG_EVENT_CREATE))
		return -EINVAL;

	if (!vma)
		return -EINVAL;

	i915_vma_get(vma);
	i915_vma_add_debugger_fence(vma);

	return 0;
}

static int handle_vm_bind_ack(struct i915_debug_ack *ack)
{
	struct i915_vma *vma = u64_to_ptr(struct i915_vma, ack->event.ack_data);

	if (!(ack->event.flags & PRELIM_DRM_I915_DEBUG_EVENT_CREATE))
		return -EINVAL;

	if (!vma)
		return -EINVAL;

	i915_vma_signal_debugger_fence(vma);
	i915_vma_put(vma);

	return 0;
}

static void
remove_ack(struct i915_debugger *debugger, struct i915_debug_ack *ack)
{
	struct rb_root * const root = &debugger->ack_tree;

	lockdep_assert_held(&debugger->lock);

	BUG_ON(RB_EMPTY_NODE(&ack->rb_node));
	rb_erase(&ack->rb_node, root);
	RB_CLEAR_NODE(&ack->rb_node);

	DEBUG_ACK(debugger, ack);
}

static long
handle_ack(struct i915_debugger *debugger, struct i915_debug_ack *ack)
{
	long ret = -EINVAL;

	switch (ack->event.type) {
	case PRELIM_DRM_I915_DEBUG_EVENT_VM_BIND:
		ret = handle_vm_bind_ack(ack);
		GEM_WARN_ON(ret);
		break;
	}

	DEBUG_ACK(debugger, ack);

	return ret;
}

static struct i915_debug_ack *
create_ack(struct i915_debugger *debugger,
	   const struct i915_debug_event *event,
	   void *data)
{
	struct i915_debug_ack *ack;
	int ret = -EINVAL;

	ack = kzalloc(sizeof(*ack), GFP_KERNEL);
	if (!ack)
		return ERR_PTR(-ENOMEM);

	ack->event.type = event->type;
	ack->event.flags = event->flags;
	ack->event.seqno = event->seqno;
	BUILD_BUG_ON(sizeof(data) > sizeof(ack->event.ack_data));
	ack->event.ack_data = ptr_to_u64(data);

	switch (ack->event.type) {
	case PRELIM_DRM_I915_DEBUG_EVENT_VM_BIND:
		ret = prepare_vm_bind_ack(ack);
		break;
	default:
		GEM_WARN_ON(ack->event.type);
		break;
	}

	if (ret) {
		kfree(ack);
		return ERR_PTR(ret);
	}

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

static void i915_debugger_close(struct i915_debugger *debugger)
{
	i915_debugger_detach(debugger);

	mutex_lock(&debugger->lock);
	release_acks(debugger);
	DD_INFO(debugger, "closed");
	mutex_unlock(&debugger->lock);

	wake_up_all(&debugger->write_done);
	complete(&debugger->read_done);
}

static __poll_t i915_debugger_poll(struct file *file, poll_table *wait)
{
	struct i915_debugger * const debugger = file->private_data;

	if (is_debugger_closed(debugger))
		return 0;

	poll_wait(file, &debugger->write_done, wait);

	if (event_pending(debugger) && !is_debugger_closed(debugger))
		return EPOLLIN;

	return 0;
}

static ssize_t i915_debugger_read(struct file *file,
				  char __user *buf,
				  size_t count,
				  loff_t *ppos)
{
	return 0;
}

static struct i915_debugger *i915_debugger_get(struct drm_i915_private *i915)
{
	struct i915_debugger *debugger;

	rcu_read_lock();
	debugger = rcu_dereference(i915->debug.debugger);
	if (debugger && !kref_get_unless_zero(&debugger->ref))
		debugger = NULL;
	rcu_read_unlock();

	return debugger;
}

static inline bool client_debugged(const struct i915_drm_client * const client)
{
	struct drm_i915_private * const i915 = client->clients->i915;
	struct i915_debugger *debugger;
	bool debugged;

	if (likely(!READ_ONCE(client->debugger_session)))
		return false;

	rcu_read_lock();
	debugger = rcu_dereference(i915->debug.debugger);
	if (debugger)
		debugged = is_client_connected(debugger, client);
	else
		debugged = false;
	rcu_read_unlock();

	return debugged;
}

static int _i915_debugger_send_event(struct i915_debugger * const debugger,
				     const struct i915_debug_event *event,
				     void *ack_data)
{
	struct drm_i915_private * const i915 = debugger->i915;
	const unsigned long user_ms = i915->params.debugger_timeout_ms;
	const unsigned long retry_timeout_ms = 100;
	struct i915_debug_ack *ack = NULL;
	const bool needs_ack = event->flags & PRELIM_DRM_I915_DEBUG_EVENT_NEED_ACK;
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

	if (needs_ack)
		ack = create_ack(debugger, event, ack_data);

	disconnect_ts = ktime_add_ms(ktime_get_raw(), user_ms);
	mutex_lock(&debugger->lock);

	do {
		const struct i915_debug_event *blocking_event;
		u64 blocking_seqno;

		if (is_debugger_closed(debugger)) {
			DD_INFO(debugger, "disconnect on send: debugger was closed\n");
			goto disconnect;
		}

		blocking_event = event_pending(debugger);
		if (!blocking_event)
			break;

		/*
		 * If we did not get access to event, there might be stuck
		 * reader or other writer have raced us. Take a snapshot
		 * of that event seqno.
		 */
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
		blocking_event = event_pending(debugger);
		if (!blocking_event)
			expired = true;
		else if (blocking_event->seqno != blocking_seqno)
			expired = false;
	} while (!expired);

	if (event_pending(debugger)) {
		DD_INFO(debugger, "disconnect: send wait expired");
		goto disconnect;
	}

	reinit_completion(&debugger->read_done);
	debugger->event = event;

	if (needs_ack) {
		if (IS_ERR(ack)) {
			DD_ERR(debugger, "disconnect: ack not created %ld", PTR_ERR(ack));
			goto disconnect;
		}

		if (!insert_ack(debugger, ack)) {
			DD_ERR(debugger, "disconnect: duplicate ack found for %llu",
			       event->seqno);
			handle_ack(debugger, ack);
			kfree(ack);
			goto disconnect;
		}
	}
	mutex_unlock(&debugger->lock);

	wake_up_all(&debugger->write_done);

	if (event_pending(debugger) != event)
		return 0;

	schedule();
	if (event_pending(debugger) != event)
		return 0;

	mutex_lock(&debugger->lock);
	do {
		if (is_debugger_closed(debugger)) {
			DD_INFO(debugger, "disconnect: debugger closed on waiting read");
			goto disconnect;
		}

		/* If it is not our event, we can safely return */
		if (event_pending(debugger) != event)
			break;

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
	} while (!expired);

	/* If it is still our event pending, disconnect */
	if (event_pending(debugger) == event) {
		DD_INFO(debugger, "disconnect: timeout waiting for read");
		goto disconnect;
	}

	mutex_unlock(&debugger->lock);
	return 0;

disconnect:
	mutex_unlock(&debugger->lock);
	i915_debugger_close(debugger);

	return -ENODEV;
}

static int i915_debugger_send_event(struct i915_debugger * const debugger,
				    const struct i915_debug_event *event)
{
	return _i915_debugger_send_event(debugger, event, NULL);
}

static struct i915_debug_event *
__i915_debugger_create_event(struct i915_debugger * const debugger,
			   u32 type, u32 flags, u32 size)
{
	struct i915_debug_event *event;

	GEM_WARN_ON(size <= sizeof(*event));

	event = kzalloc(size, GFP_KERNEL);
	if (!event) {
		DD_ERR(debugger, "unable to create event 0x%08x (ENOMEM), disconnecting", type);
		i915_debugger_close(debugger);
		return NULL;
	}

	event->type = type;
	event->flags = flags;
	event->size = size;

	return event;
}

static struct i915_debug_event *
i915_debugger_create_event(struct i915_debugger * const debugger,
			   u32 type, u32 flags, u32 size)
{
	struct i915_debug_event *event;

	event = __i915_debugger_create_event(debugger, type, flags, size);

	if (event)
		event->seqno = atomic_long_inc_return(&debugger->event_seqno);

	return event;
}

static long wait_for_write(struct i915_debugger *debugger,
			   const unsigned long timeout_ms)
{
	const long waitjiffs =
		msecs_to_jiffies(timeout_ms);

	if (is_debugger_closed(debugger)) {
		complete(&debugger->read_done);
		return -ENODEV;
	}

	if (event_pending(debugger))
		return waitjiffs;

	return wait_event_interruptible_timeout(debugger->write_done,
						event_pending(debugger),
						waitjiffs);
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
	void *buf;
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

	buf = kzalloc(user_event.size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	waits = 0;
	mutex_lock(&debugger->lock);
	do {
		if (is_debugger_closed(debugger)) {
			ret = -ENODEV;
			goto unlock;
		}

		event = event_pending(debugger);
		if (event)
			break;

		mutex_unlock(&debugger->lock);
		if (nonblock) {
			ret = -EAGAIN;
			goto out;
		}

		ret = wait_for_write(debugger, 100);
		if (ret < 0)
			goto out;

		mutex_lock(&debugger->lock);
	} while (waits++ < 10);

	if (!event) {
		ret = -ETIMEDOUT;
		complete(&debugger->read_done);
		goto unlock;
	}

	if (is_debugger_closed(debugger)) {
		ret = -ENODEV;
		goto unlock;
	}

	if (unlikely(user_event.size < event->size)) {
		ret = -EMSGSIZE;
		goto unlock;
	}

	memcpy(&user_event, event, sizeof(user_event));
	memcpy(buf, event->data, event->size - sizeof(*user_orig));

	i915_debugger_print_event(debugger, "read", event);

	debugger->event = NULL;
	complete(&debugger->read_done);
	mutex_unlock(&debugger->lock);
	ret = 0;

	if (copy_to_user(user_orig, &user_event, sizeof(*user_orig))) {
		ret = -EFAULT;
		goto out;
	}

	if (copy_to_user(user_orig + 1, buf,
			 user_event.size - sizeof(*user_orig))) {
		ret = -EFAULT;
		goto out;
	}

out:
	kfree(buf);
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

static void gen12_invalidate_eus(struct drm_i915_private *i915)
{
	const u32 bit = GEN12_INST_STATE_CACHE_INVALIDATE;
	intel_wakeref_t wakeref;
	struct intel_gt *gt;
	int id;

	for_each_gt(i915, id, gt) {
		with_intel_gt_pm_if_awake(gt, wakeref) {
			intel_uncore_write(gt->uncore, GEN9_CS_DEBUG_MODE2,
					   _MASKED_BIT_ENABLE(bit));
		}
	}
}

static int engine_rcu_async_flush(struct intel_engine_cs *engine,
				  unsigned int timeout_us)
{
	struct intel_uncore * const uncore = engine->gt->uncore;
	const i915_reg_t psmi_addr = RING_PSMI_CTL(engine->mmio_base);
	const enum forcewake_domains fw =
		intel_uncore_forcewake_for_reg(uncore, psmi_addr,
					       FW_REG_READ | FW_REG_WRITE) |
		intel_uncore_forcewake_for_reg(uncore,
					       GEN12_RCU_ASYNC_FLUSH,
					       FW_REG_READ | FW_REG_WRITE);
	u32 psmi_ctrl;
	u32 id;
	int ret;

	if (engine->class == COMPUTE_CLASS)
		id = engine->instance + 1;
	else if (engine->class == RENDER_CLASS)
		id = 0;
	else
		GEM_WARN_ON(engine->class);

	if (!intel_engine_pm_get_if_awake(engine))
		return 0;

	spin_lock_irq(&uncore->lock);
	intel_uncore_forcewake_get__locked(uncore, fw);

	psmi_ctrl = intel_uncore_read_fw(uncore, psmi_addr);
	if (!(psmi_ctrl & GEN6_PSMI_SLEEP_MSG_DISABLE))
		intel_uncore_write_fw(uncore, psmi_addr,
				      _MASKED_BIT_ENABLE(GEN6_PSMI_SLEEP_MSG_DISABLE));

	ret = __intel_wait_for_register_fw(uncore, GEN12_RCU_ASYNC_FLUSH,
					   GEN12_RCU_ASYNC_FLUSH_IN_PROGRESS, 0,
					   timeout_us, 0, NULL);
	if (ret)
		goto out;

	intel_uncore_write_fw(uncore, GEN12_RCU_ASYNC_FLUSH,
			      id << GEN12_RCU_ASYNC_FLUSH_ENGINE_ID_SHIFT |
			      GEN12_RCU_ASYNC_FLUSH_ENABLE_MASK);

	ret = __intel_wait_for_register_fw(uncore, GEN12_RCU_ASYNC_FLUSH,
					   GEN12_RCU_ASYNC_FLUSH_IN_PROGRESS, 0,
					   timeout_us, 0, NULL);
out:
	if (!(psmi_ctrl & GEN6_PSMI_SLEEP_MSG_DISABLE))
		intel_uncore_write_fw(uncore, psmi_addr,
				      _MASKED_BIT_DISABLE(GEN6_PSMI_SLEEP_MSG_DISABLE));

	intel_uncore_forcewake_put__locked(uncore, fw);
	spin_unlock_irq(&uncore->lock);

	intel_engine_pm_put(engine);

	return ret;
}

static void dg2_invalidate_eus(struct drm_i915_private *i915)
{
	const unsigned int timeout_us = 100;
	struct intel_gt *gt;
	int gt_id;

	for_each_gt(i915, gt_id, gt) {
		struct intel_engine_cs *engine;
		int engine_id;

		for_each_engine(engine, gt, engine_id) {
			if (engine->class == COMPUTE_CLASS ||
			    engine->class == RENDER_CLASS) {
				if (engine_rcu_async_flush(engine, timeout_us))
					drm_warn(&i915->drm,
						 "debugger: eu invalidation timeout for gt%d, engine %s\n",
						 gt_id, engine->name);
			}
		}
	}
}

static int gen12_gt_invalidate_l3(struct intel_gt *gt,
				  unsigned int timeout_us)
{
	struct intel_uncore * const uncore = gt->uncore;
	const enum forcewake_domains fw =
		intel_uncore_forcewake_for_reg(uncore, GEN7_MISCCPCTL,
					       FW_REG_READ | FW_REG_WRITE) |
		intel_uncore_forcewake_for_reg(uncore, GEN11_GLBLINVL,
					       FW_REG_READ | FW_REG_WRITE);
	intel_wakeref_t wakeref;
	u32 cpctl, cpctl_org, inv, mask;
	int ret;

	/* Reasonable to expect that when it went to sleep, it flushed */
	wakeref = intel_gt_pm_get_if_awake(gt);
	if (!wakeref)
		return 0;

	mask = GEN12_DOP_CLOCK_GATE_RENDER_ENABLE;
	if (GRAPHICS_VER_FULL(gt->i915) >= IP_VER(12, 50))
		mask |= GEN8_DOP_CLOCK_GATE_CFCLK_ENABLE;

	spin_lock_irq(&uncore->lock);
	intel_uncore_forcewake_get__locked(uncore, fw);

	cpctl_org = intel_uncore_read_fw(uncore, GEN7_MISCCPCTL);
	if (cpctl_org & mask)
		intel_uncore_write_fw(uncore, GEN7_MISCCPCTL, cpctl_org & ~mask);

	cpctl = intel_uncore_read_fw(uncore, GEN7_MISCCPCTL);
	if (cpctl & mask) {
		ret = cpctl & GEN12_DOP_CLOCK_GATE_LOCK ? -EACCES : -ENXIO;
		goto out;
	}

	ret = __intel_wait_for_register_fw(uncore, GEN11_GLBLINVL,
					   GEN11_L3_GLOBAL_INVALIDATE, 0,
					   timeout_us, 0, &inv);
	if (ret)
		goto out;

	intel_uncore_write_fw(uncore, GEN11_GLBLINVL,
			      inv | GEN11_L3_GLOBAL_INVALIDATE);

	ret = __intel_wait_for_register_fw(uncore, GEN11_GLBLINVL,
					   GEN11_L3_GLOBAL_INVALIDATE, 0,
					   timeout_us, 0, &inv);

out:
	if (cpctl_org != cpctl)
		intel_uncore_write_fw(uncore, GEN7_MISCCPCTL, cpctl_org);

	intel_uncore_forcewake_put__locked(uncore, fw);
	spin_unlock_irq(&uncore->lock);

	intel_gt_pm_put(gt, wakeref);

	return ret;
}

static void gen12_invalidate_l3(struct drm_i915_private *i915)
{
	const unsigned int timeout_us = 100;
	struct intel_gt *gt;
	int id, ret;

	for_each_gt(i915, id, gt) {
		ret = gen12_gt_invalidate_l3(gt, timeout_us);
		if (ret)
			drm_notice_once(&gt->i915->drm,
					"debugger: gt%d l3 invalidation fail: %d, "
					"use i915.mocs_table_path=uncached.bin\n", id, ret);
	}
}

static void gpu_invalidate_eus(struct drm_i915_private *i915)
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
		return gen12_invalidate_eus(i915);

	dg2_invalidate_eus(i915);
}

static void gpu_invalidate_l3(struct drm_i915_private *i915)
{
	gen12_invalidate_l3(i915);
}

static loff_t i915_debugger_vm_llseek(struct file *file,
				      loff_t offset, int whence)
{
	struct i915_address_space *vm = file->private_data;

	return fixed_size_llseek(file, offset, whence, vm->total);
}

static struct page *__nth_page(struct scatterlist *sg, unsigned long offset)
{
	return nth_page(sg_page(sg), offset >> PAGE_SHIFT);
}

/* returns the number of bytes that have not been read or written */
static int __debugger_copy_from_to_user(struct i915_vma *vma,
					struct scatterlist *sg,
					loff_t k_offset,
					size_t count, loff_t u_offset,
					bool write,
					void __user *r_buffer,
					const void __user *w_buffer)
{
	unsigned int dma_len = sg_dma_len(sg);
	size_t to_copy = count;
	size_t copied = 0;
	unsigned int len;
	int ret = 0;

	GEM_BUG_ON(k_offset + count > dma_len);

	for (len = 0; len < dma_len; len += PAGE_SIZE) {
		void *vaddr;

		if (i915_gem_object_is_lmem(vma->obj)) {
			struct intel_memory_region *mem = vma->obj->mm.region;
			resource_size_t offset;

			offset = sg_dma_address(sg);
			offset -= mem->region.start;

			vaddr = io_mapping_map_wc(&mem->iomap, offset, dma_len);
			mb();
		} else if (i915_gem_object_has_struct_page(vma->obj)) {
			if (k_offset >= PAGE_SIZE) {
				k_offset -= PAGE_SIZE;
				continue;
			}
			to_copy = min_t(size_t, count - copied,
					PAGE_SIZE - k_offset);
			if (!to_copy)
				break;

			vaddr = kmap(__nth_page(sg, len));
			drm_clflush_virt_range(vaddr + k_offset, to_copy);
		} else {
			GEM_BUG_ON(copied);
			return -EINVAL;
		}

		ret = write ?
		      copy_from_user(vaddr + k_offset,
				     w_buffer + u_offset + copied, to_copy) :
		      copy_to_user(r_buffer + u_offset + copied,
				   vaddr + k_offset, to_copy);

		if (i915_gem_object_is_lmem(vma->obj)) {
			mb();
			io_mapping_unmap(vaddr);
			return ret;
		} else {
			struct page *p = __nth_page(sg, len);

			drm_clflush_virt_range(vaddr + k_offset, to_copy);

			mark_page_accessed(p);
			if (write)
				set_page_dirty(p);

			kunmap(p);
		}

		if (ret)
			return count - copied - to_copy + ret;

		copied += to_copy;
		k_offset = 0;
	}

	return 0;
}

static ssize_t __vm_read_write(struct i915_address_space *vm,
			       char __user *r_buffer,
			       const char __user *w_buffer,
			       size_t count, loff_t *__pos, bool write)
{
	struct drm_mm_node *node;
	size_t to_copy = count;
	struct i915_vma *vma;
	loff_t pos = *__pos;
	loff_t end = pos + count;
	ssize_t copied = 0;

	if (range_overflows_t(u64, pos, count, vm->total))
		return -EINVAL;

	rcu_read_lock();
	drm_mm_for_each_node_in_range(node, &vm->mm, pos, end) {
		struct scatterlist *sg;
		s64 offset;
		u32 i;

		vma = container_of(node, typeof(*vma), node);

		if (node->color == I915_COLOR_UNEVICTABLE) {
			rcu_read_unlock();
			return copied ?: -EPERM;
		}

		if (!i915_active_acquire_if_busy(&vma->active)) {
			int err = -EAGAIN;

			vma = i915_vma_tryget(vma);
			rcu_read_unlock();

			if (vma) {
				err = i915_vma_pin(vma, vma->node.size, 0,
						   PIN_USER | PIN_OFFSET_FIXED |
						   vma->node.start);
				if (!err) {
					err = i915_active_acquire(&vma->active);
					i915_vma_unpin(vma);
				}
				i915_vma_put(vma);
			}

			if (err)
				return copied ?: err;
		} else {
			rcu_read_unlock();
		}

		offset = pos - node->start;
		for_each_sg(vma->pages->sgl, sg, vma->pages->nents, i) {
			unsigned int dma_len = sg_dma_len(sg);
			unsigned long ret;
			size_t c;

			/*
			 * Report an error if the user tries
			 * to write into unpopulated space
			 */
			if (unlikely(offset < 0)) {
				copied = copied ?: -EINVAL;
				goto exit_release;
			}

			if (offset > dma_len) {
				offset -= dma_len;
				continue;
			}

			c = min_t(unsigned int, to_copy, dma_len - offset);

			ret = __debugger_copy_from_to_user(vma, sg, offset,
							   c, copied, write,
							   r_buffer, w_buffer);
			if (ret > 0) {
				/* it failed to copy/write everything */
				copied += c - ret;
				goto exit_release;
			} else if (ret < 0) {
				copied = copied ?: ret;
				goto exit_release;
			}

			copied += c;

			if (copied == count)
				goto exit_release;

			to_copy -= c;
			/*
			 * we haven't copied everything but we don't
			 * have anymore gaps from the beginning of the
			 * page
			 */
			offset = 0;
		}
		rcu_read_lock();
		i915_active_release(&vma->active);
	}
	rcu_read_unlock();

	ND_VERBOSE(vm->i915,
		   "vm op=%s vm_address=%px, count = %zu, offset = %llu, copied=%zd",
		   write ? "write" : "read", vm, count, *__pos, copied);
	/*
	 * if copied is '0' it means that we haven't found
	 * any node in the VM's memory range
	 */
	goto exit;

exit_release:
	i915_active_release(&vma->active);

exit:
	/* pread/pwrite ignore this increment */
	*__pos += copied;

	return copied;
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

	s = debugger_vm_write(vm, buffer, count, pos);

	gpu_invalidate_eus(vm->i915);

	return s;
}

static ssize_t i915_debugger_vm_read(struct file *file, char __user *buffer,
				     size_t count, loff_t *pos)
{
	struct i915_address_space *vm = file->private_data;
	ssize_t s;

	gpu_invalidate_l3(vm->i915);

	s = debugger_vm_read(file->private_data, buffer, count, pos);

	return s;
}

static vm_fault_t vm_mmap_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *area = vmf->vma;
	struct i915_address_space *vm = area->vm_private_data;
	vm_fault_t ret = VM_FAULT_SIGBUS;
	struct drm_mm_node *node;

	rcu_read_lock();
	drm_mm_for_each_node_in_range(node, &vm->mm,
				      vmf->pgoff << PAGE_SHIFT,
				      (vmf->pgoff + 1) << PAGE_SHIFT) {
		struct i915_vma *vma = container_of(node, typeof(*vma), node);
		struct scatterlist *sg;
		u64 addr;
		int i;

		if (node->color == I915_COLOR_UNEVICTABLE) {
			rcu_read_unlock();
			return VM_FAULT_SIGBUS;
		}

		/* grab the pages while we probe */
		if (!i915_active_acquire_if_busy(&vma->active)) {
			int err = -EAGAIN;

			vma = i915_vma_tryget(vma);
			rcu_read_unlock();
			if (vma) {
				err = i915_vma_pin(vma,
						   vma->node.size, 0,
						   PIN_USER |
						   PIN_OFFSET_FIXED |
						   vma->node.start);
				if (err == 0) {
					err = i915_active_acquire(&vma->active);
					i915_vma_unpin(vma);
				}
				i915_vma_put(vma);
			}
			if (err)
				return i915_error_to_vmf_fault(err);

		} else {
			rcu_read_unlock();
		}

		/* Having found the node, we need to find the start within it */
		addr = node->start;
		for_each_sg(vma->pages->sgl, sg, vma->pages->nents, i) {
			if (addr + sg_dma_len(sg) > vmf->pgoff << PAGE_SHIFT) {
				pgprot_t prot = pgprot_decrypted(area->vm_page_prot);

				addr = (vmf->pgoff << PAGE_SHIFT) - addr;
				if (i915_gem_object_has_struct_page(vma->obj)) {
					addr >>= PAGE_SHIFT;
					addr += page_to_pfn(sg_page(sg));
				} else {
					addr += sg_dma_address(sg);
					addr += (vma->obj->mm.region->iomap.base -
						 vma->obj->mm.region->region.start);
					addr >>= PAGE_SHIFT;
					prot = pgprot_writecombine(prot);
				}

				ret = vmf_insert_pfn_prot(area,
							  vmf->address,
							  addr,
							  prot);
				break;
			}

			addr += sg_dma_len(sg);
		}

		rcu_read_lock();
		i915_active_release(&vma->active);
	}
	rcu_read_unlock();

	return ret;
}

static const struct vm_operations_struct vm_mmap_ops = {
	.fault = vm_mmap_fault,
};

static int i915_debugger_vm_mmap(struct file *file, struct vm_area_struct *area)
{
	struct i915_address_space *vm = file->private_data;

	area->vm_ops = &vm_mmap_ops;
	area->vm_private_data = file->private_data;
	area->vm_flags |= VM_PFNMAP;

	gpu_invalidate_l3(vm->i915);

	return 0;
}

static int i915_debugger_vm_release(struct inode *inode, struct file *file)
{
	struct i915_address_space *vm = file->private_data;
	struct drm_device *dev = &vm->i915->drm;

	gpu_invalidate_eus(vm->i915);

	i915_vm_put(vm);
	drm_dev_put(dev);

	return 0;
}

static const struct file_operations vm_fops = {
	.owner   = THIS_MODULE,
	.llseek  = i915_debugger_vm_llseek,
	.read    = i915_debugger_vm_read,
	.write   = i915_debugger_vm_write,
	.mmap    = i915_debugger_vm_mmap,
	.release = i915_debugger_vm_release,
};

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
		file->f_mode |= FMODE_PREAD;
		break;
	case O_WRONLY:
		file->f_mode |= FMODE_PWRITE;
		break;
	case O_RDWR:
		file->f_mode |= FMODE_PREAD | FMODE_PWRITE;
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
				    struct intel_engine_cs *engine,
				    u8 *bits,
				    unsigned int bitmask_size)
{
	struct intel_gt *gt = engine->gt;
	struct intel_uncore *uncore = gt->uncore;
	u32 td_ctl;

	/* Make sure we dont promise anything but interrupting all */
	if (bitmask_size)
		return -EINVAL;

	if (IS_PONTEVECCHIO(gt->i915))
		intel_uncore_write_with_mcr_broadcast(uncore,
						      GEN7_ROW_CHICKEN2,
						      _MASKED_BIT_ENABLE(PVC_DISABLE_BTB));

	td_ctl = intel_uncore_read(uncore, TD_CTL);

	/* Halt on next thread dispatch */
	if (!(td_ctl & TD_CTL_FORCE_EXTERNAL_HALT))
		intel_uncore_write_with_mcr_broadcast(uncore, TD_CTL,
						      td_ctl | TD_CTL_FORCE_EXTERNAL_HALT);

	/*
	 * The sleep is needed because some interrupts are ignored
	 * by the HW, hence we allow the HW some time to acknowledge
	 * that.
	 */
	usleep_range(100, 200);

	/* Halt regardless of thread dependancies */
	if (!(td_ctl & TD_CTL_FORCE_EXCEPTION))
		intel_uncore_write_with_mcr_broadcast(uncore, TD_CTL,
						      td_ctl | TD_CTL_FORCE_EXCEPTION);

	usleep_range(100, 200);

	intel_uncore_write_with_mcr_broadcast(uncore, TD_CTL, td_ctl &
					      ~(TD_CTL_FORCE_EXTERNAL_HALT | TD_CTL_FORCE_EXCEPTION));

	if (IS_PONTEVECCHIO(gt->i915))
		intel_uncore_write_with_mcr_broadcast(uncore,
						      GEN7_ROW_CHICKEN2,
						      _MASKED_BIT_DISABLE(PVC_DISABLE_BTB));

	intel_engine_schedule_heartbeat(engine);

	return 0;
}

/*
 * On EU_ATT register there are two rows with 4 eus each with 8 threads per eu.
 * For example on some TGL there is one slice and 6 sublices. This makes 48 eus.
 * However the sseu reports 16 eus per subslice. This is explained by
 * lockstep execution units so there are 2 eus working in pairs.
 * With this in mind the total execution unit number matches but our attention
 * resolution is then half.
 */

#define MAX_ROWS 2u
#define MAX_EUS_PER_ROW 4u
#define MAX_THREADS 8u

/*
 * Using the userspace view for slice/subslices seems wrong but this is only
 * for userspace to match the bitmask sizes. When we divide the actual
 * gslices for hw access, sizes should match.
 *
 */
static unsigned int thread_attn_bitmap_size(const struct intel_gt * const gt)
{
	const struct sseu_dev_info * const sseu = &gt->info.sseu;

	BUILD_BUG_ON(MAX_EUS_PER_ROW * MAX_ROWS * MAX_THREADS !=
		     2 * sizeof(u32) * BITS_PER_BYTE);

	return sseu->max_slices * sseu->max_subslices *
		MAX_ROWS * MAX_THREADS * MAX_EUS_PER_ROW / BITS_PER_BYTE;
}

struct ss_iter {
	struct i915_debugger *debugger;
	unsigned int i;

	unsigned int size;
	u8 *bits;
};

static int read_attn_ss_fw(struct intel_gt *gt, void *data,
			   unsigned int slice, unsigned int subslice, bool present)
{
	struct ss_iter *iter = data;
	struct i915_debugger *debugger = iter->debugger;
	unsigned int row;

	for (row = 0; row < MAX_ROWS; row++) {
		u32 val;

		if (iter->i >= iter->size)
			return 0;

		if (GEM_WARN_ON((iter->i + sizeof(val)) >
				(thread_attn_bitmap_size(gt))))
			return -EIO;

		if (present) {
			val = intel_uncore_read_fw(gt->uncore, TD_ATT(row));

			DD_INFO(debugger, "TD_ATT: (%d:%d:%d): 0x%08x\n",
				slice, subslice, row, val);
		} else {
			val = 0;
			DD_INFO(debugger, "TD_ATT: (%d:%d:%d): 0x%08x FUSED OFF\n",
				slice, subslice, row, val);
		}

		memcpy(&iter->bits[iter->i], &val, sizeof(val));
		iter->i += sizeof(val);
	}

	return 0;
}

static void eu_control_stopped(struct i915_debugger *debugger,
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

	intel_gt_for_each_compute_slice_subslice(engine->gt,
						 false,
						 read_attn_ss_fw, &iter);
}

static int check_attn_ss_fw(struct intel_gt *gt, void *data,
			    unsigned int slice, unsigned int subslice,
			    bool present)
{
	struct ss_iter *iter = data;
	struct i915_debugger *debugger = iter->debugger;
	unsigned int row;

	for (row = 0; row < MAX_ROWS; row++) {
		u32 val, cur = 0;

		if (iter->i >= iter->size)
			return 0;

		if (GEM_WARN_ON((iter->i + sizeof(val)) >
				(thread_attn_bitmap_size(gt))))
			return -EIO;

		memcpy(&val, &iter->bits[iter->i], sizeof(val));
		iter->i += sizeof(val);

		if (present)
			cur = intel_uncore_read_fw(gt->uncore, TD_ATT(row));

		if ((val | cur) != cur) {
			DD_INFO(debugger,
				"WRONG CLEAR (%d:%d:%d) TD_CRL: 0x%08x; TD_ATT: 0x%08x\n",
				slice, subslice, row, val, cur);
			return -EINVAL;
		}
	}

	return 0;
}

static int clear_attn_ss_fw(struct intel_gt *gt, void *data,
			    unsigned int slice, unsigned int subslice,
			    bool present)
{
	struct ss_iter *iter = data;
	struct i915_debugger *debugger = iter->debugger;
	unsigned int row;

	for (row = 0; row < MAX_ROWS; row++) {
		u32 val;

		if (iter->i >= iter->size)
			return 0;

		if (GEM_WARN_ON((iter->i + sizeof(val)) >
				(thread_attn_bitmap_size(gt))))
			return -EIO;

		memcpy(&val, &iter->bits[iter->i], sizeof(val));
		iter->i += sizeof(val);

		if (!val)
			continue;

		if (present) {
			intel_uncore_write_fw(gt->uncore, TD_CLR(row), val);

			DD_INFO(debugger,
				"TD_CLR: (%d:%d:%d): 0x%08x\n",
				slice, subslice, row, val);
		} else {
			DD_WARN(debugger,
				"TD_CLR: (%d:%d:%d): 0x%08x write to fused off subslice\n",
				slice, subslice, row, val);
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
							       false,
							       check_attn_ss_fw,
							       &iter);
		if (ret)
			return ret;

		iter.i = 0;
	}


	intel_gt_for_each_compute_slice_subslice(engine->gt,
						 true,
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

	hw_attn_size = thread_attn_bitmap_size(engine->gt);
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
		ret = eu_control_interrupt_all(debugger, engine, bits, attn_size);
		break;
	case PRELIM_I915_DEBUG_EU_THREADS_CMD_STOPPED:
		eu_control_stopped(debugger, engine, bits, attn_size);
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
		if (!is_client_connected(debugger, client) ||
		    READ_ONCE(client->closed))
			client = NULL;
		else
			client = i915_drm_client_get_rcu(client);
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
	long ret;
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

	ret = -EINVAL;
	mutex_lock(&debugger->lock);
	ack = find_ack(debugger, user_arg.seqno);
	if (ack) {
		ret = handle_ack(debugger, ack);
		if (ret == 0) {
			remove_ack(debugger, ack);
			kfree(ack);
		}
	}
	mutex_unlock(&debugger->lock);

	return ret;
}

static long i915_debugger_ioctl(struct file *file,
				unsigned int cmd,
				unsigned long arg)
{
	struct i915_debugger * const debugger = file->private_data;
	long ret;

	if (is_debugger_closed(debugger)) {
		ret = -ENODEV;
		goto out;
	}

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

out:
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
			      u32 flags, u64 handle)
{
	struct i915_debug_event_vm *vm_event;
	struct i915_debug_event *event;

	event = i915_debugger_create_event(debugger, PRELIM_DRM_I915_DEBUG_EVENT_VM,
					   flags, sizeof(*vm_event));
	if (event) {
		vm_event = from_event(vm_event, event);
		vm_event->client_handle = client->id;
		vm_event->handle = handle;

		i915_debugger_send_event(debugger, event);
		kfree(event);
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
		i915_debugger_close(debugger);
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


static bool __i915_debugger_has_resource(struct i915_debugger *debugger,
					 const void *data)
{
	return __i915_debugger_get_handle(debugger, data, NULL) == 0;
}

static int __i915_debugger_del_handle(struct i915_debugger *debugger,
				      u32 id)
{
	if (!xa_erase(&debugger->resources_xa, id))
		return -ENOENT;
	return 0;
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
		i915_debugger_close(debugger);
		return;
	}

	__i915_debugger_vm_send_event(debugger, client,
				      PRELIM_DRM_I915_DEBUG_EVENT_CREATE,
				      handle);
}

static void i915_debugger_discover_vm_bind(struct i915_debugger *debugger,
					   struct i915_address_space *vm)
{
	int i;
	int ret = 0;
	u64 n = 0;
	size_t size = 0;
	u32 vm_handle;
	struct i915_vma *vma;
	struct i915_vma_metadata *metadata;
	struct list_head *lists[] = {
		&vm->vm_bind_list,
		&vm->vm_bound_list,
		NULL
	}, **list;
	struct i915_debug_event_vm_bind *e;
	void *ev;
	void *__ev;

	ret = __i915_debugger_get_handle(debugger, vm, &vm_handle);
	if (ret) {
		DD_WARN(debugger, "discover_vm_bind did not found handle for vm %p\n", vm);
		return;
	}

	i915_gem_vm_bind_lock(vm);

	for (list = lists; *list; list++)
		list_for_each_entry(vma, *list, vm_bind_link) {
			size += sizeof(struct i915_debug_event_vm_bind);
			list_for_each_entry(metadata,
					    &vma->metadata_list, vma_link)
				size += sizeof(e->uuids[0]);
		}

	if (!size) {
		i915_gem_vm_bind_unlock(vm);
		return;
	}

	ev = kzalloc(size, GFP_KERNEL);
	if (!ev) {
		DD_ERR(debugger, "could not allocate bind event, disconnecting\n");
		goto exit_unlock;
	}

	for (list = lists, __ev = ev; *list; list++)
		list_for_each_entry(vma, *list, vm_bind_link) {
			e = __ev;

			e->base.type     = PRELIM_DRM_I915_DEBUG_EVENT_VM_BIND;
			e->base.flags    = PRELIM_DRM_I915_DEBUG_EVENT_CREATE;
			e->base.seqno    = atomic_long_inc_return(&debugger->event_seqno);
			e->base.size     = sizeof(*e);
			e->client_handle = vm->client->id;
			e->vm_handle     = vm_handle;
			e->va_start      = vma->start;
			e->va_length     = vma->last - vma->start + 1;
			e->flags         = 0;

			list_for_each_entry(metadata,
					    &vma->metadata_list, vma_link) {
				e->uuids[e->num_uuids++] = metadata->uuid->handle;
				e->base.size += sizeof(e->uuids[0]);
			}

			__ev += e->base.size;
			n++;
		}

	i915_gem_vm_bind_unlock(vm);

	for (i = 0, __ev = ev; i < n; i++) {
		struct i915_debug_event_vm_bind *e = __ev;

		i915_debugger_send_event(debugger, to_event(e));
		__ev += e->base.size;
	}

	kfree(ev);

	return;

exit_unlock:
	i915_gem_vm_bind_unlock(vm);
	i915_debugger_close(debugger);
}

static void i915_debugger_discover_vm(struct i915_debugger *debugger,
				      struct i915_drm_client *client)
{
	struct i915_address_space *vm;
	unsigned long i;

	if (!client->file) /* protect kernel internals */
		return;

	if (!is_client_connected(debugger, client))
		return;

	xa_for_each(&client->file->vm_xa, i, vm) {
		if (__i915_debugger_has_resource(debugger, vm))
			continue;

		__i915_debugger_vm_create(debugger, client, vm);
		i915_debugger_discover_vm_bind(debugger, vm);
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

	event = i915_debugger_create_event(debugger, PRELIM_DRM_I915_DEBUG_EVENT_CONTEXT_PARAM,
					   PRELIM_DRM_I915_DEBUG_EVENT_CREATE,
					   sizeof(*ep));
	if (!event)
		return;

	ep = from_event(ep, event);
	ep->client_handle = client->id;
	ep->ctx_handle = ctx_id;
	ep->param.ctx_id = ctx_id;
	ep->param.param = I915_CONTEXT_PARAM_VM;
	ep->param.value = vm_handle;

	i915_debugger_send_event(debugger, event);

	kfree(event);
}

static void i915_debugger_ctx_vm_create(struct i915_debugger *debugger,
					struct i915_gem_context *ctx)
{
	struct i915_address_space *vm = i915_gem_context_get_vm_rcu(ctx);
	bool vm_found;

	vm_found = __i915_debugger_has_resource(debugger, vm);
	if (!vm_found)
		__i915_debugger_vm_create(debugger, ctx->client, vm);

	i915_debugger_ctx_vm_def(debugger, ctx->client, ctx->id, vm);

	if (!vm_found)
		i915_debugger_discover_vm_bind(debugger, vm);

	i915_vm_put(vm);
}

static void
i915_ctx_disable_preempt_timeout(const struct i915_gem_context *ctx)
{
	struct i915_gem_engines_iter it;
	struct intel_context *ce;

	if (!intel_uc_wants_guc_submission(&to_gt(ctx->i915)->uc))
		return;

	for_each_gem_engine(ce, ctx->engines, it)
		intel_guc_context_set_preemption_timeout(ce, 0);
}

static void
i915_debugger_discover_contexts(struct i915_debugger *debugger,
				struct i915_drm_client *client)
{
	struct i915_gem_context *ctx;

	if (!is_client_connected(debugger, client))
		return;

	rcu_read_lock();
	list_for_each_entry_rcu(ctx, &client->ctx_list, client_link) {
		if (!i915_gem_context_get_rcu(ctx))
			continue;

		if (!i915_gem_context_is_closed(ctx)) {
			rcu_read_unlock();

			i915_ctx_disable_preempt_timeout(ctx);

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
				   const struct task_struct * const task)
{
	bool registered = false;

	rcu_read_lock();
	if (!READ_ONCE(client->closed) &&
	    !is_debugger_closed(debugger) &&
	    debugger->target_task == task) {
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

	for_each_gt(i915, gt_id, gt) {
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

static void __restore_guc_preempt_timeout(struct intel_engine_cs *engine)
{
	unsigned long preempt_timeout_ms = engine->props.preempt_timeout_ms;
	struct i915_sched_engine *sched_engine;
	struct i915_request *rq;
	unsigned long flags;

	sched_engine = engine->sched_engine;

	spin_lock_irqsave(&sched_engine->lock, flags);

	list_for_each_entry(rq, &sched_engine->requests, sched.link) {
		struct intel_context *context = rq->context;

		if (i915_debugger_active_on_context(context))
			intel_guc_context_set_preemption_timeout(context,
							preempt_timeout_ms);
	}

	spin_unlock_irqrestore(&sched_engine->lock, flags);
}

static void
i915_debugger_restore_guc_preempt_timeout(struct drm_i915_private *i915)
{
	struct intel_gt *gt;
	unsigned int gt_id;

	for_each_gt(i915, gt_id, gt) {
		struct intel_engine_cs *engine;
		unsigned int engine_id;

		for_each_engine(engine, gt, engine_id)
			__restore_guc_preempt_timeout(engine);
	}
}

static int i915_debugger_release(struct inode *inode, struct file *file)
{
	struct i915_debugger *debugger = file->private_data;

	if (intel_uc_wants_guc_submission(&to_gt(debugger->i915)->uc))
		i915_debugger_restore_guc_preempt_timeout(debugger->i915);

	i915_debugger_close(debugger);
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
		   const struct prelim_drm_i915_debugger_open_param * const param)
{
	const u64 known_open_flags = PRELIM_DRM_I915_DEBUG_FLAG_FD_NONBLOCK;
	struct i915_debugger *debugger;
	struct task_struct *discovery_task;
	unsigned long f_flags = 0;
	int debug_fd;
	bool allowed;
	int ret;

	if (!param->pid)
		return -EINVAL;

	if (param->flags & ~known_open_flags)
		return -EINVAL;

	if (param->version)
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
	atomic_long_set(&debugger->event_seqno, 0);
	debugger->ack_tree = RB_ROOT;
	init_completion(&debugger->read_done);
	init_waitqueue_head(&debugger->write_done);
	init_completion(&debugger->discovery);
	xa_init_flags(&debugger->resources_xa, XA_FLAGS_ALLOC1);

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

	mutex_lock(&i915->debug.mutex);
	if (i915->debug.debugger) {
		ret = -EBUSY;
		goto err_unlock;
	}

	/* XXX handle the overflow without bailing out */
	if (i915->debug.session_count + 1 == 0) {
		drm_err(&i915->drm, "debugger connections exhausted. (you need module reload)\n");
		ret = -EBUSY;
		goto err_unlock;
	}

	debug_fd = anon_inode_getfd("[i915_debugger]", &fops, debugger, f_flags);
	if (debug_fd < 0) {
		ret = debug_fd;
		goto err_unlock;
	}


	if (i915->params.debugger_log_level < 0)
		debugger->debug_lvl = DD_DEBUG_LEVEL_WARN;
	else
		debugger->debug_lvl = min_t(int, i915->params.debugger_log_level,
					    DD_DEBUG_LEVEL_VERBOSE);

	debugger->i915 = i915;
	debugger->session = ++i915->debug.session_count;
	rcu_assign_pointer(i915->debug.debugger, debugger);
	mutex_unlock(&i915->debug.mutex);

	complete(&debugger->read_done);
	wake_up_process(discovery_task);

	compute_engines_reschedule_heartbeat(debugger);

	DD_INFO(debugger, "connected, debug level = %d", debugger->debug_lvl);

	if (debugger->debug_lvl >= DD_DEBUG_LEVEL_VERBOSE)
		printk(KERN_WARNING "i915_debugger: verbose debug level exposing raw addresses!\n");

	return debug_fd;

err_unlock:
	mutex_unlock(&i915->debug.mutex);
	discovery_thread_stop(discovery_task);
err_put_task:
	put_task_struct(debugger->target_task);
err_free:
	xa_destroy(&debugger->resources_xa);
	kfree(debugger);

	return ret;
}

static bool i915_debugger_modparms_are_sane(struct drm_i915_private *i915)
{
	const struct i915_params * const p = &i915->params;
	int fails = 0;

	if (p->debug_eu != 1 && ++fails)
		drm_warn(&i915->drm, "i915_debugger: i915.debug_eu=1 not set (is %d)\n",
			 p->debug_eu);

	return fails == 0;
}

int i915_debugger_open_ioctl(struct drm_device *dev,
			     void *data,
			     struct drm_file *file)
{
	struct drm_i915_private *i915 = to_i915(dev);
	const struct prelim_drm_i915_debugger_open_param * const param = data;

	if (!i915_debugger_modparms_are_sane(i915))
		return -ENODEV;

	return i915_debugger_open(i915, param);
}

void i915_debugger_init(struct drm_i915_private *i915)
{
	mutex_init(&i915->debug.mutex);
}

void i915_debugger_fini(struct drm_i915_private *i915)
{
	mutex_lock(&i915->debug.mutex);
	rcu_replace_pointer(i915->debug.debugger, NULL, true);
	mutex_unlock(&i915->debug.mutex);
}

void i915_debugger_wait_on_discovery(struct drm_i915_private * const i915)
{
	struct pid *pid;
	unsigned long timeleft;
	struct i915_debugger *debugger;
	const unsigned long waitjiffs = msecs_to_jiffies(5000);

	debugger = i915_debugger_get(i915);
	if (!debugger)
		return;

	if (is_debugger_closed(debugger))
		goto out;

	if (debugger->target_task != current)
		goto out;

	timeleft = wait_for_completion_interruptible_timeout(&debugger->discovery,
							     waitjiffs);
	if (timeleft == -ERESTARTSYS) {
		pid = get_task_pid(current, PIDTYPE_PID);
		DD_WARN(debugger,
			"task %d interrupted while waited during debugger discovery process\n",
			pid_nr(pid));
		put_pid(pid);
	} else if (!timeleft) {
		pid = get_task_pid(current, PIDTYPE_PID);
		DD_WARN(debugger,
			"task %d waited too long for discovery to complete. Ignoring barrier.\n",
			pid_nr(pid));
		put_pid(pid);
	}
out:
	i915_debugger_put(debugger);
}

void i915_debugger_client_register(struct i915_drm_client *client)
{
	struct i915_debugger *debugger;

	GEM_WARN_ON(client_debugged(client));

	debugger = i915_debugger_get(client->clients->i915);
	if (!debugger)
		return;

	i915_debugger_client_task_register(debugger, client, current);

	i915_debugger_put(debugger);
}

void i915_debugger_client_release(struct i915_drm_client *client)
{
	WRITE_ONCE(client->debugger_session, 0);
}

static struct i915_debugger *
i915_debugger_get_for_client(const struct i915_drm_client *client)
{
	struct i915_debugger *debugger;

	if (!client_debugged(client))
		return NULL;

	debugger = i915_debugger_get(client->clients->i915);
	if (!debugger)
		return NULL;

	if (is_client_connected(debugger, client))
		return debugger;

	i915_debugger_put(debugger);

	return NULL;
}

static int send_engine_attentions(struct i915_debugger *debugger,
				  struct intel_engine_cs *engine,
				  struct i915_drm_client *client,
				  struct intel_context *ce)
{
	struct i915_debug_event_eu_attention *ea;
	struct i915_debug_event *event;
	unsigned int size;
	int ret;

	if (is_debugger_closed(debugger))
		return -ENODEV;

	/* XXX test for CONTEXT_DEBUG when igt/umd is there */

	size = struct_size(ea, bitmask, thread_attn_bitmap_size(engine->gt));
	event = __i915_debugger_create_event(debugger,
					     PRELIM_DRM_I915_DEBUG_EVENT_EU_ATTENTION,
					     PRELIM_DRM_I915_DEBUG_EVENT_STATE_CHANGE,
					     size);
	if (!event)
		return -ENOMEM;

	ea = from_event(ea, event);
	ea->client_handle = client->id;

	ea->ci.engine_class = engine->uabi_class;
	ea->ci.engine_instance = engine->uabi_instance;
	ea->bitmask_size = thread_attn_bitmap_size(engine->gt);
	ea->ctx_handle = ce->dbg_id.gem_context_id;
	ea->lrc_handle = ce->dbg_id.lrc_id;

	mutex_lock(&debugger->lock);
	eu_control_stopped(debugger, engine, &ea->bitmask[0], ea->bitmask_size);
	event->seqno = atomic_long_inc_return(&debugger->event_seqno);
	mutex_unlock(&debugger->lock);

	ret = i915_debugger_send_event(debugger, event);

	kfree(event);

	return ret;
}

static void i915_debugger_send_engine_attention(struct intel_engine_cs *engine)
{
	struct i915_debugger *debugger;
	struct i915_drm_client *client;
	struct intel_context *ce;
	int ret;

	debugger = i915_debugger_get(engine->i915);
	if (!debugger)
		return;

	ce = engine_active_context_get(engine);
	if (!ce) {
		DD_INFO(debugger, "%s: no active context found\n", engine->name);
		i915_debugger_put(debugger);
		return;
	}

	if (!ce->client) {
		DD_INFO(debugger, "%s: no active client found\n", engine->name);
		intel_context_put(ce);
		i915_debugger_put(debugger);
		return;
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
	if (!is_client_connected(debugger, client)) {
		DD_INFO(debugger, "%s: active client not connected\n", engine->name);
		ret = -ENOTCONN;
	} else if (!completion_done(&debugger->discovery)) {
		DD_INFO(debugger, "%s: discovery not yet done\n", engine->name);
		ret = -EBUSY;
	} else {
		ret = send_engine_attentions(debugger, engine, client, ce);
	}

	i915_drm_client_put(client);
	intel_context_put(ce);
	i915_debugger_put(debugger);

	DD_INFO(debugger, "%s: i915_send_engine_attention:  %d\n", ret);
}

static void
i915_debugger_send_client_event_ctor(const struct i915_drm_client *client,
				     u32 type, u32 flags, u64 size,
				     void (*constructor)(struct i915_debug_event *,
							 const void *),
				     const void *data)
{
	struct i915_debugger *debugger;
	struct i915_debug_event *event;

	debugger = i915_debugger_get_for_client(client);
	if (!debugger)
		return;

	event = i915_debugger_create_event(debugger, type, flags, size);
	if (event) {
		constructor(event, data);
		i915_debugger_send_event(debugger, event);
		kfree(event);
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
					     client_event_ctor, &p);
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
					     ctx_event_ctor, &p);
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
					     uuid_event_ctor, &p);
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

static void i915_debugger_wait_for_vma_ack(struct i915_vma *vma)
{
	struct dma_fence *fence;

	rcu_read_lock();
	fence = dma_fence_get_rcu_safe(&vma->debugger.fence);
	rcu_read_unlock();
	if (fence) {
		dma_fence_wait(fence, false);
		dma_fence_put(fence);
	}
}

static void __i915_debugger_vm_bind_send_event(struct i915_debugger *debugger,
					       struct i915_drm_client *client,
					       struct i915_vma *vma,
					       u32 flags,
					       bool block_until_ack)
{
	struct i915_vma_metadata *metadata;
	struct i915_debug_event_vm_bind *ev;
	struct i915_debug_event *event;
	u32 vm_handle;
	u64 size;

	if (!vma) {
		GEM_WARN_ON(!vma);
		return;
	}

	i915_vma_get(vma);

	if(__i915_debugger_get_handle(debugger, vma->vm, &vm_handle)) {
		DD_ERR(debugger, "handle not found for vm %p, disconnecting\n", vma->vm);
		i915_vma_put(vma);
		i915_debugger_close(debugger);
		return;
	}

	size = sizeof(*ev);
	list_for_each_entry(metadata, &vma->metadata_list, vma_link)
		size += sizeof(ev->uuids[0]);

	if (flags & PRELIM_DRM_I915_DEBUG_EVENT_CREATE)
		flags |= PRELIM_DRM_I915_DEBUG_EVENT_NEED_ACK;

	event = i915_debugger_create_event(debugger, PRELIM_DRM_I915_DEBUG_EVENT_VM_BIND,
					   flags, size);
	if (!event) {
		i915_vma_put(vma);
		DD_ERR(debugger, "debugger: vm_bind_send: alloc fail, bailing out\n");
		return;
	}

	ev = from_event(ev, event);

	ev->client_handle = client->id;
	ev->vm_handle     = vm_handle;
	ev->va_start      = vma->start;
	ev->va_length     = vma->last - vma->start + 1;
	ev->flags         = 0;
	ev->num_uuids     = 0;

	list_for_each_entry(metadata, &vma->metadata_list, vma_link)
		ev->uuids[ev->num_uuids++] = metadata->uuid->handle;

	_i915_debugger_send_event(debugger, event, vma);

	kfree(event);

	if (flags & PRELIM_DRM_I915_DEBUG_EVENT_NEED_ACK && block_until_ack)
		i915_debugger_wait_for_vma_ack(vma);

	i915_vma_put(vma);
}

void i915_debugger_vm_bind_create(struct i915_drm_client *client,
				  struct i915_vma *vma,
				  struct prelim_drm_i915_gem_vm_bind *va)
{
	struct i915_debugger *debugger;
	const bool block_here_until_ack = va->flags & PRELIM_I915_GEM_VM_BIND_IMMEDIATE;

	debugger = i915_debugger_get_for_client(client);
	if (!debugger)
		return;

	__i915_debugger_vm_bind_send_event(debugger, client, vma,
					   PRELIM_DRM_I915_DEBUG_EVENT_CREATE,
					   block_here_until_ack);

	i915_debugger_put(debugger);
}

void i915_debugger_vm_bind_destroy(struct i915_drm_client *client,
				   struct i915_vma *vma)
{
	struct i915_debugger *debugger;

	debugger = i915_debugger_get_for_client(client);
	if (!debugger)
		return;

	__i915_debugger_vm_bind_send_event(debugger, client, vma,
					   PRELIM_DRM_I915_DEBUG_EVENT_DESTROY,
					   false);
	i915_debugger_put(debugger);
}

void i915_debugger_vm_create(struct i915_drm_client *client,
			     struct i915_address_space *vm)
{
	struct i915_debugger *debugger;

	if (!client) {
		GEM_WARN_ON(!client);
		return;
	}

	if (!vm) {
		GEM_WARN_ON(!vm);
		return;
	}

	debugger = i915_debugger_get_for_client(client);
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
	int ret;

	if (!client)
		return;

	if (!vm) {
		GEM_WARN_ON(!vm);
		return;
	}

	debugger = i915_debugger_get_for_client(client);
	if (!debugger)
		return;

	if (atomic_read(&vm->open) > 1)
		goto out;

	ret = __i915_debugger_get_handle(debugger, vm, &handle);
	if (ret) {
		GEM_WARN_ON(ret);
		goto out;
	}

	__i915_debugger_del_handle(debugger, handle);
	__i915_debugger_vm_send_event(debugger, client,
				      PRELIM_DRM_I915_DEBUG_EVENT_DESTROY,
				      handle);

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

	debugger = i915_debugger_get_for_client(client);
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

	debugger = i915_debugger_get_for_client(client);
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

	event = i915_debugger_create_event(debugger, PRELIM_DRM_I915_DEBUG_EVENT_CONTEXT_PARAM,
					   PRELIM_DRM_I915_DEBUG_EVENT_CREATE,
					   event_size);
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
					event_size);
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

	i915_debugger_send_event(debugger, to_event(event_param));

	if (event_engine)
		i915_debugger_send_event(debugger, to_event(event_engine));

	i915_debugger_put(debugger);

	kfree(event_engine);
	kfree(event_param);
}

int i915_debugger_handle_engine_attention(struct intel_engine_cs *engine)
{
	int ret;

	if (!intel_engine_has_eu_attention(engine))
		return 0;

	ret = intel_gt_eu_threads_needing_attention(engine->gt);
	if (ret <= 0)
		return ret;

	atomic_inc(&engine->gt->reset.eu_attention_count);

	/* We dont care if it fails reach this debugger at this time */
	i915_debugger_send_engine_attention(engine);

	/*
	 * We do care if there is no debugger at all to resolve
	 * the situation
	 */
	if (!rcu_access_pointer(engine->i915->debug.debugger))
		return -ENODEV;

	return 0;
}

static bool i915_debugger_active_on_client(struct i915_drm_client *client)
{
	struct i915_debugger *debugger = i915_debugger_get_for_client(client);

	if (debugger)
		i915_debugger_put(debugger);

	return !!debugger; /* implict casting */
}

bool i915_debugger_prevents_hangcheck(struct intel_engine_cs *engine)
{
	if (!intel_engine_has_eu_attention(engine))
		return false;

	return rcu_access_pointer(engine->i915->debug.debugger);
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

void i915_debugger_register_context(struct intel_context *context)
{
	if (!intel_engine_uses_guc(context->engine))
		return;

	if (!i915_debugger_active_on_context(context))
		return;

	intel_guc_context_set_preemption_timeout(context, 0);
}

#define i915_DEBUGGER_ATTENTION_INTERVAL 100
long i915_debugger_attention_poll_interval(struct intel_engine_cs *engine)
{
	long delay = 0;

	GEM_BUG_ON(!engine);

	if (intel_engine_has_eu_attention(engine) &&
	    rcu_access_pointer(engine->i915->debug.debugger))
		delay = i915_DEBUGGER_ATTENTION_INTERVAL;

	return delay;
}

#if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST)
#include "selftests/i915_debugger.c"
#endif
