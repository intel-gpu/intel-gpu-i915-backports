/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BACKPORT_LINUX_MEI_AUX_H
#define _BACKPORT_LINUX_MEI_AUX_H

#ifdef BPM_MEI_AUX_BUS_AVAILABLE
#include_next <linux/mei_aux.h>
#else
#include <linux/platform_device.h>
#endif

#endif /* _BACKPORT_LINUX_MEI_AUX_H */
