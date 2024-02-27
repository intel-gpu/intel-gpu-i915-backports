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

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>

#ifdef BPM_I2C_ACPI_GET_I2C_RESOURCE_NOT_PRESENT
/**
 * i2c_acpi_get_i2c_resource - Gets I2cSerialBus resource if type matches
 * @ares:       ACPI resource
 * @i2c:        Pointer to I2cSerialBus resource will be returned here
 *
 * Checks if the given ACPI resource is of type I2cSerialBus.
 * In this case, returns a pointer to it to the caller.
 *
 * Returns true if resource type is of I2cSerialBus, otherwise false.
 */
bool i2c_acpi_get_i2c_resource(struct acpi_resource *ares,
                               struct acpi_resource_i2c_serialbus **i2c)
{
        struct acpi_resource_i2c_serialbus *sb;

        if (ares->type != ACPI_RESOURCE_TYPE_SERIAL_BUS)
                return false;

        sb = &ares->data.i2c_serial_bus;
        if (sb->type != ACPI_RESOURCE_SERIAL_TYPE_I2C)
                return false;

        *i2c = sb;
        return true;
}
EXPORT_SYMBOL_GPL(i2c_acpi_get_i2c_resource);
#endif

#ifdef BPM_I2C_ACPI_FIND_ADAPTER_BY_HANDLE_EXPORT_NOT_PRESENT
static int i2c_acpi_find_match_adapter(struct device *dev, const void *data)
{
        struct i2c_adapter *adapter = i2c_verify_adapter(dev);

        if (!adapter)
                return 0;

        return ACPI_HANDLE(dev) == (acpi_handle)data;
}

struct i2c_adapter *i2c_acpi_find_adapter_by_handle(acpi_handle handle)
{
        struct device *dev;

        dev = bus_find_device(&i2c_bus_type, NULL, handle,
                              i2c_acpi_find_match_adapter);

        return dev ? i2c_verify_adapter(dev) : NULL;
}
EXPORT_SYMBOL_GPL(i2c_acpi_find_adapter_by_handle);
#endif
