# Intel® Graphics Driver Backports for Linux® OS (intel-gpu-i915-backports)

Contains the backported kernel module source code of intel GPUs on various OS distributions and LTS Kernels. You can create Dynamic Kernel Module Support (DKMS) as well as precompiled Out of Tree modules packages, which can be installed on supported OS distributions.

We are using [backport project](https://backports.wiki.kernel.org/index.php/Main_Page) to generate out of tree i915 kernel module source codes.

This repo is a code snapshot of version of backports and does not contain individual git change history.


## Out of tree kernel drivers
This repository contains following drivers.
1. Intel® Graphics Driver Backports(i915) - The main graphics driver (includes a compatible DRM subsystem and dmabuf if necessary)
2. Intel® Converged Security Engine(cse) - Converged Security Engine
3. Intel® Platform Monitoring Technology(pmt/vsec) - Intel Platform Telemetry


## Dependencies

  These drivers have dependency on Intel® GPU firmware and few more kernel mode drivers may be needed based on specific use cases, platform, and distributions. Source code of additional drivers should be available at https://github.com/intel-gpu

- [Intel® GPU firmware](https://github.com/intel-gpu/intel-gpu-firmware) - Firmware required by intel GPUs.

Each project is tagged consistently, so when pulling these repos, pull the same tag.

## Branches
redhat/main will point to the currently supported version of RED HAT® 8.x and Vanilla 5.10 LTS.

## Supported OS Kernel/Distribution
  Our current backport supports the following OS Distribution.

| OS Distribution | OS Version | Kernel Version  | Installation Instructions |
|---  |---  |---  |--- |
| REDHAT® | 8.8 | 4.18.0-477 | [README](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/redhat/main/docs/README_redhat.md) |
| | 8.6 | 4.18.0-372 | [README](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/redhat/main/docs/README_redhat.md) |
| Vanilla LTS |  | 5.10  |  [README](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/redhat/main/docs/README_vanilla.md) |


## Product Releases:
Please refer [Releases](https://dgpu-docs.intel.com/releases/index.html)
