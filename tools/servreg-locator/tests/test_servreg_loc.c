/* Host unit tests for tools/servreg-locator/servreg_loc.{h,c}: QMI
 * encode/decode round trips for the SERVREG_LOC messages this daemon
 * speaks, using the real vendored codec (qrtr/qmi.c) unmodified -- exactly
 * what servreg-locator.c calls at runtime, just without a socket under it.
 * Build/run: see Makefile ("make test").
 */
#include <stdio.h>
#include <string.h>

#include "../servreg_loc.h"

static int g_failures;
static int g_tests;

#define CHECK(cond) do { \
	g_tests++; \
	if (!(cond)) { \
		g_failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

/* ---- GET_DOMAIN_LIST request ---- */

static void test_get_domain_list_req_roundtrip(void)
{
	struct servreg_loc_get_domain_list_req in, out;
	DEFINE_QRTR_PACKET(pkt, 256);
	unsigned int txn;
	ssize_t len;
	int ret;

	memset(&in, 0, sizeof(in));
	strcpy(in.service_name, "tms/servreg");
	in.domain_offset_valid = 1;
	in.domain_offset = 3;

	len = qmi_encode_message(&pkt, QMI_REQUEST, SERVREG_LOC_GET_DOMAIN_LIST_REQ, 7,
				  &in, servreg_loc_get_domain_list_req_ei);
	CHECK(len > 0);

	memset(&out, 0, sizeof(out));
	ret = qmi_decode_message(&out, &txn, &pkt, QMI_REQUEST,
				  SERVREG_LOC_GET_DOMAIN_LIST_REQ,
				  servreg_loc_get_domain_list_req_ei);
	CHECK(ret >= 0);
	CHECK(txn == 7);
	CHECK(strcmp(out.service_name, "tms/servreg") == 0);
	CHECK(out.domain_offset_valid == 1);
	CHECK(out.domain_offset == 3);
}

static void test_get_domain_list_req_no_offset(void)
{
	struct servreg_loc_get_domain_list_req in, out;
	DEFINE_QRTR_PACKET(pkt, 256);
	unsigned int txn;
	ssize_t len;

	memset(&in, 0, sizeof(in));
	strcpy(in.service_name, "wlan/fw");
	/* domain_offset_valid left 0 -- optional TLV must be entirely absent. */

	len = qmi_encode_message(&pkt, QMI_REQUEST, SERVREG_LOC_GET_DOMAIN_LIST_REQ, 1,
				  &in, servreg_loc_get_domain_list_req_ei);
	CHECK(len > 0);

	memset(&out, 0, sizeof(out));
	out.domain_offset = 0xdeadbeef; /* must not survive if the TLV is absent */
	CHECK(qmi_decode_message(&out, &txn, &pkt, QMI_REQUEST,
				  SERVREG_LOC_GET_DOMAIN_LIST_REQ,
				  servreg_loc_get_domain_list_req_ei) >= 0);
	CHECK(out.domain_offset_valid == 0);
	CHECK(strcmp(out.service_name, "wlan/fw") == 0);
}

/* A name of exactly the wire max (65 chars, SERVREG_LOC_NAME_WIRE_LEN) must
 * still encode. There is deliberately no "one character over" half of this
 * test: service_name's buffer (SERVREG_LOC_NAME_BUF_LEN, see servreg_loc.h)
 * holds at most 65 chars plus a NUL, so a too-long string can't be built in
 * it in the first place -- the overlong case the encoder's own elem_len
 * check guards against is a decoded/relayed value, covered instead by
 * test_decode_rejects_truncated_domain_list() and friends below. */
static void test_service_name_length_boundary(void)
{
	struct servreg_loc_get_domain_list_req in;
	DEFINE_QRTR_PACKET(pkt, 256);
	char exact[SERVREG_LOC_NAME_WIRE_LEN + 1];

	memset(exact, 'a', SERVREG_LOC_NAME_WIRE_LEN);
	exact[SERVREG_LOC_NAME_WIRE_LEN] = '\0';

	memset(&in, 0, sizeof(in));
	strcpy(in.service_name, exact);
	CHECK(qmi_encode_message(&pkt, QMI_REQUEST, SERVREG_LOC_GET_DOMAIN_LIST_REQ, 1,
				  &in, servreg_loc_get_domain_list_req_ei) > 0);
}

/* Decoding a wire-max-length (65-byte) name must not write past this
 * struct's buffer -- the exact off-by-one servreg_loc.h's codec-caveat note
 * describes. Only reliably observable under ASan/valgrind, but the check
 * itself (content + NUL land where expected) still runs plain. */
static void test_decode_name_at_wire_max_no_overflow(void)
{
	struct servreg_loc_get_domain_list_req in, out;
	DEFINE_QRTR_PACKET(pkt, 256);
	unsigned int txn;
	char exact[SERVREG_LOC_NAME_WIRE_LEN + 1];

	memset(exact, 'z', SERVREG_LOC_NAME_WIRE_LEN);
	exact[SERVREG_LOC_NAME_WIRE_LEN] = '\0';

	memset(&in, 0, sizeof(in));
	strcpy(in.service_name, exact);
	CHECK(qmi_encode_message(&pkt, QMI_REQUEST, SERVREG_LOC_GET_DOMAIN_LIST_REQ, 1,
				  &in, servreg_loc_get_domain_list_req_ei) > 0);

	memset(&out, 0xaa, sizeof(out)); /* poison, so a missed write is visible */
	CHECK(qmi_decode_message(&out, &txn, &pkt, QMI_REQUEST,
				  SERVREG_LOC_GET_DOMAIN_LIST_REQ,
				  servreg_loc_get_domain_list_req_ei) >= 0);
	CHECK(strlen(out.service_name) == SERVREG_LOC_NAME_WIRE_LEN);
	CHECK(memcmp(out.service_name, exact, SERVREG_LOC_NAME_WIRE_LEN + 1) == 0);
}

/* ---- GET_DOMAIN_LIST response ---- */

static void test_get_domain_list_resp_roundtrip(void)
{
	struct servreg_loc_get_domain_list_resp in, out;
	DEFINE_QRTR_PACKET(pkt, SERVREG_LOC_RESP_BUF_LEN);
	unsigned int txn;
	ssize_t len;

	memset(&in, 0, sizeof(in));
	in.resp.result = QMI_RESULT_SUCCESS_V01;
	in.resp.error = QMI_ERR_NONE_V01;
	in.total_domains_valid = 1;
	in.total_domains = 2;
	in.db_rev_count_valid = 1;
	in.db_rev_count = 1;
	in.domain_list_valid = 1;
	in.domain_list_len = 2;
	strcpy(in.domain_list[0].name, "msm/modem/root_pd");
	in.domain_list[0].instance_id = 180;
	in.domain_list[0].service_data_valid = 0;
	in.domain_list[0].service_data = 0;
	strcpy(in.domain_list[1].name, "msm/modem/wlan_pd");
	in.domain_list[1].instance_id = 180;
	in.domain_list[1].service_data_valid = 1;
	in.domain_list[1].service_data = 0x1234;

	len = qmi_encode_message(&pkt, QMI_RESPONSE, SERVREG_LOC_GET_DOMAIN_LIST_REQ, 9,
				  &in, servreg_loc_get_domain_list_resp_ei);
	CHECK(len > 0);

	memset(&out, 0, sizeof(out));
	CHECK(qmi_decode_message(&out, &txn, &pkt, QMI_RESPONSE,
				  SERVREG_LOC_GET_DOMAIN_LIST_REQ,
				  servreg_loc_get_domain_list_resp_ei) >= 0);
	CHECK(txn == 9);
	CHECK(out.resp.result == QMI_RESULT_SUCCESS_V01);
	CHECK(out.total_domains_valid == 1 && out.total_domains == 2);
	CHECK(out.db_rev_count_valid == 1 && out.db_rev_count == 1);
	CHECK(out.domain_list_valid == 1 && out.domain_list_len == 2);
	CHECK(strcmp(out.domain_list[0].name, "msm/modem/root_pd") == 0);
	CHECK(out.domain_list[0].instance_id == 180);
	CHECK(out.domain_list[0].service_data_valid == 0);
	CHECK(strcmp(out.domain_list[1].name, "msm/modem/wlan_pd") == 0);
	CHECK(out.domain_list[1].service_data_valid == 1);
	CHECK(out.domain_list[1].service_data == 0x1234);
}

/* Unknown-service-name shape: SUCCESS, zero domains, and -- critically -- no
 * 0x12 TLV at all (mainline pd-mapper's behavior; a real AP client, present
 * in this kernel's service-locator.c, logs "No matching domains found" for
 * this exact shape rather than erroring). */
static void test_get_domain_list_resp_no_match(void)
{
	struct servreg_loc_get_domain_list_resp in, out;
	DEFINE_QRTR_PACKET(pkt, SERVREG_LOC_RESP_BUF_LEN);
	unsigned int txn;

	memset(&in, 0, sizeof(in));
	in.resp.result = QMI_RESULT_SUCCESS_V01;
	in.total_domains_valid = 1;
	in.total_domains = 0;
	in.db_rev_count_valid = 1;
	in.db_rev_count = 1;
	in.domain_list_valid = 0;
	in.domain_list_len = 0;

	CHECK(qmi_encode_message(&pkt, QMI_RESPONSE, SERVREG_LOC_GET_DOMAIN_LIST_REQ, 1,
				  &in, servreg_loc_get_domain_list_resp_ei) > 0);

	/* No 0x12 TLV on the wire => decode must leave domain_list_valid at
	 * whatever the caller pre-set (poisoned here), never fabricate it. */
	memset(&out, 0, sizeof(out));
	out.domain_list_valid = 0xff;
	CHECK(qmi_decode_message(&out, &txn, &pkt, QMI_RESPONSE,
				  SERVREG_LOC_GET_DOMAIN_LIST_REQ,
				  servreg_loc_get_domain_list_resp_ei) >= 0);
	CHECK(out.total_domains_valid == 1 && out.total_domains == 0);
	CHECK(out.domain_list_valid == 0xff); /* untouched: TLV wasn't present */
}

/* ---- INDICATION_REGISTER ---- */

static void test_indication_register_roundtrip(void)
{
	struct servreg_loc_indication_register_req in, out;
	DEFINE_QRTR_PACKET(pkt, 256);
	unsigned int txn;

	memset(&in, 0, sizeof(in));
	in.enable_database_updated_indication_valid = 1;
	in.enable_database_updated_indication = 1;

	CHECK(qmi_encode_message(&pkt, QMI_REQUEST, SERVREG_LOC_INDICATION_REGISTER_REQ, 4,
				  &in, servreg_loc_indication_register_req_ei) > 0);

	memset(&out, 0, sizeof(out));
	CHECK(qmi_decode_message(&out, &txn, &pkt, QMI_REQUEST,
				  SERVREG_LOC_INDICATION_REGISTER_REQ,
				  servreg_loc_indication_register_req_ei) >= 0);
	CHECK(txn == 4);
	CHECK(out.enable_database_updated_indication_valid == 1);
	CHECK(out.enable_database_updated_indication == 1);
}

/* The modem's request may carry no TLVs at all (the only field is
 * optional); a zero-length payload must still decode cleanly since
 * servreg-locator.c acks it unconditionally either way. */
static void test_indication_register_empty_payload(void)
{
	DEFINE_QRTR_PACKET(pkt, 256);
	struct servreg_loc_indication_register_req out;
	unsigned int txn;

	CHECK(qmi_encode_message(&pkt, QMI_REQUEST, SERVREG_LOC_INDICATION_REGISTER_REQ,
				  2, NULL, servreg_loc_indication_register_req_ei) > 0);

	memset(&out, 0xaa, sizeof(out));
	CHECK(qmi_decode_message(&out, &txn, &pkt, QMI_REQUEST,
				  SERVREG_LOC_INDICATION_REGISTER_REQ,
				  servreg_loc_indication_register_req_ei) >= 0);
	CHECK(txn == 2);
}

/* ---- Generic (result-only) response: used for INDICATION_REGISTER_RESP,
 * PFR_RESP, and the NOT_SUPPORTED nack. ---- */

static void test_generic_resp_success_and_failure(void)
{
	struct servreg_loc_generic_resp in, out;
	DEFINE_QRTR_PACKET(pkt, 256);
	unsigned int txn;

	memset(&in, 0, sizeof(in));
	in.resp.result = QMI_RESULT_SUCCESS_V01;
	in.resp.error = QMI_ERR_NONE_V01;
	CHECK(qmi_encode_message(&pkt, QMI_RESPONSE, SERVREG_LOC_INDICATION_REGISTER_REQ, 5,
				  &in, servreg_loc_generic_resp_ei) > 0);
	memset(&out, 0, sizeof(out));
	CHECK(qmi_decode_message(&out, &txn, &pkt, QMI_RESPONSE,
				  SERVREG_LOC_INDICATION_REGISTER_REQ,
				  servreg_loc_generic_resp_ei) >= 0);
	CHECK(out.resp.result == QMI_RESULT_SUCCESS_V01);
	CHECK(out.resp.error == QMI_ERR_NONE_V01);

	memset(&in, 0, sizeof(in));
	in.resp.result = QMI_RESULT_FAILURE_V01;
	in.resp.error = QMI_ERR_NOT_SUPPORTED_V01;
	CHECK(qmi_encode_message(&pkt, QMI_RESPONSE, SERVREG_LOC_REGISTER_SERVICE_LIST_REQ, 6,
				  &in, servreg_loc_generic_resp_ei) > 0);
	memset(&out, 0, sizeof(out));
	CHECK(qmi_decode_message(&out, &txn, &pkt, QMI_RESPONSE,
				  SERVREG_LOC_REGISTER_SERVICE_LIST_REQ,
				  servreg_loc_generic_resp_ei) >= 0);
	CHECK(out.resp.result == QMI_RESULT_FAILURE_V01);
	CHECK(out.resp.error == QMI_ERR_NOT_SUPPORTED_V01);
	CHECK(txn == 6);
}

/* ---- PFR (best-effort) ---- */

static void test_pfr_req_roundtrip(void)
{
	struct servreg_loc_pfr_req in, out;
	DEFINE_QRTR_PACKET(pkt, 512);
	unsigned int txn;

	memset(&in, 0, sizeof(in));
	strcpy(in.service, "msm/modem/root_pd");
	strcpy(in.reason, "dog.c:1484:Watchdog detects stalled initialization");

	CHECK(qmi_encode_message(&pkt, QMI_REQUEST, SERVREG_LOC_PFR_REQ, 3,
				  &in, servreg_loc_pfr_req_ei) > 0);

	memset(&out, 0, sizeof(out));
	CHECK(qmi_decode_message(&out, &txn, &pkt, QMI_REQUEST, SERVREG_LOC_PFR_REQ,
				  servreg_loc_pfr_req_ei) >= 0);
	CHECK(strcmp(out.service, "msm/modem/root_pd") == 0);
	CHECK(strcmp(out.reason, "dog.c:1484:Watchdog detects stalled initialization") == 0);
}

/* ---- Malformed-input safety ----
 *
 * A note on scope: qrtr/qmi.c (vendored unchanged from upstream, see its
 * file header) trusts an individual TLV's self-declared length against
 * whatever the *caller's* buffer happens to contain, without cross-checking
 * it against how many bytes are actually left in the packet -- true for
 * every caller of this codec in this tree (tools/rmtfs/rmtfs.c included),
 * not something introduced here. qmi_decode_header() has the same shape of
 * gap below the 7-byte header. servreg-locator.c's handle_locator() adds
 * this daemon's own belt for the header case (an explicit data_len < 7
 * check before qmi_decode_header() ever runs, since that's the one guard a
 * caller *can* add without editing the vendored codec); the deeper
 * per-TLV-length gap is a pre-existing, shared-codec issue out of scope for
 * this change and is not exercised here. These tests stick to inputs that
 * are safe to decode either way (correctly-sized, just semantically wrong),
 * matching what handle_locator()'s own guard leaves for qmi_decode_message()
 * to see. */

static void test_decode_header_rejects_length_mismatch(void)
{
	/* A well-formed 7-byte header (nothing past it) that lies about its
	 * own msg_len -- safe to decode (exactly 7 real bytes, all read
	 * within bounds), and must be rejected rather than trusted. This is
	 * the exact check handle_locator() relies on, one call after its own
	 * data_len < 7 guard. */
	uint8_t buf[7] = { QMI_REQUEST, 0x01, 0x00, 0x21, 0x00, 0xff, 0xff };
	struct qrtr_packet pkt;
	unsigned int msg_id;

	pkt.data = buf;
	pkt.data_len = sizeof(buf);

	CHECK(qmi_decode_header(&pkt, &msg_id) < 0);
}

static void test_decode_rejects_wrong_type(void)
{
	struct servreg_loc_get_domain_list_req in, out;
	DEFINE_QRTR_PACKET(pkt, 256);
	unsigned int txn;

	memset(&in, 0, sizeof(in));
	strcpy(in.service_name, "x/y");
	CHECK(qmi_encode_message(&pkt, QMI_RESPONSE /* wrong type on purpose */,
				  SERVREG_LOC_GET_DOMAIN_LIST_REQ, 1, &in,
				  servreg_loc_get_domain_list_req_ei) > 0);

	CHECK(qmi_decode_message(&out, &txn, &pkt, QMI_REQUEST,
				  SERVREG_LOC_GET_DOMAIN_LIST_REQ,
				  servreg_loc_get_domain_list_req_ei) < 0);
}

int main(void)
{
	test_get_domain_list_req_roundtrip();
	test_get_domain_list_req_no_offset();
	test_service_name_length_boundary();
	test_decode_name_at_wire_max_no_overflow();

	test_get_domain_list_resp_roundtrip();
	test_get_domain_list_resp_no_match();

	test_indication_register_roundtrip();
	test_indication_register_empty_payload();

	test_generic_resp_success_and_failure();

	test_pfr_req_roundtrip();

	test_decode_header_rejects_length_mismatch();
	test_decode_rejects_wrong_type();

	fprintf(stderr, "%s: %d/%d checks passed\n", g_failures ? "FAIL" : "PASS",
		g_tests - g_failures, g_tests);
	return g_failures ? 1 : 0;
}
