// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include <linux/kobject.h>
#include <linux/sysfs.h>

#include "i915_drv.h"
#include "intel_engine.h"
#include "intel_engine_heartbeat.h"
#include "sysfs_engines.h"

#include "gt/intel_gt.h"
#include "gt/intel_gt_requests.h"

static ssize_t
i915_sysfs_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf);

static ssize_t
i915_sysfs_store(struct kobject *kobj, struct kobj_attribute *attr, const char
		 *buf, size_t count);

typedef ssize_t (*show)(struct kobject *kobj, struct kobj_attribute *attr, char *buf);
typedef ssize_t (*store)(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count);

struct i915_ext_attr {
	struct kobj_attribute attr;
	show i915_show;
	store i915_store;
};

struct kobj_engine {
	struct kobject base;
	struct intel_engine_cs *engine;
};

static struct intel_engine_cs *kobj_to_engine(struct kobject *kobj)
{
	return container_of(kobj, struct kobj_engine, base)->engine;
}

static ssize_t
name_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", kobj_to_engine(kobj)->name);
}

static struct i915_ext_attr name_attr =	{
	__ATTR(name, 0444, i915_sysfs_show, NULL), name_show, NULL};

static ssize_t
class_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", kobj_to_engine(kobj)->uabi_class);
}

static struct i915_ext_attr class_attr = {
	__ATTR(class, 0444, i915_sysfs_show, NULL), class_show, NULL};

static ssize_t
inst_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", kobj_to_engine(kobj)->uabi_instance);
}

static struct i915_ext_attr inst_attr =	{
	__ATTR(instance, 0444, i915_sysfs_show, NULL), inst_show, NULL};

static ssize_t
mmio_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%x\n", kobj_to_engine(kobj)->mmio_base);
}

static struct i915_ext_attr mmio_attr =	{
	__ATTR(mmio_base, 0444, i915_sysfs_show, NULL), mmio_show, NULL};

static const char * const vcs_caps[] = {
	[ilog2(I915_VIDEO_CLASS_CAPABILITY_HEVC)] = "hevc",
	[ilog2(I915_VIDEO_AND_ENHANCE_CLASS_CAPABILITY_SFC)] = "sfc",
	[ilog2(PRELIM_I915_VIDEO_CLASS_CAPABILITY_VDENC)] = "vdenc",
};

static const char * const vecs_caps[] = {
	[ilog2(I915_VIDEO_AND_ENHANCE_CLASS_CAPABILITY_SFC)] = "sfc",
};

static const char * const bcs_caps[] = {
	[ilog2(PRELIM_I915_COPY_CLASS_CAP_BLOCK_COPY)] = "block_copy",
	[ilog2(PRELIM_I915_COPY_CLASS_CAP_SATURATE_PCIE)] = "saturate_pcie",
	[ilog2(PRELIM_I915_COPY_CLASS_CAP_SATURATE_LINK)] = "saturate_link",
	[ilog2(PRELIM_I915_COPY_CLASS_CAP_SATURATE_LMEM)] = "saturate_lmem",
};

static ssize_t repr_trim(char *buf, ssize_t len)
{
	/* Trim off the trailing space and replace with a newline */
	if (len > PAGE_SIZE)
		len = PAGE_SIZE;
	if (len > 0)
		buf[len - 1] = '\n';

	return len;
}

