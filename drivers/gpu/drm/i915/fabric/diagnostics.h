/* SPDX-License-Identifier: MIT */
/*
 * Copyright(c) 2021 Intel Corporation.
 *
 */

#ifndef DIAGNOSTICS_H_INCLUDED
#define DIAGNOSTICS_H_INCLUDED

#include <linux/dcache.h>
#include <linux/types.h>

#include "iaf_drv.h"

__printf(4, 5)
void print_diag(char *buf, size_t *buf_offset, size_t buf_size, const char *fmt, ...);
void diagnostics_port_node_init(struct fport *port, struct dentry *debugfs_dir);

#endif
