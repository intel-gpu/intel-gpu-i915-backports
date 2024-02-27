/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2014-2019 Intel Corporation
 */

#ifndef _INTEL_GUC_ADS_H_
#define _INTEL_GUC_ADS_H_

#include <linux/types.h>
#include <linux/iosys-map.h>

struct intel_guc;
struct drm_printer;
struct intel_engine_cs;

int intel_guc_ads_create(struct intel_guc *guc);
void intel_guc_ads_destroy(struct intel_guc *guc);
void intel_guc_ads_init_late(struct intel_guc *guc);
void intel_guc_ads_reset(struct intel_guc *guc);
void intel_guc_ads_print_policy_info(struct intel_guc *guc,
				     struct drm_printer *p);
struct iosys_map intel_guc_engine_usage_record_map_v1(struct intel_engine_cs *engine);
int intel_guc_engine_usage_record_map_v2(struct intel_guc *guc,
					 struct intel_engine_cs *engine,
					 u32 vf_idx,
					 struct iosys_map *engine_map,
					 struct iosys_map *global_map);
u32 intel_guc_engine_usage_offset_global(struct intel_guc *guc);

#endif
