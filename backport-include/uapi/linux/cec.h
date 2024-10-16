/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/*
 * cec - HDMI Consumer Electronics Control public header
 *
 * Copyright 2016 Cisco Systems, Inc. and/or its affiliates. All rights reserved.
 */

#ifndef _BACKPORT_CEC_UAPI_H
#define _BACKPORT_CEC_UAPI_H

#include_next <linux/cec.h>

#ifdef BPM_CEC_MSG_SET_AUDIO_VOLUME_LEVEL_NOT_PRESENT
#define CEC_MSG_SET_AUDIO_VOLUME_LEVEL 0x73
#endif

#endif
