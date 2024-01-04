#ifndef __BACKPORT_MMAP_LOCK_H
#define __BACKPORT_MMAP_LOCK_H
#include <linux/version.h>

#ifndef BPM_MMAP_WRITE_LOCK_NOT_PRESENT
#include_next<linux/mmap_lock.h>
#endif

#ifdef BPM_MMAP_WRITE_LOCK_NOT_PRESENT
static inline void mmap_write_lock(struct mm_struct *mm)
{
       down_write(&mm->mmap_sem);
}

static inline void mmap_read_lock(struct mm_struct *mm)
{
       down_read(&mm->mmap_sem);
}

static inline void mmap_write_unlock(struct mm_struct *mm)
{
       up_write(&mm->mmap_sem);
}

static inline void mmap_read_unlock(struct mm_struct *mm)
{
       up_read(&mm->mmap_sem);
}

static inline int mmap_write_lock_killable(struct mm_struct *mm)
{
       return down_write_killable(&mm->mmap_sem);
}

static inline bool mmap_read_trylock(struct mm_struct *mm)
{
       return down_read_trylock(&mm->mmap_sem) != 0;
}
#endif

#ifdef BPM_MMAP_ASSERT_LOCKED_NOT_PRESENT
static inline void mmap_assert_locked(struct mm_struct *mm)
{
               lockdep_assert_held(&mm->mmap_sem);
                       VM_BUG_ON_MM(!rwsem_is_locked(&mm->mmap_sem), mm);
}

#endif

#endif /* __BACKPORT_MMAP_LOCK_H */
