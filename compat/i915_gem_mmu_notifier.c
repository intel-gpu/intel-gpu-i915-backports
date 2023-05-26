#include <linux/overflow.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <drm/drm_print.h>
#include <linux/slab.h>

#ifdef BPM_MMU_INTERVAL_NOTIFIER_NOTIFIER_NOT_PRESENT
#include <linux/i915_gem_mmu_notifier.h>

static bool
mn_itree_is_invalidating(struct mmu_notifier_subscriptions *subscriptions)
{
	lockdep_assert_held(&subscriptions->lock);
	return subscriptions->invalidate_seq & 1;
}

static struct mmu_interval_notifier *
mn_itree_inv_start_range(struct mmu_notifier_subscriptions *subscriptions,
			 const struct mmu_notifier_range *range,
			 unsigned long *seq)
{
	struct interval_tree_node *node;
	struct mmu_interval_notifier *res = NULL;

	spin_lock(&subscriptions->lock);
	subscriptions->active_invalidate_ranges++;
	node = interval_tree_iter_first(&subscriptions->itree, range->start,
					range->end - 1);
	if (node) {
		subscriptions->invalidate_seq |= 1;
		res = container_of(node, struct mmu_interval_notifier,
				   interval_tree);
	}

	*seq = subscriptions->invalidate_seq;
	spin_unlock(&subscriptions->lock);
	return res;
}

static struct mmu_interval_notifier *
mn_itree_inv_next(struct mmu_interval_notifier *interval_sub,
		  const struct mmu_notifier_range *range)
{
	struct interval_tree_node *node;

	node = interval_tree_iter_next(&interval_sub->interval_tree,
				       range->start, range->end - 1);
	if (!node)
		return NULL;
	return container_of(node, struct mmu_interval_notifier, interval_tree);
}

static void mn_itree_inv_end(struct mmu_notifier_subscriptions *subscriptions)
{
	struct mmu_interval_notifier *interval_sub;
	struct hlist_node *next;

	spin_lock(&subscriptions->lock);
	if (--subscriptions->active_invalidate_ranges ||
	    !mn_itree_is_invalidating(subscriptions)) {
		spin_unlock(&subscriptions->lock);
		return;
	}

	/* Make invalidate_seq even */
	subscriptions->invalidate_seq++;

	/*
	 * The inv_end incorporates a deferred mechanism like rtnl_unlock().
	 * Adds and removes are queued until the final inv_end happens then
	 * they are progressed. This arrangement for tree updates is used to
	 * avoid using a blocking lock during invalidate_range_start.
	 */
	hlist_for_each_entry_safe(interval_sub, next,
				  &subscriptions->deferred_list,
				  deferred_item) {
		if (RB_EMPTY_NODE(&interval_sub->interval_tree.rb)) {
			interval_tree_insert(&interval_sub->interval_tree,
					     &subscriptions->itree);
		} else {
			interval_tree_remove(&interval_sub->interval_tree,
					     &subscriptions->itree);
		}
		hlist_del(&interval_sub->deferred_item);
	}
	spin_unlock(&subscriptions->lock);

	wake_up_all(&subscriptions->wq);
}

