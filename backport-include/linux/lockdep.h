/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 *
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

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5,6,0))
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

#endif
