/* sns_msgs: our own C definitions of the Sensors1 QMI messages sensord
 * uses, with qmi_elem_info tables for the vendored codec (../qrtr/qmi.c).
 *
 * Written by hand from the IDL tables compiled into the stock binaries
 * (tools/sns-idl-dump.py; dump in logs/sns-idl-dump-2026-09-26.txt). No
 * vendor header was used or copied: the field names below are ours, the
 * TLV numbers, element types and array bounds are the dump's, and the
 * meaning of each field is our reading of how the stock HAL
 * (sensors.ssc.so) fills or consumes it, noted per field. The C layout
 * here only has to agree with the ei tables below, not with the vendor's
 * structs; the wire format is what must match.
 *
 * Wire conventions (common to every SNS service, see the dump):
 *  - the response TLV 0x02 is 2 bytes, u8 result + u8 error (NOT the
 *    4-byte qmi_response_type_v01 of other QMI services);
 *  - variable arrays carry a u8 element count on the wire, u16 when the
 *    bound exceeds 255 (REG2 group data), decoded into a u32 *_len field;
 *  - every service answers msg 0x00 (cancel) and 0x01 (version).
 */
#ifndef CHEF_CYCLO_SNS_MSGS_H
#define CHEF_CYCLO_SNS_MSGS_H

#include <stdint.h>

#include "libqrtr.h"

/* ---------------------------------------------------------- services */

/* Instances as dump_servers prints them (instance << 8 | version); see
 * logs/stock-sensors-survey-2026-09-26.txt. */
#define SNS_SMGR_SVC		0x100	/* ADSP, inst 0x3201 */
#define SNS_SMGR_VERS		1
#define SNS_SMGR_INST		0x32
#define SNS_REG2_SVC		0x10f	/* apps (sensors.qti on stock), inst 0x0002 */
#define SNS_REG2_VERS		2
#define SNS_REG2_INST		0
#define SNS_TIME2_SVC		0x118	/* apps inst 0x3202, ADSP inst 0x0a02 */
#define SNS_TIME2_VERS		2
#define SNS_TIME2_APPS_INST	0x32

/* ------------------------------------------------------- common part */

#define SNS_MSG_CANCEL		0x00
#define SNS_MSG_VERSION		0x01

#define SNS_RESULT_SUCCESS	0
#define SNS_RESULT_FAILURE	1
/* Error codes, as sensors.qti's registry lookup returns them (5 = the
 * storage layer failed, 7 = no such item/group); the rest of the
 * numbering is the sensor1 client API's. */
#define SNS_ERR_NONE		0
#define SNS_ERR_FAILED		5
#define SNS_ERR_BAD_PARAM	7
#define SNS_ERR_BAD_MSG_ID	9

struct sns_resp {
	uint8_t result;
	uint8_t err;
};

/* Any response that is only the result TLV (cancel, writes, delete...). */
struct sns_generic_resp {
	struct sns_resp resp;
};

/* msg 0x01 response: TLV 0x03 u32, TLV 0x04 u16. We read them as the
 * interface (IDL minor) version and the highest message ID, which is
 * what the numbers are for the services whose IDL version we know. */
struct sns_version_resp {
	struct sns_resp resp;
	uint32_t interface_version;
	uint16_t max_msg_id;
};

extern struct qmi_elem_info sns_generic_resp_ei[];
extern struct qmi_elem_info sns_version_resp_ei[];

/* ------------------------------------------------------ REG2 (0x10f) */

#define SNS_REG2_ITEM_READ	0x02
#define SNS_REG2_ITEM_WRITE	0x03
#define SNS_REG2_GROUP_READ	0x04
#define SNS_REG2_GROUP_WRITE	0x05
#define SNS_REG2_MSG_06		0x06	/* meaning unknown: u8[<=256] + opt u16,u16,u8 */
#define SNS_REG2_MAX_MSG_ID	0x06
#define SNS_REG2_IDL_MINOR	48
#define SNS_REG2_ITEM_MAX	8
#define SNS_REG2_GROUP_MAX	256

