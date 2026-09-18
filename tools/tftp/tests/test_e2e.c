/* End-to-end loopback tests for tftpserv.c's socket-facing logic (Round 1
 * review F9): tftpserv.c is compiled with -DTFTP_TEST_HOOKS and linked
 * against tests/fake_qrtr.c's in-memory qrtr_open/close/sendto/recvfrom/
 * publish instead of tools/msmipc.c's real AF_MSM_IPC ones (which don't
 * exist off the real device), so the daemon's actual RRQ/WRQ/OACK/DATA/ACK
 * state machine runs, just over a fake transport. Drives it via
 * service_control() (one control-socket packet) and
 * tftp_service_clients_once() (service every active client once + sweep
 * idle timeouts), both exposed only under TFTP_TEST_HOOKS -- see
 * tests/tftpserv_test.h and tftpserv.c's file header.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../ramfs.h"
#include "../tftp.h"
#include "../translate.h"
#include "fake_qrtr.h"
#include "tftpserv_test.h"

static int g_failures;
static int g_tests;

#define CHECK(cond) do { \
		g_tests++; \
		if (!(cond)) { \
			g_failures++; \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		} \
	} while (0)

#define CLIENT_NODE 5

static char g_tmproot[] = "/tmp/tftp-e2e-test.XXXXXX";
static char g_fw_dir[600];
static char g_seed_dir[600];

/* ---- fixtures (same shape as tests/test_translate.c's, duplicated for
 * this file's independence) ---- */

static void write_file(const char *path, const void *data, size_t size)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	if (fd < 0) {
		perror(path);
		exit(1);
	}
	if (size && write(fd, data, size) != (ssize_t)size) {
		perror(path);
		exit(1);
	}
	close(fd);
}

static void write_pattern(const char *path, size_t size)
{
	unsigned char *buf = malloc(size ? size : 1);
	size_t i;

	for (i = 0; i < size; i++)
		buf[i] = (unsigned char)(i & 0xff);
	write_file(path, buf, size);
	free(buf);
}

static void mkdir_p(const char *path)
{
	char tmp[512];
	char *p;

	snprintf(tmp, sizeof(tmp), "%s", path);
	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0700);
			*p = '/';
		}
	}
	mkdir(tmp, 0700);
}

static char *joinpath(char *out, size_t outsz, const char *base, const char *rel)
{
	snprintf(out, outsz, "%s/%s", base, rel);
	return out;
}

/* Firmware file used by the RRQ-of-firmware and client-limit tests: 700
 * bytes (> one 512-byte default block, so single- and multi-block RRQs
 * both have a real fixture) of a deterministic byte pattern. */
#define FW_FILE_SIZE 700
#define FW_FILE_NAME "modemr.bin"

static void setup_fixtures(void)
{
	char p[700];

	if (!mkdtemp(g_tmproot)) {
		perror("mkdtemp");
		exit(1);
	}

	joinpath(g_fw_dir, sizeof(g_fw_dir), g_tmproot, "firmware");
	mkdir_p(g_fw_dir);
	joinpath(p, sizeof(p), g_fw_dir, FW_FILE_NAME);
	write_pattern(p, FW_FILE_SIZE);

	joinpath(g_seed_dir, sizeof(g_seed_dir), g_tmproot, "seed");
	mkdir_p(g_seed_dir);
	joinpath(p, sizeof(p), g_seed_dir, "shob.bin");
	write_pattern(p, 37282);
	joinpath(p, sizeof(p), g_seed_dir, "dhob.bin");
	write_pattern(p, 16384);
	joinpath(p, sizeof(p), g_seed_dir, "server_check.txt");
	write_file(p, "hello", 5);
	joinpath(p, sizeof(p), g_seed_dir, "hob_report.txt");
	write_pattern(p, 4757);
	joinpath(p, sizeof(p), g_seed_dir, "dhob_report.txt");
	write_pattern(p, 138);
	mkdir_p(joinpath(p, sizeof(p), g_seed_dir, "mot_rfs"));
	joinpath(p, sizeof(p), g_seed_dir, "mot_rfs/imei_sv");
	write_file(p, "7", 1);
	mkdir_p(joinpath(p, sizeof(p), g_seed_dir, "datablock"));
	joinpath(p, sizeof(p), g_seed_dir, "datablock/id_00");
	write_pattern(p, 2600);
	joinpath(p, sizeof(p), g_seed_dir, "datablock/id_01");
	write_pattern(p, 2600);
}

