/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */
#ifndef __INTEL_SA_MEDIA__
#define __INTEL_SA_MEDIA__

#include <linux/types.h>

#include "gt/intel_gt.h"

int intel_sa_mediagt_setup(struct intel_gt *gt, unsigned int id,
			   phys_addr_t phys_addr, u32 gsi_offset);

#endif /* __INTEL_SA_MEDIA_H__ */
