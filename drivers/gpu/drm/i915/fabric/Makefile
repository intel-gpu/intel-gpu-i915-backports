# SPDX-License-Identifier: MIT
#
# Copyright(c) 2020 - 2022 Intel Corporation.
#
# Intel Connectivity fabric driver
#

obj-$(CPTCFG_DRM_I915_INTEL_FABRIC) += iaf.o

iaf-y := debugfs.o \
	 dev_diag.o \
	 error.o \
	 fw.o \
	 main.o \
	 mei_iaf_user.o \
	 mbdb.o \
	 netlink.o \
	 ops.o \
	 parallel.o \
	 port.o \
	 port_diag.o \
	 routing_debug.o \
	 routing_engine.o \
	 routing_event.o \
	 routing_io.o \
	 routing_logic.o \
	 routing_p2p.o \
	 routing_pause.o \
	 routing_topology.o \
	 statedump.o \
	 sysfs.o \
	 trace.o \

iaf-$(CPTCFG_IAF_DEBUG_MBOX_ACCESS) += mbox.o

#export CPTCFG_IAF_DEBUG_SELFTESTS=y
subdir-ccflags-$(CPTCFG_IAF_DEBUG_SELFTESTS) += -DCPTCFG_IAF_DEBUG_SELFTESTS
iaf-$(CPTCFG_IAF_DEBUG_SELFTESTS) += selftests/selftest.o\
				     selftests/routing_selftest.o\
				     selftests/routing_mock.o\

CFLAGS_trace.o = -I$(src)
