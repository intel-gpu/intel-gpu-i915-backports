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
#endif

#endif /* __BACKPORT_MMAP_LOCK_H */
