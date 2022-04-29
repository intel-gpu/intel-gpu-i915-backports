/* SPDX-License-Identifier: MIT */
/*
 * Copyright(c) 2020 Intel Corporation.
 *
 */

#ifndef MBOX_H_INCLUDED
#define MBOX_H_INCLUDED

#if IS_ENABLED(CPTCFG_IAF_DEBUG_MBOX_ACCESS)
int mbox_init_module(void);
void mbox_term_module(void);
#else
static inline int mbox_init_module(void) { return 0; }
static inline void mbox_term_module(void) {}
#endif

#endif
