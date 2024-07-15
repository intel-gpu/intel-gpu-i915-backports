#ifndef __BACKPORT_LINUX_I2C_H
#define __BACKPORT_LINUX_I2C_H
#include_next <linux/i2c.h>
#include <linux/version.h>
#include <linux/acpi.h>

#ifdef BPM_KMAP_ATOMIC_NOT_PRESENT
#include <linux/highmem.h>
#endif

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

/* This backports
 *
 * commit 14674e70119ea01549ce593d8901a797f8a90f74
 * Author: Mark Brown <broonie@opensource.wolfsonmicro.com>
 * Date:   Wed May 30 10:55:34 2012 +0200
 *
 *     i2c: Split I2C_M_NOSTART support out of I2C_FUNC_PROTOCOL_MANGLING
 */
#ifndef I2C_FUNC_NOSTART
#define I2C_FUNC_NOSTART 0x00000010 /* I2C_M_NOSTART */
#endif

/* This backports:
 *
 * commit 7c92784a546d2945b6d6973a30f7134be78eb7a4
 * Author: Lars-Peter Clausen <lars@metafoo.de>
 * Date:   Wed Nov 16 10:13:36 2011 +0100
 *
 *     I2C: Add helper macro for i2c_driver boilerplate
 */
#ifndef module_i2c_driver
#define module_i2c_driver(__i2c_driver) \
	module_driver(__i2c_driver, i2c_add_driver, \
			i2c_del_driver)
#endif

#ifndef I2C_CLIENT_SCCB
#define I2C_CLIENT_SCCB	0x9000		/* Use Omnivision SCCB protocol */
					/* Must match I2C_M_STOP|IGNORE_NAK */
#endif

#endif /* __BACKPORT_LINUX_I2C_H */
