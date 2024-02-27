// SPDX-License-Identifier: MIT
/*
 * Copyright © 2020 Intel Corporation
 */

#include "i915_drv.h"
#include "intel_ddi.h"
#include "intel_ddi_buf_trans.h"
#include "intel_de.h"
#include "intel_display_types.h"
#include "intel_dp.h"
#include "intel_cx0_phy.h"

/* icl_combo_phy_trans */
static const union intel_ddi_buf_trans_entry _icl_combo_phy_trans_dp_hbr2_edp_hbr3[] = {
							/* NT mV Trans mV db    */
	{ .icl = { 0xA, 0x35, 0x3F, 0x00, 0x00 } },	/* 350   350      0.0   */
	{ .icl = { 0xA, 0x4F, 0x37, 0x00, 0x08 } },	/* 350   500      3.1   */
	{ .icl = { 0xC, 0x71, 0x2F, 0x00, 0x10 } },	/* 350   700      6.0   */
	{ .icl = { 0x6, 0x7F, 0x2B, 0x00, 0x14 } },	/* 350   900      8.2   */
	{ .icl = { 0xA, 0x4C, 0x3F, 0x00, 0x00 } },	/* 500   500      0.0   */
	{ .icl = { 0xC, 0x73, 0x34, 0x00, 0x0B } },	/* 500   700      2.9   */
	{ .icl = { 0x6, 0x7F, 0x2F, 0x00, 0x10 } },	/* 500   900      5.1   */
	{ .icl = { 0xC, 0x6C, 0x3C, 0x00, 0x03 } },	/* 650   700      0.6   */
	{ .icl = { 0x6, 0x7F, 0x35, 0x00, 0x0A } },	/* 600   900      3.5   */
	{ .icl = { 0x6, 0x7F, 0x3F, 0x00, 0x00 } },	/* 900   900      0.0   */
};

static const struct intel_ddi_buf_trans icl_combo_phy_trans_dp_hbr2_edp_hbr3 = {
	.entries = _icl_combo_phy_trans_dp_hbr2_edp_hbr3,
	.num_entries = ARRAY_SIZE(_icl_combo_phy_trans_dp_hbr2_edp_hbr3),
};

static const union intel_ddi_buf_trans_entry _icl_combo_phy_trans_edp_hbr2[] = {
							/* NT mV Trans mV db    */
	{ .icl = { 0x0, 0x7F, 0x3F, 0x00, 0x00 } },	/* 200   200      0.0   */
	{ .icl = { 0x8, 0x7F, 0x38, 0x00, 0x07 } },	/* 200   250      1.9   */
	{ .icl = { 0x1, 0x7F, 0x33, 0x00, 0x0C } },	/* 200   300      3.5   */
	{ .icl = { 0x9, 0x7F, 0x31, 0x00, 0x0E } },	/* 200   350      4.9   */
	{ .icl = { 0x8, 0x7F, 0x3F, 0x00, 0x00 } },	/* 250   250      0.0   */
	{ .icl = { 0x1, 0x7F, 0x38, 0x00, 0x07 } },	/* 250   300      1.6   */
	{ .icl = { 0x9, 0x7F, 0x35, 0x00, 0x0A } },	/* 250   350      2.9   */
	{ .icl = { 0x1, 0x7F, 0x3F, 0x00, 0x00 } },	/* 300   300      0.0   */
	{ .icl = { 0x9, 0x7F, 0x38, 0x00, 0x07 } },	/* 300   350      1.3   */
	{ .icl = { 0x9, 0x7F, 0x3F, 0x00, 0x00 } },	/* 350   350      0.0   */
};

static const struct intel_ddi_buf_trans icl_combo_phy_trans_edp_hbr2 = {
	.entries = _icl_combo_phy_trans_edp_hbr2,
	.num_entries = ARRAY_SIZE(_icl_combo_phy_trans_edp_hbr2),
};

static const union intel_ddi_buf_trans_entry _icl_combo_phy_trans_hdmi[] = {
							/* NT mV Trans mV db    */
	{ .icl = { 0xA, 0x60, 0x3F, 0x00, 0x00 } },	/* 450   450      0.0   */
	{ .icl = { 0xB, 0x73, 0x36, 0x00, 0x09 } },	/* 450   650      3.2   */
	{ .icl = { 0x6, 0x7F, 0x31, 0x00, 0x0E } },	/* 450   850      5.5   */
	{ .icl = { 0xB, 0x73, 0x3F, 0x00, 0x00 } },	/* 650   650      0.0   ALS */
	{ .icl = { 0x6, 0x7F, 0x37, 0x00, 0x08 } },	/* 650   850      2.3   */
	{ .icl = { 0x6, 0x7F, 0x3F, 0x00, 0x00 } },	/* 850   850      0.0   */
	{ .icl = { 0x6, 0x7F, 0x35, 0x00, 0x0A } },	/* 600   850      3.0   */
};

static const struct intel_ddi_buf_trans icl_combo_phy_trans_hdmi = {
	.entries = _icl_combo_phy_trans_hdmi,
	.num_entries = ARRAY_SIZE(_icl_combo_phy_trans_hdmi),
	.hdmi_default_entry = ARRAY_SIZE(_icl_combo_phy_trans_hdmi) - 1,
};

static const union intel_ddi_buf_trans_entry _dg1_combo_phy_trans_dp_rbr_hbr[] = {
							/* NT mV Trans mV db    */
	{ .icl = { 0xA, 0x32, 0x3F, 0x00, 0x00 } },	/* 350   350      0.0   */
	{ .icl = { 0xA, 0x48, 0x35, 0x00, 0x0A } },	/* 350   500      3.1   */
	{ .icl = { 0xC, 0x63, 0x2F, 0x00, 0x10 } },	/* 350   700      6.0   */
	{ .icl = { 0x6, 0x7F, 0x2C, 0x00, 0x13 } },	/* 350   900      8.2   */
	{ .icl = { 0xA, 0x43, 0x3F, 0x00, 0x00 } },	/* 500   500      0.0   */
	{ .icl = { 0xC, 0x60, 0x36, 0x00, 0x09 } },	/* 500   700      2.9   */
	{ .icl = { 0x6, 0x7F, 0x30, 0x00, 0x0F } },	/* 500   900      5.1   */
	{ .icl = { 0xC, 0x60, 0x3F, 0x00, 0x00 } },	/* 650   700      0.6   */
	{ .icl = { 0x6, 0x7F, 0x37, 0x00, 0x08 } },	/* 600   900      3.5   */
	{ .icl = { 0x6, 0x7F, 0x3F, 0x00, 0x00 } },	/* 900   900      0.0   */
};

static const struct intel_ddi_buf_trans dg1_combo_phy_trans_dp_rbr_hbr = {
	.entries = _dg1_combo_phy_trans_dp_rbr_hbr,
	.num_entries = ARRAY_SIZE(_dg1_combo_phy_trans_dp_rbr_hbr),
};

