ccflags-y += -I$(src) -Wframe-larger-than=1280
ifeq ($(CONFIG_BACKPORT_INTEGRATE),)
obj-m += i915-compat.o
else
obj-y += i915-compat.o
endif
i915-compat-y += main.o

# Kernel backport compatibility code
i915-compat-$(CPTCFG_KERNEL_3_0) += compat-3.0.o
i915-compat-$(CPTCFG_KERNEL_3_1) += compat-3.1.o
i915-compat-$(CPTCFG_KERNEL_3_2) += backport-3.2.o
i915-compat-$(CPTCFG_KERNEL_3_3) += compat-3.3.o
i915-compat-$(CPTCFG_KERNEL_3_4) += compat-3.4.o
i915-compat-$(CPTCFG_KERNEL_3_5) += compat-3.5.o user_namespace.o
i915-compat-$(CPTCFG_KERNEL_3_6) += compat-3.6.o
i915-compat-$(CPTCFG_KERNEL_3_7) += compat-3.7.o
i915-compat-$(CPTCFG_KERNEL_3_8) += compat-3.8.o
i915-compat-$(CPTCFG_KERNEL_3_9) += compat-3.9.o
i915-compat-$(CPTCFG_KERNEL_3_10) += backport-3.10.o
i915-compat-$(CPTCFG_KERNEL_3_11) += backport-3.11.o
i915-compat-$(CPTCFG_KERNEL_3_12) += backport-3.12.o
i915-compat-$(CPTCFG_KERNEL_3_13) += backport-3.13.o memneq.o
i915-compat-$(CPTCFG_KERNEL_3_14) += backport-3.14.o
i915-compat-$(CPTCFG_KERNEL_3_15) += backport-3.15.o
i915-compat-$(CPTCFG_KERNEL_3_16) += backport-3.16.o
i915-compat-$(CPTCFG_KERNEL_3_17) += backport-3.17.o
i915-compat-$(CPTCFG_KERNEL_3_18) += backport-3.18.o
i915-compat-$(CPTCFG_KERNEL_3_19) += backport-3.19.o
i915-compat-$(CPTCFG_KERNEL_4_0) += backport-4.0.o
i915-compat-$(CPTCFG_KERNEL_4_1) += backport-4.1.o
i915-compat-$(CPTCFG_KERNEL_4_2) += backport-4.2.o
i915-compat-$(CPTCFG_KERNEL_4_3) += backport-4.3.o
i915-compat-$(CPTCFG_KERNEL_4_4) += backport-4.4.o
i915-compat-$(CPTCFG_KERNEL_4_5) += backport-4.5.o
i915-compat-$(CPTCFG_KERNEL_4_6) += backport-4.6.o
i915-compat-$(CPTCFG_KERNEL_4_7) += backport-4.7.o
i915-compat-$(CPTCFG_KERNEL_4_8) += backport-4.8.o
i915-compat-$(CPTCFG_KERNEL_4_10) += backport-4.10.o
i915-compat-$(CPTCFG_KERNEL_5_2) += page_alloc.o
i915-compat-$(CPTCFG_KERNEL_5_10) += i915_gem_mmu_notifier.o memcontrol.o
i915-compat-$(CPTCFG_KERNEL_5_13) += softirq.o
i915-compat-$(CPTCFG_KERNEL_5_15) += backport-5.15.o timer.o
ifeq ($(CPTCFG_DRM_I915_DISPLAY),y)
i915-compat-$(CPTCFG_BUILD_I915) += backport-nodrm.o
endif
i915-compat-y += backport-5.19.o
i915-compat-y += slub.o slab.o ptrace.o vmscan.o swap.o dma-resv.o swiotlb.o hdmi.o power_runtime.o proc_fs.o
i915-compat-$(CPTCFG_PCIEAER) += aer.o


get_release_version = $(strip $(shell expr $(1) \* 256 + 0$(2)))
ifneq ($(RHEL_RELEASE_VERSION),)
disable_rhel_drm_backport = $(strip $(shell expr $(RHEL_RELEASE_VERSION) \<= $(call  get_release_version,8,2)))
endif

ifneq (1,$(disable_rhel_drm_backport))
# Enable shims for the RHEL DRM backport
ccflags-y += -DRH_DRM_BACKPORT
endif
