// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright © 2023 Intel Corporation
 */

#include <linux/list.h>

#include "i915_vfio_pci.h"

#define BITSTREAM_MAGIC 0x4f49465635313949ULL
#define BITSTREAM_VERSION 0x1

struct i915_vfio_data_device_desc {
	/** @magic: constant, driver specific value */
	u64 magic;
	/** @version: device data version */
	u64 version;
	u16 vendor;
	u16 device;
	u32 rsvd;
	/** @flags: optional flags */
	u64 flags;
} __packed;

enum i915_vfio_pci_migration_data_type {
	I915_VFIO_DATA_DESC = 0,
	I915_VFIO_DATA_GGTT,
	I915_VFIO_DATA_LMEM,
	I915_VFIO_DATA_GUC,
	I915_VFIO_DATA_CCS,
	I915_VFIO_DATA_DONE,
};

static const char *i915_vfio_data_type_str(enum i915_vfio_pci_migration_data_type type)
{
	switch (type) {
	case I915_VFIO_DATA_DESC: return "DESC";
	case I915_VFIO_DATA_GGTT: return "GGTT";
	case I915_VFIO_DATA_LMEM: return "LMEM";
	case I915_VFIO_DATA_GUC: return "GUC";
	case I915_VFIO_DATA_CCS: return "CCS";
	case I915_VFIO_DATA_DONE: return "DONE";
	default: return "";
	}
}

static int
__i915_vfio_produce_prepare(struct i915_vfio_pci_migration_file *migf, unsigned int tile, u32 type)
{
	struct i915_vfio_pci_core_device *i915_vdev = migf->i915_vdev;
	struct device *dev = i915_vdev_to_dev(i915_vdev);
	const struct i915_vfio_pci_resource_ops *ops;
	struct i915_vfio_pci_migration_data *data;
	ssize_t size;
	void *buf;
	int ret;

	switch (type) {
	case I915_VFIO_DATA_DESC:
		break;
	case I915_VFIO_DATA_GGTT:
		ops = &i915_vdev->pf_ops->ggtt;
		break;
	case I915_VFIO_DATA_GUC:
		ops = &i915_vdev->pf_ops->fw;
		break;
	default:
		return -EINVAL;
	}

	size = (type == I915_VFIO_DATA_DESC) ? sizeof(struct i915_vfio_data_device_desc) :
					       ops->size(i915_vdev->pf, i915_vdev->vfid, tile);

	if (!size || size == -ENODEV) {
		dev_dbg(dev, "Skipping %s for tile%u, ret=%zd\n",
			i915_vfio_data_type_str(type), tile, size);

		return 0;
	} else if (size < 0) {
		dev_dbg(dev, "Error querying %s size for tile%u, ret=%pe\n",
			i915_vfio_data_type_str(type), tile, ERR_PTR(size));
		return size;
	}

	buf = kvmalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
		goto out_free_buf;
	}

	data->hdr.type = type;
	data->hdr.tile = tile;
	data->hdr.offset = 0;
	data->hdr.size = size;
	data->hdr.flags = 0;

	data->pos = 0;
	data->buf.vaddr = buf;
	data->buf.size = size;

	list_add(&data->link, &migf->save_data);

	return 0;

out_free_buf:
	kvfree(buf);

	return ret;
}

static int __i915_vfio_produce(struct i915_vfio_pci_migration_file *migf,
			       struct i915_vfio_pci_migration_data *data)
{
	struct i915_vfio_pci_core_device *i915_vdev = migf->i915_vdev;
	struct device *dev = i915_vdev_to_dev(i915_vdev);
	const struct i915_vfio_pci_resource_ops *ops;

	switch (data->hdr.type) {
	case I915_VFIO_DATA_GGTT:
		ops = &i915_vdev->pf_ops->ggtt;
		break;
	case I915_VFIO_DATA_GUC:
		ops = &i915_vdev->pf_ops->fw;
		break;
	default:
		return -EINVAL;
	}

	dev_dbg(dev, "Producing %s for tile%llu, size=%llu\n",
		i915_vfio_data_type_str(data->hdr.type), data->hdr.tile, data->hdr.size);

	return ops->save(i915_vdev->pf, i915_vdev->vfid, data->hdr.tile, data->buf.vaddr,
			 data->hdr.size);
}

static inline bool i915_vfio_data_is_ccs(struct i915_vfio_pci_migration_data *data)
{
	return (data->hdr.type == I915_VFIO_DATA_CCS) ? true : false;
}