/* Fresh daemon + transport state for one test case. */
static void reset_all(void)
{
	fake_qrtr_reset();
	tftp_test_reset();
	ramfs_init();
	if (translate_init(g_fw_dir) < 0) {
		fprintf(stderr, "translate_init failed\n");
		exit(1);
	}
	if (translate_seed(g_seed_dir) < 0) {
		fprintf(stderr, "translate_seed failed\n");
		exit(1);
	}
	tftp_test_clock_ms = 0;
}

/* ---- wire-format builders ---- */

static size_t build_request(uint8_t *buf, uint16_t opcode, const char *filename,
			     const uint8_t *opts, size_t opts_len)
{
	size_t n = 0;

	buf[n++] = (uint8_t)(opcode >> 8);
	buf[n++] = (uint8_t)opcode;
	n += (size_t)snprintf((char *)buf + n, 256 - n, "%s", filename) + 1;
	n += (size_t)snprintf((char *)buf + n, 256 - n, "octet") + 1;
	if (opts && opts_len) {
		memcpy(buf + n, opts, opts_len);
		n += opts_len;
	}
	return n;
}

struct optbuilder {
	uint8_t buf[512];
	size_t len;
};

static void ob_add(struct optbuilder *ob, const char *name, const char *value)
{
	ob->len += (size_t)snprintf((char *)ob->buf + ob->len, sizeof(ob->buf) - ob->len,
				     "%s", name) + 1;
	ob->len += (size_t)snprintf((char *)ob->buf + ob->len, sizeof(ob->buf) - ob->len,
				     "%s", value) + 1;
}

static size_t build_ack(uint8_t *buf, uint16_t block)
{
	buf[0] = 0;
	buf[1] = TFTP_OP_ACK;
	buf[2] = (uint8_t)(block >> 8);
	buf[3] = (uint8_t)block;
	return 4;
}

static size_t build_data(uint8_t *buf, uint16_t block, const void *payload, size_t len)
{
	buf[0] = 0;
	buf[1] = TFTP_OP_DATA;
	buf[2] = (uint8_t)(block >> 8);
	buf[3] = (uint8_t)block;
	if (len)
		memcpy(buf + 4, payload, len);
	return 4 + len;
}

/* Looks up option `name`'s value string within a received OACK packet
 * (buf/len includes the 2-byte opcode). Returns 0 and fills `out` on
 * success, -1 if not present. */
static int oack_get(const uint8_t *buf, size_t len, const char *name, char *out, size_t outsz)
{
	const char *p = (const char *)buf + 2;
	const char *end = (const char *)buf + len;

	while (p < end) {
		size_t nlen = strnlen(p, (size_t)(end - p));
		const char *namep = p;
		size_t vlen;

		if (nlen == (size_t)(end - p))
			return -1;
		p += nlen + 1;
		if (p >= end)
			return -1;
		vlen = strnlen(p, (size_t)(end - p));
		if (vlen == (size_t)(end - p))
			return -1;
		if (strcmp(namep, name) == 0) {
			snprintf(out, outsz, "%s", p);
			return 0;
		}
		p += vlen + 1;
	}
	return -1;
}

/* ---- tests ---- */

static void test_publish_args(void)
{
	uint32_t service;
	uint16_t version, instance;
	int ctrl;

	reset_all();
	ctrl = tftp_open_and_publish();
	CHECK(ctrl >= 0);

	/* Round 1 review F1: wire field 0x00000001 == version 1, instance 0
	 * (msmipc.c's qrtr_publish() encodes (instance << 8) | version). */
	CHECK(fake_qrtr_last_publish(&service, &version, &instance) == 0);
	CHECK(service == 0x1000);
	CHECK(version == 1);
	CHECK(instance == 0);
}

