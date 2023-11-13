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
| Red Hat® Enterprise Linux® 9.0       | [backport/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/backport/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_redhat.md)| Yes | Yes |
| Red Hat® Enterprise Linux® 8.8       | [redhat/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/redhat/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/redhat/main/docs/README_redhat.md)| Yes | Yes |
| Red Hat® Enterprise Linux® 8.6       | [redhat/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/redhat/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/redhat/main/docs/README_redhat.md)| Yes | Yes |
| SUSE® Linux® Enterprise Server 15SP5 | [backport/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/backport/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_sles.md)| Yes | Yes |
| SUSE® Linux® Enterprise Server 15SP4 | [backport/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/backport/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_sles.md)| Yes | Yes |
| Ubuntu® 22.04 Desktop (6.2 generic)  | [backport/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/backport/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_ubuntu.md)| Yes | Yes |
| Ubuntu® 22.04 Server (5.15 generic)  | [backport/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/backport/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_ubuntu.md)| Yes | Yes |
| Vanilla Kernel (5.15 LTS) | [backport/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/backport/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_vanilla.md)| Yes | No |
| Vanilla Kernel (5.10 LTS)       | [redhat/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/redhat/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/redhat/main/docs/README_vanilla.md)| Yes | No |

## Product Releases:

Two types of release streams are available:

1. Production/LTS release:
   - Use Production/LTS releases for stability. Intel recommends production streams for most uses. Features, hardware support, major OS version support, etc. available  at launch remain locked. Targeted fixes for critical bugs/security issues will be provided through the lifetime of the release. New OS minor version support will be provided with updates.
2. Rolling Stable Releases:
   - Use Rolling Stable releases for early access to new features/new hardware. Rolling updates include a mix of feature changes and bug/security fixes. Risks of new bugs and regressions are higher for rolling stable than for production. Customers must install the next release for any updates, including bug fixes. Major and minor OS version support are locked.


## Active Production release:

| Release | Branch| Tag|
|---    |---    |--- |
| 2328.38:update released 20230929 | backport |[I915_23WW39.5_682.38_23.6.37_230425.49](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/RELEASE_2335_23.6/README.md)|
| | redhat | [RHEL88_23WW39.5_682.38_23.6.37_230425.47](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/redhat/RELEASE_2335_23.6/README.md)|

Please select appropriate tag for a praticular branch based on the supported OS table [Supported OS Distributions](#supported-os-distributions)

Please refer [Releases](https://dgpu-docs.intel.com/releases/index.html) for more details.
