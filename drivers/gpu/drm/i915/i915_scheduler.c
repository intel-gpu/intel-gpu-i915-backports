/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2018 Intel Corporation
 */

#include <linux/mutex.h>
#include <linux/tick.h>

#include "i915_drv.h"
#include "i915_request.h"
#include "i915_scheduler.h"

static struct kmem_cache *slab_dependencies;
static struct kmem_cache *slab_priorities;

/*
 * Virtual engines complicate acquiring the engine timeline lock,
 * as their rq->engine pointer is not stable until under that
 * engine lock. The simple ploy we use is to take the lock then
 * check that the rq still belongs to the newly locked engine.
 */
#define se_lock_irqsave(rq, flags) ({ \
	struct i915_request * const rq__ = (rq); \
	struct i915_sched_engine *se__ = READ_ONCE(rq__->sched_engine); \
	spin_lock_irqsave(&se__->lock, (flags)); \
	while (unlikely(se__ != READ_ONCE((rq__)->sched_engine))) { \
		spin_unlock(&se__->lock); \
		se__ = READ_ONCE(rq__->sched_engine); \
		spin_lock(&se__->lock); \
	} \
	se__; \
})

static inline int rq_prio(const struct i915_request *rq)
{
	return READ_ONCE(rq->sched.attr.priority);
}

static inline bool rq_set_prio(struct i915_request *rq, int prio)
{
	int old = rq_prio(rq);

	GEM_BUG_ON(!rq->sched.dfs.prev);

	do {
		if (unlikely(old >= prio))
			return false;
	} while (!try_cmpxchg(&rq->sched.attr.priority, &old, prio));

	return true;
}

static struct i915_sched_node *node_get(struct i915_sched_node *node)
{
	i915_request_get(container_of(node, struct i915_request, sched));
	return node;
}

static void node_put(struct i915_sched_node *node)
{
	i915_request_put(container_of(node, struct i915_request, sched));
}

static struct i915_request *
__node_to_request(struct i915_sched_node *node)
{
	return container_of(node, struct i915_request, sched);
}

static const struct i915_request *
node_to_request(const struct i915_sched_node *node)
{
	return container_of(node, const struct i915_request, sched);
}

static inline bool node_signaled(const struct i915_sched_node *node)
{
	return i915_request_signaled(node_to_request(node));
}

static inline struct i915_priolist *to_priolist(struct rb_node *rb)
{
	return rb_entry(rb, struct i915_priolist, node);
}

static void assert_priolists(struct i915_sched_engine * const sched_engine)
{
	struct rb_node *rb;
	long last_prio;

	if (!IS_ENABLED(CPTCFG_DRM_I915_DEBUG_GEM))
		return;

	GEM_BUG_ON(rb_first_cached(&sched_engine->queue) !=
		   rb_first(&sched_engine->queue.rb_root));

	last_prio = INT_MAX;
	for (rb = rb_first_cached(&sched_engine->queue); rb; rb = rb_next(rb)) {
		const struct i915_priolist *p = to_priolist(rb);

		GEM_BUG_ON(p->priority > last_prio);
		last_prio = p->priority;
	}
}

