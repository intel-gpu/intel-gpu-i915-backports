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

# BKPT_VER is extracted from BACKPORTS_RELEASE_TAG, which is auto genereated from backport description.Tagging is needed
# for decoding this, Sample in the version file 'BACKPORTS_RELEASE_TAG="SUSE_SLES15SP2_DG1_200901.0-1-g1a26a874"'
# Backports tagging is needed for this to work, for example "SUSE_SLES15SP2_DG1_200901.0" is the tag for 200901.0
# release in backports branch

BKPT_VER=$(shell cat versions | grep BACKPORTS_RELEASE_TAG | cut -d "_" -f 8 | cut -d "\"" -f 1 | cut -d "-" -f 1 2>/dev/null || echo 1)

BASE_KER_VER = $(shell cat $(KLIB_BUILD)/include/generated/autoconf.h | grep CONFIG_VERSION_SIGNATURE | cut -d '"' -f 2 | cut -d " " -f 2 | cut -d "-" -f 1,2 | cut -d '.' -f 1-3 2> /dev/null)

KERN_VER = $(shell cat $(KLIB_BUILD)/include/generated/autoconf.h | grep CONFIG_VERSION_SIGNATURE | cut -d '"' -f 2 | cut -d " " -f 2 | cut -d "-" -f 1 | cut -d '.' -f 1,2 2> /dev/null)

# DII_TAG is extracted from DII_KERNEL_TAG, which is auto genereated from base kernel source. Tagging is needed
# for decoding this, Sample in the version file DII_KERNEL_TAG="PROD_DG1_200828.0"
DII_TAG=$(shell cat versions | grep DII_KERNEL_TAG | cut -f 2 -d "\"" | cut -d "_" -f 2 2>/dev/null || echo 1)

KER_VER := $(shell cat versions | grep BASE_KERNEL_NAME_22.04 | cut -d "\"" -f 2 | cut -d "-" -f 2-|sed "s/-/./g" 2>/dev/null || echo 1)
UBUNTU_BACKPORT_MAJOR = $(shell cat $(KLIB_BUILD)/include/generated/autoconf.h | grep CONFIG_VERSION_SIGNATURE | cut -d '-' -f 2 | cut -d '.' -f 1 2> /dev/null)
UBUNTU_BACKPORT_MINOR = $(shell cat $(KLIB_BUILD)/include/generated/autoconf.h | grep CONFIG_VERSION_SIGNATURE | cut -d '-' -f 2 | cut -d '.' -f 2 2> /dev/null)

ifeq ($(UBUNTU_BACKPORT_MAJOR),)
	UBUNTU_BACKPORT_MAJOR = 0;
else ifeq ($(UBUNTU_BACKPORT_MINOR),)
	UBUNTU_BACKPORT_MINOR = 0;
endif

ifeq ($(shell expr $(KERN_VER) \<= 5.16), 1)
	OSV_VER = 20.04
else ifeq ($(shell expr $(KERN_VER) \>= 5.17), 1)
	OSV_VER = 22.04
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

# VERSION is generated as 0.DII_TAG.BackportVersion
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
        echo '#define UBUNTU_BACKPORT_MAJOR $(UBUNTU_BACKPORT_MAJOR)'; \
        echo '#define UBUNTU_BACKPORT_MINOR $(UBUNTU_BACKPORT_MINOR)'; \
	echo '#define UBUNTU_BACKPORT_RELEASE_VERSION(a,b) (((a) << 16) + ((b) << 8))'; \
        echo '#define UBUNTU_BACKPORT_RELEASE_CODE $(shell \
	expr $(UBUNTU_BACKPORT_MAJOR) \* 65536 + 0$(UBUNTU_BACKPORT_MINOR) \* 256)'
endef

$(version_h): $(BACKPORT_DIR)/Makefile FORCE
ifeq ($(OSV_VER), 20.04)
	@gawk -i inplace '!/BASE_KERNEL_NAME_$(OSV_VER)/' versions
	@echo 'BASE_KERNEL_NAME_$(OSV_VER)="$(BASE_KER_VER)"' >> versions
else ifeq ($(OSV_VER), 22.04)
	@gawk -i inplace '!/BASE_KERNEL_NAME_$(OSV_VER)/' $(BACKPORT_DIR)/versions
	@echo 'BASE_KERNEL_NAME_$(OSV_VER)="$(BASE_KER_VER)"' >> versions
