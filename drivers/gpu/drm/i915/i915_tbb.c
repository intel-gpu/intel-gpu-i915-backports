//  SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 *
 * TBB is a variant on kworker thread pools that allow for late greedy
 * scheduing of CPU tasks. That is the tasks are executed on CPU cores when
 * they become available, rather than predetermining which core or node they
 * should be executed on when first scheduling the work. This allows us to
 * dynamically load balance the tasks to avoid oversubscribing OS cores or
 * trying to utilise active nohz_full cores.
 */

#include <linux/delay.h>

#include <linux/kthread.h>
#include <linux/moduleparam.h>
#include <linux/rbtree.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/smpboot.h>
#include <linux/tick.h>
#include <linux/topology.h>
#include <linux/wait.h>

#include <drm/drm_print.h>

#include "i915_driver.h"
#include "i915_sysrq.h"
#include "i915_tbb.h"
#include "i915_utils.h"

static DEFINE_SPINLOCK(nodes_lock);
static struct rb_root nodes;
static struct i915_tbb_node no_node;

#if IS_ENABLED(CONFIG_NO_HZ_FULL)
static bool __read_mostly use_nohz = CPTCFG_DRM_I915_NOHZ_OFFLOAD;
#else
#define use_nohz false
#endif

#define to_node(x) rb_entry(x, struct i915_tbb_node, rb)
#define as_ptr(x) ((void *)(long)(x))

static int node_key(const void *key, const struct rb_node *node)
{
	return (long)key - to_node(node)->nid;
}

static int node_cmp(struct rb_node *node, const struct rb_node *tree)
{
	return to_node(node)->nid - to_node(tree)->nid;
}

struct i915_tbb_node *i915_tbb_node(int nid)
{
	if (nid == NUMA_NO_NODE)
		nid = 0;

	return to_node(rb_find(as_ptr(nid), &nodes, node_key)) ?: &no_node;
}

void i915_tbb_run_local(struct i915_tbb_node *node, struct list_head *local, void (*fn)(struct i915_tbb *task))
{
	while (!list_empty(local)) {
		struct i915_tbb *task;

		i915_tbb_lock_irq(node);
		task = list_first_entry_or_null(local, typeof(*task), local);
		if (task) {
			list_del_init(&task->link);
			list_del(&task->local);
		}
		i915_tbb_unlock_irq(node);
		if (unlikely(!task))
			return;

		fn(task);
	}
}

struct i915_tbb_thread {
	struct wait_queue_entry wait;
	struct list_head local;
	struct i915_tbb_node *node;
	int cpu;
};
static DEFINE_PER_CPU(struct i915_tbb_thread, i915_tbb_thread);

struct destroy_work {
	struct work_struct base;
	struct task_struct *tsk;
};

static void destroy_worker(struct work_struct *base)
{
	struct destroy_work *wrk = container_of(base, typeof(*wrk), base);

	kthread_park(wrk->tsk);
	kthread_stop(wrk->tsk);
	put_task_struct(wrk->tsk);
	kfree(wrk);
}

static void stop_kthread(struct task_struct *tsk)
{
	struct destroy_work *wrk;

	wrk = kmalloc(sizeof(*wrk), GFP_KERNEL);
	if (!wrk)
		return;

	wrk->tsk = tsk;
	INIT_WORK(&wrk->base, destroy_worker);
	schedule_work(&wrk->base);
}

static int tbb_wakefn(wait_queue_entry_t *wait, unsigned mode, int sync, void *key)
{
	struct i915_tbb_thread *tbb = container_of(wait, typeof(*tbb), wait);

	if (unlikely(tbb->cpu == raw_smp_processor_id()))
		return 0;

	return autoremove_wake_function(wait, mode, sync, key);
}

static void __printfn_info(struct drm_printer *p, struct va_format *vaf)
{
	pr_info(DRIVER_NAME ": %pV", vaf);
}

