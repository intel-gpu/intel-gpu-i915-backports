# Intel® Graphics Driver Backports for Linux® OS (intel-gpu-i915-backports)

This backport provides early access of discrete GFX functionalities which are not upstreamed yet. Currently we are supporting Intel® Arc™ A­-Series Graphics (Codename Alchemist) Intel® Data Center GPU Flex Series Intel® Data Center GPU Max Series

For Alchemist discrete Graphics cards, support is provided without display. This repo can be used for the features like GPU debug functionality.
For normal cases, please use upstream 6.2 or later kernel version.

You can create Dynamic Kernel Module Support (DKMS) as well as pre-compiled out-of-tree modules packages, which can be installed on supported OS distributions.

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
| Red Hat® Enterprise Linux® 9.4       |  [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_redhat.md)| Yes | Yes |
| Red Hat® Enterprise Linux® 9.3       |  [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_redhat.md)| Yes | Yes |
| Red Hat® Enterprise Linux® 9.2       |  [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_redhat.md)| Yes | Yes |
| Red Hat® Enterprise Linux® 9.0       |  [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_redhat.md)| Yes | Yes |
| Red Hat® Enterprise Linux® 8.10       |  [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_redhat.md)| Yes | Yes |
| Red Hat® Enterprise Linux® 8.9       |  [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_redhat.md)| Yes | Yes |
| Red Hat® Enterprise Linux® 8.8       |  [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_redhat.md)| Yes | Yes |
| Red Hat® Enterprise Linux® 8.6       |  [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_redhat.md)| Yes | Yes |
| SUSE® Linux® Enterprise Server 15SP6 |  [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_sles.md)| Yes | Yes |
| SUSE® Linux® Enterprise Server 15SP5 |  [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_sles.md)| Yes | Yes |
| SUSE® Linux® Enterprise Server 15SP4 |  [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_sles.md)| Yes | Yes |
| Ubuntu® 24.04 Desktop (6.8 generic)  |  [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_ubuntu.md)| Yes | Yes |
| Ubuntu® 24.04 Server (6.8 generic)   |  [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_ubuntu.md)| Yes | Yes |
| Ubuntu® 22.04 Desktop (6.5 generic)  |  [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_ubuntu.md)| Yes | Yes |
| Ubuntu® 22.04 Server (5.15 generic)  |  [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_ubuntu.md)| Yes | Yes |
| Vanilla Kernel (6.6 LTS)  | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_vanilla.md)| Yes | No |
| Vanilla Kernel (6.1 LTS)  | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_vanilla.md)| Yes | No |
| Vanilla Kernel (5.15 LTS) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_vanilla.md)| Yes | No |
| Vanilla Kernel (5.10 LTS) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_vanilla.md)| Yes | No |

Note: [redhat/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/redhat/main) branch is deprecated, please use the [backport/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/backport/main) branch for Red Hat®.

## Product Releases:

Two types of release streams are available:

1. Production/LTS release:
   - We recommend the LTS stream for most use cases due to its stability. Features, hardware support, and compatibility with major operating systems introduced at the start of an active release will remain consistent throughout its lifetime. During this period, we will provide updates to address critical bugs and security issues. Intel offers a single active production release. Update releases provide enhancements and fixes to this initial release and are not separate versions. New updates can only be applied to the most recent previous update. The following table lists recent production and LTS release.
2. Rolling Stable Releases:
   - Use rolling stable releases for early access to new features and hardware support. Rolling updates include a mix of feature enhancements, as well as bug and security fixes. The risk of new bugs and regressions is higher with rolling stable releases compared to production and LTS releases. We recommend installing the latest release for any updates, including bug fixes. Support for major and minor operating system versions is locked at the time the rolling update is published. We recommend moving to the latest release as soon as it becomes available. Intel does not provide updates for previous rolling stable releases. These releases are listed here solely for changelog information. Only the most recent active rolling stable release is recommended for use.

## Active LTS/Production releases:

| Type | Status | Version | Release-date | Branch | Tag |
|---    |---    |---   |---    |---    |---    |
| **LTS update release** | **Active** | **2350.103** | **2024-09-25** | **backport/RELEASE_2405_23.10** | [I915_24WW39.4_803.103_23.10.72_231129.76](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/RELEASE_2405_23.10/README.md) |
| Production update release | Not supported | 2328.48 | 2023-12-19 | backport/RELEASE_2335_23.6 | [I915_23WW51.5_682.48_23.6.42_230425.56](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/RELEASE_2335_23.6/README.md)|
| Production update release | Not supported | 2328.48 | 2023-12-19 | redhat/RELEASE_2335_23.6 | [RHEL89_23WW51.5_682.48_23.6.42_230425.55](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/redhat/RELEASE_2335_23.6/README.md) |

Please select the appropriate tag for a particular branch based on the supported OS table [Supported OS Distributions](#supported-os-distributions)

Please refer to [Releases](https://dgpu-docs.intel.com/releases/index.html) for more details.
