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

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/moduleparam.h>
#include <linux/rbtree.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/stat.h>
#include <linux/slab.h>
#include <linux/smpboot.h>
#include <linux/tick.h>
#include <linux/topology.h>
#include <linux/wait.h>

#include <asm/local.h>

#include <uapi/linux/sched/types.h>

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

#if IS_ENABLED(CONFIG_DRM_I915_DEBUG_TBB)
#define TBB_BUG_ON(expr) BUG_ON(expr)
#else
#define TBB_BUG_ON(expr) BUILD_BUG_ON_INVALID(expr)
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
	if (nid <= 0)
		return &no_node;

	return to_node(rb_find(as_ptr(nid), &nodes, node_key)) ?: &no_node;
}

static void task_remove(struct i915_tbb *task)
{
	list_del_init(&task->link);
	__list_del_clearprev(&task->local);
}

void i915_tbb_run_local(struct i915_tbb_node *node, struct list_head *local, void (*fn)(struct i915_tbb *task))
{
	while (!list_empty(local)) {
		struct i915_tbb *task;

		i915_tbb_lock_irq(node);
		task = list_first_entry_or_null(local, typeof(*task), local);
		if (task)
			task_remove(task);
		i915_tbb_unlock_irq(node);
		if (unlikely(!task))
			return;

		fn(task);
		cond_resched();
	}
}

struct i915_tbb_thread {
	struct wait_queue_entry wait;
#define I915_TBB_SUSPEND 31
#define I915_TBB_RENICE 30
#define I915_TBB_CPU GENMASK(29, 8)
	struct list_head local;
	struct llist_head imm;
};
static DEFINE_PER_CPU(struct i915_tbb_thread, i915_tbb_thread);

static inline bool __p_thread(unsigned long flags)
{
	return flags & WQ_FLAG_EXCLUSIVE;
}

static inline bool p_thread(const struct i915_tbb_thread *t)
{
	return __p_thread(t->wait.flags);
}

static inline struct i915_tbb_node *thread_get_node(const struct i915_tbb_thread *t)
{
	return (struct i915_tbb_node *)t->wait.func;
}

static inline int __thread_get_cpu(unsigned long flags)
{
	return FIELD_GET(I915_TBB_CPU, flags);
}

static inline int thread_get_cpu(const struct i915_tbb_thread *t)
{
	return __thread_get_cpu(t->wait.flags);
}

static inline bool thread_is_running(const struct i915_tbb_thread *t)
{
	struct task_struct *tsk = t->wait.private;

	return task_is_running(tsk);
}

static inline bool thread_is_waiting(const struct i915_tbb_thread *t)
{
	struct task_struct *tsk = t->wait.private;

#ifdef BPM_TASK_STRUCT_STATE_MEMBER_NOT_PRESENT
	return READ_ONCE(tsk->state) & TASK_NOLOAD;
#else
	return READ_ONCE(tsk->__state) & TASK_NOLOAD;
#endif
}

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

static inline bool tbb_ready(const struct i915_tbb_node *node)
{
	return !list_empty(&node->tasks);
}

static inline bool __wake_up_thread(struct i915_tbb_thread *t)
{
	if (!thread_is_waiting(t))
		return false;

	if (test_tsk_thread_flag(t->wait.private, TIF_NEED_RESCHED))
		return false;

	return default_wake_function(&t->wait, TASK_NOLOAD, 0, NULL);
}

static inline bool __thread_is_suspended(unsigned long flags)
{
	return test_bit(I915_TBB_SUSPEND, &flags);
}

static inline bool thread_is_suspended(const struct i915_tbb_thread *t)
{
	return __thread_is_suspended(t->wait.flags);
}

static inline void thread_set_suspend(struct i915_tbb_thread *t)
{
	set_bit(I915_TBB_SUSPEND, (unsigned long *)&t->wait.flags);
}

static inline void thread_clr_suspend(struct i915_tbb_thread *t)
{
	clear_bit(I915_TBB_SUSPEND, (unsigned long *)&t->wait.flags);
}