static inline bool i915_vfio_data_is_chunkable(struct i915_vfio_pci_migration_data *data)
{
	switch (data->hdr.type) {
	case I915_VFIO_DATA_LMEM:
	case I915_VFIO_DATA_CCS:
		return true;
	default:
		return false;
	}
}

static int __i915_vfio_consume_prepare(struct i915_vfio_pci_migration_file *migf,
				       struct i915_vfio_pci_migration_data *data)
{
	if (data->buf.vaddr)
		return -EPERM;

	data->buf.size = data->hdr.size;
	data->buf.vaddr = kvmalloc(data->buf.size, GFP_KERNEL);
	if (!data->buf.vaddr)
		return -ENOMEM;

	return 0;
}

static int __i915_vfio_consume(struct i915_vfio_pci_migration_file *migf,
			       struct i915_vfio_pci_migration_data *data)
{
	struct i915_vfio_pci_core_device *i915_vdev = migf->i915_vdev;
	struct device *dev = i915_vdev_to_dev(i915_vdev);
	const struct i915_vfio_pci_resource_ops *ops;

	switch (data->hdr.type) {
	case I915_VFIO_DATA_GGTT:
		ops = &i915_vdev->pf_ops->ggtt;
		break;
	case I915_VFIO_DATA_GUC:
		ops = &i915_vdev->pf_ops->fw;
		break;
	default:
		return -EINVAL;
	}

	dev_dbg(dev, "Consuming %s for tile%llu, size=%llu\n",
		i915_vfio_data_type_str(data->hdr.type), data->hdr.tile, data->hdr.size);

	return ops->load(i915_vdev->pf, i915_vdev->vfid, data->hdr.tile, data->buf.vaddr,
			 data->hdr.size);
}

#define __resource(x, type) \
static int \
i915_vfio_produce_prepare_##x(struct i915_vfio_pci_migration_file *migf, unsigned int tile) \
{ \
	return __i915_vfio_produce_prepare(migf, tile, type); \
} \
static int \
i915_vfio_produce_##x(struct i915_vfio_pci_migration_file *migf, \
		      struct i915_vfio_pci_migration_data *data) \
{ \
	return __i915_vfio_produce(migf, data); \
} \
static int \
i915_vfio_consume_##x(struct i915_vfio_pci_migration_file *migf, \
		      struct i915_vfio_pci_migration_data *data) \
{ \
	return __i915_vfio_consume(migf, data); \
}

__resource(ggtt, I915_VFIO_DATA_GGTT);
__resource(fw, I915_VFIO_DATA_GUC);

void *i915_vfio_smem_alloc(struct pci_dev *pdev, size_t size)
{
#if IS_ENABLED(CPTCFG_I915_VFIO_PCI_TEST)
	return kvmalloc(size, GFP_KERNEL);
#else
	return i915_sriov_smem_alloc(pdev, size);
#endif
}

void i915_vfio_smem_free(struct pci_dev *pdev, const void *obj)
{
#if IS_ENABLED(CPTCFG_I915_VFIO_PCI_TEST)
	kvfree(obj);
#else
	i915_sriov_smem_free(pdev, obj);

#endif
}

#define MAX_CCS_CHUNK_SIZE SZ_256K
#define COMPRESSION_RATIO 256

static int
__i915_vfio_produce_prepare_chunkable(struct i915_vfio_pci_migration_file *migf, unsigned int tile,
				      u32 type)
{
	struct i915_vfio_pci_core_device *i915_vdev = migf->i915_vdev;
	struct device *dev = i915_vdev_to_dev(i915_vdev);
	const struct i915_vfio_pci_chunkable_resource_ops *ops;
	struct i915_vfio_pci_migration_data *data;
	ssize_t size, buf_size;
	void *buf;
	int ret;

	switch (type) {
	case I915_VFIO_DATA_LMEM:
		ops = &i915_vdev->pf_ops->lmem;
		buf_size = SZ_64M;
		break;
	case I915_VFIO_DATA_CCS:
		ops = &i915_vdev->pf_ops->ccs;
		buf_size = MAX_CCS_CHUNK_SIZE;
		break;
	default:
		return -EINVAL;
	}

	size = ops->size(i915_vdev->pf, i915_vdev->vfid, tile);

	if (!size || size == -ENODEV) {
		dev_dbg(dev, "Skipping %s for tile%u, ret=%zd\n",
			i915_vfio_data_type_str(type), tile, size);

		return 0;
	} else if (size < 0) {
		dev_dbg(dev, "Error querying %s size for tile%u, ret=%pe\n",
			i915_vfio_data_type_str(type), tile, ERR_PTR(size));
		return size;
	}

	buf_size = min(buf_size, size);

	if (IS_ENABLED(CPTCFG_I915_VFIO_PCI_TEST))
		buf_size = size / 8;

	buf = i915_vfio_smem_alloc(i915_vdev->pf, buf_size);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
		goto out_free_buf;
	}

	data->hdr.type = type;
	data->hdr.tile = tile;
	data->hdr.offset = 0;
	data->hdr.size = size;
	data->hdr.flags = 0;

	data->pos = 0;
	data->buf.vaddr = buf;
	data->buf.size = buf_size;

	list_add(&data->link, &migf->save_data);

	return 0;

