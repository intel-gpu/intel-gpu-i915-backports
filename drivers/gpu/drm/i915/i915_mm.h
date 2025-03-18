/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef __I915_MM_H__
#define __I915_MM_H__

#include <linux/types.h>

struct vm_area_struct;
struct scatterlist;

int remap_io_sg(struct vm_area_struct *vma,
		unsigned long addr, unsigned long size,
		struct scatterlist *sgl, unsigned long offset,
		resource_size_t iobase, bool write);

#endif /* __I915_MM_H__ */
