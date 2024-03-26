
## Make Build Options

Build options provides list of different arguments which can be passed during package creation or build.
All backport build options can be found using below command.

```
$ make dkms-pkg-help
```
Sample output Generated:
```
--------------------------------------------------------------------------------------
Build Configurations:
  KLIB   : path/to/linux/kernel/headers
  KLIB_BUILD     : path/to/linux/kernel/headers/build
  BUILD_VERSION  : Pass build version to be added to package name
  RELEASE_TYPE   : <opensource/prerelease/custom> Package release type created
 			Example: make <Target> RELEASE_TYPE=test
 			Package names would be intel-i915-dkms-test for DKMS or intel-i915-test for binary
 			Note: If custom packages are created, tracking the conflicting package
 			is difficult. Make sure no other intel-i915* packages are already installed before
 			you installing current one.
  BUILD_CONFIG   : Specify build config variant
 			Ex: make <Target> BUILD_CONFIG=disabledisplay
  OS_DISTRIBUTION: Distro targeted package
 			You can set this value by passing supported kernel name
 			Ex: make <Target> OS_DISTRIBUTION=UBUNTU_22.04_SERVER

--------------------------------------------------------------------------------------
--------------------------------------------------------------------------------------
 DKMS Targets:
 Debian Targets:
  dkmsdeb-pkg    - Build single DKMS Debian package for dmabuf, drm, i915 and dependent child drivers (mei and pmt/vsec)
  dmadkmsdeb-pkg - Build DKMS debian package for dmabuf
  drmdkmsdeb-pkg - Build DKMS debian package for drm, i915 and dependent child drivers (mei and pmt/vsec)
  i915dkmsdeb-pkg - Build DKMS debian package i915 and dependent child drivers (mei and pmt/vsec)

 Example: make i915dkmsdeb-pkg OS_DISTRIBUTION=UBUNTU_22.04_SERVER

 Debian package name contains UBUNTU_22.04_SERVER Kernel version as default.

 ##### List of Debian supported OS distro versions #####
 UBUNTU_22.04_DESKTOP	UBUNTU_22.04_SERVER

 RPM Targets:
  dkmsrpm-pkg     - Build single DKMS RPM package for dmabuf, drm, i915 and dependent child drivers (mei and pmt/vsec)
  dmadkmsrpm-pkg  - Build dkms RPM package for dmabuf
  i915dkmsrpm-pkg - Build dkms RPM package for i915 and dependent child drivers (mei and pmt/vsec)
  drmdkmsrpm-pkg -  Build dkms RPM package for drm, i915 and dependent child drivers (mei and pmt/vsec)

 Example: make i915dkmsrpm-pkg OS_DISTRIBUTION=SLES15_SP5

 Rpm package contains the SLES15_SP5 Kernel version as default

 ##### List of RPM supported OS distro Versions #####
 SLES15_SP5	SLES15_SP4
 RHEL_9.3	RHEL_9.2	RHEL_9.0	RHEL_8.9	RHEL_8.8	RHEL_8.6

 ##### List of LTS kernel versions #####
 VANILLA_6.1LTS	VANILLA_5.15LTS	VANILLA_5.10LTS	VANILLA_5.4LTS
 For LTS kernels, either RPM or Debian package can be created

 For Specific OS distro/LTS kernels, pass the supported kernel name to OS_DISTRIBUTION option

--------------------------------------------------------------------------------------
```