static const union intel_ddi_buf_trans_entry _dg1_combo_phy_trans_dp_hbr2_hbr3[] = {
							/* NT mV Trans mV db    */
	{ .icl = { 0xA, 0x32, 0x3F, 0x00, 0x00 } },	/* 350   350      0.0   */
	{ .icl = { 0xA, 0x48, 0x35, 0x00, 0x0A } },	/* 350   500      3.1   */
	{ .icl = { 0xC, 0x63, 0x2F, 0x00, 0x10 } },	/* 350   700      6.0   */
	{ .icl = { 0x6, 0x7F, 0x2C, 0x00, 0x13 } },	/* 350   900      8.2   */
	{ .icl = { 0xA, 0x43, 0x3F, 0x00, 0x00 } },	/* 500   500      0.0   */
	{ .icl = { 0xC, 0x60, 0x36, 0x00, 0x09 } },	/* 500   700      2.9   */
	{ .icl = { 0x6, 0x7F, 0x30, 0x00, 0x0F } },	/* 500   900      5.1   */
	{ .icl = { 0xC, 0x58, 0x3F, 0x00, 0x00 } },	/* 650   700      0.6   */
	{ .icl = { 0x6, 0x7F, 0x35, 0x00, 0x0A } },	/* 600   900      3.5   */
	{ .icl = { 0x6, 0x7F, 0x3F, 0x00, 0x00 } },	/* 900   900      0.0   */
};

static const struct intel_ddi_buf_trans dg1_combo_phy_trans_dp_hbr2_hbr3 = {
	.entries = _dg1_combo_phy_trans_dp_hbr2_hbr3,
	.num_entries = ARRAY_SIZE(_dg1_combo_phy_trans_dp_hbr2_hbr3),
};


static const union intel_ddi_buf_trans_entry _tgl_dkl_phy_trans_dp_hbr[] = {
					/* VS	pre-emp	Non-trans mV	Pre-emph dB */
	{ .dkl = { 0x7, 0x0, 0x00 } },	/* 0	0	400mV		0 dB */
	{ .dkl = { 0x5, 0x0, 0x05 } },	/* 0	1	400mV		3.5 dB */
	{ .dkl = { 0x2, 0x0, 0x0B } },	/* 0	2	400mV		6 dB */
	{ .dkl = { 0x0, 0x0, 0x18 } },	/* 0	3	400mV		9.5 dB */
	{ .dkl = { 0x5, 0x0, 0x00 } },	/* 1	0	600mV		0 dB */
	{ .dkl = { 0x2, 0x0, 0x08 } },	/* 1	1	600mV		3.5 dB */
	{ .dkl = { 0x0, 0x0, 0x14 } },	/* 1	2	600mV		6 dB */
	{ .dkl = { 0x2, 0x0, 0x00 } },	/* 2	0	800mV		0 dB */
	{ .dkl = { 0x0, 0x0, 0x0B } },	/* 2	1	800mV		3.5 dB */
	{ .dkl = { 0x0, 0x0, 0x00 } },	/* 3	0	1200mV		0 dB HDMI default */
};

static const struct intel_ddi_buf_trans tgl_dkl_phy_trans_dp_hbr = {
	.entries = _tgl_dkl_phy_trans_dp_hbr,
	.num_entries = ARRAY_SIZE(_tgl_dkl_phy_trans_dp_hbr),
};

static const union intel_ddi_buf_trans_entry _tgl_dkl_phy_trans_dp_hbr2[] = {
					/* VS	pre-emp	Non-trans mV	Pre-emph dB */
	{ .dkl = { 0x7, 0x0, 0x00 } },	/* 0	0	400mV		0 dB */
	{ .dkl = { 0x5, 0x0, 0x05 } },	/* 0	1	400mV		3.5 dB */
	{ .dkl = { 0x2, 0x0, 0x0B } },	/* 0	2	400mV		6 dB */
	{ .dkl = { 0x0, 0x0, 0x19 } },	/* 0	3	400mV		9.5 dB */
	{ .dkl = { 0x5, 0x0, 0x00 } },	/* 1	0	600mV		0 dB */
	{ .dkl = { 0x2, 0x0, 0x08 } },	/* 1	1	600mV		3.5 dB */
	{ .dkl = { 0x0, 0x0, 0x14 } },	/* 1	2	600mV		6 dB */
	{ .dkl = { 0x2, 0x0, 0x00 } },	/* 2	0	800mV		0 dB */
	{ .dkl = { 0x0, 0x0, 0x0B } },	/* 2	1	800mV		3.5 dB */
	{ .dkl = { 0x0, 0x0, 0x00 } },	/* 3	0	1200mV		0 dB HDMI default */
};

static const struct intel_ddi_buf_trans tgl_dkl_phy_trans_dp_hbr2 = {
	.entries = _tgl_dkl_phy_trans_dp_hbr2,
	.num_entries = ARRAY_SIZE(_tgl_dkl_phy_trans_dp_hbr2),
};

static const union intel_ddi_buf_trans_entry _tgl_dkl_phy_trans_hdmi[] = {
					/* HDMI Preset	VS	Pre-emph */
	{ .dkl = { 0x7, 0x0, 0x0 } },	/* 1		400mV	0dB */
	{ .dkl = { 0x6, 0x0, 0x0 } },	/* 2		500mV	0dB */
	{ .dkl = { 0x4, 0x0, 0x0 } },	/* 3		650mV	0dB */
	{ .dkl = { 0x2, 0x0, 0x0 } },	/* 4		800mV	0dB */
	{ .dkl = { 0x0, 0x0, 0x0 } },	/* 5		1000mV	0dB */
	{ .dkl = { 0x0, 0x0, 0x5 } },	/* 6		Full	-1.5 dB */
	{ .dkl = { 0x0, 0x0, 0x6 } },	/* 7		Full	-1.8 dB */
	{ .dkl = { 0x0, 0x0, 0x7 } },	/* 8		Full	-2 dB */
	{ .dkl = { 0x0, 0x0, 0x8 } },	/* 9		Full	-2.5 dB */
	{ .dkl = { 0x0, 0x0, 0xA } },	/* 10		Full	-3 dB */
};

static const struct intel_ddi_buf_trans tgl_dkl_phy_trans_hdmi = {
	.entries = _tgl_dkl_phy_trans_hdmi,
	.num_entries = ARRAY_SIZE(_tgl_dkl_phy_trans_hdmi),
	.hdmi_default_entry = ARRAY_SIZE(_tgl_dkl_phy_trans_hdmi) - 1,
};

static const union intel_ddi_buf_trans_entry _tgl_combo_phy_trans_dp_hbr[] = {
							/* NT mV Trans mV db    */
	{ .icl = { 0xA, 0x32, 0x3F, 0x00, 0x00 } },	/* 350   350      0.0   */
	{ .icl = { 0xA, 0x4F, 0x37, 0x00, 0x08 } },	/* 350   500      3.1   */
	{ .icl = { 0xC, 0x71, 0x2F, 0x00, 0x10 } },	/* 350   700      6.0   */
	{ .icl = { 0x6, 0x7D, 0x2B, 0x00, 0x14 } },	/* 350   900      8.2   */
	{ .icl = { 0xA, 0x4C, 0x3F, 0x00, 0x00 } },	/* 500   500      0.0   */
	{ .icl = { 0xC, 0x73, 0x34, 0x00, 0x0B } },	/* 500   700      2.9   */
	{ .icl = { 0x6, 0x7F, 0x2F, 0x00, 0x10 } },	/* 500   900      5.1   */
	{ .icl = { 0xC, 0x6C, 0x3C, 0x00, 0x03 } },	/* 650   700      0.6   */
	{ .icl = { 0x6, 0x7F, 0x35, 0x00, 0x0A } },	/* 600   900      3.5   */
	{ .icl = { 0x6, 0x7F, 0x3F, 0x00, 0x00 } },	/* 900   900      0.0   */
};

