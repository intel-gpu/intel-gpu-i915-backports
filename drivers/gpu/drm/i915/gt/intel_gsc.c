// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2019-2022, Intel Corporation. All rights reserved.
 */

#include <linux/irq.h>
#if IS_ENABLED(CONFIG_AUXILIARY_BUS)
#include <linux/mei_aux.h>
#else
#include <linux/platform_device.h>
#include <linux/mfd/core.h>
#endif
#include "i915_drv.h"
#include "i915_reg.h"
#include "gem/i915_gem_region.h"
#include "gt/intel_gsc.h"
#include "gt/intel_gt.h"

#define GSC_BAR_LENGTH  0x00000FFC

static void gsc_irq_mask(struct irq_data *d)
{
	/* generic irq handling */
}

static void gsc_irq_unmask(struct irq_data *d)
{
	/* generic irq handling */
}

static struct irq_chip gsc_irq_chip = {
	.name = "gsc_irq_chip",
	.irq_mask = gsc_irq_mask,
	.irq_unmask = gsc_irq_unmask,
};

static int gsc_irq_init(int irq)
{
	irq_set_chip_and_handler_name(irq, &gsc_irq_chip,
				      handle_simple_irq, "gsc_irq_handler");

	return irq_set_chip_data(irq, NULL);
}
#if !IS_ENABLED(CONFIG_AUXILIARY_BUS)
/* gsc (graphics system controller) resources */
static const struct resource gsc_dg2_resources[] = {
	DEFINE_RES_IRQ_NAMED(0, "gsc-irq"),
	DEFINE_RES_MEM_NAMED(DG2_GSC_HECI1_BASE,
			     GSC_BAR_LENGTH,
			     "gsc-mmio"),
};

static const struct resource gsc_pvc_resources[] = {
	DEFINE_RES_IRQ_NAMED(0, "gsc-irq"),
	DEFINE_RES_MEM_NAMED(PVC_GSC_HECI1_BASE,
			     GSC_BAR_LENGTH,
			     "gsc-mmio"),
};

/* gscfi (graphics system controller firmware interface) resources */
static const struct resource gscfi_dg1_resources[] = {
	DEFINE_RES_IRQ_NAMED(0, "gscfi-irq"),
	DEFINE_RES_MEM_NAMED(DG1_GSC_HECI2_BASE,
			     GSC_BAR_LENGTH,
			     "gscfi-mmio"),
};

static const struct resource gscfi_dg2_resources[] = {
	DEFINE_RES_IRQ_NAMED(0, "gscfi-irq"),
	DEFINE_RES_MEM_NAMED(DG2_GSC_HECI2_BASE,
			     GSC_BAR_LENGTH,
			     "gscfi-mmio"),
};

static const struct resource gscfi_pvc_resources[] = {
	DEFINE_RES_IRQ_NAMED(0, "gscfi-irq"),
	DEFINE_RES_MEM_NAMED(PVC_GSC_HECI2_BASE,
			     GSC_BAR_LENGTH,
			     "gscfi-mmio"),
};
#endif /* CONFIG_AUXILIARY_BUS */

static int
gsc_ext_om_alloc(struct intel_gsc *gsc, struct intel_gsc_intf *intf, size_t size)
{
	struct intel_gt *gt = gsc_to_gt(gsc);
	struct drm_i915_gem_object *obj;
	int err;

	obj = i915_gem_object_create_lmem(gt->i915, size,
					  I915_BO_ALLOC_CONTIGUOUS |
					  I915_BO_CPU_CLEAR);
	if (IS_ERR(obj)) {
		drm_err(&gt->i915->drm, "Failed to allocate gsc memory\n");
		return PTR_ERR(obj);
	}

	err = i915_gem_object_pin_pages_unlocked(obj);
	if (err) {
		drm_err(&gt->i915->drm, "Failed to pin pages for gsc memory\n");
		goto out_put;
	}

	intf->gem_obj = obj;

	return 0;

out_put:
	i915_gem_object_put(obj);
	return err;
}

static void gsc_ext_om_destroy(struct intel_gsc_intf *intf)
{
	struct drm_i915_gem_object *obj = fetch_and_zero(&intf->gem_obj);

	if (!obj)
		return;

	if (i915_gem_object_has_pinned_pages(obj))
		i915_gem_object_unpin_pages(obj);

	i915_gem_object_put(obj);
}

