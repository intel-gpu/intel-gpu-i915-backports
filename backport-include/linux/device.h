#ifndef __BACKPORT_DEVICE_H
#define __BACKPORT_DEVICE_H
#include <linux/export.h>
#include_next <linux/device.h>
#ifdef BPM_DEVICE_ATTR_ADMIN_RX_NOT_PRESENT
#define DEVICE_ATTR_ADMIN_RW(_name) \
              struct device_attribute dev_attr_##_name = __ATTR_RW_MODE(_name, 0600)
#define DEVICE_ATTR_ADMIN_RO(_name) \
       struct device_attribute dev_attr_##_name = __ATTR_RO_MODE(_name, 0400)
#endif

#ifdef BPM_FIND_BY_DEVICE_TYPE_NOT_AVAILABLE

static inline int device_match_devt(struct device *dev, const void *pdevt)
{
       return dev->devt == *(dev_t *)pdevt;
}

/**
 * class_find_device_by_devt : device iterator for locating a particular device
 * matching the device type.
 * @class: class type
 * @devt: device type of the device to match.
 */
static inline struct device *class_find_device_by_devt(struct class *class,
						       dev_t devt)
{
	return class_find_device(class, NULL, &devt, device_match_devt);
}
#endif

#endif

