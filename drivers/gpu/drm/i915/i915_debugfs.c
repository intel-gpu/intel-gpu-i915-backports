/*
 * Copyright © 2008 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *    Keith Packard <keithp@keithp.com>
 *
 */

#include <linux/sched/mm.h>
#include <linux/sort.h>
#include <linux/string_helpers.h>

#include <drm/drm_debugfs.h>

#include "gem/i915_gem_context.h"
#include "gt/intel_engine_heartbeat.h"
#include "gt/intel_engine_pm.h"
#include "gt/intel_engine_regs.h"
#include "gt/intel_gpu_commands.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_buffer_pool.h"
#include "gt/intel_gt_clock_utils.h"
#include "gt/intel_gt_debugfs.h"
#include "gt/intel_gt_pm.h"
#include "gt/intel_gt_pm_debugfs.h"
#include "gt/intel_gt_regs.h"
#include "gt/intel_gt_requests.h"
#include "gt/intel_mocs.h"
#include "gt/intel_rc6.h"
#include "gt/intel_reset.h"
#include "gt/intel_ring.h"
#include "gt/intel_rps.h"
#include "gt/intel_sseu_debugfs.h"
#include "gt/intel_tlb.h"

#include "i915_debugfs.h"
#include "i915_debugfs_params.h"
#include "i915_driver.h"
#include "i915_irq.h"
#include "i915_scheduler.h"
#include "intel_mchbar_regs.h"
#include "intel_pm.h"

static int i915_mocs_table_show(struct seq_file *m, void *data)
{
	struct drm_i915_private *i915 = m->private;
	int ret;

	ret = intel_mocs_seq_write(i915, m);
	if (ret)
		return ret;

	return 0;
}

static int i915_capabilities_show(struct seq_file *m, void *data)
{
	struct drm_i915_private *i915 = m->private;
	struct drm_printer p = drm_seq_file_printer(m);
	struct intel_gt *gt;
	unsigned int id;

	seq_printf(m, "pch: %d\n", INTEL_PCH_TYPE(i915));

	intel_device_info_print_static(INTEL_INFO(i915), &p);
	intel_device_info_print_runtime(RUNTIME_INFO(i915), &p);
	i915_print_iommu_status(i915, &p);
	for_each_gt(gt, i915, id)
		intel_gt_info_print(&gt->info, &p);
	intel_driver_caps_print(&i915->caps, &p);

	kernel_param_lock(THIS_MODULE);
	i915_params_dump(&i915->params, &p);
	kernel_param_unlock(THIS_MODULE);

	return 0;
}

static int sriov_info_show(struct seq_file *m, void *data)
{
	struct drm_i915_private *i915 = m->private;
	struct drm_printer p = drm_seq_file_printer(m);

	i915_sriov_print_info(i915, &p);

	return 0;
}

static void show_xfer(struct seq_file *m,
		      struct intel_gt *gt,
		      const char *name,
		      u64 bytes,
		      u64 time)
{
	time = intel_gt_clock_interval_to_ns(gt, time);
	if (!time)
		return;

	seq_printf(m, "GT%d %-12s: %llu MiB in %llums, %llu MiB/s\n",
		   gt->info.id, name,
		   bytes >> 20,
		   div_u64(time, NSEC_PER_MSEC),
		   div64_u64(mul_u64_u32_shr(bytes, NSEC_PER_SEC, 20), time));
}