struct sns_reg2_id_req {		/* ITEM_READ and GROUP_READ: TLV 0x01 u16 */
	uint16_t id;
};

struct sns_reg2_item_read_resp {
	struct sns_resp resp;
	uint16_t id;			/* TLV 0x03 */
	uint32_t data_len;		/* TLV 0x04 u8[<=8], wire len u8 */
	uint8_t data[SNS_REG2_ITEM_MAX];
};

struct sns_reg2_item_write_req {
	uint16_t id;			/* TLV 0x01 */
	uint32_t data_len;		/* TLV 0x02 u8[<=8] */
	uint8_t data[SNS_REG2_ITEM_MAX];
};

struct sns_reg2_group_read_resp {
	struct sns_resp resp;
	uint16_t id;			/* TLV 0x03 */
	uint32_t data_len;		/* TLV 0x04 u8[<=256], wire len u16 */
	uint8_t data[SNS_REG2_GROUP_MAX];
};

struct sns_reg2_group_write_req {
	uint16_t id;			/* TLV 0x01 */
	uint32_t data_len;		/* TLV 0x02 u8[<=256], wire len u16 */
	uint8_t data[SNS_REG2_GROUP_MAX];
};

extern struct qmi_elem_info sns_reg2_id_req_ei[];
extern struct qmi_elem_info sns_reg2_item_read_resp_ei[];
extern struct qmi_elem_info sns_reg2_item_write_req_ei[];
extern struct qmi_elem_info sns_reg2_group_read_resp_ei[];
extern struct qmi_elem_info sns_reg2_group_write_req_ei[];

/* ----------------------------------------------------- TIME2 (0x118) */

#define SNS_TIME2_TIMESTAMP	0x02
#define SNS_TIME2_MAX_MSG_ID	0x03
#define SNS_TIME2_IDL_MINOR	5

/* msg 0x02 request: TLV 0x10 optional u8, read as "also send periodic
 * indications (0x03)". sensord never sends them; it logs the flag. */
struct sns_time2_req {
	uint8_t reg_report_valid;
	uint8_t reg_report;
};

/* msg 0x02 response, every field optional. Our reading of the TLVs: 0x10
 * the DSP 32768 Hz tick count, 0x11 the apps time in ns, 0x12 the tick
 * counter's rollover count, 0x13 an error/status word, 0x14 the apps
 * boot time in ns. Unverified: sensord only serves this with -t, and
 * logs every request so the live run can tell whether anything asks. */
struct sns_time2_resp {
	struct sns_resp resp;
	uint8_t dsps_ticks_valid;
	uint32_t dsps_ticks;
	uint8_t apps_ns_valid;
	uint64_t apps_ns;
	uint8_t rollover_valid;
	uint32_t rollover;
	uint8_t status_valid;
	uint32_t status;
	uint8_t apps_boot_ns_valid;
	uint64_t apps_boot_ns;
};

extern struct qmi_elem_info sns_time2_req_ei[];
extern struct qmi_elem_info sns_time2_resp_ei[];

/* ------------------------------------------------------ SMGR (0x100) */

#define SNS_SMGR_REPORT		0x02	/* periodic report add/delete */
#define SNS_SMGR_REPORT_IND	0x03
#define SNS_SMGR_ALL_INFO	0x05
#define SNS_SMGR_SINGLE_INFO	0x06
#define SNS_SMGR_BUFFERING	0x21	/* what the stock HAL uses */
#define SNS_SMGR_BUFFERING_IND	0x22

#define SNS_SMGR_ACTION_ADD	1
#define SNS_SMGR_ACTION_DELETE	2

/* SMGR sensor IDs the stock HAL hard-codes in its prepareAddMsg()s. */
#define SNS_SMGR_ID_ACCEL	0
#define SNS_SMGR_ID_GYRO	10
#define SNS_SMGR_ID_MAG		20
#define SNS_SMGR_ID_PROX_LIGHT	40	/* data type 0 proximity, 1 light */
#define SNS_SMGR_DT_PRIMARY	0
#define SNS_SMGR_DT_SECONDARY	1