struct list_head *
i915_sched_lookup_priolist(struct i915_sched_engine *sched_engine, int prio)
{
	struct i915_priolist *p;
	struct rb_node **parent, *rb;
	bool first = true;

	lockdep_assert_held(&sched_engine->lock);
	assert_priolists(sched_engine);

	if (unlikely(sched_engine->no_priolist))
		prio = I915_PRIORITY_NORMAL;

find_priolist:
	/* most positive priority is scheduled first, equal priorities fifo */
	rb = NULL;
	parent = &sched_engine->queue.rb_root.rb_node;
	while (*parent) {
		rb = *parent;
		p = to_priolist(rb);
		if (prio > p->priority) {
			parent = &rb->rb_left;
		} else if (prio < p->priority) {
			parent = &rb->rb_right;
			first = false;
		} else {
			return &p->requests;
		}
	}

	if (prio == I915_PRIORITY_NORMAL) {
		p = &sched_engine->default_priolist;
	} else {
		p = kmem_cache_alloc(slab_priorities, GFP_ATOMIC);
		/* Convert an allocation failure to a priority bump */
		if (unlikely(!p)) {
			prio = I915_PRIORITY_NORMAL; /* recurses just once */

			/* To maintain ordering with all rendering, after an
			 * allocation failure we have to disable all scheduling.
			 * Requests will then be executed in fifo, and schedule
			 * will ensure that dependencies are emitted in fifo.
			 * There will be still some reordering with existing
			 * requests, so if userspace lied about their
			 * dependencies that reordering may be visible.
			 */
			sched_engine->no_priolist = true;
			goto find_priolist;
		}
	}

	p->priority = prio;
	INIT_LIST_HEAD(&p->requests);

	rb_link_node(&p->node, rb, parent);
	rb_insert_color_cached(&p->node, &sched_engine->queue, first);

	return &p->requests;
}

void __i915_priolist_free(struct i915_priolist *p)
{
	kmem_cache_free(slab_priorities, p);
}

static struct i915_request *
stack_push(struct i915_request *rq,
	   struct i915_request *prev,
	   struct list_head *pos)
{
	while (cmpxchg(&rq->sched.dfs.prev, NULL, prev))
		cpu_relax();
	WRITE_ONCE(rq->sched.dfs.pos, pos);
	return rq;
}

static struct i915_request *
stack_pop(struct i915_request *rq,
	  struct list_head **pos)
{
	*pos = READ_ONCE(rq->sched.dfs.pos);
	return xchg(&rq->sched.dfs.prev, NULL);
}

static bool
stack_prio(const struct i915_request *rq, int prio)
{
	return rq_prio(rq) >= prio && READ_ONCE(rq->sched.dfs.prev) == NULL;
}

static void __i915_request_set_priority(struct i915_request *rq, int prio)
{
	struct list_head *pos = &rq->sched.signalers_list;

	/*
	 * Recursively bump all dependent priorities to match the new request.
	 *
	 * A naive approach would be to use recursion:
	 * static void update_priorities(struct i915_sched_node *node, prio) {
	 *	list_for_each_entry(dep, &node->signalers_list, signal_link)
	 *		update_priorities(dep->signal, prio)
	 *	queue_request(node);
	 * }
	 * but that may have unlimited recursion depth and so runs a very
	 * real risk of overunning the kernel stack. Instead, we build
	 * a flat list of all dependencies starting with the current request.
	 * As we walk the list of dependencies, we add all of its dependencies
	 * to the end of the list (this may include an already visited
	 * request) and continue to walk onwards onto the new dependencies. The
	 * end result is a topological list of requests in reverse order, the
	 * last element in the list is the request we must execute first.
	 */
	stack_push(rq, ERR_PTR(-1), NULL);
	do {
		struct i915_sched_engine *se;
		unsigned long flags;

		list_for_each_continue(pos, &rq->sched.signalers_list) {
			struct i915_dependency *p =
				list_entry_rcu(pos, typeof(*p), signal_link);
			struct i915_request *s =
				container_of(p->signaler, typeof(*s), sched);

			if (!(s->sched.flags & I915_SCHED_HAS_PHYSICAL_CHAIN))
				continue;

			if (i915_request_signaled(s))
				continue;

			if (stack_prio(s, prio))
				continue;

			/* Remember our position along this branch */
			rq = stack_push(s, rq, pos);
			pos = &rq->sched.signalers_list;
		}

		/* depth-first update to priority */
		if (unlikely(!rq_set_prio(rq, prio)))
			continue;

		if (!rq->engine || !i915_sw_fence_signaled(&rq->submit))
			continue;

		se = se_lock_irqsave(rq, flags);
		RQ_TRACE(rq, "set-priority:%d\n", prio);
		GEM_BUG_ON(rq_prio(rq) != prio);

		/*
		 * Once the request is ready, it will be placed into the
		 * priority lists and then onto the HW runlist. Before the
		 * request is ready, it does not contribute to our preemption
		 * decisions and we can safely ignore it, as it will, and
		 * any preemption required, be dealt with upon submission.
		 * See engine->submit_request()
		 */
		if (!i915_request_is_ready(rq))
			goto unlock;

		if (se->bump_inflight_request_prio)
			se->bump_inflight_request_prio(rq, prio);

		if (!i915_request_in_priority_queue(rq))
			goto unlock;

		/* Defer (tasklet) submission until after all updates. */
		list_move_tail(&rq->sched.link, i915_sched_lookup_priolist(se, prio));
		if (se->kick_backend)
			se->kick_backend(rq, prio);

unlock:
		spin_unlock_irqrestore(&se->lock, flags);
	} while ((rq = stack_pop(rq, &pos)) != ERR_PTR(-1));
}

