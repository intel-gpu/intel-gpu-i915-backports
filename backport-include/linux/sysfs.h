/*
 *
 * Copyright © 2021 Intel Corporation
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#ifndef __BACKPORT_SYSFS_H
#define __BACKPORT_SYSFS_H
#include <linux/version.h>
#include_next <linux/sysfs.h>

#if LINUX_VERSION_IS_LESS(5,10,0)
#define sysfs_emit LINUX_I915_BACKPORT(sysfs_emit)
#define sysfs_emit_at LINUX_I915_BACKPORT(sysfs_emit_at)

#ifdef CONFIG_SYSFS

__printf(2, 3)
int sysfs_emit(char *buf, const char *fmt, ...);
__printf(3, 4)
int sysfs_emit_at(char *buf, int at, const char *fmt, ...);

#else
__printf(2, 3)
static inline int sysfs_emit(char *buf, const char *fmt, ...)
{
        return 0;
}

__printf(3, 4)
static inline int sysfs_emit_at(char *buf, int at, const char *fmt, ...)
{
        return 0;
}

#endif /* CONFIG_SYSFS */

#endif /* LINUX_VERSION_IS_LESS */
#endif /* __BACKPORT_SYSFS_H */
