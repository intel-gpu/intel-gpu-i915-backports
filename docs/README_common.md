
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

		Display support has been disabled by default for all OSVs except Ubuntu OSVs.
                If you want to force enable display, pass BUILD_CONFIG=enabledispaly
                        Ex : make <Target> BUILD_CONFIG=enabledisplay
		The display is not present for the Data Center GPU's so this option should not be enable
		enabledisplay should only be enabled for DG2 if needed, but please note that we are not
		verifying display so it may be broken.

  OS_DISTRIBUTION: Distro targeted package
 			You can set this value by passing supported kernel name
 			Ex: make <Target> OS_DISTRIBUTION=UBUNTU_22.04_SERVER

--------------------------------------------------------------------------------------
--------------------------------------------------------------------------------------
 DKMS Targets:
 Debian Targets:
  dkmsdeb-pkg    - Build single DKMS Debian package for dmabuf, drm, i915 and dependent child drivers (mei and pmt/vsec)
  dmabufdkmsdeb-pkg - Build DKMS debian package for dmabuf
  drmdkmsdeb-pkg - Build DKMS debian package for drm, i915 and dependent child drivers (mei and pmt/vsec)
  i915dkmsdeb-pkg - Build DKMS debian package i915 and dependent child drivers (mei and pmt/vsec)

 Example: make i915dkmsdeb-pkg OS_DISTRIBUTION=UBUNTU_24.04_SERVER

 Debian package name contains UBUNTU_24.04_SERVER Kernel version as default.

 ##### List of Debian supported OS distro versions #####
 UBUNTU_24.04_DESKTOP	UBUNTU_24.04_SERVER
 UBUNTU_22.04_DESKTOP	UBUNTU_22.04_SERVER

 RPM Targets:
  dkmsrpm-pkg     - Build single DKMS RPM package for dmabuf, drm, i915 and dependent child drivers (mei and pmt/vsec)
  dmabufdkmsrpm-pkg  - Build dkms RPM package for dmabuf
  i915dkmsrpm-pkg - Build dkms RPM package for i915 and dependent child drivers (mei and pmt/vsec)
  drmdkmsrpm-pkg -  Build dkms RPM package for drm, i915 and dependent child drivers (mei and pmt/vsec)

 Example: make i915dkmsrpm-pkg OS_DISTRIBUTION=SLES15_SP6

 Rpm package contains the SLES15_SP6 Kernel version as default

 ##### List of RPM supported OS distro Versions #####
 SLES15_SP6     SLES15_SP5	SLES15_SP4
 RHEL_9.5	RHEL_9.4	RHEL_9.3	RHEL_9.2	RHEL_9.0	RHEL_8.10	RHEL_8.9	RHEL_8.8	RHEL_8.6

 ##### List of LTS kernel versions #####
 VANILLA_6.1LTS	VANILLA_5.15LTS	VANILLA_5.10LTS
 For LTS kernels, either RPM or Debian package can be created

 For Specific OS distro/LTS kernels, pass the supported kernel name to OS_DISTRIBUTION option

--------------------------------------------------------------------------------------
```
