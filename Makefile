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

version_h := $(BACKPORT_DIR)/backport-include/linux/osv_version.h

export KLIB KLIB_BUILD BACKPORT_DIR KMODDIR KMODPATH_ARG version_h

# disable built-in rules for this file
.SUFFIXES:

.PHONY: default
default: $(version_h)
	@$(MAKE) modules

ARCH := x86_64

# BKPT_VER is extracted from BACKPORTS_RELEASE_TAG, which is auto genereated from backport description.Tagging is needed
# for decoding this, Sample in the version file 'BACKPORTS_RELEASE_TAG="SLES15_SP3_6365_PRERELEASE_221027.0"'
# Backports tagging is needed for this to work, for example "SLES15_SP3_6365_PRERELEASE_221027.0" is the tag for 221027.0
# release in backports branch
#
BKPT_VER=$(shell cat versions | grep BACKPORTS_RELEASE_TAG | cut -d "_" -f 7 | cut -d "\"" -f 1 | cut -d "-" -f 1 2>/dev/null || echo 1)

#DII_TAG is extracted from DII_KERNEL_TAG, which is auto genereated from base kernel source.
#Tagging is needed for decoding this,
#Ex: For DII_KERNEL_TAG="DII_6001_prerelease" : DII_TAG will be 6001
#    For DII_KERNEL_TAG="PROD_I915_6469.0.7"  : DII_TAG will be 6469.0.7
#    For DII_KERNEL_TAG="I915-23.4.0"         : DII_TAG will be 23.4.0
#for DII_TAG_PREFIX output will be DII or PROD or I915-23.4.0.

DII_TAG_PREFIX=$(shell cat versions | grep DII_KERNEL_TAG | cut -f 2 -d "\"" | cut -d "_" -f 1 2>/dev/null || echo 1)

ifeq ($(DII_TAG_PREFIX) , PROD)
DII_TAG=$(shell cat versions | grep DII_KERNEL_TAG | cut -f 2 -d "\"" | cut -d "_" -f 3 2>/dev/null || echo 1)
else ifeq ($(DII_TAG_PREFIX) , DII)
DII_TAG=$(shell cat versions | grep DII_KERNEL_TAG | cut -f 2 -d "\"" | cut -d "_" -f 2 2>/dev/null || echo 1)
else
DII_TAG=$(shell cat versions | grep DII_KERNEL_TAG | cut -f 2 -d "\"" | cut -d "-" -f 2 2>/dev/null || echo 1)
endif

KER_VER := $(shell cat versions | grep BASE_KERNEL_NAME | cut -d "\"" -f 2 | cut -d "-" -f 1-|sed "s/-/./g" 2>/dev/null || echo 1)

ifneq ($(MAKECMDGOALS) , dkmsrpm-pkg)
BASE_KER_VER=$(shell cat $(KLIB_BUILD)/include/config/kernel.release | cut -d '-' -f 1-2 2> /dev/null)

SLES_KERN_VER=$(shell cat $(KLIB_BUILD)/include/config/kernel.release | cut -d "-" -f 1 | cut -d '.' -f 1,2,3 2> /dev/null)

SLES_BACKPORT_MAJOR = $(shell cat $(KLIB_BUILD)/include/config/kernel.release | cut -d '-' -f 2 | cut -d '.' -f 2 2> /dev/null)
SLES_BACKPORT_MINOR = $(shell cat $(KLIB_BUILD)/include/config/kernel.release | cut -d '-' -f 2 | cut -d '.' -f 3 2> /dev/null)
endif

###
# Easy method for doing a status message
       kecho := :
 quiet_kecho := echo
silent_kecho := :
kecho := $($(quiet)kecho)

###
# filechk is used to check if the content of a generated file is updated.
# Sample usage:
# define filechk_sample
#       echo $KERNELRELEASE
# endef
# version.h : Makefile
#       $(call filechk,sample)
# The rule defined shall write to stdout the content of the new file.
# The existing file will be compared with the new one.
# - If no file exist it is created
# - If the content differ the new file is used
# - If they are equal no change, and no timestamp update
# - stdin is piped in from the first prerequisite ($<) so one has
#   to specify a valid file as first prerequisite (often the kbuild file)
define filechk
        $(Q)set -e;                             \
        mkdir -p $(dir $@);                     \
        { $(filechk_$(1)); } > $@.tmp;          \
        if [ -r $@ ] && cmp -s $@ $@.tmp; then  \
                rm -f $@.tmp;                   \
        else                                    \
                $(kecho) '  UPD     $@';        \
                mv -f $@.tmp $@;                \
        fi
endef

