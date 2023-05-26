/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Runtime locking correctness validator
 *
 *  Copyright (C) 2006,2007 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 *  Copyright (C) 2007 Red Hat, Inc., Peter Zijlstra
 *
 * see Documentation/locking/lockdep-design.rst for more details.
 */

#ifndef _BACKPORT_LINUX_LOCKDEP_H
#define _BACKPORT_LINUX_LOCKDEP_H
#include <linux/version.h>
#include_next <linux/lockdep.h>

#define lock_acquire_shared_recursive(l, s, t, n, i)    lock_acquire(l, s, t, 2, 1, n, i)

#ifdef CONFIG_LOCK_STAT
#define LOCK_CONTENDED_RETURN(_lock, try, lock)                 \
({                                                              \
        int ____err = 0;                                        \
        if (!try(_lock)) {                                      \
                lock_contended(&(_lock)->dep_map, _RET_IP_);    \
                ____err = lock(_lock);                          \
        }                                                       \
        if (!____err)                                           \
                lock_acquired(&(_lock)->dep_map, _RET_IP_);     \
        ____err;                                                \
})
#else
#define LOCK_CONTENDED_RETURN(_lock, try, lock) \
        lock(_lock)
#endif

#ifdef BPM_MIGHT_LOCK_NESTED_NOT_PRESENT
#ifdef CONFIG_PROVE_LOCKING
# define might_lock_nested(lock, subclass)				\
do {									\
	typecheck(struct lockdep_map *, &(lock)->dep_map);		\
	lock_acquire(&(lock)->dep_map, subclass, 0, 1, 1, NULL,		\
			_THIS_IP_);					\
	lock_release(&(lock)->dep_map, _THIS_IP_);			\
} while (0)
#else
# define might_lock_nested(lock, subclass) do { } while (0)
#endif
#endif

#ifdef BPM_LOCKDEP_ASSERT_API_NOT_PRESENT
#define lockdep_assert_once(c)                  do { } while (0)
#define lockdep_assert_none_held_once()         \
        lockdep_assert_once(!current->lockdep_depth)
#endif

#ifdef BPM_LOCKDEP_ASSERT_NOT_HELD_NOT_PRESENT

#ifdef CONFIG_LOCKDEP
#define lockdep_assert_not_held(l)      \
	        lockdep_assert(lockdep_is_held(l) != LOCK_STATE_HELD)
#else
#define lockdep_assert_not_held(l)              do { (void)(l); } while (0)
#endif

#endif

#endif /* _BACKPORT_LINUX_LOCKDEP_H */