endif
	$(call filechk,osv_version.h)

# VERSION is generated as 1.DII_TAG.BackportVersion
VERSION := 0.$(DII_TAG).$(BKPT_VER).$(KER_VER)

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

# i915dkmsdeb-pkg
# Creates Backports i915 alone dkms package
# command: make BUILD_VERSION=<build version> RELEASE_TYPE=<opensource/prerelease/custom> i915dkmsdeb-pkg
# BUILD_VERSION : pass build version to be added to package name
# RELEASE_TYPE : <opensource/prerelease> package need to be createdi
# RELEASE_TYPE=<custom> is used to create custome package.
# Example: RELEASE_TYPE=test
#         Package names would be intel-dmabuf-dkms-test, intel-i915-dkms-test
# Note: If custom packages are created, tracking the conflicting package is difficult. Make sure no other package is
# already installed before you intalling current one.
# ------------------------------------------------------------------------------
I915DKMSMK_CONTROL := $(BACKPORT_DIR)/scripts/backport-mkdebcontrol
I915DKMSMK_RULES := $(BACKPORT_DIR)/scripts/backport-mkdebrules
I915DKMSMK_INSTALL := $(BACKPORT_DIR)/scripts/backport-mkdebinstall
I915DKMSMK_DKMS := $(BACKPORT_DIR)/scripts/backport-mkdebdkms
I915DKMSMK_README := $(BACKPORT_DIR)/scripts/backport-mkdebreadme
I915DKMSMK_COPYRIGHT := $(BACKPORT_DIR)/scripts/backport-mkdebcopyright

I915MODULE_NAME := intel-i915-dkms$(PKG_SUFFIX)

I915VERSION := $(VERSION)
I915RELEASE := $(RELEASE)

.PHONY: i915dkmsdeb-pkg
i915dkmsdeb-pkg:
	$(CONFIG_SHELL) $(I915DKMSMK_CONTROL) -n $(I915MODULE_NAME) -v $(I915VERSION) -r $(I915RELEASE) -p $(RELEASE_TYPE) > $(BACKPORT_DIR)/debian/control
	$(CONFIG_SHELL) $(I915DKMSMK_RULES) -n $(I915MODULE_NAME) -v $(I915VERSION) -r $(I915RELEASE) -p $(RELEASE_TYPE) > $(BACKPORT_DIR)/debian/rules
	cp $(BACKPORT_DIR)/debian/changelog.in $(BACKPORT_DIR)/debian/changelog
	sed -i 's/pkg-name/$(I915MODULE_NAME)/g' $(BACKPORT_DIR)/debian/changelog
	cp $(BACKPORT_DIR)/debian/package.install.in $(BACKPORT_DIR)/debian/$(I915MODULE_NAME).install.in
	$(CONFIG_SHELL) $(I915DKMSMK_DKMS) -n $(I915MODULE_NAME) -v $(I915VERSION) -r $(I915RELEASE) -p $(RELEASE_TYPE) > $(BACKPORT_DIR)/debian/$(I915MODULE_NAME).dkms.in
	$(CONFIG_SHELL) $(I915DKMSMK_README) -n $(I915MODULE_NAME) -v $(I915VERSION) -r $(I915RELEASE) -p $(RELEASE_TYPE) > $(BACKPORT_DIR)/debian/README.Debian
	$(CONFIG_SHELL) $(I915DKMSMK_COPYRIGHT) -n $(I915MODULE_NAME) -v $(I915VERSION) -r $(I915RELEASE) -p $(RELEASE_TYPE) > $(BACKPORT_DIR)/debian/copyright
	+dch -l "+i${I915RELEASE}-" -m "build ${I915RELEASE}"
	+dpkg-buildpackage -j`nproc --all` -us -uc -b -rfakeroot


.PHONY: mrproper
mrproper:
	@test -f .config && $(MAKE) clean || true
	@rm -f .config
	@rm -f .kernel_config_md5 Kconfig.versions Kconfig.kernel
	@rm -f backport-include/backport/autoconf.h
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
			if [ "$$print" = "1" ] ; then					\
				echo config KERNEL_$$(echo $$v | tr . _)	;\
				echo "    def_bool y"					;\
			fi								;\
			if [ "$$v" = "$$kver" ] ; then print=1 ; fi			;\
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
