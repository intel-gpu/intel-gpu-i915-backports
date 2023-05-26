/*
 * Copyright (c) 2021
 *
 * Backport functionality introduced in Linux 5.8.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifdef BPM_CPU_LATENCY_QOS_NOT_PRESENT
#include <linux/pm_qos.h>

#ifdef CPTCFG_CPU_IDLE
/* Definitions related to the CPU latency QoS. */

static struct pm_qos_constraints cpu_latency_constraints = {
	.list = PLIST_HEAD_INIT(cpu_latency_constraints.list),
	.target_value = PM_QOS_CPU_LATENCY_DEFAULT_VALUE,
	.default_value = PM_QOS_CPU_LATENCY_DEFAULT_VALUE,
	.no_constraint_value = PM_QOS_CPU_LATENCY_DEFAULT_VALUE,
	.type = PM_QOS_MIN,
};

/**
 * * cpu_latency_qos_limit - Return current system-wide CPU latency QoS limit.
 * */
s32 cpu_latency_qos_limit(void)
{
	return pm_qos_read_value(&cpu_latency_constraints);
}
/**
 * cpu_latency_qos_request_active - Check the given PM QoS request.
 * @req: PM QoS request to check.
 *
 * Return: 'true' if @req has been added to the CPU latency QoS list, 'false'
 * otherwise.
 */
bool cpu_latency_qos_request_active(struct pm_qos_request *req)
{
	return req->qos == &cpu_latency_constraints;
}
EXPORT_SYMBOL_GPL(cpu_latency_qos_request_active);

static void cpu_latency_qos_apply(struct pm_qos_request *req,
		enum pm_qos_req_action action, s32 value)
{
	int ret = pm_qos_update_target(req->qos, &req->node, action, value);
	if (ret > 0)
		wake_up_all_idle_cpus();
}

/**
 * cpu_latency_qos_add_request - Add new CPU latency QoS request.
 * @req: Pointer to a preallocated handle.
 * @value: Requested constraint value.
 *
 * Use @value to initialize the request handle pointed to by @req, insert it as
 * a new entry to the CPU latency QoS list and recompute the effective QoS
 * constraint for that list.
 *
 * Callers need to save the handle for later use in updates and removal of the
 * QoS request represented by it.
 */
void cpu_latency_qos_add_request(struct pm_qos_request *req, s32 value)
{
	if (!req)
		return;

	if (cpu_latency_qos_request_active(req)) {
		WARN(1, KERN_ERR "%s called for already added request\n", __func__);
		return;
	}

//	trace_pm_qos_add_request(value);

	req->qos = &cpu_latency_constraints;
	cpu_latency_qos_apply(req, PM_QOS_ADD_REQ, value);
}
EXPORT_SYMBOL_GPL(cpu_latency_qos_add_request);

/**
 * cpu_latency_qos_update_request - Modify existing CPU latency QoS request.
 * @req : QoS request to update.
 * @new_value: New requested constraint value.
 *
 * Use @new_value to update the QoS request represented by @req in the CPU
 * latency QoS list along with updating the effective constraint value for that
 * list.
 */
void cpu_latency_qos_update_request(struct pm_qos_request *req, s32 new_value)
{
	if (!req)
		return;

	if (!cpu_latency_qos_request_active(req)) {
		WARN(1, KERN_ERR "%s called for unknown object\n", __func__);
		return;
	}

//	trace_pm_qos_update_request(new_value);

	if (new_value == req->node.prio)
		return;

	cpu_latency_qos_apply(req, PM_QOS_UPDATE_REQ, new_value);
}
EXPORT_SYMBOL_GPL(cpu_latency_qos_update_request);

/*
 * cpu_latency_qos_remove_request - Remove existing CPU latency QoS request.
 * @req: QoS request to remove.
 *
 * Remove the CPU latency QoS request represented by @req from the CPU latency
 * QoS list along with updating the effective constraint value for that list.
 */
void cpu_latency_qos_remove_request(struct pm_qos_request *req)
{
	if (!req)
		return;

	if (!cpu_latency_qos_request_active(req)) {
		WARN(1, KERN_ERR "%s called for unknown object\n", __func__);
		return;
	}

//	trace_pm_qos_remove_request(PM_QOS_DEFAULT_VALUE);

	cpu_latency_qos_apply(req, PM_QOS_REMOVE_REQ, PM_QOS_DEFAULT_VALUE);
	memset(req, 0, sizeof(*req));
}
EXPORT_SYMBOL_GPL(cpu_latency_qos_remove_request);
#endif /* CPTCFG_CPU_IDLE */
#endif /* BPM_CPU_LATENCY_QOS_NOT_PRESENT */
