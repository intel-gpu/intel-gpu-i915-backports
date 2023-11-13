
# Intel® Graphics Driver Backports for Linux® OS (intel-gpu-i915-backports)

This branch provides i915 driver source code backported for Red Hat® Enterprise Linux® to enable intel discrete GPUs support.

We are using [backport project](https://backports.wiki.kernel.org/index.php/Main_Page) to generate out of tree i915 kernel module source codes.

This repo is a code snapshot of particular version of backports and does not contain individual git change history.

## Branches
 redhat/main will point to the currently supported version of Red Hat® Enterprise Linux®.

 We will add a new branch redhat/rhel<x.y> whenever a version is deprecated or moved to the maintenance phase.

## Supported Version/kernel
  Our current backport is based on Red Hat® Enterprise Linux® 8.8. We are using the header of the latest available kernel at the time of backporting. However, it may not be compatible with the latest version at the time of installation.
  Please refer [Version](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/redhat/main/versions)
  file to check the RHEL_8.8_KERNEL_VERSION. It will point to the kernel version which is being used during backporting.

  In case of an issue with the latest kernel, please install the kernel version pointed by RHEL_8.8_KERNEL_VERSION.

    $sudo dnf check-update; sudo dnf install -y kernel-<RHEL_8.8_KERNEL_VERSION>.el8_8.x86_64 \
    kernel-devel-<RHEL_8.8_KERNEL_VERSION>.el8_8.x86_64
    Example:
         $sudo dnf check-update; sudo dnf install -y kernel-4.18.0-477.13.1.el8_8.x86_64 \
         kernel-devel-4.18.0-477.13.1.el8.x86_64

## Product Releases:
Please refer [Releases](https://dgpu-docs.intel.com/releases/index.html)

## Prerequisite
We have dependencies on the following packages
  - make
  - linux-glibc-devel
  - lsb-release
  - rpm-build
  - flex
  - bison
  - awk
```
$sudo dnf install make linux-glibc-devel lsb-release rpm-build bison flex awk
```
For dkms modules, we need to install `dkms` package also.

```
$sudo dnf install dkms
```

## Out of tree kernel drivers
This repository contains following drivers.
1. Intel® Graphics Driver Backports(i915) - The main graphics driver (includes a compatible DRM subsystem and dmabuf if necessary)
2. Intel® Converged Security Engine(cse) - Converged Security Engine
3. Intel® Platform Monitoring Technology(pmt/vsec) - Intel Platform Telemetry

## Dependencies

These drivers have dependency on Intel® GPU firmware and few more kernel mode drivers may be needed based on specific use cases, platform, and distributions. Source code of additional drivers should be available at https://github.com/intel-gpu

- [Intel® GPU firmware](https://github.com/intel-gpu/intel-gpu-firmware) - Firmware required by intel GPUs.

Each project is tagged consistently, so when pulling these repos, pull the same tag.

## Package creation

### Dynamic Kernel Module Support(DKMS)

There are two ways to create i915 dkms packages.
1. Using default command:

```
$make dkmsrpm-pkg
Example: 
	$make dkmsrpm-pkg

      Generated package name:
		intel-dmabuf-dkms-1.23.6.24.230425.33-71.x86_64.rpm
		intel-i915-dkms-1.23.6.24.230425.33-71.x86_64.rpm
```
2. OS distribution option:

    Adds OS kernel version as part of dkms pacakge name.

```
$make dkmsrpm-pkg OS_DISTRIBUTION=<OS Distribution>
Example:
        $make dkmsrpm-pkg OS_DISTRIBUTION=RHEL_8.8
      
       Generated package name :
		intel-dmabuf-dkms-1.23.6.24.230425.33.4.18.0-477.13.1-71.x86_64.rpm
		intel-i915-dkms-1.23.6.24.230425.33.4.18.0-477.13.1-71.x86_64.rpm
```
  Use below help command to get the list of supported os distributions.
```
$make dkms-pkg-help

Generated outout:
   DKMS Targets:
    dkmsrpm-pkg  -  Build DKMS rpm package
   
   ##### List of RPM supported osv kernel versions #####
   RHEL_8.8
   RHEL_8.7
   RHEL_8.6
```
Above  will create rpm packages at $HOME/rpmbuild/RPMS/x86_64/

### Binary RPM

Creation of binary rpm can be done using the below command. By default it will use header of booted kernel, However it can be pointed to other headers via optional KLIB and KLIB_BUILD arguement
```
$make KLIB=<Header Path> KLIB_BUILD=<Header Path> binrpm-pkg

Exmaple:
	$make KLIB=/lib/modules/$(uname -r) KLIB_BUILD=/lib/modules/$(uname -r) binrpm-pkg

    Generated Files:
        intel-dmabuf-<version>.$(uname -r)-1.x86_64.rpm
        intel-i915-<version>.$(uname -r)-1.x86_64.rpm
```

## Installation

```
$sudo rpm -ivh intel-*.rpm
### Reboot the device after installation of all packages.
$sudo reboot
```

## Known limitation
KVGMT is not supported on Server Gfx cards so we need to blacklist KVGMT, on later version due to KVMGT refactoring
we may have unknown symbol warning during dkms installation.

Example:
```
depmod: WARNING: /lib/modules/$(uname -r)/kernel/drivers/gpu/drm/i915/kvmgt.ko.xz needs unknown symbol intel_gvt_set_ops
depmod: WARNING: /lib/modules/$(uname -r)/kernel/drivers/gpu/drm/i915/kvmgt.ko.xz needs unknown symbol intel_gvt_clear_ops
```
Blacklist kvmgt to avoid kvmgt module load during kernel boot.
```
$sudo grubby --update-kernel=/boot/vmlinuz-$(uname -r) --args="modprobe.blacklist=kvmgt"
```

## Installation varification
Please grep **backport**  from dmesg after reboot. you should see something like below

```
> sudo dmesg |grep -i backport
[2.684355] DMABUF-COMPAT BACKPORTED INIT
[2.684371] Loading dma-buf modules backported from I915-23.6.24
[2.684372] DMA BUF backport generated by backports.git RHEL_88_23.6.24_PSB_230425.33
[2.684910] I915 COMPAT BACKPORTED INIT
[2.684911] Loading I915 modules backported from I915-23.6.24
[2.684912] I915 Backport generated by backports.git RHEL_88_23.6.24_PSB_230425.33
[2.702432] [drm] DRM BACKPORTED INIT
[2.724641] [drm] DRM_KMS_HELPER BACKPORTED INIT
[2.838672] [drm] I915 BACKPORTED INIT
```