ifneq ($(MAKECMDGOALS) , dkmsrpm-pkg)
define filechk_osv_version.h
        echo '#define SLES_BACKPORT_MAJOR $(SLES_BACKPORT_MAJOR)'; \
        echo '#define SLES_BACKPORT_MINOR $(SLES_BACKPORT_MINOR)'; \
        echo '#define SLES_RELEASE_VERSION(a,b) (((a) << 8) + (b))'; \
        echo '#define SLES_RELEASE_CODE \
                $(shell expr $(SLES_BACKPORT_MAJOR) \* 256 + $(SLES_BACKPORT_MINOR))'
endef

$(version_h): $(BACKPORT_DIR)/Makefile FORCE
ifeq ($(SLES_KERN_VER), 5.3.18)
	@gawk -i inplace '!/BASE_KERNEL_NAME/' versions
	@echo 'BASE_KERNEL_NAME="$(BASE_KER_VER)"' >> versions
endif
	$(call filechk,osv_version.h)
endif

# VERSION is generated as 1.DII_TAG.BackportVersion
VERSION := 1.$(DII_TAG).$(BKPT_VER).$(KER_VER)

ifneq ($(BUILD_VERSION), )
RELEASE := $(BUILD_VERSION)
else
RELEASE := 1
endif

RELEASE_TYPE ?= opensource

ifeq ($(RELEASE_TYPE), opensource)
PKG_SUFFIX=
else
PKG_SUFFIX=-$(RELEASE_TYPE)
endif

# dmadkmsrpm-pkg
# Creates Backports dmabuf dkms package
# command: make BUILD_VERSION=<build version> RELEASE_TYPE=<opensource/prerelease/custom> dmadkmsrpm-pkg
# Rpm generated can be copied to client machine and install
# BUILD_VERSION : pass build version to be added to package name
# RELEASE_TYPE: <opensource/prerelease> package need to be created
# will trigger source build and install on modules
#------------------------------------------------------------------------------
export KBUILD_ONLYDMADIRS := $(sort $(filter-out arch/%,$(vmlinux-alldirs)) drivers/dma-buf include scripts)
DMA_TAR_CONTENT := $(KBUILD_ONLYDMADIRS) .config Makefile* local-symbols MAINTAINERS \
               Kconfig* COPYING versions defconfigs backport-include kconf compat
DMADKMSMKSPEC := $(BACKPORT_DIR)/scripts/backport-mkdmabufdkmsspec
DMADKMSMKCONF := $(BACKPORT_DIR)/scripts/backport-mkdmabufdkmsconf
DMAMODULE_NAME := intel-dmabuf-dkms$(PKG_SUFFIX)
DMAVERSION := $(VERSION)
DMARELEASE := $(RELEASE)

.PHONY: dmadkmsrpm-pkg
dmadkmsrpm-pkg:
	cp $(BACKPORT_DIR)/defconfigs/dmabuf .config
	$(CONFIG_SHELL) $(DMADKMSMKCONF) -n $(DMAMODULE_NAME) -v $(DMAVERSION) -r $(DMARELEASE) -p $(RELEASE_TYPE) > $(BACKPORT_DIR)/dkms.conf
	$(CONFIG_SHELL) $(DMADKMSMKSPEC) -n $(DMAMODULE_NAME) -v $(DMAVERSION) -r $(DMARELEASE) -p $(RELEASE_TYPE) > $(BACKPORT_DIR)/$(DMAMODULE_NAME).spec
	patch -p1 < $(BACKPORT_DIR)/scripts/disable_drm.patch ;
	tar -cz $(RCS_TAR_IGNORE) -f $(DMAMODULE_NAME)-$(DMAVERSION)-src.tar.gz \
	        $(DMA_TAR_CONTENT) $(DMAMODULE_NAME).spec dkms.conf;
	+rpmbuild $(RPMOPTS) --target $(ARCH) -ta $(DMAMODULE_NAME)-$(DMAVERSION)-src.tar.gz \
	--define='_smp_mflags %{nil}'
	patch -p1 -R < $(BACKPORT_DIR)/scripts/disable_drm.patch ;

DMABINMODULE_NAME := intel-dmabuf$(PKG_SUFFIX)
DMABINMKSPEC := $(BACKPORT_DIR)/scripts/backport-mkdmabufbinspec

