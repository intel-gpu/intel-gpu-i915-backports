/*  SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef __I915_TBB_H__
#define __I915_TBB_H__

#include <linux/list.h>
#include <linux/kref.h>
#include <linux/rbtree.h>
#include <linux/topology.h>
#include <linux/wait.h>

#include <asm/local.h>

struct i915_tbb_node {
	struct rb_node rb;
	struct list_head tasks;
	wait_queue_head_t wq;
	struct kref ref;
	struct {
		local_t tasks;
		local_t local;
		local_t immediate;
		local_t primary;
		local_t secondary;
		local_t yields;
		local_t wakeups;

#ifdef CONFIG_DRM_I915_DEBUG_TBB
		unsigned long last_wake_any;
		unsigned long last_wake_thread;
		unsigned long last_execute;
		unsigned long last_dispatch;
		unsigned long last_sleep;
		unsigned long last_run;
		unsigned long last_schedule;

		int cpu_wake_any;
		int cpu_wake_thread;
		int cpu_execute;
		int cpu_dispatch;
		int cpu_sleep;
		int cpu_run;
		int cpu_schedule;
#endif
	} stats;
	int nid;
};

#ifdef CONFIG_DRM_I915_DEBUG_TBB
#define TBB_STAMP(stats, name, cpu) do { \
	WRITE_ONCE((stats)->last_##name, jiffies); \
	WRITE_ONCE((stats)->cpu_##name, cpu); \
} while (0)
#else
#define TBB_STAMP(stats, name, cpu)
#endif

struct i915_tbb {
	struct list_head link;
	union {
		struct list_head local;
		struct llist_node imm;
	};
	void (*fn)(struct i915_tbb *self);
};

struct i915_tbb_node *i915_tbb_node(int nid);

static inline spinlock_t *i915_tbb_get_lock(struct i915_tbb_node *node)
{
	return &node->wq.lock;
}

static inline bool i915_tbb_trylock(struct i915_tbb_node *node, unsigned long *flags)
{
	return spin_trylock_irqsave(i915_tbb_get_lock(node), *flags);
}

static inline void __i915_tbb_unlock(struct i915_tbb_node *node)
{
	spin_unlock(i915_tbb_get_lock(node));
}

static inline unsigned long i915_tbb_lock(struct i915_tbb_node *node)
{
	unsigned long flags;

	spin_lock_irqsave(i915_tbb_get_lock(node), flags);
	return flags;
}

static inline void i915_tbb_unlock(struct i915_tbb_node *node, unsigned long flags)
{
	spin_unlock_irqrestore(i915_tbb_get_lock(node), flags);
}

static inline void i915_tbb_lock_irq(struct i915_tbb_node *node)
{
	spin_lock_irq(i915_tbb_get_lock(node));
}

static inline void i915_tbb_unlock_irq(struct i915_tbb_node *node)
{
	spin_unlock_irq(i915_tbb_get_lock(node));
}

static inline void i915_tbb_init_task(struct i915_tbb *tsk, void (*fn)(struct i915_tbb *task))
{
	tsk->fn = fn;
	tsk->local.prev = NULL;
	INIT_LIST_HEAD(&tsk->link);
}

void i915_tbb_add_task_locked(struct i915_tbb_node *node, struct i915_tbb *task);
void i915_tbb_run_local(struct i915_tbb_node *node, struct list_head *tasks, void (*fn)(struct i915_tbb *task));

void i915_tbb_add_task_on(struct i915_tbb *task, int cpu);

static inline void i915_tbb_add_task(struct i915_tbb *task)
{
	i915_tbb_add_task_on(task, WORK_CPU_UNBOUND);
}

int i915_tbb_suspend_local(void);
void i915_tbb_resume_local(int cpu);

bool i915_tbb_allow_spin(void);
long i915_tbb_schedule(long timeout);

int i915_tbb_init(void);
void i915_tbb_exit(void);

#endif /* __I915_TBB_H__ */
