// SPDX-License-Identifier: GPL-2.0
/*
 * Intel Platform Monitory Technology Telemetry driver
 *
 * Copyright (c) 2020, Intel Corporation.
 * All Rights Reserved.
 *
 * Author: "David E. Box" <david.e.box@linux.intel.com>
 */

#include <linux/auxiliary_bus.h>
#include <linux/intel_vsec.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/overflow.h>

#ifdef BPM_ADD_MODULE_VERSION_MACRO_IN_ALL_MOD
#include <backport/bp_module_version.h>
#endif

#include "class.h"

#define TELEM_SIZE_OFFSET	0x0
#define TELEM_GUID_OFFSET	0x4
#define TELEM_BASE_OFFSET	0x8
#define TELEM_ACCESS(v)		((v) & GENMASK(3, 0))
#define TELEM_TYPE(v)		(((v) & GENMASK(7, 4)) >> 4)
/* size is in bytes */
#define TELEM_SIZE(v)		(((v) & GENMASK(27, 12)) >> 10)

/* Used by client hardware to identify a fixed telemetry entry*/
#define TELEM_CLIENT_FIXED_BLOCK_GUID	0x10000000

#define NUM_BYTES_QWORD(v)	((v) << 3)
#define SAMPLE_ID_OFFSET(v)	((v) << 3)

static DEFINE_MUTEX(list_lock);

enum telem_type {
	TELEM_TYPE_PUNIT = 0,
	TELEM_TYPE_CRASHLOG,
	TELEM_TYPE_PUNIT_FIXED,
};

struct pmt_telem_priv {
	int				num_entries;
	struct intel_pmt_entry		entry[];
};

static bool pmt_telem_region_overlaps(struct intel_pmt_entry *entry,
				      struct device *dev)
{
	u32 guid = readl(entry->disc_table + TELEM_GUID_OFFSET);

	if (intel_pmt_is_early_client_hw(dev)) {
		u32 type = TELEM_TYPE(readl(entry->disc_table));

		pr_debug("%s: is early client hardware, telem_type %u\n", __func__, type);
		if ((type == TELEM_TYPE_PUNIT_FIXED) ||
		    (guid == TELEM_CLIENT_FIXED_BLOCK_GUID))
			return true;
	} else
		pr_debug("%s: is not early client hardware\n", __func__);

	return false;
}

static int pmt_telem_header_decode(struct intel_pmt_entry *entry,
				   struct intel_pmt_header *header,
				   struct device *dev)
{
	void __iomem *disc_table = entry->disc_table;

	if (pmt_telem_region_overlaps(entry, dev))
		return 1;

	header->access_type = TELEM_ACCESS(readl(disc_table));
	header->guid = readl(disc_table + TELEM_GUID_OFFSET);
	header->base_offset = readl(disc_table + TELEM_BASE_OFFSET);
	if (entry->base_adjust) {
		u32 new_base = header->base_offset + entry->base_adjust;
		dev_dbg(dev, "Adjusting baseoffset from 0x%x to 0x%x\n",
			header->base_offset, new_base);
		header->base_offset = new_base;
	}

	/* Size is measured in DWORDS, but accessor returns bytes */
	header->size = TELEM_SIZE(readl(disc_table));

	/*
	 * Some devices may expose non-functioning entries that are
	 * reserved for future use. They have zero size. Do not fail
	 * probe for these. Just ignore them.
	 */
	if (header->size == 0)
		return 1;

	entry->header.access_type = header->access_type;
	entry->header.guid = header->guid;
	entry->header.base_offset = header->base_offset;
	entry->header.size = header->size;

	return 0;
}

static DEFINE_XARRAY_ALLOC(telem_array);
static struct intel_pmt_namespace pmt_telem_ns = {
	.name = "telem",
	.xa = &telem_array,
	.pmt_header_decode = pmt_telem_header_decode,
};

/* Called when all users unregister and the device is removed */
static void pmt_telem_ep_release(struct kref *kref)
{
	struct telem_endpoint *ep;

	pr_debug("%s: begin release kref\n", __func__);
	ep = container_of(kref, struct telem_endpoint, kref);
	kfree(ep);
	pr_debug("%s: end release kref for %px\n", __func__, ep);
}

/*
 * driver api
 */
