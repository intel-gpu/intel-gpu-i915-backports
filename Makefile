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
KERNEL_MAKEFILE := $(KLIB_BUILD)/Makefile.rhelver
CONFIG_MD5 := $(shell md5sum $(KERNEL_CONFIG) 2>/dev/null | sed 's/\s.*//')

#
# Packaging targets Available
#

#
# PKG_DISTRO_TARGETS
# DKMS Package Targets
PKG_DISTRO_TARGETS := dmadkmsrpm-pkg i915dkmsrpm-pkg dkmsrpm-pkg dmadkmsdeb-pkg i915dkmsdeb-pkg dkmsdeb-pkg

#
# BIN_PKG_TARGETS
# Binary Package Targets
BIN_PKG_TARGETS := binrpm-pkg bindeb-pkg

ARCH := x86_64
export KLIB KLIB_BUILD BACKPORT_DIR KMODDIR KMODPATH_ARG ARCH PKG_DISTRO_TARGETS

## TBD: Move to Backport.Makefile
ifeq ($(BUILD_CONFIG),pmt)
export INTEL_PMT_FORCED=1
else
export INTEL_PMT_FORCED=0
endif

# disable built-in rules for this file
.SUFFIXES:

.PHONY: default
default:
	@$(MAKE) modules

# TBD : Move this target to Makefile.backport, Removing this will also work
# as unknown targets are called in DEFAULT target.
.PHONY:remove-drm-headers
remove-drm-headers:
	@$(MAKE) -f Makefile.real remove-drm-headers

.PHONY: mrproper
mrproper:
	@test -f .config && $(MAKE) clean || true
	@rm -f .config
	@rm -f .kernel_config_md5 Kconfig.versions Kconfig.kernel
	@rm -f backport-include/backport/autoconf.h
	@git ls-files -z -d | xargs -0 git checkout --
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
			sed 's/^\(\([3-5]\|2\.6\)\.[0-9]\+\).*/\1/;t;d')		;\
		test "$$kver" != "" || echo "Kernel version parse failed!"		;\
		test "$$kver" != ""							;\
		kvers="$$(seq 14 39 | sed 's/^/2.6./')"					;\
		kvers="$$kvers $$(seq 0 19 | sed 's/^/3./')"				;\
		kvers="$$kvers $$(seq 0 20 | sed 's/^/4./')"				;\
		kvers="$$kvers $$(seq 0 99 | sed 's/^/5./')"				;\
		print=0									;\
		for v in $$kvers ; do							\
			if [ "$$v" = "$$kver" ] ; then print=1 ; fi			;\
			if [ "$$print" = "1" ] ; then					\
				echo config KERNEL_$$(echo $$v | tr . _)	;\
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
		echo " done."								;\
	fi										;\
	echo "$(CONFIG_MD5)" > .kernel_config_md5
endif ### ifeq (,$(filter $(PKG_DISTRO_TARGETS), $(MAKECMDGOALS)))
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

# TBD: Move help of backports to Makefile.backport
# Includes macros from BUILD_VERSION in common-help
# dkms-help binrpm-help
.PHONY: common-help
common-help:
	@echo "--------------------------------------------------------------------------------------"
	@echo "Build Configurations:"
	@echo "  KLIB 			: path/to/headers"
	@echo "  KLIB_BUILD 		: path/to/headers/build "
	@echo "  BUILD_VERSION		: Pass build version to be added to package name"
	@echo "  RELEASE_TYPE 		: <opensource/prerelease/custom> Package release type created"
	@echo "  				Example: make <Target> RELEASE_TYPE=test "
	@echo "  				Package names would be intel-i915-dkms-test for DKMS or intel-i915-test for binary"
	@echo "  				Note: If custom packages are created, tracking the conflicting package "
	@echo "  				is difficult. Make sure no other package is already installed before "
	@echo "  				you intalling current one."
	@echo "  BUILD_CONFIG 		: Specifiv build config variant"
	@echo "  				Ex: make <Target> BUILD_CONFIG=sp2 "
	@echo "  OS_DISTRIBUTION 	: Distro targeted package"
	@echo "  				You can set this value by passing supported kernel name"
	@echo "  				Ex: make <Target> OS_DISTRIBUTION=RHEL_8.8"
	@echo "  				###   List of supported osv kernel versions   ### "
	@echo "  				$$(cat versions |& tail -n +4 | cut -d '_' -f 1-2 | grep RHEL | tr '\n' '\t')"
	@echo "  				$$(cat versions |& tail -n +4 | cut -d '_' -f 1-2 | grep VANILLA)"
	@echo "  				Please provide supported kernel name to OS_DISTRIBUTION"
	@echo ""
	@echo "--------------------------------------------------------------------------------------"

.PHONY: dkms-pkg-help
dkms-pkg-help: common-help
	@echo "--------------------------------------------------------------------------------------"
	@echo "DKMS Targets:"
	@echo "  dmadkmsrpm-pkg  - Build package RPM for dma dkms package"
	@echo "  i915dkmsrpm-pkg - Build package RPM for i915 dkms package"
	@echo "  dkmsrpm-pkg     - Build above two target dmadkmsrpm-pkg i915dkmsrpm-pkg"
	@echo ""
	@echo "  dmadkmsdeb-pkg  - Build package debian for dma dkms package"
	@echo "  i915dkmsdeb-pkg - Build package debian for i915 dkms package"
	@echo "  dkmsdeb-pkg     - Build above two target dmadkmsdeb-pkg i915dkmsdeb-pkg"
	@echo ""
	@echo "DKMS Package creation using targets help:"
	@echo "  Command:make  <Build Configurations> <target>"
	@echo "  Ex: make BUILD_VERION=10 i915dkmsrpm-pkg"
	@echo ""
	@echo "--------------------------------------------------------------------------------------"

.PHONY: bin-pkg-help
bin-pkg-help: common-help
	@echo "--------------------------------------------------------------------------------------"
	@echo "Binary Targets:"
	@echo "   bindeb-pkg  -  Build binary debian package for respective kernel "
	@echo "  binrpm-pkg  - Build binary rpm package for respective kernel"
	@echo ""
	@echo " Command:  make <Build Configurations> <target>"
	@echo " Ex: make BUILD_CONFIG=sp2 binrpm-pkg"
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