static void test_rrq_readwrite_with_options(void)
{
	uint8_t req[256], resp[600];
	struct optbuilder ob;
	int ctrl, eph;
	size_t n;
	char val[32];
	ssize_t len;
	uint32_t node, port;

	reset_all();
	ctrl = tftp_open_and_publish();

	memset(&ob, 0, sizeof(ob));
	ob_add(&ob, "blksize", "1024");
	ob_add(&ob, "tsize", "0");
	ob_add(&ob, "timeoutms", "2000");
	n = build_request(req, TFTP_OP_RRQ, "/readwrite/server_check.txt", ob.buf, ob.len);

	fake_qrtr_inject(ctrl, CLIENT_NODE, 100, req, n);
	service_control(ctrl);

	eph = fake_qrtr_last_opened();
	CHECK(eph != ctrl);
	CHECK(fake_qrtr_sent_count(eph) == 1);
	len = fake_qrtr_sent(eph, 0, &node, &port, resp, sizeof(resp));
	CHECK(len > 0);
	CHECK(resp[0] == 0 && resp[1] == TFTP_OP_OACK);
	CHECK(node == CLIENT_NODE && port == 100);
	CHECK(oack_get(resp, (size_t)len, "blksize", val, sizeof(val)) == 0 &&
	      strcmp(val, "1024") == 0);
	CHECK(oack_get(resp, (size_t)len, "tsize", val, sizeof(val)) == 0 &&
	      strcmp(val, "5") == 0); /* "hello" is 5 bytes */
	CHECK(oack_get(resp, (size_t)len, "timeoutms", val, sizeof(val)) == 0 &&
	      strcmp(val, "2000") == 0);

	/* ACK(0) -> DATA block 1, the whole 5-byte file (short: finishing). */
	n = build_ack(req, 0);
	fake_qrtr_inject(eph, CLIENT_NODE, 100, req, n);
	tftp_service_clients_once();

	CHECK(fake_qrtr_sent_count(eph) == 2);
	len = fake_qrtr_sent(eph, 1, NULL, NULL, resp, sizeof(resp));
	CHECK(len == 4 + 5);
	CHECK(resp[1] == TFTP_OP_DATA && resp[2] == 0 && resp[3] == 1);
	CHECK(memcmp(resp + 4, "hello", 5) == 0);
	CHECK(tftp_test_active_clients() == 1); /* finishing, not yet closed */

	/* Closing ACK(1) -> transfer complete. */
	n = build_ack(req, 1);
	fake_qrtr_inject(eph, CLIENT_NODE, 100, req, n);
	tftp_service_clients_once();
	CHECK(tftp_test_active_clients() == 0);
}

static void test_rrq_timeout_name_echoed(void)
{
	uint8_t req[256], resp[128];
	struct optbuilder ob;
	int ctrl, eph;
	size_t n;
	char val[32];
	ssize_t len;

	reset_all();
	ctrl = tftp_open_and_publish();

	memset(&ob, 0, sizeof(ob));
	ob_add(&ob, "timeout", "5");
	n = build_request(req, TFTP_OP_RRQ, "/readwrite/server_check.txt", ob.buf, ob.len);
	fake_qrtr_inject(ctrl, CLIENT_NODE, 101, req, n);
	service_control(ctrl);

	eph = fake_qrtr_last_opened();
	len = fake_qrtr_sent(eph, 0, NULL, NULL, resp, sizeof(resp));
	CHECK(len > 0 && resp[1] == TFTP_OP_OACK);
	/* Round 1 review F4: must echo "timeout" (seconds), not "timeoutms". */
	CHECK(oack_get(resp, (size_t)len, "timeout", val, sizeof(val)) == 0 &&
	      strcmp(val, "5") == 0);
	CHECK(oack_get(resp, (size_t)len, "timeoutms", val, sizeof(val)) == -1);
}

/* Round 2 review F12: once finishing, only the ACK matching final_block may
 * close the transfer -- a duplicate/late ACK of an earlier block must be
 * ignored (client stays open, no new packet sent) rather than ending it
 * "ok" ahead of the real closing ACK. */
