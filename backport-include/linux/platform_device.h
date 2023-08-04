#ifndef _BACKPORT_PLATFORM_DEVICE_H_
#define _BACKPORT_PLATFORM_DEVICE_H_
#include_next <linux/platform_device.h>

#ifdef BPM_PLATFORM_GET_IRQ_OPTIONAL_NOT_PRESENT
#define platform_get_irq_optional platform_get_irq
#endif

#endif