#if IS_ENABLED(CONFIG_AUXILIARY_BUS)
static void intel_gsc_forcewake_get(void *gsc)
{
	struct intel_uncore *uncore = gsc_to_gt(gsc)->uncore;
	intel_wakeref_t wakeref;

	with_intel_runtime_pm(uncore->rpm, wakeref)
		intel_uncore_forcewake_get(uncore, FORCEWAKE_GT);
}

static void intel_gsc_forcewake_put(void *gsc)
{
	struct intel_uncore *uncore = gsc_to_gt(gsc)->uncore;
	intel_wakeref_t wakeref;

	with_intel_runtime_pm(uncore->rpm, wakeref)
		intel_uncore_forcewake_put(uncore, FORCEWAKE_GT);
}

/**
 * struct gsc_def - GSC definitions per generation
 *
 * @name: device name
 * @bar: base offset for HECI bar
 * @bar_size: size of HECI bar
 * @use_polling: use register polling instead of interrupts
 * @slow_firmware: the firmware is slow and requires longer timeouts
 * @lmem_size: size of extended operation memory for GSC, if required
 */
struct gsc_def {
	const char *name;
	unsigned long bar;
	size_t bar_size;
	bool use_polling;
	bool slow_firmware;
	size_t lmem_size;
};
#endif

/* gsc resources and definitions (HECI1 and HECI2) */
#if IS_ENABLED(CONFIG_AUXILIARY_BUS)
static const struct gsc_def gsc_def_dg1[] = {
	{
		/* HECI1 not yet implemented. */
	},
	{
		.name = "mei-gscfi",
		.bar = DG1_GSC_HECI2_BASE,
		.bar_size = GSC_BAR_LENGTH,
	}
};
#else
static const struct mfd_cell intel_gsc_dg1_cell[] = {
        {
                .id = 0,
        },
        {
                .id = 1,
                .name = "mei-gscfi",
                .num_resources = ARRAY_SIZE(gscfi_dg1_resources),
                .resources  = gscfi_dg1_resources,
        }
};
#endif


#if IS_ENABLED(CONFIG_AUXILIARY_BUS)
static const struct gsc_def gsc_def_xehpsdv[] = {
	{
		/* HECI1 not enabled on the device. */
	},
	{
		.name = "mei-gscfi",
		.bar = DG1_GSC_HECI2_BASE,
		.bar_size = GSC_BAR_LENGTH,
		.use_polling = true,
		.slow_firmware = true,
	}
};
#endif

#if IS_ENABLED(CONFIG_AUXILIARY_BUS)
static const struct gsc_def gsc_def_dg2[] = {
	{
		.name = "mei-gsc",
		.bar = DG2_GSC_HECI1_BASE,
		.bar_size = GSC_BAR_LENGTH,
		.lmem_size = SZ_4M,
	},
	{
		.name = "mei-gscfi",
		.bar = DG2_GSC_HECI2_BASE,
		.bar_size = GSC_BAR_LENGTH,
	}
};
#else
static const struct mfd_cell intel_gsc_dg2_cell[] = {
        {
                .id = 0,
                .name = "mei-gsc",
                .num_resources = ARRAY_SIZE(gsc_dg2_resources),
                .resources  = gsc_dg2_resources,
        },
        {
                .id = 1,
                .name = "mei-gscfi",
                .num_resources = ARRAY_SIZE(gscfi_dg2_resources),
                .resources  = gscfi_dg2_resources,
        }
};
#endif

#if IS_ENABLED(CONFIG_AUXILIARY_BUS)
static const struct gsc_def gsc_def_pvc[] = {
	{
		/* HECI1 not enabled on the device. */
	},
	{
		.name = "mei-gscfi",
		.bar = PVC_GSC_HECI2_BASE,
		.bar_size = GSC_BAR_LENGTH,
		.slow_firmware = true,
	}
};
#else
static const struct mfd_cell intel_gsc_pvc_cell[] = {
        {
                .id =  0,
                .name = "mei-gsc",
                .num_resources = ARRAY_SIZE(gsc_pvc_resources),
                .resources  = gsc_pvc_resources,
        },
        {
                .id = 1,
                .name = "mei-gscfi",
                .num_resources = ARRAY_SIZE(gscfi_pvc_resources),
                .resources  = gscfi_pvc_resources,
        }
};
#endif

#if IS_ENABLED(CONFIG_AUXILIARY_BUS)
static void gsc_release_dev(struct device *dev)
{
	struct auxiliary_device *aux_dev = to_auxiliary_dev(dev);
	struct mei_aux_device *adev = auxiliary_dev_to_mei_aux_dev(aux_dev);

	kfree(adev);
}
#endif

