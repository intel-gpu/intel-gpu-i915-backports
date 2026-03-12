/* SPDX-License-Identifier: MIT */
/*
 * Copyright(c) 2022 Intel Corporation.
 *
 */

#ifndef IAF_ERROR_H_INCLUDED
#define IAF_ERROR_H_INCLUDED

#include "iaf_drv.h"

const char *err_sts_str(size_t index);
u64 err_sts_read_viral(struct fsubdev *sd);
void err_sts_read_bridge_port_regs(struct fsubdev *sd, struct fport *port, u64 regs[]);
void reset_errors(struct fsubdev *sd);

#endif /* IAF_ERROR_H_INCLUDED */