unsigned long
mmu_interval_read_begin(struct mmu_interval_notifier *interval_sub)
{
	struct mmu_notifier_subscriptions *subscriptions =
		interval_sub->i915_mm->notifier_subscriptions;
	unsigned long seq;
	bool is_invalidating;

	/*
	 * If the subscription has a different seq value under the user_lock
	 * than we started with then it has collided.
	 *
	 * If the subscription currently has the same seq value as the
	 * subscriptions seq, then it is currently between
	 * invalidate_start/end and is colliding.
	 *
	 * The locking looks broadly like this:
	 *   mn_tree_invalidate_start():          mmu_interval_read_begin():
	 *                                         spin_lock
	 *                                          seq = READ_ONCE(interval_sub->invalidate_seq);
	 *                                          seq == subs->invalidate_seq
	 *                                         spin_unlock
	 *    spin_lock
	 *     seq = ++subscriptions->invalidate_seq
	 *    spin_unlock
	 *     op->invalidate_range():
	 *       user_lock
	 *        mmu_interval_set_seq()
	 *         interval_sub->invalidate_seq = seq
	 *       user_unlock
	 *
	 *                          [Required: mmu_interval_read_retry() == true]
	 *
	 *   mn_itree_inv_end():
	 *    spin_lock
	 *     seq = ++subscriptions->invalidate_seq
	 *    spin_unlock
	 *
	 *                                        user_lock
	 *                                         mmu_interval_read_retry():
	 *                                          interval_sub->invalidate_seq != seq
	 *                                        user_unlock
	 *
	 * Barriers are not needed here as any races here are closed by an
	 * eventual mmu_interval_read_retry(), which provides a barrier via the
	 * user_lock.
	 */
	spin_lock(&subscriptions->lock);
	/* Pairs with the WRITE_ONCE in mmu_interval_set_seq() */
	seq = READ_ONCE(interval_sub->invalidate_seq);
	is_invalidating = seq == subscriptions->invalidate_seq;
	spin_unlock(&subscriptions->lock);

	/*
	 * interval_sub->invalidate_seq must always be set to an odd value via
	 * mmu_interval_set_seq() using the provided cur_seq from
	 * mn_itree_inv_start_range(). This ensures that if seq does wrap we
	 * will always clear the below sleep in some reasonable time as
	 * subscriptions->invalidate_seq is even in the idle state.
	 */
	lock_map_acquire(&__mmu_notifier_invalidate_range_start_map);
	lock_map_release(&__mmu_notifier_invalidate_range_start_map);
	if (is_invalidating)
		wait_event(subscriptions->wq,
			   READ_ONCE(subscriptions->invalidate_seq) != seq);

	/*
	 * Notice that mmu_interval_read_retry() can already be true at this
	 * point, avoiding loops here allows the caller to provide a global
	 * time bound.
	 */

	return seq;
}
EXPORT_SYMBOL_GPL(mmu_interval_read_begin);

int mn_itree_invalidate(struct mmu_notifier_subscriptions *subscriptions,
			const struct mmu_notifier_range *range)
{
	struct mmu_interval_notifier *interval_sub;
	unsigned long cur_seq;

	for (interval_sub =
		     mn_itree_inv_start_range(subscriptions, range, &cur_seq);
	     interval_sub;
	     interval_sub = mn_itree_inv_next(interval_sub, range)) {
		bool ret;

		ret = interval_sub->ops->invalidate(interval_sub, range,
						    cur_seq);
		if (!ret) {
			if (WARN_ON(mmu_notifier_range_blockable(range)))
				continue;
			goto out_would_block;
		}
	}
	return 0;

out_would_block:
	/*
	 * On -EAGAIN the non-blocking caller is not allowed to call
	 * invalidate_range_end()
	 */
	mn_itree_inv_end(subscriptions);
	return -EAGAIN;
}
EXPORT_SYMBOL_GPL(mn_itree_invalidate);

void mn_itree_invalidate_end(struct mmu_notifier_subscriptions *subscriptions)
{
	lock_map_acquire(&__mmu_notifier_invalidate_range_start_map);
	if (subscriptions->has_itree)
		mn_itree_inv_end(subscriptions);

	lock_map_release(&__mmu_notifier_invalidate_range_start_map);
}
EXPORT_SYMBOL_GPL(mn_itree_invalidate_end);

static int __mmu_notifier_subscriptions_init(struct i915_mm_struct *i915_mm)
{
	struct mmu_notifier_subscriptions *subscriptions = NULL;
	struct mm_struct *mm = i915_mm->mm;
//	int ret;

//	mmap_assert_write_locked(mm);
	BUG_ON(atomic_read(&mm->mm_users) <= 0);

	if (!i915_mm->notifier_subscriptions) {
		/*
		 * kmalloc cannot be called under mm_take_all_locks(), but we
		 * know that mm->notifier_subscriptions can't change while we
		 * hold the write side of the mmap_lock.
		 */
		subscriptions = kzalloc(
			sizeof(struct mmu_notifier_subscriptions), GFP_KERNEL);
		if (!subscriptions)
			return -ENOMEM;

		spin_lock_init(&subscriptions->lock);
		subscriptions->invalidate_seq = 2;
		subscriptions->itree = RB_ROOT_CACHED;
		init_waitqueue_head(&subscriptions->wq);
		INIT_HLIST_HEAD(&subscriptions->deferred_list);
	}

//	ret = mm_take_all_locks(mm);
//	if (unlikely(ret))
//		goto out_clean;

	/*
	 * Serialize the update against mmu_notifier_unregister. A
	 * side note: mmu_notifier_release can't run concurrently with
	 * us because we hold the mm_users pin (either implicitly as
	 * current->mm or explicitly with get_task_mm() or similar).
	 * We can't race against any other mmu notifier method either
	 * thanks to mm_take_all_locks().
	 *
	 * release semantics on the initialization of the
	 * mmu_notifier_subscriptions's contents are provided for unlocked
	 * readers.  acquire can only be used while holding the mmgrab or
	 * mmget, and is safe because once created the
	 * mmu_notifier_subscriptions is not freed until the mm is destroyed.
	 * As above, users holding the mmap_lock or one of the
	 * mm_take_all_locks() do not need to use acquire semantics.
	 */
	if (subscriptions)
		smp_store_release(&i915_mm->notifier_subscriptions, subscriptions);

	i915_mm->notifier_subscriptions->has_itree = true;
//	mm_drop_all_locks(mm);
//	BUG_ON(atomic_read(&mm->mm_users) <= 0);
	return 0;

//out_clean:
//	kfree(subscriptions);
//	return ret;
}

