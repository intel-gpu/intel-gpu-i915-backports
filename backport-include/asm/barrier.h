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
#ifndef __BACKPORT_ASM_BARRIER_H
#define __BACKPORT_ASM_BARRIER_H

#include <linux/version.h>
#if LINUX_VERSION_IS_GEQ(3,4,0) || \
    defined(CONFIG_ALPHA) || defined(CONFIG_MIPS)
#include_next <asm/barrier.h>
#endif /* >= 3.4 */

#ifndef dma_rmb
#define dma_rmb()	rmb()
#endif

#ifndef dma_wmb
#define dma_wmb()	wmb()
#endif

#ifndef smp_mb__after_atomic
#define smp_mb__after_atomic smp_mb__after_clear_bit
#endif

#ifndef smp_acquire__after_ctrl_dep
#define smp_acquire__after_ctrl_dep()		smp_rmb()
#endif

#ifndef __smp_mb
#define __smp_mb()      mb()
#endif

#ifndef __smp_store_mb
#define __smp_store_mb(var, value)  do { WRITE_ONCE(var, value); __smp_mb(); } while (0)
#endif

#ifdef CONFIG_SMP
#ifndef smp_store_mb
#define smp_store_mb(var, value)  __smp_store_mb(var, value)
#endif
#else
#ifndef smp_store_mb
#define smp_store_mb(var, value)  do { WRITE_ONCE(var, value); barrier(); } while (0)
#endif
#endif

#ifndef smp_mb__before_atomic
#define smp_mb__before_atomic()	barrier()
#endif

#ifndef smp_load_acquire

#if defined(CONFIG_X86_PPRO_FENCE)
#define smp_load_acquire(p)						\
({									\
	typeof(*p) ___p1 = ACCESS_ONCE(*p);				\
	compiletime_assert_atomic_type(*p);				\
	smp_mb();							\
	___p1;								\
})

#else /* regular x86 TSO memory ordering */
#define smp_load_acquire(p)						\
({									\
	typeof(*p) ___p1 = ACCESS_ONCE(*p);				\
	compiletime_assert_atomic_type(*p);				\
	barrier();							\
	___p1;								\
})
#endif /* CONFIG_X86_PPRO_FENCE */

#endif /* smp_load_acquire */

#endif /* __BACKPORT_ASM_BARRIER_H */