static const struct intel_ddi_buf_trans tgl_combo_phy_trans_dp_hbr = {
	.entries = _tgl_combo_phy_trans_dp_hbr,
	.num_entries = ARRAY_SIZE(_tgl_combo_phy_trans_dp_hbr),
};

static const union intel_ddi_buf_trans_entry _tgl_combo_phy_trans_dp_hbr2[] = {
							/* NT mV Trans mV db    */
	{ .icl = { 0xA, 0x35, 0x3F, 0x00, 0x00 } },	/* 350   350      0.0   */
	{ .icl = { 0xA, 0x4F, 0x37, 0x00, 0x08 } },	/* 350   500      3.1   */
	{ .icl = { 0xC, 0x63, 0x2F, 0x00, 0x10 } },	/* 350   700      6.0   */
	{ .icl = { 0x6, 0x7F, 0x2B, 0x00, 0x14 } },	/* 350   900      8.2   */
	{ .icl = { 0xA, 0x47, 0x3F, 0x00, 0x00 } },	/* 500   500      0.0   */
	{ .icl = { 0xC, 0x63, 0x34, 0x00, 0x0B } },	/* 500   700      2.9   */
	{ .icl = { 0x6, 0x7F, 0x2F, 0x00, 0x10 } },	/* 500   900      5.1   */
	{ .icl = { 0xC, 0x61, 0x3C, 0x00, 0x03 } },	/* 650   700      0.6   */
	{ .icl = { 0x6, 0x7B, 0x35, 0x00, 0x0A } },	/* 600   900      3.5   */
	{ .icl = { 0x6, 0x7F, 0x3F, 0x00, 0x00 } },	/* 900   900      0.0   */
};

static const struct intel_ddi_buf_trans tgl_combo_phy_trans_dp_hbr2 = {
	.entries = _tgl_combo_phy_trans_dp_hbr2,
	.num_entries = ARRAY_SIZE(_tgl_combo_phy_trans_dp_hbr2),
};

static const union intel_ddi_buf_trans_entry _tgl_uy_combo_phy_trans_dp_hbr2[] = {
							/* NT mV Trans mV db    */
	{ .icl = { 0xA, 0x35, 0x3F, 0x00, 0x00 } },	/* 350   350      0.0   */
	{ .icl = { 0xA, 0x4F, 0x36, 0x00, 0x09 } },	/* 350   500      3.1   */
	{ .icl = { 0xC, 0x60, 0x32, 0x00, 0x0D } },	/* 350   700      6.0   */
	{ .icl = { 0xC, 0x7F, 0x2D, 0x00, 0x12 } },	/* 350   900      8.2   */
	{ .icl = { 0xC, 0x47, 0x3F, 0x00, 0x00 } },	/* 500   500      0.0   */
	{ .icl = { 0xC, 0x6F, 0x36, 0x00, 0x09 } },	/* 500   700      2.9   */
	{ .icl = { 0x6, 0x7D, 0x32, 0x00, 0x0D } },	/* 500   900      5.1   */
	{ .icl = { 0x6, 0x60, 0x3C, 0x00, 0x03 } },	/* 650   700      0.6   */
	{ .icl = { 0x6, 0x7F, 0x34, 0x00, 0x0B } },	/* 600   900      3.5   */
	{ .icl = { 0x6, 0x7F, 0x3F, 0x00, 0x00 } },	/* 900   900      0.0   */
};

static const struct intel_ddi_buf_trans tgl_uy_combo_phy_trans_dp_hbr2 = {
	.entries = _tgl_uy_combo_phy_trans_dp_hbr2,
	.num_entries = ARRAY_SIZE(_tgl_uy_combo_phy_trans_dp_hbr2),
};

/*
 * Cloned the HOBL entry to comply with the voltage and pre-emphasis entries
 * that DisplayPort specification requires
 */
static const union intel_ddi_buf_trans_entry _tgl_combo_phy_trans_edp_hbr2_hobl[] = {
							/* VS	pre-emp	*/
	{ .icl = { 0x6, 0x7F, 0x3F, 0x00, 0x00 } },	/* 0	0	*/
	{ .icl = { 0x6, 0x7F, 0x3F, 0x00, 0x00 } },	/* 0	1	*/
	{ .icl = { 0x6, 0x7F, 0x3F, 0x00, 0x00 } },	/* 0	2	*/
	{ .icl = { 0x6, 0x7F, 0x3F, 0x00, 0x00 } },	/* 0	3	*/
	{ .icl = { 0x6, 0x7F, 0x3F, 0x00, 0x00 } },	/* 1	0	*/
	{ .icl = { 0x6, 0x7F, 0x3F, 0x00, 0x00 } },	/* 1	1	*/
	{ .icl = { 0x6, 0x7F, 0x3F, 0x00, 0x00 } },	/* 1	2	*/
	{ .icl = { 0x6, 0x7F, 0x3F, 0x00, 0x00 } },	/* 2	0	*/
	{ .icl = { 0x6, 0x7F, 0x3F, 0x00, 0x00 } },	/* 2	1	*/
};

static const struct intel_ddi_buf_trans tgl_combo_phy_trans_edp_hbr2_hobl = {
	.entries = _tgl_combo_phy_trans_edp_hbr2_hobl,
	.num_entries = ARRAY_SIZE(_tgl_combo_phy_trans_edp_hbr2_hobl),
};

static const union intel_ddi_buf_trans_entry _rkl_combo_phy_trans_dp_hbr[] = {
							/* NT mV Trans mV db    */
	{ .icl = { 0xA, 0x2F, 0x3F, 0x00, 0x00 } },	/* 350   350      0.0   */
	{ .icl = { 0xA, 0x4F, 0x37, 0x00, 0x08 } },	/* 350   500      3.1   */
	{ .icl = { 0xC, 0x63, 0x2F, 0x00, 0x10 } },	/* 350   700      6.0   */
	{ .icl = { 0x6, 0x7D, 0x2A, 0x00, 0x15 } },	/* 350   900      8.2   */
	{ .icl = { 0xA, 0x4C, 0x3F, 0x00, 0x00 } },	/* 500   500      0.0   */
	{ .icl = { 0xC, 0x73, 0x34, 0x00, 0x0B } },	/* 500   700      2.9   */
	{ .icl = { 0x6, 0x7F, 0x2F, 0x00, 0x10 } },	/* 500   900      5.1   */
	{ .icl = { 0xC, 0x6E, 0x3E, 0x00, 0x01 } },	/* 650   700      0.6   */
	{ .icl = { 0x6, 0x7F, 0x35, 0x00, 0x0A } },	/* 600   900      3.5   */
	{ .icl = { 0x6, 0x7F, 0x3F, 0x00, 0x00 } },	/* 900   900      0.0   */
};

static const struct intel_ddi_buf_trans rkl_combo_phy_trans_dp_hbr = {
	.entries = _rkl_combo_phy_trans_dp_hbr,
	.num_entries = ARRAY_SIZE(_rkl_combo_phy_trans_dp_hbr),
};