int pmt_telem_get_next_endpoint(int start)
{
	struct intel_pmt_entry *entry;
	unsigned long found_idx;

	mutex_lock(&list_lock);
	xa_for_each_start(&telem_array, found_idx, entry, start) {
		/*
		 * Return first found index after start.
		 * 0 is not valid id.
		 */
		if (found_idx > start)
			break;
	}
	mutex_unlock(&list_lock);

	return found_idx == start ? 0 : found_idx;
}
EXPORT_SYMBOL_GPL(pmt_telem_get_next_endpoint);

struct telem_endpoint *pmt_telem_register_endpoint(int devid)
{
	struct intel_pmt_entry *entry;
	unsigned long index = devid;

	mutex_lock(&list_lock);
	entry = xa_find(&telem_array, &index, index, XA_PRESENT);
	if (!entry) {
		mutex_unlock(&list_lock);
		return ERR_PTR(-ENXIO);
	}

	kref_get(&entry->ep->kref);

	pr_debug("%s: kref for [%px] is now %d\n", __func__, entry, kref_read(&entry->ep->kref));
	mutex_unlock(&list_lock);

	return entry->ep;
}
EXPORT_SYMBOL_GPL(pmt_telem_register_endpoint);

void pmt_telem_unregister_endpoint(struct telem_endpoint *ep)
{
	kref_put(&ep->kref, pmt_telem_ep_release);
	pr_debug("%s: kref for [%px] is now %d\n", __func__, ep, kref_read(&ep->kref));
}
EXPORT_SYMBOL(pmt_telem_unregister_endpoint);

int pmt_telem_get_endpoint_info(int devid,
				struct telem_endpoint_info *info)
{
	struct intel_pmt_entry *entry;
	unsigned long index = devid;
	int err = 0;

	if (!info)
		return -EINVAL;

	mutex_lock(&list_lock);
	entry = xa_find(&telem_array, &index, index, XA_PRESENT);
	if (!entry) {
		err = -ENXIO;
		goto unlock;
	}

	info->pdev = entry->ep->parent;
	info->header = entry->ep->header;

unlock:
	mutex_unlock(&list_lock);
	return err;

}
EXPORT_SYMBOL_GPL(pmt_telem_get_endpoint_info);

int
pmt_telem_read(struct telem_endpoint *ep, u32 id, u64 *data, u32 count)
{
	u32 offset, size;

	if (!ep->present)
		return -ENODEV;

	offset = SAMPLE_ID_OFFSET(id);
	size = ep->header.size;

	if ((offset + NUM_BYTES_QWORD(count)) > size)
		return -EINVAL;

	pr_debug("%s: Reading id %d, offset 0x%x, count %d, base %px\n",
		 __func__, id, SAMPLE_ID_OFFSET(id), count, ep->base);

	pm_runtime_get_sync(&ep->parent->dev);
	memcpy_fromio(data, ep->base + offset, NUM_BYTES_QWORD(count));
	pm_runtime_mark_last_busy(&ep->parent->dev);
	pm_runtime_put_autosuspend(&ep->parent->dev);

	return ep->present ? 0 : -EPIPE;
}
EXPORT_SYMBOL_GPL(pmt_telem_read);

void pmt_telem_runtime_pm_get(struct telem_endpoint *ep)
{
	pm_runtime_get_sync(&ep->parent->dev);
}
EXPORT_SYMBOL(pmt_telem_runtime_pm_get);

void pmt_telem_runtime_pm_put(struct telem_endpoint *ep)
{
	pm_runtime_put_sync(&ep->parent->dev);
}
EXPORT_SYMBOL(pmt_telem_runtime_pm_put);

struct telem_endpoint *
pmt_telem_find_and_register_endpoint(struct pci_dev *pcidev, u32 guid, u16 pos)
{
	int devid = 0;
	int inst = 0;
	int err = 0;

	while ((devid = pmt_telem_get_next_endpoint(devid))) {
		struct telem_endpoint_info ep_info;

		err = pmt_telem_get_endpoint_info(devid, &ep_info);
		if (err)
			return ERR_PTR(err);

		if (ep_info.header.guid == guid && ep_info.pdev == pcidev) {
			if (inst == pos)
				return pmt_telem_register_endpoint(devid);
			++inst;
		}
	}

	return ERR_PTR(-ENXIO);
}
EXPORT_SYMBOL(pmt_telem_find_and_register_endpoint);