/* Item fields as the HAL sets them: decimation 3 normally (1 for its
 * "raw data mode"), calibration 0 full / 1 factory (uncalibrated
 * sensors) / 2 raw, sample quality 1. */
#define SNS_SMGR_DECIMATION_DEFAULT	3
#define SNS_SMGR_CAL_FULL		0
#define SNS_SMGR_SAMPLE_QUALITY_DEFAULT	1

#define SNS_SMGR_MAX_SENSORS	20
#define SNS_SMGR_MAX_DT		3
#define SNS_SMGR_SHORT_NAME	16
#define SNS_SMGR_NAME		80
#define SNS_SMGR_VENDOR		20

/* ALL_SENSOR_INFO (0x05): empty request. */
struct sns_smgr_short_info {
	uint8_t sensor_id;
	uint32_t name_len;
	uint8_t name[SNS_SMGR_SHORT_NAME];
};

struct sns_smgr_all_info_resp {
	struct sns_resp resp;
	uint32_t n;			/* TLV 0x03 var[<=20] */
	struct sns_smgr_short_info info[SNS_SMGR_MAX_SENSORS];
};

/* SINGLE_SENSOR_INFO (0x06): TLV 0x01 u8 sensor id. */
struct sns_smgr_single_info_req {
	uint8_t sensor_id;
};

struct sns_smgr_dt_info {
	uint8_t sensor_id;
	uint8_t data_type;
	uint32_t name_len;
	uint8_t name[SNS_SMGR_NAME];
	uint32_t vendor_len;
	uint8_t vendor[SNS_SMGR_VENDOR];
	uint32_t version;
	uint16_t max_rate_hz;
	uint16_t idle_power;
	uint16_t max_power;
	uint32_t max_range;		/* Q16, in the channel's SMGR unit */
	uint32_t resolution;		/* Q16 */
};

/* TLV 0x03 is a struct holding the var[<=3] data-type array; optional
 * TLVs 0x10..0x14 (buffer depths, suids, ODR lists) are skipped. */
struct sns_smgr_single_info_resp {
	struct sns_resp resp;
	uint32_t n;
	struct sns_smgr_dt_info dt[SNS_SMGR_MAX_DT];
};

/* BUFFERING (0x21): one or more items, samples batched per report period. */
#define SNS_SMGR_BUF_ITEMS	5
#define SNS_SMGR_BUF_SAMPLES	100

struct sns_smgr_buf_item {
	uint8_t sensor_id;
	uint8_t data_type;
	uint8_t decimation;
	uint8_t calibration;
	uint16_t sampling_rate_hz;	/* > 1000 would mean a period in ms */
	uint16_t sample_quality;
};

struct sns_suspend_notify {
	uint32_t proc_type;		/* 0 = apps */
	uint8_t send_during_suspend;
};

struct sns_smgr_buf_req {
	uint8_t report_id;		/* TLV 0x01 */
	uint8_t action;			/* TLV 0x02 */
	uint32_t report_rate_q16;	/* TLV 0x03 reports per second, Q16 */
	uint32_t n_items;		/* TLV 0x04 var[<=5] */
	struct sns_smgr_buf_item item[SNS_SMGR_BUF_ITEMS];
	uint8_t notify_valid;		/* TLV 0x10 */
	struct sns_suspend_notify notify;
	uint8_t src_module_valid;	/* TLV 0x11 */
	uint8_t src_module;
};

struct sns_smgr_reason {
	uint8_t item;
	uint8_t reason;
};

struct sns_smgr_buf_resp {
	struct sns_resp resp;
	uint8_t report_id_valid;	/* TLV 0x10 */
	uint8_t report_id;
	uint8_t ack_nak_valid;		/* TLV 0x11: 0 accepted, 1 accepted with changes */
	uint8_t ack_nak;
	uint8_t reasons_valid;		/* TLV 0x12 var[<=10] */
	uint32_t n_reasons;
	struct sns_smgr_reason reasons[10];
};