static const union intel_ddi_buf_trans_entry _rkl_combo_phy_trans_dp_hbr2_hbr3[] = {
							/* NT mV Trans mV db    */
	{ .icl = { 0xA, 0x35, 0x3F, 0x00, 0x00 } },	/* 350   350      0.0   */
	{ .icl = { 0xA, 0x50, 0x38, 0x00, 0x07 } },	/* 350   500      3.1   */
	{ .icl = { 0xC, 0x61, 0x33, 0x00, 0x0C } },	/* 350   700      6.0   */
	{ .icl = { 0x6, 0x7F, 0x2E, 0x00, 0x11 } },	/* 350   900      8.2   */
	{ .icl = { 0xA, 0x47, 0x3F, 0x00, 0x00 } },	/* 500   500      0.0   */
	{ .icl = { 0xC, 0x5F, 0x38, 0x00, 0x07 } },	/* 500   700      2.9   */
	{ .icl = { 0x6, 0x7F, 0x2F, 0x00, 0x10 } },	/* 500   900      5.1   */
	{ .icl = { 0xC, 0x5F, 0x3F, 0x00, 0x00 } },	/* 650   700      0.6   */
	{ .icl = { 0x6, 0x7E, 0x36, 0x00, 0x09 } },	/* 600   900      3.5   */
	{ .icl = { 0x6, 0x7F, 0x3F, 0x00, 0x00 } },	/* 900   900      0.0   */
};

static const struct intel_ddi_buf_trans rkl_combo_phy_trans_dp_hbr2_hbr3 = {
	.entries = _rkl_combo_phy_trans_dp_hbr2_hbr3,
	.num_entries = ARRAY_SIZE(_rkl_combo_phy_trans_dp_hbr2_hbr3),
};

static const union intel_ddi_buf_trans_entry _adls_combo_phy_trans_dp_hbr2_hbr3[] = {
							/* NT mV Trans mV db    */
	{ .icl = { 0xA, 0x35, 0x3F, 0x00, 0x00 } },	/* 350   350      0.0   */
	{ .icl = { 0xA, 0x4F, 0x37, 0x00, 0x08 } },	/* 350   500      3.1   */
	{ .icl = { 0xC, 0x63, 0x31, 0x00, 0x0E } },	/* 350   700      6.0   */
	{ .icl = { 0x6, 0x7F, 0x2C, 0x00, 0x13 } },	/* 350   900      8.2   */
	{ .icl = { 0xA, 0x47, 0x3F, 0x00, 0x00 } },	/* 500   500      0.0   */
	{ .icl = { 0xC, 0x63, 0x37, 0x00, 0x08 } },	/* 500   700      2.9   */
	{ .icl = { 0x6, 0x73, 0x32, 0x00, 0x0D } },	/* 500   900      5.1   */
	{ .icl = { 0xC, 0x58, 0x3F, 0x00, 0x00 } },	/* 650   700      0.6   */
	{ .icl = { 0x6, 0x7F, 0x35, 0x00, 0x0A } },	/* 600   900      3.5   */
	{ .icl = { 0x6, 0x7F, 0x3F, 0x00, 0x00 } },	/* 900   900      0.0   */
};

static const struct intel_ddi_buf_trans adls_combo_phy_trans_dp_hbr2_hbr3 = {
	.entries = _adls_combo_phy_trans_dp_hbr2_hbr3,
	.num_entries = ARRAY_SIZE(_adls_combo_phy_trans_dp_hbr2_hbr3),
};

static const union intel_ddi_buf_trans_entry _adls_combo_phy_trans_edp_hbr2[] = {
							/* NT mV Trans mV db    */
	{ .icl = { 0x9, 0x73, 0x3D, 0x00, 0x02 } },	/* 200   200      0.0   */
	{ .icl = { 0x9, 0x7A, 0x3C, 0x00, 0x03 } },	/* 200   250      1.9   */
	{ .icl = { 0x9, 0x7F, 0x3B, 0x00, 0x04 } },	/* 200   300      3.5   */
	{ .icl = { 0x4, 0x6C, 0x33, 0x00, 0x0C } },	/* 200   350      4.9   */
	{ .icl = { 0x2, 0x73, 0x3A, 0x00, 0x05 } },	/* 250   250      0.0   */
	{ .icl = { 0x2, 0x7C, 0x38, 0x00, 0x07 } },	/* 250   300      1.6   */
	{ .icl = { 0x4, 0x5A, 0x36, 0x00, 0x09 } },	/* 250   350      2.9   */
	{ .icl = { 0x4, 0x57, 0x3D, 0x00, 0x02 } },	/* 300   300      0.0   */
	{ .icl = { 0x4, 0x65, 0x38, 0x00, 0x07 } },	/* 300   350      1.3   */
	{ .icl = { 0x4, 0x6C, 0x3A, 0x00, 0x05 } },	/* 350   350      0.0   */
};

static const struct intel_ddi_buf_trans adls_combo_phy_trans_edp_hbr2 = {
	.entries = _adls_combo_phy_trans_edp_hbr2,
	.num_entries = ARRAY_SIZE(_adls_combo_phy_trans_edp_hbr2),
};

static const union intel_ddi_buf_trans_entry _adls_combo_phy_trans_edp_hbr3[] = {
							/* NT mV Trans mV db    */
	{ .icl = { 0xA, 0x35, 0x3F, 0x00, 0x00 } },	/* 350   350      0.0   */
	{ .icl = { 0xA, 0x4F, 0x37, 0x00, 0x08 } },	/* 350   500      3.1   */
	{ .icl = { 0xC, 0x63, 0x31, 0x00, 0x0E } },	/* 350   700      6.0   */
	{ .icl = { 0x6, 0x7F, 0x2C, 0x00, 0x13 } },	/* 350   900      8.2   */
	{ .icl = { 0xA, 0x47, 0x3F, 0x00, 0x00 } },	/* 500   500      0.0   */
	{ .icl = { 0xC, 0x63, 0x37, 0x00, 0x08 } },	/* 500   700      2.9   */
	{ .icl = { 0x6, 0x73, 0x32, 0x00, 0x0D } },	/* 500   900      5.1   */
	{ .icl = { 0xC, 0x58, 0x3F, 0x00, 0x00 } },	/* 650   700      0.6   */
	{ .icl = { 0x6, 0x7F, 0x35, 0x00, 0x0A } },	/* 600   900      3.5   */
	{ .icl = { 0x6, 0x7F, 0x3F, 0x00, 0x00 } },	/* 900   900      0.0   */
};

static const struct intel_ddi_buf_trans adls_combo_phy_trans_edp_hbr3 = {
	.entries = _adls_combo_phy_trans_edp_hbr3,
	.num_entries = ARRAY_SIZE(_adls_combo_phy_trans_edp_hbr3),
};

static const union intel_ddi_buf_trans_entry _adlp_combo_phy_trans_dp_hbr[] = {
							/* NT mV Trans mV db    */
	{ .icl = { 0xA, 0x35, 0x3F, 0x00, 0x00 } },	/* 350   350      0.0   */
	{ .icl = { 0xA, 0x4F, 0x37, 0x00, 0x08 } },	/* 350   500      3.1   */
	{ .icl = { 0xC, 0x71, 0x31, 0x00, 0x0E } },	/* 350   700      6.0   */
	{ .icl = { 0x6, 0x7F, 0x2C, 0x00, 0x13 } },	/* 350   900      8.2   */
	{ .icl = { 0xA, 0x4C, 0x3F, 0x00, 0x00 } },	/* 500   500      0.0   */
	{ .icl = { 0xC, 0x73, 0x34, 0x00, 0x0B } },	/* 500   700      2.9   */
	{ .icl = { 0x6, 0x7F, 0x2F, 0x00, 0x10 } },	/* 500   900      5.1   */
	{ .icl = { 0xC, 0x7C, 0x3C, 0x00, 0x03 } },	/* 650   700      0.6   */
	{ .icl = { 0x6, 0x7F, 0x35, 0x00, 0x0A } },	/* 600   900      3.5   */
	{ .icl = { 0x6, 0x7F, 0x3F, 0x00, 0x00 } },	/* 900   900      0.0   */
};