static void test_rrq_finishing_ignores_wrong_ack(void)
{
	uint8_t req[256], resp[600];
	struct optbuilder ob;
	int ctrl, eph;
	size_t n;
	ssize_t len;

	reset_all();
	ctrl = tftp_open_and_publish();

	/* Negotiated (do_oack) path, so the short final block is sent via
	 * handle_reader()'s ACK-driven loop, which is what actually sets
	 * c->finishing (see tftpserv.c's handle_rrq() for the unnegotiated
	 * path's own, separate, immediate-send behavior). OACK -> ACK(0) ->
	 * DATA block 1 (all 5 bytes of "hello", short -> finishing,
	 * final_block == 1). */
	memset(&ob, 0, sizeof(ob));
	ob_add(&ob, "blksize", "512");
	n = build_request(req, TFTP_OP_RRQ, "/readwrite/server_check.txt", ob.buf, ob.len);
	fake_qrtr_inject(ctrl, CLIENT_NODE, 114, req, n);
	service_control(ctrl);
	eph = fake_qrtr_last_opened();
	CHECK(fake_qrtr_sent_count(eph) == 1); /* OACK */

	n = build_ack(req, 0);
	fake_qrtr_inject(eph, CLIENT_NODE, 114, req, n);
	tftp_service_clients_once();
	CHECK(fake_qrtr_sent_count(eph) == 2);
	len = fake_qrtr_sent(eph, 1, NULL, NULL, resp, sizeof(resp));
	CHECK(len == 4 + 5 && resp[3] == 1);
	CHECK(tftp_test_active_clients() == 1); /* finishing, final_block == 1 */

	/* A stray ACK(0) (final_block - 1, e.g. a retransmit that crossed
	 * with our DATA) must not close it or send anything new. */
	n = build_ack(req, 0);
	fake_qrtr_inject(eph, CLIENT_NODE, 114, req, n);
	tftp_service_clients_once();
	CHECK(tftp_test_active_clients() == 1);
	CHECK(fake_qrtr_sent_count(eph) == 2);

	/* The correct ACK(1) closes it. */
	n = build_ack(req, 1);
	fake_qrtr_inject(eph, CLIENT_NODE, 114, req, n);
	tftp_service_clients_once();
	CHECK(tftp_test_active_clients() == 0);
	CHECK(fake_qrtr_sent_count(eph) == 2); /* still no new packet */
}

/* Round 1 review F3: rsize/seek termination, all three reachable cases.
 * Uses the seeded hob_report.txt (4757 bytes, deterministic pattern). */
static void test_rrq_seek_rsize_non_multiple(void)
{
	uint8_t req[256], resp[600];
	struct optbuilder ob;
	int ctrl, eph;
	size_t n;
	ssize_t len;

	reset_all();
	ctrl = tftp_open_and_publish();

	memset(&ob, 0, sizeof(ob));
	ob_add(&ob, "blksize", "512");
	ob_add(&ob, "rsize", "600"); /* not a multiple of 512 */
	n = build_request(req, TFTP_OP_RRQ, "/readwrite/hob_report.txt", ob.buf, ob.len);
	fake_qrtr_inject(ctrl, CLIENT_NODE, 102, req, n);
	service_control(ctrl);
	eph = fake_qrtr_last_opened();

	n = build_ack(req, 0);
	fake_qrtr_inject(eph, CLIENT_NODE, 102, req, n);
	tftp_service_clients_once();
	len = fake_qrtr_sent(eph, 1, NULL, NULL, resp, sizeof(resp));
	CHECK(len == 4 + 512); /* first block: full */
	CHECK(tftp_test_active_clients() == 1);

	n = build_ack(req, 1);
	fake_qrtr_inject(eph, CLIENT_NODE, 102, req, n);
	tftp_service_clients_once();
	len = fake_qrtr_sent(eph, 2, NULL, NULL, resp, sizeof(resp));
	CHECK(len == 4 + 88); /* 600 - 512 = 88: short, natural rsize cap */
	CHECK(tftp_test_active_clients() == 1); /* finishing */

	n = build_ack(req, 2);
	fake_qrtr_inject(eph, CLIENT_NODE, 102, req, n);
	tftp_service_clients_once();
	CHECK(tftp_test_active_clients() == 0);
}

static void test_rrq_seek_rsize_exact_multiple(void)
{
	uint8_t req[256], resp[600];
	struct optbuilder ob;
	int ctrl, eph;
	size_t n;
	ssize_t len;

	reset_all();
	ctrl = tftp_open_and_publish();

	memset(&ob, 0, sizeof(ob));
	ob_add(&ob, "blksize", "512");
	ob_add(&ob, "rsize", "1024"); /* exact multiple of 512 */
	n = build_request(req, TFTP_OP_RRQ, "/readwrite/hob_report.txt", ob.buf, ob.len);
	fake_qrtr_inject(ctrl, CLIENT_NODE, 103, req, n);
	service_control(ctrl);
	eph = fake_qrtr_last_opened();

	n = build_ack(req, 0);
	fake_qrtr_inject(eph, CLIENT_NODE, 103, req, n);
	tftp_service_clients_once();
	len = fake_qrtr_sent(eph, 1, NULL, NULL, resp, sizeof(resp));
	CHECK(len == 4 + 512); /* block 1: full */

	n = build_ack(req, 1);
	fake_qrtr_inject(eph, CLIENT_NODE, 103, req, n);
	tftp_service_clients_once();
	len = fake_qrtr_sent(eph, 2, NULL, NULL, resp, sizeof(resp));
	CHECK(len == 4 + 512); /* block 2: also full -- rsize exhausted exactly here */
	CHECK(tftp_test_active_clients() == 1); /* NOT finishing yet: both blocks were full */

	/* Round 1 review F3a: this ACK must produce an explicit, empty
	 * block 3 -- not silence, and not a torn-down transfer. */
	n = build_ack(req, 2);
	fake_qrtr_inject(eph, CLIENT_NODE, 103, req, n);
	tftp_service_clients_once();
	CHECK(fake_qrtr_sent_count(eph) == 4); /* OACK, block1, block2, block3 */
	len = fake_qrtr_sent(eph, 3, NULL, NULL, resp, sizeof(resp));
	CHECK(len == 4); /* block 3: empty, header only */
	CHECK(resp[1] == TFTP_OP_DATA && resp[2] == 0 && resp[3] == 3);
	CHECK(tftp_test_active_clients() == 1); /* now finishing */

	n = build_ack(req, 3);
	fake_qrtr_inject(eph, CLIENT_NODE, 103, req, n);
	tftp_service_clients_once();
	CHECK(tftp_test_active_clients() == 0);
	CHECK(fake_qrtr_sent_count(eph) == 4); /* no further packets sent */
}

