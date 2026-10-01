/* Wire-format tests for sns_msgs.c: every message sensord sends is
 * encoded and compared byte for byte with a hand-assembled buffer, and
 * every message it receives is decoded from one. The hand-assembled
 * bytes follow logs/sns-idl-dump-2026-09-26.txt (TLV numbers, element
 * sizes, u8 vs u16 array counts), which is what ties sensord to the
 * stock IDL rather than to its own tables. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sns_msgs.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

static void dump(const char *what, const uint8_t *p, size_t n)
{
	size_t i;

	fprintf(stderr, "%s:", what);
	for (i = 0; i < n; i++)
		fprintf(stderr, " %02x", p[i]);
	fprintf(stderr, "\n");
}

/* Encode and compare with header + expected TLV bytes. */
static void expect_encoded(const char *name, int type, int msg, int txn, const void *c,
			   struct qmi_elem_info *ei, const uint8_t *tlv, size_t tlvlen)
{
	DEFINE_QRTR_PACKET(pkt, 2048);
	uint8_t want[2048];
	ssize_t n;

	n = qmi_encode_message(&pkt, type, msg, txn, c, ei);
	want[0] = (uint8_t)type;
	want[1] = (uint8_t)txn;
	want[2] = (uint8_t)(txn >> 8);
	want[3] = (uint8_t)msg;
	want[4] = (uint8_t)(msg >> 8);
	want[5] = (uint8_t)tlvlen;
	want[6] = (uint8_t)(tlvlen >> 8);
	if (tlvlen)
		memcpy(want + 7, tlv, tlvlen);
	if (n != (ssize_t)(7 + tlvlen) || memcmp(pkt.data, want, 7 + tlvlen)) {
		fprintf(stderr, "%s: encoding mismatch (got %zd bytes, want %zu)\n", name, n, 7 + tlvlen);
		if (n > 0)
			dump("  got ", pkt.data, (size_t)n);
		dump("  want", want, 7 + tlvlen);
		failures++;
	}
}

/* Wrap TLV bytes in a QMI header and decode them. The buffer is an
 * exact-size heap block, so the ASan build catches any over-read. */
static int decode(int type, int msg, const uint8_t *tlv, size_t tlvlen, void *c,
		  struct qmi_elem_info *ei)
{
	uint8_t *buf = malloc(7 + tlvlen);
	struct qrtr_packet pkt;
	unsigned int txn;
	int rc;

	buf[0] = (uint8_t)type;
	buf[1] = 7;
	buf[2] = 0;
	buf[3] = (uint8_t)msg;
	buf[4] = (uint8_t)(msg >> 8);
	buf[5] = (uint8_t)tlvlen;
	buf[6] = (uint8_t)(tlvlen >> 8);
	memcpy(buf + 7, tlv, tlvlen);
	memset(&pkt, 0, sizeof(pkt));
	pkt.data = buf;
	pkt.data_len = 7 + tlvlen;
	rc = qmi_decode_message(c, &txn, &pkt, type, msg, ei);
	free(buf);
	return rc;
}

static void test_common(void)
{
	struct sns_generic_resp g = { { SNS_RESULT_FAILURE, SNS_ERR_BAD_PARAM } };
	struct sns_version_resp v = { { 0, 0 }, 48, 6 };
	static const uint8_t g_tlv[] = { 0x02, 0x02, 0x00, 0x01, 0x07 };
	static const uint8_t v_tlv[] = { 0x02, 0x02, 0x00, 0x00, 0x00,
					  0x03, 0x04, 0x00, 0x30, 0x00, 0x00, 0x00,
					  0x04, 0x02, 0x00, 0x06, 0x00 };

	/* The SNS response TLV is 2 bytes, not the 4-byte QMI one. */
	expect_encoded("generic resp", QMI_RESPONSE, SNS_REG2_ITEM_WRITE, 0x1234, &g,
		       sns_generic_resp_ei, g_tlv, sizeof(g_tlv));
	expect_encoded("version resp", QMI_RESPONSE, SNS_MSG_VERSION, 1, &v, sns_version_resp_ei,
		       v_tlv, sizeof(v_tlv));
}

