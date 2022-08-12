/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _BACKPORT_LINUX_SEQLOCK_H
#define _BACKPORT_LINUX_SEQLOCK_H
#include_next<linux/seqlock.h>


#if defined(CONFIG_LOCKDEP) || defined(CONFIG_PREEMPT_RT)
#define __SEQ_LOCK(expr)        expr
#else
#define __SEQ_LOCK(expr)
#endif

typedef struct seqcount_mutex {
        seqcount_t              seqcount;
        __SEQ_LOCK(locktype     *lock);
} seqcount_mutex_t;

#define seqcount_mutex_init(s, lock)       		\
	do {						\
		seqcount_mutex_t *____s = (s);		\
		seqcount_init(&____s->seqcount);	\
		__SEQ_LOCK(____s->lock = (lock));	\
        } while (0)

static __always_inline seqcount_t * 
__seqcount_ptr(seqcount_mutex_t *s)
{
	return &s->seqcount;
}

#define write_seqcount_invalidate(s)			\
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

#endif /* _BACKPORT_LINUX_SEQLOCK_H */