out_free_buf:
	i915_vfio_smem_free(i915_vdev->pf, buf);

	return ret;
}

static int
__i915_vfio_consume_prepare_chunkable(struct i915_vfio_pci_migration_file *migf,
				      struct i915_vfio_pci_migration_data *data)
{
	struct i915_vfio_pci_core_device *i915_vdev = migf->i915_vdev;

	if (data->buf.vaddr)
		return -EPERM;

	switch (data->hdr.type) {
	case I915_VFIO_DATA_LMEM:
		data->buf.size = SZ_64M;
		break;
	case I915_VFIO_DATA_CCS:
		data->buf.size = MAX_CCS_CHUNK_SIZE;
		break;
	default:
		return -EINVAL;
	}

	if (IS_ENABLED(CPTCFG_I915_VFIO_PCI_TEST))
		data->buf.size = data->hdr.size / 8;

	data->buf.vaddr = i915_vfio_smem_alloc(i915_vdev->pf, data->buf.size);
	if (IS_ERR(data->buf.vaddr))
		return PTR_ERR(data->buf.vaddr);

	return 0;
}

static ssize_t
__i915_vfio_produce_chunk(struct i915_vfio_pci_migration_file *migf,
			  struct i915_vfio_pci_migration_data *data, u64 offset, size_t chunk_size)
{
	struct i915_vfio_pci_core_device *i915_vdev = migf->i915_vdev;
	const struct i915_vfio_pci_chunkable_resource_ops *ops;
	struct device *dev = i915_vdev_to_dev(i915_vdev);

	switch (data->hdr.type) {
	case I915_VFIO_DATA_LMEM:
		ops = &i915_vdev->pf_ops->lmem;
		break;
	case I915_VFIO_DATA_CCS:
		ops = &i915_vdev->pf_ops->ccs;
		break;
	default:
		return -EINVAL;
	}

	dev_dbg(dev, "Producing %s for tile%llu, offset=%llu, size=%zu\n",
		i915_vfio_data_type_str(data->hdr.type), data->hdr.tile, offset, chunk_size);

	return ops->save(i915_vdev->pf, i915_vdev->vfid, data->hdr.tile, data->buf.vaddr,
			 i915_vfio_data_is_ccs(data) ? offset * COMPRESSION_RATIO : offset,
			 i915_vfio_data_is_ccs(data) ? chunk_size * COMPRESSION_RATIO : chunk_size);
}

static int
__i915_vfio_consume_chunk(struct i915_vfio_pci_migration_file *migf,
			  struct i915_vfio_pci_migration_data *data, u64 offset, size_t chunk_size)
{
	struct i915_vfio_pci_core_device *i915_vdev = migf->i915_vdev;
	const struct i915_vfio_pci_chunkable_resource_ops *ops;
	struct device *dev = i915_vdev_to_dev(i915_vdev);

	switch (data->hdr.type) {
	case I915_VFIO_DATA_LMEM:
		ops = &i915_vdev->pf_ops->lmem;
		break;
	case I915_VFIO_DATA_CCS:
		ops = &i915_vdev->pf_ops->ccs;
		break;
	default:
		return -EINVAL;
	}

	dev_dbg(dev, "Consuming %s for tile%llu, offset=%llu, size=%zu\n",
		i915_vfio_data_type_str(data->hdr.type), data->hdr.tile, offset, chunk_size);