static ssize_t
__caps_show(struct intel_engine_cs *engine,
	    unsigned long caps, char *buf, bool show_unknown)
{
	const char * const *repr;
	int count, n;
	ssize_t len;

	switch (engine->class) {
	case VIDEO_DECODE_CLASS:
		repr = vcs_caps;
		count = ARRAY_SIZE(vcs_caps);
		break;

	case VIDEO_ENHANCEMENT_CLASS:
		repr = vecs_caps;
		count = ARRAY_SIZE(vecs_caps);
		break;

	case COPY_ENGINE_CLASS:
		repr = bcs_caps;
		count = ARRAY_SIZE(bcs_caps);
		break;

	default:
		repr = NULL;
		count = 0;
		break;
	}
	GEM_BUG_ON(count > BITS_PER_LONG);

	len = 0;
	for_each_set_bit(n, &caps, show_unknown ? BITS_PER_LONG : count) {
		if (n >= count || !repr[n]) {
			if (GEM_WARN_ON(show_unknown))
				len += snprintf(buf + len, PAGE_SIZE - len,
						"[%x] ", n);
		} else {
			len += snprintf(buf + len, PAGE_SIZE - len,
					"%s ", repr[n]);
		}
		if (GEM_WARN_ON(len >= PAGE_SIZE))
			break;
	}
	return repr_trim(buf, len);
}

static ssize_t
caps_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct intel_engine_cs *engine = kobj_to_engine(kobj);

	return __caps_show(engine, engine->uabi_capabilities, buf, true);
}

static struct i915_ext_attr caps_attr =	{
	__ATTR(capabilities, 0444, i915_sysfs_show, NULL), caps_show, NULL};

static ssize_t
all_caps_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return __caps_show(kobj_to_engine(kobj), -1, buf, false);
}

static struct i915_ext_attr all_caps_attr = {
	__ATTR(known_capabilities, 0444, i915_sysfs_show, NULL), all_caps_show, NULL};


static ssize_t
max_spin_store(struct kobject *kobj, struct kobj_attribute *attr,
	       const char *buf, size_t count)
{
	struct intel_engine_cs *engine = kobj_to_engine(kobj);
	unsigned long long duration, clamped;
	int err;

	/*
	 * When waiting for a request, if is it currently being executed
	 * on the GPU, we busywait for a short while before sleeping. The
	 * premise is that most requests are short, and if it is already
	 * executing then there is a good chance that it will complete
	 * before we can setup the interrupt handler and go to sleep.
	 * We try to offset the cost of going to sleep, by first spinning
	 * on the request -- if it completed in less time than it would take
	 * to go sleep, process the interrupt and return back to the client,
	 * then we have saved the client some latency, albeit at the cost
	 * of spinning on an expensive CPU core.
	 *
	 * While we try to avoid waiting at all for a request that is unlikely
	 * to complete, deciding how long it is worth spinning is for is an
	 * arbitrary decision: trading off power vs latency.
	 */

	err = kstrtoull(buf, 0, &duration);
	if (err)
		return err;

	clamped = intel_clamp_max_busywait_duration_ns(engine, duration);
	if (duration != clamped)
		return -EINVAL;

	WRITE_ONCE(engine->props.max_busywait_duration_ns, duration);

	return count;
}

static ssize_t
max_spin_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct intel_engine_cs *engine = kobj_to_engine(kobj);

	return sprintf(buf, "%lu\n", engine->props.max_busywait_duration_ns);
}

static struct i915_ext_attr max_spin_attr = {
	__ATTR(max_busywait_duration_ns, 0644, i915_sysfs_show,
	       i915_sysfs_store), max_spin_show, max_spin_store};

static ssize_t
max_spin_default(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct intel_engine_cs *engine = kobj_to_engine(kobj);

	return sprintf(buf, "%lu\n", engine->defaults.max_busywait_duration_ns);
}

static struct i915_ext_attr max_spin_def = {
	__ATTR(max_busywait_duration_ns, 0444, i915_sysfs_show, NULL),
	max_spin_default, NULL};

