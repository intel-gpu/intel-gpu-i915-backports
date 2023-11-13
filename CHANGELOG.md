# ChangeLog
## Release RHEL88_23WW43.5_736.25_23.8.20_230810.24
* i915: Fix for deadlock in vm_unbind
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

## Release RHEL88_23WW37.5_704.30_23.7.17_230608.20
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

## Release RHEL88_23WW35.5_682.20_23.6.28_230425.38
* i915: fix to prevent invalid eu stall data after RC6 on max
* i915: fix for spechpc hang seen on max
* i915: fix for ze_peak and ze_peek hangs seen on max
* i915: fix for memory leak seen on flex
* i915: fix to properly clean up exceptions after misbehaving application seen on max
* i915: fix to avoid workloads visible after host process has ended
* i915: fix for race condition related to decoupling gt parking and vma close
* i915: fix with eudebug to reset gt when application is terminated at breakpoint

## Release RHEL88_23WW31.5_682.14_23.6.24_230425.33
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

## Release RHEL88_23WW28.5_647.21_23.5.19_230406.22
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

## Release RHEL88_23WW25.5_647.8_23.5.15_230406.17
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

## Release RHEL87_23WW21.5_627.7_23.4.15_PSB_230307.15
* i915: fix for reported list corruption on flex and ai issue seen on max
* i915: fix for LMEM->SMEM migration
* i915: fix for unexpected page faults on buffer object migration with vm_prefetch
* i915: fix for eudebug to avoid closed clients on vm search
* i915: fix corrupted state in eudebug error handling

## Release RHEL86_23WW14.5_602_23.3.19_PSB_230122.14
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
