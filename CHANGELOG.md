# ChangeLog

## Release I915_26WW25.4_1146.78_25.2.57_250224.65
* Removed fast retirement of RPS boosting requests to fix a performance regression caused by unintentionally setting dma-latency to 0,
  which forced all CPU cores to busy-spin on idle and diverted power and thermal budget from application workloads.
* Rescheduled TD_ATT scanning until after pagefault handling clears the in-pagefault state to avoid leaving attention bits
  uncleared and contexts unterminated.
* Fixed a CONFIG_TRANSPARENT_HUGEPAGE issue by using PMD_SIZE instead of a THP.
* Incremented the EU_ATTN PMU counter for attentions raised during pagefault handling.
* Deferred global GT resets for ATT events while any engine is inside pagefault handling.
* Restored the null context guard for fences that do not have hardware contexts.
* Removed the unused dev variable from i915_sysfs_store.
* Fixed sysfs GT error attribute callback signatures to avoid store callback mismatches.
* Updated RAS HBM error information handling by expanding field sizes, improving field layout readability, and
  allowing administrators to clear the sysfs attributes.
* Reduced metadata size by stopping page-boundary padding.
* Tracked the originating engine for each VMA in multi-process captures so captured data is associated with the correct engine.
* Canceled pending vm_bind work during closure to avoid indefinite stalls when debugging blocks the operation.
* Started releasing the vm->client reference immediately after its final use for the debugger destroy event.
* Prevented new VMAs from being inserted into a closed ppGTT.
* Fixed a deadlock race between VM_UNBIND and eviction by reordering the locking.
* Started keeping the VM open while performing VM_BIND to avoid binds racing with VM destruction.
* Started waiting for asynchronous vm_bind completion before forcing VM closure.
* Added an early check for unevictable resident objects to avoid waiting on objects that cannot be evicted.
* Prevented execution from reviving a VM that is already closing.
* Moved VM reference ownership to vm->open so open VMs retain the required reference.
* Propagated context construction parameters to the backend so settings such as context isolation are preserved.
* Reworked global reprioritization to process dependent requests in topological order and avoid submission-order inversions.
* Fixed the execbuf unwind path to avoid leaking clients and freeing active contexts on transient errors.
* Fixed CPU page-table leaks caused by replacing split PMDs with huge PTE leaves.
* Filtered coredump active requests by using CURRENT_LRCA so pagefault captures identify the correct active context.
* Added backport support for kernel version 6.18.