static int i915_gem_object_info_show(struct seq_file *m, void *data)
{
	struct drm_i915_private *i915 = m->private;
	struct drm_printer p = drm_seq_file_printer(m);
	struct intel_memory_region *mr;
	enum intel_region_id id;
	struct intel_gt *gt;

	for_each_memory_region(mr, i915, id)
		intel_memory_region_print(mr, 0, &p, 0);

	for_each_gt(gt, i915, id) {
		intel_wakeref_t wf;
		u64 t;

		t = local64_read(&gt->stats.migration_stall);
		if (t >> 20)
			seq_printf(m, "GT%d migration stalls: %lldms\n",
				   id, div_u64(t, NSEC_PER_MSEC));

		if (!gt->counters.map)
			continue;

		with_intel_gt_pm(gt, wf) {
			show_xfer(m, gt, "clear-smem",
				  gt->counters.map[INTEL_GT_CLEAR_SMEM_BYTES],
				  gt->counters.map[INTEL_GT_CLEAR_SMEM_CYCLES]);
			show_xfer(m, gt, "clear-on-alloc",
				  gt->counters.map[INTEL_GT_CLEAR_ALLOC_BYTES],
				  gt->counters.map[INTEL_GT_CLEAR_ALLOC_CYCLES]);
			show_xfer(m, gt, "clear-on-free",
				  gt->counters.map[INTEL_GT_CLEAR_FREE_BYTES],
				  gt->counters.map[INTEL_GT_CLEAR_FREE_CYCLES]);
			show_xfer(m, gt, "clear-on-idle",
				  gt->counters.map[INTEL_GT_CLEAR_IDLE_BYTES],
				  gt->counters.map[INTEL_GT_CLEAR_IDLE_CYCLES]);
			show_xfer(m, gt, "swap-in",
				  gt->counters.map[INTEL_GT_SWAPIN_BYTES],
				  gt->counters.map[INTEL_GT_SWAPIN_CYCLES]);
			show_xfer(m, gt, "swap-out",
				  gt->counters.map[INTEL_GT_SWAPOUT_BYTES],
				  gt->counters.map[INTEL_GT_SWAPOUT_CYCLES]);
			show_xfer(m, gt, "copy",
				  gt->counters.map[INTEL_GT_COPY_BYTES],
				  gt->counters.map[INTEL_GT_COPY_CYCLES]);
		}
	}

	return 0;
}

static int
i915_get_mem_region_acct_limit(struct seq_file *m, void *data, u32 index)
{
	struct drm_i915_private *i915 = m->private;
	struct intel_memory_region *mr;
	int id;

	seq_printf(m, "usr_acct_limit:%u\n", i915->mm.user_acct_limit[index]);

	for_each_memory_region(mr, i915, id) {
		if (mr->type != INTEL_MEMORY_LOCAL)
			continue;

		seq_printf(m, "%s: available:%pa bytes\n",
			   mr->name, &mr->acct_limit[index]);
	}

	return 0;
}

static int lmem_alloc_limit_info_show(struct seq_file *m, void *data)
{
	return i915_get_mem_region_acct_limit(m,  data,
					      INTEL_MEMORY_OVERCOMMIT_LMEM);
}

static int sharedmem_alloc_limit_info_show(struct seq_file *m, void *data)
{
	return i915_get_mem_region_acct_limit(m,  data,
					      INTEL_MEMORY_OVERCOMMIT_SHARED);
}