void i915_request_set_priority(struct i915_request *rq, int prio)
{
	if (prio <= rq_prio(rq))
		return;

	if (!(rq->sched.flags & I915_SCHED_HAS_PHYSICAL_CHAIN))
		return;

	rcu_read_lock();
	__i915_request_set_priority(rq, prio);
	GEM_BUG_ON(rq_prio(rq) < prio);
	rcu_read_unlock();
}

void i915_sched_node_init(struct i915_sched_node *node)
{
	spin_lock_init(&node->lock);

	INIT_LIST_HEAD(&node->signalers_list);
	INIT_LIST_HEAD(&node->waiters_list);
	INIT_LIST_HEAD(&node->link);

	node->dfs.prev = NULL;

	i915_sched_node_reinit(node);
}

void i915_sched_node_reinit(struct i915_sched_node *node)
{
	node->attr.priority = I915_PRIORITY_INVALID;
	node->semaphores = 0;
	node->flags = 0;

	GEM_BUG_ON(!list_empty(&node->signalers_list));
	GEM_BUG_ON(!list_empty(&node->waiters_list));
	GEM_BUG_ON(!list_empty(&node->link));
}

static struct i915_dependency *
i915_dependency_alloc(void)
{
	return kmem_cache_alloc(slab_dependencies, GFP_KERNEL);
}

static void
rcu_dependency_free(struct rcu_head *rcu)
{
	kmem_cache_free(slab_dependencies,
			container_of(rcu, typeof(struct i915_dependency), rcu));
}

static void
i915_dependency_free(struct i915_dependency *dep)
{
	call_rcu(&dep->rcu, rcu_dependency_free);
}

bool __i915_sched_node_add_dependency(struct i915_sched_node *node,
				      struct i915_sched_node *signal,
				      struct i915_dependency *dep,
				      unsigned long flags)
{
	int prio = I915_PRIORITY_INVALID;
	bool ret = false;

	if (!(signal->flags & I915_SCHED_HAS_PHYSICAL_CHAIN))
		return false;

	/* The signal->lock is always the outer lock in this double-lock. */
	spin_lock_irq(&signal->lock);

	if (!node_signaled(signal)) {
		dep->signaler = signal;
		dep->waiter = node_get(node);
		dep->flags = flags;

		if (node->attr.priority > READ_ONCE(signal->attr.priority))
			prio = node->attr.priority;

		/* All set, now publish. Beware the lockless walkers. */
		spin_lock_nested(&node->lock, SINGLE_DEPTH_NESTING);
		list_add_rcu(&dep->signal_link, &node->signalers_list);
		list_add_rcu(&dep->wait_link, &signal->waiters_list);
		spin_unlock(&node->lock);

		/* Propagate the chains */
		node->flags |= signal->flags;
		ret = true;
	}

	spin_unlock_irq(&signal->lock);

	if (prio != I915_PRIORITY_INVALID)
		i915_request_set_priority(__node_to_request(signal), prio);

	return ret;
}

int i915_sched_node_add_dependency(struct i915_sched_node *node,
				   struct i915_sched_node *signal,
				   unsigned long flags)
{
	struct i915_dependency *dep;

	if (!(signal->flags & I915_SCHED_HAS_PHYSICAL_CHAIN))
		return 0;

	dep = i915_dependency_alloc();
	if (!dep)
		return -ENOMEM;

	if (!__i915_sched_node_add_dependency(node, signal, dep,
					      flags | I915_DEPENDENCY_ALLOC))
		i915_dependency_free(dep);

	return 0;
}