static const struct intel_ddi_buf_trans adlp_combo_phy_trans_dp_hbr = {
	.entries = _adlp_combo_phy_trans_dp_hbr,
	.num_entries = ARRAY_SIZE(_adlp_combo_phy_trans_dp_hbr),
};

static const union intel_ddi_buf_trans_entry _adlp_combo_phy_trans_dp_hbr2_hbr3[] = {
							/* NT mV Trans mV db    */
	{ .icl = { 0xA, 0x35, 0x3F, 0x00, 0x00 } },	/* 350   350      0.0   */
	{ .icl = { 0xA, 0x4F, 0x37, 0x00, 0x08 } },	/* 350   500      3.1   */
	{ .icl = { 0xC, 0x71, 0x30, 0x00, 0x0F } },	/* 350   700      6.0   */
	{ .icl = { 0x6, 0x7F, 0x2B, 0x00, 0x14 } },	/* 350   900      8.2   */
	{ .icl = { 0xA, 0x4C, 0x3F, 0x00, 0x00 } },	/* 500   500      0.0   */
	{ .icl = { 0xC, 0x73, 0x34, 0x00, 0x0B } },	/* 500   700      2.9   */
	{ .icl = { 0x6, 0x7F, 0x30, 0x00, 0x0F } },	/* 500   900      5.1   */
	{ .icl = { 0xC, 0x63, 0x3F, 0x00, 0x00 } },	/* 650   700      0.6   */
	{ .icl = { 0x6, 0x7F, 0x38, 0x00, 0x07 } },	/* 600   900      3.5   */
	{ .icl = { 0x6, 0x7F, 0x3F, 0x00, 0x00 } },	/* 900   900      0.0   */
};

static const union intel_ddi_buf_trans_entry _adlp_combo_phy_trans_edp_hbr2[] = {
							/* NT mV Trans mV db    */
	{ .icl = { 0x4, 0x50, 0x38, 0x00, 0x07 } },	/* 200   200      0.0   */
	{ .icl = { 0x4, 0x58, 0x35, 0x00, 0x0A } },	/* 200   250      1.9   */
	{ .icl = { 0x4, 0x60, 0x34, 0x00, 0x0B } },	/* 200   300      3.5   */
	{ .icl = { 0x4, 0x6A, 0x32, 0x00, 0x0D } },	/* 200   350      4.9   */
	{ .icl = { 0x4, 0x5E, 0x38, 0x00, 0x07 } },	/* 250   250      0.0   */
	{ .icl = { 0x4, 0x61, 0x36, 0x00, 0x09 } },	/* 250   300      1.6   */
	{ .icl = { 0x4, 0x6B, 0x34, 0x00, 0x0B } },	/* 250   350      2.9   */
	{ .icl = { 0x4, 0x69, 0x39, 0x00, 0x06 } },	/* 300   300      0.0   */
	{ .icl = { 0x4, 0x73, 0x37, 0x00, 0x08 } },	/* 300   350      1.3   */
	{ .icl = { 0x4, 0x7A, 0x38, 0x00, 0x07 } },	/* 350   350      0.0   */
};

static const union intel_ddi_buf_trans_entry _adlp_combo_phy_trans_dp_hbr2_edp_hbr3[] = {
							/* NT mV Trans mV db    */
	{ .icl = { 0xA, 0x35, 0x3F, 0x00, 0x00 } },	/* 350   350      0.0   */
	{ .icl = { 0xA, 0x4F, 0x37, 0x00, 0x08 } },	/* 350   500      3.1   */
	{ .icl = { 0xC, 0x71, 0x30, 0x00, 0x0f } },	/* 350   700      6.0   */
	{ .icl = { 0x6, 0x7F, 0x2B, 0x00, 0x14 } },	/* 350   900      8.2   */
	{ .icl = { 0xA, 0x4C, 0x3F, 0x00, 0x00 } },	/* 500   500      0.0   */
	{ .icl = { 0xC, 0x73, 0x34, 0x00, 0x0B } },	/* 500   700      2.9   */
	{ .icl = { 0x6, 0x7F, 0x30, 0x00, 0x0F } },	/* 500   900      5.1   */
	{ .icl = { 0xC, 0x63, 0x3F, 0x00, 0x00 } },	/* 650   700      0.6   */
	{ .icl = { 0x6, 0x7F, 0x38, 0x00, 0x07 } },	/* 600   900      3.5   */
	{ .icl = { 0x6, 0x7F, 0x3F, 0x00, 0x00 } },	/* 900   900      0.0   */
};

static const struct intel_ddi_buf_trans adlp_combo_phy_trans_dp_hbr2_hbr3 = {
	.entries = _adlp_combo_phy_trans_dp_hbr2_hbr3,
	.num_entries = ARRAY_SIZE(_adlp_combo_phy_trans_dp_hbr2_hbr3),
};

static const struct intel_ddi_buf_trans adlp_combo_phy_trans_edp_hbr3 = {
	.entries = _adlp_combo_phy_trans_dp_hbr2_edp_hbr3,
	.num_entries = ARRAY_SIZE(_adlp_combo_phy_trans_dp_hbr2_edp_hbr3),
};

static const struct intel_ddi_buf_trans adlp_combo_phy_trans_edp_up_to_hbr2 = {
	.entries = _adlp_combo_phy_trans_edp_hbr2,
	.num_entries = ARRAY_SIZE(_adlp_combo_phy_trans_edp_hbr2),
};

static const union intel_ddi_buf_trans_entry _adlp_dkl_phy_trans_dp_hbr[] = {
					/* VS	pre-emp	Non-trans mV	Pre-emph dB */
	{ .dkl = { 0x7, 0x0, 0x01 } },	/* 0	0	400mV		0 dB */
	{ .dkl = { 0x5, 0x0, 0x06 } },	/* 0	1	400mV		3.5 dB */
	{ .dkl = { 0x2, 0x0, 0x0B } },	/* 0	2	400mV		6 dB */
	{ .dkl = { 0x0, 0x0, 0x17 } },	/* 0	3	400mV		9.5 dB */
	{ .dkl = { 0x5, 0x0, 0x00 } },	/* 1	0	600mV		0 dB */
	{ .dkl = { 0x2, 0x0, 0x08 } },	/* 1	1	600mV		3.5 dB */
	{ .dkl = { 0x0, 0x0, 0x14 } },	/* 1	2	600mV		6 dB */
	{ .dkl = { 0x2, 0x0, 0x00 } },	/* 2	0	800mV		0 dB */
	{ .dkl = { 0x0, 0x0, 0x0B } },	/* 2	1	800mV		3.5 dB */
	{ .dkl = { 0x0, 0x0, 0x00 } },	/* 3	0	1200mV		0 dB */
};

static const struct intel_ddi_buf_trans adlp_dkl_phy_trans_dp_hbr = {
	.entries = _adlp_dkl_phy_trans_dp_hbr,
	.num_entries = ARRAY_SIZE(_adlp_dkl_phy_trans_dp_hbr),
};

