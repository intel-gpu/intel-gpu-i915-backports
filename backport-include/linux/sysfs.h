/*
* Copyright © 2021 Intel Corporation
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
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*
*/

#ifndef _BACKPORT_SYSFS_H_
#define _BACKPORT_SYSFS_H_
#include <linux/version.h>

#include_next <linux/sysfs.h>

#ifdef BPM_SYSFS_EMIT_NOT_PRESENT

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

#endif
#endif

#ifdef BPM_DEVICE_ATTR_ADMIN_RX_NOT_PRESENT
#define __ATTR_RW_MODE(_name, _mode) {                                 \
       .attr   = { .name = __stringify(_name),                         \
                   .mode = VERIFY_OCTAL_PERMISSIONS(_mode) },          \
       .show   = _name##_show,                                         \
       .store  = _name##_store,                                        \
}
#endif
#endif /* _BACKPORT_SYSFS_H_ */