#if IS_ENABLED(CPTCFG_DRM_I915_CAPTURE_ERROR)
static ssize_t gpu_state_read(struct file *file, char __user *ubuf,
			      size_t count, loff_t *pos)
{
	struct i915_gpu_coredump *error;
	ssize_t ret;
	void *buf;

	error = file->private_data;
	if (!error)
		return 0;

	/* Bounce buffer required because of kernfs __user API convenience. */
	buf = kmalloc(count, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = i915_gpu_coredump_copy_to_buffer(error, buf, *pos, count);
	if (ret <= 0)
		goto out;

	if (!copy_to_user(ubuf, buf, ret))
		*pos += ret;
	else
		ret = -EFAULT;

out:
	kfree(buf);
	return ret;
}

static int gpu_state_release(struct inode *inode, struct file *file)
{
	i915_gpu_coredump_put(file->private_data);
	return 0;
}

static ssize_t
i915_error_state_write(struct file *filp,
		       const char __user *ubuf,
		       size_t cnt,
		       loff_t *ppos)
{
	struct i915_gpu_coredump *error = filp->private_data;

	if (!error)
		return 0;

	drm_dbg(&error->i915->drm, "Resetting error state\n");
	i915_reset_error_state(error->i915);

	return cnt;
}

static int i915_error_state_open(struct inode *inode, struct file *file)
{
	struct i915_gpu_coredump *error;

	error = i915_first_error_state(inode->i_private);
	if (IS_ERR(error))
		return PTR_ERR(error);

	file->private_data  = error;
	return 0;
}

DEFINE_I915_RAW_ATTRIBUTE(i915_error_state_fops, i915_error_state_open,
			  gpu_state_release, gpu_state_read,
			  i915_error_state_write, default_llseek);
#endif

static int i915_frequency_info_show(struct seq_file *m, void *unused)
{
	struct drm_i915_private *i915 = m->private;
	struct intel_gt *gt = to_gt(i915);
	struct drm_printer p = drm_seq_file_printer(m);

	intel_gt_pm_frequency_dump(gt, &p);

	return 0;
}

static int i915_rps_boost_info_show(struct seq_file *m, void *data)
{
	struct drm_i915_private *dev_priv = m->private;
	struct intel_rps *rps = &to_gt(dev_priv)->rps;

	seq_printf(m, "RPS enabled? %s\n", str_yes_no(intel_rps_is_enabled(rps)));
	seq_printf(m, "RPS active? %s\n", str_yes_no(intel_rps_is_active(rps)));
	seq_printf(m, "GPU busy? %s\n", str_yes_no(intel_gt_pm_is_awake(to_gt(dev_priv))));
	seq_printf(m, "Boosts outstanding? %d\n",
		   atomic_read(&rps->num_waiters));
	seq_printf(m, "Interactive? %d\n", READ_ONCE(rps->power.interactive));
	seq_printf(m, "Frequency requested %d, actual %d\n",
		   intel_gpu_freq(rps, rps->cur_freq),
		   intel_rps_read_actual_frequency(rps));
	seq_printf(m, "  min hard:%d, soft:%d; max soft:%d, hard:%d\n",
		   intel_gpu_freq(rps, rps->min_freq),
		   intel_gpu_freq(rps, rps->min_freq_softlimit),
		   intel_gpu_freq(rps, rps->max_freq_softlimit),
		   intel_gpu_freq(rps, rps->max_freq));
	seq_printf(m, "  idle:%d, efficient:%d, boost:%d\n",
		   intel_gpu_freq(rps, rps->idle_freq),
		   intel_gpu_freq(rps, rps->efficient_freq),
		   intel_gpu_freq(rps, rps->boost_freq));

	seq_printf(m, "Wait boosts: %d\n", READ_ONCE(rps->boosts));

	return 0;
}

#ifdef CONFIG_PM

static int i915_runtime_dump_child_status(struct device *dev, void *data)
{
	struct seq_file *m = data;
	const char *rpm_status;

	/* Early return if runtime_pm is disabled */
	if (dev->power.disable_depth)
		return 0;

	switch (dev->power.runtime_status) {
	case RPM_SUSPENDED:
		rpm_status = "suspended";
		break;
	case RPM_SUSPENDING:
		rpm_status = "suspending";
		break;
	case RPM_RESUMING:
		rpm_status = "resuming";
		break;
	case RPM_ACTIVE:
		rpm_status = "active";
		break;
	default:
		rpm_status = "unknown";
	}

	seq_printf(m, "\t%s %s: Runtime status: %s\n", dev_driver_string(dev),
		   dev_name(dev), rpm_status);

	return 0;
}

static void config_pm_dump(struct seq_file *m)
{
	struct drm_i915_private *i915 = m->private;
	struct pci_dev *pdev = to_pci_dev(i915->drm.dev);

	seq_printf(m, "Usage count: %d\n",
		   atomic_read(&i915->drm.dev->power.usage_count));
	seq_printf(m, "Runtime active children: %d\n",
		   atomic_read(&i915->drm.dev->power.child_count));
	device_for_each_child(&pdev->dev, m, i915_runtime_dump_child_status);
}

#else /* !CONFIG_PM */

static int config_pm_dump(struct seq_file *m)
{
	seq_printf(m, "Device Power Management (CONFIG_PM) disabled\n");
	return 0;
}

#endif /* CONFIG_PM */

static int i915_runtime_pm_status_show(struct seq_file *m, void *unused)
{
	struct drm_i915_private *dev_priv = m->private;
	struct pci_dev *pdev = to_pci_dev(dev_priv->drm.dev);

	if (!HAS_RUNTIME_PM(dev_priv))
		seq_puts(m, "Runtime power management not supported\n");

#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
	seq_printf(m, "Runtime power status: %s\n",
		   str_enabled_disabled(!dev_priv->power_domains.init_wakeref));
#endif

	seq_printf(m, "GPU idle: %s\n", str_yes_no(!intel_gt_pm_is_awake(to_gt(dev_priv))));
	seq_printf(m, "IRQs disabled: %s\n",
		   str_yes_no(!intel_irqs_enabled(dev_priv)));
	config_pm_dump(m);
	seq_printf(m, "PCI device power state: %s [%d]\n",
		   pci_power_name(pdev->current_state),
		   pdev->current_state);

	if (IS_ENABLED(CPTCFG_DRM_I915_DEBUG_RUNTIME_PM)) {
		struct drm_printer p = drm_seq_file_printer(m);

		print_intel_runtime_pm_wakeref(&dev_priv->runtime_pm, &p, 0);
	}

	return 0;
}

static int i915_gpu_info_show(struct seq_file *m, void *unused)
{
	struct drm_printer p = drm_seq_file_printer(m);

	i915_show(m->private, &p, 0);
	return 0;
}

static u32
get_whitelist_reg(const struct intel_engine_cs *engine, unsigned int i)
{
	i915_reg_t reg =
		i < engine->whitelist.count ?
		engine->whitelist.list[i].reg :
		RING_NOPID(engine->mmio_base);

	return i915_mmio_reg_offset(reg);
}

static const char *valid(bool state)
{
	return state ? "valid" : "invalid";
}

static int show_whitelist(struct drm_printer *p, struct intel_engine_cs *engine)
{
	int err = 0;
	int i;

	drm_printf(p, "%s: Privileged access allowed: %u\n",
		   engine->name, engine->whitelist.count);

	for (i = 0; i < RING_MAX_NONPRIV_SLOTS; i++) {
		i915_reg_t reg = RING_FORCE_TO_NONPRIV(engine->mmio_base, i);
		u32 expected = get_whitelist_reg(engine, i);
		u32 actual = intel_uncore_read(engine->uncore, reg);

		drm_printf(p, "reg:%04x: { raw:%08x, expected:%08x, %s }\n",
			   i915_mmio_reg_offset(reg),
			   actual, expected, valid(actual == expected));
		if (actual != expected)
			err = -ENXIO;
	}

	return err;
}

static int show_engine_wal(struct drm_printer *p,
			   const char *name,
			   struct intel_engine_cs *engine,
			   const struct i915_wa_list *wal)
{
	drm_printf(p, "%s: Workarounds applied: %u\n", name, wal->count);
	return intel_engine_show_workarounds(p, engine, wal);
}

static int show_gt_wal(struct drm_printer *p,
		       const char *name,
		       struct intel_gt *gt,
		       const struct i915_wa_list *wal)
{
	drm_printf(p, "%s: Workarounds applied: %u\n", name, wal->count);
	return intel_gt_show_workarounds(p, gt, wal);
}

static int workarounds_show(struct seq_file *m, void *unused)
{
	struct drm_i915_private *i915 = m->private;
	struct drm_printer p = drm_seq_file_printer(m);
	struct intel_engine_cs *engine;
	struct intel_gt *gt;
	char buf[80];
	int ret = 0;
	int id;

	for_each_uabi_engine(engine, i915) {
		intel_engine_pm_get(engine);

		cmpxchg(&ret, 0, show_whitelist(&p, engine));

		sprintf(buf, "%s context", engine->name);
		cmpxchg(&ret, 0, show_engine_wal(&p, buf, engine, &engine->ctx_wa_list));

		cmpxchg(&ret, 0, show_engine_wal(&p, engine->name, engine, &engine->wa_list));

		drm_printf(&p, "\n");
		intel_engine_pm_put(engine);
	}

	for_each_gt(gt, i915, id) {
		cmpxchg(&ret, 0, show_gt_wal(&p, gt->name, gt, &gt->wa_list));
		drm_printf(&p, "\n");
	}

	if (ret)
		drm_printf(&p, "Error: %d\n", ret);

	return 0;
}

static int i915_wedged_get(void *data, u64 *val)
{
	struct drm_i915_private *i915 = data;
	struct intel_gt *gt;
	unsigned int i;

	*val = 0;

	for_each_gt(gt, i915, i) {
		int ret;
		u64 v;

		ret = intel_gt_debugfs_reset_show(gt, &v);
		if (ret)
			return ret;

		/* at least one tile should be wedged */
		*val |= !!v;
		if (*val)
			break;
	}

	return 0;
}

static int i915_wedged_set(void *data, u64 val)
{
	struct drm_i915_private *i915 = data;
	struct intel_gt *gt;
	unsigned int i;

	for_each_gt(gt, i915, i)
		intel_gt_debugfs_reset_store(gt, val);

	return 0;
}

DEFINE_I915_SIMPLE_ATTRIBUTE(i915_wedged_fops,
			     i915_wedged_get, i915_wedged_set,
			     "%llu\n");

static int lmemtest_get(void *data, u64 *val)
{
	struct drm_i915_private *i915 = data;
	struct intel_gt *gt;
	unsigned int i;

	*val = 0;
	for_each_gt(gt, i915, i)
		if (gt->lmem)
			*val |= gt->lmem->memtest;

	return 0;
}

static int lmemtest_set(void *data, u64 val)
{
	struct drm_i915_private *i915 = data;
	struct intel_gt *gt;
	unsigned int i;
	int err = 0;

	for_each_gt(gt, i915, i) {
		err = i915_gem_lmemtest(gt, &gt->lmem->memtest);
		if (err)
			break;
	}

	return err;
}

DEFINE_SIMPLE_ATTRIBUTE(lmemtest_fops,
			lmemtest_get, lmemtest_set,
			"0x%016llx\n");

static int
i915_perf_noa_delay_set(void *data, u64 val)
{
	struct drm_i915_private *i915 = data;

	/*
	 * This would lead to infinite waits as we're doing timestamp
	 * difference on the CS with only 32bits.
	 */
	if (intel_gt_ns_to_clock_interval(to_gt(i915), val) > U32_MAX)
		return -EINVAL;

	atomic64_set(&i915->perf.noa_programming_delay, val);
	return 0;
}

static int
i915_perf_noa_delay_get(void *data, u64 *val)
{
	struct drm_i915_private *i915 = data;

	*val = atomic64_read(&i915->perf.noa_programming_delay);
	return 0;
}

DEFINE_I915_SIMPLE_ATTRIBUTE(i915_perf_noa_delay_fops,
			     i915_perf_noa_delay_get,
			     i915_perf_noa_delay_set,
			     "%llu\n");

#define DROP_UNBOUND	BIT(0)
#define DROP_BOUND	BIT(1)
#define DROP_RETIRE	BIT(2)
#define DROP_ACTIVE	BIT(3)
#define DROP_FREED	BIT(4)
#define DROP_SHRINK_ALL	BIT(5)
#define DROP_IDLE	BIT(6)
#define DROP_RESET_ACTIVE	BIT(7)
#define DROP_RESET_SEQNO	BIT(8)
#define DROP_RCU	BIT(9)
#define DROP_ALL (DROP_UNBOUND	| \
		  DROP_BOUND	| \
		  DROP_RETIRE	| \
		  DROP_ACTIVE	| \
		  DROP_FREED	| \
		  DROP_SHRINK_ALL |\
		  DROP_IDLE	| \
		  DROP_RESET_ACTIVE | \
		  DROP_RESET_SEQNO | \
		  DROP_RCU)