static ssize_t
timeslice_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	struct intel_engine_cs *engine = kobj_to_engine(kobj);
	unsigned long long duration, clamped;
	int err;

	/*
	 * Execlists uses a scheduling quantum (a timeslice) to alternate
	 * execution between ready-to-run contexts of equal priority. This
	 * ensures that all users (though only if they of equal importance)
	 * have the opportunity to run and prevents livelocks where contexts
	 * may have implicit ordering due to userspace semaphores.
	 */

	err = kstrtoull(buf, 0, &duration);
	if (err)
		return err;

	clamped = intel_clamp_timeslice_duration_ms(engine, duration);
	if (duration != clamped)
		return -EINVAL;

	WRITE_ONCE(engine->props.timeslice_duration_ms, duration);

	if (execlists_active(&engine->execlists))
		set_timer_ms(&engine->execlists.timer, duration);

	return count;
}

static ssize_t
timeslice_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct intel_engine_cs *engine = kobj_to_engine(kobj);

	return sprintf(buf, "%lu\n", engine->props.timeslice_duration_ms);
}

static struct i915_ext_attr timeslice_duration_attr = {
	__ATTR(timeslice_duration_ms, 0644,i915_sysfs_show, i915_sysfs_store),
	timeslice_show, timeslice_store};

static ssize_t
timeslice_default(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct intel_engine_cs *engine = kobj_to_engine(kobj);

	return sprintf(buf, "%lu\n", engine->defaults.timeslice_duration_ms);
}

static struct i915_ext_attr timeslice_duration_def = {
	__ATTR(timeslice_duration_ms, 0444, i915_sysfs_show, NULL),
	timeslice_default, NULL};

static ssize_t
stop_store(struct kobject *kobj, struct kobj_attribute *attr,
	   const char *buf, size_t count)
{
	struct intel_engine_cs *engine = kobj_to_engine(kobj);
	unsigned long long duration, clamped;
	int err;

	/*
	 * When we allow ourselves to sleep before a GPU reset after disabling
	 * submission, even for a few milliseconds, gives an innocent context
	 * the opportunity to clear the GPU before the reset occurs. However,
	 * how long to sleep depends on the typical non-preemptible duration
	 * (a similar problem to determining the ideal preempt-reset timeout
	 * or even the heartbeat interval).
	 */

	err = kstrtoull(buf, 0, &duration);
	if (err)
		return err;

	clamped = intel_clamp_stop_timeout_ms(engine, duration);
	if (duration != clamped)
		return -EINVAL;

	WRITE_ONCE(engine->props.stop_timeout_ms, duration);
	return count;
}

static ssize_t
stop_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct intel_engine_cs *engine = kobj_to_engine(kobj);

	return sprintf(buf, "%lu\n", engine->props.stop_timeout_ms);
}

static struct i915_ext_attr stop_timeout_attr = {
	__ATTR(stop_timeout_ms, 0644, i915_sysfs_show, i915_sysfs_store),
	stop_show, stop_store};

static ssize_t
stop_default(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct intel_engine_cs *engine = kobj_to_engine(kobj);

	return sprintf(buf, "%lu\n", engine->defaults.stop_timeout_ms);
}

static struct i915_ext_attr stop_timeout_def = {
	__ATTR(stop_timeout_ms, 0444, i915_sysfs_show, NULL), stop_default, NULL};

static ssize_t
preempt_timeout_store(struct kobject *kobj, struct kobj_attribute *attr,
		      const char *buf, size_t count)
{
	struct intel_engine_cs *engine = kobj_to_engine(kobj);
	unsigned long long timeout, clamped;
	int err;

	/*
	 * After initialising a preemption request, we give the current
	 * resident a small amount of time to vacate the GPU. The preemption
	 * request is for a higher priority context and should be immediate to
	 * maintain high quality of service (and avoid priority inversion).
	 * However, the preemption granularity of the GPU can be quite coarse
	 * and so we need a compromise.
	 */

	err = kstrtoull(buf, 0, &timeout);
	if (err)
		return err;

	clamped = intel_clamp_preempt_timeout_ms(engine, timeout);
	if (timeout != clamped)
		return -EINVAL;

	WRITE_ONCE(engine->props.preempt_timeout_ms, timeout);

	if (READ_ONCE(engine->execlists.pending[0]))
		set_timer_ms(&engine->execlists.preempt, timeout);

	return count;
}

