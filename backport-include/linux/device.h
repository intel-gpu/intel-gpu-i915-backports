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
#endif