static inline bool thread_has_renice(const struct i915_tbb_thread *t)
{
	return test_bit(I915_TBB_RENICE, (unsigned long *)&t->wait.flags);
}

static inline bool thread_set_renice(struct i915_tbb_thread *t)
{
	if (!IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_TBB_RENICE))
		return true;

	return test_and_set_bit(I915_TBB_RENICE, (unsigned long *)&t->wait.flags);
}

static inline bool thread_clr_renice(struct i915_tbb_thread *t)
{
	if (!IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_TBB_RENICE))
		return false;

	return test_and_clear_bit(I915_TBB_RENICE, (unsigned long *)&t->wait.flags);
}

static inline bool __thread_is_fifo(unsigned long flags)
{
	return flags & (BIT(I915_TBB_RENICE) | WQ_FLAG_EXCLUSIVE);
}

static inline bool thread_is_fifo(const struct i915_tbb_thread *t)
{
	return __thread_is_fifo(t->wait.flags);
}

static bool tbb_wake_up(struct i915_tbb_node *node, int excl)
{
	int active = 0;

	/*
	 * First pass: only wake up the first thread that is guaranteed to run,
	 * e.g. OS cores and those with raised rt_prio.
	 *
	 * Second pass: wake up the next available thread, irrespective of its
	 * fifo status. If there was an active fifo thread already running,
	 * finish, otherwise keep waking up each thread, triggering the
	 * thundering herd as we are unsure if any may run due to application
	 * pressure.
	 */
	do{
		struct i915_tbb_thread *t;

		list_for_each_entry(t, &node->wq.head, wait.entry) {
			unsigned long flags = t->wait.flags;

			if (excl < 0 && !__thread_is_fifo(flags))
				continue;

			if (unlikely(__thread_is_suspended(flags)))
				continue;

			if (!__wake_up_thread(t))
				continue;

			local_inc(&node->stats.wakeups);
			TBB_STAMP(&node->stats, wake_any, __thread_get_cpu(flags));
			if (excl - active < 0)
				return true;

			active += __thread_is_fifo(flags);
		}
	} while (excl++ < 0);

	return false;
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

	cpus = bitmap_zalloc(6 * ALIGN(NR_CPUS, BITS_PER_LONG), GFP_ATOMIC);
	if (cpus) {
		int num_primary = 0;
		int num_secondary = 0;
		int num_running = 0;
		int num_suspend = 0;
		int num_prio = 0;
		int num_queued = 0;
		int cpu;

		for_each_online_cpu(cpu) {
			struct i915_tbb_thread *t = per_cpu_ptr(&i915_tbb_thread, cpu);

			if (!t || thread_get_node(t) != node || !t->wait.private)
				continue;

			if (p_thread(t)) {
				__set_bit(cpu, cpus);
				num_primary++;
			} else {
				__set_bit(ALIGN(NR_CPUS , BITS_PER_LONG) + cpu, cpus);
				num_secondary++;
			}

			if (thread_is_suspended(t)) {
				__set_bit(3 * ALIGN(NR_CPUS , BITS_PER_LONG) + cpu, cpus);
				num_suspend++;
			} else {
				if (!thread_is_waiting(t)) {
					__set_bit(2 * ALIGN(NR_CPUS , BITS_PER_LONG) + cpu, cpus);
					num_running++;
				}

				if (thread_has_renice(t)) {
					__set_bit(4 * ALIGN(NR_CPUS , BITS_PER_LONG) + cpu, cpus);
					num_prio++;
				}
			}

			if (!list_empty(&t->local) || !llist_empty(&t->imm)) {
				__set_bit(5 * ALIGN(NR_CPUS , BITS_PER_LONG) + cpu, cpus);
				num_queued++;
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
			if (num_running) {
				i_printf(&p, indent, "Running: %d (%*pbl)\n",
					 num_running, NR_CPUS, cpus + 2 * DIV_ROUND_UP(NR_CPUS, BITS_PER_LONG));
			}
			if (num_suspend) {
				i_printf(&p, indent, "Suspended: %d (%*pbl)\n",
					 num_suspend, NR_CPUS, cpus + 3 * DIV_ROUND_UP(NR_CPUS, BITS_PER_LONG));
			}
			if (num_prio) {
				i_printf(&p, indent, "Boosted: %d (%*pbl)\n",
					 num_prio, NR_CPUS, cpus + 4 * DIV_ROUND_UP(NR_CPUS, BITS_PER_LONG));
			}
			if (num_queued) {
				i_printf(&p, indent, "Queued: %d (%*pbl)\n",
					 num_queued, NR_CPUS, cpus + 5 * DIV_ROUND_UP(NR_CPUS, BITS_PER_LONG));
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

#if IS_ENABLED(CONFIG_DRM_I915_DEBUG_TBB)
		i_printf(&p, indent - 2, "Debug:\n");
		i_printf(&p, indent, "Last wake any: %ld (%d)\n", node->stats.last_wake_any, node->stats.cpu_wake_any);
		i_printf(&p, indent, "Last wake thread: %ld (%d)\n", node->stats.last_wake_thread, node->stats.cpu_wake_thread);
		i_printf(&p, indent, "Last sleep: %ld (%d)\n", node->stats.last_sleep, node->stats.cpu_sleep);
		i_printf(&p, indent, "Last run: %ld (%d)\n", node->stats.last_run, node->stats.cpu_run);
		i_printf(&p, indent, "Last execute: %ld (%d)\n", node->stats.last_execute, node->stats.cpu_execute);
		i_printf(&p, indent, "Last dispatch: %ld (%d)\n", node->stats.last_dispatch, node->stats.cpu_dispatch);
		i_printf(&p, indent, "Last schedule: %ld (%d)\n", node->stats.last_schedule, node->stats.cpu_schedule);
#endif

		indent -= 2;
	}

	if (local_read(&node->stats.tasks)) {
		i_printf(&p, indent, "Execution:\n");
		indent += 2;

		i_printf(&p, indent, "Tasks: %ld\n", local_read(&node->stats.tasks));
		i_printf(&p, indent, "Local: %ld\n", local_read(&node->stats.local));
		i_printf(&p, indent, "Immediate: %ld\n", local_read(&node->stats.immediate));
		if (local_read(&node->stats.secondary)) {
			i_printf(&p, indent, "Primary: %ld\n", local_read(&node->stats.primary));
			i_printf(&p, indent, "Secondary: %ld\n", local_read(&node->stats.secondary));
		}
		i_printf(&p, indent, "Yields: %ld\n", local_read(&node->stats.yields));
		i_printf(&p, indent, "Wakeups: %ld\n", local_read(&node->stats.wakeups));

		indent -= 2;
	}
}

static inline wait_queue_func_t as_func(struct i915_tbb_node *node)
{
	return (wait_queue_func_t)node;
}

static void tbb_create(unsigned int cpu)
{
	struct i915_tbb_thread *t = per_cpu_ptr(&i915_tbb_thread, cpu);
	struct task_struct *tsk = t->wait.private;
	struct i915_tbb_node *node;
	int nid = cpu_to_node(cpu);

	init_wait(&t->wait);
	t->wait.func = as_func(&no_node);
	t->wait.private = tsk;
	t->wait.flags = FIELD_PREP(I915_TBB_CPU, cpu);
	if (!tick_nohz_full_cpu(cpu))
		t->wait.flags |= WQ_FLAG_EXCLUSIVE;
	INIT_LIST_HEAD(&t->local);
	init_llist_head(&t->imm);
	if (nid <= 0)
		return;

	spin_lock(&nodes_lock);
	node = to_node(rb_find(as_ptr(nid), &nodes, node_key));
	if (node)
		kref_get(&node->ref);
	spin_unlock(&nodes_lock);
	if (!node) {
		struct i915_tbb_node *new;

		new = kzalloc_node(sizeof(*node), GFP_KERNEL, nid);
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

		kfree(new);
	}

	t->wait.func = as_func(node);
}

static void __tbb_wait_queue(struct i915_tbb_thread *t, struct i915_tbb_node *node)
{
	/*
	 * Hand-rolled prepare_to_wait to prioritise exclusive wakeups.
	 *
	 * Core priority is: local ioctl core, OS cores, idle NOHZ
	 * (application) cores.
	 *
	 * As the OS cores are exclusive waiter placed at the head of the
	 * waitqueue, they will consume any wake_up() first, and only if the
	 * work exceeds the capacity of the OS cores will we then spill over to
	 * NOHZ cores.
	 */
	i915_tbb_lock_irq(node);
	if (list_empty(&t->wait.entry)) {
		struct list_head *head;

		/*
		 * XXX Adding an element to the end of the waitqueue may break
		 * wake_up() which uses a bookmarking system to cond_resched()
		 * every 64 elements. We typically may have 104 cores /
		 * elements on each node's waitqueue, and so hit the bookmark
		 * break. When the tbb_should_run() wakes up, it will call
		 * __tbb_wait_queue() and insert the element at the end of the
		 * waitqueue after the bookmark, thus forcing the wake_up() to
		 * hit the breakpoint once more, causing the wake_up() never to
		 * finish iterating the waitqueue.
		 *
		 * Note that wake_up_locked() doesn't use the bookmark break
		 * and so is not susceptible to the same infinite wake_up().
		 */
		head = &node->wq.head;
		if (!p_thread(t))
			head = head->prev;
		list_add(&t->wait.entry, head);
	}
	i915_tbb_unlock_irq(node);
}

static void sched_set_idle(struct task_struct *p)
{
#ifdef BPM_SCHED_SETATTR_NOCHECK_NOT_PRESENT
	sched_set_normal(p, MAX_NICE);
#else
	struct sched_attr attr = {
		.sched_policy = SCHED_IDLE,
		.sched_nice = MAX_NICE,
	};

	if (sched_setattr_nocheck(p, &attr)) {
		attr.sched_policy = SCHED_NORMAL;
		sched_setattr_nocheck(p, &attr);
	}
#endif
}

static void tbb_setup(unsigned int cpu)
{
	struct i915_tbb_thread *t = per_cpu_ptr(&i915_tbb_thread, cpu);

	__tbb_wait_queue(t, thread_get_node(t));

	/*
	 * Force our tasks to run as soon as possible on OS cores, but for
	 * nohz_full (tickless) cores we set the tbb task's priority to be the
	 * minimum such that if there are any other proceses running on that
	 * core they will take priority and prevent TBB from executing on that
	 * cpu.
	 *
	 * OS cores are exclusive waiters at the head of the waitqueue, they
	 * will consume any wake up if they are idle.  NOHZ cores are
	 * not-exclusive waiters, all will be woken if there is more work than
	 * can be absorbed by the OS cores, however due to the priority only
	 * idle cores will be woken and take work.
	 */
	if (p_thread(t))
		sched_set_fifo_low(t->wait.private);
	else if (!use_nohz)
		stop_kthread(t->wait.private);
	else
		sched_set_idle(t->wait.private);
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
	struct i915_tbb_node *node = thread_get_node(t);

	finish_wait(&node->wq, &t->wait);
	thread_set_suspend(t);
	t->wait.private = NULL;

	if (node != &no_node) {
		spin_lock(&nodes_lock);
		kref_put(&node->ref, tbb_release);
		spin_unlock(&nodes_lock);

		t->wait.func = as_func(&no_node);
	}
}

static bool tbb_should_wake_up(struct i915_tbb_node *node, struct i915_tbb_thread *t)
{
	return tbb_ready(node) && (p_thread(t) || !list_empty(&t->local));
}

static bool idle_app_cpu(struct i915_tbb_thread *t)
{
	return p_thread(t) || single_task_running();
}

static bool wake_up_thread(struct i915_tbb_thread *t)
{
	if (thread_is_suspended(t))
		return false;

	if (t->wait.private == current) {
		__set_current_state(TASK_RUNNING);
		return true;
	}

	if (__wake_up_thread(t)) {
		if (thread_get_cpu(t) == task_cpu(current))
			set_tsk_need_resched(current);

		local_inc(&thread_get_node(t)->stats.yields);
		TBB_STAMP(&thread_get_node(t)->stats, wake_thread, thread_get_cpu(t));
		return true;
	}

	return thread_is_fifo(t);
}

static bool wake_up_next(struct i915_tbb_thread *t)
{
	struct i915_tbb_node *node = thread_get_node(t);

	t = list_next_entry(t, wait.entry);
	if (list_is_head(&t->wait.entry, &node->wq.head))
		t = list_first_entry(&node->wq.head, typeof(*t), wait.entry);

	return tbb_wake_up(node, -wake_up_thread(t));
}

static int tbb_should_run(unsigned int cpu)
{
	struct i915_tbb_thread *t = per_cpu_ptr(&i915_tbb_thread, cpu);
	struct i915_tbb_node *node = thread_get_node(t);

	set_current_state(TASK_IDLE); /* don't contribute to system load */
	TBB_STAMP(&node->stats, sleep, cpu);
	if (!tbb_ready(node))
		goto idle;

	if (thread_is_suspended(t) || need_resched()) {
		if (p_thread(t) || !list_empty(&t->local)) {
			/* try again after schedule() */
			if (!thread_is_suspended(t))
				__set_current_state(TASK_RUNNING);
		}

		goto idle;
	}

	/* Only run remote tasks on nohz_full cores if completely idle */
	if (list_empty(&t->local) && !idle_app_cpu(t))
		goto idle;

	TBB_STAMP(&node->stats, run, cpu);
	return 1;

idle:
	if (thread_clr_renice(t)) {
		preempt_enable();
		if (p_thread(t))
			sched_set_fifo_low(t->wait.private);
		else
			sched_set_idle(t->wait.private);
		if (unlikely(thread_has_renice(t)))
			sched_set_fifo(t->wait.private);
		preempt_disable();
	}

	/* Lost our exclusive wakeup, so delegate another thread to take over */
	if (tbb_ready(node))
		wake_up_next(t);

	return 0;
}

static void tbb_dispatch(unsigned int cpu)
{
	struct i915_tbb_thread *t = per_cpu_ptr(&i915_tbb_thread, cpu);
	struct i915_tbb_node *node = thread_get_node(t);
	bool cascade = true;

	TBB_STAMP(&node->stats, execute, cpu);
	do {
		struct i915_tbb *task;

		if (!tbb_ready(node))
			return;

		i915_tbb_lock_irq(node);
		if (!list_empty(&t->local)) {
			local_inc(&node->stats.local);
			task = list_first_entry(&t->local, struct i915_tbb, local);
		} else if (!list_empty(&node->tasks) && idle_app_cpu(t)) {
			local_inc(p_thread(t) ? &node->stats.primary : &node->stats.secondary);
			task = list_first_entry(&node->tasks, struct i915_tbb, link);
		} else {
			i915_tbb_unlock_irq(node);
			return;
		}

		task_remove(task);
		i915_tbb_unlock_irq(node);

		if (cascade && tbb_ready(node))
			cascade = wake_up_next(t);

		TBB_STAMP(&node->stats, dispatch, cpu);
		do {
			local_inc(&node->stats.tasks);
			task->fn(task);

			if (llist_empty(&t->imm))
				break;

			task = container_of(t->imm.first, typeof(*task), imm);
			TBB_BUG_ON(!list_empty(&task->link));
			t->imm.first = task->imm.next;
			task->local.prev = NULL;

			local_inc(&node->stats.immediate);
		} while (1);
	} while (!need_resched());
}

int i915_tbb_suspend_local(void)
{
	int cpu = raw_smp_processor_id();
	struct i915_tbb_thread *t = per_cpu_ptr(&i915_tbb_thread, cpu);
	struct i915_tbb_node *node = thread_get_node(t);

	if (unlikely(!t->wait.private))
		return cpu;

	/*
	 * If we were an exclusive primary thread and had work assigned to us,
	 * we need to transfer that work onto the next thread.
	 */
	thread_set_suspend(t);
	if (unlikely(!llist_empty(&t->imm))) {
		struct i915_tbb *task, *next;
		unsigned long flags;

		flags = i915_tbb_lock(node);
		llist_for_each_entry_safe(task, next, __llist_del_all(&t->imm), imm) {
			list_add_tail(&task->link, &node->tasks);
			list_add_tail(&task->local, &t->local);
		}
		i915_tbb_unlock(node, flags);
	}
	if (!thread_is_waiting(t) && tbb_should_wake_up(node, t))
		wake_up_next(t);

	return cpu;
}

void i915_tbb_resume_local(int cpu)
{
	struct i915_tbb_thread *t = per_cpu_ptr(&i915_tbb_thread, cpu);

	if (unlikely(!t->wait.private))
		return;

	/* On returning to the pool, see if there is work we should be doing */
	thread_clr_suspend(t);
	if (tbb_should_wake_up(thread_get_node(t), t))
		wake_up_thread(t);
}

static struct i915_tbb_thread *this_thread(void)
{
	return per_cpu_ptr(&i915_tbb_thread, raw_smp_processor_id());
}

static bool __i915_tbb_wake(void)
{
	struct i915_tbb_thread *t = this_thread();

	if (!tbb_ready(thread_get_node(t)))
		return false;

	return wake_up_thread(t);
}

bool i915_tbb_allow_spin(void)
{
	if (need_resched())
		return false;

	if (!list_empty(&this_thread()->local))
		return false;

	return single_task_running();
}

long i915_tbb_schedule(long timeout)
{
	if (__i915_tbb_wake()) {
		__maybe_unused struct i915_tbb_thread *t = this_thread();

		TBB_STAMP(&thread_get_node(t)->stats, schedule, thread_get_cpu(t));
	}
	return io_schedule_timeout(timeout);
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
	i915_sysrq_register(sysrq_show, &no_node);

	smpboot_register_percpu_thread(&threads);
	return 0;
}

void i915_tbb_exit(void)
{
	struct i915_tbb_node *node, *n;
	int cpu;

	rbtree_postorder_for_each_entry_safe(node, n, &nodes, rb)
		i915_sysrq_unregister(sysrq_show, node);
	i915_sysrq_unregister(sysrq_show, &no_node);

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

	list_add_tail(&task->link, &node->tasks);
	if (list_is_first(&task->link, &node->tasks))
		wake_up_next(this_thread());
}

static struct list_head *head_or_tail(struct list_head *head, bool tail)
{
	return tail ? head->prev : head;
}

static void __i915_tbb_add_task_on(struct i915_tbb *task, struct i915_tbb_thread *t)
{
	struct i915_tbb_node *node = thread_get_node(t);
	unsigned long flags = i915_tbb_lock(node);
	bool wakeup;

	list_add(&task->link, head_or_tail(&node->tasks, !in_hardirq()));
	if (t->wait.private) {
		list_add(&task->local, &t->local);
		wakeup = list_is_last(&task->local, &t->local);
	} else {
		INIT_LIST_HEAD(&task->local);
		wakeup = true;
	}

	i915_tbb_unlock(node, flags);
	if (!wakeup)
		return;

	if (!wake_up_thread(t)) {
		wake_up_next(t);
		return;
	}

	if (!in_atomic() && !irqs_disabled() && !thread_set_renice(t))
		sched_set_fifo(t->wait.private);
}

void i915_tbb_add_task_on(struct i915_tbb *task, int cpu)
{
	struct i915_tbb_thread *t = per_cpu_ptr(&i915_tbb_thread, cpu == WORK_CPU_UNBOUND ? raw_smp_processor_id() : cpu);

#if 0
	if (READ_ONCE(task->local.prev) || xchg(&task->local.prev, &task->local))
		return;
#endif

	TBB_BUG_ON(!list_empty(&task->link));

	/* "Supplementary" tasks tend to be very quick, e.g. notifications */
	if (t->wait.private == current && !in_hardirq()) {
		__llist_add(&task->imm, &t->imm);
		return;
	}

	__i915_tbb_add_task_on(task, t);
}

#if IS_ENABLED(CONFIG_NO_HZ_FULL)
module_param_named(nohz_offload, use_nohz, bool, 0400);
MODULE_PARM_DESC(nohz_offload, "Allow utilisation of idle nohz_full cores to offload CPU tasks onto");
#endif
