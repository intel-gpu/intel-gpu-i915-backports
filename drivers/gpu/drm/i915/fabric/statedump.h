/* SPDX-License-Identifier: MIT */
/*
 * Copyright(c) 2020 - 2021 Intel Corporation.
 *
 */

#ifndef STATEDUMP_H_INCLUDED
#define STATEDUMP_H_INCLUDED

#include <linux/dcache.h>

#include "iaf_drv.h"

void statedump_node_init(struct fsubdev *sd, struct dentry *sd_dir_node);

#endif