int mmu_notifier_subscriptions_init(struct i915_mm_struct *i915_mm)
{
	int ret;

	down_write(&i915_mm->mm->mmap_sem);
	ret = __mmu_notifier_subscriptions_init(i915_mm);
	up_write(&i915_mm->mm->mmap_sem);
	return ret;
}
EXPORT_SYMBOL_GPL(mmu_notifier_subscriptions_init);

static void __mn_itree_release(struct mmu_notifier_subscriptions *subscriptions,
			       struct mm_struct *mm)
{
	struct mmu_notifier_range range = {
		.flags = MMU_NOTIFIER_RANGE_BLOCKABLE,
		.event = MMU_NOTIFY_CLEAR, //MMU_NOTIFY_RELEASE,
		.mm = mm,
		.start = 0,
		.end = ULONG_MAX,
	};
	struct mmu_interval_notifier *interval_sub;
	unsigned long cur_seq;
	bool ret;

	for (interval_sub =
		     mn_itree_inv_start_range(subscriptions, &range, &cur_seq);
	     interval_sub;
	     interval_sub = mn_itree_inv_next(interval_sub, &range)) {
		ret = interval_sub->ops->invalidate(interval_sub, &range,
						    cur_seq);
		WARN_ON(!ret);
	}

	mn_itree_inv_end(subscriptions);
}

void mn_itree_release(struct mmu_notifier_subscriptions *subscriptions,
		      struct i915_mm_struct *i915_mm)
{
	if (subscriptions->has_itree)
		__mn_itree_release(subscriptions, i915_mm->mm);
}
EXPORT_SYMBOL_GPL(mn_itree_release);

void __mmu_notifier_subscriptions_destroy(struct i915_mm_struct *i915_mm)
{
	kfree(i915_mm->notifier_subscriptions);
	i915_mm->notifier_subscriptions = LIST_POISON1; /* debug */
}
EXPORT_SYMBOL_GPL(__mmu_notifier_subscriptions_destroy);

