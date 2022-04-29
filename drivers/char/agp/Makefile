# SPDX-License-Identifier: GPL-2.0
agpgart-y := backend.o generic.o isoch.o

ifeq ($(CPTCFG_DRM_LEGACY),y)
agpgart-$(CONFIG_COMPAT)	+= compat_ioctl.o
agpgart-y			+= frontend.o
endif


obj-$(CPTCFG_AGP)		+= agpgart.o
obj-$(CPTCFG_AGP_ALI)		+= ali-agp.o
obj-$(CPTCFG_AGP_ATI)		+= ati-agp.o
obj-$(CPTCFG_AGP_AMD)		+= amd-k7-agp.o
obj-$(CPTCFG_AGP_AMD64)		+= amd64-agp.o
obj-$(CPTCFG_AGP_ALPHA_CORE)	+= alpha-agp.o
obj-$(CPTCFG_AGP_EFFICEON)	+= efficeon-agp.o
obj-$(CPTCFG_AGP_HP_ZX1)	+= hp-agp.o
obj-$(CPTCFG_AGP_PARISC)	+= parisc-agp.o
obj-$(CPTCFG_AGP_I460)		+= i460-agp.o
obj-$(CPTCFG_AGP_INTEL)		+= intel-agp.o
obj-$(CPTCFG_INTEL_GTT)		+= intel-gtt.o
obj-$(CPTCFG_AGP_NVIDIA)	+= nvidia-agp.o
obj-$(CPTCFG_AGP_SIS)		+= sis-agp.o
obj-$(CPTCFG_AGP_SWORKS)	+= sworks-agp.o
obj-$(CPTCFG_AGP_UNINORTH)	+= uninorth-agp.o
obj-$(CPTCFG_AGP_VIA)		+= via-agp.o
