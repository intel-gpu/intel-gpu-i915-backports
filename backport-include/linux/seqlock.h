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
#ifdef BPM_SEQCOUNT_MUTEX_INIT_NOT_PRESENT
#if defined(CONFIG_LOCKDEP) || defined(CONFIG_PREEMPT_RT)
#define __SEQ_LOCK(expr)        expr
#else
#define __SEQ_LOCK(expr)
#endif

typedef struct seqcount_mutex {
       seqcount_t              seqcount;
       __SEQ_LOCK(locktype     *lock);
} seqcount_mutex_t;

#define seqcount_mutex_init(s, lock)                    \
        do {                                            \
                seqcount_mutex_t *____s = (s);          \
                seqcount_init(&____s->seqcount);        \
                __SEQ_LOCK(____s->lock = (lock));       \
        } while (0)

static __always_inline seqcount_t *
__seqcount_ptr(seqcount_mutex_t *s)
{
        return &s->seqcount;
}

#define write_seqcount_invalidate(s)                    \
        write_seqcount_t_invalidate(__seqcount_ptr(s))

static inline void write_seqcount_t_invalidate(seqcount_t *s)
{
        smp_wmb();
        s->sequence+=2;
}

static inline unsigned seqprop_sequence(const seqcount_mutex_t *s)
{
        unsigned seq = READ_ONCE(s->seqcount.sequence);
        bool preemptible = true;
        if (!IS_ENABLED(CONFIG_PREEMPT_RT))
                return seq;

        if (preemptible && unlikely(seq & 1)) {
                __SEQ_LOCK(mutex_lock(s->lock));
                __SEQ_LOCK(mutex_unlock(s->lock));

                /*
                 * Re-read the sequence counter since the (possibly
                 * preempted) writer made progress.
                 */
                seq = READ_ONCE(s->seqcount.sequence);
        }

        return seq;
}
#endif

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
#endif

#ifdef BPM_SEQPROP_SEQUENCE_NOT_PRESENT
#define seqprop_sequence(s)	__seqprop(s, sequence)
#endif
#endif /* _BACKPORT_LINUX_SEQLOCK_H */
