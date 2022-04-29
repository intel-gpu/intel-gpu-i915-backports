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

#ifndef __BACKPORT_LINUX_CAPABILITY_H
#define __BACKPORT_LINUX_CAPABILITY_H

#include_next <linux/capability.h>

#if LINUX_VERSION_IS_LESS(5,8,0)
#define perfmon_capable LINUX_I915_BACKPORT(perfmon_capable)

/*
 * Fixme: CAP_PERFMON macro should be part of uapi/linux/capability.h
 * but when backported uapi/linux/capability.h with CAP_PERFMON macro,
 * getting more undefind errors due to multiple inclusion of capability.h in
 * headers. Hence CAP_PERFMON declared here.
 *
 */
#define CAP_PERFMON             38

#ifdef CONFIG_MULTIUSER
extern bool capable(int cap);
#else
static inline bool capable(int cap)
{
        return true;
}
#endif /* CONFIG_MULTIUSER */

static inline bool perfmon_capable(void)
{
        return capable(CAP_PERFMON) || capable(CAP_SYS_ADMIN);
}

#endif /* LINUX_VERSION_IS_LESS */
#endif /* __BACKPORT_LINUX_CAPABILITY_H */