static void test_reg2(void)
{
	struct sns_reg2_id_req id = { 0 };
	struct sns_reg2_item_read_resp ir;
	static struct sns_reg2_group_read_resp gr;
	static struct sns_reg2_group_write_req gw;
	struct sns_reg2_item_write_req iw;
	static const uint8_t id_tlv[] = { 0x01, 0x02, 0x00, 0x87, 0x0a };
	uint8_t ir_tlv[] = { 0x02, 0x02, 0x00, 0x00, 0x00,
			     0x03, 0x02, 0x00, 0x06, 0x09,
			     0x04, 0x03, 0x00, 0x02, 0x03, 0x00 };
	uint8_t gr_tlv[5 + 5 + 3 + 2 + 40], gw_tlv[5 + 3 + 2 + 3];
	static const uint8_t iw_tlv[] = { 0x01, 0x02, 0x00, 0xbc, 0x02,
					  0x02, 0x05, 0x00, 0x04, 0x11, 0x22, 0x33, 0x44 };
	size_t o = 0;
	int i;

	/* item/group read request: TLV 0x01 u16 */
	CHECK(decode(QMI_REQUEST, SNS_REG2_GROUP_READ, id_tlv, sizeof(id_tlv), &id,
		     sns_reg2_id_req_ei) >= 0);
	CHECK(id.id == 2695);

	/* item read response: u8[<=8] with a u8 count */
	memset(&ir, 0, sizeof(ir));
	ir.id = 2310;		/* 0x0906 */
	ir.data_len = 2;
	ir.data[0] = 3;
	expect_encoded("item read resp", QMI_RESPONSE, SNS_REG2_ITEM_READ, 2, &ir,
		       sns_reg2_item_read_resp_ei, ir_tlv, sizeof(ir_tlv));

	/* group read response: u8[<=256] with a u16 count */
	memset(&gr, 0, sizeof(gr));
	gr.id = 2695;
	gr.data_len = 40;
	for (i = 0; i < 40; i++)
		gr.data[i] = (uint8_t)i;
	memcpy(gr_tlv + o, "\x02\x02\x00\x00\x00", 5);
	o += 5;
	memcpy(gr_tlv + o, "\x03\x02\x00\x87\x0a", 5);
	o += 5;
	gr_tlv[o++] = 0x04;
	gr_tlv[o++] = 42;
	gr_tlv[o++] = 0;
	gr_tlv[o++] = 40;
	gr_tlv[o++] = 0;
	for (i = 0; i < 40; i++)
		gr_tlv[o++] = (uint8_t)i;
	expect_encoded("group read resp", QMI_RESPONSE, SNS_REG2_GROUP_READ, 3, &gr,
		       sns_reg2_group_read_resp_ei, gr_tlv, o);

	/* group write request decodes the u16 count */
	o = 0;
	memcpy(gw_tlv + o, "\x01\x02\x00\xe8\x03", 5);
	o += 5;
	memcpy(gw_tlv + o, "\x02\x05\x00\x03\x00\xaa\xbb\xcc", 8);
	o += 8;
	memset(&gw, 0, sizeof(gw));
	CHECK(decode(QMI_REQUEST, SNS_REG2_GROUP_WRITE, gw_tlv, o, &gw,
		     sns_reg2_group_write_req_ei) >= 0);
	CHECK(gw.id == 1000 && gw.data_len == 3 && gw.data[0] == 0xaa && gw.data[2] == 0xcc);

	memset(&iw, 0, sizeof(iw));
	CHECK(decode(QMI_REQUEST, SNS_REG2_ITEM_WRITE, iw_tlv, sizeof(iw_tlv), &iw,
		     sns_reg2_item_write_req_ei) >= 0);
	CHECK(iw.id == 700 && iw.data_len == 4 && iw.data[3] == 0x44);
}

static void test_time2(void)
{
	struct sns_time2_req q;
	struct sns_time2_resp r;
	static const uint8_t q_tlv[] = { 0x10, 0x01, 0x00, 0x01 };
	static const uint8_t r_tlv[] = { 0x02, 0x02, 0x00, 0x00, 0x00,
					 0x10, 0x04, 0x00, 0x78, 0x56, 0x34, 0x12,
					 0x11, 0x08, 0x00, 1, 2, 3, 4, 5, 6, 7, 8 };

	memset(&q, 0, sizeof(q));
	CHECK(decode(QMI_REQUEST, SNS_TIME2_TIMESTAMP, q_tlv, sizeof(q_tlv), &q, sns_time2_req_ei) >= 0);
	CHECK(q.reg_report_valid && q.reg_report == 1);
	memset(&r, 0, sizeof(r));
	r.dsps_ticks_valid = 1;
	r.dsps_ticks = 0x12345678;
	r.apps_ns_valid = 1;
	r.apps_ns = 0x0807060504030201ULL;
	expect_encoded("time2 resp", QMI_RESPONSE, SNS_TIME2_TIMESTAMP, 4, &r, sns_time2_resp_ei,
		       r_tlv, sizeof(r_tlv));
}

static void test_smgr_info(void)
{
	static struct sns_smgr_all_info_resp all;
	static struct sns_smgr_single_info_resp one;
	struct sns_smgr_single_info_req q = { 40 };
	static const uint8_t q_tlv[] = { 0x01, 0x01, 0x00, 0x28 };
	uint8_t tlv[512];
	size_t o = 0, lenpos;
	int i;

	expect_encoded("single info req", QMI_REQUEST, SNS_SMGR_SINGLE_INFO, 9, &q,
		       sns_smgr_single_info_req_ei, q_tlv, sizeof(q_tlv));
	expect_encoded("all info req", QMI_REQUEST, SNS_SMGR_ALL_INFO, 8, NULL, sns_empty_ei, NULL, 0);

	/* ALL_SENSOR_INFO: TLV 0x03 = u8 count, then {u8 id, u8 len, name} */
	memcpy(tlv, "\x02\x02\x00\x00\x00\x03\x0e\x00\x02\x00\x05" "ACCEL\x28\x04" "PROX", 22);
	memset(&all, 0, sizeof(all));
	CHECK(decode(QMI_RESPONSE, SNS_SMGR_ALL_INFO, tlv, 22, &all, sns_smgr_all_info_resp_ei) >= 0);
	CHECK(all.n == 2 && all.info[0].sensor_id == 0 && all.info[0].name_len == 5 &&
	      memcmp(all.info[0].name, "ACCEL", 5) == 0 && all.info[1].sensor_id == 40 &&
	      all.info[1].name_len == 4);

	/* SINGLE_SENSOR_INFO: TLV 0x03 wraps u8 count + data-type records;
	 * an optional TLV sensord does not model (0x12, u64 suids) follows. */
	memcpy(tlv + o, "\x02\x02\x00\x00\x00", 5);
	o += 5;
	tlv[o++] = 0x03;
	lenpos = o;
	o += 2;
	tlv[o++] = 2;
	for (i = 0; i < 2; i++) {
		const char *name = i ? "EPL ALS" : "EPL PROX";

		tlv[o++] = 40;
		tlv[o++] = (uint8_t)i;
		tlv[o++] = (uint8_t)strlen(name);
		memcpy(tlv + o, name, strlen(name));
		o += strlen(name);
		tlv[o++] = 7;
		memcpy(tlv + o, "Eminent", 7);
		o += 7;
		memcpy(tlv + o, "\x02\x00\x00\x00", 4);		/* version 2 */
		o += 4;
		memcpy(tlv + o, "\x0a\x00\x01\x00\x02\x00", 6);	/* 10 Hz, powers */
		o += 6;
		memcpy(tlv + o, "\x00\x00\x05\x00", 4);		/* range 5.0 Q16 */
		o += 4;
		memcpy(tlv + o, "\x00\x80\x00\x00", 4);		/* res 0.5 Q16 */
		o += 4;
	}
	tlv[lenpos] = (uint8_t)(o - lenpos - 2);
	tlv[lenpos + 1] = 0;
	memcpy(tlv + o, "\x12\x09\x00\x01\x11\x22\x33\x44\x55\x66\x77\x88", 12);
	o += 12;
	memset(&one, 0, sizeof(one));
	CHECK(decode(QMI_RESPONSE, SNS_SMGR_SINGLE_INFO, tlv, o, &one,
		     sns_smgr_single_info_resp_ei) >= 0);
	CHECK(one.n == 2);
	CHECK(one.dt[0].sensor_id == 40 && one.dt[0].data_type == 0 && one.dt[0].name_len == 8 &&
	      memcmp(one.dt[0].name, "EPL PROX", 8) == 0);
	CHECK(one.dt[1].data_type == 1 && one.dt[1].vendor_len == 7 && one.dt[1].version == 2 &&
	      one.dt[1].max_rate_hz == 10 && one.dt[1].max_range == 0x50000 &&
	      one.dt[1].resolution == 0x8000);
}

