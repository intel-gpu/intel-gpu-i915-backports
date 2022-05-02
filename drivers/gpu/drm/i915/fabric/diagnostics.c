// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2021 - 2022 Intel Corporation.
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/string.h>
#include <linux/stringify.h>

#include "diagnostics.h"
#include "ops.h"
#include "port.h"

void print_diag(char *buf, size_t *buf_offset, size_t buf_size, const char *fmt,  ...)
{
	va_list args;
	int i;

	va_start(args, fmt);
	i = vscnprintf(buf + *buf_offset, buf_size - *buf_offset, fmt, args);
	va_end(args);

	*buf_offset += i;
}

#define SERDES_HISTOGRAM_FILE_NAME "serdes_histogram"

#define LANE_COLUMN_WIDTH        9
#define NEWLINE_WIDTH            1
#define NULL_TERMINATOR_WIDTH    1
#define MAX_LINE_WIDTH           ((LANE_COLUMN_WIDTH * LANES) + NEWLINE_WIDTH)

#define SERDES_HISTOGRAM_HEADERS 2

#define LPN_HEADER_FMT   "Logical Port %d\n"
/* Each Lane # label is LANE_COLUMN_WIDTH in size and right justified */
#define LANE_HEADER_FMT  "   Lane 0   Lane 1   Lane 2   Lane 3\n"
#define DATA_ELEMENT_FMT "%" __stringify(LANE_COLUMN_WIDTH) "u"

#define HISTOGRAM_DISPLAY_BUF_SIZE ((MAX_LINE_WIDTH * SERDES_HISTOGRAM_HEADERS) + \
				    (MAX_LINE_WIDTH * LANE_DATA_ELEMENTS) + \
				    NULL_TERMINATOR_WIDTH)

static int serdes_histogram_open(struct inode *inode, struct file *file)
{
	struct fport *port = inode->i_private;
	struct serdes_histogram_info {
		struct debugfs_blob_wrapper blob;
		struct mbdb_serdes_histogram_rsp rsp;
		char buf[HISTOGRAM_DISPLAY_BUF_SIZE];
	} *info;
	struct mbdb_serdes_histogram_rsp *rsp;
	size_t buf_size;
	size_t buf_offset;
	char *buf;
	int lane;
	int data_element;
	int ret;

	if (!port)
		return -EINVAL;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	rsp = &info->rsp;

	ret = ops_serdes_histogram_get(port->sd, port->lpn, rsp);
	if (ret) {
		kfree(info);
		return ret;
	}

	buf_size = ARRAY_SIZE(info->buf);
	buf = info->buf;
	buf_offset = 0;

	print_diag(buf, &buf_offset, buf_size, LPN_HEADER_FMT, port->lpn);
	print_diag(buf, &buf_offset, buf_size, LANE_HEADER_FMT);

	for (data_element = 0; data_element < LANE_DATA_ELEMENTS; data_element++) {
		for (lane = 0; lane < LANES; lane++)
			print_diag(buf, &buf_offset, buf_size, DATA_ELEMENT_FMT,
				   rsp->lane[lane].data[data_element]);
		print_diag(buf, &buf_offset, buf_size, "\n");
	}

	info->blob.data = info->buf;
	info->blob.size = buf_offset;
	file->private_data = info;

	return 0;
}

static const struct file_operations serdes_histogram_fops = {
	.owner = THIS_MODULE,
	.open = serdes_histogram_open,
	.read = blob_read,
	.release = blob_release,
	.llseek = default_llseek,
};

static void serdes_histogram_node_init(struct fport *port, struct dentry *debugfs_dir)
{
	debugfs_create_file(SERDES_HISTOGRAM_FILE_NAME, 0400, debugfs_dir, port,
			    &serdes_histogram_fops);
}

#define SERDES_EQINFO_FILE_NAME "serdes_eqinfo"

#define EQINFO_MAX_LINE_LENGTH 94
#define EQINFO_OUTPUT_LINES 113

#define EQINFO_DISPLAY_BUF_SIZE (EQINFO_OUTPUT_LINES * EQINFO_MAX_LINE_LENGTH)

#define EQINFO_LANE_HDR_FMT  "SerdesEqInfo       lane 0             lane 1             lane 2             lane 3\n"

#define EQINFO_8BIT_HDR_FMT  "%-16s %#-18hhx %#-18hhx %#-18hhx %#-18hhx\n"
#define EQINFO_16BIT_HDR_FMT "%-16s %#-18hx %#-18hx %#-18hx %#-18hx\n"
#define EQINFO_32BIT_HDR_FMT "%-16s %#-18x %#-18x %#-18x %#-18x\n"
#define EQINFO_64BIT_HDR_FMT "%-16s %#-18llx %#-18llx %#-18llx %#-18llx\n"

