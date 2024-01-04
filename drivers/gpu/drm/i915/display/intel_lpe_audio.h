/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_LPE_AUDIO_H__
#define __INTEL_LPE_AUDIO_H__

struct drm_i915_private;

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
#include <linux/types.h>

enum pipe;
enum port;

int  intel_lpe_audio_init(struct drm_i915_private *dev_priv);
void intel_lpe_audio_teardown(struct drm_i915_private *dev_priv);
void intel_lpe_audio_irq_handler(struct drm_i915_private *dev_priv);
void intel_lpe_audio_notify(struct drm_i915_private *dev_priv,
			    enum pipe pipe, enum port port,
			    const void *eld, int ls_clock, bool dp_output);
#else
static inline void intel_lpe_audio_irq_handler(struct drm_i915_private *dev_priv) { return; }
#endif /* CPTCFG_DRM_I915_DISPLAY */

#endif /* __INTEL_LPE_AUDIO_H__ */
