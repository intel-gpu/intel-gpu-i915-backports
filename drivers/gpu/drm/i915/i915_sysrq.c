// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/sysrq.h>
#include <linux/utsname.h>

#include "gt/uc/intel_guc.h"

#include "gt/intel_engine.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_pm.h"
#include "gt/intel_timeline.h"
#include "gt/intel_tlb.h"

#include "i915_driver.h"
#include "i915_drm_client.h"
#include "i915_drv.h"
#include "i915_irq.h"
#include "i915_request.h"
#include "i915_sysrq.h"
#include "intel_memory_region.h"
#include "intel_wakeref.h"

#ifdef BPM_ADD_MODULE_VERSION_MACRO_IN_ALL_MOD
#include <backport/bp_module_version.h>
#endif

static DEFINE_MUTEX(sysrq_mutex);
static LIST_HEAD(sysrq_list);

struct sysrq_cb {
	struct list_head link;
	struct rcu_head rcu;

	void (*fn)(void *data);
	void *data;
};

#ifdef BPM_SYSRQ_KEY_OP_HANDLER_INT_ARG_NOT_PRESENT
typedef u8 sysrq_key;
#else
typedef int sysrq_key;
#endif

static void sysrq_handle_showgpu(sysrq_key key)
{
	struct sysrq_cb *cb;

	rcu_read_lock();
	list_for_each_entry(cb, &sysrq_list, link)
		cb->fn(cb->data);
	rcu_read_unlock();
}

#ifdef BPM_CONST_SYSRQ_KEY_OP_NOT_PRESENT
static struct sysrq_key_op sysrq_showgpu_op = {
#else
static const struct sysrq_key_op sysrq_showgpu_op = {
#endif
        .handler        = sysrq_handle_showgpu,
        .help_msg       = "show-gpu(G)",
        .action_msg     = "Show GPU state",
        .enable_mask    = SYSRQ_ENABLE_DUMP,
};

static int register_sysrq(void (*fn)(void *data), void *data)
{
	struct sysrq_cb *cb;
	int ret = 0;

	cb = kmalloc(sizeof(*cb), GFP_KERNEL);
	if (!cb)
		return -ENOMEM;

	cb->fn = fn;
	cb->data = data;

	mutex_lock(&sysrq_mutex);
	if (list_empty(&sysrq_list))
		ret = register_sysrq_key('G', &sysrq_showgpu_op);
	if (ret == 0)
		list_add_tail_rcu(&cb->link, &sysrq_list);
	else
		kfree(cb);
	mutex_unlock(&sysrq_mutex);

	return ret;
}

static void unregister_sysrq(void (*fn)(void *data), void *data)
{
	struct sysrq_cb *cb;

	mutex_lock(&sysrq_mutex);
	list_for_each_entry(cb, &sysrq_list, link) {
		if (cb->fn == fn && cb->data == data) {
			list_del_rcu(&cb->link);
			if (list_empty(&sysrq_list))
				unregister_sysrq_key('G', &sysrq_showgpu_op);
			kfree_rcu(cb, rcu);
			break;
		}
	}
	mutex_unlock(&sysrq_mutex);

	/* Flush the handler before our caller can free fn/data */
	synchronize_rcu();
}

static void show_gpu_mem(struct drm_i915_private *i915, struct drm_printer *p, int indent)
{
	struct intel_memory_region *mr;
	enum intel_region_id id;

	i_printf(p, indent, "Memory:\n");
	indent += 2;

	for_each_memory_region(mr, i915, id) {
		i_printf(p, indent, "- region:\n");
		intel_memory_region_print(mr, 0, p, indent + 2);
	}
}

static void show_ccs_mode(struct intel_gt *gt, struct drm_printer *p, int indent)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id tmp;
	char buf[240], *s = buf;
	const char *prefix;

	if (!IS_PONTEVECCHIO(gt->i915))
		return;

	s += snprintf(s, sizeof(buf) - (s - buf), "mode:%08x, ", gt->ccs.mode);

	prefix =", ";
	s += snprintf(s, sizeof(buf) - (s - buf), "config:%08x", gt->ccs.config);
	if (gt->ccs.config) {
		prefix = " [";
		for_each_engine_masked(engine, gt, gt->ccs.config, tmp) {
			s += snprintf(s, sizeof(buf) - (s - buf), "%s%s", prefix, engine->name);
			prefix = ", ";
		}
		prefix = "], ";
	}
	s += snprintf(s, sizeof(buf) - (s - buf), "%s", prefix);