	return ops->load(i915_vdev->pf, i915_vdev->vfid, data->hdr.tile, data->buf.vaddr,
			 i915_vfio_data_is_ccs(data) ? offset * COMPRESSION_RATIO : offset,
			 i915_vfio_data_is_ccs(data) ? chunk_size * COMPRESSION_RATIO : chunk_size);
}

#define __chunkable_resource(x, type) \
static int \
i915_vfio_produce_prepare_##x(struct i915_vfio_pci_migration_file *migf, unsigned int tile) \
{ \
	return __i915_vfio_produce_prepare_chunkable(migf, tile, type); \
} \
static ssize_t \
i915_vfio_produce_chunk_##x(struct i915_vfio_pci_migration_file *migf, \
			    struct i915_vfio_pci_migration_data *data, u64 offset, \
			    size_t chunk_size) \
{ \
	return __i915_vfio_produce_chunk(migf, data, offset, chunk_size); \
} \
static int \
i915_vfio_consume_chunk_##x(struct i915_vfio_pci_migration_file *migf, \
			    struct i915_vfio_pci_migration_data *data, u64 offset, \
			    size_t chunk_size) \
{ \
	return __i915_vfio_consume_chunk(migf, data, offset, chunk_size); \
}

__chunkable_resource(lmem, I915_VFIO_DATA_LMEM);
__chunkable_resource(ccs, I915_VFIO_DATA_CCS);


static int
i915_vfio_produce_prepare_desc(struct i915_vfio_pci_migration_file *migf)
{
	return __i915_vfio_produce_prepare(migf, 0, I915_VFIO_DATA_DESC);
}

static int i915_vfio_produce_desc(struct i915_vfio_pci_migration_file *migf,
				  struct i915_vfio_pci_migration_data *data)
{
	struct i915_vfio_pci_core_device *i915_vdev = migf->i915_vdev;
	struct device *dev = i915_vdev_to_dev(i915_vdev);
	struct i915_vfio_data_device_desc desc;

	desc.magic = BITSTREAM_MAGIC;
	desc.version = BITSTREAM_VERSION;
	desc.vendor = i915_vdev_to_pdev(migf->i915_vdev)->vendor;
	desc.device = i915_vdev_to_pdev(migf->i915_vdev)->device;
	desc.flags = 0x0;

	dev_dbg(dev, "Producing %s, size=%llu\n",
		i915_vfio_data_type_str(I915_VFIO_DATA_DESC), data->hdr.size);

	memcpy(data->buf.vaddr, &desc, sizeof(desc));

	return 0;
}

static int i915_vfio_consume_desc(struct i915_vfio_pci_migration_file *migf,
				  struct i915_vfio_pci_migration_data *data)
{
	struct i915_vfio_pci_core_device *i915_vdev = migf->i915_vdev;
	struct device *dev = i915_vdev_to_dev(i915_vdev);
	struct i915_vfio_data_device_desc *desc = data->buf.vaddr;

	dev_dbg(dev, "Consuming %s, size=%llu\n",
		i915_vfio_data_type_str(I915_VFIO_DATA_DESC), data->hdr.size);

	if (data->hdr.size != sizeof(*desc))
		return -EINVAL;

	if (desc->magic != BITSTREAM_MAGIC)
		return -EINVAL;

	if (desc->version != BITSTREAM_VERSION)
		return -EINVAL;

	if (desc->vendor != i915_vdev_to_pdev(migf->i915_vdev)->vendor)
		return -EINVAL;

	if (desc->device != i915_vdev_to_pdev(migf->i915_vdev)->device)
		return -EINVAL;

	return 0;
}

static int i915_vfio_pci_produce_data(struct i915_vfio_pci_migration_file *migf,
				      struct i915_vfio_pci_migration_data *data)
{
	switch (data->hdr.type) {
	case I915_VFIO_DATA_DESC:
		if (data->hdr.tile)
			return 0;
		return i915_vfio_produce_desc(migf, data);
	case I915_VFIO_DATA_GGTT:
		return i915_vfio_produce_ggtt(migf, data);
	case I915_VFIO_DATA_GUC:
		return i915_vfio_produce_fw(migf, data);
	default:
		return -EINVAL;
	}
}

static ssize_t i915_vfio_consume_data(struct i915_vfio_pci_migration_file *migf,
				      struct i915_vfio_pci_migration_data *data)
{
	switch (data->hdr.type) {
	case I915_VFIO_DATA_DESC:
		return i915_vfio_consume_desc(migf, data);
	case I915_VFIO_DATA_GGTT:
		return i915_vfio_consume_ggtt(migf, data);
	case I915_VFIO_DATA_GUC:
		return i915_vfio_consume_fw(migf, data);
	default:
		return -EINVAL;
	}
}

