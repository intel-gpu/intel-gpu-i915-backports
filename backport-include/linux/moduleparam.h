/*
 * Copyright _ 2018 Intel Corporation
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
 * Authors:
 *    Owen Zhang <owen.zhang@intel.com>
 *
 */
#ifndef __BACKPORT_LINUX_MODULEPARAM_H
#define __BACKPORT_LINUX_MODULEPARAM_H
#include_next <linux/moduleparam.h>

#if LINUX_VERSION_IS_LESS(4,2,0)
#define kernel_param_lock LINUX_I915_BACKPORT(kernel_param_lock)
static inline void kernel_param_lock(struct module *mod)
{
	__kernel_param_lock();
}
#define kernel_param_unlock LINUX_I915_BACKPORT(kernel_param_unlock)
static inline void kernel_param_unlock(struct module *mod)
{
	__kernel_param_unlock();
}
#endif

#if LINUX_VERSION_IS_LESS(3,8,0)
#undef __MODULE_INFO
#ifdef MODULE
#define __MODULE_INFO(tag, name, info)					  \
static const char __UNIQUE_ID(name)[]					  \
  __used __attribute__((section(".modinfo"), unused, aligned(1)))	  \
  = __stringify(tag) "=" info
#else  /* !MODULE */
/* This struct is here for syntactic coherency, it is not used */
#define __MODULE_INFO(tag, name, info)					  \
  struct __UNIQUE_ID(name) {}
#endif
#endif /* < 3.8 */

#if LINUX_VERSION_IS_LESS(3,17,0)
extern struct kernel_param_ops param_ops_ullong;
extern int param_set_ullong(const char *val, const struct kernel_param *kp);
extern int param_get_ullong(char *buffer, const struct kernel_param *kp);
#define param_check_ullong(name, p) __param_check(name, p, unsigned long long)
#endif

#ifndef module_param_hw_array
#define module_param_hw_array(name, type, hwtype, nump, perm) \
	module_param_array(name, type, nump, perm)
#endif

#ifndef module_param_named_unsafe
#define module_param_named_unsafe module_param_named
#endif

#ifndef module_param_unsafe
#define module_param_unsafe(name, type, perm)			\
	module_param_named_unsafe(name, name, type, perm)
#endif


#endif /* __BACKPORT_LINUX_MODULEPARAM_H */