	prefix ="";
	s += snprintf(s, sizeof(buf) - (s - buf), "active:%08x", gt->ccs.active);
	if (gt->ccs.active) {
		prefix = " [";
		for_each_engine_masked(engine, gt, gt->ccs.active, tmp) {
			s += snprintf(s, sizeof(buf) - (s - buf), "%s%s", prefix, engine->name);
			prefix = ", ";
		}
		prefix = "]";
	}
	s += snprintf(s, sizeof(buf) - (s - buf), "%s", prefix);

	i_printf(p, indent ,"multiCCS: { %s }\n", buf);
}

static void show_gt(struct intel_gt *gt, struct drm_printer *p, int indent)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	intel_wakeref_t wakeref;
	u64 t;

	if (!intel_gt_pm_is_awake(gt)) {
		i_printf(p, indent, "GT%d: idle\n", gt->info.id);
		return;
	}

	if (intel_uncore_read(gt->uncore, SOFTWARE_FLAGS_SPR33) == -1) {
		i_printf(p, indent, "GT%d: dead\n", gt->info.id);
		return;
	}

	i_printf(p, indent, "GT%d: awake: %s [%d], %llums, mask: %x\n",
		 gt->info.id,
		 str_yes_no(intel_gt_pm_is_awake(gt)),
		 atomic_read(&gt->wakeref.count),
		 ktime_to_ms(intel_gt_get_awake_time(gt)),
		 atomic_read(&gt->user_engines));
	indent += 2;
	if (intel_gt_pm_is_awake(gt))
		intel_wakeref_show(&gt->wakeref, p, indent);

	i_printf(p, indent, "Interrupts: { count: %lu, total: %lluns, avg: %luns, max: %luns }\n",
		 READ_ONCE(gt->stats.irq.count),
		 READ_ONCE(gt->stats.irq.total),
		 ewma_irq_time_read(&gt->stats.irq.avg),
		 READ_ONCE(gt->stats.irq.max));
	if (HAS_RECOVERABLE_PAGE_FAULT(gt->i915)) {
		i_printf(p, indent, "Pagefaults: { minor: %lu, major: %lu, invalid: %lu, debugger: %s }\n",
			 local_read(&gt->stats.pagefault_minor),
			 local_read(&gt->stats.pagefault_major),
			 local_read(&gt->stats.pagefault_invalid),
			 str_yes_no(i915_active_fence_isset(&gt->eu_debug.fault)));
	}
	intel_gt_show_tlb(gt, p, indent);

	t = local64_read(&gt->stats.migration_stall);
	if (t >> 20)
		i_printf(p, indent, "Migration: { stalls: %lldms } \n",
			 div_u64(t, NSEC_PER_MSEC));

	show_ccs_mode(gt, p, indent);
	i_printf(p, indent, "EU: { config: %ux%ux%u, total: %u }\n",
		 hweight8(gt->info.sseu.slice_mask),
		 intel_sseu_subslice_total(&gt->info.sseu),
		 gt->info.sseu.eu_per_subslice,
		 gt->info.sseu.eu_total);

	with_intel_gt_pm_if_awake(gt, wakeref)
		intel_guc_print_info(&gt->uc.guc, p, indent);

	for_each_engine(engine, gt, id) {
		if (intel_engine_is_idle(engine))
			continue;

		intel_engine_dump(engine, p, indent);
	}

	intel_gt_show_timelines(gt, p, indent, i915_request_show_with_schedule);
}

static void show_gts(struct drm_i915_private *i915, struct drm_printer *p, int indent)
{
	struct intel_gt *gt;
	int i;

	for_each_gt(gt, i915, i)
		show_gt(gt, p, indent);
}

static void show_rpm(struct drm_i915_private *i915, struct drm_printer *p, int indent)
{
#if IS_ENABLED(CPTCFG_DRM_I915_DISPLAY)
	i_printf(p, indent, "Runtime power status: %s\n",
		 str_enabled_disabled(!i915->power_domains.init_wakeref));
#endif
	print_intel_runtime_pm_wakeref(&i915->runtime_pm, p, indent);
}

static void __dev_printfn_info(struct drm_printer *p, struct va_format *vaf)
{
	dev_info(p->arg, "%pV", vaf);
}