#define PRINT_LANES_EQINFO_FIELD_8(name, field, buf, buf_offset, buf_size) \
	print_diag(buf, buf_offset, buf_size, EQINFO_8BIT_HDR_FMT, name, \
		   (unsigned char)eq_info[0].field, (unsigned char)eq_info[1].field, \
		   (unsigned char)eq_info[2].field, (unsigned char)eq_info[3].field)

#define PRINT_LANES_EQINFO_FIELD_16(name, field, buf, buf_offset, buf_size) \
	print_diag(buf, buf_offset, buf_size, EQINFO_16BIT_HDR_FMT, name, \
		   (unsigned short)eq_info[0].field, (unsigned short)eq_info[1].field, \
		   (unsigned short)eq_info[2].field, (unsigned short)eq_info[3].field)

#define PRINT_LANES_EQINFO_FIELD_32(name, field, buf, buf_offset, buf_size) \
	print_diag(buf, buf_offset, buf_size, EQINFO_32BIT_HDR_FMT, name, \
		   (unsigned int)eq_info[0].field, (unsigned int)eq_info[1].field, \
		   (unsigned int)eq_info[2].field, (unsigned int)eq_info[3].field)

#define PRINT_LANES_EQINFO_FIELD_64(name, field, buf, buf_offset, buf_size) \
	print_diag(buf, buf_offset, buf_size, EQINFO_64BIT_HDR_FMT, name, \
		   eq_info[0].field, eq_info[1].field, \
		   eq_info[2].field, eq_info[3].field)

static void serdes_eqinfo_process(char *buf, size_t *buf_offset, size_t buf_size,
				  struct mbdb_serdes_eq_info *eq_info)
{
	char name[17];
	int i;

