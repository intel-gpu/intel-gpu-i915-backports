#
# Makefile for the output source package
#

ifeq ($(KERNELRELEASE),)

MAKEFLAGS += --no-print-directory
SHELL := /bin/bash
BACKPORT_DIR := $(shell pwd)

KMODDIR ?= updates
ifneq ($(origin KLIB), undefined)
KMODPATH_ARG := "INSTALL_MOD_PATH=$(KLIB)"
else
KLIB := /lib/modules/$(shell uname -r)/
KMODPATH_ARG :=
endif
KLIB_BUILD ?= $(KLIB)/build/
KERNEL_CONFIG := $(KLIB_BUILD)/.config
KERNEL_MAKEFILE := $(KLIB_BUILD)/Makefile
CONFIG_MD5 := $(shell md5sum $(KERNEL_CONFIG) 2>/dev/null | sed 's/\s.*//')
KBUILD_MODPOST_WARN := 1
export CUSTOM_KERN_VER := $(shell cat $(KLIB_BUILD)/include/generated/utsrelease.h | grep "UTS_RELEASE" | cut -d '"' -f2 | cut -d '.' -f1-2 | cut -d '-' -f1 | tr -d '+')

#
# Packaging targets Available
#
DEB_PKG_DISTRO_TARGETS := dmabufdkmsdeb-pkg drmdkmsdeb-pkg i915dkmsdeb-pkg dkmsdeb-pkg
RPM_PKG_DISTRO_TARGETS := dmabufdkmsrpm-pkg drmdkmsrpm-pkg i915dkmsrpm-pkg dkmsrpm-pkg

#
# PKG_DISTRO_TARGETS
# DKMS Package Targets
PKG_DISTRO_TARGETS := $(DEB_PKG_DISTRO_TARGETS) $(RPM_PKG_DISTRO_TARGETS)

#
# BIN_PKG_TARGETS
# Binary Package Targets
BIN_PKG_TARGETS := binrpm-pkg i915bindeb-pkg

ARCH := x86_64
export KLIB KLIB_BUILD BACKPORT_DIR KMODDIR KMODPATH_ARG ARCH KBUILD_MODPOST_WARN PKG_DISTRO_TARGETS DEB_PKG_DISTRO_TARGETS RPM_PKG_DISTRO_TARGETS

export MODULE_I915=i915
ifeq ($(BUILD_CONFIG),i915_hwe)
export MODULE_I915=i915_hwe
endif

ifeq ($(BUILD_CONFIG),pmt)
export INTEL_PMT_FORCED=1
else
export INTEL_PMT_FORCED=0
endif

export DISABLE_DISPLAY=0
SLES_OSV_VER := $(shell cat $(KERNEL_CONFIG) | grep "CONFIG_LOCALVERSION=" | cut -d '"' -f2 | cut -d '-' -f2 | cut -d '.' -f1-1)
ifneq (, $(SLES_OSV_VER))
VERSION_CHECK = $(shell expr $(SLES_OSV_VER) \>= 150400)
endif

ifeq ($(BUILD_CONFIG),disabledisplay)
        DISABLE_DISPLAY=1
else ifeq ($(BUILD_CONFIG), CUSTOM_KERN_1)
ifeq ($(shell expr $(CUSTOM_KERN_VER) \>= 6.4), 1)
        DISABLE_DISPLAY=1
endif
else ifeq ($(VERSION_CHECK), 1)
        DISABLE_DISPLAY=1
endif

ifeq ($(BUILD_CONFIG), enabledisplay)
	DISABLE_DISPLAY=0
endif

# disable built-in rules for this file
.SUFFIXES:

.PHONY: default
default:
	@$(MAKE) modules

.PHONY: mrproper
mrproper:
	@test -f .config && $(MAKE) clean || true
	@rm -f .config
	@rm -f .kernel_config_md5 Kconfig.versions Kconfig.kernel
	@rm -f backport-include/backport/autoconf.h
	@$(MAKE) -f Makefile.real mrproper

.DEFAULT:
	@set -e ; test -f local-symbols || (						\
	echo "/--------------"								;\
	echo "| You shouldn't run make in the backports tree, but only in"		;\
	echo "| the generated output. This here is only the skeleton code"		;\
	echo "| copied into the output directory. To use the backport system"		;\
	echo "| from scratch, go into the top-level directory and run"			;\
	echo "|	./gentree.py /path/to/linux-next/ /tmp/output"				;\
	echo "| and then make menuconfig/... in the output directory. See"		;\
	echo "|	./gentree.py --help"							;\
	echo "| for more options."							;\
	echo "\\--"									;\
	false)
