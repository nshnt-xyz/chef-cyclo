/* Host unit tests for tools/qmux.c: QMUX framing, the QMI SDU header, TLVs,
 * CTL (service 0) emulation and the CID table. No sockets, no kernel
 * dependency -- everything under test is a pure function of its inputs.
 * Build/run: see tools/Makefile ("make test").
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../qmux.h"

static int g_failures;
static int g_tests;

#define CHECK(cond) do { \
	g_tests++; \
	if (!(cond)) { \
		g_failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

/* ---- QMUX framing ---- */

static void test_frame_scan_need_more(void)
{
	uint8_t buf[8] = { 0x01, 0x09, 0x00, 0x00, 0x00, 0x00 };

	CHECK(qmux_frame_scan(buf, 0) == 0);
	CHECK(qmux_frame_scan(buf, 1) == 0);
	CHECK(qmux_frame_scan(buf, 2) == 0);
	/* length=9 means total frame = 10 bytes; only 6 supplied so far */
	CHECK(qmux_frame_scan(buf, 6) == 0);
}

static void test_frame_scan_malformed(void)
{
	uint8_t bad_marker[4] = { 0x02, 0x05, 0x00, 0x00 };
	uint8_t short_length[4] = { 0x01, 0x04, 0x00, 0x00 }; /* length 4 < 5 */

	CHECK(qmux_frame_scan(bad_marker, sizeof(bad_marker)) == -1);
	CHECK(qmux_frame_scan(short_length, sizeof(short_length)) == -1);
}

static void test_frame_build_scan_parse_roundtrip(void)
{
	uint8_t sdu[3] = { 0xaa, 0xbb, 0xcc };
	uint8_t frame[32];
	ssize_t built;
	ssize_t scanned;
	struct qmux_frame f;

	built = qmux_frame_build(frame, sizeof(frame), QMUX_FLAG_C2S, 16, 3, sdu, sizeof(sdu));
	CHECK(built == 6 + 3);

	/* One byte short: must report "need more", not treat it as a
	 * different, shorter frame. */
	CHECK(qmux_frame_scan(frame, (size_t)built - 1) == 0);

	scanned = qmux_frame_scan(frame, (size_t)built);
	CHECK(scanned == built);

	/* A second frame's first byte tacked on must not change what the
	 * first frame scans as. */
	frame[built] = 0x01;
	CHECK(qmux_frame_scan(frame, (size_t)built + 1) == built);

	CHECK(qmux_frame_parse(frame, (size_t)scanned, &f) == 0);
	CHECK(f.flags == QMUX_FLAG_C2S);
	CHECK(f.service == 16);
	CHECK(f.client == 3);
	CHECK(f.sdu_len == sizeof(sdu));
	CHECK(memcmp(f.sdu, sdu, sizeof(sdu)) == 0);
}

static void test_frame_parse_too_short(void)
{
	uint8_t frame[4] = { 0x01, 0x00, 0x00, 0x00 };
	struct qmux_frame f;

	CHECK(qmux_frame_parse(frame, sizeof(frame), &f) == -1);
}

static void test_frame_build_overflow(void)
{
	uint8_t sdu[4] = { 0 };
	uint8_t tiny[4];

	/* out_cap too small for header+sdu */
	CHECK(qmux_frame_build(tiny, sizeof(tiny), 0, 0, 0, sdu, sizeof(sdu)) == -1);
}

/* ---- QMI SDU header ---- */

static void test_sdu_header_roundtrip_ctl(void)
{
	uint8_t tlvs[2] = { 0x11, 0x22 };
	uint8_t sdu[32];
	ssize_t n;
	struct qmi_sdu_header hdr;

	n = qmi_sdu_build(sdu, sizeof(sdu), QMI_SERVICE_CTL, QMI_REQUEST, 0x05, 0x0022, tlvs, sizeof(tlvs));
	CHECK(n == 6 + 2);

	CHECK(qmi_sdu_parse_header(sdu, (size_t)n, QMI_SERVICE_CTL, &hdr) == 6);
	CHECK(hdr.ctl_flag == QMI_REQUEST);
	CHECK(hdr.txn == 0x05);
	CHECK(hdr.msg_id == 0x0022);
	CHECK(hdr.tlv_len == 2);
}