struct sns_smgr_buf_index {
	uint8_t sensor_id;
	uint8_t data_type;
	uint8_t first;			/* index into samples[] */
	uint8_t count;
	uint32_t first_ts;		/* DSP 32768 Hz ticks */
	uint32_t rate_q16;
};

struct sns_smgr_buf_sample {
	int32_t data[3];		/* Q16, SMGR axes */
	uint16_t ts_offset;		/* ticks since the previous sample */
	uint8_t flags;
	uint8_t quality;
};

struct sns_smgr_buf_ind {
	uint8_t report_id;		/* TLV 0x01 */
	uint32_t n_index;		/* TLV 0x02 var[<=5] */
	struct sns_smgr_buf_index index[SNS_SMGR_BUF_ITEMS];
	uint32_t n_samples;		/* TLV 0x03 var[<=100] */
	struct sns_smgr_buf_sample samples[SNS_SMGR_BUF_SAMPLES];
	uint8_t ind_type_valid;		/* TLV 0x10 */
	uint8_t ind_type;
};

/* Periodic REPORT (0x02/0x03): the older one-sample-per-indication API;
 * sensord uses it only with -P. */
#define SNS_SMGR_REP_ITEMS	10

struct sns_smgr_rep_item {
	uint8_t sensor_id;
	uint8_t data_type;
	uint8_t sensitivity;
	uint8_t decimation;
	uint16_t min_sample_rate;
	uint8_t stationary;
	uint8_t threshold_test;
	uint8_t threshold_outside;
	uint8_t threshold_delta;
	uint8_t threshold_all_axes;
	int32_t threshold[2];
};

struct sns_smgr_rep_req {
	uint8_t report_id;		/* TLV 0x01 */
	uint8_t action;			/* TLV 0x02 */
	uint16_t report_rate_hz;	/* TLV 0x03 */
	uint8_t buffer_factor;		/* TLV 0x04 */
	uint32_t n_items;		/* TLV 0x05 var[<=10] */
	struct sns_smgr_rep_item item[SNS_SMGR_REP_ITEMS];
};

struct sns_smgr_rep_resp {
	struct sns_resp resp;
	uint8_t report_id;		/* TLV 0x03 */
	uint8_t ack_nak;		/* TLV 0x04 */
	uint32_t n_reasons;		/* TLV 0x05 var[<=10] */
	struct sns_smgr_reason reasons[10];
};

struct sns_smgr_rep_data {
	uint8_t sensor_id;
	uint8_t data_type;
	int32_t data[3];
	uint32_t ts;			/* DSP ticks */
	uint8_t flags;
	uint8_t quality;
	uint8_t sensitivity;
};

struct sns_smgr_rep_ind {
	uint8_t report_id;		/* TLV 0x01 */
	uint8_t status;			/* TLV 0x02 */
	uint16_t current_rate;		/* TLV 0x03 */
	uint32_t n_items;		/* TLV 0x04 var[<=10] */
	struct sns_smgr_rep_data item[SNS_SMGR_REP_ITEMS];
};

extern struct qmi_elem_info sns_empty_ei[];
extern struct qmi_elem_info sns_smgr_all_info_resp_ei[];
extern struct qmi_elem_info sns_smgr_single_info_req_ei[];
extern struct qmi_elem_info sns_smgr_single_info_resp_ei[];
extern struct qmi_elem_info sns_smgr_buf_req_ei[];
extern struct qmi_elem_info sns_smgr_buf_resp_ei[];
extern struct qmi_elem_info sns_smgr_buf_ind_ei[];
extern struct qmi_elem_info sns_smgr_rep_req_ei[];
extern struct qmi_elem_info sns_smgr_rep_resp_ei[];
extern struct qmi_elem_info sns_smgr_rep_ind_ei[];

/* QMI header (type u8, txn u16, msg id u16, length u16, packed). */
#define SNS_QMI_HDR_LEN 7

#endif
