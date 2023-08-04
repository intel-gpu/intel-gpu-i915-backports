/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2019-2020 Intel Corporation
 *
 * Please see Documentation/driver-api/auxiliary_bus.rst for more information.
 */
 
#ifndef __BACKPORT_INCLUDE_LINUX_AUXILIARY_BUS_H__
#define __BACKPORT_INCLUDE_LINUX_AUXILIARY_BUS_H__

#include_next <linux/auxiliary_bus.h>

#ifdef BPM_AUXILIARY_BUS_HELPERS_NOT_PRESENT
static inline void *auxiliary_get_drvdata(struct auxiliary_device *auxdev)
{
       return dev_get_drvdata(&auxdev->dev);
}

static inline void auxiliary_set_drvdata(struct auxiliary_device *auxdev, void *data)
{
       dev_set_drvdata(&auxdev->dev, data);
}
#endif
#endif
