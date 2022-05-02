#ifndef _BACKPORT_LINUX_NVMEM_CONSUMER_H
#define _BACKPORT_LINUX_NVMEM_CONSUMER_H
#include <linux/version.h>
#include_next <linux/nvmem-consumer.h>

#if KERNEL_VERSION(5, 5, 0) > LINUX_VERSION_CODE
#define nvmem_device_find LINUX_I915_BACKPORT(nvmem_device_find)
struct nvmem_device *nvmem_device_find(void *data,
                        int (*match)(struct device *dev, const void *data));

#endif /* KERNEL_VERSION(5, 5, 0) */
#endif /* _BACKPORT_LINUX_NVMEM_CONSUMER_H */