static int pmt_telem_add_endpoint(struct device *dev,
				  struct pmt_telem_priv *priv,
				  struct intel_pmt_entry *entry)
{
	struct telem_endpoint *ep;

	/*
	 * Endpoint lifetimes are managed by kref, not devres.
	 */
	entry->ep = kzalloc(sizeof(*(entry->ep)), GFP_KERNEL);
	if (!entry->ep)
		return -ENOMEM;

	ep = entry->ep;
	ep->dev = dev;
	ep->parent = to_pci_dev(dev->parent);
	ep->header.access_type = entry->header.access_type;
	ep->header.guid = entry->header.guid;
	ep->header.base_offset = entry->header.base_offset;
	ep->header.size = entry->header.size;

	/* use the already ioremapped entry base */
	ep->base = entry->base;
	ep->present = true;

	kref_init(&ep->kref);

	return 0;
}

static void pmt_telem_remove(struct auxiliary_device *auxdev)
{
	struct pmt_telem_priv *priv = auxiliary_get_drvdata(auxdev);
	struct intel_pmt_entry *entry;
	int i;

	dev_dbg(&auxdev->dev, "%s\n", __func__);

	for (i = 0, entry = priv->entry; i < priv->num_entries; i++, entry++) {
		kref_put(&priv->entry[i].ep->kref, pmt_telem_ep_release);
		dev_dbg(&auxdev->dev, "kref count of ep #%d [%px] is %d\n", i, entry->ep, kref_read(&entry->ep->kref));
		intel_pmt_dev_destroy(&priv->entry[i], &pmt_telem_ns);
	}
};

static int pmt_telem_probe(struct auxiliary_device *auxdev, const struct auxiliary_device_id *id)
{
	struct intel_vsec_device *intel_vsec_dev = auxdev_to_ivdev(auxdev);
	struct intel_pmt_entry *entry;
	struct pmt_telem_priv *priv;
	size_t size;
	int i, ret;

	size = struct_size(priv, entry, intel_vsec_dev->num_resources);
	priv = devm_kzalloc(&auxdev->dev, size, GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	auxiliary_set_drvdata(auxdev, priv);

	for (i = 0, entry = &priv->entry[priv->num_entries];
	     i < intel_vsec_dev->num_resources;
	     i++, entry++) {
		dev_dbg(&auxdev->dev, "Getting resource %d\n", i);
		entry->base_adjust = intel_vsec_dev->info->base_adjust;

		ret = intel_pmt_dev_create(entry, &pmt_telem_ns, intel_vsec_dev, i);
		if (ret < 0)
			goto abort_probe;
		if (ret)
			continue;

		priv->num_entries++;

		ret = pmt_telem_add_endpoint(&auxdev->dev, priv, entry);
		if (ret)
			goto abort_probe;

		dev_dbg(&auxdev->dev, "kref count of ep #%d [%px] is %d\n", i, entry->ep, kref_read(&entry->ep->kref));
	}

	return 0;

abort_probe:
	pmt_telem_remove(auxdev);
	return ret;
}

static const struct auxiliary_device_id pmt_telem_id_table[] = {
	{ .name = "intel_vsec.telemetry" },
	{}
};
MODULE_DEVICE_TABLE(auxiliary, pmt_telem_id_table);

static struct auxiliary_driver pmt_telem_aux_driver = {
	.id_table	= pmt_telem_id_table,
	.remove		= pmt_telem_remove,
	.probe		= pmt_telem_probe,
};

static int __init pmt_telem_init(void)
{
	printk(KERN_INFO "BACKPORTED VSEC TELEMETRY INIT\n");
	return auxiliary_driver_register(&pmt_telem_aux_driver);
}
module_init(pmt_telem_init);

static void __exit pmt_telem_exit(void)
{
	auxiliary_driver_unregister(&pmt_telem_aux_driver);
	xa_destroy(&telem_array);

}
module_exit(pmt_telem_exit);

MODULE_AUTHOR("David E. Box <david.e.box@linux.intel.com>");
#ifdef BPM_ADD_MODULE_VERSION_MACRO_IN_ALL_MOD
MODULE_VERSION(BACKPORT_MOD_VER);
#endif
MODULE_DESCRIPTION("Intel PMT Telemetry driver");
MODULE_LICENSE("GPL v2");