static void gsc_destroy_one(struct drm_i915_private *i915,
			    struct intel_gsc *gsc, unsigned int intf_id)
{
	struct intel_gsc_intf *intf = &gsc->intf[intf_id];

#if IS_ENABLED(CONFIG_AUXILIARY_BUS)
	if (intf->adev) {
		auxiliary_device_delete(&intf->adev->aux_dev);
		auxiliary_device_uninit(&intf->adev->aux_dev);
		intf->adev = NULL;
	}
#endif
	if (intf->irq >= 0)
		irq_free_desc(intf->irq);
	intf->irq = -1;

	gsc_ext_om_destroy(intf);
}

static void gsc_init_one(struct drm_i915_private *i915, struct intel_gsc *gsc,
			 unsigned int intf_id)
{
	struct pci_dev *pdev = to_pci_dev(i915->drm.dev);
#if IS_ENABLED(CONFIG_AUXILIARY_BUS)
	struct mei_aux_device *adev;
	struct auxiliary_device *aux_dev;
	const struct gsc_def *def;
	bool forcewake_needed = false;
#else
	const struct mfd_cell *cells;
	struct mfd_cell cell;
	size_t lmem_size = 0;
	struct resource res;
#endif
	struct intel_gsc_intf *intf = &gsc->intf[intf_id];
	bool use_polling = false;
	int ret;

	intf->irq = -1;
	intf->id = intf_id;

	/*
	 * On the multi-tile setups the GSC is functional on the first tile only
	 */
	if (gsc_to_gt(gsc)->info.id != 0) {
		drm_dbg(&i915->drm, "Not initializing gsc for remote tiles\n");
		return;
	}

	if (intf_id == 0 && !HAS_HECI_PXP(i915))
		return;

#if IS_ENABLED(CONFIG_AUXILIARY_BUS)
	if (IS_DG1(i915)) {
		def = &gsc_def_dg1[intf_id];
	} else if (IS_XEHPSDV(i915)) {
		def = &gsc_def_xehpsdv[intf_id];
	} else if (IS_DG2(i915)) {
		def = &gsc_def_dg2[intf_id];
	} else if (IS_PONTEVECCHIO(i915)) {
		def = &gsc_def_pvc[intf_id];
		/* Use polling on PVC A-step HW bug Wa */
		if (IS_PVC_BD_STEP(i915, STEP_A0, STEP_B0))
			use_polling = true;
		if (pvc_needs_rc6_wa(i915))
			forcewake_needed = true;
	} else {
		drm_warn_once(&i915->drm, "Unknown platform\n");
		return;
	}

	if (!def->name) {
		drm_warn_once(&i915->drm, "HECI%d is not implemented!\n", intf_id + 1);
		return;
	}

	/* skip irq initialization */
	if (def->use_polling || use_polling)
		goto add_device;

#else
	if (IS_DG1(i915)) {
		cells = intel_gsc_dg1_cell;
	} else if (IS_XEHPSDV(i915)) {
		cells = intel_gsc_dg1_cell;
		/* Use polling on XEHPSDV HW bug Wa */
		use_polling = true;
	} else if (IS_DG2(i915)) {
		cells = intel_gsc_dg2_cell;
		if (intf->id == 0)
			lmem_size = SZ_4M;
	} else if (IS_PONTEVECCHIO(i915)) {
		cells = intel_gsc_pvc_cell;
		/* Use polling on PVC A-step HW bug Wa */
		if (IS_PVC_BD_STEP(i915, STEP_A0, STEP_B0))
			use_polling = true;
	} else {
		drm_warn_once(&i915->drm, "Unknown platform\n");
		return;
	}

	memcpy(&cell, &cells[intf->id], sizeof(cell));

	if (lmem_size) {
		dev_dbg(&pdev->dev, "setting up GSC lmem\n");

		if (gsc_ext_om_alloc(gsc, intf, lmem_size)) {
			dev_err(&pdev->dev, "setting up gsc extended operational memory failed\n");
			goto fail;
		}

		memset(&res, 0, sizeof(res));
		res.start = i915_gem_object_get_dma_address(intf->gem_obj, 0);
		res.end = res.start + lmem_size;

		cell.pdata_size = sizeof(res);
		cell.platform_data = &res;
	}
	/* skip irq initialization */
	if (use_polling)
		goto add_device;
#endif

	intf->irq = irq_alloc_desc(0);
	if (intf->irq < 0) {
		drm_err(&i915->drm, "gsc irq error %d\n", intf->irq);
		goto fail;
	}

	ret = gsc_irq_init(intf->irq);
	if (ret < 0) {
		drm_err(&i915->drm, "gsc irq init failed %d\n", ret);
		goto fail;
	}

add_device:

#if IS_ENABLED(CONFIG_AUXILIARY_BUS)
	adev = kzalloc(sizeof(*adev), GFP_KERNEL);
	if (!adev)
		goto fail;

	if (def->lmem_size) {
		drm_dbg(&i915->drm, "setting up GSC lmem\n");

		if (gsc_ext_om_alloc(gsc, intf, def->lmem_size)) {
			drm_err(&i915->drm, "setting up gsc extended operational memory failed\n");
			kfree(adev);
			goto fail;
		}

		adev->ext_op_mem.start = i915_gem_object_get_dma_address(intf->gem_obj, 0);
		adev->ext_op_mem.end = adev->ext_op_mem.start + def->lmem_size;
	}

	adev->irq = intf->irq;
	adev->bar.parent = &pdev->resource[0];
	adev->bar.start = def->bar + pdev->resource[0].start;
	adev->bar.end = adev->bar.start + def->bar_size - 1;
	adev->bar.flags = IORESOURCE_MEM;
	adev->bar.desc = IORES_DESC_NONE;
	adev->slow_firmware = def->slow_firmware;
	adev->forcewake_needed = forcewake_needed;
	adev->gsc = gsc;
	adev->forcewake_get = intel_gsc_forcewake_get;
	adev->forcewake_put = intel_gsc_forcewake_put;

	aux_dev = &adev->aux_dev;
	aux_dev->name = def->name;
	aux_dev->id = (pci_domain_nr(pdev->bus) << 16) |
		      PCI_DEVID(pdev->bus->number, pdev->devfn);
	aux_dev->dev.parent = &pdev->dev;
	aux_dev->dev.release = gsc_release_dev;

	ret = auxiliary_device_init(aux_dev);
	if (ret < 0) {
		drm_err(&i915->drm, "gsc aux init failed %d\n", ret);
		kfree(adev);
		goto fail;
	}

	ret = auxiliary_device_add(aux_dev);
	if (ret < 0) {
		drm_err(&i915->drm, "gsc aux add failed %d\n", ret);
		/* adev will be freed with the put_device() and .release sequence */
		auxiliary_device_uninit(aux_dev);
		goto fail;
	}
	intf->adev = adev;
#else
	/* this takes a copy of the data, so it is ok to use local vars */
	ret = mfd_add_devices(&pdev->dev, PLATFORM_DEVID_AUTO,
			&cell, 1, &pdev->resource[0], intf->irq, NULL);
	if (ret < 0) {
		dev_err(&pdev->dev, "cell creation failed\n");
		goto fail;
	}
#endif
	return;
fail:
	gsc_destroy_one(i915, gsc, intf->id);
}