static int
i915_drop_caches_get(void *data, u64 *val)
{
	*val = DROP_ALL;

	return 0;
}

static bool has_sriov_wa(struct drm_i915_private *i915)
{
	/* Both pf and vf take an untracked wakeref for their lifetime */
	return IS_SRIOV_PF(i915) || IS_SRIOV_VF(i915);
}

static int
gt_idle(struct intel_gt *gt, u64 val)
{
	int ret;

	if (val & (DROP_RETIRE | DROP_IDLE))
		intel_gt_retire_requests(gt);

	/*
	 * FIXME: At the moment we ugly assume that if we are PF/VF we are idle.
	 * We need a better mechanism to verify this on SR-IOV.
	 */
	if (val & DROP_IDLE && !has_sriov_wa(gt->i915)) {
		ret = intel_gt_pm_wait_for_idle(gt, 30 * HZ);
		if (ret)
			return ret;

		i915_vma_clock_flush(&gt->vma_clock);
	}

	return 0;
}

static void reset_active(struct intel_gt *gt)
{
	struct intel_engine_cs *engine;
	unsigned long hb = 0, pt = 0;
	enum intel_engine_id id;
	intel_wakeref_t wf;
	long timeout;

	wf = intel_gt_pm_get_if_awake(gt);
	if (!wf)
		return;

	timeout = msecs_to_jiffies(I915_IDLE_ENGINES_TIMEOUT);
	if (intel_gt_retire_requests_timeout(gt, &timeout))
		goto out;

	/*
	 * Wait for the pulse to clear any stuck work along each engine
	 * and then allow for the queue to clear (allow for a hearbeart
	 * interval).
	 */
	for_each_engine(engine, gt, id) {
		if (!intel_engine_pm_get_if_awake(engine))
			continue;

		hb = max(hb, engine->defaults.heartbeat_interval_ms);
		if (intel_engine_pulse(engine) == 0)
			pt = max(pt, engine->props.preempt_timeout_ms);

		intel_engine_pm_put(engine);
	}

	timeout = msecs_to_jiffies(I915_IDLE_ENGINES_TIMEOUT + pt + hb);
	if (intel_gt_wait_for_idle(gt, timeout) == -ETIME)
		intel_gt_set_wedged(gt);

out:
	intel_gt_pm_put(gt, wf);

	intel_gt_retire_requests(gt);
	if (!has_sriov_wa(gt->i915) && intel_gt_pm_wait_for_idle(gt, 30 * HZ))
		intel_gt_set_wedged(gt);
}

