#ifndef _BACKPORT_LINUX_NVMEM_CONSUMER_H
#define _BACKPORT_LINUX_NVMEM_CONSUMER_H
#include <linux/version.h>
#include_next <linux/nvmem-consumer.h>

#ifdef BPM_NVMEM_DEVICE_FIND_NOT_PRESENT
#define nvmem_device_find LINUX_I915_BACKPORT(nvmem_device_find)
struct nvmem_device *nvmem_device_find(void *data,
                        int (*match)(struct device *dev, const void *data));

#endif
#endif /* _BACKPORT_LINUX_NVMEM_CONSUMER_H */