static const union intel_ddi_buf_trans_entry _adlp_dkl_phy_trans_dp_hbr2_hbr3[] = {
					/* VS	pre-emp	Non-trans mV	Pre-emph dB */
	{ .dkl = { 0x7, 0x0, 0x00 } },	/* 0	0	400mV		0 dB */
	{ .dkl = { 0x5, 0x0, 0x04 } },	/* 0	1	400mV		3.5 dB */
	{ .dkl = { 0x2, 0x0, 0x0A } },	/* 0	2	400mV		6 dB */
	{ .dkl = { 0x0, 0x0, 0x18 } },	/* 0	3	400mV		9.5 dB */
	{ .dkl = { 0x5, 0x0, 0x00 } },	/* 1	0	600mV		0 dB */
	{ .dkl = { 0x2, 0x0, 0x06 } },	/* 1	1	600mV		3.5 dB */
	{ .dkl = { 0x0, 0x0, 0x14 } },	/* 1	2	600mV		6 dB */
	{ .dkl = { 0x2, 0x0, 0x00 } },	/* 2	0	800mV		0 dB */
	{ .dkl = { 0x0, 0x0, 0x09 } },	/* 2	1	800mV		3.5 dB */
	{ .dkl = { 0x0, 0x0, 0x00 } },	/* 3	0	1200mV		0 dB */
};

static const struct intel_ddi_buf_trans adlp_dkl_phy_trans_dp_hbr2_hbr3 = {
	.entries = _adlp_dkl_phy_trans_dp_hbr2_hbr3,
	.num_entries = ARRAY_SIZE(_adlp_dkl_phy_trans_dp_hbr2_hbr3),
};

static const union intel_ddi_buf_trans_entry _dg2_snps_trans[] = {
	{ .snps = { 25, 0, 0 } },	/* VS 0, pre-emph 0 */
	{ .snps = { 32, 0, 6 } },	/* VS 0, pre-emph 1 */
	{ .snps = { 35, 0, 10 } },	/* VS 0, pre-emph 2 */
	{ .snps = { 43, 0, 17 } },	/* VS 0, pre-emph 3 */
	{ .snps = { 35, 0, 0 } },	/* VS 1, pre-emph 0 */
	{ .snps = { 45, 0, 8 } },	/* VS 1, pre-emph 1 */
	{ .snps = { 48, 0, 14 } },	/* VS 1, pre-emph 2 */
	{ .snps = { 47, 0, 0 } },	/* VS 2, pre-emph 0 */
	{ .snps = { 55, 0, 7 } },	/* VS 2, pre-emph 1 */
	{ .snps = { 62, 0, 0 } },	/* VS 3, pre-emph 0 */
};

static const struct intel_ddi_buf_trans dg2_snps_trans = {
	.entries = _dg2_snps_trans,
	.num_entries = ARRAY_SIZE(_dg2_snps_trans),
	.hdmi_default_entry = ARRAY_SIZE(_dg2_snps_trans) - 1,
};

static const union intel_ddi_buf_trans_entry _dg2_snps_trans_uhbr[] = {
	{ .snps = { 62, 0, 0 } },	/* preset 0 */
	{ .snps = { 55, 0, 7 } },	/* preset 1 */
	{ .snps = { 50, 0, 12 } },	/* preset 2 */
	{ .snps = { 44, 0, 18 } },	/* preset 3 */
	{ .snps = { 35, 0, 21 } },	/* preset 4 */
	{ .snps = { 59, 3, 0 } },	/* preset 5 */
	{ .snps = { 53, 3, 6 } },	/* preset 6 */
	{ .snps = { 48, 3, 11 } },	/* preset 7 */
	{ .snps = { 42, 5, 15 } },	/* preset 8 */
	{ .snps = { 37, 5, 20 } },	/* preset 9 */
	{ .snps = { 56, 6, 0 } },	/* preset 10 */
	{ .snps = { 48, 7, 7 } },	/* preset 11 */
	{ .snps = { 45, 7, 10 } },	/* preset 12 */
	{ .snps = { 39, 8, 15 } },	/* preset 13 */
	{ .snps = { 48, 14, 0 } },	/* preset 14 */
	{ .snps = { 45, 4, 4 } },	/* preset 15 */
};

static const struct intel_ddi_buf_trans dg2_snps_trans_uhbr = {
	.entries = _dg2_snps_trans_uhbr,
	.num_entries = ARRAY_SIZE(_dg2_snps_trans_uhbr),
};

static const union intel_ddi_buf_trans_entry _mtl_c10_trans_dp14[] = {
	{ .snps = { 26, 0, 0  } },      /* preset 0 */
	{ .snps = { 33, 0, 6  } },      /* preset 1 */
	{ .snps = { 38, 0, 11 } },      /* preset 2 */
	{ .snps = { 43, 0, 19 } },      /* preset 3 */
	{ .snps = { 39, 0, 0  } },      /* preset 4 */
	{ .snps = { 45, 0, 7  } },      /* preset 5 */
	{ .snps = { 46, 0, 13 } },      /* preset 6 */
	{ .snps = { 46, 0, 0  } },      /* preset 7 */
	{ .snps = { 55, 0, 7  } },      /* preset 8 */
	{ .snps = { 62, 0, 0  } },      /* preset 9 */
};

static const struct intel_ddi_buf_trans mtl_cx0_trans = {
	.entries = _mtl_c10_trans_dp14,
	.num_entries = ARRAY_SIZE(_mtl_c10_trans_dp14),
	.hdmi_default_entry = ARRAY_SIZE(_mtl_c10_trans_dp14) - 1,
};

/* DP2.0 */
static const union intel_ddi_buf_trans_entry _mtl_c20_trans_uhbr[] = {
	{ .snps = { 48, 0, 0 } },       /* preset 0 */
	{ .snps = { 43, 0, 5 } },       /* preset 1 */
	{ .snps = { 40, 0, 8 } },       /* preset 2 */
	{ .snps = { 37, 0, 11 } },      /* preset 3 */
	{ .snps = { 33, 0, 15 } },      /* preset 4 */
	{ .snps = { 46, 2, 0 } },       /* preset 5 */
	{ .snps = { 42, 2, 4 } },       /* preset 6 */
	{ .snps = { 38, 2, 8 } },       /* preset 7 */
	{ .snps = { 35, 2, 11 } },      /* preset 8 */
	{ .snps = { 33, 2, 13 } },      /* preset 9 */
	{ .snps = { 44, 4, 0 } },       /* preset 10 */
	{ .snps = { 40, 4, 4 } },       /* preset 11 */
	{ .snps = { 37, 4, 7 } },       /* preset 12 */
	{ .snps = { 33, 4, 11 } },      /* preset 13 */
	{ .snps = { 40, 8, 0 } },	/* preset 14 */
	{ .snps = { 28, 2, 2 } },	/* preset 15 */
};

/* HDMI2.0 */
static const union intel_ddi_buf_trans_entry _mtl_c20_trans_hdmi[] = {
	{ .snps = { 48, 0, 0 } },       /* preset 0 */
	{ .snps = { 38, 4, 6 } },       /* preset 1 */
	{ .snps = { 36, 4, 8 } },       /* preset 2 */
	{ .snps = { 34, 4, 10 } },      /* preset 3 */
	{ .snps = { 32, 4, 12 } },      /* preset 4 */
};

static const struct intel_ddi_buf_trans mtl_c20_trans_hdmi = {
	.entries = _mtl_c20_trans_hdmi,
	.num_entries = ARRAY_SIZE(_mtl_c20_trans_hdmi),
	.hdmi_default_entry = 0,
};