static ssize_t
i915_vfio_produce_data_chunk(struct i915_vfio_pci_migration_file *migf,
			     struct i915_vfio_pci_migration_data *data, u64 offset,
			     size_t chunk_size)
{
	switch (data->hdr.type) {
	case I915_VFIO_DATA_LMEM:
		return i915_vfio_produce_chunk_lmem(migf, data, offset, chunk_size);
	case I915_VFIO_DATA_CCS:
		return i915_vfio_produce_chunk_ccs(migf, data, offset, chunk_size);
	default:
		return -EINVAL;
	}
}

static int
i915_vfio_consume_data_chunk(struct i915_vfio_pci_migration_file *migf,
				 struct i915_vfio_pci_migration_data *data, u64 offset,
				 size_t chunk_size)
{
	switch (data->hdr.type) {
	case I915_VFIO_DATA_LMEM:
		return i915_vfio_consume_chunk_lmem(migf, data, offset, chunk_size);
	case I915_VFIO_DATA_CCS:
		return i915_vfio_consume_chunk_ccs(migf, data, offset, chunk_size);
	default:
		return -EINVAL;
	}
}

static void i915_vfio_save_data_free(struct i915_vfio_pci_migration_file *migf,
				     struct i915_vfio_pci_migration_data *data)
{
	struct i915_vfio_pci_core_device *i915_vdev = migf->i915_vdev;

	list_del_init(&data->link);

	if (i915_vfio_data_is_chunkable(data))
		i915_vfio_smem_free(i915_vdev->pf, data->buf.vaddr);
	else
		kvfree(data->buf.vaddr);

	kfree(data);
}

void i915_vfio_save_data_release(struct i915_vfio_pci_migration_file *migf)
{
	struct i915_vfio_pci_migration_data *data, *next;

	if (!migf)
		return;

	list_for_each_entry_safe(data, next, &migf->save_data, link)
		i915_vfio_save_data_free(migf, data);
}

static void i915_vfio_resume_data_free(struct i915_vfio_pci_migration_file *migf,
				       struct i915_vfio_pci_migration_data *data)
{
	struct i915_vfio_pci_core_device *i915_vdev = migf->i915_vdev;
	data->hdr_processed = false;
	data->pos = 0;

	if (i915_vfio_data_is_chunkable(data))
		i915_vfio_smem_free(i915_vdev->pf, data->buf.vaddr);
	else
		kvfree(data->buf.vaddr);

	data->buf.vaddr = NULL;
}

static int
i915_vfio_produce_prepare(struct i915_vfio_pci_migration_file *migf,
			  enum i915_vfio_pci_migration_data_type type, unsigned int tile)
{
	switch (type) {
	case I915_VFIO_DATA_DESC:
		if (tile)
			return 0;
		return i915_vfio_produce_prepare_desc(migf);
	case I915_VFIO_DATA_GGTT:
		return i915_vfio_produce_prepare_ggtt(migf, tile);
	case I915_VFIO_DATA_LMEM:
		return i915_vfio_produce_prepare_lmem(migf, tile);
	case I915_VFIO_DATA_GUC:
		return i915_vfio_produce_prepare_fw(migf, tile);
	case I915_VFIO_DATA_CCS:
		return i915_vfio_produce_prepare_ccs(migf, tile);
	default:
		return -EINVAL;
	}
}

int i915_vfio_save_data_prepare(struct i915_vfio_pci_migration_file *migf)
{
	enum i915_vfio_pci_migration_data_type type;
	unsigned int tile;
	int ret;

	for (tile = 0; tile < I915_VFIO_MAX_TILE; tile++) {
		for (type = I915_VFIO_DATA_DESC; type < I915_VFIO_DATA_DONE; type++) {
			ret = i915_vfio_produce_prepare(migf, type, tile);
			if (ret)
				goto out;
		}
	}

	return 0;

out:
	i915_vfio_save_data_release(migf);
	return ret;
}