static ssize_t
preempt_timeout_show(struct kobject *kobj, struct kobj_attribute *attr,
		     char *buf)
{
	struct intel_engine_cs *engine = kobj_to_engine(kobj);

	return sprintf(buf, "%lu\n", engine->props.preempt_timeout_ms);
}

static struct i915_ext_attr preempt_timeout_attr = {
	__ATTR(preempt_timeout_ms, 0644, i915_sysfs_show, i915_sysfs_store),
	preempt_timeout_show, preempt_timeout_store};

static ssize_t
preempt_timeout_default(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf)
{
	struct intel_engine_cs *engine = kobj_to_engine(kobj);

	return sprintf(buf, "%lu\n", engine->defaults.preempt_timeout_ms);
}

static struct i915_ext_attr preempt_timeout_def = {
	__ATTR(preempt_timeout_ms, 0444, i915_sysfs_show, NULL),
	preempt_timeout_default, NULL};

static ssize_t
heartbeat_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	struct intel_engine_cs *engine = kobj_to_engine(kobj);
	unsigned long long delay, clamped;
	int err;

	/*
	 * We monitor the health of the system via periodic heartbeat pulses.
	 * The pulses also provide the opportunity to perform garbage
	 * collection.  However, we interpret an incomplete pulse (a missed
	 * heartbeat) as an indication that the system is no longer responsive,
	 * i.e. hung, and perform an engine or full GPU reset. Given that the
	 * preemption granularity can be very coarse on a system, the optimal
	 * value for any workload is unknowable!
	 */

	err = kstrtoull(buf, 0, &delay);
	if (err)
		return err;

	clamped = intel_clamp_heartbeat_interval_ms(engine, delay);
	if (delay != clamped)
		return -EINVAL;

	err = intel_engine_set_heartbeat(engine, delay);
	if (err)
		return err;

	return count;
}

static ssize_t
heartbeat_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct intel_engine_cs *engine = kobj_to_engine(kobj);

	return sprintf(buf, "%lu\n", engine->props.heartbeat_interval_ms);
}

static struct i915_ext_attr heartbeat_interval_attr = {
	__ATTR(heartbeat_interval_ms, 0644, i915_sysfs_show, i915_sysfs_store),
	heartbeat_show, heartbeat_store};

static ssize_t
heartbeat_default(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct intel_engine_cs *engine = kobj_to_engine(kobj);

	return sprintf(buf, "%lu\n", engine->defaults.heartbeat_interval_ms);
}

static struct i915_ext_attr heartbeat_interval_def = {
	__ATTR(heartbeat_interval_ms, 0444, i915_sysfs_show, NULL),
	heartbeat_default, NULL};

static ssize_t
runtime_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct intel_engine_cs *engine = kobj_to_engine(kobj);
	ktime_t dummy;

	return sprintf(buf, "%llu\n",
		       ktime_to_ms(intel_engine_get_busy_time(engine, 0, &dummy)));
}

static struct i915_ext_attr runtime_attr = {
	__ATTR(runtime_ms, 0444, i915_sysfs_show, NULL), runtime_show, NULL};

static void kobj_engine_release(struct kobject *kobj)
{
	kfree(kobj);
}

static struct kobj_type kobj_engine_type = {
	.release = kobj_engine_release,
	.sysfs_ops = &kobj_sysfs_ops
};

static struct kobject *
kobj_engine(struct kobject *dir, struct intel_engine_cs *engine)
{
	struct kobj_engine *ke;

	ke = kzalloc(sizeof(*ke), GFP_KERNEL);
	if (!ke)
		return NULL;

	kobject_init(&ke->base, &kobj_engine_type);
	ke->engine = engine;

	if (kobject_add(&ke->base, dir, "%s", engine->name)) {
		kobject_put(&ke->base);
		return NULL;
	}

	/* xfer ownership to sysfs tree */
	return &ke->base;
}

