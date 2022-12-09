// SPDX-License-Identifier: GPL-2.0
/*
 * nvmem framework core.
 *
 * Copyright (C) 2015 Srinivas Kandagatla <srinivas.kandagatla@linaro.org>
 * Copyright (C) 2013 Maxime Ripard <maxime.ripard@free-electrons.com>
 */

#include <linux/module.h>
#include <linux/nvmem-consumer.h>

#ifdef BPM_NVMEM_DEVICE_FIND_NOT_PRESENT

/**
 * nvmem_device_find() - Find nvmem device with matching function
 *
 * @data: Data to pass to match function
 * @match: Callback function to check device
 *
 * Return: ERR_PTR() on error or a valid pointer to a struct nvmem_device
 * on success.
 */
struct nvmem_device *nvmem_device_find(void *data,
                        int (*match)(struct device *dev, const void *data))
{
        return ERR_PTR(-EINVAL);
}
EXPORT_SYMBOL_GPL(nvmem_device_find);

#endif /* BPM_NVMEM_DEVICE_FIND_NOT_PRESENT */