static void gsc_irq_handler(struct intel_gt *gt, unsigned int intf_id)
{
	int ret;

	if (intf_id >= INTEL_GSC_NUM_INTERFACES) {
		drm_warn_once(&gt->i915->drm, "GSC irq: intf_id %d is out of range", intf_id);
		return;
	}

	if (!HAS_HECI_GSC(gt->i915)) {
		drm_warn_once(&gt->i915->drm, "GSC irq: not supported");
		return;
	}

	if (gt->gsc.intf[intf_id].irq < 0)
		return;

	ret = generic_handle_irq(gt->gsc.intf[intf_id].irq);
	if (ret)
		drm_err_ratelimited(&gt->i915->drm, "error handling GSC irq: %d\n", ret);
}

void intel_gsc_irq_handler(struct intel_gt *gt, u32 iir)
{
	if (iir & GSC_IRQ_INTF(0))
		gsc_irq_handler(gt, 0);
	if (iir & GSC_IRQ_INTF(1))
		gsc_irq_handler(gt, 1);
}

void intel_gsc_init(struct intel_gsc *gsc, struct drm_i915_private *i915)
{
	unsigned int i;

	if (!HAS_HECI_GSC(i915) || IS_SRIOV_VF(i915))
		return;

	for (i = 0; i < INTEL_GSC_NUM_INTERFACES; i++)
		gsc_init_one(i915, gsc, i);
}

void intel_gsc_fini(struct intel_gsc *gsc)
{
	struct intel_gt *gt = gsc_to_gt(gsc);
	unsigned int i;

	if (!HAS_HECI_GSC(gt->i915) || IS_SRIOV_VF(gt->i915))
		return;

	for (i = 0; i < INTEL_GSC_NUM_INTERFACES; i++)
		gsc_destroy_one(gt->i915, gsc, i);
}