## Release I915_26WW11.2_1146.59_25.2.43_250224.50
* Introduced a fast GPU recovery mechanism for Open Accelerator Module (OAM) that restores GPU functionality
  after fatal errors without requiring a full system reboot or GPU reset, significantly reducing downtime.
  To enable this feature:
  1. Remove pci=noaer from the kernel command line.
  2. Add pcie_ports=native and i915.enable_fatal_error_recovery=1 to the kernel command line.
  3. After rebooting, verify the configuration by running cat /proc/cmdline to confirm the kernel parameters
     and cat /sys/kernel/debug/dri/*/i915_params/enable_fatal_error_recovery to ensure it returns 1.
* Added backport support for kernel version 6.15 and 6.17.
* Fixed VM_BIND wakeup to use full memory barrier preventing missed wakeups caused by local read reordering.
* Fixed error state compression to always provide output buffer when flushing stream.
* Fixed scatterlist marker restoration to prevent reading past end of list.
* Added the local KOBJ_ATTR_RO() definition to enable compilation on DKMS targets.
* Converted MFDI eye margin sysfs to use DEV_ATTR for better DKMS kernel integration.
* Fixed ifdef mismatch for remap_sg() to ensure struct members are defined when used.
* Fixed PMU events decoupling on device release to prevent use after free.
* Fixed vma_set_flags() to hold required semaphore.
* Added sysfs entries to track MDFI eye margin errors (mdfi_eye_margin_error and mdfi_eye_margin_status).
* Fixed IAF error scanning to skip uninitialized subdevices.
* Added monitoring for IAF bridge and viral error status registers with logging to dmesg and uevent.
* Fixed VM_BIND object lifetime to prevent use after free during asynchronous operations.
* Added verification that GuC successfully copied hwconfig data.
* Added hwconfig storage initialization to prevent exposing stale kernel data to userspace.
* Added backport support for kernel version 6.16.
* Fixed BLT operations on Alchemist GPUs to use full 64KiB pages.
* Added timer to clean dirty cache after device idles.
* Fixed potential deadlock by removing object flushing from GT parking.
* Added px-cache shrinking on GT parking to prevent local memory exhaustion and fragmentation.

## Release I915_25WW50.4_1146.40_25.2.29_250224.35
* Resolved an issue causing Intel Data Center GPU Max Series to hang under certain conditions.

## Release I915_25WW36.5_1146.31_25.2.25_250224.31
* Introduced an experimental recovery mechanism for handling fatal GPU errors, designed to restore
  functionality without requiring a full system reboot or GPU reset. This helps reduce downtime and improves
  system reliability. This feature is disabled by default and can be enabled using the enable_fatal_error_recovery
  flag: a value of 1 routes fatal errors as Message Signaled Interrupts (MSI) and attempts a Secondary Bus Reset
  (SBR), while a value of 2 routes errors as MSI without attempting an SBR.
* Enabled the dynamic ICS via the opt-in KLV feature.
* Updated the Graphics Micro Controller (GuC) to version 70.44.1.
* Extended 2M userptr support to 1G.
* Enabled backport support for kernel version 6.13.
* Added support for the HBM_REPLACE bit to signal High Bandwidth Memory (HBM) health status and its transition
  to the REPLACE state. This enhancement enables the driver to detect the bit and prevent loading when the state
  changes to REPLACE, while also reporting the issue and prompting HBM replacement.
* Enabled group busyness counters in a VF.
* Supported dumping multiple engines for offline debugging.
* Supported 4K pages in lmem swapper.
* Enhanced HBM training failure reporting.
* Added extra debug info for GuC CT errors.
* Added PCI ID for new PVC vector-only SKU.
* Added write barriers between flat-ppgtt init and usage.
* Showed multiCCS status in sysrq-G.
* Showed pagefault address in canonical format.
* Added jiffies for missing age parameter.
* Tuned active defrag and idle buddy allocation.
* Supported marking VM_BIND vmas as read-only.
* Updated DG2 HuC to version 7.10.14.
* Added the survivability lite feature for firmware updates on Flex.
* Enhanced the offline installer for RHEL by including pre-built kernel modules for the Intel i915 graphics driver.
  These modules simplify installation and management of the driver. For further details, see the documentation
  provided with the offline installer. To access this documentation, run the offline installer with the -s parameter.

## Release I915_25WW30.4_1146.12_25.2.16_250224.22
* Improved GPU error reporting by including UUID resources for better diagnostics.
* Enhanced responsiveness during memory management tasks.
* Refined tbb thread handling to improve scheduling efficiency, avoid redundant parking during cancellations,
  and ensure proper wake-up behavior.

## Release I915_25WW27.3_1146.10_25.2.13_250224.19
* Fixed an issue where the sched_setattr_nocheck API was not exported in kernel versions earlier than 5.14.
* Switched to locked variant of wake_up_interruptible for safer thread wake-ups.
* Reduced spurious wake-ups for single-task shmem/userptr jobs.
* Started propagating wake-up from suspended threads to avoid delayed task execution.
* Replaced function type casting with typed function stubs.
* Added a reference around vm_bind to maintain the Virtual Memory Area’s (VMA) validity.
* Started clearing the Multi Die Fabric Interconnect (MDFI) boot time errors, as they are expected during the
  initialization of MDFI fabric and may be confused with runtime errors.
* Started using the kobject attribute instead of the device attribute for num_cslices and ccs_mode sysfs entries on
  RHEL 8.X.
* Started handling additional PCI AER corner cases to be able to reset devices without locking up the machine.
* Reordered hardware waits and GPU reset logic during PCI faults to avoid blocking on unresponsive hardware
  while recovering from a hardware failure.
* Fixed an issue where a mutex could be held indefinitely when attempting to remove an idle Virtual Memory
  Area (VMA) from the VM.
* Prevented memory allocations during page faults triggered by GPU reset.
* Updated GTT_MMAP_VERSION to align with corresponding changes in user space.
* Allowed data to be discarded on forced unbinds, avoiding swaps to inaccessible system memory.
* Set the lmem_offset to 0 after use so that the next local memory block does not carry the same offset leading to
  lost data during Single Root I/O Virtualization (SR-IOV) migrations.
* Fixed incorrect annotations.
* Fixed error unwinding in i915_virtualization_probe.
* Added periodic checks for forward progress by monitoring context switches and user interrupts. If the same
  context remains active without interrupts since the last check, a warning is  generated with no further action.
* Prevented default context creation when wedged.
* Cleaned up faulting initialization.
* Prevented DPC NPD after initialization failure by early iaf setup and driver-device decoupling on probe failure.
* Started protecting per-CPU px_cache from interrupts.
* Started sending a TLB invalidation request after each Virtual Memory Area (VMA) binding for GuC use, instead
  of deferring until before enabling GuC, to prevent Single Root I/O Virtualization (SR-IOV) failures.
* Started periodic check for mmio failures.
* Started handling CT fault injection during early initialization by ensuring CT descriptor objects are not
  dereferenced before assignment, preventing failures on early faults.
* Started checking for context creation failure during execbuf.
* Added support for deferred context attachment to existing clients.
* Removed the residual calls to the empty i915_oa_init_reg_state to completely excise an old use-after-free.
* Skipped the HuC authentication register check as it is no longer needed.
* Prevented soft lockup during defragmentation on eviction.
* Prevented a potential compute hang on Alchemist GPUs.
* Updated CT desc->head after consuming a receive chunk to prevent buffer overflow and slow GuC messaging.
* Added device PCI IDs to GPU dumps.
* Updated ce->vm on parallel child contexts.
* Corrected the CSC hardware errors.
* Added the eudbg event for deferred default context allocation.
* Removed lockdep assertions around Global Graphics Translation Table (GGTT) updates to prevent conflicts.
* Preserved Translation Lookaside Buffer (TLB) seqno when splitting clear pages into multiple smaller pages if
  there is an outstanding TLB invalidation for those pages.
* Deferred the default context allocation until first use, reducing overhead when a device opens.

## Release I915_25WW18.2_1099.17_25.1.17_250113.16
* Updated the Graphics Micro Controller (GuC) to version 70.44.1.
* Resolved a hang detection issue on Intel Data Center GPU Max Series by re-enabling GPU hang checks.
  Hang detection now only logs a warning message without terminating the application.

## Release I915_25WW12.2_1099.12_25.1.15_250113.14
* Introduced page fault handling improvements.
* Fixed an issue causing the CSC hardware errors.
* Removed unnecessary lockdep debugging checks from Global Graphics Translation Table (GGTT) updates.
* Fixed timeout issues by preserving Translation Lookaside Buffer (TLB) seqno when splitting clear pages.
* Fixed issues causing compilation errors on kernel 6.6 and later.
* Fixed an issue where prefetch was attempted on empty objects.
* Fixed an issue where pid_task() could fail if the target process had already exited.
* Implemented a workaround for Address Translation Services for Memory (ATS-M) and introduced support for
  G8 power state to reduce idle power consumption.
* Modified the logic to avoid calling pm_qos_request a second time on an existing request during breadcrumb reset.
* Disabled C-states for breadcrumb interrupts to reduce Direct Memory Access (DMA) latency.
* Cleaned up incomplete shmemfs obj->base.filp on failed swapout.
* Hardcoded memory health status in sysfs to prevent breakage.
* Implemented flushing of freed objects before reporting available memory to stabilize the reported memory
  levels.
* Modified implementation to retry eviction only when it is blocked by active or locked objects, aiming to reduce
  response time.
* Optimized Virtual Memory Area (VMA) prefetch by short-circuiting redundant operations.
* Corrected Compressed Color Surface (CCS) copies for Single Root I/O Virtualization (SR-IOV) save and restore.
* Restricted shmem flags to a valid set for swapin to resolve a page fault issue.
* Modified the implementation to repeat the Translation Lookaside Buffer (TLB) flush invalidation request,
  resolving the issue with the failing Hardware Performance Library (HPL).
* Removed early unlocked unbind from object free to avoid race conditions between lockless unbinding and
  eviction of non-persistent VMAs.
* Introduced changes to protect i915_drm_client_fini from early shutdown.
* Started supporting compilation with CONFIG_PAGE_TABLE_ISOLATION to fix a compilation issue on RHEL.
* Optimized the unbind step in the GT IFR flow by skipping context runtime updates when the device is quiesced.
  This change reduces the execution time.

## Release I915_25WW06.5_1077.18_24.8.5_241129.8
* Resolved the thundering herd problem in ct_receive by waking only the specific receiving process through
  ct_request. This prevents waking unrelated processes and avoids inefficient iteration, especially during
  concurrent page faults.
* Resolved issues related to map_pages() and iotlb_sync_map() functions.
* Implemented changes to ensure that all blocking send operations are awakened and canceled if completion
  tracking (CT) fences are disabled during an ongoing send operation.
* Fixed an issue causing node hangs when applications were profiled using VTune. The issue was addressed by
  initializing chunk->policy for shmem allocations.
* Changed the intel_fbdev_restore_mode return type from void to int to meet the fbdev client registration API
  requirement introduced in kernel 6.12.
* Fixed a node reboot issue that occurred due to a general protection fault. The issue was addressed by
  protecting the acquisition of ce->timeline in signal_irq_work.
* Deferred ct_receive from the ct_send_nb path to prevent deadlock caused by calling handlers under spinlocks.
  The patch removes ct_receive from the non-blocking send path to reduce latency, allowing the caller to handle
  scheduling of ct_receive for backlog clearing.
* Enabled backport support for 6.12 kernel.

## Release I915_24WW52.1_1057.13_24.7.9_241015.10
* Implemented GPU error capture during the splitting of cleared backing stores.
* Initiated earlier shrinking of all system memory objects to mark pages as dirty and preserve their contents
  across hibernation.
* Resolved an issue where the Guided Matrix Multiplication Race Condition Sample hang instead of crashing due
  to driver errors.
* Prevented eviction of overlapping VM_BINDs to comply with ppGTT rules.
* Fixed HBM diagnostics logging on Intel Data Center GPU Max Series.
* Started tracking the duration of user stalls during TLB invalidation.
* Optimized the userptr task placement for improved task-local performance in LAMMPS benchmarks.
* Adjusted the starting point of put_page_range to align with the compound_head, ensuring correct iteration across
  adjacent compound pages.
* Fixed a performance issue in addr_range and id sysfs calls by optimizing kobject attribute handling on RHEL8.x
  systems.
* Improved handling of engine reset failures to prevent deadlocks during GT resets and G2H notifications.
* Started storing the current UID for core dumps and display active clients in sysrq-G.
* Started including the page fault address for CAT errors.
* Restored RPM ownership to core to fix device runtime-pm drop in PSB builds.
* Fixed issues causing performance drops.
* Improved error resilience for iommu mappings.
* Ensured ENABLE_PG is cleared on unbind/unload to prevent reset failures in power-saving mode.
* Fixed an issue involving the uninitialized use of the domain variable.

## Release I915_24WW44.4_1032.21_24.6.12_240823.13
* Disabled per-CPU page table allocations, enforcing allocation on each operation as a baseline.
* Fixed an issue where HPL failed on Intel® Data Center GPU Max Series.
* Added additional rcu_barrier on cache release to ensure the objects are freed before completing the module unload.
* Fixed a Multi Die Fabric Interconnect (MDFI) training issue that occurred during reboot.
* Improved error reporting on Intel® Data Center GPU Max Series.
* Fixed an off-by-one error that left the last entry uninitialized after a get_user_pages failure.
* Improved error handling for clearing shared memory pages.
* Introduced support for SR-IOV (Single Root I/O Virtualization) save and restore virtual functions.
* Enabled render power gate when RC6 is disabled on Intel® Data Center GPU Max Series.

## Release I915_24WW42.2_996.26_24.5.15_240718.18
* Fixed an issue where the time spent on PCIe data transferring between CPU and GPU was significantly longer than expected.

* Fixed an issue where page fault was not reported properly to GNU Debugger (GDB) when many hardware threads were running.

* Fixed an issue that prevented GPU metrics from being queried successfully.

* Added changes to incrementally allocate SG tables to avoid over-allocating memory.

* Fixed race between object free and eviction.

* Fixed an issue that caused the i915 load module to fail and result in kernel taint.

* Fixed an issue causing transient eviction failures.

* Fixed an issue where GPU state did not show the correct backport information.

* Fixed an issue causing inherited error state from the previous blitter offload.

* Fixed an issue causing kernel warnings when processing lengthy G2H message queues.

* Fixed an issue causing corruption of scatter lists on blit error handling.

* Fixed an issue causing HPL residual check failures on Intel® Data Center GPU Max Series.

* Removed disabled context from scheduling.

## Release I915_24WW33.3_950.13_24.4.12_240603.18
* i915: Introduced telemetry register updates and additional debugging information for High Bandwidth Memory (HBM)
        diagnostics performed during system reset.
* i915: Fixed an issue with High Performance Linpack (HPL) residual check failures on Intel® Data Center GPU Max 1100.
* i915: Fixed synchronization issues for user space signaling of vm_bind on multi-GPU devices.
* i915: Fixed an issue that caused pausing virtual machines when reading Execution Unit (EU) metrics on host by the
        Level Zero Metrics API.
* i915: Fixed an issue where a Graphics Microcontroller Unit (GuC) communication error occurred while using the
        Graphics Debugger (GDB), causing Intel® Data Center GPU Max Series to become non-functional.
* i915: Fixed an issue where rebooting a device caused an error in the GPU’s Memory-Mapped Input/Output (MMIO)
        system mentioning that the forcewake register was not behaving as expected.
* i915: Fixed an issue in the Intel® XPU System Management Interface where the GPU power limit was incorrectly set.
        This fix deprecates the power1_rated_max hardware monitoring sysfs attribute on Intel® Data Center GPU Flex Series.
* i915: Implemented changes to ensure the graphics technology remains active when tasks are being queued.
* i915: Fixed the Logical Ring Context (LRC) page size to align with the Graphics Microcontroller Unit (GuC) context
        state size.
* i915: Fixed a Graphics Microcontroller Unit (GuC) context null error that occurred on Intel® Data Center GPU Max
        Series after reset.
* i915: Introduced a fix to block driver load when Memory Mapped Input/Output (MMIO) communication fails.
* i915: Implemented a fix to prevent inherited error states from affecting the new blitter offload.

## Release I915_24WW28.5_914.32_24.3.23_240419.26
* i915: Updated the telemetry register and High Bandwidth Memory (HBM) error reporting.
* i915: Improved formatting of logging certain hardware error messages.
* i915: Fixed the Logical Ring Context (LRC) page size to match Graphics Microcontroller Unit (GUC) golden context
        state size.
* i915: Fixed concurrent unbinds preventing Virtual Memory Area (VMA) bind.
* i915: Minimized page fault reporting for closed contexts during process exit.
* i915: Introduced a fix to avoid invalid data during sysfs files show and restore callbacks.
* i915: Introduced a modification that queues multiple page faults events.
* i915: Fixed the way memory controller registers are checked because of an issue related to an open file descriptor for
        the Performance Monitoring Unit (PMU) during a Function Level Reset (FLR) operation.
* i915: Extended timeouts to capture slower SIP resolves.
* i915: Fixed match faulting virtual machine for request and context lookup.
* i915: Introduced a fix to avoid hang seen in page fault handling.
* i915: Introduced a fix to avoid CPU cache impact when clearing system pages for potential improved performance.
* i915: Introduced a fix to offload blitter clears for system memory for potential improved performance.
* i915: Fixed potential memory leak in the process of swapping between different GPU objects.
* i915: Introduced a fix to ensure CPUs are available for the i915 threads with mismatching nohz_full and numa
        configs.
* i915: Introduced a fix to avoid scheduling i915 work on nohz_full CPU cores.
* i915: Introduced a fix to load and use Graphics Microcontroller Unit (GuC) firmware v70.25.0.
* i915: Fixed incorrect bit usage in eviction processing.
* i915: Introduced a fix to avoid Memory Mapped Input/Output (MMIO) operations before memory is initialized.
* i915: Introduced a fix to ensure proper Page Attribute Table (PAT) settings after migration.
* i915: Fixed page fault on blitter operations.
* i915: Fixed race condition on local memory initialization.
* i915: Fixed deadlock on blitter command stream 0 pagefault.
* i915: Introduced a fix to load and use hardware control firmware v7.10.16 on Flex.
* i915: Disabled clflush bypass under virtual machine environments.
* i915: Fixed Page Size 64KB (PS64) alignment on local memory and shared memory (lmem<->smem) copies.
* i915: Fixed a locking issue during Graphics Execution Manager (GEM) shrink.
* i915: Disabled move_notify by default due to a reboot seen on Max.
* i915: Improved eviction processing performance.
* i915: Introduced a fix to quiesce GT traffic on shutdown.
* i915: Introduced a fix to prevent power states (C-states) while processing page faults.
* i915: Fixed returning an error from reg_read Input/Output Control (ioctl) inside VF.
* i915: Invalidated Translation Lookaside Buffer (TLB) after out of bounds user space access serviced by a scratch page.

## Release I915_24WW23.5_881.19_24.2.17_240301.20
* i915: update to reduce pagefault reporting for closed context during process exit
* i915: update to extend waits to capture for slower SIP resolves
* i915: fix match faulting vm for request/context lookup
* i915: fix to avoid invalid data during sysfs files show and restore callbacks
* i915: fix to invalidate TLB after out of bounds userspace access serviced by a scratch page
* i915: Update to load and use HuC fw v7.10.16 on Flex
* i915: fix to disable move_notify by default as cause of reboot seen on max
* i915: update to quiesce gt traffic on shutdown
* i915: update to load and use HuC fw v7.10.15 on flex
* i915: fix for rcs hang occasionally seen during memory migration with virtualization on flex
* i915: fix to use compact PT for flat-CCS updates on flex
* i915: fix to flush after posting PTE updates for blitter swap
* i915: update to disable preemption of kernel blitter contexts
* i915: fix for error unwind of ppGTT inserts after allocation failure
* i915: fix for thread safety during pt allocation
* i915: fix ring head update race condition after reset
* i915: fix use after free during object clear
* i915: update to add dummy blt w/a prior to flusing blitter copies
* i915: Update to use GuC fw v70.22.0 on flex while remaining on v70.19.2 for max
* i915: update to load and user GuC fw v70.20.2
* i915: add survivability lite feature for fw updates on flex
* i915: fix to Wa_16018031267 Wa_16018063123 implementations on max
* i915: fix for a null pointer deref on page fault of an imported object
* i915: update to allow pcie recovery flow in pf when sriov vf are enabled
* i915: update vm unbind tlb update to be synchronous

## Release I915_24WW12.5_821.30_24.1.11_240117.14
* i915: fix to prevent double-free of vma->pages/obj->mm.pages during migration
* i915: update to retry guc during pagefault processing to avoid -EIO warnings under load
* i915: update to recover from errors during pagefault by banning context and resetting gt
* i915: update to load and use as default HuC fw v7.10.14 on flex
* i915: update to not use nonblocking ct sends for secondary fault replies as impacted quicksilver benchmarks
* i915: update timeslice back to 5ms for performance on lammps wl
* i915: update to harden eviction blt loops limits
* i915: update for opportunistic migration back to lmem after eviction
* i915: add Wa_16018031267 and Wa_16018063123 for flex and max
* i915: fix to improve responsiveness of memory eviction
* i915: fix to avoid warning race during dma_fence error propagation
* i915: fix to avoid deadlock during wait-for-bind before eviction
* i915: fix to avoid cat error due to off by one memory address
* i915: fix to correct packet length for send_fault_reply
* i915: fix to correctly steer eustall register access
* i915: fix to wa to correctly steer GuC writes after engine reset
* i915: fix for gt wakeref handling
* i915: fix for off by one in memory error handling
* i915: Fix for issue seen during an L0 compliance test
* i915: fix from static analysis in memory free path
* i915: fix for potential race condition found through static analysis

## Release I915_24WW05.5_803.29_23.10.32_231129.32
* i915: Add sriov force fixed CCS-1 mode for max
* i915: Add support to dump multiple engines for offline debugging
* i915: Add info dmesg to describe if eudebug enabled
* i915: Add additional capability related to tracking engine busyness
* i915: update for opportunistic migration back to lmem after eviction
* i915: fix to improve responsiveness of memory eviction
* i915: update timeslice back to 5ms for performance on lammps wl
* i915: add Wa_16018031267 and Wa_16018063123 for flex and max
* i915: fix to avoid warning race during dma_fence error propagation
* i915: fix to avoid deadlock during wait-for-bind before eviction
* i915: fix to correct packet length for send_fault_reply
* i915: fix to avoid cat error due to off by one memory address
* i915: fix to wa to correctly steer GuC writes after engine reset
* i915: fix for gt wakeref handling
* i915: fix for off by one in memory error handling
* i915: fix to ensure pte writes from gpu are fully written
* i915: Fix for issue seen during an L0 compliance test
* i915: Add 1550vg PCIe Device ID for max
* i915: add enable group busyness counters in a vf
* i915: update to support 4K pages in lmem swapper
* i915: fix for use after free during cleanup of pinned contexts
* i915: fix for off by one error when clearing ppgtt error
* i915: update to stop driver load and report if HBM training failures reported by fsp firmware
* i915: fix to avoid handling ras errors that were already handled
* i915: update the timeout for recording default contexts
* i915: add sriov force fixed CCS-1 mode for max
* i915: fix pf-coredump vma capturing w/o debugger
* i915: add support to dump multiple engines for offline debugging
* i915: fix eudebug to ensure pagefault dumping is async
* i915: fix for eudebug to clean up on last vm destroy
* i915: add info dmesg to describe if eudebug enabled
* i915: update to enable softpg for improved small blitter latency on max
* i915: fix invalid ref after request retired
* i915: add additional capability related to tracking engine busyness
* i915: fix to avoid evicting pagetables during suspend/resume
* i915: fix for suspend/resume restoration to ensure correctness of completion status
* i915: fix for eudebug race of client's PID when looking for debugger
* i915: fix for debugger error message on vma evict
* i915: fix for divide by zero error in OA
* i915: fix to properly clean up vm reference on early process termination
* i915: update to print error msg on unclean debugger shutdown
* i915: fix race in debugger destruction path
* i915: update to allow user to specify mempolicy in numa configurations

## Release I915_23WW49.5_775.20_23.9.11_231003.15
* i915: add sriov force fixed CCS-1 mode for max
* i915: update to allow user to specify mempolicy in numa configurations
* i915: fix to ensure dmabuf read targets are valid
* i915: add new device id for flex
* i915: add new PCI ID for max
* i915: fix for use after free in coredump buffer capture
* i915: update to enhance GT FATAL error log for max
* i915: update for ocl walker latence performance improvement on flex
* i915: update to allow partial mmaps
* i915: fix for race in guc context enable/disable
* i915: fix for race during tlb waits on resets
* i915: update to balance cpu usage vs wake latency for performance seen on flex
* i915: update to gracefully handle case of numa node with no memory installed
* i915: fix for sriov to avoid double blocking in gem mmap ioctl
* i915: fix for deadlock in vm_unbind
* i915: deprecate broken DRM_I915_REQUEST_TIMEOUT config option so does not accidentally get invoked on certain compiled OSV kernels
* i915: update for improvement in execution latency
* i915: add kmd-umd interface for VM_SET_ATOMIC APIs
* i915: fix for out of bounds array access
* i915: fix for sriov to avoid SYNCOBJ IOCTLs from blocking during migration
* i915: add wa_18028616096 for flex
* i915: fix to avoid falsely reported gpu hang errors
* i915: for for eudebug finalize error seen during L0 conformance tests
* i915: fix to avoid null pointer dereference in numa configurations
* i915: update to optimize eviction swapping algorithm performance
* i915: fix to consider numa domain placement in get_pages for improved performance seen on max
* i915: add sriov user space blocking functionality for vf
* i915: update to propagate CAT error notification to userspace
* i915: fix to address gpu hang on suspend/resume seen on flex
* i915: add wa_14015150844 for flex
* i915: fix serialise blocking CT sends to avoid exponential backoff
* i915: ingore. Later reverted. update to load and use guc fw v70.11.0
* i915: fix for eudebug active request breakpoint detection affecting multi-kernel program
* i915: fix to avoid warning seen during suspend on flex

## Release I915_23WW43.5_736.25_23.8.20_230810.22
* i915: Fix for eudebug finalize error seen during L0 conformance tests
* i915: Fix serialise blocking CT sends to avoid exponential backoff
* i915: Fix for eudebug active request breakpoint detection affecting multi-kernel program
* i915: Fix to avoid warning seen during suspend on flex
* i915: Add support for online eudebug support on page fault
* i915: Update to iaf driver to reduce redundant and overly verbose diagnostic messaging on max
* i915: Update to enhance mdfi error severity reporting
* i915: Fix for eudebug to avoid discovering the same vm twice
* i915: Fix performance drop seen with pt-cosmic tagger on max
* i915: Add sysfs entry for exposing thermal swing throttling reason on max
* i915: Fix for memory leak seen with eudebug on max
* i915: Fix for deadlock between vm_unbind and memory eviction
* i915: Fix for performance regression seen during memory swapping on max
* i915: Fix to restore wa after eudebugger use leading to gpu hang on flex
* i915: Fix to avoid stray mei warning
* i915: Update to load and use GuC fw v70.9.1
* i915: Update behavior of PRELIM_I915_GEM_VM_BIND_MAKE_RESIDENT while VM_BIND is held by user
* i915: Fix for timeout on suspend seen on flex
* i915: Fix for circular locking in gem object unbind
* i915: Fix infinite loop for atomic_system object imported via dma buf
* i915: Fix for crash when allocating huge memory seen on max
* i915: Fix to avoid deadlock in page fault handling
* i915: Update to load and use guc fw v70.9.0
* i915: Fix to prevent invalid eu stall data after RC6 on max
* i915: Fix to avoid a deadlock seen with eu debug
* i915: Update to enable async vm_unbind for pytorch performance and eu debug
* i915: Fix for eu debug corrupted page fault error capture file
* i915: Fix to avoid writing userfence for an aborted vm_bind
* i915: Fix for spechpc hang seen on max
* i915: Fix to avoid deadlock in page fault handling
* i915: Update to load and use GuC fw v70.9.1
* i915: Update to load and use guc fw v70.9.0
* i915: Fix for performance regression seen during memory swapping on max
* i915: Update to load and use GuC fw v70.9.1
* i915: Fix for memory leak seen with eudebug on max
* i915: Fix to restore wa after eudebugger use leading to gpu hang on flex
* i915: Fix for eu debug corrupted page fault error capture file
* i915: Fix to prevent invalid eu stall data after RC6 on max
* i915: Fix to avoid a deadlock seen with eu debug

## Release I915_23WW37.5_704.30_23.7.17_230608.25
* i915: Update to load and use GuC fw v70.9.1
* i915: fix for memory leak seen with eudebug on max
* i915: fix to restore wa after eudebugger use leading to gpu hang on flex
* i915: fix for spechpc hang seen on max
* i915: fix for ze_peak and ze_peek hangs seen on max
* i915: fix to avoid workloads visible after host process has ended
* i915: fix for memory leak seen on flex
* i915: fix to properly clean up exceptions after misbehaving application seen on max
* i915: add support to report reset_count and report low level driver error counters via sysfs on max
* i915: fix for race condition related to decoupling gt parking and vma close
* i915: fix with eudebug to reset gt when application is terminated at breakpoint
* i915: updates to improve GuC error reporting
* i915: fix parameters for hbm error logging on max
* i915: fix to avoid incorrect reporting of max errors on flex
* i915: fix to avoid spurious error message on iaf startup on max
* i915: update to enhance RAS error logging on max
* i915: fix to restore default hardware power gating behaviour for performance
* i915: fix to maintain lmem accounting across migration
* i915: fix for IAF page fault seen during module load/unload on max
* i915: update for mei outside of i915 to enable async suspend for devices on mei bus
* i915: fix for exec buffer lock warn on max
* i915: update to minimize dma wakeup latency seen on flex
* i915: update to have more migration performed in the background to improve performance
* i915: fix for eudebug to purge deferred vm-bind on unbind
* i915: fix to align with hw's minimum invalidation page size requirement

## Release I915_23WW35.5_682.20_23.6.28_230425.37
* i915: fix to prevent invalid eu stall data after RC6 on max
* i915: fix for spechpc hang seen on max
* i915: fix for ze_peak and ze_peek hangs seen on max
* i915: fix for memory leak seen on flex
* i915: fix to properly clean up exceptions after misbehaving application seen on max
* i915: fix to avoid workloads visible after host process has ended
* i915: fix for race condition related to decoupling gt parking and vma close
* i915: fix with eudebug to reset gt when application is terminated at breakpoint

## Release I915_23WW31.5_682.14_23.6.24_230425.29
* i915: updates to improve GuC error reporting
* i915: fix to avoid incorrect reporting of max errors on flex
* i915: update to enhance RAS error logging on max
* i915: fix parameters for hbm error logging on max
* i915: fix to avoid spurious error message on iaf startup on max
* i915: fix to maintain lmem accounting across migration
* i915: fix for exec buffer lock warn on max
* i915: fix to align with hw's minimum invalidation page size requirement
* i915: fix for IAF page fault seen during module load/unload on max
* i915: fix for eudebug to purge deferred vm-bind on unbind
* i915: fix for race condition related to decoupling gt parking and vma close
* i915: update to load and use guc fw v70.7.0
* i915: fix to avoid deadlock with concurrent eviction
* i915: fix for race between eviction and revalidation
* i915: fix for handling error during construction of vma bind work
* i915: fix to prevent deadlock due to reuse of fences

## Release I915_23WW28.5_647.21_23.5.19_230406.21
* i915: fix for 6942. fix for eudebug to ensure device is awake when connecting new debugger
* i915: fix to ensure debugger checkpoints are restored following engine reset
* i915: update anr firmware for xelink on max
* i915: fix for race condition in mid-batch preemption
* i915: fix for memory leak when doing coredump memory capture
* i915: update in mei outside of i915 to suppress warn message only in specific scenarios
* i915: update to queue retry eviction after a failure
* i915: fix to prevent eviction starvation with i915_gem_set_domain_ioctl
* i915: update to queue retry eviction after a failure
* i915: update to improve performance stability on ocl benchmark
* i915: update to latest stable iaf driver version for max

## Release I915_23WW25.5_647.8_23.5.15_230406.17
* i915: add support for SOC NONFATAL error handling
* i915: add enhanced HBM error reporting for max
* i915: fix in handling of soc global error processing seen on max
* i915: fix for null pointer dereference in ubuntu kmd-backport on flex
* i915: add wa for random gpu hang when eviction happens under high memory pressure seen on flex
* i915: fix for deadlock in lmem_invalidation
* i915: add initial mmap support for pci memory via mmap
* i915: update to queue retry eviction after a failure
* i915: fix to prevent eviction starvation with i915_gem_set_domain_ioctl
* i915: update to queue retry eviction after a failure
* i915: update to improve performance stability on ocl benchmark
* i915: update to latest stable iaf driver version for max
* i915: add support for SOC NONFATAL error handling
* i915: add enhanced HBM error reporting for max
* i915: fix in handling of soc global error processing seen on max
* i915: update to retry eviction during clear-on-idle
* i915: fix to avoid cat errors during page table clears seen on flex
* i915: fix block pfn calculation for compute testing of system allocator

## Release I915_23WW21.5_627.7_23.4.15_PSB_230307.15
* i915: fix for reported list corruption on flex and ai issue seen on max
* i915: fix for LMEM->SMEM migration
* i915: fix for unexpected page faults on buffer object migration with vm_prefetch
* i915: fix for eudebug to avoid closed clients on vm search
* i915: fix corrupted state in eudebug error handling

## Release I915_23WW14.5_602_23.3.19_PSB_230122.18
* i915: update to load and use guc fw version 70.6.4
* i915: fix to adjust reserved blt priority to avoid deadlock
* i915: fix to allow evictions when over-commit enabled (is by default)
* i915: add over commit limit during gem_create and sysfs interfaces
* i915: add new pvc PCIe IDs
* i915: fix for setting pl1 power limit on flex
* i915: fix for card warm reset after dpc event seen on max
* i915: add selftest that can touch all lmem (customer requested for max)
* i915: update queue error event handling for eu debug
* i915: fix to avoid spurious warning in eudebug
* i915: fix issue in handling eudebug specific work arounds
* i915: update to report bw usage by blitter for memory clears
* i915: update to bump gpu clocks whenever stall for gpu clears
* i915: update for performance improvement during tlb invalidate
* i915: fix to avoid waking hw in suspend path
* i915: update to enhance fatal sco error logging
* i915: add reporting of correctable and non-correctable errors
* i915: fix for cat error seen with first touch policy in level zero conformance test on max
* i915: fix null pointer deref affecting sriov in driver_flr
* i915: fix bug on and locking around page faults seen on max
* i915: add support for gsc/csc hw error handling on max
* i915: fix missing wakeref in gt suspend
* i915: add reporting of correctable and non-correctable errors
* i915: fix for cat error seen with first touch policy in level zero conformance test on max
* i915: add wa to avoid render corruption on flex
* i915: fix bug on and locking around page faults seen on max
* i915: add support for gsc/csc hw error handling on max
* i915: fix missing wakeref in gt suspend
* i915: fix for eu debug use after free
* i915: fix for eu debug sleeping with lock held
* i915: update to not leave pages pinned on device page fault
* i915: fix for potential use after free
