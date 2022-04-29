#ifndef __BACKPORT_MMU_NOTIFIER_H
#define __BACKPORT_MMU_NOTIFIER_H
#include <linux/version.h>
#include_next <linux/mmu_notifier.h>

#if LINUX_VERSION_IS_LESS(5,2,0)
static inline bool
mmu_notifier_range_blockable(const struct mmu_notifier_range *range)
{
	return range->blockable;
}
#endif

#endif /* __BACKPORT_MMU_NOTIFIER_H */
