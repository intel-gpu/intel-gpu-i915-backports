/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
*      PCI standard defines
*      Copyright 1994, Drew Eckhardt
*      Copyright 1997--1999 Martin Mares <mj@ucw.cz>
*
*      For more information, please consult the following manuals (look at
*      http://www.pcisig.com/ for how to get them):
*
*      PCI BIOS Specification
*      PCI Local Bus Specification
*      PCI to PCI Bridge Specification
*      PCI System Design Guide
*
*      For HyperTransport information, please consult the following manuals
*      from http://www.hypertransport.org :
*
*      The HyperTransport I/O Link Specification
*/

#ifndef _BACKPORT_LINUX_PCI_REGS_H
#define _BACKPORT_LINUX_PCI_REGS_H

#include_next <linux/pci_regs.h>

#define PCI_EXT_CAP_ID_VF_REBAR 0x24    /* VF Resizable BAR */

#ifdef BPM_PCI_STD_NUM_BARS_NOT_DEFINED
#define PCI_STD_NUM_BARS        6       /* Number of standard BARs */
#endif

#endif

