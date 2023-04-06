# SPDX-License-Identifier: GPL-2.0

# Makefile for the drm device driver.  This driver provides support for the
# Direct Rendering Infrastructure (DRI) in XFree86 4.1.0 and higher.

drm-y       :=	drm_aperture.o drm_auth.o drm_cache.o \
		drm_file.o drm_gem.o drm_ioctl.o drm_irq.o \
		drm_drv.o \
		drm_sysfs.o drm_hashtab.o drm_mm.o \
		drm_crtc.o drm_fourcc.o drm_modes.o drm_edid.o drm_displayid.o \
		drm_encoder_slave.o \
		drm_trace_points.o drm_prime.o \
		drm_rect.o drm_vma_manager.o drm_flip_work.o \
		drm_modeset_lock.o drm_atomic.o drm_bridge.o \
		drm_framebuffer.o drm_connector.o drm_blend.o \
		drm_encoder.o drm_mode_object.o drm_property.o \
		drm_plane.o drm_color_mgmt.o drm_print.o \
		drm_dumb_buffers.o drm_mode_config.o drm_vblank.o \
		drm_syncobj.o drm_lease.o drm_writeback.o drm_client.o \
		drm_client_modeset.o drm_atomic_uapi.o drm_hdcp.o \
		drm_managed.o drm_vblank_work.o

drm-$(CPTCFG_DRM_LEGACY) += drm_agpsupport.o drm_bufs.o drm_context.o drm_dma.o \
			    drm_legacy_misc.o drm_lock.o drm_memory.o drm_scatter.o \
			    drm_vm.o
drm-$(CPTCFG_DRM_LIB_RANDOM) += lib/drm_random.o
drm-$(CONFIG_COMPAT) += drm_ioc32.o
drm-$(CPTCFG_DRM_GEM_CMA_HELPER) += drm_gem_cma_helper.o
drm-$(CPTCFG_DRM_GEM_SHMEM_HELPER) += drm_gem_shmem_helper.o
drm-$(CONFIG_DRM_PANEL) += drm_panel.o
drm-$(CONFIG_OF) += drm_of.o
drm-$(CONFIG_PCI) += drm_pci.o
drm-$(CONFIG_DEBUG_FS) += drm_debugfs.o drm_debugfs_crc.o
drm-$(CPTCFG_DRM_LOAD_EDID_FIRMWARE) += drm_edid_load.o
#
#obj-$(CPTCFG_DRM_DP_AUX_BUS+= drm_dp_aux_bus.o

drm_vram_helper-y := drm_gem_vram_helper.o
obj-$(CPTCFG_DRM_VRAM_HELPER) += drm_vram_helper.o

drm_ttm_helper-y := drm_gem_ttm_helper.o
obj-$(CPTCFG_DRM_TTM_HELPER) += drm_ttm_helper.o

drm_kms_helper-y := drm_bridge_connector.o drm_crtc_helper.o drm_dp_helper.o \
		drm_dsc.o drm_probe_helper.o \
		drm_plane_helper.o drm_dp_mst_topology.o drm_atomic_helper.o \
		drm_kms_helper_common.o drm_dp_dual_mode_helper.o \
		drm_simple_kms_helper.o drm_modeset_helper.o \
		drm_scdc_helper.o drm_gem_atomic_helper.o \
		drm_gem_framebuffer_helper.o \
		drm_atomic_state_helper.o drm_damage_helper.o \
		drm_format_helper.o drm_self_refresh_helper.o

drm_kms_helper-$(CPTCFG_DRM_PANEL_BRIDGE) += bridge/panel.o
drm_kms_helper-$(CPTCFG_DRM_FBDEV_EMULATION) += drm_fb_helper.o
drm_kms_helper-$(CPTCFG_DRM_KMS_CMA_HELPER) += drm_fb_cma_helper.o
drm_kms_helper-$(CPTCFG_DRM_DP_AUX_CHARDEV) += drm_dp_aux_dev.o
drm_kms_helper-$(CPTCFG_DRM_DP_CEC) += drm_dp_cec.o

obj-$(CPTCFG_DRM_KMS_HELPER) += drm_kms_helper.o
obj-$(CPTCFG_DRM_DEBUG_SELFTEST) += selftests/

obj-$(CPTCFG_DRM)	+= drm.o
obj-$(CPTCFG_DRM_MIPI_DBI) += drm_mipi_dbi.o
obj-$(CPTCFG_DRM_MIPI_DSI) += drm_mipi_dsi.o
obj-$(CPTCFG_DRM_PANEL_ORIENTATION_QUIRKS) += drm_panel_orientation_quirks.o
obj-$(CPTCFG_DRM_I915)	+= i915/
obj-$(CPTCFG_DRM_VGEM)	+= vgem/