	PRINT_LANES_EQINFO_FIELD_32("eqP4Rev", eq_p4_rev, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_32("eqP4Time", eq_p4_time, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_32("rxdTxdP4Rev", rxd_txd_p4_rev, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_32("rxdTxdP4Time", rxd_txd_p4_time, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("eqCompileOptions", eq_compile_options, buf, buf_offset,
				   buf_size);
	PRINT_LANES_EQINFO_FIELD_8("agcMode", agc_mode, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("agc1LmsMu", agc1_lms_mu, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("agc1PeakNcycExp", agc1_peak_ncyc_exp, buf, buf_offset,
				   buf_size);
	PRINT_LANES_EQINFO_FIELD_8("agc2LmsMu", agc2_lms_mu, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("agc2PeakNcycExp", agc2_peak_ncyc_exp, buf, buf_offset,
				   buf_size);
	PRINT_LANES_EQINFO_FIELD_8("agcLpfMu", agc_lpf_mu, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("agcTarg", agc_targ, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("agc1LmsEn", agc1_lms_en, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("agc1LmsLd", agc1_lms_ld, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("agc2LmsEn", agc2_lms_en, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("agc2LmsLd", agc2_lms_ld, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("agc1LmsLdVal", agc1_lms_ld_val, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("agc1Ctl", agc1_ctl, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("agc1Peak", agc1_peak, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("agc1Ppeak", agc1_ppeak, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("agc2LmsLdVal", agc2_lms_ld_val, buf, buf_offset, buf_size);

	for (i = 0; i < ARRAY_SIZE(eq_info[0].agc2_ctl); i++) {
		scnprintf(name, sizeof(name), "agc2Ctl[%d]", i);
		PRINT_LANES_EQINFO_FIELD_8(name, agc2_ctl[i], buf, buf_offset, buf_size);
	}

	for (i = 0; i < ARRAY_SIZE(eq_info[0].agc2_peak); i++) {
		scnprintf(name, sizeof(name), "agc2Peak[%d]", i);
		PRINT_LANES_EQINFO_FIELD_8(name, agc2_peak[i], buf, buf_offset, buf_size);
	}

	for (i = 0; i < ARRAY_SIZE(eq_info[0].agc2_ppeak); i++) {
		scnprintf(name, sizeof(name), "agc2Ppeak[%d]", i);
		PRINT_LANES_EQINFO_FIELD_8(name, agc2_ppeak[i], buf, buf_offset, buf_size);
	}

	PRINT_LANES_EQINFO_FIELD_8("cdrPropMu", cdr_prop_mu, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("cdrIntgMu", cdr_intg_mu, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("cdrFltMu", cdr_flt_mu, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("cdrPherrScale", cdr_pherr_scale, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("cdrSsEn", cdr_ss_en, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("cdrFltEn", cdr_flt_en, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("cdrIntgEn", cdr_intg_en, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("cdrPhase", cdr_phase, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("cdrIntg", cdr_intg, buf, buf_offset, buf_size);

	PRINT_LANES_EQINFO_FIELD_16("cdrPhErrFlt", cdr_ph_err_flt, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_64("cntrIlvExclMsk", cntr_ilv_excl_msk, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_32("ppm", ppm, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("cntrSh", cntr_sh, buf, buf_offset, buf_size);

	for (i = 0; i < ARRAY_SIZE(eq_info[0].hcntr); i++) {
		scnprintf(name, sizeof(name), "hcntr[%d]", i);
		PRINT_LANES_EQINFO_FIELD_8(name, hcntr[i], buf, buf_offset, buf_size);
	}

	for (i = 0; i < ARRAY_SIZE(eq_info[0].cntr_ch_est); i++) {
		scnprintf(name, sizeof(name), "cntrChEst[%d]", i);
		PRINT_LANES_EQINFO_FIELD_16(name, cntr_ch_est[i], buf, buf_offset, buf_size);
	}

	PRINT_LANES_EQINFO_FIELD_8("ffeLmsMu", ffe_lms_mu, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("ffeLmsLkMuDelta", ffe_lms_lk_mu_delta, buf, buf_offset,
				   buf_size);
	PRINT_LANES_EQINFO_FIELD_8("ffeLmsLkEn", ffe_lms_lk_en, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("dfeLmsMu", dfe_lms_mu, buf, buf_offset, buf_size);

	for (i = 0; i < ARRAY_SIZE(eq_info[0].eq_targ); i++) {
		scnprintf(name, sizeof(name), "eqTarg[%d]", i);
		PRINT_LANES_EQINFO_FIELD_16(name, eq_targ[i], buf, buf_offset, buf_size);
	}

	for (i = 0; i < ARRAY_SIZE(eq_info[0].dfe_nthr); i++) {
		scnprintf(name, sizeof(name), "dfeNthr[%d]", i);
		PRINT_LANES_EQINFO_FIELD_16(name, dfe_nthr[i], buf, buf_offset, buf_size);
	}

	for (i = 0; i < ARRAY_SIZE(eq_info[0].dfe_zthr); i++) {
		scnprintf(name, sizeof(name), "dfeZthr[%d]", i);
		PRINT_LANES_EQINFO_FIELD_16(name, dfe_zthr[i], buf, buf_offset, buf_size);
	}

	for (i = 0; i < ARRAY_SIZE(eq_info[0].dfe_pthr); i++) {
		scnprintf(name, sizeof(name), "dfePthr[%d]", i);
		PRINT_LANES_EQINFO_FIELD_16(name, dfe_pthr[i], buf, buf_offset, buf_size);
	}

	for (i = 0; i < ARRAY_SIZE(eq_info[0].hffe); i++) {
		scnprintf(name, sizeof(name), "hffe[%d]", i);
		PRINT_LANES_EQINFO_FIELD_16(name, hffe[i], buf, buf_offset, buf_size);
	}

	PRINT_LANES_EQINFO_FIELD_32("gf0", gf0, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_16("hdfe", hdfe, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("nrzSliceEn", nrz_slice_en, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("rmtTxLane", rmt_tx_lane, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_16("lmsSumErr", lms_sum_err, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_16("lmsSumErrShf", lms_sum_err_shf, buf, buf_offset, buf_size);

	for (i = 0; i < ARRAY_SIZE(eq_info[0].tx_fir_eh); i++) {
		scnprintf(name, sizeof(name), "txFirEh[%d]", i);
		PRINT_LANES_EQINFO_FIELD_8(name, tx_fir_eh[i], buf, buf_offset, buf_size);
	}

	PRINT_LANES_EQINFO_FIELD_8("txFirEhM1", tx_fir_eh_m1, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_8("pllLolCnt", pll_lol_cnt, buf, buf_offset, buf_size);
	PRINT_LANES_EQINFO_FIELD_16("pmonUlvtFreq", pmon_ulvt_freq, buf, buf_offset, buf_size);
}

static int serdes_eqinfo_open(struct inode *inode, struct file *file)
{
	struct fport *port = inode->i_private;
	struct serdes_eqinfo_info {
		struct debugfs_blob_wrapper blob;
		struct mbdb_serdes_eq_info_get_rsp rsp;
		char buf[EQINFO_DISPLAY_BUF_SIZE];
	} *info;
	size_t buf_size;
	size_t buf_offset;
	char *buf;
	int ret;

	if (!port)
		return -EINVAL;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	ret = ops_serdes_eqinfo_get(port->sd, port->lpn, &info->rsp);
	if (ret) {
		kfree(info);
		return ret;
	}

	buf_size = ARRAY_SIZE(info->buf);
	buf = info->buf;
	buf_offset = 0;

	print_diag(buf, &buf_offset, buf_size, LPN_HEADER_FMT, port->lpn);
	print_diag(buf, &buf_offset, buf_size, EQINFO_LANE_HDR_FMT);

	serdes_eqinfo_process(buf, &buf_offset, buf_size, info->rsp.eq_info);

	info->blob.data = buf;
	info->blob.size = buf_offset;
	file->private_data = info;

	return 0;
}

static const struct file_operations serdes_eqinfo_fops = {
	.owner = THIS_MODULE,
	.open = serdes_eqinfo_open,
	.read = blob_read,
	.release = blob_release,
	.llseek = default_llseek,
};

static void serdes_eqinfo_node_init(struct fport *port, struct dentry *debugfs_dir)
{
	debugfs_create_file(SERDES_EQINFO_FILE_NAME, 0400, debugfs_dir, port, &serdes_eqinfo_fops);
}

#define LCB_COUNTERS_FILE_NAME "lcb_ctrs"

#define LCB_ERR_INFO_NAMES_B0 \
	"TOTAL_CRC_ERR", \
	"CRC_ERR_LN0", \
	"CRC_ERR_LN1", \
	"CRC_ERR_LN2", \
	"CRC_ERR_LN3", \
	"CRC_ERR_MULTI_LN", \
	"TX_REPLAY", \
	"RX_REPLAY", \
	"SEQ_CRC", \
	"ESCAPE_0_ONLY", \
	"ESCAPE_0_PLUS1", \
	"ESCAPE_0_PLUS2", \
	"REINIT_FROM_PEER", \
	"SBE", \
	"MISC_FLG", \
	NULL, \
	NULL, \
	NULL, \
	NULL, \
	NULL, \
	NULL, \
	NULL, \
	NULL, \
	NULL, \
	"FEC_CERR_1", \
	"FEC_CERR_2", \
	"FEC_CERR_3", \
	"FEC_CERR_4", \
	"FEC_CERR_5", \
	"FEC_CERR_6", \
	"FEC_CERR_7", \
	"FEC_CERR_8", \
	"FEC_UERR_CNT", \
	NULL, \
	NULL, \
	NULL, \
	NULL, \
	NULL, \
	NULL, \
	NULL, \
	"FEC_ERR_LN0", \
	"FEC_ERR_LN1", \
	"FEC_ERR_LN2", \
	"FEC_ERR_LN3", \
	"RX_RESYNC_CNT"

static const char * const lcb_err_info_names_b0[] = {
	LCB_ERR_INFO_NAMES_B0
};

static const char * const lcb_prf_names[] = {
	"GOOD_LTP",
	"ACCEPTED_LTP",
	"TX_RELIABLE_LTP",
	"RX_FLIT",
	"TX_FLIT",
	NULL,
	"GOOD_FECCW"
};

#define LCB_ERR_INFO_VALUES_B0 ARRAY_SIZE(lcb_err_info_names_b0)
#define LCB_ERR_INFO_VALUES_NUM LCB_ERR_INFO_VALUES_B0
#define LCB_PRF_VALUES ARRAY_SIZE(lcb_prf_names)

#define LCB_COUNTERS_DISPLAY_BUF_SIZE (PAGE_SIZE - sizeof(struct debugfs_blob_wrapper))

#define LCB_COUNTERS_FMT "%-16s %llu\n"

struct lcb_counters_regs_data {
	DECLARE_MBDB_OP_PORT_STATUS_GET_RSP(regs_op, LCB_ERR_INFO_VALUES_NUM + LCB_PRF_VALUES);
} __packed;

static int lcb_counters_open(struct inode *inode, struct file *file)
{
	struct fport *port = inode->i_private;
	struct mbdb_op_csr_range csr_ranges[] = {
		{ .offset = O_LCB_ERR_INFO_OFFSET,
		  .num_csrs = LCB_ERR_INFO_VALUES_B0
		},
		{ .offset = O_LCB_PRF_OFFSET,
		  .num_csrs = LCB_PRF_VALUES
		},
	};
	struct lcb_counters_regs_data regs = {};
	struct lcb_counters_info {
		struct debugfs_blob_wrapper blob;
		char buf[LCB_COUNTERS_DISPLAY_BUF_SIZE];
	} *info;
	const char * const *lcb_err_info_names = lcb_err_info_names_b0;
	size_t buf_size;
	size_t buf_offset;
	char *buf;
	int ret;
	int i;

	ret = ops_port_status_get(port->sd, port->lpn, ARRAY_SIZE(csr_ranges), csr_ranges,
				  &regs.regs_op);
	if (ret)
		return ret;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	buf_size = ARRAY_SIZE(info->buf);
	buf = info->buf;
	buf_offset = 0;

	print_diag(buf, &buf_offset, buf_size, "%s %u\n", "LOGICAL_PORT", port->lpn);
	print_diag(buf, &buf_offset, buf_size, LCB_COUNTERS_FMT, "FR_RTC", regs.cp_free_run_rtc);

	for (i = 0; i < csr_ranges[0].num_csrs; i++)
		if (lcb_err_info_names[i])
			print_diag(buf, &buf_offset, buf_size, LCB_COUNTERS_FMT,
				   lcb_err_info_names[i], regs.regs[i]);

	for (i = 0; i < LCB_PRF_VALUES; i++)
		if (lcb_prf_names[i])
			print_diag(buf, &buf_offset, buf_size, LCB_COUNTERS_FMT, lcb_prf_names[i],
				   regs.regs[i + csr_ranges[0].num_csrs]);

	info->blob.data = buf;
	info->blob.size = buf_offset;
	file->private_data = info;

	return 0;
}

static const struct file_operations lcb_counters_fops = {
	.owner = THIS_MODULE,
	.open = lcb_counters_open,
	.read = blob_read,
	.release = blob_release,
	.llseek = default_llseek,
};

static void lcb_counters_node_init(struct fport *port, struct dentry *debugfs_dir)
{
	debugfs_create_file(LCB_COUNTERS_FILE_NAME, 0400, debugfs_dir, port,
			    &lcb_counters_fops);
}

#define SERDES_CHANNEL_ESTIMATION_MAX_BUF_SIZE (PAGE_SIZE * 10)
#define SERDES_CHANNEL_ESTIMATION_MAX_ELEMENTS 1020
#define SERDES_CHANNEL_ESTIMATION_FILE_NAME "serdes_channel_estimation"
#define SERDES_CHANNEL_ESTIMATION_DATA_ELEMENT_FMT "%9hd"

static int serdes_channel_estimation_open(struct inode *inode, struct file *file)
{
	struct fport *port = inode->i_private;
	struct serdes_channel_estimation_info {
		struct debugfs_blob_wrapper blob;
		struct mbdb_serdes_ch_est_rsp rsp[LANES];
		char buf[SERDES_CHANNEL_ESTIMATION_MAX_BUF_SIZE];
	} *info;
	struct mbdb_serdes_ch_est_rsp *rsp;
	size_t buf_size;
	size_t buf_offset;
	char *buf;
	u8 lane;
	u16 elements;
	u16 data_element;
	int ret;

	if (!port)
		return -EINVAL;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	elements = SERDES_CHANNEL_ESTIMATION_MAX_ELEMENTS;

	for (lane = 0; lane < LANES; lane++) {
		rsp = &info->rsp[lane];
		ret = ops_serdes_channel_estimate_get(port->sd, port->lpn, lane, rsp);
		if (ret) {
			kfree(info);
			return ret;
		}

		elements = min_t(u16, elements, rsp->elements);
	}

	buf_size = ARRAY_SIZE(info->buf);
	buf = info->buf;
	buf_offset = 0;

	print_diag(buf, &buf_offset, buf_size, LPN_HEADER_FMT, port->lpn);
	print_diag(buf, &buf_offset, buf_size, LANE_HEADER_FMT);

	for (data_element = 0; data_element < elements; data_element++) {
		for (lane = 0; lane < LANES; lane++)
			print_diag(buf, &buf_offset, buf_size,
				   SERDES_CHANNEL_ESTIMATION_DATA_ELEMENT_FMT,
				   info->rsp[lane].data[data_element]);

		print_diag(buf, &buf_offset, buf_size, "\n");
	}

	info->blob.data = info->buf;
	info->blob.size = buf_offset;
	file->private_data = info;

	return 0;
}

static const struct file_operations serdes_channel_estimation_fops = {
	.owner = THIS_MODULE,
	.open = serdes_channel_estimation_open,
	.read = blob_read,
	.release = blob_release,
	.llseek = default_llseek,
};

static void serdes_channel_estimation_node_init(struct fport *port, struct dentry *debugfs_dir)
{
	debugfs_create_file(SERDES_CHANNEL_ESTIMATION_FILE_NAME, 0400, debugfs_dir, port,
			    &serdes_channel_estimation_fops);
}

/*
 * Remote TX lanes
 *
 * Data is transmitted on up to four lanes and may be "swizzled" so that TX lanes are connected to
 * differently-numbered RX lanes. Report the source lane for all four lanes.
 */

#define REMOTE_TX_LANES_FILE_NAME "remote_tx_lanes"
#define TX_LANES_STRING_SIZE (10)

/* lane number as a character, replacing illegal lane values with x (lane is unconnected) */
static char lane_indicator(u8 lane)
{
	return lane < LANES ? '0' + lane : 'x';
}

static ssize_t remote_tx_lanes_read(struct file *fp, char __user *buf, size_t count, loff_t *fpos)
{
	struct fport *port = fp->private_data;
	char rd_buf[TX_LANES_STRING_SIZE];
	size_t siz;
	u64 value = 0;
	int err;

	err = ops_linkmgr_port_csr_rd(port->sd, port->lpn, O_LCB_STS_RX_LOGICAL_ID, sizeof(value),
				      &value);
	if (err)
		return err;

	siz = scnprintf(rd_buf, sizeof(rd_buf), "%c %c %c %c\n",
			lane_indicator(FIELD_GET(PEER_TX_ID_LN0, value)),
			lane_indicator(FIELD_GET(PEER_TX_ID_LN1, value)),
			lane_indicator(FIELD_GET(PEER_TX_ID_LN2, value)),
			lane_indicator(FIELD_GET(PEER_TX_ID_LN3, value)));

	return simple_read_from_buffer(buf, count, fpos, rd_buf, siz);
}

static const struct file_operations remote_lanes_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.llseek = no_llseek,
	.read = remote_tx_lanes_read
};

static void remote_tx_lanes_init(struct fport *port, struct dentry *debugfs_dir)
{
	debugfs_create_file(REMOTE_TX_LANES_FILE_NAME, 0400, debugfs_dir, port, &remote_lanes_fops);
}

/*
 * Port enables
 *
 * "enable" identifies whether the port is used at all; "usage_enable" identifies whether routing
 * will configure it to carry data
 */

#define PORT_ENABLE_FILE_NAME "enable"
#define USAGE_ENABLE_FILE_NAME "usage_enable"
#define ENABLE_STRING_SIZE 3

/* port control as a character indicating boolean state */
static char control_flag(struct fport *port, enum PORT_CONTROL bit)
{
	return test_bit(bit, port->controls) ? 'Y' : 'N';
}

static ssize_t port_ena_read(struct file *fp, char __user *buf, size_t count, loff_t *fpos)
{
	struct fport *port = fp->private_data;
	char rd_buf[ENABLE_STRING_SIZE];
	size_t siz;

	siz = scnprintf(rd_buf, sizeof(rd_buf), "%c\n", control_flag(port, PORT_CONTROL_ENABLED));

	return simple_read_from_buffer(buf, count, fpos, rd_buf, siz);
}

static ssize_t usage_ena_read(struct file *fp, char __user *buf, size_t count, loff_t *fpos)
{
	struct fport *port = fp->private_data;
	char rd_buf[ENABLE_STRING_SIZE];
	size_t siz;

	siz = scnprintf(rd_buf, sizeof(rd_buf), "%c\n", control_flag(port, PORT_CONTROL_ROUTABLE));

	return simple_read_from_buffer(buf, count, fpos, rd_buf, siz);
}

static ssize_t func_ena_write(struct file *fp, const char __user *buf, size_t count, loff_t *fpos,
			      int (*enablefn)(struct fport *), int (*disablefn)(struct fport *))
{
	struct fport *port = fp->private_data;
	char *kbuf;
	bool set;
	int err;

	if (!count)
		return 0;

	kbuf = kzalloc(count + 1, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	/* only proceed if entire string can be copied */
	if (copy_from_user(kbuf, buf, count)) {
		kfree(kbuf);
		return -EFAULT;
	}

	err = strtobool(kbuf, &set);
	kfree(kbuf);
	if (err)
		return err;

	err = set ? enablefn(port) : disablefn(port);
	if (err)
		return err;

	*fpos += count;
	return count;
}

static ssize_t port_ena_write(struct file *fp, const char __user *buf, size_t count, loff_t *fpos)
{
	return func_ena_write(fp, buf, count, fpos, enable_port, disable_port);
}

static ssize_t usage_ena_write(struct file *fp, const char __user *buf, size_t count, loff_t *fpos)
{
	return func_ena_write(fp, buf, count, fpos, enable_usage_port, disable_usage_port);
}

static const struct file_operations port_enable_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.llseek = no_llseek,
	.read = port_ena_read,
	.write = port_ena_write
};

static const struct file_operations usage_enable_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.llseek = no_llseek,
	.read = usage_ena_read,
	.write = usage_ena_write
};

static void enable_nodes_init(struct fport *port, struct dentry *debugfs_dir)
{
	debugfs_create_file(PORT_ENABLE_FILE_NAME, 0600, debugfs_dir, port, &port_enable_fops);
	debugfs_create_file(USAGE_ENABLE_FILE_NAME, 0600, debugfs_dir, port, &usage_enable_fops);
}

/*
 * Per-port TX tuning parameters
 *
 * There are two sets of TX tuning parameters for each port, based on speed class: FAST (>= 90G) and
 * SLOW (<= 53G). Each set contains one value per lane. A third set of parameters allows users to
 * query or set parameters corresponding to the currently-configured speed.
 */
#define TX_TUNING_FAST_FILE_NAME "tx_tuning_fast"
#define TX_TUNING_SLOW_FILE_NAME "tx_tuning_slow"
#define TX_TUNING_CURR_FILE_NAME "tx_tuning_current"

#define BAD_TUNE_IDX (~0)
#define UNSPECIFIED_TX_TUNING_INDICES { BAD_TUNE_IDX, BAD_TUNE_IDX, BAD_TUNE_IDX, BAD_TUNE_IDX }
#define LONGEST_TX_TUNING_STRING (48)
#define TUNING_SEPS " \t"

#define LEGAL_TX_TUNING_INDEX(_i) ((_i) < 256)

static int read_tx_tunings(struct fport *port, u32 link_speed, u32 idx[LANES])
{
	struct port_var_data var_data = {};
	int err;
	int i;

	err = ops_port_var_table_read(port->sd, port->lpn, link_speed, &var_data);
	if (err)
		return err;

	for (i = 0; i < LANES; ++i)
		idx[i] = var_data.tx_tuning[i];

	return 0;
}

static int write_tx_tunings(struct fport *port, u32 link_speed, const u32 idx[LANES])
{
	struct port_var_data var_data;
	int err;
	int i;

	for (i = 0; i < LANES; ++i)
		if (!LEGAL_TX_TUNING_INDEX(idx[i]))
			break;

	err = i < LANES ? ops_port_var_table_read(port->sd, port->lpn, link_speed, &var_data) : 0;
	if (err)
		return err;

	for (i = 0; i < LANES; ++i)
		if (LEGAL_TX_TUNING_INDEX(idx[i]))
			var_data.tx_tuning[i] = idx[i];

	return ops_port_var_table_write(port->sd, port->lpn, link_speed, &var_data, false);
}

static ssize_t tune_read_spd(struct file *fp, char __user *buf, size_t count, loff_t *fpos,
			     u32 link_speed)
{
	char rd_buf[LONGEST_TX_TUNING_STRING];
	u32 idx[LANES];
	size_t siz;
	int err;

	if (!link_speed) {
		siz = scnprintf(rd_buf, sizeof(rd_buf), "? ? ? ?\n");
		return simple_read_from_buffer(buf, count, fpos, rd_buf, siz);
	}

	err = read_tx_tunings(fp->private_data, link_speed, idx);
	if (err)
		return err;

	siz = scnprintf(rd_buf, sizeof(rd_buf), "%u %u %u %u\n", idx[0], idx[1], idx[2], idx[3]);
	return simple_read_from_buffer(buf, count, fpos, rd_buf, siz);
}

static ssize_t tune_write_spd(struct file *fp, const char __user *buf, size_t count, loff_t *fpos,
			      u32 link_speed)
{
	u32 idx[LANES] = UNSPECIFIED_TX_TUNING_INDICES;
	char *next_token;
	char *curr_token;
	char *kbuf;
	int lane;
	int err;

	if (!count)
		return 0;

	/* do not try to process unreasonably long input */
	if (count > LONGEST_TX_TUNING_STRING)
		return -EINVAL;

	kbuf = kzalloc(count + 1, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	if (copy_from_user(kbuf, buf, count)) {
		kfree(kbuf);
		return -EFAULT;
	}

	/*
	 * process only fully-copied input, consisting of 4 unsigned numbers separated by
	 * single spaces/tabs: any illegal number (e.g., "-", "none") causes the existing
	 * corresponding value to be retained
	 */

	next_token = kbuf;

	for (lane = 0; next_token && *next_token && lane < LANES; ++lane) {
		curr_token = strsep(&next_token, TUNING_SEPS);
		err = kstrtou32(curr_token, 0, &idx[lane]);
		if (err)
			idx[lane] = BAD_TUNE_IDX;
	}

	kfree(kbuf);

	err = write_tx_tunings(fp->private_data, link_speed, idx);
	if (err < 0)
		return err;

	*fpos += count;

	return count;
}

static ssize_t tune_read_fast(struct file *fp, char __user *buf, size_t count, loff_t *fpos)
{
	return tune_read_spd(fp, buf, count, fpos, LINK_SPEED_FAST);
}

static ssize_t tune_read_slow(struct file *fp, char __user *buf, size_t count, loff_t *fpos)
{
	return tune_read_spd(fp, buf, count, fpos, LINK_SPEED_SLOW);
}

static ssize_t tune_read_current(struct file *fp, char __user *buf, size_t count, loff_t *fpos)
{
	struct fport *port = fp->private_data;
	u32 link_speed;

	/*
	 * read tuning parameters for active speed class, which must uniquely match either FAST or
	 * SLOW bitmap
	 *
	 * if active speed does not match this criterion (i.e., if no speed is active), fall back to
	 * enabled speed(s) if it/they uniquely match either FAST or SLOW bitmap
	 */

	link_speed = port->portinfo->link_speed_active;

	if ((bool)(link_speed & LINK_SPEED_SLOW) == (bool)(link_speed & LINK_SPEED_FAST)) {
		link_speed = port->portinfo->link_speed_enabled;

		if ((bool)(link_speed & LINK_SPEED_SLOW) == (bool)(link_speed & LINK_SPEED_FAST))
			link_speed = 0;
	}

	return tune_read_spd(fp, buf, count, fpos, link_speed);
}

static ssize_t tune_write_fast(struct file *fp, const char __user *buf, size_t count, loff_t *fpos)
{
	return tune_write_spd(fp, buf, count, fpos, LINK_SPEED_FAST);
}

static ssize_t tune_write_slow(struct file *fp, const char __user *buf, size_t count, loff_t *fpos)
{
	return tune_write_spd(fp, buf, count, fpos, LINK_SPEED_SLOW);
}

static ssize_t tune_write_current(struct file *fp, const char __user *buf, size_t count,
				  loff_t *fpos)
{
	struct fport *port = fp->private_data;
	u32 link_speed;

	/*
	 * write tuning parameters for active speed class which must match either FAST or SLOW
	 * bitmap
	 *
	 * if active speed does not match this criterion (i.e., if no speed is active), instead
	 * enabled speed(s)
	 */

	link_speed = port->portinfo->link_speed_active;

	if (!(link_speed & (LINK_SPEED_SLOW | LINK_SPEED_FAST)))
		link_speed = port->portinfo->link_speed_enabled;

	return tune_write_spd(fp, buf, count, fpos, link_speed);
}

static const struct file_operations tune_fast_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.llseek = no_llseek,
	.read = tune_read_fast,
	.write = tune_write_fast
};

static const struct file_operations tune_slow_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.llseek = no_llseek,
	.read = tune_read_slow,
	.write = tune_write_slow
};

static const struct file_operations tune_current_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.llseek = no_llseek,
	.read = tune_read_current,
	.write = tune_write_current
};

static void tx_tuning_nodes_init(struct fport *port, struct dentry *debugfs_dir)
{
	debugfs_create_file(TX_TUNING_FAST_FILE_NAME, 0600, debugfs_dir, port, &tune_fast_fops);
	debugfs_create_file(TX_TUNING_SLOW_FILE_NAME, 0600, debugfs_dir, port, &tune_slow_fops);
	debugfs_create_file(TX_TUNING_CURR_FILE_NAME, 0600, debugfs_dir, port, &tune_current_fops);
}

/**
 * diagnostics_port_node_init() - Add diagnostic nodes to a port debugfs hierarchy
 * @port: fabric port to reference
 * @debugfs_dir: port directory in debugfs to populate under
 *
 * Create debugfs nodes to query (and in the case of tx tuning parameters, set) SERDES-related
 * information and LCB counters. They are removed recursively, so no matching remove function is
 * needed.
 */
void diagnostics_port_node_init(struct fport *port, struct dentry *debugfs_dir)
{
	if (test_bit(MBOX_OP_CODE_SERDES_HISTOGRAM_GET, port->sd->fw_version.supported_opcodes))
		serdes_histogram_node_init(port, debugfs_dir);
	if (test_bit(MBOX_OP_CODE_SERDES_EQINFO_GET, port->sd->fw_version.supported_opcodes))
		serdes_eqinfo_node_init(port, debugfs_dir);
	if (test_bit(MBOX_OP_CODE_SERDES_CHEST_GET, port->sd->fw_version.supported_opcodes))
		serdes_channel_estimation_node_init(port, debugfs_dir);
	lcb_counters_node_init(port, debugfs_dir);
	remote_tx_lanes_init(port, debugfs_dir);
	enable_nodes_init(port, debugfs_dir);
	if (test_bit(MBOX_OP_CODE_VARIABLE_TABLE_READ, port->sd->fw_version.supported_opcodes))
		tx_tuning_nodes_init(port, debugfs_dir);
}