static void test_sdu_header_roundtrip_service(void)
{
	uint8_t tlvs[3] = { 1, 2, 3 };
	uint8_t sdu[32];
	ssize_t n;
	struct qmi_sdu_header hdr;

	n = qmi_sdu_build(sdu, sizeof(sdu), 16 /* LOC */, QMI_INDICATION, 0x1234, 0x0024, tlvs, sizeof(tlvs));
	CHECK(n == 7 + 3);

	CHECK(qmi_sdu_parse_header(sdu, (size_t)n, 16, &hdr) == 7);
	CHECK(hdr.ctl_flag == QMI_INDICATION);
	CHECK(hdr.txn == 0x1234);
	CHECK(hdr.msg_id == 0x0024);
	CHECK(hdr.tlv_len == 3);
}

static void test_sdu_header_malformed(void)
{
	uint8_t too_short[3] = { 0, 0, 0 };
	uint8_t bad_len[7] = { 0, 0, 0, 0, 0, 0xff, 0xff }; /* CTL header claims 0xffff TLV bytes but has none */
	struct qmi_sdu_header hdr;

	CHECK(qmi_sdu_parse_header(too_short, sizeof(too_short), QMI_SERVICE_CTL, &hdr) == -1);
	CHECK(qmi_sdu_parse_header(bad_len, 6, QMI_SERVICE_CTL, &hdr) == -1);
}

/* ---- TLVs ---- */

static void test_tlv_put_find_roundtrip(void)
{
	uint8_t buf[32];
	size_t off = 0;
	ssize_t n;
	const uint8_t *val;
	size_t val_len;

	n = tlv_put_u8(buf + off, sizeof(buf) - off, 0x01, 0x7f);
	CHECK(n == 4);
	off += (size_t)n;

	n = tlv_put_u16(buf + off, sizeof(buf) - off, 0x10, 0xbeef);
	CHECK(n == 5);
	off += (size_t)n;

	CHECK(tlv_find(buf, off, 0x01, &val, &val_len) == 0);
	CHECK(val_len == 1 && val[0] == 0x7f);

	CHECK(tlv_find(buf, off, 0x10, &val, &val_len) == 0);
	CHECK(val_len == 2 && (val[0] | (val[1] << 8)) == 0xbeef);

	CHECK(tlv_find(buf, off, 0x99, &val, &val_len) == -1); /* well-formed, absent */
}

static void test_tlv_find_malformed(void)
{
	uint8_t truncated_header[2] = { 0x01, 0x05 }; /* missing length hi byte */
	uint8_t value_runs_past_end[3] = { 0x01, 0xff, 0x00 }; /* claims 255 bytes, has 0 */
	const uint8_t *val;
	size_t val_len;

	CHECK(tlv_find(truncated_header, sizeof(truncated_header), 0x01, &val, &val_len) == -2);
	CHECK(tlv_find(value_runs_past_end, sizeof(value_runs_past_end), 0x01, &val, &val_len) == -2);
}

static void test_tlv_put_result(void)
{
	uint8_t buf[8];
	ssize_t n = tlv_put_result(buf, sizeof(buf), 1, 0x0011);
	const uint8_t *val;
	size_t val_len;

	CHECK(n == 7);
	CHECK(tlv_find(buf, (size_t)n, 0x02, &val, &val_len) == 0);
	CHECK(val_len == 4);
	CHECK((val[0] | (val[1] << 8)) == 1);
	CHECK((val[2] | (val[3] << 8)) == 0x0011);
}

/* ---- CTL emulation: end-to-end through QMUX framing ---- */

/* Simulates what a real qmicli would do: build a QMUX-framed CTL request,
 * feed the raw bytes through the same scan/parse a real connection buffer
 * would use, and check the (equally QMUX-framed) response the daemon-side
 * builders would send back decodes the way libqmi expects. */
