/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef __I915_DEBUGGER_TYPES_H__
#define __I915_DEBUGGER_TYPES_H__

#include <linux/mutex.h>
#include <linux/kref.h>
#include <uapi/drm/i915_drm.h>
#include <linux/completion.h>
#include <linux/wait.h>
#include <linux/xarray.h>
#include <linux/rbtree.h>

struct task_struct;
struct drm_i915_private;

struct i915_debug_event {
	u32 type;
	u32 flags;
	u64 seqno;
	union {
		u64 size;
		u64 ack_data;
	};
	u8 data[0];
} __packed;

struct i915_debug_event_client {
	struct i915_debug_event base;
	u64 handle;
} __packed;

struct i915_debug_event_context {
	struct i915_debug_event base;
	u64 client_handle;
	u64 handle;
} __packed;

struct i915_debug_event_uuid {
	struct i915_debug_event base;
	u64 client_handle;
	u64 handle;
	u64 class_handle;
	u64 payload_size;
} __packed;

struct i915_debug_event_vm {
	struct i915_debug_event base;
	u64 client_handle;
	u64 handle;
} __packed;

struct i915_debug_event_vm_bind {
	struct i915_debug_event base;
	u64 client_handle;

	u64 vm_handle;
	u64 va_start;
	u64 va_length;
	u32 num_uuids;
	u32 flags;
	u64 uuids[0];
} __packed;

struct i915_debug_event_context_param {
	struct i915_debug_event base;
	u64 client_handle;
	u64 ctx_handle;
	struct drm_i915_gem_context_param param;
} __packed;

struct i915_debug_engine_info {
	struct i915_engine_class_instance engine;
	u64 lrc_handle;
} __packed;

struct i915_debug_event_engines {
	struct i915_debug_event base;
	u64 client_handle;
	u64 ctx_handle;
	u64 num_engines;
	struct i915_debug_engine_info engines[0];
} __packed;

struct i915_debug_event_eu_attention {
	struct i915_debug_event base;
	u64 client_handle;
	u64 ctx_handle;
	u64 lrc_handle;

	u32 flags;
	struct i915_engine_class_instance ci;

	u32 bitmask_size;
	u8  bitmask[0];
} __packed;

struct i915_debug_vm_open {
	u64 client_handle;
	u64 handle;
	u64 flags;
};

struct i915_debug_ack {
	struct rb_node rb_node;
	struct i915_debug_event event;
};

struct i915_debugger {
	struct kref ref;
	struct rcu_head rcu;
	struct mutex lock;
	struct drm_i915_private *i915;
	int debug_lvl;
	struct task_struct *target_task;
	wait_queue_head_t write_done;
	struct completion read_done;
	struct completion discovery;
	unsigned int next_handle;
	struct xarray resources_xa;
	int disconnect_reason;

	struct list_head connection_link;

	u64 session;
	atomic_long_t event_seqno;

	struct rb_root ack_tree;

	const struct i915_debug_event *event;
};

#endif /* __I915_DEBUGGER_TYPES_H__ */
