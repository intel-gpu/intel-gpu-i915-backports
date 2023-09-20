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

- [Intel® GPU firmware] (https://github.com/intel-gpu/intel-gpu-firmware) - Firmware required by intel GPUs.

Each project is tagged consistently, so when pulling these repos, pull the same tag.

## Supported OS Distributions

|   OSV |Branch         | Installation Instructions | Building | Testing|
|---    |---    | --- | --- | --- |
| Red Hat® Enterprise Linux® 9.2       | [backport/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/backport/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_redhat.md)| Yes | Yes |
| Red Hat® Enterprise Linux® 8.8       | [redhat/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/redhat/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/redhat/main/docs/README_redhat.md)| Yes | Yes |
| Red Hat® Enterprise Linux® 8.6       | [redhat/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/redhat/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/redhat/main/docs/README_redhat.md)| Yes | Yes |
| SUSE® Linux® Enterprise Server 15SP4 | [backport/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/backport/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_sles.md)| Yes | Yes |
| Ubuntu® 22.04 Desktop (5.19 generic)  | [backport/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/backport/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_ubuntu.md)| Yes | Yes |
| Ubuntu® 22.04 Server (5.15 generic)  | [backport/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/backport/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_ubuntu.md)| Yes | Yes |
| Vanilla Kernel (5.15 LTS) | [backport/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/backport/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_vanilla.md)| Yes | No |
| Vanilla Kernel (5.10 LTS)       | [redhat/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/redhat/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/redhat/main/docs/README_vanilla.md)| Yes | No |

## Product Releases:
Please refer [Releases](https://dgpu-docs.intel.com/releases/index.html)
