# SPDX-License-Identifier: GPL-2.0
#
# Makefile for linux/drivers/platform/x86/intel/pmt
# Intel Platform Monitoring Technology Drivers
#

ifeq ($(CPTCFG_INTEL_VSEC),m)
obj-$(CPTCFG_INTEL_PMT_CLASS)		+= pmt_class.o
pmt_class-y				:= class.o
obj-$(CPTCFG_INTEL_PMT_TELEMETRY)	+= pmt_telemetry.o
pmt_telemetry-y				:= telemetry.o
obj-$(CPTCFG_INTEL_PMT_CRASHLOG)	+= pmt_crashlog.o
pmt_crashlog-y				:= crashlog.o
endif
ifeq ($(CPTCFG_MFD_INTEL_PMT),m)
obj-$(CPTCFG_INTEL_PMT_CLASS)		+= intel_pmt_class.o
obj-$(CPTCFG_INTEL_PMT_TELEMETRY)	+= intel_pmt_telemetry.o
obj-$(CPTCFG_INTEL_PMT_CRASHLOG)	+= intel_pmt_crashlog.o
endif
