/* SPDX-License-Identifier: GPL-2.0 */
/*
 * seqcount_t / seqlock_t - a reader-writer consistency mechanism with
 * lockless readers (read-only retry loops), and no writer starvation.
 *
 * See Documentation/locking/seqlock.rst
 *
 * Copyrights:
 * - Based on x86_64 vsyscall gettimeofday: Keith Owens, Andrea Arcangeli
 * - Sequence counters with associated locks, (C) 2020 Linutronix GmbH
 */

#ifndef _BACKPORT_LINUX_SEQLOCK_H
#define _BACKPORT_LINUX_SEQLOCK_H

#include_next<linux/seqlock.h>

#ifdef BPM_SEQCOUNT_SEQUENCE_NOT_PRESENT
static inline unsigned __seqcount_sequence(const seqcount_t *s)
{
        return READ_ONCE(s->sequence);
}

#define SEQCOUNT_LOCKNAME(lockname, locktype, preemptible, lockmember, lockbase, lock_acquire) \
static __always_inline unsigned                                         \
__seqcount_##lockname##_sequence(const seqcount_##lockname##_t *s)      \
{                                                                       \
        unsigned seq = READ_ONCE(s->seqcount.sequence);                 \
                                                                        \
        if (!IS_ENABLED(CONFIG_PREEMPT_RT))                             \
                return seq;                                             \
                                                                        \
        if (preemptible && unlikely(seq & 1)) {                         \
                __SEQ_LOCK(lock_acquire);                               \
                __SEQ_LOCK(lockbase##_unlock(s->lock));                 \
                                                                        \
                /*                                                      \
                 * Re-read the sequence counter since the (possibly     \
                 * preempted) writer made progress.                     \
                 */                                                     \
                seq = READ_ONCE(s->seqcount.sequence);                  \
        }                                                               \
                                                                        \
        return seq;                                                     \
}

#define __SEQ_RT	IS_ENABLED(CONFIG_PREEMPT_RT)

SEQCOUNT_LOCKNAME(raw_spinlock, raw_spinlock_t,  false,    s->lock,        raw_spin, raw_spin_lock(s->lock))
SEQCOUNT_LOCKNAME(spinlock,     spinlock_t,      __SEQ_RT, s->lock,        spin,     spin_lock(s->lock))
SEQCOUNT_LOCKNAME(rwlock,       rwlock_t,        __SEQ_RT, s->lock,        read,     read_lock(s->lock))
SEQCOUNT_LOCKNAME(mutex,        struct mutex,    true,     s->lock,        mutex,    mutex_lock(s->lock))
SEQCOUNT_LOCKNAME(ww_mutex,     struct ww_mutex, true,     &s->lock->base, ww_mutex, ww_mutex_lock(s->lock, NULL))

#define seqprop_sequence(s)	__seqprop(s, sequence)
#endif
#endif /* _BACKPORT_LINUX_SEQLOCK_H */
