/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The proc filesystem constants/structures
 */
#ifndef _BACKPORT_LINUX_PROC_FS_H
#define _BACKPORT_LINUX_PROC_FS_H

#include_next <linux/proc_fs.h>

#ifdef BPM_PCIE_AER_IS_NATIVE_API_NOT_PRESENT
int check_pcie_port_param(void);

#ifdef CONFIG_PCIEPORTBUS
extern bool pcie_ports_native;
#else
#define pcie_ports_native	false
#endif

#endif

#endif /* _BACKPORT_LINUX_PROC_FS_H*/
