#
# Makefile for the output source package
#

ifeq ($(KERNELRELEASE),)

MAKEFLAGS += --no-print-directory
SHELL := /bin/bash
BACKPORT_DIR := $(shell pwd)
MKSPEC := $(BACKPORT_DIR)/scripts/bp-mkspec

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

# dkmsrpm-pkg
# Creates Backports dkms package
# command: make dkmsrpm-pkg
# Rpm generated can be copied to client machine and install
# will trigger source build and install on modules
#----------------------------------------------------------------------------
export KBUILD_ALLDIRS := $(sort $(filter-out arch/%,$(vmlinux-alldirs)) LICENSES arch drivers include scripts)
TAR_CONTENT := $(KBUILD_ALLDIRS) .config Makefile* local-symbols MAINTAINERS \
               Kconfig* COPYING versions kernel-defconfigs defconfigs backport-include \
	       kconf compat
DKMSMKSPEC := $(BACKPORT_DIR)/scripts/backport-mkdkmsspec
DKMSMKCONF := $(BACKPORT_DIR)/scripts/backport-mkdkmsconf
MODULE_NAME ?= intel-gpgpu-dkms-prerelease
ARCH := x86_64

# BKPT_VER is extracted from BACKPORTS_RELEASE_TAG, which is auto genereated from backport description.Tagging is needed
# for decoding this, Sample in the version file 'BACKPORTS_RELEASE_TAG="SUSE_SLES15SP2_DG1_200901.0-1-g1a26a874"'
# Backports tagging is needed for this to work, for example "SUSE_SLES15SP2_DG1_200901.0" is the tag for 200901.0
# release in backports branch
#
BKPT_VER=$(shell cat versions | grep BACKPORTS_RELEASE_TAG | cut -d "_" -f 7 | cut -d "\"" -f 1 | cut -d "-" -f 1 2>/dev/null || echo 1)

# DII_TAG is extracted from DII_KERNEL_TAG, which is auto genereated from base kernel source. Tagging is needed
# for decoding this, Sample in the version file DII_KERNEL_TAG="PROD_DG1_200828.0"
DII_TAG=$(shell cat versions | grep DII_KERNEL_TAG | cut -f 2 -d "\"" | cut -d "_" -f 2 2>/dev/null || echo 1)

KER_VER := $(shell cat versions | grep BASE_KERNEL_NAME | cut -d "\"" -f 2 | cut -d "-" -f 2-|sed "s/-/./g" 2>/dev/null || echo 1)

SLES_BACKPORT_MAJOR = $(shell cat $(KLIB_BUILD)/include/config/kernel.release | cut -d '-' -f 2 | cut -d '.' -f 1 2> /dev/null)
SLES_BACKPORT_MINOR = $(shell cat $(KLIB_BUILD)/include/config/kernel.release | cut -d '-' -f 2 | cut -d '.' -f 2 2> /dev/null)

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

define filechk_osv_version.h
        echo '#define SLES_BACKPORT_MAJOR $(SLES_BACKPORT_MAJOR)'; \
        echo '#define SLES_BACKPORT_MINOR $(SLES_BACKPORT_MINOR)'; \
        echo '#define SLES_RELEASE_VERSION(a,b) (((a) << 8) + (b))'; \
        echo '#define SLES_RELEASE_CODE \
                $(shell expr $(SLES_BACKPORT_MAJOR) \* 256 + $(SLES_BACKPORT_MINOR))'
endef

$(version_h): $(BACKPORT_DIR)/Makefile FORCE
	$(call filechk,osv_version.h)

# VERSION is generated as 1.DII_TAG.BackportVersion
VERSION := 0.$(DII_TAG).$(BKPT_VER).$(KER_VER)

ifneq ($(BUILD_VERSION), )
RELEASE := $(BUILD_VERSION)
else
RELEASE := 1
endif

RELEASE_TYPE ?= prerelease

.PHONY: dkmsrpm-pkg
dkmsrpm-pkg:
	$(CONFIG_SHELL) $(DKMSMKCONF) $(MODULE_NAME) $(VERSION) $(RELEASE) > $(BACKPORT_DIR)/dkms.conf
	$(CONFIG_SHELL) $(DKMSMKSPEC) $(MODULE_NAME) $(VERSION) $(RELEASE) > $(BACKPORT_DIR)/$(MODULE_NAME).spec
	tar -cz $(RCS_TAR_IGNORE) -f $(MODULE_NAME)-$(VERSION)-src.tar.gz \
		$(TAR_CONTENT) $(MODULE_NAME).spec dkms.conf;
	+rpmbuild $(RPMOPTS) --target $(ARCH) -ta $(MODULE_NAME)-$(VERSION)-src.tar.gz \
	--define='_smp_mflags %{nil}'

# dmadkmsrpm-pkg
# Creates Backports dmabuf dkms package
# command: make BUILD_VERSION=<build version> RELEASE_TYPE=<opensource/prerelease> dmadkmsrpm-pkg
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
ifeq ($(RELEASE_TYPE), opensource)
DMAMODULE_NAME := intel-dmabuf-dkms
else
DMAMODULE_NAME := intel-dmabuf-dkms-prerelease
endif
DMAVERSION := $(VERSION)
DMARELEASE := $(RELEASE)