static int
gt_drop_caches(struct intel_gt *gt, u64 val)
{
	int ret;

	if (val & DROP_RETIRE)
		intel_gt_retire_requests(gt);

	if (val & (DROP_RESET_ACTIVE | DROP_IDLE | DROP_ACTIVE)) {
		ret = intel_gt_wait_for_idle(gt, MAX_SCHEDULE_TIMEOUT);
		if (ret)
			return ret;
	}

	if (val & DROP_RESET_ACTIVE && intel_gt_terminally_wedged(gt))
		intel_gt_handle_error(gt, ALL_ENGINES, 0, NULL);

	if (val & DROP_FREED)
		intel_gt_flush_buffer_pool(gt);

	if (gt->wq)
		flush_workqueue(gt->wq);
	return 0;
}

static void shrink_smem(struct drm_i915_private *i915, u64 val)
{
	intel_wakeref_t wakeref;

	if (!(val & (DROP_BOUND | DROP_UNBOUND | DROP_SHRINK_ALL)))
		return;

	with_intel_runtime_pm(&i915->runtime_pm, wakeref) {
		unsigned int noreclaim_state;

		noreclaim_state = memalloc_noreclaim_save();
		fs_reclaim_acquire(GFP_KERNEL);

		if (val & DROP_BOUND)
			i915_gem_shrink(i915, LONG_MAX, NULL, I915_SHRINK_BOUND);

		if (val & DROP_UNBOUND)
			i915_gem_shrink(i915, LONG_MAX, NULL, I915_SHRINK_UNBOUND);

		if (val & DROP_SHRINK_ALL)
			i915_gem_shrink_all(i915);

		fs_reclaim_release(GFP_KERNEL);
		memalloc_noreclaim_restore(noreclaim_state);
	}
}

