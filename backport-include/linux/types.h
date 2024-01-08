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
 * Authors:
 *    Owen Zhang <owen.zhang@intel.com>
 *
 */

#ifndef _BACKPORT_LINUX_TYPES_H
#define _BACKPORT_LINUX_TYPES_H
#include <linux/version.h>


#include <asm/posix_types.h>
#undef __FD_SETSIZE
#define __FD_SETSIZE    1024

#include_next <linux/types.h>

#if LINUX_VERSION_IS_LESS(3,17,0)
typedef __s64 time64_t;
#endif

#if LINUX_VERSION_IS_LESS(4,16,0)
typedef unsigned __poll_t;
#endif

#endif /* _BACKPORT_LINUX_TYPES_H */
