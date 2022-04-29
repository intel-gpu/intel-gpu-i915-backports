/* SPDX-License-Identifier: MIT */
/*
 * Copyright(c) 2022 Intel Corporation.
 *
 */

#ifndef DEV_DIAG_H_INCLUDED
#define DEV_DIAG_H_INCLUDED

//#include <linux/dcache.h>
//#include <linux/types.h>

#include "iaf_drv.h"

void create_dev_debugfs_files(struct fsubdev *sd);

#endif
