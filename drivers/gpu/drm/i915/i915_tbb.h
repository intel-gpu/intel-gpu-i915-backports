/*  SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef __I915_TBB_H__
#define __I915_TBB_H__

#include <linux/list.h>
#include <linux/kref.h>
#include <linux/rbtree.h>
#include <linux/wait.h>

struct i915_tbb {
	struct list_head link;
	struct list_head local;
	void (*fn)(struct i915_tbb *self);
};

struct i915_tbb_node {
	struct rb_node rb;
	struct list_head tasks;
	wait_queue_head_t wq;
	struct kref ref;
	int nid;
};

struct i915_tbb_node *i915_tbb_node(int nid);

static inline spinlock_t *i915_tbb_get_lock(struct i915_tbb_node *node)
{
	return &node->wq.lock;
}

static inline void i915_tbb_lock(struct i915_tbb_node *node)
{
	spin_lock(i915_tbb_get_lock(node));
}

static inline void i915_tbb_unlock(struct i915_tbb_node *node)
{
	spin_unlock(i915_tbb_get_lock(node));
}

void i915_tbb_add_task_locked(struct i915_tbb_node *node, struct i915_tbb *task);
void i915_tbb_run_local(struct i915_tbb_node *node, struct list_head *tasks, void (*fn)(struct i915_tbb *task));

int i915_tbb_suspend_local(void);
void i915_tbb_resume_local(int cpu);

int i915_tbb_init(void);
void i915_tbb_exit(void);

#endif /* __I915_TBB_H__ */