static void sysrq_show(void *data)
{
	struct i915_tbb_node *node = data;
	struct drm_printer p = {
		.printfn = __printfn_info,
	};
	int indent = 0;
	unsigned long *cpus;

	i_printf(&p, indent, "---\n");
	i_printf(&p, indent, "Threads:\n");
	indent += 2;

	i_printf(&p, indent, "NUMA node: %d\n", node->nid);

	cpus = bitmap_zalloc(2 * ALIGN(NR_CPUS, BITS_PER_LONG), GFP_ATOMIC);
	if (cpus) {
		int num_secondary = 0, num_primary = 0;
		int cpu;

		for_each_online_cpu(cpu) {
			struct i915_tbb_thread *t = per_cpu_ptr(&i915_tbb_thread, cpu);

			if (!t || t->node != node)
				continue;

			if (t->wait.flags & WQ_FLAG_EXCLUSIVE) {
				__set_bit(cpu, cpus);
				num_primary++;
			} else {
				__set_bit(ALIGN(NR_CPUS , BITS_PER_LONG) + cpu, cpus);
				num_secondary++;
			}
		}

		if (num_primary | num_secondary) {
			i_printf(&p, indent, "CPUs:\n");
			indent += 2;

			i_printf(&p, indent, "Primary: %d (%*pbl)\n",
				 num_primary, NR_CPUS, cpus);
			if (num_secondary) {
				i_printf(&p, indent, "Secondary: %d (%*pbl)\n",
					 num_secondary, NR_CPUS, cpus + DIV_ROUND_UP(NR_CPUS, BITS_PER_LONG));
			}

			indent -= 2;
		}

		bitmap_free(cpus);
	}

	if (!list_empty(&node->tasks)) {
		struct i915_tbb *task;
		unsigned int flags;
		void *last = NULL;
		int count = 0;

		i_printf(&p, indent, "Tasks:\n");
		indent += 2;

		flags = i915_tbb_lock(node);
		list_for_each_entry(task, &node->tasks, link) {
			if (task->fn != last) {
				if (count > 1)
					i_printf(&p, indent, "- %ps x %d\n", last, count);
				else if (count > 0)
					i_printf(&p, indent, "- %ps\n", last);

				last = task->fn;
				count = 0;
			}
			count++;
		}
		i915_tbb_unlock(node, flags);

		if (count > 1)
			i_printf(&p, indent, "- %ps x %d\n", last, count);
		else if (count > 0)
			i_printf(&p, indent, "- %ps\n", last);

		indent -= 2;
	}
}

static void tbb_create(unsigned int cpu)
{
	struct i915_tbb_thread *t = per_cpu_ptr(&i915_tbb_thread, cpu);
	struct task_struct *tsk = t->wait.private;
	struct i915_tbb_node *node, *new;
	int nid = cpu_to_node(cpu);

	t->cpu = cpu;
	init_wait(&t->wait);
	t->wait.func = tbb_wakefn;
	t->wait.private = tsk;
	if (!tick_nohz_full_cpu(cpu))
		t->wait.flags |= WQ_FLAG_EXCLUSIVE;
	INIT_LIST_HEAD(&t->local);

	new = kmalloc_node(sizeof(*node), GFP_KERNEL, nid);
	if (!new)
		return;

	init_waitqueue_head(&new->wq);
	INIT_LIST_HEAD(&new->tasks);
	kref_init(&new->ref);
	new->nid = nid;

	spin_lock(&nodes_lock);
	node = to_node(rb_find_add(&new->rb, &nodes, node_cmp));
	if (node) {
		kref_get(&node->ref);
	} else {
		node = new;
		new = NULL;
	}
	spin_unlock(&nodes_lock);

	if (!new)
		i915_sysrq_register(sysrq_show, node);

	t->node = node;
	kfree(new);
}

static void tbb_setup(unsigned int cpu)
{
	struct i915_tbb_thread *t = per_cpu_ptr(&i915_tbb_thread, cpu);

	if (t->wait.flags & WQ_FLAG_EXCLUSIVE)
		sched_set_fifo_low(t->wait.private);
	else if (!use_nohz)
		stop_kthread(t->wait.private);
	else
		sched_set_normal(t->wait.private, 20);
}

static void tbb_release(struct kref *ref)
{
	struct i915_tbb_node *node = container_of(ref, typeof(*node), ref);

	rb_erase(&node->rb, &nodes);
	kfree(node);
}

static void tbb_cleanup(unsigned int cpu, bool online)
{
	struct i915_tbb_thread *t = per_cpu_ptr(&i915_tbb_thread, cpu);
	struct i915_tbb_node *node;

	node = t->node;
	if (node) {
		finish_wait(&node->wq, &t->wait);

		spin_lock(&nodes_lock);
		kref_put(&node->ref, tbb_release);
		spin_unlock(&nodes_lock);

		t->node = NULL;
	}

	t->wait.private = NULL;
}

static void __tbb_wait_queue(struct i915_tbb_thread *t, struct i915_tbb_node *node)
{
	/* Hand-rolled prepare_to_wait to prioritise exclusive wakeups */
	i915_tbb_lock_irq(node);
	if (list_empty(&t->wait.entry)) {
		struct list_head *head;

		head = &node->wq.head;
		if (!(t->wait.flags & WQ_FLAG_EXCLUSIVE))
			head = head->prev;
		list_add(&t->wait.entry, head);
	}
	i915_tbb_unlock_irq(node);
}

static bool tbb_ready(struct i915_tbb_thread *t, struct i915_tbb_node *node)
{
	return !list_empty(&node->tasks);
}

static int tbb_should_run(unsigned int cpu)
{
	struct i915_tbb_thread *t = per_cpu_ptr(&i915_tbb_thread, cpu);
	struct i915_tbb_node *node = t->node;

	if (unlikely(!node))
		return 0;

	if (tbb_ready(t, node))
		return 1;

	set_current_state(TASK_IDLE);
	__tbb_wait_queue(t, node);

	return tbb_ready(t, node);
}

