
#  Intel® Graphics Driver Backports for Linux® OS (intel-gpu-i915-backports)

Contains the backported i915 Source Code of intel GPUs on various OSV Kernels. You can create Dynamic Kernel Module Support(DKMS) based packages, which can be installed on supported OS distributions.

We are using [backport project](https://backports.wiki.kernel.org/index.php/Main_Page) to generate out of tree i915 kernel module source codes.

This repo is a code snapshot of particular version of backports and does not contain individual git change history.

# Dependencies

This driver is part of a collection of kernel-mode drivers that enable support for Intel graphics. The backports collection within https://github.com/intel-gpu includes:

- [Intel® Graphics Driver Backports for Linux](https://github.com/intel-gpu/intel-gpu-i915-backports) - The main graphics driver (includes a compatible DRM subsystem and dmabuf if necessary)
- [Intel® Converged Security Engine Backports](https://github.com/intel-gpu/intel-gpu-cse-backports) - Converged Security Engine
- [Intel® Platform Monitoring Technology Backports](https://github.com/intel-gpu/intel-gpu-pmt-backports/) - Intel Platform Telemetry
- [Intel® GPU firmware](https://github.com/intel-gpu/intel-gpu-firmware) - Firmware required by intel GPUs.

Each project is tagged consistently, so when pulling these repos, pull the same tag.


# Branches
backport/main will point to the currently supported version of Ubuntu® and SLES.


# Supported OS Kernel/Distribution
  Our current backport supports the following OS Distribution.

| OS Distribution | OS Version | Kernel Version  | Installation Instructions |
|---  |---  |---  |--- |
| Ubuntu® | 22.04 | Kernel 5.17 oem | [README](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_ubuntu.md) |
| | 22.04/20.04 |  Kernel 5.15 generic | [README](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_ubuntu.md) |
| | Mainline LTS |  Kernel 5.15.72 | [README](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_ubuntu.md) |
| SLES | 15SP4 | Kernel 5.14 |  [README](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_sles.md) |


| Product  | Backport Released Tag|OS Version	 | Kernel Version  | Building | Testing |
|--- |--- |---  |---  |---  |--- |
|Intel® Data Center GPU Flex Series|I915_22WW51.5_518.5_6365.0.0_221124.0 |Ubuntu® 22.04 | 5.17.0-1019-oem| Yes |No |
|| | Ubuntu® 22.04 |  5.15.0-50 |Yes |Yes |
| || Mainline LTS |  5.15.72 |Yes |No |

