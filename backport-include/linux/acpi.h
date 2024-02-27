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

#ifndef __BACKPORT_LINUX_ACPI_H
#define __BACKPORT_LINUX_ACPI_H
#include_next <linux/acpi.h>
#include <linux/version.h>

#if LINUX_VERSION_IS_LESS(3,8,0)
/*
 * Backports
 *
 * commit 95f8a082b9b1ead0c2859f2a7b1ac91ff63d8765
 * Author: Rafael J. Wysocki <rafael.j.wysocki@intel.com>
 * Date:   Wed Nov 21 00:21:50 2012 +0100
 *
 *     ACPI / driver core: Introduce struct acpi_dev_node and related macros
 *
 *     To avoid adding an ACPI handle pointer to struct device on
 *     architectures that don't use ACPI, or generally when CONFIG_ACPI is
 *     not set, in which cases that pointer is useless, define struct
 *     acpi_dev_node that will contain the handle pointer if CONFIG_ACPI is
 *     set and will be empty otherwise and use it to represent the ACPI
 *     device node field in struct device.
 *
 *     In addition to that define macros for reading and setting the ACPI
 *     handle of a device that don't generate code when CONFIG_ACPI is
 *     unset.  Modify the ACPI subsystem to use those macros instead of
 *     referring to the given device's ACPI handle directly.
 *
 *     Signed-off-by: Rafael J. Wysocki <rafael.j.wysocki@intel.com>
 *     Reviewed-by: Mika Westerberg <mika.westerberg@linux.intel.com>
 *     Acked-by: Greg Kroah-Hartman <gregkh@linuxfoundation.org>
 */
#ifdef CONFIG_ACPI
#define ACPI_HANDLE(dev) DEVICE_ACPI_HANDLE(dev)
#else
#define ACPI_HANDLE(dev) (NULL)
#endif /* CONFIG_ACPI */
#endif /* LINUX_VERSION_IS_LESS(3,8,0) */

#ifndef ACPI_COMPANION
#ifdef CONFIG_ACPI
static inline struct acpi_device *_acpi_get_companion(struct device *dev)
{
	struct acpi_device *adev;
	int ret;

	ret = acpi_bus_get_device(ACPI_HANDLE(dev), &adev);
	if (ret < 0)
		adev = NULL;

	return adev;
}
#define ACPI_COMPANION(dev)	_acpi_get_companion(dev)
#else
#define ACPI_COMPANION(dev)	(NULL)
#endif /* CONFIG_ACPI */
#endif /* ACPI_COMPANION */

#if LINUX_VERSION_IS_LESS(3,19,0)
#define acpi_dev_remove_driver_gpios LINUX_I915_BACKPORT(acpi_dev_remove_driver_gpios)
static inline void acpi_dev_remove_driver_gpios(struct acpi_device *adev) {}
#endif /* LINUX_VERSION_IS_LESS(3, 19, 0) */

#if LINUX_VERSION_IN_RANGE(3,19,0, 4,13,0)
#define devm_acpi_dev_add_driver_gpios LINUX_I915_BACKPORT(devm_acpi_dev_add_driver_gpios)
static inline int devm_acpi_dev_add_driver_gpios(struct device *dev,
			      const struct acpi_gpio_mapping *gpios)
{
	return -ENXIO;
}
#endif /* LINUX_VERSION_IN_RANGE(3,19,0, 4,13,0) */

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

#endif /* __BACKPORT_LINUX_ACPI_H */
