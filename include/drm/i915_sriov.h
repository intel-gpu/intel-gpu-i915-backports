/* SPDX-License-Identifier: MIT */

#include <linux/types.h>

struct drm_i915_private;

int i915_sriov_pf_pause_vf(struct drm_i915_private *i915, unsigned int vfid);
int i915_sriov_pf_resume_vf(struct drm_i915_private *i915, unsigned int vfid);

int i915_sriov_pf_wait_vf_flr_done(struct drm_i915_private *i915, unsigned int vfid);

unsigned int i915_sriov_pf_get_vf_tile_mask(struct drm_i915_private *i915, unsigned int vfid);

size_t
i915_sriov_pf_get_vf_ggtt_size(struct drm_i915_private *i915, unsigned int vfid, unsigned int tile);
ssize_t
i915_sriov_pf_save_vf_ggtt(struct drm_i915_private *i915, unsigned int vfid, unsigned int tile,
			   void *buf, size_t size);
int
i915_sriov_pf_load_vf_ggtt(struct drm_i915_private *i915, unsigned int vfid, unsigned int tile,
			   const void *buf, size_t size);

size_t
i915_pf_get_vf_lmem_size(struct drm_i915_private *i915, unsigned int vfid, unsigned int tile);
ssize_t
i915_sriov_pf_save_vf_lmem(struct drm_i915_private *i915, unsigned int vfid, unsigned int tile,
			   void *buf, loff_t offset, size_t size);
int
i915_sriov_pf_load_vf_lmem(struct drm_i915_private *i915, unsigned int vfid, unsigned int tile,
			   const void *buf, loff_t offset, size_t size);

size_t
i915_pf_get_vf_fw_state_size(struct drm_i915_private *i915, unsigned int vfid, unsigned int tile);
ssize_t
i915_sriov_pf_save_vf_fw_state(struct drm_i915_private *i915, unsigned int vfid, unsigned int tile,
			       void *buf, size_t size);
int
i915_sriov_pf_load_vf_fw_state(struct drm_i915_private *i915, unsigned int vfid, unsigned int tile,
			       const void *buf, size_t size);