static void test_rrq_seek_rsize_beyond_eof(void)
{
	uint8_t req[256], resp[600];
	struct optbuilder ob;
	int ctrl, eph;
	size_t n;
	ssize_t len;

	reset_all();
	ctrl = tftp_open_and_publish();

	/* hob_report.txt is 4757 bytes; seek 4700 leaves only 57, but rsize
	 * asks for 200 from there -- more than the file actually has. */
	memset(&ob, 0, sizeof(ob));
	ob_add(&ob, "blksize", "512");
	ob_add(&ob, "seek", "4700");
	ob_add(&ob, "rsize", "200");
	n = build_request(req, TFTP_OP_RRQ, "/readwrite/hob_report.txt", ob.buf, ob.len);
	fake_qrtr_inject(ctrl, CLIENT_NODE, 104, req, n);
	service_control(ctrl);
	eph = fake_qrtr_last_opened();

	/* Round 1 review F3b: previously tftp_send_data() returned -1 here
	 * (response_size 200 > available 57) and the transfer was torn down
	 * with no ERROR packet. Must now send the 57 available bytes. */
	n = build_ack(req, 0);
	fake_qrtr_inject(eph, CLIENT_NODE, 104, req, n);
	tftp_service_clients_once();
	CHECK(fake_qrtr_sent_count(eph) == 2); /* OACK + one DATA, no ERROR */
	len = fake_qrtr_sent(eph, 1, NULL, NULL, resp, sizeof(resp));
	CHECK(len == 4 + 57);
	CHECK(resp[1] == TFTP_OP_DATA);
	CHECK(tftp_test_active_clients() == 1); /* finishing */

	n = build_ack(req, 1);
	fake_qrtr_inject(eph, CLIENT_NODE, 104, req, n);
	tftp_service_clients_once();
	CHECK(tftp_test_active_clients() == 0);
}

static void test_rrq_firmware_file(void)
{
	uint8_t req[256], resp[600];
	int ctrl, eph;
	size_t n;
	ssize_t len;
	char path[64];

	reset_all();
	ctrl = tftp_open_and_publish();
	snprintf(path, sizeof(path), "/readonly/firmware/image/%s", FW_FILE_NAME);

	n = build_request(req, TFTP_OP_RRQ, path, NULL, 0);
	fake_qrtr_inject(ctrl, CLIENT_NODE, 105, req, n);
	service_control(ctrl);
	eph = fake_qrtr_last_opened();

	/* No options: DATA block 1 is sent immediately, no OACK. */
	CHECK(fake_qrtr_sent_count(eph) == 1);
	len = fake_qrtr_sent(eph, 0, NULL, NULL, resp, sizeof(resp));
	CHECK(len == 4 + TFTP_DEFAULT_BLKSIZE);
	CHECK(resp[1] == TFTP_OP_DATA && resp[3] == 1);
	CHECK((unsigned char)resp[4] == 0); /* pattern byte 0 */

	n = build_ack(req, 1);
	fake_qrtr_inject(eph, CLIENT_NODE, 105, req, n);
	tftp_service_clients_once();
	len = fake_qrtr_sent(eph, 1, NULL, NULL, resp, sizeof(resp));
	CHECK(len == 4 + (FW_FILE_SIZE - TFTP_DEFAULT_BLKSIZE)); /* final short block */
	CHECK((unsigned char)resp[4] == (unsigned char)(TFTP_DEFAULT_BLKSIZE & 0xff));

	n = build_ack(req, 2);
	fake_qrtr_inject(eph, CLIENT_NODE, 105, req, n);
	tftp_service_clients_once();
	CHECK(tftp_test_active_clients() == 0);
}

