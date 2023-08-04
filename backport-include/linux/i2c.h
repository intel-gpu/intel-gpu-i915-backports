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

#ifndef _BACKPORT_LINUX_I2C_H
#define _BACKPORT_LINUX_I2C_H
#include <linux/version.h>
#include_next <linux/i2c.h>
#include <linux/acpi.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,18,0)
/**
  * struct i2c_lock_operations - represent I2C locking operations
  * @lock_bus: Get exclusive access to an I2C bus segment
  * @trylock_bus: Try to get exclusive access to an I2C bus segment
  * @unlock_bus: Release exclusive access to an I2C bus segment
  *
  * The main operations are wrapped by i2c_lock_bus and i2c_unlock_bus.
  */
struct i2c_lock_operations {
        void (*lock_bus)(struct i2c_adapter *, unsigned int flags);
        int (*trylock_bus)(struct i2c_adapter *, unsigned int flags);
        void (*unlock_bus)(struct i2c_adapter *, unsigned int flags);
};

#endif

#ifdef BPM_I2C_ACPI_GET_I2C_RESOURCE_NOT_PRESENT

struct acpi_resource;
struct acpi_resource_i2c_serialbus;

#define i2c_acpi_get_i2c_resource LINUX_DMABUF_BACKPORT(i2c_acpi_get_i2c_resource)
#if IS_ENABLED(CONFIG_ACPI)
bool i2c_acpi_get_i2c_resource(struct acpi_resource *ares,
                               struct acpi_resource_i2c_serialbus **i2c);
#else
static inline bool i2c_acpi_get_i2c_resource(struct acpi_resource *ares,
                                             struct acpi_resource_i2c_serialbus **i2c)
{
        return false;
}
#endif
#endif

#ifdef BPM_I2C_ACPI_FIND_ADAPTER_BY_HANDLE_EXPORT_NOT_PRESENT

#define i2c_acpi_find_adapter_by_handle LINUX_DMABUF_BACKPORT(i2c_acpi_find_adapter_by_handle)
#if IS_ENABLED(CONFIG_ACPI)
struct i2c_adapter *i2c_acpi_find_adapter_by_handle(acpi_handle handle);
#else
static inline struct i2c_adapter *i2c_acpi_find_adapter_by_handle(acpi_handle handle)
{
        return NULL;
}
#endif
#endif

#ifdef BPM_I2C_CLIENT_HAS_DRIVER_NOT_PRESENT
static inline bool i2c_client_has_driver(struct i2c_client *client)
{
        return !IS_ERR_OR_NULL(client) && client->dev.driver;
}
#endif

#ifdef BPM_I2C_NEW_CLIENT_DEVICE_NOT_PRESENT
#define i2c_new_client_device i2c_new_device
#endif

#endif /*_BACKPORT_LINUX_I2C_H */
