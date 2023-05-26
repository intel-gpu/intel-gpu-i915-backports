# ChangeLog
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
