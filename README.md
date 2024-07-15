
# Intel® Graphics Driver Backports for Linux® OS (intel-gpu-i915-backports)

Contains the backported kernel module source code for Intel® GPUs on various OS distributions and LTS Kernels.

This backport provides early access of discrete GFX functionalities which are not upstreamed yet.
Currently we are supporting
	Intel® Arc™ A-Series Graphics (Codename Alchemist)
	Intel® Data Center GPU Flex Series
	Intel® Data Center GPU Max Series

For Alchemist discrete Graphics cards, support is provided without display. This repo can be used for the features like GPU debug functionality. For normal cases, please use upstream 6.2 or later kernel version.

You can create Dynamic Kernel Module Support (DKMS) as well as pre-compiled out-of-tree modules packages, which can be installed on supported OS distributions.

We are using [backport project](https://backports.wiki.kernel.org/index.php/Main_Page) to generate out-of-tree i915 kernel module source codes.

This repo is a code snapshot of version of backports and does not contain individual git change history.

## Out-of-tree kernel drivers
This repository contains following drivers.
1. Intel® Graphics Driver Backports(i915) - The main graphics driver (includes a compatible DRM subsystem and dmabuf if necessary)
2. Intel® Converged Security Engine(CSE) - Converged Security Engine
3. Intel® Platform Monitoring Technology(PMT/VSEC) - Intel® Platform Telemetry

## Supported OS Kernel/Distribution
  Our current backport supports the following OS Distribution.

| OS Distribution | OS Version | Kernel Version  | Installation Instructions |
|---  |---  |---  |--- |
| Ubuntu® Desktop | 24.04 | 6.8 generic | [README](docs/README_ubuntu.md) |
| Ubuntu® Server | 24.04 | 6.8 generic | [README](docs/README_ubuntu.md) |
| Ubuntu® Desktop | 22.04 | 6.5 generic | [README](docs/README_ubuntu.md) |
| Ubuntu® Server | 22.04 | 5.15 generic | [README](docs/README_ubuntu.md) |
| SLES® | 15SP5 |  5.14.21.150500.xx |  [README](docs/README_sles.md) |
| SLES® | 15SP4 |  5.14.21.150400.xx |  [README](docs/README_sles.md) |
| RHEL® | 9.4  |  5.14.0-427.xx |  [README](docs/README_redhat.md) |
| RHEL® | 9.3  |  5.14.0-362.xx |  [README](docs/README_redhat.md) |
| RHEL® | 9.2  |  5.14.0-284.xx |  [README](docs/README_redhat.md) |
| RHEL® | 9.0  |  5.14.0-70.xx |  [README](docs/README_redhat.md) |
| RHEL® | 8.10 |  4.18.0-544.xx |  [README](docs/README_redhat.md) |
| RHEL® | 8.9  |  4.18.0-513.xx |  [README](docs/README_redhat.md) |
| RHEL® | 8.8  |  4.18.0-477.xx |  [README](docs/README_redhat.md) |
| RHEL® | 8.6  |  4.18.0-372.xx |  [README](docs/README_redhat.md) |
| Vanilla LTS* |  |  6.1.xx  | [README](docs/README_vanilla.md) |
| Vanilla LTS* |  |  5.15.xx | [README](docs/README_vanilla.md) |
| Vanilla LTS* |  |  5.10.xx | [README](docs/README_vanilla.md) |

Note: * - Kernel builds are verified but not tested.

## Product Releases:
Please refer [Releases](https://dgpu-docs.intel.com/releases/index.html)