static const struct intel_ddi_buf_trans mtl_c20_trans_uhbr = {
	.entries = _mtl_c20_trans_uhbr,
	.num_entries = ARRAY_SIZE(_mtl_c20_trans_uhbr),
};

bool is_hobl_buf_trans(const struct intel_ddi_buf_trans *table)
{
	return table == &tgl_combo_phy_trans_edp_hbr2_hobl;
}

static bool use_edp_hobl(struct intel_encoder *encoder)
{
	struct intel_dp *intel_dp = enc_to_intel_dp(encoder);
	struct intel_connector *connector = intel_dp->attached_connector;

	return connector->panel.vbt.edp.hobl && !intel_dp->hobl_failed;
}

static bool use_edp_low_vswing(struct intel_encoder *encoder)
{
	struct intel_dp *intel_dp = enc_to_intel_dp(encoder);
	struct intel_connector *connector = intel_dp->attached_connector;

	return connector->panel.vbt.edp.low_vswing;
}

static const struct intel_ddi_buf_trans *
intel_get_buf_trans(const struct intel_ddi_buf_trans *trans, int *num_entries)
{
	*num_entries = trans->num_entries;
	return trans;
}

static const struct intel_ddi_buf_trans *
tgl_get_combo_buf_trans_dp(struct intel_encoder *encoder,
			   const struct intel_crtc_state *crtc_state,
			   int *n_entries)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);

	if (crtc_state->port_clock > 270000) {
		if (IS_TGL_UY(dev_priv)) {
			return intel_get_buf_trans(&tgl_uy_combo_phy_trans_dp_hbr2,
						   n_entries);
		} else {
			return intel_get_buf_trans(&tgl_combo_phy_trans_dp_hbr2,
						   n_entries);
		}
	} else {
		return intel_get_buf_trans(&tgl_combo_phy_trans_dp_hbr,
					   n_entries);
	}
}

static const struct intel_ddi_buf_trans *
tgl_get_combo_buf_trans_edp(struct intel_encoder *encoder,
			    const struct intel_crtc_state *crtc_state,
			    int *n_entries)
{
	if (crtc_state->port_clock > 540000) {
		return intel_get_buf_trans(&icl_combo_phy_trans_dp_hbr2_edp_hbr3,
					   n_entries);
	} else if (use_edp_hobl(encoder)) {
		return intel_get_buf_trans(&tgl_combo_phy_trans_edp_hbr2_hobl,
					   n_entries);
	} else if (use_edp_low_vswing(encoder)) {
		return intel_get_buf_trans(&icl_combo_phy_trans_edp_hbr2,
					   n_entries);
	}

	return tgl_get_combo_buf_trans_dp(encoder, crtc_state, n_entries);
}

static const struct intel_ddi_buf_trans *
tgl_get_combo_buf_trans(struct intel_encoder *encoder,
			const struct intel_crtc_state *crtc_state,
			int *n_entries)
{
	if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_HDMI))
		return intel_get_buf_trans(&icl_combo_phy_trans_hdmi, n_entries);
	else if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_EDP))
		return tgl_get_combo_buf_trans_edp(encoder, crtc_state, n_entries);
	else
		return tgl_get_combo_buf_trans_dp(encoder, crtc_state, n_entries);
}

static const struct intel_ddi_buf_trans *
dg1_get_combo_buf_trans_dp(struct intel_encoder *encoder,
			   const struct intel_crtc_state *crtc_state,
			   int *n_entries)
{
	if (crtc_state->port_clock > 270000)
		return intel_get_buf_trans(&dg1_combo_phy_trans_dp_hbr2_hbr3,
					   n_entries);
	else
		return intel_get_buf_trans(&dg1_combo_phy_trans_dp_rbr_hbr,
					   n_entries);
}

static const struct intel_ddi_buf_trans *
dg1_get_combo_buf_trans_edp(struct intel_encoder *encoder,
			    const struct intel_crtc_state *crtc_state,
			    int *n_entries)
{
	if (crtc_state->port_clock > 540000)
		return intel_get_buf_trans(&icl_combo_phy_trans_dp_hbr2_edp_hbr3,
					   n_entries);
	else if (use_edp_hobl(encoder))
		return intel_get_buf_trans(&tgl_combo_phy_trans_edp_hbr2_hobl,
					   n_entries);
	else if (use_edp_low_vswing(encoder))
		return intel_get_buf_trans(&icl_combo_phy_trans_edp_hbr2,
					   n_entries);
	else
		return dg1_get_combo_buf_trans_dp(encoder, crtc_state, n_entries);
}

static const struct intel_ddi_buf_trans *
dg1_get_combo_buf_trans(struct intel_encoder *encoder,
			const struct intel_crtc_state *crtc_state,
			int *n_entries)
{
	if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_HDMI))
		return intel_get_buf_trans(&icl_combo_phy_trans_hdmi, n_entries);
	else if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_EDP))
		return dg1_get_combo_buf_trans_edp(encoder, crtc_state, n_entries);
	else
		return dg1_get_combo_buf_trans_dp(encoder, crtc_state, n_entries);
}

static const struct intel_ddi_buf_trans *
rkl_get_combo_buf_trans_dp(struct intel_encoder *encoder,
			   const struct intel_crtc_state *crtc_state,
			   int *n_entries)
{
	if (crtc_state->port_clock > 270000)
		return intel_get_buf_trans(&rkl_combo_phy_trans_dp_hbr2_hbr3, n_entries);
	else
		return intel_get_buf_trans(&rkl_combo_phy_trans_dp_hbr, n_entries);
}

static const struct intel_ddi_buf_trans *
rkl_get_combo_buf_trans_edp(struct intel_encoder *encoder,
			    const struct intel_crtc_state *crtc_state,
			    int *n_entries)
{
	if (crtc_state->port_clock > 540000) {
		return intel_get_buf_trans(&icl_combo_phy_trans_dp_hbr2_edp_hbr3,
					   n_entries);
	} else if (use_edp_hobl(encoder)) {
		return intel_get_buf_trans(&tgl_combo_phy_trans_edp_hbr2_hobl,
					   n_entries);
	} else if (use_edp_low_vswing(encoder)) {
		return intel_get_buf_trans(&icl_combo_phy_trans_edp_hbr2,
					   n_entries);
	}

	return rkl_get_combo_buf_trans_dp(encoder, crtc_state, n_entries);
}

static const struct intel_ddi_buf_trans *
rkl_get_combo_buf_trans(struct intel_encoder *encoder,
			const struct intel_crtc_state *crtc_state,
			int *n_entries)
{
	if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_HDMI))
		return intel_get_buf_trans(&icl_combo_phy_trans_hdmi, n_entries);
	else if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_EDP))
		return rkl_get_combo_buf_trans_edp(encoder, crtc_state, n_entries);
	else
		return rkl_get_combo_buf_trans_dp(encoder, crtc_state, n_entries);
}

static const struct intel_ddi_buf_trans *
adls_get_combo_buf_trans_dp(struct intel_encoder *encoder,
			    const struct intel_crtc_state *crtc_state,
			    int *n_entries)
{
	if (crtc_state->port_clock > 270000)
		return intel_get_buf_trans(&adls_combo_phy_trans_dp_hbr2_hbr3, n_entries);
	else
		return intel_get_buf_trans(&tgl_combo_phy_trans_dp_hbr, n_entries);
}