# dmabinrpm-pkg
# Creates Backport binary package for dma-buf
# command: make BUILD_VERSION=<build version> RELEASE_TYPE=<opensource/prerelease/custom> dmabinrpm-pkg
# BUILD_VERSION : pass build version to be added to package name
# RELEASE_TYPE : <opensource/prerelease> package need to be created
# Build machine should have kernel version and variant binary is targeted for.
# Generates Module.symvers package for usage by other drivers
#------------------------------------------------------------------------------
export KBUILD_ONLYI915DIRS := $(sort $(filter-out arch/%,$(vmlinux-alldirs)) drivers/gpu drivers/char drivers/platform drivers/pci include scripts)
.PHONY: dmabinrpm-pkg
dmabinrpm-pkg:
	cp $(BACKPORT_DIR)/defconfigs/dmabuf .config
	$(CONFIG_SHELL) $(DMABINMKSPEC) -n $(DMABINMODULE_NAME) -v $(DMAVERSION) -r $(DMARELEASE) -p $(RELEASE_TYPE) > $(BACKPORT_DIR)/$(DMABINMODULE_NAME).spec
	patch -p1 < $(BACKPORT_DIR)/scripts/disable_drm.patch;
	tar -cz $(RCS_TAR_IGNORE) -f $(DMABINMODULE_NAME)-$(DMAVERSION)-src.tar.gz \
	        $(DMA_TAR_CONTENT) $(DMABINMODULE_NAME).spec;
	+rpmbuild $(RPMOPTS) --target $(ARCH) -ba $(DMABINMODULE_NAME).spec \
	--define='_smp_mflags %{nil}' --define='_sourcedir $(PWD)'
	patch -p1 -R < $(BACKPORT_DIR)/scripts/disable_drm.patch ;


# i915dkmsrpm-pkg
# Creates Backports i915 alone dkms package
# command: make BUILD_VERSION=<build version> RELEASE_TYPE=<opensource/prerelease/custom> i915dkmsrpm-pkg
# BUILD_VERSION : pass build version to be added to package name
# RELEASE_TYPE : <opensource/prerelease> package need to be created
# Depends on package generated by dmadkmsrpm-pkg
#------------------------------------------------------------------------------
export KBUILD_ONLYI915DIRS := $(sort $(filter-out arch/%,$(vmlinux-alldirs)) drivers/gpu drivers/char drivers/platform drivers/pci include scripts)
I915_TAR_CONTENT := $(KBUILD_ONLYI915DIRS) .config Makefile* local-symbols MAINTAINERS \
               Kconfig* COPYING versions defconfigs backport-include kconf compat
I915DKMSMKSPEC := $(BACKPORT_DIR)/scripts/backport-mki915dkmsspec
I915DKMSMKCONF := $(BACKPORT_DIR)/scripts/backport-mki915dkmsconf
I915MODULE_NAME := intel-i915-dkms$(PKG_SUFFIX)
I915VERSION := $(VERSION)
I915RELEASE := $(RELEASE)

.PHONY: i915dkmsrpm-pkg
i915dkmsrpm-pkg:
	cp $(BACKPORT_DIR)/defconfigs/i915 .config
	$(CONFIG_SHELL) $(I915DKMSMKCONF) -n $(I915MODULE_NAME) -v $(I915VERSION) -r $(I915RELEASE) -p $(RELEASE_TYPE) > $(BACKPORT_DIR)/dkms.conf
	$(CONFIG_SHELL) $(I915DKMSMKSPEC) -n $(I915MODULE_NAME) -v $(I915VERSION) -r $(I915RELEASE) -p $(RELEASE_TYPE) > $(BACKPORT_DIR)/$(I915MODULE_NAME).spec
	patch -p1 < $(BACKPORT_DIR)/scripts/disable_dma.patch ;
	tar -cz $(RCS_TAR_IGNORE) -f $(I915MODULE_NAME)-$(I915VERSION)-src.tar.gz \
	        $(I915_TAR_CONTENT) $(I915MODULE_NAME).spec dkms.conf;
	+rpmbuild $(RPMOPTS) --target $(ARCH) -ta $(I915MODULE_NAME)-$(I915VERSION)-src.tar.gz \
	--define='_smp_mflags %{nil}'
	patch -p1 -R < $(BACKPORT_DIR)/scripts/disable_dma.patch ;

# i915binrpm-pkg
# Creates Backport binary package for i915
# command: make BUILD_VERSION=<build version> RELEASE_TYPE=<opensource/prerelease/custom> i915binrpm-pkg
# BUILD_VERSION : pass build version to be added to package name
# RELEASE_TYPE : <opensource/prerelease> package need to be created
# Build machine should have kernel version and variant binary is targeted for.
# Depends on package generated by dmabinrpm-pkg and should be installed on build machine
#------------------------------------------------------------------------------
export KBUILD_ONLYI915DIRS := $(sort $(filter-out arch/%,$(vmlinux-alldirs)) drivers/gpu drivers/char drivers/platform drivers/pci include scripts)
I915BINMKSPEC := $(BACKPORT_DIR)/scripts/backport-mki915binspec
I915BINMODULE_NAME := intel-i915$(PKG_SUFFIX)

