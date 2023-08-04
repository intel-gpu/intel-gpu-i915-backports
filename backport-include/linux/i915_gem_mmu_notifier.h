#ifndef __BACKPORT_I915_GEM_MMU_NOTIFIER_H
#define __BACKPORT_I915_GEM_MMU_NOTIFIER_H

#ifdef BPM_MMU_INTERVAL_NOTIFIER_NOTIFIER_NOT_PRESENT
#include <linux/interval_tree.h>
#include <linux/kref.h>
#include <linux/mmu_notifier.h>

#ifdef BPM_MMU_NOTIFIER_EVENT_NOT_PRESENT
enum mmu_notifier_event {
        MMU_NOTIFY_UNMAP = 0,
        MMU_NOTIFY_CLEAR,
        MMU_NOTIFY_PROTECTION_VMA,
        MMU_NOTIFY_PROTECTION_PAGE,
        MMU_NOTIFY_SOFT_DIRTY,
        MMU_NOTIFY_RELEASE,
};
#define MMU_NOTIFIER_RANGE_BLOCKABLE (1 << 0)
#define mmu_notifier_range LINUX_I915_BACKPORT(mmu_notifier_range)
struct mmu_notifier_range {
        struct mm_struct *mm;
        unsigned long start;
        unsigned long end;
        unsigned flags;
        enum mmu_notifier_event event;
};

#define mmu_notifier_range_blockable LINUX_I915_BACKPORT(mmu_notifier_range_blockable)
static inline bool
mmu_notifier_range_blockable(const struct mmu_notifier_range *range)
{
        return true;
}
#endif

struct mmu_notifier_subscriptions {
	/* all mmu notifiers registered in this mm are queued in this list */
	struct hlist_head list;
	bool has_itree;
	/* to serialize the list modifications and hlist_unhashed */
	spinlock_t lock;
	unsigned long invalidate_seq;
	unsigned long active_invalidate_ranges;
	struct rb_root_cached itree;
	wait_queue_head_t wq;
	struct hlist_head deferred_list;
};

struct i915_mm_struct {
	struct mm_struct *mm;
	struct drm_i915_private *i915;
	struct i915_mmu_notifier *mn;
	struct hlist_node node;
	struct kref kref;
	struct rcu_work work;
	struct mmu_notifier_subscriptions *notifier_subscriptions;
};

struct mmu_interval_notifier {
	struct interval_tree_node interval_tree;
	const struct mmu_interval_notifier_ops *ops;
	struct mm_struct *mm;
	struct i915_mm_struct *i915_mm;
	struct hlist_node deferred_item;
	unsigned long invalidate_seq;
};

struct mmu_interval_notifier_ops {
	bool (*invalidate)(struct mmu_interval_notifier *interval_sub,
			   const struct mmu_notifier_range *range,
			   unsigned long cur_seq);
};

int mmu_notifier_subscriptions_init(struct i915_mm_struct *i915_mm);
void __mmu_notifier_subscriptions_destroy(struct i915_mm_struct *i915_mm);
static inline void mmu_notifier_subscriptions_destroy(struct i915_mm_struct *i915_mm)
{
	if (i915_mm->notifier_subscriptions)
		__mmu_notifier_subscriptions_destroy(i915_mm);
}


int mn_itree_invalidate(struct mmu_notifier_subscriptions *subscriptions,
			const struct mmu_notifier_range *range);
void mn_itree_invalidate_end(struct mmu_notifier_subscriptions *subscriptions);
void mn_itree_release(struct mmu_notifier_subscriptions *subscriptions,
		      struct i915_mm_struct *i915_mm);

unsigned long
mmu_interval_read_begin(struct mmu_interval_notifier *interval_sub);

int mmu_interval_notifier_insert(struct mmu_interval_notifier *interval_sub,
				 struct i915_mm_struct *i915_mm, unsigned long start,
				 unsigned long length,
				 const struct mmu_interval_notifier_ops *ops);

void mmu_interval_notifier_remove(struct mmu_interval_notifier *interval_sub);

static inline void
mmu_interval_set_seq(struct mmu_interval_notifier *interval_sub,
		     unsigned long cur_seq)
{
	WRITE_ONCE(interval_sub->invalidate_seq, cur_seq);
}

static inline bool
mmu_interval_check_retry(struct mmu_interval_notifier *interval_sub,
			 unsigned long seq)
{
	/* Pairs with the WRITE_ONCE in mmu_interval_set_seq() */
	return READ_ONCE(interval_sub->invalidate_seq) != seq;
}

static inline bool
mmu_interval_read_retry(struct mmu_interval_notifier *interval_sub,
			unsigned long seq)
{
	return interval_sub->invalidate_seq != seq;
}
#endif /* BPM_MMU_INTERVAL_NOTIFIER_NOTIFIER_NOT_PRESENT */
#endif /*__BACKPORT_I915_GEM_MMU_NOTIFIER_H */
