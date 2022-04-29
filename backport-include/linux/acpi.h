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

#ifndef _BACKPORT_LINUX_ACPI_H
#define _BACKPORT_LINUX_ACPI_H
#include <linux/version.h>
#include <asm/io.h>
#include_next <linux/acpi.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,18,0)
#ifndef ACPI_HANDLE
#define ACPI_HANDLE DEVICE_ACPI_HANDLE
#endif

#define acpi_evaluate_dsm LINUX_I915_BACKPORT(acpi_evaluate_dsm)
union acpi_object *acpi_evaluate_dsm(acpi_handle handle, const u8 *uuid,
			int rev, int func, union acpi_object *argv4);

#define acpi_evaluate_dsm_typed LINUX_I915_BACKPORT(acpi_evaluate_dsm_typed)
static inline union acpi_object *
acpi_evaluate_dsm_typed(acpi_handle handle, const u8 *uuid, int rev, int func,
			union acpi_object *argv4, acpi_object_type type)
{
	union acpi_object *obj;

	obj = acpi_evaluate_dsm(handle, uuid, rev, func, argv4);
	if (obj && obj->type != type) {
		ACPI_FREE(obj);
		obj = NULL;
	}

	return obj;
}

#ifndef acpi_os_ioremap
#define acpi_os_ioremap LINUX_I915_BACKPORT(acpi_os_ioremap)
static inline void __iomem *acpi_os_ioremap(acpi_physical_address phys,
					    acpi_size size)
{
       return ioremap_cache(phys, size);
}
#endif

#define acpi_video_verify_backlight_support LINUX_I915_BACKPORT(acpi_video_verify_backlight_support)
#if (defined CONFIG_ACPI_VIDEO || defined CONFIG_ACPI_VIDEO_MODULE)
extern bool acpi_video_verify_backlight_support(void);
#else
static inline bool acpi_video_verify_backlight_support(void) { return false; }
#endif

#define acpi_target_system_state LINUX_I915_BACKPORT(acpi_target_system_state)
#ifdef CONFIG_ACPI_SLEEP
u32 acpi_target_system_state(void);
#else
static inline u32 acpi_target_system_state(void) { return ACPI_STATE_S0; }
#endif


#if defined(CONFIG_ACPI) && defined(CONFIG_DYNAMIC_DEBUG)
__printf(3, 4)
void __acpi_handle_debug(struct _ddebug *descriptor, acpi_handle handle, const char *fmt, ...);
#else
#define __acpi_handle_debug(descriptor, handle, fmt, ...)		\
	acpi_handle_printk(KERN_DEBUG, handle, fmt, ##__VA_ARGS__);
#endif


#ifndef acpi_handle_warn
#define acpi_handle_warn(handle, fmt, ...)				\
	acpi_handle_printk(KERN_WARNING, handle, fmt, ##__VA_ARGS__)
#endif
#endif
#endif
