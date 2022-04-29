#  Intel® Graphics Driver Backports for Linux® OS (intel-gpu-i915-backports)

This branch provides i915 driver source code backported for Ubuntu® to enable intel discrete GPUs support.

We are using [backport project](https://backports.wiki.kernel.org/index.php/Main_Page) to generate out of tree i915 kernel module source codes.
 
# Branches
 ubuntu/main will point to the currently supported version of Ubuntu®.
 
 We will add a new branch ubuntu/ubuntu<x.y> whenever a version is deprecated or moved to the maintenance phase.
  
# Supported Version/kernel
  our current backport is based on **Ubuntu® 20.04 (linux-oem kernel 5.14)**. We are using the header of the latest available kernel at the time of backporting. However, it may not be compatible with the latest version at the time of installation.
  Please refer [Version](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/ubuntu/main/versions)
  file to check the BASE_KERNEL_NAME. It will point to the kernel version which is being used during backporting.

# Dynamic Kernel Module Support(DKMS) based package creation

We need to create dmabuf and i915 dkms packages using the below command.

    dpkg-buildpackage

# Installation

     sudo dpkg -i intel-gpgpu-dkms-ubuntu-5.14-oem_5606.220414.0_all.deb
  
 ## Requisite
I915 dkms module has dependency on [firmware](tbd)  and [intel-gpu-mei-backports](https://github.com/intel-gpu/intel-gpu-mei-backports).

Please follow the documentation repo for details of all dependencies.
