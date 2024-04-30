# ChangeLog
## Release I915_24WW17.5_803.45_23.10.43_231129.43
* i915: Update to load and use HuC fw v7.10.15 on flex
* i915: Fix for rcs hang occasionally seen during memory migration with virtualization on flex
* i915: Fix to use compact PT for flat-CCS updates on flex
* i915: Fix to flush after posting PTE updates for blitter swap
* i915: Update to disable preemption of kernel blitter contexts
* i915: Fix for error unwind of ppGTT inserts after allocation failure
* i915: Fix for thread safety during pt allocation
* i915: Fix ring head update race condition after reset
* i915: Fix use after free during object clear
* i915: Update to add dummy blt w/a prior to flusing blitter copies
* i915: Fix to Wa_16018031267 Wa_16018063123 implementations on max
* i915: Fix for a null pointer deref on page fault of an imported object
* i915: Fix to Wa_16018031267 Wa_16018063123 implementations on flex
* i915: Update dmesg to show pagefault address in canonical format
* i915: Fix to properly mark vma as read only when requested in VM_BIND
* i915: Fix to prevent double-free of vma->pages/obj->mm.pages during migration

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
