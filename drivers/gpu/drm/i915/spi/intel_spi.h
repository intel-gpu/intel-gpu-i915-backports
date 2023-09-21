/* SPDX-License-Identifier: MIT */
/*
 * Copyright(c) 2019-2022, Intel Corporation. All rights reserved.
 */

#ifndef __INTEL_SPI_DEV_H__
#define __INTEL_SPI_DEV_H__

#if IS_ENABLED(CONFIG_AUXILIARY_BUS)
#include <linux/auxiliary_bus.h>
#endif

struct drm_i915_private;

#define I915_SPI_REGIONS 14
struct i915_spi_region {
	const char *name;
};

#if IS_ENABLED(CONFIG_AUXILIARY_BUS)
struct intel_spi {
	struct auxiliary_device aux_dev;
	struct drm_i915_private *i915;
	bool writeable_override;
	struct resource bar;
	const struct i915_spi_region *regions;
};
#else
struct intel_spi {
	struct drm_i915_private *i915;
};
#endif

#if IS_ENABLED(CONFIG_AUXILIARY_BUS)
#define auxiliary_dev_to_intel_spi_dev(auxiliary_dev) \
	container_of(auxiliary_dev, struct intel_spi, aux_dev)
#endif

void intel_spi_init(struct intel_spi *spi, struct drm_i915_private *i915);

void intel_spi_fini(struct intel_spi *spi);

#endif /* __INTEL_SPI_DEV_H__ */