void i915_sched_node_retire(struct i915_sched_node *node)
{
	struct i915_dependency *dep, *tmp;

	if (list_empty(&node->waiters_list))
		return;

	/*
	 * Everyone we depended upon (the fences we wait to be signaled)
	 * should retire before us and remove themselves from our list.
	 * However, retirement is run independently on each timeline and
	 * so we may be called out-of-order. As we need to avoid taking
	 * the signaler's lock, just mark up our completion and be wary
	 * in traversing the signalers->waiters_list.
	 */

	/* Remove ourselves from everyone who depends upon us */
	list_for_each_entry_safe(dep, tmp, &node->waiters_list, wait_link) {
		struct i915_sched_node *w = dep->waiter;

		GEM_BUG_ON(dep->signaler != node);

		spin_lock_irq(&w->lock);
		list_del_rcu(&dep->signal_link);
		spin_unlock_irq(&w->lock);

		if (dep->flags & I915_DEPENDENCY_ALLOC)
			i915_dependency_free(dep);

		node_put(w);
	}
	INIT_LIST_HEAD_RCU(&node->waiters_list);
}

void i915_request_show_with_schedule(struct drm_printer *m,
				     const struct i915_request *rq,
				     const char *prefix,
				     int indent)
{
	struct i915_dependency *dep;

	i915_request_show(m, rq, prefix, indent);
	if (i915_request_completed(rq))
		return;

	rcu_read_lock();
	for_each_signaler(dep, rq) {
		const struct i915_request *signaler =
			node_to_request(dep->signaler);

		/* Dependencies along the same timeline are expected. */
		if (signaler->timeline == rq->timeline)
			continue;

		if (i915_request_signaled(signaler))
			continue;

		i915_request_show(m, signaler, prefix, indent + 2);
	}
	rcu_read_unlock();
}

static void default_destroy(struct kref *kref)
{
	struct i915_sched_engine *se =
		container_of(kref, typeof(*se), ref);
	struct i915_priolist *pos, *n;

	tasklet_kill(&se->tasklet); /* flush the callback */

	rbtree_postorder_for_each_entry_safe(pos, n, &se->queue.rb_root, node)
		i915_priolist_free(pos);

	kfree(se);
}

static bool default_disabled(struct i915_sched_engine *sched_engine)
{
	return false;
}

struct i915_sched_engine *
i915_sched_engine_create(unsigned int subclass)
{
	struct i915_sched_engine *sched_engine;

	sched_engine = kzalloc(sizeof(*sched_engine), GFP_KERNEL);
	if (!sched_engine)
		return NULL;

	kref_init(&sched_engine->ref);

	sched_engine->queue = RB_ROOT_CACHED;
	sched_engine->queue_priority_hint = INT_MIN;
	sched_engine->destroy = default_destroy;
	sched_engine->disabled = default_disabled;

	INIT_LIST_HEAD(&sched_engine->requests);
	INIT_LIST_HEAD(&sched_engine->hold);

	spin_lock_init(&sched_engine->lock);
	lockdep_set_subclass(&sched_engine->lock, subclass);
	mark_lock_used_irq(&sched_engine->lock);

	return sched_engine;
}

void i915_scheduler_module_exit(void)
{
	kmem_cache_destroy(slab_dependencies);
	kmem_cache_destroy(slab_priorities);
}

int __init i915_scheduler_module_init(void)
{
	slab_dependencies = KMEM_CACHE(i915_dependency, SLAB_HWCACHE_ALIGN);
	if (!slab_dependencies)
		return -ENOMEM;

	slab_priorities = KMEM_CACHE(i915_priolist, 0);
	if (!slab_priorities)
		goto err_priorities;

	return 0;

err_priorities:
	kmem_cache_destroy(slab_priorities);
	return -ENOMEM;
}