static void test_rrq_enoent_fresh_socket(void)
{
	uint8_t req[256], resp[128];
	int ctrl, eph;
	size_t n;
	ssize_t len;

	reset_all();
	ctrl = tftp_open_and_publish();

	n = build_request(req, TFTP_OP_RRQ, "/readwrite/never-existed.bin", NULL, 0);
	fake_qrtr_inject(ctrl, CLIENT_NODE, 106, req, n);
	service_control(ctrl);

	/* No client is created for a rejected path -- the reply goes out on
	 * a fresh, throwaway socket (tftp_send_error_to()). */
	CHECK(tftp_test_active_clients() == 0);
	eph = fake_qrtr_last_opened();
	CHECK(eph != ctrl);
	CHECK(fake_qrtr_sent_count(eph) == 1);
	len = fake_qrtr_sent(eph, 0, NULL, NULL, resp, sizeof(resp));
	CHECK(len >= 4);
	CHECK(resp[1] == TFTP_OP_ERROR);
	CHECK(resp[3] == TFTP_ERROR_ENOENT);
}

static void test_wrq_no_options_ack0_and_content(void)
{
	uint8_t req[256], resp[16];
	int ctrl, eph;
	size_t n;
	ssize_t len;
	struct ram_file *rf;

	reset_all();
	ctrl = tftp_open_and_publish();

	n = build_request(req, TFTP_OP_WRQ, "/readwrite/server_check.txt", NULL, 0);
	fake_qrtr_inject(ctrl, CLIENT_NODE, 107, req, n);
	service_control(ctrl);
	eph = fake_qrtr_last_opened();

	/* Round 1 review's ported fix: no options -> ACK(0), not a (broken)
	 * attempt to send DATA on a write-only file. */
	CHECK(fake_qrtr_sent_count(eph) == 1);
	len = fake_qrtr_sent(eph, 0, NULL, NULL, resp, sizeof(resp));
	CHECK(len == 4 && resp[1] == TFTP_OP_ACK && resp[2] == 0 && resp[3] == 0);

	n = build_data(req, 1, "hello", 5);
	fake_qrtr_inject(eph, CLIENT_NODE, 107, req, n);
	tftp_service_clients_once();
	len = fake_qrtr_sent(eph, 1, NULL, NULL, resp, sizeof(resp));
	CHECK(len == 4 && resp[1] == TFTP_OP_ACK && resp[3] == 1);
	CHECK(tftp_test_active_clients() == 0); /* short block: WRQ done */

	rf = ramfs_lookup("rw/server_check.txt");
	CHECK(rf && rf->len == 5 && memcmp(rf->buf, "hello", 5) == 0);
}

static void test_wrq_with_options_oack(void)
{
	uint8_t req[256], resp[16];
	struct optbuilder ob;
	int ctrl, eph;
	size_t n;
	ssize_t len;
	struct ram_file *rf;

	reset_all();
	ctrl = tftp_open_and_publish();

	memset(&ob, 0, sizeof(ob));
	ob_add(&ob, "blksize", "512");
	n = build_request(req, TFTP_OP_WRQ, "/readwrite/server_check.txt", ob.buf, ob.len);
	fake_qrtr_inject(ctrl, CLIENT_NODE, 108, req, n);
	service_control(ctrl);
	eph = fake_qrtr_last_opened();

	len = fake_qrtr_sent(eph, 0, NULL, NULL, resp, sizeof(resp));
	CHECK(len > 0 && resp[1] == TFTP_OP_OACK);

	n = build_data(req, 1, "hi!!", 4);
	fake_qrtr_inject(eph, CLIENT_NODE, 108, req, n);
	tftp_service_clients_once();
	CHECK(tftp_test_active_clients() == 0);

	rf = ramfs_lookup("rw/server_check.txt");
	CHECK(rf && rf->len == 4 && memcmp(rf->buf, "hi!!", 4) == 0);
}