static int
__i915_drop_caches_set(struct drm_i915_private *i915, u64 val)
{
	struct intel_gt *gt;
	unsigned int i;
	int ret;

	/* Reset all GT first before doing any waits/flushes */
	if (val & DROP_RESET_ACTIVE) {
		for_each_gt(gt, i915, i)
			reset_active(gt);
	}

	/* Flush all the active requests across both GT ... */
	for_each_gt(gt, i915, i) {
		ret = gt_drop_caches(gt, val);
		if (ret)
			return ret;
	}

	shrink_smem(i915, val);

	/* ... before waiting for idle as there may be cross-gt wakerefs. */
	for_each_gt(gt, i915, i) {
		ret = gt_idle(gt, val);
		if (ret)
			return ret;
	}

	if (val & DROP_RCU)
		rcu_barrier();

	if (val & DROP_FREED)
		i915_gem_drain_freed_objects(i915);

	if (val & DROP_IDLE)
		flush_workqueue(i915->wq);

	return 0;
}

static int
i915_drop_caches_set(void *data, u64 val)
{
	struct drm_i915_private *i915 = data;
	int loop;
	int ret;

	DRM_DEBUG("Dropping caches: 0x%08llx [0x%08llx]\n",
		  val, val & DROP_ALL);

	/*
	 * Run through twice in case we wake up while freeing.
	 *
	 * Primarily this is concerned with L4WA and the like, where
	 * during freeing of objects we may then wake the device up,
	 * invalidating the earlier wait-for-idle. Since the user
	 * expects the device to be idle if they ask for DROP_IDLE,
	 * we want to repeat the wait.
	 *
	 * After the first loop, there should be no more user objects to free
	 * and so the system should settle and require no more than 2 loops
	 * to idle after freeing.
	 */
	for (loop = 0; loop < 2; loop++) {
		ret = __i915_drop_caches_set(i915, val);
		if (ret)
			return ret;
	}

	return 0;
}