ifeq (,$(filter $(PKG_DISTRO_TARGETS), $(MAKECMDGOALS)))
	@set -e ; test -f $(KERNEL_CONFIG) || (						\
	echo "/--------------"								;\
	echo "| Your kernel headers are incomplete/not installed."			;\
	echo "| Please install kernel headers, including a .config"			;\
	echo "| file or use the KLIB/KLIB_BUILD make variables to"			;\
	echo "| set the kernel to build against, e.g."					;\
	echo "|   make KLIB=/lib/modules/3.1.7/"					;\
	echo "| to compile/install for the installed kernel 3.1.7"			;\
	echo "| (that isn't currently running.)"					;\
	echo "\\--"									;\
	false)
	@set -e ; if [ "$$(cat .kernel_config_md5 2>/dev/null)" != "$(CONFIG_MD5)" ]	;\
	then 										\
		echo -n "Generating local configuration database from kernel ..."	;\
		grep -v -f local-symbols $(KERNEL_CONFIG) | grep = | (			\
			while read l ; do						\
				if [ "$${l:0:7}" != "CONFIG_" ] ; then			\
					continue					;\
				fi							;\
				l=$${l:7}						;\
				n=$${l%%=*}						;\
				v=$${l#*=}						;\
				if [ "$$v" = "m" ] ; then				\
					echo config $$n					;\
					echo '    tristate' 				;\
				elif [ "$$v" = "y" ] ; then				\
					echo config $$n					;\
					echo '    bool'					;\
				else							\
					continue					;\
				fi							;\
				echo "    default $$v"					;\
				echo ""							;\
			done								\
		) > Kconfig.kernel							;\
		kver=$$($(MAKE) --no-print-directory -C $(KLIB_BUILD) kernelversion |	\
			sed 's/^\(\([3-6]\|2\.6\)\.[0-9]\+\).*/\1/;t;d')		;\
		test "$$kver" != "" || echo "Kernel version parse failed!"		;\
		test "$$kver" != ""							;\
		kvers="$$(seq 14 39 | sed 's/^/2.6./')"					;\
		kvers="$$kvers $$(seq 0 19 | sed 's/^/3./')"				;\
		kvers="$$kvers $$(seq 0 20 | sed 's/^/4./')"				;\
		kvers="$$kvers $$(seq 0 20 | sed 's/^/5./')"				;\
		kvers="$$kvers $$(seq 0 99 | sed 's/^/6./')"				;\
		print=0									;\
		for v in $$kvers ; do							\
			if [ "$$v" = "$$kver" ] ; then print=1 ; fi			;\
			if [ "$$print" = "1" ] ; then					\
				echo config KERNEL_$$(echo $$v | tr . _)		;\
				echo "    def_bool y"					;\
			fi								;\
		done > Kconfig.versions							;\
		# RHEL as well, sadly we need to grep for it				;\
		RHEL_MAJOR=$$(grep '^RHEL_MAJOR' $(KERNEL_MAKEFILE) | 			\
					sed 's/.*=\s*\([0-9]*\)/\1/;t;d')		;\
		RHEL_MINOR=$$(grep '^RHEL_MINOR' $(KERNEL_MAKEFILE) | 			\
					sed 's/.*=\s*\([0-9]*\)/\1/;t;d')		;\
		for v in $$(seq 0 $$RHEL_MINOR) ; do 					\
			echo config BACKPORT_RHEL_KERNEL_$${RHEL_MAJOR}_$$v		;\
			echo "    def_bool y"						;\
		done >> Kconfig.versions						;\
		i915_only_mode=0  							;\
		if [ "$(BUILD_CONFIG)" = "nodrm" ] ; then 				\
			i915_only_mode=1 						;\
			echo -e "\nNote: nodrm build config set!, This mode will only build" ;\
			echo "I915 module and child modules, base kernel's drm module will be " ;\
			echo "utilized." 							;\
		else										\
			# Add default OS distro/LTS kernels's list to build i915_only		;\
			# Syntax: OSV_MAJOR.MINOR 						;\
			# OSV = RHEL/SLES/UBUNTU/... 						;\
			# Ex: i915_only_kernel_list=(RHEL_8.6 SLES_15.3)			;\
			#									;\
			i915_only_kernel_list=() 						;\
			for i in $${i915_only_kernel_list[@]} ; do 				\
				OSV_NAME=$$(echo $$i | cut -f 1 -d '_' ) 			;\
				OSV_MAJOR=$$(echo $$i | cut -f 2 -d '_' | cut -f 1 -d "." )	;\
				OSV_MINOR=$$(echo $$i | cut -f 2 -d '_' | cut -f 2 -d "." )	;\
				if [ "$$OSV_NAME" = "RHEL" ] && \
					[ "$$OSV_MAJOR.$$OSV_MINOR" = "$$RHEL_MAJOR.$$RHEL_MINOR" ]; then \
					i915_only_mode=1 					;\
				fi 								;\
				#TBD : add other OSV identifier or make it generic		;\
			done			 						;\
			if [ "$$i915_only_mode" != "1" ]; then 					\
				# Check kernel version is > default value 				;\
				i915_only_kver="5.14"							;\
				i915_only_kver_maj=$$(echo $$i915_only_kver | cut -f 1 -d "." ) ;\
				i915_only_kver_min=$$(echo $$i915_only_kver | cut -f 2 -d "." ) ;\
				i915_only_code=$$(expr $$i915_only_kver_maj \* 256 + 0$$i915_only_kver_min \* 8) ;\
				base_kver_maj=$$(echo $$kver | cut -f 1 -d "." ) ;\
				base_kver_min=$$(echo $$kver | cut -f 2 -d "." ) ;\
				base_kver_code=$$(expr $$base_kver_maj \* 256 + 0$$base_kver_min \* 8) ;\
				if [ $$base_kver_code -ge $$i915_only_code  ]; then 		\
					i915_only_mode=1						;\
				fi 									;\
			fi										;\
		fi											;\
		if [ "$$i915_only_mode" = "1" ]; then					\
			echo config BUILD_I915_ONLY 					;\
			echo "    def_bool y"						;\
		fi >> Kconfig.versions							;\
		echo " done."								;\
	fi										;\
	echo "$(CONFIG_MD5)" > .kernel_config_md5
endif ### ifeq (,$(filter $(PKG_DISTRO_TARGETS), $(MAKECMDGOALS)))
ifneq ("$(wildcard .config)","")
	@$(MAKE) -f Makefile.real updateconfig
endif
	@$(MAKE) -f Makefile.real "$@"

.PHONY: defconfig-help
defconfig-help:
	@echo "Driver or subsystem configuration targets:"
	@set -e						;\
		bk_configs="$$(ls defconfigs/*)" 	;\
		for cfg in $$bk_configs; do		\
			echo "  defconfig-$${cfg##defconfigs/}"	;\
		done
	@echo ""

.PHONY: common-help
common-help:
	@echo "--------------------------------------------------------------------------------------"
	@echo "Build Configurations:"
	@echo "  KLIB   : path/to/linux/kernel/headers"
	@echo "  KLIB_BUILD     : path/to/linux/kernel/headers/build"
	@echo "  BUILD_VERSION  : Pass build version to be added to package name"
	@echo "  RELEASE_TYPE   : <opensource/prerelease/custom> Package release type created"
	@echo " 			Example: make <Target> RELEASE_TYPE=test "
	@echo " 			Package names would be intel-i915-dkms-test for DKMS or intel-i915-test for binary"
	@echo " 			Note: If custom packages are created, tracking the conflicting package "
	@echo " 			is difficult. Make sure no other intel-i915* packages are already installed before "
	@echo " 			you installing current one."
	@echo "  BUILD_CONFIG   : Specify build config variant"
	@echo ""
	@echo "		To disable display, pass BUILD_CONFIG=disabledispaly "
	@echo " 		Ex: make <Target> BUILD_CONFIG=disabledisplay "
	@echo ""
	@echo "		##### List of OSVs for which display has been disabled by default ###### "
	@echo "			SLES15_SP6     SLES15_SP5      SLES15_SP4 "
	@echo ""
	@echo "		If you want to force enable display, pass BUILD_CONFIG=enabledispaly "
	@echo "			Ex : make <Target> BUILD_CONFIG=enabledisplay "
	@echo ""
	@echo "  OS_DISTRIBUTION: Distro targeted package"
	@echo " 			You can set this value by passing supported kernel name"
	@echo " 			Ex: make <Target> OS_DISTRIBUTION=UBUNTU_22.04_SERVER"
	@echo ""
	@echo "--------------------------------------------------------------------------------------"

.PHONY: dkms-pkg-help
dkms-pkg-help: common-help
	@echo "--------------------------------------------------------------------------------------"
	@echo " DKMS Targets: "
	@echo " Debian Targets: "
	@echo "  dkmsdeb-pkg    - Build single DKMS Debian package for dmabuf, drm, i915 and dependent child drivers (mei and pmt/vsec)"
	@echo "  dmabufdkmsdeb-pkg - Build DKMS debian package for dmabuf"
	@echo "  drmdkmsdeb-pkg - Build DKMS debian package for drm, i915 and dependent child drivers (mei and pmt/vsec)"
	@echo "  i915dkmsdeb-pkg - Build DKMS debian package i915 and dependent child drivers (mei and pmt/vsec)"
	@echo ""
	@echo " Example: make i915dkmsdeb-pkg OS_DISTRIBUTION=UBUNTU_22.04_SERVER "
	@echo ""
	@echo " Debian package name contains UBUNTU_22.04_SERVER Kernel version as default. "
	@echo ""
	@echo " ##### List of Debian supported OS distro versions ##### "
	@echo " $$(cat versions |& tail -n +4 | cut -d "_" -f 1-3 | grep "UBUNTU" | grep -v "SERVER_KERNEL"| tr '\n' '\t') "
	@echo ""
	@echo " RPM Targets: "
	@echo "  dkmsrpm-pkg     - Build single DKMS RPM package for dmabuf, drm, i915 and dependent child drivers (mei and pmt/vsec)"
	@echo "  dmabufdkmsrpm-pkg  - Build dkms RPM package for dmabuf"
	@echo "  i915dkmsrpm-pkg - Build dkms RPM package for i915 and dependent child drivers (mei and pmt/vsec)"
	@echo "  drmdkmsrpm-pkg -  Build dkms RPM package for drm, i915 and dependent child drivers (mei and pmt/vsec)"
	@echo ""
	@echo " Example: make i915dkmsrpm-pkg OS_DISTRIBUTION=SLES15_SP5 "
	@echo ""
	@echo " Rpm package contains the SLES15_SP5 Kernel version as default"
	@echo ""
	@echo " ##### List of RPM supported OS distro Versions ##### "
	@echo " $$(cat versions |& tail -n +4 | cut -d "_" -f 1-2 | grep SLES | tr '\n' '\t') "
	@echo " $$(cat versions |& tail -n +4 | cut -d "_" -f 1-2 | grep RHEL | tr '\n' '\t') "
	@echo ""
	@echo " ##### List of LTS kernel versions ##### "
	@echo " $$(cat versions |& tail -n +4 | cut -d "_" -f 1-2 | grep VANILLA | tr '\n' '\t')"
	@echo ""
	@echo " For LTS kernels, either RPM or Debian package can be created"
	@echo ""
	@echo " For Specific OS distro/LTS kernels, pass the supported kernel name to OS_DISTRIBUTION option "
	@echo ""
	@echo "--------------------------------------------------------------------------------------"

.PHONY: bin-pkg-help
bin-pkg-help: common-help
	@echo "--------------------------------------------------------------------------------------"
	@echo " Binary Targets: "
	@echo "   i915bindeb-pkg - Build binary debian package for respective kernel "
	@echo "   binrpm-pkg	 - Build binary rpm package for respective kernel "
	@echo ""
	@echo "   Command:  make <Build Configurations> <target> "
	@echo " 	 Ex: make RELEASE_TYPE=prerelease binrpm-pkg "
	@echo ""
	@echo "--------------------------------------------------------------------------------------"

.PHONY: help
help: defconfig-help common-help dkms-pkg-help bin-pkg-help
	@echo "--------------------------------------------------------------------------------------"
	@echo "Cleaning targets:"
	@echo "  clean           - Remove most generated files but keep the config and"
	@echo "                    enough build support to build external modules"
	@echo "  mrproper        - Remove all generated files + config + various backup files"
	@echo ""
	@echo "Driver configuration help:"
	@echo "  defconfig-help  - List all prearranged defconfig-targets we have"
	@echo "                    designed for you. You can use this to find"
	@echo "                    driver specific configs in case all you really"
	@echo "                    need is to just compile one or a small group "
	@echo "                    of drivers."
	@echo ""
	@echo "Configuration targets:"
	@echo "  menuconfig      - Update current config utilising a menu based program"
	@echo "  oldconfig       - Update current config utilising a provided .config as base"
	@echo "  oldaskconfig    - ??"
	@echo "  silentoldconfig - Same as oldconfig, but quietly, additionally update deps"
	@echo "  allnoconfig     - New config where all options are answered with no"
	@echo "  allyesconfig    - New config where all options are accepted with yes"
	@echo "  allmodconfig    - New config selecting modules when possible"
	@echo "  alldefconfig    - New config with all symbols set to default"
	@echo "  randconfig      - New config with random answer to all options"
	@echo "  listnewconfig   - List new options"
	@echo "  olddefconfig    - Same as silentoldconfig but sets new symbols to their default value"
	@echo ""
	@echo "Other generic targets:"
	@echo "  all             - Build all targets marked with [*]"
	@echo "* modules         - Build all modules"
	@echo ""
	@echo "Architecture specific targets:"
	@echo "  install         - Install modules"
	@echo "  uninstall       - Uninstall modules"
	@echo ""
	@echo "Execute "make" or "make all" to build all targets marked with [*]"
	@echo "--------------------------------------------------------------------------------------"
else
include $(BACKPORT_DIR)/Makefile.backport
endif

PHONY += FORCE
FORCE:
