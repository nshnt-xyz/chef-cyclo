/* qmi_elem_info tables for sns_msgs.h. See that header for where the
 * layouts come from. One table per message; nested structs get their own
 * table and are referenced through .ei_array, as the vendored codec
 * (../qrtr/qmi.c, from linux-msm/qrtr) expects. */
#include <stddef.h>

#include "sns_msgs.h"

#define MSZ(st, f)	sizeof(((st *)0)->f)
#define MSZ0(st, f)	sizeof(((st *)0)->f[0])

/* Scalar field (u8/u16/u32/u64 by size; signedness is the C type's). */
#define EI_NUM(tlv, st, f) { \
	.data_type = MSZ(st, f) == 1 ? QMI_UNSIGNED_1_BYTE : \
		     MSZ(st, f) == 2 ? QMI_UNSIGNED_2_BYTE : \
		     MSZ(st, f) == 4 ? QMI_UNSIGNED_4_BYTE : QMI_UNSIGNED_8_BYTE, \
	.elem_len = 1, .elem_size = MSZ(st, f), .array_type = NO_ARRAY, \
	.tlv_type = (tlv), .offset = offsetof(st, f) }

/* Fixed-length array of scalars. */
#define EI_NUMS(tlv, st, f, n) { \
	.data_type = MSZ0(st, f) == 1 ? QMI_UNSIGNED_1_BYTE : \
		     MSZ0(st, f) == 2 ? QMI_UNSIGNED_2_BYTE : QMI_UNSIGNED_4_BYTE, \
	.elem_len = (n), .elem_size = MSZ0(st, f), .array_type = STATIC_ARRAY, \
	.tlv_type = (tlv), .offset = offsetof(st, f) }

#define EI_OPT(tlv, st, f) { \
	.data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = 1, \
	.array_type = NO_ARRAY, .tlv_type = (tlv), .offset = offsetof(st, f) }

/* Element count of the variable array that follows: wsz is the wire size
 * of the count (1, or 2 when the bound exceeds 255). The C field is u32. */
#define EI_LEN(tlv, st, f, wsz) { \
	.data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = (wsz), \
	.array_type = NO_ARRAY, .tlv_type = (tlv), .offset = offsetof(st, f) }

#define EI_VNUMS(tlv, st, f, n) { \
	.data_type = MSZ0(st, f) == 1 ? QMI_UNSIGNED_1_BYTE : \
		     MSZ0(st, f) == 2 ? QMI_UNSIGNED_2_BYTE : QMI_UNSIGNED_4_BYTE, \
	.elem_len = (n), .elem_size = MSZ0(st, f), .array_type = VAR_LEN_ARRAY, \
	.tlv_type = (tlv), .offset = offsetof(st, f) }

#define EI_STRUCT(tlv, st, f, sub) { \
	.data_type = QMI_STRUCT, .elem_len = 1, .elem_size = MSZ(st, f), \
	.array_type = NO_ARRAY, .tlv_type = (tlv), .offset = offsetof(st, f), \
	.ei_array = (sub) }

#define EI_VSTRUCTS(tlv, st, f, n, sub) { \
	.data_type = QMI_STRUCT, .elem_len = (n), .elem_size = MSZ0(st, f), \
	.array_type = VAR_LEN_ARRAY, .tlv_type = (tlv), .offset = offsetof(st, f), \
	.ei_array = (sub) }

#define EI_END { .data_type = QMI_EOTI }

/* ------------------------------------------------------- common part */

static struct qmi_elem_info sns_resp_ei[] = {
	EI_NUM(0, struct sns_resp, result),
	EI_NUM(0, struct sns_resp, err),
	EI_END
};

struct qmi_elem_info sns_empty_ei[] = {
	EI_END
};

struct qmi_elem_info sns_generic_resp_ei[] = {
	EI_STRUCT(0x02, struct sns_generic_resp, resp, sns_resp_ei),
	EI_END
};

struct qmi_elem_info sns_version_resp_ei[] = {
	EI_STRUCT(0x02, struct sns_version_resp, resp, sns_resp_ei),
	EI_NUM(0x03, struct sns_version_resp, interface_version),
	EI_NUM(0x04, struct sns_version_resp, max_msg_id),
	EI_END
};

/* -------------------------------------------------------------- REG2 */

struct qmi_elem_info sns_reg2_id_req_ei[] = {
	EI_NUM(0x01, struct sns_reg2_id_req, id),
	EI_END
};

struct qmi_elem_info sns_reg2_item_read_resp_ei[] = {
	EI_STRUCT(0x02, struct sns_reg2_item_read_resp, resp, sns_resp_ei),
	EI_NUM(0x03, struct sns_reg2_item_read_resp, id),
	EI_LEN(0x04, struct sns_reg2_item_read_resp, data_len, 1),
	EI_VNUMS(0x04, struct sns_reg2_item_read_resp, data, SNS_REG2_ITEM_MAX),
	EI_END
};

struct qmi_elem_info sns_reg2_item_write_req_ei[] = {
	EI_NUM(0x01, struct sns_reg2_item_write_req, id),
	EI_LEN(0x02, struct sns_reg2_item_write_req, data_len, 1),
	EI_VNUMS(0x02, struct sns_reg2_item_write_req, data, SNS_REG2_ITEM_MAX),
	EI_END
};

struct qmi_elem_info sns_reg2_group_read_resp_ei[] = {
	EI_STRUCT(0x02, struct sns_reg2_group_read_resp, resp, sns_resp_ei),
	EI_NUM(0x03, struct sns_reg2_group_read_resp, id),
	EI_LEN(0x04, struct sns_reg2_group_read_resp, data_len, 2),
	EI_VNUMS(0x04, struct sns_reg2_group_read_resp, data, SNS_REG2_GROUP_MAX),
	EI_END
};

struct qmi_elem_info sns_reg2_group_write_req_ei[] = {
	EI_NUM(0x01, struct sns_reg2_group_write_req, id),
	EI_LEN(0x02, struct sns_reg2_group_write_req, data_len, 2),
	EI_VNUMS(0x02, struct sns_reg2_group_write_req, data, SNS_REG2_GROUP_MAX),
	EI_END
};

/* ------------------------------------------------------------- TIME2 */

struct qmi_elem_info sns_time2_req_ei[] = {
	EI_OPT(0x10, struct sns_time2_req, reg_report_valid),
	EI_NUM(0x10, struct sns_time2_req, reg_report),
	EI_END
};

struct qmi_elem_info sns_time2_resp_ei[] = {
	EI_STRUCT(0x02, struct sns_time2_resp, resp, sns_resp_ei),
	EI_OPT(0x10, struct sns_time2_resp, dsps_ticks_valid),
	EI_NUM(0x10, struct sns_time2_resp, dsps_ticks),
	EI_OPT(0x11, struct sns_time2_resp, apps_ns_valid),
	EI_NUM(0x11, struct sns_time2_resp, apps_ns),
	EI_OPT(0x12, struct sns_time2_resp, rollover_valid),
	EI_NUM(0x12, struct sns_time2_resp, rollover),
	EI_OPT(0x13, struct sns_time2_resp, status_valid),
	EI_NUM(0x13, struct sns_time2_resp, status),
	EI_OPT(0x14, struct sns_time2_resp, apps_boot_ns_valid),
	EI_NUM(0x14, struct sns_time2_resp, apps_boot_ns),
	EI_END
};

/* -------------------------------------------------------------- SMGR */

static struct qmi_elem_info sns_smgr_short_info_ei[] = {
	EI_NUM(0, struct sns_smgr_short_info, sensor_id),
	EI_LEN(0, struct sns_smgr_short_info, name_len, 1),
	EI_VNUMS(0, struct sns_smgr_short_info, name, SNS_SMGR_SHORT_NAME),
	EI_END
};

struct qmi_elem_info sns_smgr_all_info_resp_ei[] = {
	EI_STRUCT(0x02, struct sns_smgr_all_info_resp, resp, sns_resp_ei),
	EI_LEN(0x03, struct sns_smgr_all_info_resp, n, 1),
	EI_VSTRUCTS(0x03, struct sns_smgr_all_info_resp, info, SNS_SMGR_MAX_SENSORS,
		    sns_smgr_short_info_ei),
	EI_END
};

struct qmi_elem_info sns_smgr_single_info_req_ei[] = {
	EI_NUM(0x01, struct sns_smgr_single_info_req, sensor_id),
	EI_END
};

static struct qmi_elem_info sns_smgr_dt_info_ei[] = {
	EI_NUM(0, struct sns_smgr_dt_info, sensor_id),
	EI_NUM(0, struct sns_smgr_dt_info, data_type),
	EI_LEN(0, struct sns_smgr_dt_info, name_len, 1),
	EI_VNUMS(0, struct sns_smgr_dt_info, name, SNS_SMGR_NAME),
	EI_LEN(0, struct sns_smgr_dt_info, vendor_len, 1),
	EI_VNUMS(0, struct sns_smgr_dt_info, vendor, SNS_SMGR_VENDOR),
	EI_NUM(0, struct sns_smgr_dt_info, version),
	EI_NUM(0, struct sns_smgr_dt_info, max_rate_hz),
	EI_NUM(0, struct sns_smgr_dt_info, idle_power),
	EI_NUM(0, struct sns_smgr_dt_info, max_power),
	EI_NUM(0, struct sns_smgr_dt_info, max_range),
	EI_NUM(0, struct sns_smgr_dt_info, resolution),
	EI_END
};

/* TLV 0x03's value is itself a struct {len; dt[]}; model it as the
 * response struct's own (n, dt[]) pair wrapped in one struct element. */
struct sns_smgr_dt_list {
	uint32_t n;
	struct sns_smgr_dt_info dt[SNS_SMGR_MAX_DT];
};

static struct qmi_elem_info sns_smgr_dt_list_ei[] = {
	EI_LEN(0, struct sns_smgr_dt_list, n, 1),
	EI_VSTRUCTS(0, struct sns_smgr_dt_list, dt, SNS_SMGR_MAX_DT, sns_smgr_dt_info_ei),
	EI_END
};

_Static_assert(offsetof(struct sns_smgr_single_info_resp, dt) -
	       offsetof(struct sns_smgr_single_info_resp, n) ==
	       offsetof(struct sns_smgr_dt_list, dt),
	       "sns_smgr_single_info_resp must embed a sns_smgr_dt_list");

struct qmi_elem_info sns_smgr_single_info_resp_ei[] = {
	EI_STRUCT(0x02, struct sns_smgr_single_info_resp, resp, sns_resp_ei),
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct sns_smgr_dt_list),
	  .array_type = NO_ARRAY, .tlv_type = 0x03,
	  .offset = offsetof(struct sns_smgr_single_info_resp, n),
	  .ei_array = sns_smgr_dt_list_ei },
	EI_END
};

static struct qmi_elem_info sns_smgr_buf_item_ei[] = {
	EI_NUM(0, struct sns_smgr_buf_item, sensor_id),
	EI_NUM(0, struct sns_smgr_buf_item, data_type),
	EI_NUM(0, struct sns_smgr_buf_item, decimation),
	EI_NUM(0, struct sns_smgr_buf_item, calibration),
	EI_NUM(0, struct sns_smgr_buf_item, sampling_rate_hz),
	EI_NUM(0, struct sns_smgr_buf_item, sample_quality),
	EI_END
};

static struct qmi_elem_info sns_suspend_notify_ei[] = {
	EI_NUM(0, struct sns_suspend_notify, proc_type),
	EI_NUM(0, struct sns_suspend_notify, send_during_suspend),
	EI_END
};

struct qmi_elem_info sns_smgr_buf_req_ei[] = {
	EI_NUM(0x01, struct sns_smgr_buf_req, report_id),
	EI_NUM(0x02, struct sns_smgr_buf_req, action),
	EI_NUM(0x03, struct sns_smgr_buf_req, report_rate_q16),
	EI_LEN(0x04, struct sns_smgr_buf_req, n_items, 1),
	EI_VSTRUCTS(0x04, struct sns_smgr_buf_req, item, SNS_SMGR_BUF_ITEMS, sns_smgr_buf_item_ei),
	EI_OPT(0x10, struct sns_smgr_buf_req, notify_valid),
	EI_STRUCT(0x10, struct sns_smgr_buf_req, notify, sns_suspend_notify_ei),
	EI_OPT(0x11, struct sns_smgr_buf_req, src_module_valid),
	EI_NUM(0x11, struct sns_smgr_buf_req, src_module),
	EI_END
};

static struct qmi_elem_info sns_smgr_reason_ei[] = {
	EI_NUM(0, struct sns_smgr_reason, item),
	EI_NUM(0, struct sns_smgr_reason, reason),
	EI_END
};

struct qmi_elem_info sns_smgr_buf_resp_ei[] = {
	EI_STRUCT(0x02, struct sns_smgr_buf_resp, resp, sns_resp_ei),
	EI_OPT(0x10, struct sns_smgr_buf_resp, report_id_valid),
	EI_NUM(0x10, struct sns_smgr_buf_resp, report_id),
	EI_OPT(0x11, struct sns_smgr_buf_resp, ack_nak_valid),
	EI_NUM(0x11, struct sns_smgr_buf_resp, ack_nak),
	EI_OPT(0x12, struct sns_smgr_buf_resp, reasons_valid),
	EI_LEN(0x12, struct sns_smgr_buf_resp, n_reasons, 1),
	EI_VSTRUCTS(0x12, struct sns_smgr_buf_resp, reasons, 10, sns_smgr_reason_ei),
	EI_END
};

static struct qmi_elem_info sns_smgr_buf_index_ei[] = {
	EI_NUM(0, struct sns_smgr_buf_index, sensor_id),
	EI_NUM(0, struct sns_smgr_buf_index, data_type),
	EI_NUM(0, struct sns_smgr_buf_index, first),
	EI_NUM(0, struct sns_smgr_buf_index, count),
	EI_NUM(0, struct sns_smgr_buf_index, first_ts),
	EI_NUM(0, struct sns_smgr_buf_index, rate_q16),
	EI_END
};

static struct qmi_elem_info sns_smgr_buf_sample_ei[] = {
	EI_NUMS(0, struct sns_smgr_buf_sample, data, 3),
	EI_NUM(0, struct sns_smgr_buf_sample, ts_offset),
	EI_NUM(0, struct sns_smgr_buf_sample, flags),
	EI_NUM(0, struct sns_smgr_buf_sample, quality),
	EI_END
};

struct qmi_elem_info sns_smgr_buf_ind_ei[] = {
	EI_NUM(0x01, struct sns_smgr_buf_ind, report_id),
	EI_LEN(0x02, struct sns_smgr_buf_ind, n_index, 1),
	EI_VSTRUCTS(0x02, struct sns_smgr_buf_ind, index, SNS_SMGR_BUF_ITEMS, sns_smgr_buf_index_ei),
	EI_LEN(0x03, struct sns_smgr_buf_ind, n_samples, 1),
	EI_VSTRUCTS(0x03, struct sns_smgr_buf_ind, samples, SNS_SMGR_BUF_SAMPLES,
		    sns_smgr_buf_sample_ei),
	EI_OPT(0x10, struct sns_smgr_buf_ind, ind_type_valid),
	EI_NUM(0x10, struct sns_smgr_buf_ind, ind_type),
	EI_END
};

static struct qmi_elem_info sns_smgr_rep_item_ei[] = {
	EI_NUM(0, struct sns_smgr_rep_item, sensor_id),
	EI_NUM(0, struct sns_smgr_rep_item, data_type),
	EI_NUM(0, struct sns_smgr_rep_item, sensitivity),
	EI_NUM(0, struct sns_smgr_rep_item, decimation),
	EI_NUM(0, struct sns_smgr_rep_item, min_sample_rate),
	EI_NUM(0, struct sns_smgr_rep_item, stationary),
	EI_NUM(0, struct sns_smgr_rep_item, threshold_test),
	EI_NUM(0, struct sns_smgr_rep_item, threshold_outside),
	EI_NUM(0, struct sns_smgr_rep_item, threshold_delta),
	EI_NUM(0, struct sns_smgr_rep_item, threshold_all_axes),
	EI_NUMS(0, struct sns_smgr_rep_item, threshold, 2),
	EI_END
};

struct qmi_elem_info sns_smgr_rep_req_ei[] = {
	EI_NUM(0x01, struct sns_smgr_rep_req, report_id),
	EI_NUM(0x02, struct sns_smgr_rep_req, action),
	EI_NUM(0x03, struct sns_smgr_rep_req, report_rate_hz),
	EI_NUM(0x04, struct sns_smgr_rep_req, buffer_factor),
	EI_LEN(0x05, struct sns_smgr_rep_req, n_items, 1),
	EI_VSTRUCTS(0x05, struct sns_smgr_rep_req, item, SNS_SMGR_REP_ITEMS, sns_smgr_rep_item_ei),
	EI_END
};

struct qmi_elem_info sns_smgr_rep_resp_ei[] = {
	EI_STRUCT(0x02, struct sns_smgr_rep_resp, resp, sns_resp_ei),
	EI_NUM(0x03, struct sns_smgr_rep_resp, report_id),
	EI_NUM(0x04, struct sns_smgr_rep_resp, ack_nak),
	EI_LEN(0x05, struct sns_smgr_rep_resp, n_reasons, 1),
	EI_VSTRUCTS(0x05, struct sns_smgr_rep_resp, reasons, 10, sns_smgr_reason_ei),
	EI_END
};

static struct qmi_elem_info sns_smgr_rep_data_ei[] = {
	EI_NUM(0, struct sns_smgr_rep_data, sensor_id),
	EI_NUM(0, struct sns_smgr_rep_data, data_type),
	EI_NUMS(0, struct sns_smgr_rep_data, data, 3),
	EI_NUM(0, struct sns_smgr_rep_data, ts),
	EI_NUM(0, struct sns_smgr_rep_data, flags),
	EI_NUM(0, struct sns_smgr_rep_data, quality),
	EI_NUM(0, struct sns_smgr_rep_data, sensitivity),
	EI_END
};

struct qmi_elem_info sns_smgr_rep_ind_ei[] = {
	EI_NUM(0x01, struct sns_smgr_rep_ind, report_id),
	EI_NUM(0x02, struct sns_smgr_rep_ind, status),
	EI_NUM(0x03, struct sns_smgr_rep_ind, current_rate),
	EI_LEN(0x04, struct sns_smgr_rep_ind, n_items, 1),
	EI_VSTRUCTS(0x04, struct sns_smgr_rep_ind, item, SNS_SMGR_REP_ITEMS, sns_smgr_rep_data_ei),
	EI_END
};

/* ------------------------------------------------- SAM QMAG_CAL */

struct qmi_elem_info sns_sam_enable_req_ei[] = {
	EI_OPT(0x10, struct sns_sam_enable_req, period_valid),
	EI_NUM(0x10, struct sns_sam_enable_req, period_q16),
	EI_END
};

struct qmi_elem_info sns_sam_instance_resp_ei[] = {
	EI_STRUCT(0x02, struct sns_sam_instance_resp, resp, sns_resp_ei),
	EI_OPT(0x10, struct sns_sam_instance_resp, instance_valid),
	EI_NUM(0x10, struct sns_sam_instance_resp, instance),
	EI_END
};

struct qmi_elem_info sns_sam_disable_req_ei[] = {
	EI_NUM(0x01, struct sns_sam_disable_req, instance),
	EI_END
};

struct qmi_elem_info sns_sam_qmag_ind_ei[] = {
	EI_NUM(0x01, struct sns_sam_qmag_ind, instance),
	EI_NUM(0x02, struct sns_sam_qmag_ind, timestamp),
	EI_NUMS(0x03, struct sns_sam_qmag_ind, bias, 3),
	EI_NUM(0x04, struct sns_sam_qmag_ind, accuracy),
	EI_END
};

struct qmi_elem_info sns_sam_error_ind_ei[] = {
	EI_NUM(0x01, struct sns_sam_error_ind, error),
	EI_NUM(0x02, struct sns_sam_error_ind, instance),
	EI_END
};

#define EI_ATTR(tlv, i) { \
	.data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = 4, \
	.array_type = NO_ARRAY, .tlv_type = (tlv), \
	.offset = offsetof(struct sns_sam_attr_resp, attr) + 4 * (i) }

struct qmi_elem_info sns_sam_attr_resp_ei[] = {
	EI_STRUCT(0x02, struct sns_sam_attr_resp, resp, sns_resp_ei),
	EI_ATTR(0x03, 0), EI_ATTR(0x04, 1), EI_ATTR(0x05, 2),
	EI_ATTR(0x06, 3), EI_ATTR(0x07, 4), EI_ATTR(0x08, 5),
	EI_ATTR(0x09, 6), EI_ATTR(0x0a, 7), EI_ATTR(0x0b, 8),
	EI_OPT(0x10, struct sns_sam_attr_resp, suid_valid),
	EI_NUM(0x10, struct sns_sam_attr_resp, suid),
	EI_OPT(0x11, struct sns_sam_attr_resp, reserved_valid),
	EI_NUM(0x11, struct sns_sam_attr_resp, reserved),
	EI_END
};

/* ------------------------------------------ SAM ROTATION_VECTOR */

static struct qmi_elem_info sns_sam_notify_suspend_ei[] = {
	EI_NUM(0, struct sns_sam_notify_suspend, proc_type),
	EI_NUM(0, struct sns_sam_notify_suspend, send_during_suspend),
	EI_END
};

struct qmi_elem_info sns_rotvec_enable_req_ei[] = {
	EI_NUM(0x01, struct sns_rotvec_enable_req, period_q16),
	EI_OPT(0x10, struct sns_rotvec_enable_req, rate_valid),
	EI_NUM(0x10, struct sns_rotvec_enable_req, rate_q16),
	EI_OPT(0x11, struct sns_rotvec_enable_req, coord_valid),
	EI_NUM(0x11, struct sns_rotvec_enable_req, coord),
	EI_OPT(0x12, struct sns_rotvec_enable_req, notify_valid),
	EI_STRUCT(0x12, struct sns_rotvec_enable_req, notify, sns_sam_notify_suspend_ei),
	EI_END
};

static struct qmi_elem_info sns_rotvec_result_ei[] = {
	EI_NUMS(0, struct sns_rotvec_result, q, 4),
	EI_NUM(0, struct sns_rotvec_result, accuracy),
	EI_NUM(0, struct sns_rotvec_result, coord),
	EI_END
};

struct qmi_elem_info sns_sam_rotvec_ind_ei[] = {
	EI_NUM(0x01, struct sns_sam_rotvec_ind, instance),
	EI_NUM(0x02, struct sns_sam_rotvec_ind, timestamp),
	EI_STRUCT(0x03, struct sns_sam_rotvec_ind, r, sns_rotvec_result_ei),
	EI_END
};