DEFINE_I915_SIMPLE_ATTRIBUTE(i915_drop_caches_fops,
			     i915_drop_caches_get, i915_drop_caches_set,
			     "0x%08llx\n");

static int i915_sseu_status_show(struct seq_file *m, void *unused)
{
	struct drm_i915_private *i915 = m->private;
	struct intel_gt *gt = to_gt(i915);

	return intel_sseu_status(m, gt);
}

static int i915_forcewake_open(struct inode *inode, struct file *file)
{
	struct drm_i915_private *i915 = inode->i_private;
	struct intel_gt *gt;
	unsigned int i;

	for_each_gt(gt, i915, i)
		intel_gt_pm_debugfs_forcewake_user_open(gt);

	return 0;
}

static int i915_forcewake_release(struct inode *inode, struct file *file)
{
	struct drm_i915_private *i915 = inode->i_private;
	struct intel_gt *gt;
	unsigned int i;

	for_each_gt(gt, i915, i)
		intel_gt_pm_debugfs_forcewake_user_release(gt);

	return 0;
}

int i915_debugfs_single_open(struct file *file, int (*show)(struct seq_file *, void *),
			     void *data)
{
	struct drm_i915_private *i915 = data;
	int ret;

	ret = single_open(file, show, data);
	if (!ret)
		pvc_wa_disallow_rc6(i915);

	return ret;
}

int i915_debugfs_single_release(struct inode *inode, struct file *file)
{
	struct drm_i915_private *i915 = inode->i_private;

	pvc_wa_allow_rc6(i915);
	return single_release(inode, file);
}

int i915_debugfs_raw_attr_open(struct inode *inode, struct file *file,
			       int (*open)(struct inode*, struct file*))
{
	struct drm_i915_private *i915 = inode->i_private;
	int ret = 0;

	pvc_wa_disallow_rc6(i915);
	if (open)
		ret = open(inode, file);

	if (ret)
		pvc_wa_allow_rc6(i915);

	return ret;
}

int i915_debugfs_raw_attr_close(struct inode *inode, struct file *file,
				int (*close)(struct inode*, struct file*))
{
	struct drm_i915_private *i915 = inode->i_private;
	int ret = 0;

	if (close)
		ret = close(inode, file);
	pvc_wa_allow_rc6(i915);

	return ret;
}

int i915_debugfs_simple_attr_open(struct inode *inode, struct file *file,
				  int (*get)(void *, u64 *), int (*set)(void *, u64),
				  const char *fmt)
{
	struct drm_i915_private *i915 = inode->i_private;
	int ret;

	ret = simple_attr_open(inode, file, get, set, fmt);
	if (!ret)
		pvc_wa_disallow_rc6(i915);

	return ret;
}

int i915_debugfs_simple_attr_release(struct inode *inode, struct file *file)
{
	struct drm_i915_private *i915 = inode->i_private;
	int ret = 0;

	ret = simple_attr_release(inode, file);
	pvc_wa_allow_rc6(i915);

	return ret;
}

DEFINE_I915_RAW_ATTRIBUTE(i915_forcewake_fops, i915_forcewake_open,
			  i915_forcewake_release, NULL, NULL, NULL);
DEFINE_I915_SHOW_ATTRIBUTE(i915_mocs_table);
DEFINE_I915_SHOW_ATTRIBUTE(i915_capabilities);
DEFINE_I915_SHOW_ATTRIBUTE(i915_gem_object_info);
DEFINE_I915_SHOW_ATTRIBUTE(i915_frequency_info);
DEFINE_I915_SHOW_ATTRIBUTE(i915_runtime_pm_status);
DEFINE_I915_SHOW_ATTRIBUTE(i915_gpu_info);
DEFINE_I915_SHOW_ATTRIBUTE(i915_sseu_status);
DEFINE_I915_SHOW_ATTRIBUTE(i915_rps_boost_info);
DEFINE_I915_SHOW_ATTRIBUTE(sriov_info);
DEFINE_I915_SHOW_ATTRIBUTE(workarounds);
DEFINE_I915_SHOW_ATTRIBUTE(lmem_alloc_limit_info);
DEFINE_I915_SHOW_ATTRIBUTE(sharedmem_alloc_limit_info);

