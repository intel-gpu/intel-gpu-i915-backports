#ifndef __BACKPORT_PM_QOS_H
#define __BACKPORT_PM_QOS_H
#include <linux/version.h>
#include_next <linux/pm_qos.h>

#ifdef BPM_CPU_LATENCY_QOS_NOT_PRESENT

#define PM_QOS_CPU_LATENCY_DEFAULT_VALUE PM_QOS_CPU_DMA_LAT_DEFAULT_VALUE

#define cpu_latency_qos_request_active LINUX_I915_BACKPORT(cpu_latency_qos_request_active)
#define cpu_latency_qos_add_request LINUX_I915_BACKPORT(cpu_latency_qos_add_request)
#define cpu_latency_qos_remove_request LINUX_I915_BACKPORT(cpu_latency_qos_remove_request)
#define cpu_latency_qos_update_request LINUX_I915_BACKPORT(cpu_latency_qos_update_request)

#ifdef CPTCFG_CPU_IDLE
bool cpu_latency_qos_request_active(struct pm_qos_request *req);
void cpu_latency_qos_add_request(struct pm_qos_request *req, s32 value);
void cpu_latency_qos_update_request(struct pm_qos_request *req, s32 new_value);
void cpu_latency_qos_remove_request(struct pm_qos_request *req);
#else
static inline bool cpu_latency_qos_request_active(struct pm_qos_request *req)
{
	        return false;
}
static inline void cpu_latency_qos_add_request(struct pm_qos_request *req,
		                                               s32 value) {}
static inline void cpu_latency_qos_update_request(struct pm_qos_request *req,
		                                                  s32 new_value) {}
static inline void cpu_latency_qos_remove_request(struct pm_qos_request *req) {}
#endif /* CPTCFG_CPU_IDLE */

#endif /* BPM_CPU_LATENCY_QOS_NOT_PRESENT */

#endif /* __BACKPORT_PM_QOS_H */
