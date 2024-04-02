# Intel® Graphics Driver Backports for Linux® OS (intel-gpu-i915-backports)

Contains the backported kernel module source code of intel GPUs on various OS distributions and LTS Kernels. You can create Dynamic Kernel Module Support (DKMS) as well as precompiled Out of Tree modules packages, which can be installed on supported OS distributions.

We are using [backport project](https://backports.wiki.kernel.org/index.php/Main_Page) to generate out-of-tree i915 kernel module source codes.

This repo is a code snapshot of version of backports and does not contain individual git change history.

## Out of tree kernel drivers
This repository contains the following drivers.
1. Intel® Graphics Driver Backports(i915) - The main graphics driver (includes a compatible DRM subsystem and dmabuf if necessary)
2. Intel® Converged Security Engine(cse) - Converged Security Engine
3. Intel® Platform Monitoring Technology(pmt/vsec) - Intel Platform Telemetry

## Dependencies

  These drivers have dependency on Intel® GPU firmware and a few more kernel mode drivers may be needed based on specific use cases, platform, and distributions. The source code of additional drivers should be available at <https://github.com/intel-gpu>

- [Intel® GPU firmware](https://github.com/intel-gpu/intel-gpu-firmware) - Firmware required by intel GPUs.

Each project is tagged consistently, so when pulling these repos, pull the same tag.

## Branch
[backport/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/backport/main) will point to the currently supported version of Ubuntu®, SLES® 15 SP4 and Red Hat® onwards.

## Supported OS Distributions

|   OSV | Installation Instructions | Building | Testing|
|---    | --- | --- | --- |
| Red Hat® Enterprise Linux® 9.3       |  [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_redhat.md)| Yes | Yes |
| Red Hat® Enterprise Linux® 9.2       |  [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_redhat.md)| Yes | Yes |
| Red Hat® Enterprise Linux® 9.0       |  [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_redhat.md)| Yes | Yes |
| Red Hat® Enterprise Linux® 8.9       |  [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_redhat.md)| Yes | Yes |
| Red Hat® Enterprise Linux® 8.8       |  [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_redhat.md)| Yes | Yes |
| Red Hat® Enterprise Linux® 8.6       |  [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_redhat.md)| Yes | Yes |
| SUSE® Linux® Enterprise Server 15SP5 |  [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_sles.md)| Yes | Yes |
| SUSE® Linux® Enterprise Server 15SP4 |  [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_sles.md)| Yes | Yes |
| Ubuntu® 22.04 Desktop (6.5 generic)  |  [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_ubuntu.md)| Yes | Yes |
| Ubuntu® 22.04 Server (5.15 generic)  |  [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_ubuntu.md)| Yes | Yes |
| Vanilla Kernel (6.1 LTS)  | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_vanilla.md)| Yes | No |
| Vanilla Kernel (5.15 LTS) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_vanilla.md)| Yes | No |
| Vanilla Kernel (5.10 LTS) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_vanilla.md)| Yes | No |

Note: [redhat/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/redhat/main) branch is deprecated, please use the [backport/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/backport/main) branch for Red Hat®.

## Product Releases:

Two types of release streams are available:

1. Production/LTS release:
   - Use Production/LTS releases for stability. Intel recommends production streams for most uses. Features, hardware support, major OS version support, etc. available  at launch remain locked. Targeted fixes for critical bugs/security issues will be provided through the lifetime of the release. New OS minor version support will be provided with updates.
2. Rolling Stable Releases:
   - Use Rolling Stable releases for early access to new features/new hardware. Rolling updates include a mix of feature changes and bug/security fixes. Risks of new bugs and regressions are higher for rolling stable than for production. Customers must install the next release for any updates, including bug fixes. Major and minor OS version support are locked.

## Active LTS/Production releases:

| Release | Type| Branch| Tag| Status|
|---    |---    |---    |---    |---    |
| 2350.29: initial released 20240131 | LTS | backport/RELEASE_2405_23.10 |[I915_24WW05.5_803.29_23.10.32_231129.32](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/RELEASE_2405_23.10/README.md)| Active|
| 2328.48:update released 20231229 | Production | backport/RELEASE_2335_23.6 |[I915_23WW51.5_682.48_23.6.42_230425.56](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/RELEASE_2335_23.6/README.md)| Obsolete |
| | Production | redhat/RELEASE_2335_23.6 | [RHEL89_23WW51.5_682.48_23.6.42_230425.55](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/redhat/RELEASE_2335_23.6/README.md)| Obsolete |

Please select the appropriate tag for a particular branch based on the supported OS table [Supported OS Distributions](#supported-os-distributions)

Please refer to [Releases](https://dgpu-docs.intel.com/releases/index.html) for more details.