static struct i915_debugfs_file i915_debugfs_list[] = {
	{"i915_mocs_table", &i915_mocs_table_fops, NULL},
	{"i915_capabilities", &i915_capabilities_fops, NULL},
	{"i915_gem_objects", &i915_gem_object_info_fops, NULL},
	{"i915_frequency_info", &i915_frequency_info_fops, NULL},
	{"i915_runtime_pm_status", &i915_runtime_pm_status_fops, NULL},
	{"i915_gpu_info", &i915_gpu_info_fops, NULL},
	{"i915_sseu_status", &i915_sseu_status_fops, NULL},
	{"i915_rps_boost_info", &i915_rps_boost_info_fops, NULL},
	{"i915_sriov_info", &sriov_info_fops, NULL},
	{"i915_workarounds", &workarounds_fops, NULL},
	{"lmem_alloc_limit_info", &lmem_alloc_limit_info_fops, NULL},
	{"sharedmem_alloc_limit_info", &sharedmem_alloc_limit_info_fops, NULL},
};

static struct i915_debugfs_file i915_vf_debugfs_list[] = {
	{"i915_capabilities", &i915_capabilities_fops, NULL},
	{"i915_gem_objects", &i915_gem_object_info_fops, NULL},
	{"i915_gpu_info", &i915_gpu_info_fops, NULL},
	{"i915_sriov_info", &sriov_info_fops, NULL},
};

static struct i915_debugfs_file i915_debugfs_files[] = {
	{"i915_perf_noa_delay", &i915_perf_noa_delay_fops},
	{"i915_wedged", &i915_wedged_fops},
	{"i915_gem_drop_caches", &i915_drop_caches_fops},
#if IS_ENABLED(CPTCFG_DRM_I915_CAPTURE_ERROR)
	{"i915_error_state", &i915_error_state_fops},
#endif
	{"lmemtest", &lmemtest_fops},
};

static const struct i915_debugfs_file i915_vf_debugfs_files[] = {
	{"i915_wedged", &i915_wedged_fops},
	{"i915_gem_drop_caches", &i915_drop_caches_fops},
};

void i915_register_debugfs_show_files(struct dentry *root,
				      const struct i915_debugfs_file *files,
				      unsigned long count, void *data)
{
	while (count--) {
		umode_t mode = files->fops->write ? 0644 : 0444;

		debugfs_create_file(files->name,
				    mode, root, data,
				    files->fops);
		files++;
	}
}

void i915_debugfs_register(struct drm_i915_private *dev_priv)
{
	struct drm_minor *minor = dev_priv->drm.primary;
	const struct i915_debugfs_file *debugfs_list = i915_debugfs_list;
	const struct i915_debugfs_file *debugfs_files = i915_debugfs_files;
	size_t debugfs_files_size = ARRAY_SIZE(i915_debugfs_files);
	size_t debugfs_list_size = ARRAY_SIZE(i915_debugfs_list);
	size_t i;

	i915_debugfs_params(dev_priv);

	debugfs_create_file("i915_forcewake_user", S_IRUSR, minor->debugfs_root,
			    to_i915(minor->dev), &i915_forcewake_fops);

	if (IS_SRIOV_VF(dev_priv)) {
		debugfs_files = i915_vf_debugfs_files;
		debugfs_list = i915_vf_debugfs_list;

		debugfs_files_size = ARRAY_SIZE(i915_vf_debugfs_files);
		debugfs_list_size = ARRAY_SIZE(i915_vf_debugfs_list);
	}

	for (i = 0; i < debugfs_files_size; i++) {
		debugfs_create_file(debugfs_files[i].name,
				    S_IRUGO | S_IWUSR,
				    minor->debugfs_root,
				    to_i915(minor->dev),
				    debugfs_files[i].fops);
	}

	i915_register_debugfs_show_files(minor->debugfs_root, debugfs_list,
					 debugfs_list_size, to_i915(minor->dev));
}
