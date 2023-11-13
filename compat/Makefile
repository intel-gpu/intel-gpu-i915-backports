ccflags-y += -I$(src)

ifeq (y,$(filter y,$(CPTCFG_ONLY_I915) $(CPTCFG_BUILD_DMABUF_I915_MEI)))
ifeq ($(CONFIG_BACKPORT_INTEGRATE),)
obj-m += i915-compat.o
else
obj-y += i915-compat.o
endif
endif

CPTCFG_MODULE_SIG=n
i915-compat-y += main.o

i915-compat-$(CPTCFG_KERNEL_3_11) += backport-3.10.o backport-3.11.o
i915-compat-$(CPTCFG_KERNEL_3_12) += backport-3.12.o
i915-compat-$(CPTCFG_KERNEL_3_13) += backport-3.13.o
i915-compat-$(CPTCFG_KERNEL_3_14) += backport-3.14.o
i915-compat-$(CPTCFG_KERNEL_3_15) += backport-3.15.o
i915-compat-$(CPTCFG_KERNEL_3_16) += backport-3.16.o
i915-compat-$(CPTCFG_KERNEL_3_17) += backport-3.17.o
i915-compat-$(CPTCFG_KERNEL_5_2)  += page_alloc.o
i915-compat-$(CPTCFG_KERNEL_5_7)  += backport-5.7.o swap.o
i915-compat-$(CPTCFG_KERNEL_5_8)  += hdmi.o backport-5.8.o 
i915-compat-$(CPTCFG_KERNEL_5_5)  += nvmem-core.o
i915-compat-$(CPTCFG_KERNEL_5_10) += slub.o slab.o i915_gem_mmu_notifier.o memcontrol.o
i915-compat-$(CPTCFG_KERNEL_5_12) += ptrace.o
i915-compat-$(CPTCFG_KERNEL_5_13) += backport-5.13.o
i915-compat-$(CPTCFG_KERNEL_5_15) += backport-5.15.o
i915-compat-$(CPTCFG_I915_NO_DRM) += backport-nodrm.o

get_release_version = $(strip $(shell expr $(1) \* 256 + 0$(2)))
ifneq ($(RHEL_RELEASE_VERSION),)
disable_rhel_drm_backport = $(strip $(shell expr $(RHEL_RELEASE_VERSION) \<= $(call  get_release_version,8,2)))
endif

ifneq (1,$(disable_rhel_drm_backport))
# Enable shims for the RHEL DRM backport
ccflags-y += -DRH_DRM_BACKPORT
endif