static const struct intel_ddi_buf_trans *
adls_get_combo_buf_trans_edp(struct intel_encoder *encoder,
			     const struct intel_crtc_state *crtc_state,
			     int *n_entries)
{
	if (crtc_state->port_clock > 540000)
		return intel_get_buf_trans(&adls_combo_phy_trans_edp_hbr3, n_entries);
	else if (use_edp_hobl(encoder))
		return intel_get_buf_trans(&tgl_combo_phy_trans_edp_hbr2_hobl, n_entries);
	else if (use_edp_low_vswing(encoder))
		return intel_get_buf_trans(&adls_combo_phy_trans_edp_hbr2, n_entries);
	else
		return adls_get_combo_buf_trans_dp(encoder, crtc_state, n_entries);
}

static const struct intel_ddi_buf_trans *
adls_get_combo_buf_trans(struct intel_encoder *encoder,
			 const struct intel_crtc_state *crtc_state,
			 int *n_entries)
{
	if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_HDMI))
		return intel_get_buf_trans(&icl_combo_phy_trans_hdmi, n_entries);
	else if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_EDP))
		return adls_get_combo_buf_trans_edp(encoder, crtc_state, n_entries);
	else
		return adls_get_combo_buf_trans_dp(encoder, crtc_state, n_entries);
}

static const struct intel_ddi_buf_trans *
adlp_get_combo_buf_trans_dp(struct intel_encoder *encoder,
			    const struct intel_crtc_state *crtc_state,
			    int *n_entries)
{
	if (crtc_state->port_clock > 270000)
		return intel_get_buf_trans(&adlp_combo_phy_trans_dp_hbr2_hbr3, n_entries);
	else
		return intel_get_buf_trans(&adlp_combo_phy_trans_dp_hbr, n_entries);
}

static const struct intel_ddi_buf_trans *
adlp_get_combo_buf_trans_edp(struct intel_encoder *encoder,
			     const struct intel_crtc_state *crtc_state,
			     int *n_entries)
{
	if (crtc_state->port_clock > 540000) {
		return intel_get_buf_trans(&adlp_combo_phy_trans_edp_hbr3,
					   n_entries);
	} else if (use_edp_hobl(encoder)) {
		return intel_get_buf_trans(&tgl_combo_phy_trans_edp_hbr2_hobl,
					   n_entries);
	} else if (use_edp_low_vswing(encoder)) {
		return intel_get_buf_trans(&adlp_combo_phy_trans_edp_up_to_hbr2,
					   n_entries);
	}

	return adlp_get_combo_buf_trans_dp(encoder, crtc_state, n_entries);
}

static const struct intel_ddi_buf_trans *
adlp_get_combo_buf_trans(struct intel_encoder *encoder,
			 const struct intel_crtc_state *crtc_state,
			 int *n_entries)
{
	if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_HDMI))
		return intel_get_buf_trans(&icl_combo_phy_trans_hdmi, n_entries);
	else if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_EDP))
		return adlp_get_combo_buf_trans_edp(encoder, crtc_state, n_entries);
	else
		return adlp_get_combo_buf_trans_dp(encoder, crtc_state, n_entries);
}

static const struct intel_ddi_buf_trans *
tgl_get_dkl_buf_trans_dp(struct intel_encoder *encoder,
			 const struct intel_crtc_state *crtc_state,
			 int *n_entries)
{
	if (crtc_state->port_clock > 270000) {
		return intel_get_buf_trans(&tgl_dkl_phy_trans_dp_hbr2,
					   n_entries);
	} else {
		return intel_get_buf_trans(&tgl_dkl_phy_trans_dp_hbr,
					   n_entries);
	}
}

static const struct intel_ddi_buf_trans *
tgl_get_dkl_buf_trans(struct intel_encoder *encoder,
		      const struct intel_crtc_state *crtc_state,
		      int *n_entries)
{
	if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_HDMI))
		return intel_get_buf_trans(&tgl_dkl_phy_trans_hdmi, n_entries);
	else
		return tgl_get_dkl_buf_trans_dp(encoder, crtc_state, n_entries);
}

static const struct intel_ddi_buf_trans *
adlp_get_dkl_buf_trans_dp(struct intel_encoder *encoder,
			  const struct intel_crtc_state *crtc_state,
			  int *n_entries)
{
	if (crtc_state->port_clock > 270000) {
		return intel_get_buf_trans(&adlp_dkl_phy_trans_dp_hbr2_hbr3,
					   n_entries);
	} else {
		return intel_get_buf_trans(&adlp_dkl_phy_trans_dp_hbr,
					   n_entries);
	}
}

static const struct intel_ddi_buf_trans *
adlp_get_dkl_buf_trans(struct intel_encoder *encoder,
		       const struct intel_crtc_state *crtc_state,
		       int *n_entries)
{
	if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_HDMI))
		return intel_get_buf_trans(&tgl_dkl_phy_trans_hdmi, n_entries);
	else
		return adlp_get_dkl_buf_trans_dp(encoder, crtc_state, n_entries);
}

static const struct intel_ddi_buf_trans *
dg2_get_snps_buf_trans(struct intel_encoder *encoder,
		       const struct intel_crtc_state *crtc_state,
		       int *n_entries)
{
	if (intel_crtc_has_dp_encoder(crtc_state) &&
	    intel_dp_is_uhbr(crtc_state))
		return intel_get_buf_trans(&dg2_snps_trans_uhbr, n_entries);
	else
		return intel_get_buf_trans(&dg2_snps_trans, n_entries);
}

static const struct intel_ddi_buf_trans *
mtl_get_cx0_buf_trans(struct intel_encoder *encoder,
		      const struct intel_crtc_state *crtc_state,
		      int *n_entries)
{
	struct drm_i915_private *i915 = to_i915(encoder->base.dev);
	enum phy phy = intel_port_to_phy(i915, encoder->port);

	if (intel_crtc_has_dp_encoder(crtc_state) && crtc_state->port_clock >= 1000000)
		return intel_get_buf_trans(&mtl_c20_trans_uhbr, n_entries);
	else if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_HDMI) && !(intel_is_c10phy(i915, phy)))
		return intel_get_buf_trans(&mtl_c20_trans_hdmi, n_entries);
	else
		return intel_get_buf_trans(&mtl_cx0_trans, n_entries);
}

void intel_ddi_buf_trans_init(struct intel_encoder *encoder)
{
	struct drm_i915_private *i915 = to_i915(encoder->base.dev);
	enum phy phy = intel_port_to_phy(i915, encoder->port);

	if (DISPLAY_VER(i915) >= 14) {
		encoder->get_buf_trans = mtl_get_cx0_buf_trans;
	} else if (IS_DG2(i915)) {
		encoder->get_buf_trans = dg2_get_snps_buf_trans;
	} else if (IS_ALDERLAKE_P(i915)) {
		if (intel_phy_is_combo(i915, phy))
			encoder->get_buf_trans = adlp_get_combo_buf_trans;
		else
			encoder->get_buf_trans = adlp_get_dkl_buf_trans;
	} else if (IS_ALDERLAKE_S(i915)) {
		encoder->get_buf_trans = adls_get_combo_buf_trans;
	} else if (IS_ROCKETLAKE(i915)) {
		encoder->get_buf_trans = rkl_get_combo_buf_trans;
	} else if (IS_DG1(i915)) {
		encoder->get_buf_trans = dg1_get_combo_buf_trans;
	} else if (intel_phy_is_combo(i915, phy)) {
		encoder->get_buf_trans = tgl_get_combo_buf_trans;
	} else {
		encoder->get_buf_trans = tgl_get_dkl_buf_trans;
	}
}
