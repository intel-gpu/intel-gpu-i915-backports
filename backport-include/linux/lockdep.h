#ifndef __BACKPORT_LINUX_LOCKDEP_H
#define __BACKPORT_LINUX_LOCKDEP_H
#include_next <linux/lockdep.h>
#include <linux/version.h>

#ifdef BPM_LOCKDEP_ASSERT_NOT_HELD_NOT_PRESENT

#ifdef CONFIG_LOCKDEP
#define lockdep_assert_not_held(l)      \
                lockdep_assert(lockdep_is_held(l) != LOCK_STATE_HELD)
#else
#define lockdep_assert_not_held(l)        do { (void)(l); } while (0)
#endif

#endif

#if LINUX_VERSION_IS_LESS(3,9,0)
#undef lockdep_assert_held
#ifdef CONFIG_LOCKDEP
#define lockdep_assert_held(l)	do {				\
		WARN_ON(debug_locks && !lockdep_is_held(l));	\
	} while (0)
#else
#define lockdep_assert_held(l)			do { (void)(l); } while (0)
#endif /* CONFIG_LOCKDEP */
#endif /* LINUX_VERSION_IS_LESS(3,9,0) */

#if LINUX_VERSION_IS_LESS(4,15,0)
#ifndef CONFIG_LOCKDEP
struct lockdep_map { };
#endif /* CONFIG_LOCKDEP */
#endif /* LINUX_VERSION_IS_LESS(4,15,0) */

#define lockdep_assert_once(c)                  do { } while (0)
#define lockdep_assert_none_held_once()         \
        lockdep_assert_once(!current->lockdep_depth)

#endif /* __BACKPORT_LINUX_LOCKDEP_H */
