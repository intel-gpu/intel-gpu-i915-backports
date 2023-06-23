#ifndef _BACKPORT_LINUX_MMAP_LOCK_H
#define _BACKPORT_LINUX_MMAP_LOCK_H
#include_next <linux/mmap_lock.h>

#ifdef BPM_MMAP_ASSERT_LOCKED_NOT_PRESENT
static inline void mmap_assert_locked(struct mm_struct *mm)
{
		lockdep_assert_held(&mm->mmap_sem);
			VM_BUG_ON_MM(!rwsem_is_locked(&mm->mmap_sem), mm);
}

#endif

#endif /* _BACKPORT_LINUX_MMAP_LOCK_H */