static int i915_vfio_pci_consume_prepare(struct i915_vfio_pci_migration_file *migf,
					 struct i915_vfio_pci_migration_data *data)
{
	switch (data->hdr.type) {
	case I915_VFIO_DATA_DESC:
	case I915_VFIO_DATA_GGTT:
	case I915_VFIO_DATA_GUC:
		return __i915_vfio_consume_prepare(migf, data);
	case I915_VFIO_DATA_LMEM:
	case I915_VFIO_DATA_CCS:
		return __i915_vfio_consume_prepare_chunkable(migf, data);
	default:
		return -EINVAL;
	}
}

ssize_t i915_vfio_data_read(struct i915_vfio_pci_migration_file *migf, char __user *ubuf,
			    size_t len)
{
	struct i915_vfio_pci_migration_data *data;
	size_t len_remain, len_hdr;
	loff_t buf_pos;
	ssize_t ret;

	data = list_first_entry_or_null(&migf->save_data, typeof(*data), link);
	if (!data)
		return 0;

	if (!data->hdr_processed) {
		if (len < sizeof(data->hdr))
			return -EINVAL;

		ret = migf->copy_to(ubuf, &data->hdr, sizeof(data->hdr));
		if (ret)
			return -EFAULT;

		len_hdr = sizeof(data->hdr);
		ubuf += sizeof(data->hdr);
		data->hdr_processed = true;
	} else {
		len_hdr = 0;
	}

	len_remain = len_hdr + data->hdr.size - data->pos;
	len = min(len, len_remain);

	buf_pos = data->pos % data->buf.size;

	if (i915_vfio_data_is_chunkable(data)) {
		size_t buf_remain = data->buf.size - buf_pos;

		len = min(len, buf_remain);
	}

	/* TODO: produce data asynchronously */
	if (!buf_pos && len_remain) {
		ret = i915_vfio_data_is_chunkable(data) ?
			i915_vfio_produce_data_chunk(migf, data, data->pos, min(len_remain,
						     data->buf.size)) :
			i915_vfio_pci_produce_data(migf, data);

		if (ret < 0)
			return ret;
	}

	if (migf->copy_to(ubuf, data->buf.vaddr + buf_pos, len - len_hdr))
		return -EFAULT;

	if (len < len_remain)
		data->pos += len - len_hdr;
	else
		i915_vfio_save_data_free(migf, data);

	return len;
}

ssize_t i915_vfio_data_write(struct i915_vfio_pci_migration_file *migf, const char __user *ubuf,
			     size_t len)
{
	struct i915_vfio_pci_migration_data *data = &migf->resume_data;
	size_t len_remain, len_hdr;
	loff_t buf_pos;
	int ret;

	if (!data->hdr_processed) {
		if (len < sizeof(data->hdr))
			return -EINVAL;

		if (migf->copy_from(&data->hdr, ubuf, sizeof(data->hdr)))
			return -EFAULT;

		len_hdr = sizeof(data->hdr);
		ubuf += sizeof(data->hdr);
		data->hdr_processed = true;

		ret = i915_vfio_pci_consume_prepare(migf, data);
		if (ret)
			return ret;
	} else {
		len_hdr = 0;
	}

	len_remain = len_hdr + data->hdr.size - data->pos;
	len = min(len, len_remain);

	buf_pos = data->pos % data->buf.size;

	if (i915_vfio_data_is_chunkable(data)) {
		size_t buf_remain = data->buf.size - buf_pos;

		len = min(len, buf_remain);
	}

	if (migf->copy_from(data->buf.vaddr + buf_pos, ubuf, len - len_hdr)) {
		ret = -EFAULT;
		goto out_free;
	}

	data->pos += len - len_hdr;
	buf_pos += len - len_hdr;

	/* TODO: consume data asynchronously */
	if ((buf_pos == data->buf.size || data->pos == data->hdr.size) && len_remain) {
		if (i915_vfio_data_is_chunkable(data)) {
			u64 offset = (buf_pos == data->buf.size) ? data->pos - data->buf.size :
								   data->pos - buf_pos;
			size_t size = (buf_pos == data->buf.size) ? data->buf.size : buf_pos;

			ret = i915_vfio_consume_data_chunk(migf, data, offset, size);
		} else {
			ret = i915_vfio_consume_data(migf, data);
		}

		if (ret)
			goto out_free;
	}

	if (len >= len_remain)
		i915_vfio_resume_data_free(migf, data);

	return len;

out_free:
	i915_vfio_resume_data_free(migf, data);
	return ret;
}

#if IS_ENABLED(CPTCFG_I915_VFIO_PCI_TEST)
#include "test/data_test.c"
#endif
