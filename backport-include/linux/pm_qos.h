#ifndef _COMPAT_LINUX_PM_QOS_H
#define _COMPAT_LINUX_PM_QOS_H
#include <linux/version.h>
#if LINUX_VERSION_IS_GEQ(3,2,0)
#include_next <linux/pm_qos.h>
#else
#include <linux/pm_qos_params.h>
#endif /* LINUX_VERSION_IS_GEQ(3,2,0) */

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

#ifndef PM_QOS_DEFAULT_VALUE
#define PM_QOS_DEFAULT_VALUE -1
#endif

#endif	/* _COMPAT_LINUX_PM_QOS_H */
