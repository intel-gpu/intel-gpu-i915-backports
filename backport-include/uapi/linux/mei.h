/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/*
 * Copyright(c) 2003-2015 Intel Corporation. All rights reserved.
 * Intel Management Engine Interface (Intel MEI) Linux driver
 * Intel MEI Interface Header
 */

#ifndef _BACKPORT_UAPI_LINUX_MEI_H
#define _BACKPORT_UAPI_LINUX_MEI_H

#ifdef BPM_UUID_H_NOT_PRESET
#include <linux/mei_uuid.h>
#else
#include <linux/uuid.h>
#endif

#include_next <uapi/linux/mei.h>

#endif