static void test_smgr_buffering(void)
{
	struct sns_smgr_buf_req q;
	struct sns_smgr_buf_resp r;
	static struct sns_smgr_buf_ind ind;
	/* 01 report id, 02 action, 03 u32 Q16 rate, 04 u8 count + 8-byte
	 * items, 0x10 suspend notify {u32 proc, u8 send} */
	static const uint8_t q_tlv[] = {
		0x01, 0x01, 0x00, 0x01,
		0x02, 0x01, 0x00, 0x01,
		0x03, 0x04, 0x00, 0x00, 0x00, 0x32, 0x00,
		0x04, 0x09, 0x00, 0x01, 0x00, 0x00, 0x03, 0x00, 0x32, 0x00, 0x01, 0x00,
		0x10, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	static const uint8_t del_tlv[] = {
		0x01, 0x01, 0x00, 0x03,
		0x02, 0x01, 0x00, 0x02,
		0x03, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x04, 0x01, 0x00, 0x00 };
	static const uint8_t r_tlv[] = {
		0x02, 0x02, 0x00, 0x00, 0x00,
		0x10, 0x01, 0x00, 0x01,
		0x11, 0x01, 0x00, 0x01,
		0x12, 0x03, 0x00, 0x01, 0x00, 0x05 };
	uint8_t ind_tlv[128];
	size_t o = 0;

	memset(&q, 0, sizeof(q));
	q.report_id = 1;
	q.action = SNS_SMGR_ACTION_ADD;
	q.report_rate_q16 = 50u << 16;
	q.n_items = 1;
	q.item[0].sensor_id = 0;
	q.item[0].data_type = 0;
	q.item[0].decimation = 3;
	q.item[0].calibration = 0;
	q.item[0].sampling_rate_hz = 50;
	q.item[0].sample_quality = 1;
	q.notify_valid = 1;
	expect_encoded("buffering add", QMI_REQUEST, SNS_SMGR_BUFFERING, 6, &q, sns_smgr_buf_req_ei,
		       q_tlv, sizeof(q_tlv));

	memset(&q, 0, sizeof(q));
	q.report_id = 3;
	q.action = SNS_SMGR_ACTION_DELETE;
	expect_encoded("buffering delete", QMI_REQUEST, SNS_SMGR_BUFFERING, 7, &q,
		       sns_smgr_buf_req_ei, del_tlv, sizeof(del_tlv));

	memset(&r, 0, sizeof(r));
	CHECK(decode(QMI_RESPONSE, SNS_SMGR_BUFFERING, r_tlv, sizeof(r_tlv), &r,
		     sns_smgr_buf_resp_ei) >= 0);
	CHECK(r.report_id_valid && r.report_id == 1 && r.ack_nak_valid && r.ack_nak == 1 &&
	      r.reasons_valid && r.n_reasons == 1 && r.reasons[0].reason == 5);

	/* 0x22: 01 report id; 02 u8 count + 12-byte index; 03 u8 count +
	 * 16-byte samples {i32 x3, u16 offset, u8 flags, u8 quality}; 0x10 */
	memcpy(ind_tlv + o, "\x01\x01\x00\x01", 4);
	o += 4;
	memcpy(ind_tlv + o, "\x02\x0d\x00\x01" "\x00\x00\x00\x02" "\x10\x00\x00\x00" "\x00\x00\x32\x00", 16);
	o += 16;
	memcpy(ind_tlv + o, "\x03\x21\x00\x02", 4);
	o += 4;
	memcpy(ind_tlv + o, "\x00\x80\x00\x00" "\x00\x00\x01\x00" "\xff\xff\xff\xff" "\x00\x00\x00\x00", 16);
	o += 16;
	memcpy(ind_tlv + o, "\x00\x00\x00\x00" "\x00\x00\x00\x00" "\x00\x00\x00\x00" "\x8f\x02\x01\x02", 16);
	o += 16;
	memcpy(ind_tlv + o, "\x10\x01\x00\x03", 4);
	o += 4;
	memset(&ind, 0, sizeof(ind));
	CHECK(decode(QMI_INDICATION, SNS_SMGR_BUFFERING_IND, ind_tlv, o, &ind, sns_smgr_buf_ind_ei) >= 0);
	CHECK(ind.report_id == 1 && ind.n_index == 1 && ind.n_samples == 2);
	CHECK(ind.index[0].count == 2 && ind.index[0].first_ts == 16 &&
	      ind.index[0].rate_q16 == 0x320000);
	CHECK(ind.samples[0].data[0] == 0x8000 && ind.samples[0].data[1] == 0x10000 &&
	      ind.samples[0].data[2] == -1);
	CHECK(ind.samples[1].ts_offset == 655 && ind.samples[1].flags == 1 &&
	      ind.samples[1].quality == 2);
	CHECK(ind.ind_type_valid && ind.ind_type == 3);
}

static void test_smgr_report(void)
{
	struct sns_smgr_rep_req q;
	struct sns_smgr_rep_ind ind;
	/* 01 id, 02 action, 03 u16 rate, 04 u8 buffer factor, 05 u8 count +
	 * items {u8 x4, u16, u8 x5, i32 x2}, 19 bytes each on the wire */
	static const uint8_t q_tlv[] = {
		0x01, 0x01, 0x00, 0x02,
		0x02, 0x01, 0x00, 0x01,
		0x03, 0x02, 0x00, 0x0a, 0x00,
		0x04, 0x01, 0x00, 0x00,
		0x05, 0x14, 0x00, 0x01, 0x0a, 0x00, 0x00, 0x03, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0 };
	uint8_t tlv[64];
	size_t o = 0;

	memset(&q, 0, sizeof(q));
	q.report_id = 2;
	q.action = SNS_SMGR_ACTION_ADD;
	q.report_rate_hz = 10;
	q.n_items = 1;
	q.item[0].sensor_id = 10;
	q.item[0].decimation = 3;
	expect_encoded("report add", QMI_REQUEST, SNS_SMGR_REPORT, 1, &q, sns_smgr_rep_req_ei, q_tlv,
		       sizeof(q_tlv));

	memcpy(tlv + o, "\x01\x01\x00\x02" "\x02\x01\x00\x00" "\x03\x02\x00\x0a\x00", 13);
	o += 13;
	memcpy(tlv + o, "\x04\x16\x00\x01" "\x0a\x00" "\x01\x00\x00\x00" "\x02\x00\x00\x00"
	       "\xfd\xff\xff\xff" "\x40\x00\x00\x00" "\x00\x01\x00", 25);
	o += 25;
	memset(&ind, 0, sizeof(ind));
	CHECK(decode(QMI_INDICATION, SNS_SMGR_REPORT_IND, tlv, o, &ind, sns_smgr_rep_ind_ei) >= 0);
	CHECK(ind.report_id == 2 && ind.current_rate == 10 && ind.n_items == 1);
	CHECK(ind.item[0].sensor_id == 10 && ind.item[0].data[2] == -3 && ind.item[0].ts == 64 &&
	      ind.item[0].quality == 1);
}

/* The BUFFERING item's calibration byte (4th item byte): -c factory = 1,
 * raw = 2, as the stock HAL's uncalibrated sensors send 1. */
static void test_smgr_calibration(void)
{
	struct sns_smgr_buf_req q;
	static const uint8_t f_tlv[] = {
		0x01, 0x01, 0x00, 0x03,
		0x02, 0x01, 0x00, 0x01,
		0x03, 0x04, 0x00, 0x00, 0x00, 0x14, 0x00,
		0x04, 0x09, 0x00, 0x01, 0x14, 0x00, 0x03, 0x01, 0x14, 0x00, 0x01, 0x00 };
	uint8_t r_tlv[sizeof(f_tlv)];

	memset(&q, 0, sizeof(q));
	q.report_id = 3;
	q.action = SNS_SMGR_ACTION_ADD;
	q.report_rate_q16 = 20u << 16;
	q.n_items = 1;
	q.item[0].sensor_id = SNS_SMGR_ID_MAG;
	q.item[0].decimation = 3;
	q.item[0].calibration = SNS_SMGR_CAL_FACTORY;
	q.item[0].sampling_rate_hz = 20;
	q.item[0].sample_quality = 1;
	expect_encoded("buffering magn factory", QMI_REQUEST, SNS_SMGR_BUFFERING, 9, &q,
		       sns_smgr_buf_req_ei, f_tlv, sizeof(f_tlv));
	memcpy(r_tlv, f_tlv, sizeof(r_tlv));
	r_tlv[22] = 0x02;
	q.item[0].calibration = SNS_SMGR_CAL_RAW;
	expect_encoded("buffering magn raw", QMI_REQUEST, SNS_SMGR_BUFFERING, 9, &q,
		       sns_smgr_buf_req_ei, r_tlv, sizeof(r_tlv));
}

/* QMAG_CAL (0x140), against logs/sns-idl-dump-2026-09-27-qmag.txt. */
static void test_qmag(void)
{
	struct sns_sam_enable_req en;
	struct sns_sam_disable_req dis = { .instance = 7 };
	struct sns_sam_instance_resp ir;
	struct sns_sam_qmag_ind ind;
	struct sns_sam_error_ind err;
	struct sns_sam_attr_resp attr;
	/* enable: optional TLV 0x10 u32 only */
	static const uint8_t en_tlv[] = { 0x10, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00 };
	static const uint8_t dis_tlv[] = { 0x01, 0x01, 0x00, 0x07 };
	static const uint8_t ir_tlv[] = { 0x02, 0x02, 0x00, 0x00, 0x00, 0x10, 0x01, 0x00, 0x07 };
	static const uint8_t ir_fail[] = { 0x02, 0x02, 0x00, 0x01, 0x05 };
	/* report: 01 u8 instance, 02 u32 ts, 03 u32[3] bias (no count), 04 u32 */
	static const uint8_t ind_tlv[] = {
		0x01, 0x01, 0x00, 0x07,
		0x02, 0x04, 0x00, 0x78, 0x56, 0x34, 0x12,
		0x03, 0x0c, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0xff, 0xff, 0x9a, 0x19, 0x00, 0x00,
		0x04, 0x04, 0x00, 0x03, 0x00, 0x00, 0x00 };
	static const uint8_t err_tlv[] = { 0x01, 0x01, 0x00, 0x02, 0x02, 0x01, 0x00, 0x07 };
	uint8_t attr_tlv[5 + 9 * 7 + 11 + 7];
	size_t o = 0;
	int i;

	/* default: no TLV at all (the IDL's max_len for it is 0) */
	memset(&en, 0, sizeof(en));
	expect_encoded("qmag enable, no period", QMI_REQUEST, SNS_SAM_ENABLE, 1, &en,
		       sns_sam_enable_req_ei, NULL, 0);
	/* What TLV 0x10 would look like; sensord never sends it. */
	en.period_valid = 1;
	en.period_q16 = 65536;
	expect_encoded("qmag enable, period", QMI_REQUEST, SNS_SAM_ENABLE, 2, &en,
		       sns_sam_enable_req_ei, en_tlv, sizeof(en_tlv));
	expect_encoded("qmag disable", QMI_REQUEST, SNS_SAM_DISABLE, 3, &dis, sns_sam_disable_req_ei,
		       dis_tlv, sizeof(dis_tlv));
	expect_encoded("qmag attributes", QMI_REQUEST, SNS_SAM_GET_ATTR, 4, NULL, sns_empty_ei, NULL, 0);

	memset(&ir, 0, sizeof(ir));
	CHECK(decode(QMI_RESPONSE, SNS_SAM_ENABLE, ir_tlv, sizeof(ir_tlv), &ir,
		     sns_sam_instance_resp_ei) >= 0);
	CHECK(ir.resp.result == 0 && ir.instance_valid && ir.instance == 7);
	memset(&ir, 0, sizeof(ir));
	CHECK(decode(QMI_RESPONSE, SNS_SAM_ENABLE, ir_fail, sizeof(ir_fail), &ir,
		     sns_sam_instance_resp_ei) >= 0);
	CHECK(ir.resp.result == 1 && ir.resp.err == 5 && !ir.instance_valid);

	memset(&ind, 0, sizeof(ind));
	CHECK(decode(QMI_INDICATION, SNS_SAM_REPORT_IND, ind_tlv, sizeof(ind_tlv), &ind,
		     sns_sam_qmag_ind_ei) >= 0);
	CHECK(ind.instance == 7 && ind.timestamp == 0x12345678 && ind.accuracy == 3);
	CHECK(ind.bias[0] == 0x8000 && ind.bias[1] == -0x8000 && ind.bias[2] == 0x199a);

	memset(&err, 0, sizeof(err));
	CHECK(decode(QMI_INDICATION, SNS_SAM_ERROR_IND, err_tlv, sizeof(err_tlv), &err,
		     sns_sam_error_ind_ei) >= 0);
	CHECK(err.error == 2 && err.instance == 7);

	memcpy(attr_tlv + o, "\x02\x02\x00\x00\x00", 5);
	o += 5;
	for (i = 0; i < 9; i++) {
		uint8_t t[7] = { (uint8_t)(3 + i), 4, 0, (uint8_t)(10 + i), 0, (uint8_t)i, 0 };

		memcpy(attr_tlv + o, t, 7);
		o += 7;
	}
	memcpy(attr_tlv + o, "\x10\x08\x00\x01\x02\x03\x04\x05\x06\x07\x08", 11);
	o += 11;
	memcpy(attr_tlv + o, "\x11\x04\x00\x2a\x00\x00\x00", 7);
	o += 7;
	memset(&attr, 0, sizeof(attr));
	CHECK(decode(QMI_RESPONSE, SNS_SAM_GET_ATTR, attr_tlv, o, &attr, sns_sam_attr_resp_ei) >= 0);
	CHECK(attr.attr[0] == 10 && attr.attr[3] == 0x3000d && attr.attr[8] == 0x80012);
	CHECK(attr.suid_valid && attr.suid == 0x0807060504030201ULL);
	CHECK(attr.reserved_valid && attr.reserved == 42);

	/* truncated: bias TLV one word short */
	{
		uint8_t cut[sizeof(ind_tlv) - 4 - 7];

		memcpy(cut, ind_tlv, 11);
		memcpy(cut + 11, "\x03\x08\x00\x00\x80\x00\x00\x00\x80\xff\xff", 11);
		memset(&ind, 0, sizeof(ind));
		CHECK(decode(QMI_INDICATION, SNS_SAM_REPORT_IND, cut, sizeof(cut), &ind,
			     sns_sam_qmag_ind_ei) < 0);
	}
}

/* ROTATION_VECTOR (0x112), against logs/sns-idl-dump-2026-09-27-rotvec.txt. */
static void test_rotvec(void)
{
	struct sns_rotvec_enable_req en;
	struct sns_sam_rotvec_ind ind;
	float f[4];
	/* what sensord sends: 0x01 period 0, 0x10 rate 20 Hz Q16, 0x12 {0, 0} */
	static const uint8_t en_tlv[] = {
		0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x10, 0x04, 0x00, 0x00, 0x00, 0x14, 0x00,
		0x12, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	/* all four TLVs: 26 bytes, the IDL's max_len */
	static const uint8_t en_all[] = {
		0x01, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00,
		0x10, 0x04, 0x00, 0x00, 0x00, 0x0a, 0x00,
		0x11, 0x01, 0x00, 0x01,
		0x12, 0x05, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01 };
	/* report: 01 u8 instance, 02 u32 ts, 03 {f32 x4 (0.5, -0.5, 0.25, 0.625), u8, u8} */
	static const uint8_t ind_tlv[] = {
		0x01, 0x01, 0x00, 0x09,
		0x02, 0x04, 0x00, 0x44, 0x33, 0x22, 0x11,
		0x03, 0x12, 0x00,
		0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x00, 0xbf, 0x00, 0x00, 0x80, 0x3e,
		0x00, 0x00, 0x20, 0x3f, 0x03, 0x01 };
	int i;

	memset(&en, 0, sizeof(en));
	en.rate_valid = 1;
	en.rate_q16 = 20u << 16;
	en.notify_valid = 1;
	expect_encoded("rotvec enable", QMI_REQUEST, SNS_SAM_ENABLE, 5, &en, sns_rotvec_enable_req_ei,
		       en_tlv, sizeof(en_tlv));
	en.period_q16 = 65536;
	en.rate_q16 = 10u << 16;
	en.coord_valid = 1;
	en.coord = 1;
	en.notify.proc_type = 2;
	en.notify.send_during_suspend = 1;
	expect_encoded("rotvec enable, all TLVs", QMI_REQUEST, SNS_SAM_ENABLE, 6, &en,
		       sns_rotvec_enable_req_ei, en_all, sizeof(en_all));
	CHECK(sizeof(en_all) == 26);

	memset(&ind, 0, sizeof(ind));
	CHECK(decode(QMI_INDICATION, SNS_SAM_REPORT_IND, ind_tlv, sizeof(ind_tlv), &ind,
		     sns_sam_rotvec_ind_ei) >= 0);
	CHECK(ind.instance == 9 && ind.timestamp == 0x11223344);
	for (i = 0; i < 4; i++)
		memcpy(&f[i], &ind.r.q[i], 4);
	CHECK(f[0] == 0.5f && f[1] == -0.5f && f[2] == 0.25f && f[3] == 0.625f);
	CHECK(ind.r.accuracy == 3 && ind.r.coord == 1);
	/* The result struct one byte short, TLV length consistent (17): the
	 * codec stops at the TLV end and accepts it with coord unset, so
	 * sensord checks TLV 0x03 is exactly 18 bytes itself (and ASan here
	 * shows nothing is read past the input). */
	{
		uint8_t cut[sizeof(ind_tlv) - 1];

		memcpy(cut, ind_tlv, sizeof(cut));
		cut[12] = 0x11;
		memset(&ind, 0, sizeof(ind));
		ind.r.coord = 0x55;
		CHECK(decode(QMI_INDICATION, SNS_SAM_REPORT_IND, cut, sizeof(cut), &ind,
			     sns_sam_rotvec_ind_ei) >= 0);
		CHECK(ind.r.accuracy == 3 && ind.r.coord == 0x55);
		/* TLV length beyond the input: refused */
		cut[12] = 0x12;
		CHECK(decode(QMI_INDICATION, SNS_SAM_REPORT_IND, cut, sizeof(cut), &ind,
			     sns_sam_rotvec_ind_ei) < 0);
	}
}

/* Malformed input from the DSP must be rejected without reading past it
 * (tools/qrtr/qmi.c bounds checks; run under ASan by make test). */
static void test_truncated(void)
{
	static struct sns_reg2_group_write_req gw;
	static struct sns_smgr_buf_ind ind;
	struct sns_reg2_item_write_req iw;
	struct sns_reg2_id_req id;
	/* item read with a trailing 1-byte TLV fragment */
	static const uint8_t frag[] = { 0x01, 0x02, 0x00, 0x87, 0x0a, 0x10 };
	/* TLV length beyond the input */
	static const uint8_t longtlv[] = { 0x01, 0x05, 0x00, 0x87, 0x0a };
	/* group write, TLV 0x02 length 2 but count 256 */
	static const uint8_t gwc[] = { 0x01, 0x02, 0x00, 0xe8, 0x03, 0x02, 0x02, 0x00, 0x00, 0x01 };
	/* group write, count prefix cut short (1 of 2 bytes) */
	static const uint8_t gwp[] = { 0x01, 0x02, 0x00, 0xe8, 0x03, 0x02, 0x01, 0x00, 0x05 };
	/* item write, count 200 for an 8-byte array */
	static const uint8_t iwc[] = { 0x01, 0x02, 0x00, 0xbc, 0x02,
				       0x02, 0x09, 0x00, 0xc8, 1, 2, 3, 4, 5, 6, 7, 8 };
	/* SMGR 0x22: TLV 0x03 claims 1601 bytes / 100 samples, carries none */
	uint8_t ind1[4 + 16 + 4];
	/* SMGR 0x22: TLV length fits, 100 samples claimed, one present
	 * (upstream accepted a short array at the top level) */
	uint8_t ind2[4 + 16 + 4 + 16];

	CHECK(decode(QMI_REQUEST, SNS_REG2_ITEM_READ, frag, sizeof(frag), &id, sns_reg2_id_req_ei) < 0);
	CHECK(decode(QMI_REQUEST, SNS_REG2_ITEM_READ, longtlv, sizeof(longtlv), &id,
		     sns_reg2_id_req_ei) < 0);
	CHECK(decode(QMI_REQUEST, SNS_REG2_GROUP_WRITE, gwc, sizeof(gwc), &gw,
		     sns_reg2_group_write_req_ei) < 0);
	CHECK(decode(QMI_REQUEST, SNS_REG2_GROUP_WRITE, gwp, sizeof(gwp), &gw,
		     sns_reg2_group_write_req_ei) < 0);
	CHECK(decode(QMI_REQUEST, SNS_REG2_ITEM_WRITE, iwc, sizeof(iwc), &iw,
		     sns_reg2_item_write_req_ei) < 0);

	memcpy(ind1, "\x01\x01\x00\x01" "\x02\x0d\x00\x01" "\x00\x00\x00\x02"
	       "\x10\x00\x00\x00" "\x00\x00\x32\x00" "\x03\x41\x06\x64", sizeof(ind1));
	CHECK(decode(QMI_INDICATION, SNS_SMGR_BUFFERING_IND, ind1, sizeof(ind1), &ind,
		     sns_smgr_buf_ind_ei) < 0);
	memcpy(ind2, ind1, 20);
	memcpy(ind2 + 20, "\x03\x11\x00\x64", 4);
	memset(ind2 + 24, 0, 16);
	CHECK(decode(QMI_INDICATION, SNS_SMGR_BUFFERING_IND, ind2, sizeof(ind2), &ind,
		     sns_smgr_buf_ind_ei) < 0);
}

int main(void)
{
	test_common();
	test_reg2();
	test_time2();
	test_smgr_info();
	test_smgr_buffering();
	test_smgr_report();
	test_smgr_calibration();
	test_qmag();
	test_rotvec();
	test_truncated();
	if (failures) {
		fprintf(stderr, "test-sns-msgs: %d failure(s)\n", failures);
		return 1;
	}
	printf("test-sns-msgs: all passed\n");
	return 0;
}