static bool iommu_required(struct drm_i915_private *i915)
{
	const u64 dma_mask = -BIT(INTEL_INFO(i915)->dma_mask_size);
	struct intel_memory_region *mr;
	int id;

	for_each_memory_region(mr, i915, id) {
		if (!mr->io_size)
			continue;

		if ((mr->io_start + mr->io_size - 1) & dma_mask)
			return true;
	}

	return false;
}

static void pci_show(struct pci_dev *pdev, struct drm_printer *p, int indent)
{
	extern const char *pci_speed_string(enum pci_bus_speed speed);
	enum pci_bus_speed speed;
	enum pcie_link_width width;
	u32 bw;

	pci_read_config_dword(pdev, PCI_COMMAND, &bw);
	if (bw == -1)
		return;

	bw = pcie_bandwidth_available(pdev, NULL, &speed, &width) >> 3;
	i_printf(p, indent, "PCIe: { speed: %s, width: %d, bandwidth: %d MiB/s }\n",
		 pci_speed_string(speed), width, bw);
}

void i915_show(struct drm_i915_private *i915, struct drm_printer *p, int indent)
{
	struct pci_dev *pdev = to_pci_dev(i915->drm.dev);
	const struct intel_runtime_info *r = RUNTIME_INFO(i915);

	i_printf(p, indent, "---\n");
	i_printf(p, indent, "Device: %s\n", dev_name(i915->drm.dev));
	i_printf(p, indent, "Platform: %s [%04x:%04x r%d], %s [%s] %s\n",
		 intel_platform_name(INTEL_INFO(i915)->platform),
		 pdev->vendor, pdev->device, pdev->revision,
		 init_utsname()->release,
#ifdef BPM_ADD_DEBUG_PRINTS_BKPT_MOD
		 BACKPORT_MOD_VER,
#else
		 "DII",
#endif
		 init_utsname()->machine);

	if (r->graphics.ver) {
		if (r->graphics.rel)
			i_printf(p, indent + 2, "graphics version: %u.%02u\n",
				 r->graphics.ver, r->graphics.rel);
		else
			i_printf(p, indent + 2, "graphics version: %u\n",
				 r->graphics.ver);
	}

	if (r->media.ver) {
		if (r->media.rel)
			i_printf(p, indent + 2, "media version: %u.%02u\n",
				 r->media.ver, r->media.rel);
		else
			i_printf(p, indent + 2, "media version: %u\n",
				 r->media.ver);
	}

	if (r->display.ver) {
		if (r->display.rel)
			i_printf(p, indent + 2, "display version: %u.%02u\n",
				 r->display.ver, r->display.rel);
		else
			i_printf(p, indent + 2, "display version: %u\n",
				 r->display.ver);
	}

	pci_show(pdev, p, indent);
	if (dev_to_node(i915->drm.dev) != NUMA_NO_NODE)
		i_printf(p, indent, "NUMA: { node: %d }\n", dev_to_node(i915->drm.dev));
	i_printf(p, indent, "CPU: (%*pbl)\n", cpumask_pr_args(i915->sched->cpumask));
	i_printf(p, indent, "IOMMU: { dma-width: %d, %s%s }\n",
		 INTEL_INFO(i915)->dma_mask_size,
		 str_enabled_disabled(i915_vtd_active(i915) > 0),
		 iommu_required(i915) ? ", required" : "");
	i_printf(p, indent, "IRQ: %d, %s\n",
		 pdev->irq, str_enabled_disabled(intel_irqs_enabled(i915)));
	if (IS_IOV_ACTIVE(i915))
		i_printf(p, indent, "Virtualisation: %s\n",
			 i915_iov_mode_to_string(IOV_MODE(i915)));

	show_rpm(i915, p, indent);
	show_gts(i915, p, indent);
	show_gpu_mem(i915, p, indent);
	i915_drm_clients_show(&i915->clients, p, indent);
}

static void show_gpu(void *data)
{
	struct drm_i915_private *i915 = data;
	struct drm_printer p = {
		.printfn = __dev_printfn_info,
		.arg = i915->drm.dev,
	};

	i915_show(i915, &p, 0);
}

int i915_register_sysrq(struct drm_i915_private *i915)
{
	return register_sysrq(show_gpu, i915);
}

void i915_unregister_sysrq(struct drm_i915_private *i915)
{
	unregister_sysrq(show_gpu, i915);
}
