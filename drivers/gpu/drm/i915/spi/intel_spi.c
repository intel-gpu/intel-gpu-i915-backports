// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2019-2020, Intel Corporation. All rights reserved.
 */

#if !IS_ENABLED(CONFIG_AUXILIARY_BUS)
#include <linux/mfd/core.h>
#include "gt/intel_gt.h"
#endif
#include <linux/irq.h>
#include "i915_reg.h"
#include "i915_drv.h"
#include "spi/intel_spi.h"

#if IS_ENABLED (CONFIG_AUXILIARY_BUS)
#define GEN12_GUNIT_SPI_SIZE 0x80
#else
static const struct resource spi_resources[] = {
	DEFINE_RES_MEM_NAMED(GEN12_GUNIT_SPI_BASE, 0x80, "i915-spi-mmio"),
};
#endif

static const struct i915_spi_region regions[I915_SPI_REGIONS] = {
	[0] = { .name = "DESCRIPTOR", },
	[2] = { .name = "GSC", },
	[11] = { .name = "OptionROM", },
	[12] = { .name = "DAM", },
	[13] = { .name = "PSC", },
};

#if IS_ENABLED (CONFIG_AUXILIARY_BUS)
static void i915_spi_release_dev(struct device *dev)
{
}
#endif

#if !IS_ENABLED (CONFIG_AUXILIARY_BUS)
static const struct mfd_cell intel_spi_cell = {
       .id = 2,
       .name = "i915-spi",
       .num_resources = ARRAY_SIZE(spi_resources),
       .resources = spi_resources,
       .platform_data = (void *)regions,
       .pdata_size    = sizeof(regions),
};

void intel_spi_init(struct intel_spi *spi, struct drm_i915_private *dev_priv)
{
	struct pci_dev *pdev = to_pci_dev(dev_priv->drm.dev);
	int ret;

	/* Only the DGFX devices have internal SPI */
	if (!IS_DGFX(dev_priv))
		return;
	/* No access to internal SPI from VFs */
	if (IS_SRIOV_VF(dev_priv))
		return;

	ret = mfd_add_devices(&pdev->dev, PLATFORM_DEVID_AUTO,
			&intel_spi_cell, 1,
			&pdev->resource[0], -1, NULL);

	if (ret)
		dev_err(&pdev->dev, "creating i915-spi cell failed\n");

	spi->i915 = dev_priv;
}

void intel_spi_fini(struct intel_spi *spi)
{
	struct pci_dev *pdev;

	if (!spi->i915)
		return;

	pdev = to_pci_dev(spi->i915->drm.dev);

	dev_dbg(&pdev->dev, "removing i915-spi cell\n");
}
#else
void intel_spi_init(struct intel_spi *spi, struct drm_i915_private *dev_priv)
{
        struct pci_dev *pdev = to_pci_dev(dev_priv->drm.dev);
        struct auxiliary_device *aux_dev = &spi->aux_dev;
        int ret;

        /* Only the DGFX devices have internal SPI */
        if (!IS_DGFX(dev_priv))
                return;
        /* No access to internal SPI from VFs */
        if (IS_SRIOV_VF(dev_priv))
                return;

        spi->bar.parent = &pdev->resource[0];
        spi->bar.start = GEN12_GUNIT_SPI_BASE + pdev->resource[0].start;
        spi->bar.end = spi->bar.start + GEN12_GUNIT_SPI_SIZE - 1;
        spi->bar.flags = IORESOURCE_MEM;
        spi->bar.desc = IORES_DESC_NONE;
        spi->regions = regions;

        aux_dev->name = "spi";
        aux_dev->id = (pci_domain_nr(pdev->bus) << 16) |
                       PCI_DEVID(pdev->bus->number, pdev->devfn);

	aux_dev->dev.parent = &pdev->dev;
	aux_dev->dev.release = i915_spi_release_dev;

        ret = auxiliary_device_init(aux_dev);
        if (ret) {
                dev_err(&pdev->dev, "i915-spi aux init failed %d\n", ret);
                return;
        }

        ret = auxiliary_device_add(aux_dev);
        if (ret) {
                dev_err(&pdev->dev, "i915-spi aux add failed %d\n", ret);
                auxiliary_device_uninit(aux_dev);
                return;
        }

        spi->i915 = dev_priv;
}

void intel_spi_fini(struct intel_spi *spi)
{
        struct pci_dev *pdev;

        if (!spi->i915)
                return;

        pdev = to_pci_dev(spi->i915->drm.dev);

        dev_dbg(&pdev->dev, "removing i915-spi cell\n");

        auxiliary_device_delete(&spi->aux_dev);
        auxiliary_device_uninit(&spi->aux_dev);
}
#endif