static void test_wrq_append_imei_sv(void)
{
	uint8_t req[256], resp[16];
	struct optbuilder ob;
	int ctrl, eph;
	size_t n;
	struct ram_file *rf;

	reset_all();
	ctrl = tftp_open_and_publish();

	memset(&ob, 0, sizeof(ob));
	ob_add(&ob, "append", "1");
	n = build_request(req, TFTP_OP_WRQ, "/readwrite/mot_rfs/imei_sv", ob.buf, ob.len);
	fake_qrtr_inject(ctrl, CLIENT_NODE, 109, req, n);
	service_control(ctrl);
	eph = fake_qrtr_last_opened();

	n = build_data(req, 1, "9", 1);
	fake_qrtr_inject(eph, CLIENT_NODE, 109, req, n);
	tftp_service_clients_once();
	(void)fake_qrtr_sent(eph, 1, NULL, NULL, resp, sizeof(resp));

	rf = ramfs_lookup("rw/mot_rfs/imei_sv");
	/* Seeded with "7" (1 byte); append must keep it, not overwrite. */
	CHECK(rf && rf->len == 2 && memcmp(rf->buf, "79", 2) == 0);
}

static void test_wrq_wsize_window_cadence(void)
{
	uint8_t req[600], resp[16];
	struct optbuilder ob;
	int ctrl, eph;
	size_t n;
	unsigned char payload[512];
	int i;

	reset_all();
	ctrl = tftp_open_and_publish();
	memset(payload, 0x55, sizeof(payload));

	memset(&ob, 0, sizeof(ob));
	ob_add(&ob, "blksize", "512");
	ob_add(&ob, "wsize", "4");
	n = build_request(req, TFTP_OP_WRQ, "/readwrite/window-test.bin", ob.buf, ob.len);
	fake_qrtr_inject(ctrl, CLIENT_NODE, 110, req, n);
	service_control(ctrl);
	eph = fake_qrtr_last_opened();
	CHECK(fake_qrtr_sent_count(eph) == 1); /* OACK only so far */

	/* Blocks 1-3 (full blksize): no ACK expected until block 4. */
	for (i = 1; i <= 3; i++) {
		n = build_data(req, (uint16_t)i, payload, sizeof(payload));
		fake_qrtr_inject(eph, CLIENT_NODE, 110, req, n);
		tftp_service_clients_once();
		CHECK(fake_qrtr_sent_count(eph) == 1);
	}

	n = build_data(req, 4, payload, sizeof(payload));
	fake_qrtr_inject(eph, CLIENT_NODE, 110, req, n);
	tftp_service_clients_once();
	CHECK(fake_qrtr_sent_count(eph) == 2); /* one ACK for the whole window */
	(void)fake_qrtr_sent(eph, 1, NULL, NULL, resp, sizeof(resp));
	CHECK(resp[1] == TFTP_OP_ACK && resp[2] == 0 && resp[3] == 4);
	CHECK(tftp_test_active_clients() == 1); /* still open: no short block yet */

	/* Short final block ends it, ACKed immediately. */
	n = build_data(req, 5, payload, 10);
	fake_qrtr_inject(eph, CLIENT_NODE, 110, req, n);
	tftp_service_clients_once();
	CHECK(fake_qrtr_sent_count(eph) == 3);
	CHECK(tftp_test_active_clients() == 0);
}

static void test_wrq_out_of_sequence_data_error(void)
{
	uint8_t req[256], resp[128];
	int ctrl, eph;
	size_t n;
	ssize_t len;

	reset_all();
	ctrl = tftp_open_and_publish();

	n = build_request(req, TFTP_OP_WRQ, "/readwrite/server_check.txt", NULL, 0);
	fake_qrtr_inject(ctrl, CLIENT_NODE, 111, req, n);
	service_control(ctrl);
	eph = fake_qrtr_last_opened();

	/* Block 2 instead of the expected block 1. */
	n = build_data(req, 2, "xx", 2);
	fake_qrtr_inject(eph, CLIENT_NODE, 111, req, n);
	tftp_service_clients_once();

	CHECK(fake_qrtr_sent_count(eph) == 2); /* ACK(0) + ERROR */
	len = fake_qrtr_sent(eph, 1, NULL, NULL, resp, sizeof(resp));
	CHECK(len >= 4 && resp[1] == TFTP_OP_ERROR && resp[3] == TFTP_ERROR_EBADOP);
	CHECK(tftp_test_active_clients() == 0); /* aborted */
}

