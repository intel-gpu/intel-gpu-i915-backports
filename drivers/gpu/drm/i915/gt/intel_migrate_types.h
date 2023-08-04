/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2020 Intel Corporation
 */

#ifndef __INTEL_MIGRATE_TYPES__
#define __INTEL_MIGRATE_TYPES__

#include <linux/dma-fence.h>

struct intel_context;

struct intel_migrate {
	struct intel_context *context;

	unsigned long clear_chunk;
};

#endif /* __INTEL_MIGRATE_TYPES__ */