static ssize_t
i915_sysfs_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	ssize_t value;
	struct i915_ext_attr *ea = container_of(attr, struct i915_ext_attr, attr);
	struct intel_engine_cs *engine = kobj_to_engine(kobj);

	/* Wa_16015476723 & Wa_16015666671 */
	pvc_wa_disallow_rc6(engine->i915);

	value = ea->i915_show(kobj, attr, buf);

	pvc_wa_allow_rc6(engine->i915);

	return value;
}

static ssize_t
i915_sysfs_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct i915_ext_attr *ea = container_of(attr, struct i915_ext_attr, attr);
	struct intel_engine_cs *engine = kobj_to_engine(kobj);

	/* Wa_16015476723 & Wa_16015666671 */
	pvc_wa_disallow_rc6(engine->i915);

	count = ea->i915_store(kobj, attr, buf, count);

	pvc_wa_allow_rc6(engine->i915);

	return count;
}

static void add_defaults(struct kobj_engine *parent)
{
	static const struct attribute *files[] = {
		&max_spin_def.attr.attr,
		&stop_timeout_def.attr.attr,
#if CPTCFG_DRM_I915_HEARTBEAT_INTERVAL
		&heartbeat_interval_def.attr.attr,
#endif
		NULL
	};
	struct kobj_engine *ke;

	ke = kzalloc(sizeof(*ke), GFP_KERNEL);
	if (!ke)
		return;

	kobject_init(&ke->base, &kobj_engine_type);
	ke->engine = parent->engine;

	if (kobject_add(&ke->base, &parent->base, "%s", ".defaults")) {
		kobject_put(&ke->base);
		return;
	}

	if (sysfs_create_files(&ke->base, files))
		return;

	if (intel_engine_has_timeslices(ke->engine) &&
	    sysfs_create_file(&ke->base, &timeslice_duration_def.attr.attr))
		return;

	if (intel_engine_has_preempt_reset(ke->engine) &&
	    sysfs_create_file(&ke->base, &preempt_timeout_def.attr.attr))
		return;
}

void intel_engines_add_sysfs(struct drm_i915_private *i915)
{
	static const struct attribute *files[] = {
		&name_attr.attr.attr,
		&class_attr.attr.attr,
		&inst_attr.attr.attr,
		&mmio_attr.attr.attr,
		&caps_attr.attr.attr,
		&all_caps_attr.attr.attr,
		&max_spin_attr.attr.attr,
		&stop_timeout_attr.attr.attr,
#if CPTCFG_DRM_I915_HEARTBEAT_INTERVAL
		&heartbeat_interval_attr.attr.attr,
#endif
		NULL
	};

	struct device *kdev = i915->drm.primary->kdev;
	struct intel_engine_cs *engine;
	struct kobject *dir;

	dir = kobject_create_and_add("engine", &kdev->kobj);
	if (!dir)
		return;

	for_each_uabi_engine(engine, i915) {
		struct kobject *kobj;

		kobj = kobj_engine(dir, engine);
		if (!kobj)
			goto err_engine;

		if (sysfs_create_files(kobj, files))
			goto err_object;

		if (intel_engine_has_timeslices(engine) &&
		    sysfs_create_file(kobj, &timeslice_duration_attr.attr.attr))
			goto err_engine;

		if (intel_engine_has_preempt_reset(engine) &&
		    sysfs_create_file(kobj, &preempt_timeout_attr.attr.attr))
			goto err_engine;

		if (intel_engine_supports_stats(engine) &&
		    sysfs_create_file(kobj, &runtime_attr.attr.attr))
			goto err_engine;

		add_defaults(container_of(kobj, struct kobj_engine, base));

		if (0) {
err_object:
			kobject_put(kobj);
err_engine:
			dev_err(kdev, "Failed to add sysfs engine '%s'\n",
				engine->name);
			break;
		}
	}
}