.PHONY: dmadkmsrpm-pkg
dmadkmsrpm-pkg:
	$(CONFIG_SHELL) $(DMADKMSMKCONF) -n $(DMAMODULE_NAME) -v $(DMAVERSION) -r $(DMARELEASE) -p $(RELEASE_TYPE) > $(BACKPORT_DIR)/dkms.conf
	$(CONFIG_SHELL) $(DMADKMSMKSPEC) -n $(DMAMODULE_NAME) -v $(DMAVERSION) -r $(DMARELEASE) -p $(RELEASE_TYPE) > $(BACKPORT_DIR)/$(DMAMODULE_NAME).spec
	patch -p1 < $(BACKPORT_DIR)/scripts/disable_drm.patch ;
	tar -cz $(RCS_TAR_IGNORE) -f $(DMAMODULE_NAME)-$(DMAVERSION)-src.tar.gz \
	        $(DMA_TAR_CONTENT) $(DMAMODULE_NAME).spec dkms.conf;
	+rpmbuild $(RPMOPTS) --target $(ARCH) -ta $(DMAMODULE_NAME)-$(DMAVERSION)-src.tar.gz \
	--define='_smp_mflags %{nil}'
	patch -p1 -R < $(BACKPORT_DIR)/scripts/disable_drm.patch ;

# i915dkmsrpm-pkg
# Creates Backports i915 alone dkms package
# command: make BUILD_VERSION=<build version> RELEASE_TYPE=<opensource/prerelease> i915dkmsrpm-pkg
# BUILD_VERSION : pass build version to be added to package name
# RELEASE_TYPE : <opensource/prerelease> package need to be created
# Depends on package generated by dmadkmsrpm-pkg
#------------------------------------------------------------------------------
export KBUILD_ONLYI915DIRS := $(sort $(filter-out arch/%,$(vmlinux-alldirs)) drivers/gpu drivers/char drivers/platform include scripts)
I915_TAR_CONTENT := $(KBUILD_ONLYI915DIRS) .config Makefile* local-symbols MAINTAINERS \
               Kconfig* COPYING versions defconfigs backport-include kconf compat
I915DKMSMKSPEC := $(BACKPORT_DIR)/scripts/backport-mki915dkmsspec
I915DKMSMKCONF := $(BACKPORT_DIR)/scripts/backport-mki915dkmsconf
ifeq ($(RELEASE_TYPE), opensource)
I915MODULE_NAME := intel-i915-dkms
else
I915MODULE_NAME := intel-i915-dkms-prerelease
endif

I915VERSION := $(VERSION)
I915RELEASE := $(RELEASE)

.PHONY: i915dkmsrpm-pkg
i915dkmsrpm-pkg:
	$(CONFIG_SHELL) $(I915DKMSMKCONF) -n $(I915MODULE_NAME) -v $(I915VERSION) -r $(I915RELEASE) -p $(RELEASE_TYPE) > $(BACKPORT_DIR)/dkms.conf
	$(CONFIG_SHELL) $(I915DKMSMKSPEC) -n $(I915MODULE_NAME) -v $(I915VERSION) -r $(I915RELEASE) -p $(RELEASE_TYPE) > $(BACKPORT_DIR)/$(I915MODULE_NAME).spec
	patch -p1 < $(BACKPORT_DIR)/scripts/disable_dma.patch ;
	tar -cz $(RCS_TAR_IGNORE) -f $(I915MODULE_NAME)-$(I915VERSION)-src.tar.gz \
	        $(I915_TAR_CONTENT) $(I915MODULE_NAME).spec dkms.conf;
	+rpmbuild $(RPMOPTS) --target $(ARCH) -ta $(I915MODULE_NAME)-$(I915VERSION)-src.tar.gz \
	--define='_smp_mflags %{nil}'
	patch -p1 -R < $(BACKPORT_DIR)/scripts/disable_dma.patch ;

#binrpm-pkg
#----------------------------------------------------------------------------

.PHONY: binrpm-pkg
binrpm-pkg:
	 $(MAKE) -f $(BACKPORT_DIR)/Makefile KLIB=$(KLIB) KLIB_BUILD=$(KLIB_BUILD) 
	 $(CONFIG_SHELL) $(MKSPEC) prebuilt > $(BACKPORT_DIR)/backport-modules.spec
	 +rpmbuild --build-in-place -bb $(BACKPORT_DIR)/backport-modules.spec

.PHONY: mrproper
mrproper:
	@test -f .config && $(MAKE) clean || true
	@rm -f .config
	@rm -f $(BACKPORT_DIR)/backportkernel.spec
	@rm -f .kernel_config_md5 Kconfig.versions Kconfig.kernel
	@rm -f backport-include/backport/autoconf.h
	@rm -f $(BACKPORT_DIR)/backport-modules.spec
	@rm -f $(BACKPORT_DIR)/$(MODULE_NAME).spec
	@rm -f $(BACKPORT_DIR)/$(DMAMODULE_NAME).spec
	@rm -f $(BACKPORT_DIR)/$(I915MODULE_NAME).spec
	@rm -f $(BACKPORT_DIR)/dkms.conf
	@rm -f $(BACKPORT_DIR)/$(MODULE_NAME)-*-src.tar.gz
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