static void test_ctl_allocate_cid_success_wire_roundtrip(void)
{
	uint8_t req_tlvs[8];
	ssize_t req_tlv_len;
	uint8_t req_sdu[16];
	uint8_t req_frame[32];
	ssize_t n;
	struct qmux_frame f;
	struct qmi_sdu_header hdr;
	uint8_t service;

	req_tlv_len = tlv_put_u8(req_tlvs, sizeof(req_tlvs), 0x01, 16); /* Service = LOC */
	CHECK(req_tlv_len > 0);

	n = qmi_sdu_build(req_sdu, sizeof(req_sdu), QMI_SERVICE_CTL, QMI_REQUEST, 1,
			  QMI_CTL_MSG_ALLOCATE_CID, req_tlvs, (size_t)req_tlv_len);
	CHECK(n > 0);
	n = qmux_frame_build(req_frame, sizeof(req_frame), QMUX_FLAG_C2S, QMI_SERVICE_CTL, 0, req_sdu, (size_t)n);
	CHECK(n > 0);

	CHECK(qmux_frame_scan(req_frame, (size_t)n) == n);
	CHECK(qmux_frame_parse(req_frame, (size_t)n, &f) == 0);
	CHECK(f.service == QMI_SERVICE_CTL);

	CHECK(qmi_sdu_parse_header(f.sdu, f.sdu_len, QMI_SERVICE_CTL, &hdr) == 6);
	CHECK(hdr.msg_id == QMI_CTL_MSG_ALLOCATE_CID);
	CHECK(hdr.txn == 1);
	CHECK(ctl_parse_allocate_cid_req(f.sdu + 6, hdr.tlv_len, &service) == 0);
	CHECK(service == 16);

	/* Now build the response the way qmuxd-lite would, and check a
	 * client-side decode of it. */
	{
		uint8_t resp_sdu[32];
		uint8_t resp_frame[64];
		struct qmux_frame rf;
		struct qmi_sdu_header rhdr;
		const uint8_t *val;
		size_t val_len;
		ssize_t rn;

		rn = ctl_build_allocate_cid_resp(16, 3, hdr.txn, resp_sdu, sizeof(resp_sdu));
		CHECK(rn > 0);
		rn = qmux_frame_build(resp_frame, sizeof(resp_frame), QMUX_FLAG_S2C, QMI_SERVICE_CTL, 0, resp_sdu, (size_t)rn);
		CHECK(rn > 0);

		CHECK(qmux_frame_scan(resp_frame, (size_t)rn) == rn);
		CHECK(qmux_frame_parse(resp_frame, (size_t)rn, &rf) == 0);
		CHECK(rf.flags == QMUX_FLAG_S2C);

		CHECK(qmi_sdu_parse_header(rf.sdu, rf.sdu_len, QMI_SERVICE_CTL, &rhdr) == 6);
		CHECK(rhdr.ctl_flag == QMI_CTL_FLAG_RESPONSE); /* 1, not the service-style 2 */
		CHECK(rhdr.txn == 1);
		CHECK(rhdr.msg_id == QMI_CTL_MSG_ALLOCATE_CID);

		CHECK(tlv_find(rf.sdu + 6, rhdr.tlv_len, 0x02, &val, &val_len) == 0);
		CHECK(val_len == 4 && (val[0] | (val[1] << 8)) == QMI_CTL_RESULT_SUCCESS);

		CHECK(tlv_find(rf.sdu + 6, rhdr.tlv_len, 0x01, &val, &val_len) == 0);
		CHECK(val_len == 2 && val[0] == 16 && val[1] == 3);
	}
}

static void test_ctl_allocate_cid_fail_result(void)
{
	uint8_t sdu[32];
	ssize_t n = ctl_build_allocate_cid_fail(QMI_CTL_ERR_NOT_SUPPORTED, 7, sdu, sizeof(sdu));
	struct qmi_sdu_header hdr;
	const uint8_t *val;
	size_t val_len;

	CHECK(n > 0);
	CHECK(qmi_sdu_parse_header(sdu, (size_t)n, QMI_SERVICE_CTL, &hdr) == 6);
	CHECK(hdr.txn == 7);
	CHECK(hdr.msg_id == QMI_CTL_MSG_ALLOCATE_CID);

	CHECK(tlv_find(sdu + 6, hdr.tlv_len, 0x02, &val, &val_len) == 0);
	CHECK((val[0] | (val[1] << 8)) == QMI_CTL_RESULT_FAILURE);
	CHECK((val[2] | (val[3] << 8)) == QMI_CTL_ERR_NOT_SUPPORTED);

	/* No Allocation Info TLV on failure. */
	CHECK(tlv_find(sdu + 6, hdr.tlv_len, 0x01, &val, &val_len) == -1);
}

