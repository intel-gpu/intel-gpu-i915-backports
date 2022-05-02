#  Intel® Graphics Driver Backports for Linux® OS (intel-gpu-i915-backports)

This branch provides i915 driver source code backported for SUSE® Linux® Enterprise Server to enable intel discrete GPUs support.

We are using [backport project](https://backports.wiki.kernel.org/index.php/Main_Page) to generate out of tree i915 kernel module source codes.
 
# Branches
 suse/main will point to the currently supported version of SUSE® Linux® Enterprise Server.
 
 We will add a new branch suse/sles<x.y> whenever a version is deprecated or moved to the maintenance phase.
  
# Supported Version/kernel
  our current backport is based on **SUSE® Linux® Enterprise Server 15SP3**. We are using the header of the latest available kernel at the time of backporting. However, it may not be compatible with the latest version at the time of installation.
  Please refer [Version](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/suse/main/versions)
  file to check the BASE_KERNEL_NAME. It will point to the kernel version which is being used during backporting.

# Dynamic Kernel Module Support(DKMS) based package creation

We need to create dmabuf and i915 dkms packages using the below command.

    cp defconfigs/dmabuf .config && make dmadkmsrpm-pkg
    cp defconfigs/i915 .config && make i915dkmsrpm-pkg

# Installation
    sudo rpm -ivh intel-dmabuf-dkms-0.5606.220413.1.4.18.0.348.7.1-el8_5.x86_64.rpm
    sudo rpm -ivh intel-i915-dkms-0.5606.220413.1.4.18.0.348.7.1-el8_5.x86_64.rpm
  
 ## Requisite
I915 dkms module has dependency on [firmware](tbd)  and [intel-gpu-mei-backports](https://github.com/intel-gpu/intel-gpu-mei-backports).

Please follow the documentation repo for details of all dependencies.