static int __mmu_interval_notifier_insert(
	struct mmu_interval_notifier *interval_sub, struct mm_struct *mm,
	struct mmu_notifier_subscriptions *subscriptions, unsigned long start,
	unsigned long length, const struct mmu_interval_notifier_ops *ops)
{
	interval_sub->mm = mm;
	interval_sub->ops = ops;
	RB_CLEAR_NODE(&interval_sub->interval_tree.rb);
	interval_sub->interval_tree.start = start;
	/*
	 * Note that the representation of the intervals in the interval tree
	 * considers the ending point as contained in the interval.
	 */
	if (length == 0 ||
	    check_add_overflow(start, length - 1,
			       &interval_sub->interval_tree.last))
		return -EOVERFLOW;

	/* Must call with a mmget() held */
	if (WARN_ON(atomic_read(&mm->mm_users) <= 0))
		return -EINVAL;

	/* pairs with mmdrop in mmu_interval_notifier_remove() */
	mmgrab(mm);

	/*
	 * If some invalidate_range_start/end region is going on in parallel
	 * we don't know what VA ranges are affected, so we must assume this
	 * new range is included.
	 *
	 * If the itree is invalidating then we are not allowed to change
	 * it. Retrying until invalidation is done is tricky due to the
	 * possibility for live lock, instead defer the add to
	 * mn_itree_inv_end() so this algorithm is deterministic.
	 *
	 * In all cases the value for the interval_sub->invalidate_seq should be
	 * odd, see mmu_interval_read_begin()
	 */
	spin_lock(&subscriptions->lock);
	if (subscriptions->active_invalidate_ranges) {
		if (mn_itree_is_invalidating(subscriptions))
			hlist_add_head(&interval_sub->deferred_item,
				       &subscriptions->deferred_list);
		else {
			subscriptions->invalidate_seq |= 1;
			interval_tree_insert(&interval_sub->interval_tree,
					     &subscriptions->itree);
		}
		interval_sub->invalidate_seq = subscriptions->invalidate_seq;
	} else {
		WARN_ON(mn_itree_is_invalidating(subscriptions));
		/*
		 * The starting seq for a subscription not under invalidation
		 * should be odd, not equal to the current invalidate_seq and
		 * invalidate_seq should not 'wrap' to the new seq any time
		 * soon.
		 */
		interval_sub->invalidate_seq =
			subscriptions->invalidate_seq - 1;
		interval_tree_insert(&interval_sub->interval_tree,
				     &subscriptions->itree);
	}
	spin_unlock(&subscriptions->lock);
	return 0;
}

int mmu_interval_notifier_insert(struct mmu_interval_notifier *interval_sub,
				 struct i915_mm_struct *i915_mm, unsigned long start,
				 unsigned long length,
				 const struct mmu_interval_notifier_ops *ops)
{
	struct mmu_notifier_subscriptions *subscriptions;
	struct mm_struct *mm = i915_mm->mm;
	int ret;

	might_lock(&mm->mmap_lock);

	interval_sub->i915_mm = i915_mm;
	subscriptions = smp_load_acquire(&i915_mm->notifier_subscriptions);
	if (!subscriptions || !subscriptions->has_itree) {
		ret = mmu_notifier_register(NULL, mm);
		if (ret)
			return ret;
		subscriptions = i915_mm->notifier_subscriptions;
	}

	return __mmu_interval_notifier_insert(interval_sub, mm, subscriptions,
					      start, length, ops);
}
EXPORT_SYMBOL_GPL(mmu_interval_notifier_insert);

void mmu_interval_notifier_remove(struct mmu_interval_notifier *interval_sub)
{
	struct mm_struct *mm;
	struct i915_mm_struct *i915_mm;
	struct mmu_notifier_subscriptions *subscriptions;
	unsigned long seq = 0;

	might_sleep();

	/* TODO: Check what leads to this unexpected remove call */
	if (!interval_sub || !interval_sub->mm || !interval_sub->i915_mm)
		return;

	mm = interval_sub->mm;
	i915_mm = interval_sub->i915_mm;
	subscriptions = i915_mm->notifier_subscriptions;

	spin_lock(&subscriptions->lock);
	if (mn_itree_is_invalidating(subscriptions)) {
		/*
		 * remove is being called after insert put this on the
		 * deferred list, but before the deferred list was processed.
		 */
		if (RB_EMPTY_NODE(&interval_sub->interval_tree.rb)) {
			hlist_del(&interval_sub->deferred_item);
		} else {
			hlist_add_head(&interval_sub->deferred_item,
				       &subscriptions->deferred_list);
			seq = subscriptions->invalidate_seq;
		}
	} else {
		WARN_ON(RB_EMPTY_NODE(&interval_sub->interval_tree.rb));
		interval_tree_remove(&interval_sub->interval_tree,
				     &subscriptions->itree);
	}
	spin_unlock(&subscriptions->lock);

	/*
	 * The possible sleep on progress in the invalidation requires the
	 * caller not hold any locks held by invalidation callbacks.
	 */
	lock_map_acquire(&__mmu_notifier_invalidate_range_start_map);
	lock_map_release(&__mmu_notifier_invalidate_range_start_map);
	if (seq)
		wait_event(subscriptions->wq,
			   READ_ONCE(subscriptions->invalidate_seq) != seq);

	/* pairs with mmgrab in mmu_interval_notifier_insert() */
	mmdrop(mm);
}
EXPORT_SYMBOL_GPL(mmu_interval_notifier_remove);
#endif /* BPM_MMU_INTERVAL_NOTIFIER_NOTIFIER_NOT_PRESENT */