static void test_ctl_get_version_info(void)
{
	struct qmi_svc_version list[2] = {
		{ .service = 16, .major = 1, .minor = 2 },
		{ .service = 14, .major = 1, .minor = 0 },
	};
	uint8_t sdu[64];
	ssize_t n;
	struct qmi_sdu_header hdr;
	const uint8_t *val;
	size_t val_len;

	/* Empty list is still a valid response (count 0). */
	n = ctl_build_get_version_info_resp(NULL, 0, 9, sdu, sizeof(sdu));
	CHECK(n > 0);
	CHECK(qmi_sdu_parse_header(sdu, (size_t)n, QMI_SERVICE_CTL, &hdr) == 6);
	CHECK(tlv_find(sdu + 6, hdr.tlv_len, 0x01, &val, &val_len) == 0);
	CHECK(val_len == 1 && val[0] == 0);

	n = ctl_build_get_version_info_resp(list, 2, 9, sdu, sizeof(sdu));
	CHECK(n > 0);
	CHECK(qmi_sdu_parse_header(sdu, (size_t)n, QMI_SERVICE_CTL, &hdr) == 6);
	CHECK(tlv_find(sdu + 6, hdr.tlv_len, 0x01, &val, &val_len) == 0);
	CHECK(val_len == 1 + 2 * 5);
	CHECK(val[0] == 2);
	CHECK(val[1] == 16 && (val[2] | (val[3] << 8)) == 1 && (val[4] | (val[5] << 8)) == 2);
	CHECK(val[6] == 14 && (val[7] | (val[8] << 8)) == 1 && (val[9] | (val[10] << 8)) == 0);
}

static void test_ctl_unsupported(void)
{
	uint8_t sdu[32];
	ssize_t n = ctl_build_unsupported_resp(0x9999, 2, sdu, sizeof(sdu));
	struct qmi_sdu_header hdr;
	const uint8_t *val;
	size_t val_len;

	CHECK(n > 0);
	CHECK(qmi_sdu_parse_header(sdu, (size_t)n, QMI_SERVICE_CTL, &hdr) == 6);
	CHECK(hdr.msg_id == 0x9999);
	CHECK(tlv_find(sdu + 6, hdr.tlv_len, 0x02, &val, &val_len) == 0);
	CHECK((val[0] | (val[1] << 8)) == QMI_CTL_RESULT_FAILURE);
	CHECK((val[2] | (val[3] << 8)) == QMI_CTL_ERR_NOT_SUPPORTED);
}

static void test_ctl_set_data_format_optional_tlv(void)
{
	uint16_t protocol = 0xffff;

	CHECK(ctl_parse_set_data_format_req(NULL, 0, &protocol) == 0);
	CHECK(protocol == 0); /* TLV absent -> default, not an error */
}

static void test_ctl_request_parsers_reject_malformed(void)
{
	uint8_t wrong_len[4] = { 0x01, 0x02, 0x00, 0xaa }; /* Allocate CID wants a 1-byte value */
	uint8_t service;

	CHECK(ctl_parse_allocate_cid_req(wrong_len, sizeof(wrong_len), &service) == -1);
}

/* ---- CID table ---- */

static void test_cid_alloc_lowest_first_and_release(void)
{
	struct qmuxd_cid_table t;
	uint8_t cid;

	CHECK(cid_table_init(&t, 8) == 0);

	CHECK(cid_alloc(&t, 16, 10, 100, 200, (void *)1, &cid) == 0);
	CHECK(cid == 1);
	CHECK(cid_alloc(&t, 16, 11, 100, 200, (void *)2, &cid) == 0);
	CHECK(cid == 2);

	CHECK(cid_release(&t, 16, 1) == 0);
	/* A double release must fail, not silently succeed. */
	CHECK(cid_release(&t, 16, 1) == -1);

	CHECK(cid_alloc(&t, 16, 12, 100, 200, (void *)3, &cid) == 0);
	CHECK(cid == 1); /* lowest free number reused */

	/* A different service gets its own independent CID numbering. */
	CHECK(cid_alloc(&t, 14, 13, 1, 2, (void *)4, &cid) == 0);
	CHECK(cid == 1);

	cid_table_free(&t);
}

static void test_cid_table_capacity_exhaustion(void)
{
	struct qmuxd_cid_table t;
	uint8_t cid;

	CHECK(cid_table_init(&t, 2) == 0);
	CHECK(cid_alloc(&t, 16, 1, 0, 0, NULL, &cid) == 0);
	CHECK(cid_alloc(&t, 16, 2, 0, 0, NULL, &cid) == 0);
	/* Table has no more slots, even though only 2 of 254 cids are used. */
	CHECK(cid_alloc(&t, 16, 3, 0, 0, NULL, &cid) == -1);

	cid_table_free(&t);
}

static void test_cid_per_service_exhaustion(void)
{
	struct qmuxd_cid_table t;
	uint8_t cid;
	int i;

	CHECK(cid_table_init(&t, 300) == 0);

	for (i = 0; i < QMUX_MAX_CID; i++)
		CHECK(cid_alloc(&t, 16, i, 0, 0, NULL, &cid) == 0);

	/* All 254 cids for service 16 are in use now. */
	CHECK(cid_alloc(&t, 16, 999, 0, 0, NULL, &cid) == -1);

	/* A different service is unaffected. */
	CHECK(cid_alloc(&t, 14, 999, 0, 0, NULL, &cid) == 0);
	CHECK(cid == 1);

	cid_table_free(&t);
}

static void test_cid_lookup_helpers(void)
{
	struct qmuxd_cid_table t;
	uint8_t cid;
	uint32_t node, port;
	int dummy_owner_a, dummy_owner_b;

	CHECK(cid_table_init(&t, 4) == 0);
	CHECK(cid_alloc(&t, 16, 42, 7, 8, &dummy_owner_a, &cid) == 0);

	CHECK(cid_get_sock(&t, 16, cid) == 42);
	CHECK(cid_get_sock(&t, 16, cid + 1) == -1); /* unallocated cid */

	CHECK(cid_get_target(&t, 16, cid, &node, &port) == 0);
	CHECK(node == 7 && port == 8);
	CHECK(cid_get_target(&t, 99, cid, &node, &port) == -1); /* wrong service */

	CHECK(cid_get_owner(&t, 16, cid) == &dummy_owner_a);
	CHECK(cid_set_owner(&t, 16, cid, &dummy_owner_b) == 0);
	CHECK(cid_get_owner(&t, 16, cid) == &dummy_owner_b);
	CHECK(cid_set_owner(&t, 16, cid + 1, &dummy_owner_b) == -1);

	{
		uint8_t found_service, found_cid;
		CHECK(cid_find_by_sock(&t, 42, &found_service, &found_cid) == 0);
		CHECK(found_service == 16 && found_cid == cid);
		CHECK(cid_find_by_sock(&t, 4242, &found_service, &found_cid) == -1);
	}

	/* Owner clearing: only slots pointing at the given owner change. */
	CHECK(cid_alloc(&t, 20, 43, 0, 0, &dummy_owner_b, &cid) == 0);
	cid_clear_owner_all(&t, &dummy_owner_b);
	CHECK(cid_get_owner(&t, 16, 1) == NULL);
	CHECK(cid_get_owner(&t, 20, cid) == NULL);

	cid_table_free(&t);
}

int main(void)
{
	test_frame_scan_need_more();
	test_frame_scan_malformed();
	test_frame_build_scan_parse_roundtrip();
	test_frame_parse_too_short();
	test_frame_build_overflow();

	test_sdu_header_roundtrip_ctl();
	test_sdu_header_roundtrip_service();
	test_sdu_header_malformed();

	test_tlv_put_find_roundtrip();
	test_tlv_find_malformed();
	test_tlv_put_result();

	test_ctl_allocate_cid_success_wire_roundtrip();
	test_ctl_allocate_cid_fail_result();
	test_ctl_get_version_info();
	test_ctl_unsupported();
	test_ctl_set_data_format_optional_tlv();
	test_ctl_request_parsers_reject_malformed();

	test_cid_alloc_lowest_first_and_release();
	test_cid_table_capacity_exhaustion();
	test_cid_per_service_exhaustion();
	test_cid_lookup_helpers();

	fprintf(stderr, "%s: %d/%d checks passed\n", g_failures ? "FAIL" : "PASS", g_tests - g_failures, g_tests);
	return g_failures ? 1 : 0;
}