static void tbb_dispatch(unsigned int cpu)
{
	struct i915_tbb_thread *t = per_cpu_ptr(&i915_tbb_thread, cpu);
	struct i915_tbb_node *node = t->node;

	do {
		struct i915_tbb *task;

		if (!tbb_ready(t, node))
			return;

		i915_tbb_lock_irq(node);
		task = list_first_entry_or_null(&t->local, struct i915_tbb, local);
		if (!task)
			task = list_first_entry_or_null(&node->tasks, struct i915_tbb, link);
		if (task) {
			list_del(&task->local);
			list_del_init(&task->link);
			if (!list_empty(&node->tasks))
				wake_up_locked(&node->wq);
			task->tsk = current;
		}
		i915_tbb_unlock_irq(node);
		if (!task)
			return;

		task->fn(task);
	} while (!need_resched());
}

int i915_tbb_suspend_local(void)
{
	int cpu = raw_smp_processor_id();
	struct i915_tbb_thread *t = per_cpu_ptr(&i915_tbb_thread, cpu);

	i915_tbb_lock_irq(t->node);
	if (!list_empty(&t->wait.entry))
		list_del_init(&t->wait.entry);
	else
		wake_up_locked(&t->node->wq);
	i915_tbb_unlock_irq(t->node);

	return cpu;
}

void i915_tbb_resume_local(int cpu)
{
	struct i915_tbb_thread *t = per_cpu_ptr(&i915_tbb_thread, cpu);
	struct i915_tbb_node *node = t->node;

	if (!list_empty(&t->local)) {
		wake_up_process(t->wait.private);
		return;
	}

	__tbb_wait_queue(t, node);
	if (!list_empty(&node->tasks))
		wake_up(&node->wq);
}

static struct smp_hotplug_thread threads = {
	.store = (struct task_struct **)&i915_tbb_thread.wait.private,
	.setup = tbb_setup,
	.create = tbb_create,
	.cleanup = tbb_cleanup,
	.thread_fn = tbb_dispatch,
	.thread_comm = "i915/%u:tbb",
	.thread_should_run = tbb_should_run,
};

int i915_tbb_init(void)
{
	init_waitqueue_head(&no_node.wq);
	INIT_LIST_HEAD(&no_node.tasks);
	no_node.nid = NUMA_NO_NODE;

	smpboot_register_percpu_thread(&threads);
	return 0;
}

void i915_tbb_exit(void)
{
	struct i915_tbb_node *node, *n;
	int cpu;

	rbtree_postorder_for_each_entry_safe(node, n, &nodes, rb)
		i915_sysrq_unregister(sysrq_show, node);

	for_each_online_cpu(cpu) {
		struct i915_tbb_thread *t = per_cpu_ptr(&i915_tbb_thread, cpu);
		if (t && t->wait.private)
			kthread_park(t->wait.private);
	}

	smpboot_unregister_percpu_thread(&threads);
}

void i915_tbb_add_task_locked(struct i915_tbb_node *node, struct i915_tbb *task)
{
	lockdep_assert_held(i915_tbb_get_lock(node));

	task->node = node;
	list_add_tail(&task->link, &node->tasks);
	if (list_is_first(&task->link, &node->tasks))
		wake_up_locked(&node->wq);
}

static void __i915_tbb_add_task(struct i915_tbb *task, struct i915_tbb_thread *t)
{
	struct i915_tbb_node *node = t->node;

	task->node = node;
	list_add_tail(&task->link, &node->tasks);
	list_add_tail(&task->local, &t->local);

	if (!list_empty(&t->wait.entry)) {
		list_del_init(&t->wait.entry);
		wake_up_process(t->wait.private);
		return;
	}

	if (!list_is_first(&task->local, &t->local))
		wake_up_locked(&node->wq);
}

void i915_tbb_add_task_on(struct i915_tbb *task, int cpu)
{
	struct i915_tbb_thread *t = per_cpu_ptr(&i915_tbb_thread, cpu == WORK_CPU_UNBOUND ? raw_smp_processor_id() : cpu);
	struct i915_tbb_node *node = t->node;
	unsigned long flags;

	flags = i915_tbb_lock(node);
	if (list_empty(&task->link))
		__i915_tbb_add_task(task, t);
	else
		wake_up_locked(&node->wq);
	i915_tbb_unlock(node, flags);
}

bool i915_tbb_cancel_task(struct i915_tbb *task)
{
	struct i915_tbb_node *node;
	struct task_struct *tsk = NULL;
	unsigned long flags;

	node = task->node;
	if (!node)
		return false;

	flags = i915_tbb_lock(node);
	if (!list_empty(&task->link)) {
		list_del(&task->local);
		list_del_init(&task->link);
	} else {
		tsk = task->tsk;
	}
	i915_tbb_unlock(node, flags);

	if (tsk) {
		kthread_park(tsk);
		kthread_unpark(tsk);
	}

	return !tsk;
}

#if IS_ENABLED(CONFIG_NO_HZ_FULL)
module_param_named(nohz_offload, use_nohz, bool, 0400);
MODULE_PARM_DESC(nohz_offload, "Allow utilisation of idle nohz_full cores to offload CPU tasks onto");
#endif