static void test_client_limit_9th_request_error(void)
{
	uint8_t req[256], resp[128];
	int ctrl, errsock;
	size_t n;
	ssize_t len;
	char path[64];
	int i;

	reset_all();
	ctrl = tftp_open_and_publish();
	snprintf(path, sizeof(path), "/readonly/firmware/image/%s", FW_FILE_NAME);

	for (i = 0; i < TFTP_MAX_CLIENTS; i++) {
		n = build_request(req, TFTP_OP_RRQ, path, NULL, 0);
		fake_qrtr_inject(ctrl, CLIENT_NODE, (uint32_t)(200 + i), req, n);
		service_control(ctrl);
	}
	CHECK(tftp_test_active_clients() == TFTP_MAX_CLIENTS);

	n = build_request(req, TFTP_OP_RRQ, path, NULL, 0);
	fake_qrtr_inject(ctrl, CLIENT_NODE, 299, req, n);
	service_control(ctrl);

	CHECK(tftp_test_active_clients() == TFTP_MAX_CLIENTS); /* unchanged */
	errsock = fake_qrtr_last_opened();
	len = fake_qrtr_sent(errsock, 0, NULL, NULL, resp, sizeof(resp));
	CHECK(len >= 4 && resp[1] == TFTP_OP_ERROR);
	CHECK(strstr((char *)resp + 4, "Max TFTP client limit reached") != NULL);
}

static void test_idle_timeout_sweep_frees_slot(void)
{
	uint8_t req[256];
	int ctrl;
	size_t n;
	char path[64];

	reset_all();
	ctrl = tftp_open_and_publish();
	snprintf(path, sizeof(path), "/readonly/firmware/image/%s", FW_FILE_NAME);

	tftp_test_clock_ms = 0;
	n = build_request(req, TFTP_OP_RRQ, path, NULL, 0);
	fake_qrtr_inject(ctrl, CLIENT_NODE, 112, req, n);
	service_control(ctrl);
	CHECK(tftp_test_active_clients() == 1);

	/* Default timeoutms is 1000 -> idle_timeout_for() = max(3000,5000) =
	 * 5000ms. Well past that, with no further activity. */
	tftp_test_clock_ms = 6000;
	tftp_service_clients_once();
	CHECK(tftp_test_active_clients() == 0);
}

static void test_blksize_wsize_clamped_in_oack(void)
{
	uint8_t req[256], resp[600];
	struct optbuilder ob;
	int ctrl, eph;
	size_t n;
	char val[32];
	ssize_t len;

	reset_all();
	ctrl = tftp_open_and_publish();

	memset(&ob, 0, sizeof(ob));
	ob_add(&ob, "blksize", "65464"); /* RFC 2348 max */
	ob_add(&ob, "wsize", "65535");   /* RFC 7440 max */
	n = build_request(req, TFTP_OP_RRQ, "/readwrite/server_check.txt", ob.buf, ob.len);
	fake_qrtr_inject(ctrl, CLIENT_NODE, 113, req, n);
	service_control(ctrl);
	eph = fake_qrtr_last_opened();

	len = fake_qrtr_sent(eph, 0, NULL, NULL, resp, sizeof(resp));
	CHECK(len > 0 && resp[1] == TFTP_OP_OACK);
	CHECK(oack_get(resp, (size_t)len, "blksize", val, sizeof(val)) == 0);
	CHECK(atoi(val) == TFTP_MAX_BLKSIZE);
	CHECK(oack_get(resp, (size_t)len, "wsize", val, sizeof(val)) == 0);
	CHECK(atoi(val) == TFTP_MAX_WSIZE);
}

int main(void)
{
	setup_fixtures();

	test_publish_args();
	test_rrq_readwrite_with_options();
	test_rrq_timeout_name_echoed();
	test_rrq_finishing_ignores_wrong_ack();
	test_rrq_seek_rsize_non_multiple();
	test_rrq_seek_rsize_exact_multiple();
	test_rrq_seek_rsize_beyond_eof();
	test_rrq_firmware_file();
	test_rrq_enoent_fresh_socket();
	test_wrq_no_options_ack0_and_content();
	test_wrq_with_options_oack();
	test_wrq_append_imei_sv();
	test_wrq_wsize_window_cadence();
	test_wrq_out_of_sequence_data_error();
	test_client_limit_9th_request_error();
	test_idle_timeout_sweep_frees_slot();
	test_blksize_wsize_clamped_in_oack();

	printf("%s: %d/%d checks passed\n", g_failures ? "FAIL" : "PASS",
	       g_tests - g_failures, g_tests);
	return g_failures ? 1 : 0;
}