.PHONY: i915binrpm-pkg
i915binrpm-pkg:
	cp $(BACKPORT_DIR)/defconfigs/i915 .config
	$(CONFIG_SHELL) $(I915BINMKSPEC) -n $(I915BINMODULE_NAME) -v $(I915VERSION) -r $(I915RELEASE) -p $(RELEASE_TYPE) > $(BACKPORT_DIR)/$(I915BINMODULE_NAME).spec
	patch -p1 < $(BACKPORT_DIR)/scripts/disable_dma.patch ;
	tar -cz $(RCS_TAR_IGNORE) -f $(I915BINMODULE_NAME)-$(I915VERSION)-src.tar.gz \
	        $(I915_TAR_CONTENT) $(I915BINMODULE_NAME).spec;
	+rpmbuild $(RPMOPTS) --target $(ARCH) -ta $(I915BINMODULE_NAME)-$(I915VERSION)-src.tar.gz \
	--define='_smp_mflags %{nil}' --define='_sourcedir $(PWD)'
	patch -p1 -R < $(BACKPORT_DIR)/scripts/disable_dma.patch ;

# binrpm-pkg
# Creates Backport binary package for Dma-buf, drm, i915 combined
# command: make BUILD_VERSION=<build version> RELEASE_TYPE=<opensource/prerelease/custom> binrpm-pkg
# BUILD_VERSION : pass build version to be added to package name
# RELEASE_TYPE : <opensource/prerelease> package need to be created
# Build machine should have kernel version and variant binary is targeted for.
#------------------------------------------------------------------------------
export KBUILD_DIRS := $(sort $(filter-out arch/%,$(vmlinux-alldirs)) drivers/dma-buf drivers/gpu drivers/char drivers/platform drivers/pci include scripts)
BINMKSPEC := $(BACKPORT_DIR)/scripts/backport-mkbinspec
BINMODULE_NAME := intel-i915$(PKG_SUFFIX)
TAR_CONTENT := $(KBUILD_DIRS) .config Makefile* local-symbols MAINTAINERS \
               Kconfig* COPYING versions defconfigs backport-include kconf compat

.PHONY: binrpm-pkg
binrpm-pkg:
	cp $(BACKPORT_DIR)/defconfigs/drm .config
	$(CONFIG_SHELL) $(BINMKSPEC) -n $(BINMODULE_NAME) -v $(I915VERSION) -r $(I915RELEASE) -p $(RELEASE_TYPE) > $(BACKPORT_DIR)/$(BINMODULE_NAME).spec
	tar -cz $(RCS_TAR_IGNORE) -f $(BINMODULE_NAME)-$(I915VERSION)-src.tar.gz \
	        $(TAR_CONTENT) $(BINMODULE_NAME).spec;
	+rpmbuild $(RPMOPTS) --target $(ARCH) -ta $(BINMODULE_NAME)-$(I915VERSION)-src.tar.gz \
	--define='_smp_mflags %{nil}' --define='_sourcedir $(PWD)'

# dkmsrpm-pkg
# Creates Backports both dmabuf and i915 dkms packages
# command: make BUILD_VERSION=<build version> RELEASE_TYPE=<opensource/prerelease/"custom"> dkmsrpm-pkg
# RELEASE_TYPE=<custom> is used to create custome package.
# Example: RELEASE_TYPE=test
# 	   Package names would be intel-dmabuf-dkms-test, intel-i915-dkms-test
# Note: If custom packages are created, tracking the conflicting package is difficult. Make sure no other package is
# already installed before you intalling current one.
.PHONY: dkmsrpm-pkg
dkmsrpm-pkg: i915dkmsrpm-pkg dmadkmsrpm-pkg

.PHONY: mrproper
mrproper:
	@test -f .config && $(MAKE) clean || true
	@rm -f .config
	@rm -f .kernel_config_md5 Kconfig.versions Kconfig.kernel
	@rm -f backport-include/backport/autoconf.h
	@rm -f $(BACKPORT_DIR)/$(DMAMODULE_NAME).spec
	@rm -f $(BACKPORT_DIR)/$(I915MODULE_NAME).spec
	@rm -f $(BACKPORT_DIR)/dkms.conf
	@rm -f $(BACKPORT_DIR)/$(DMAMODULE_NAME)-*-src.tar.gz
	@rm -f $(BACKPORT_DIR)/$(I915MODULE_NAME)-*-src.tar.gz
	@rm -f backport-include/backport/backport_path.h

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
ifneq ($(MAKECMDGOALS) , dkmsrpm-pkg)
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

.PHONY: help
help: defconfig-help
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
else
include $(BACKPORT_DIR)/Makefile.kernel
endif

PHONY += FORCE
FORCE:
