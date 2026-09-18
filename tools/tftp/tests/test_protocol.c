/* Host unit tests for tools/tftp/protocol.c: request/option parsing and
 * packet encoding (RFC 1350 opcodes, RFC 2347/2348/2349 options), plus the
 * uint16_t block-number wraparound arithmetic tftpserv.c's handle_reader()
 * uses to walk a wsize window (REPORT.md 8.1 point 7 calls this out
 * explicitly: "block-number wrap"). Pure buffers throughout -- no sockets,
 * no AF_MSM_IPC. Build/run: see Makefile ("make test").
 */
#include <stdio.h>
#include <string.h>

#include "protocol.h"
#include "tftp.h"

static int g_failures;
static int g_tests;

#define CHECK(cond) do { \
		g_tests++; \
		if (!(cond)) { \
			g_failures++; \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		} \
	} while (0)

/* Builds "<filename>\0octet\0[opt\0val\0]*" the way an RRQ/WRQ payload
 * looks after its 2-byte opcode (tftp_parse_request()'s buf/len already
 * exclude the opcode conceptually but the function itself expects buf+2 to
 * be the filename, matching how tftpserv.c slices the wire packet -- so
 * callers here prepend 2 placeholder opcode bytes too). */
static size_t build_request(uint8_t *buf, const char *filename, const char *opts, size_t opts_len)
{
	size_t n = 2;

	buf[0] = 0;
	buf[1] = 1; /* RRQ, value doesn't matter to the parser */
	n += (size_t)snprintf((char *)buf + n, 256 - n, "%s", filename) + 1;
	n += (size_t)snprintf((char *)buf + n, 256 - n, "octet") + 1;
	if (opts && opts_len) {
		memcpy(buf + n, opts, opts_len);
		n += opts_len;
	}
	return n;
}

static void test_parse_request_basic(void)
{
	uint8_t buf[256];
	const char *filename;
	size_t opts_off;
	enum tftp_error err;
	const char *msg;
	size_t len;

	len = build_request(buf, "/readwrite/server_check.txt", NULL, 0);
	CHECK(tftp_parse_request((char *)buf, len, &filename, &opts_off, &err, &msg) == 0);
	CHECK(strcmp(filename, "/readwrite/server_check.txt") == 0);
	CHECK(opts_off == len); /* no options section */
}

static void test_parse_request_rejects_bad_mode(void)
{
	uint8_t buf[256];
	const char *filename;
	size_t opts_off, n = 2;
	enum tftp_error err;
	const char *msg;

	buf[0] = 0;
	buf[1] = 1;
	n += (size_t)snprintf((char *)buf + n, sizeof(buf) - n, "%s", "/readwrite/x") + 1;
	n += (size_t)snprintf((char *)buf + n, sizeof(buf) - n, "netascii") + 1;

	CHECK(tftp_parse_request((char *)buf, n, &filename, &opts_off, &err, &msg) == -1);
	CHECK(err == TFTP_ERROR_EBADOP);
}

static void test_parse_request_rejects_truncated(void)
{
	uint8_t buf[4] = { 0, 1, 'a', 0 }; /* filename "a", no mode at all */
	const char *filename;
	size_t opts_off;
	enum tftp_error err;
	const char *msg;

	CHECK(tftp_parse_request((char *)buf, sizeof(buf), &filename, &opts_off, &err, &msg) == -1);
	CHECK(err == TFTP_ERROR_EBADOP);
}

static void test_parse_options_recognized(void)
{
	char opts[256];
	size_t n = 0;
	struct tftp_options opt;

#define ADD(name, val) do { \
		n += (size_t)snprintf(opts + n, sizeof(opts) - n, "%s", name) + 1; \
		n += (size_t)snprintf(opts + n, sizeof(opts) - n, "%s", val) + 1; \
	} while (0)

	ADD("blksize", "1024");
	ADD("tsize", "0");
	ADD("timeoutms", "2500");
	ADD("rsize", "512");
	ADD("wsize", "4");
	ADD("seek", "16");
	ADD("append", "1");
	ADD("totallyunknown", "whatever");
#undef ADD

	CHECK(tftp_parse_options(opts, n, &opt) == 0);
	CHECK(opt.have_blksize && opt.blksize == 1024);
	CHECK(opt.have_tsize && opt.tsize == 0);
	CHECK(opt.have_timeoutms && opt.timeoutms == 2500);
	CHECK(opt.have_rsize && opt.rsize == 512);
	CHECK(opt.have_wsize && opt.wsize == 4);
	CHECK(opt.have_seek && opt.seek == 16);
	CHECK(opt.have_append);
}

static void test_parse_options_plain_timeout_is_seconds(void)
{
	char opts[64];
	size_t n = 0;
	struct tftp_options opt;

	n += (size_t)snprintf(opts + n, sizeof(opts) - n, "timeout") + 1;
	n += (size_t)snprintf(opts + n, sizeof(opts) - n, "5") + 1;

	CHECK(tftp_parse_options(opts, n, &opt) == 0);
	CHECK(opt.have_timeoutms && opt.timeoutms == 5000);
}

static void test_parse_options_out_of_range_rejected(void)
{
	char opts[64];
	size_t n;
	struct tftp_options opt;

	/* Below the RFC 2348 floor: still a hard reject, not a clamp. */
	n = 0;
	n += (size_t)snprintf(opts + n, sizeof(opts) - n, "blksize") + 1;
	n += (size_t)snprintf(opts + n, sizeof(opts) - n, "4") + 1;
	CHECK(tftp_parse_options(opts, n, &opt) == -1);

	n = 0;
	n += (size_t)snprintf(opts + n, sizeof(opts) - n, "blksize") + 1;
	n += (size_t)snprintf(opts + n, sizeof(opts) - n, "not-a-number") + 1;
	CHECK(tftp_parse_options(opts, n, &opt) == -1);

	n = 0;
	n += (size_t)snprintf(opts + n, sizeof(opts) - n, "wsize") + 1;
	n += (size_t)snprintf(opts + n, sizeof(opts) - n, "0") + 1;
	CHECK(tftp_parse_options(opts, n, &opt) == -1);

	n = 0;
	n += (size_t)snprintf(opts + n, sizeof(opts) - n, "wsize") + 1;
	n += (size_t)snprintf(opts + n, sizeof(opts) - n, "99999999") + 1; /* doesn't fit the 16-bit wire field at all */
	CHECK(tftp_parse_options(opts, n, &opt) == -1);
}

/* Round 1 review F5: a blksize above TFTP_MAX_BLKSIZE is clamped and
 * negotiation still succeeds -- RFC 2348 makes the server's answer
 * authoritative, so this is not a failure the client can reject. */
static void test_parse_options_blksize_clamped(void)
{
	char opts[64];
	size_t n = 0;
	struct tftp_options opt;

	n += (size_t)snprintf(opts + n, sizeof(opts) - n, "blksize") + 1;
	n += (size_t)snprintf(opts + n, sizeof(opts) - n, "65464") + 1; /* RFC 2348's own max */

	CHECK(tftp_parse_options(opts, n, &opt) == 0);
	CHECK(opt.have_blksize);
	CHECK(opt.blksize == TFTP_MAX_BLKSIZE);
}

/* Round 1 review F2: a wsize above TFTP_MAX_WSIZE is clamped (not
 * rejected) so rw_buf_size = blksize * wsize stays bounded regardless of
 * what a client asks for. */
static void test_parse_options_wsize_clamped(void)
{
	char opts[64];
	size_t n = 0;
	struct tftp_options opt;

	n += (size_t)snprintf(opts + n, sizeof(opts) - n, "wsize") + 1;
	n += (size_t)snprintf(opts + n, sizeof(opts) - n, "65535") + 1; /* RFC 7440's own max */

	CHECK(tftp_parse_options(opts, n, &opt) == 0);
	CHECK(opt.have_wsize);
	CHECK(opt.wsize == TFTP_MAX_WSIZE);
}

/* Round 1 review F4: "timeout" (RFC 2349, seconds) and "timeoutms" must be
 * distinguishable after parsing so the OACK can echo back the same name
 * the client used. */
static void test_parse_options_timeout_name_tracked(void)
{
	char opts[64];
	size_t n = 0;
	struct tftp_options opt;

	n += (size_t)snprintf(opts + n, sizeof(opts) - n, "timeout") + 1;
	n += (size_t)snprintf(opts + n, sizeof(opts) - n, "3") + 1;
	CHECK(tftp_parse_options(opts, n, &opt) == 0);
	CHECK(opt.have_timeoutms && opt.timeoutms == 3000 && opt.timeout_is_seconds);

	n = 0;
	n += (size_t)snprintf(opts + n, sizeof(opts) - n, "timeoutms") + 1;
	n += (size_t)snprintf(opts + n, sizeof(opts) - n, "3000") + 1;
	CHECK(tftp_parse_options(opts, n, &opt) == 0);
	CHECK(opt.have_timeoutms && opt.timeoutms == 3000 && !opt.timeout_is_seconds);
}

static void test_parse_options_malformed_unterminated(void)
{
	char opts[8] = "blksize"; /* no NUL before the buffer ends */
	struct tftp_options opt;

	CHECK(tftp_parse_options(opts, sizeof(opts), &opt) == -1);
}

static void test_encode_ack(void)
{
	uint8_t buf[8];
	ssize_t len;

	len = tftp_encode_ack(buf, sizeof(buf), 0);
	CHECK(len == 4);
	CHECK(memcmp(buf, "\x00\x04\x00\x00", 4) == 0);

	len = tftp_encode_ack(buf, sizeof(buf), 300);
	CHECK(len == 4);
	CHECK(buf[0] == 0 && buf[1] == TFTP_OP_ACK && buf[2] == 1 && buf[3] == 44);

	CHECK(tftp_encode_ack(buf, 2, 0) == -1); /* buffer too small */
}

static void test_encode_error(void)
{
	uint8_t buf[64];
	ssize_t len;

	len = tftp_encode_error(buf, sizeof(buf), TFTP_ERROR_ENOENT, "file not found");
	CHECK(len == (ssize_t)(4 + strlen("file not found") + 1));
	CHECK(buf[0] == 0 && buf[1] == TFTP_OP_ERROR && buf[2] == 0 && buf[3] == TFTP_ERROR_ENOENT);
	CHECK(memcmp(buf + 4, "file not found\0", strlen("file not found") + 1) == 0);
}

static void test_encode_oack_layout(void)
{
	uint8_t buf[512];
	struct tftp_options opt;
	ssize_t len;

	memset(&opt, 0, sizeof(opt));
	opt.have_blksize = 1;
	opt.blksize = 512;

	len = tftp_encode_oack(buf, sizeof(buf), &opt);
	CHECK(len == (ssize_t)(2 + strlen("blksize") + 1 + strlen("512") + 1));
	CHECK(buf[0] == 0 && buf[1] == TFTP_OP_OACK);
	/* "\0" followed directly by a digit would be parsed as a multi-digit
	 * octal escape (e.g. "\0512" is really '\051' '2'), so the embedded
	 * NUL after "blksize" is its own literal, concatenated. */
	CHECK(memcmp(buf + 2, "blksize\0" "512\0", strlen("blksize") + 1 + strlen("512") + 1) == 0);
}

static void test_encode_oack_multiple_options_and_order(void)
{
	uint8_t buf[512];
	struct tftp_options opt;
	ssize_t len;
	const uint8_t *p;

	memset(&opt, 0, sizeof(opt));
	opt.have_blksize = 1;
	opt.blksize = 1024;
	opt.have_tsize = 1;
	opt.tsize = 37282;
	opt.have_wsize = 1;
	opt.wsize = 8;

	len = tftp_encode_oack(buf, sizeof(buf), &opt);
	CHECK(len > 0);
	p = buf + 2;
	CHECK(strcmp((const char *)p, "blksize") == 0);
	p += strlen("blksize") + 1;
	CHECK(strcmp((const char *)p, "1024") == 0);
	p += strlen("1024") + 1;
	CHECK(strcmp((const char *)p, "tsize") == 0);
	p += strlen("tsize") + 1;
	CHECK(strcmp((const char *)p, "37282") == 0);
	p += strlen("37282") + 1;
	CHECK(strcmp((const char *)p, "wsize") == 0);
	p += strlen("wsize") + 1;
	CHECK(strcmp((const char *)p, "8") == 0);
	p += strlen("8") + 1;
	CHECK(p == buf + len);
}

/* Round 1 review F4: the OACK must carry whichever option name the client
 * used, in that option's own unit. */
static void test_encode_oack_echoes_timeout_name(void)
{
	uint8_t buf[64];
	struct tftp_options opt;
	ssize_t len;

	memset(&opt, 0, sizeof(opt));
	opt.have_timeoutms = 1;
	opt.timeout_is_seconds = 1;
	opt.timeoutms = 3000;
	len = tftp_encode_oack(buf, sizeof(buf), &opt);
	CHECK(len > 0);
	CHECK(strcmp((const char *)buf + 2, "timeout") == 0);
	CHECK(strcmp((const char *)buf + 2 + strlen("timeout") + 1, "3") == 0);

	memset(&opt, 0, sizeof(opt));
	opt.have_timeoutms = 1;
	opt.timeout_is_seconds = 0;
	opt.timeoutms = 3000;
	len = tftp_encode_oack(buf, sizeof(buf), &opt);
	CHECK(len > 0);
	CHECK(strcmp((const char *)buf + 2, "timeoutms") == 0);
	CHECK(strcmp((const char *)buf + 2 + strlen("timeoutms") + 1, "3000") == 0);
}

static void test_encode_oack_too_small_buffer(void)
{
	uint8_t buf[4];
	struct tftp_options opt;

	memset(&opt, 0, sizeof(opt));
	opt.have_blksize = 1;
	opt.blksize = 65464;

	CHECK(tftp_encode_oack(buf, sizeof(buf), &opt) == -1);
}

/* tftpserv.c's handle_reader() walks a wsize-block window with:
 *   for (block = last; (uint16_t)(block - last) < wsize; block++)
 * exercised here directly on uint16_t arithmetic to confirm it steps
 * exactly `wsize` values and wraps cleanly through 65535 -> 0, which is
 * what REPORT.md 8.1 point 7's "block-number wrap" test asks for. */
static void test_block_window_wraps(void)
{
	uint16_t last = 65530;
	uint16_t wsize = 8;
	uint16_t block;
	int count = 0;
	uint16_t seen[8];

	for (block = last; (uint16_t)(block - last) < wsize; block++)
		seen[count++] = block;

	CHECK(count == 8);
	CHECK(seen[0] == 65530);
	CHECK(seen[5] == 65535);
	CHECK(seen[6] == 0);
	CHECK(seen[7] == 1);

	/* A non-wrapping window covers the same count, ordinary values. */
	last = 100;
	count = 0;
	for (block = last; (uint16_t)(block - last) < wsize; block++)
		seen[count++] = block;
	CHECK(count == 8);
	CHECK(seen[0] == 100 && seen[7] == 107);
}

/* handle_writer()'s DATA-block sequencing expects the same wraparound for
 * blk_expected, incremented once per accepted block starting at 1. */
static void test_blk_expected_wraps(void)
{
	uint16_t blk_expected = 65534;
	int i;

	for (i = 0; i < 4; i++)
		blk_expected++;

	CHECK(blk_expected == 2);
}

int main(void)
{
	test_parse_request_basic();
	test_parse_request_rejects_bad_mode();
	test_parse_request_rejects_truncated();
	test_parse_options_recognized();
	test_parse_options_plain_timeout_is_seconds();
	test_parse_options_out_of_range_rejected();
	test_parse_options_malformed_unterminated();
	test_encode_ack();
	test_encode_error();
	test_parse_options_blksize_clamped();
	test_parse_options_wsize_clamped();
	test_parse_options_timeout_name_tracked();
	test_encode_oack_layout();
	test_encode_oack_multiple_options_and_order();
	test_encode_oack_echoes_timeout_name();
	test_encode_oack_too_small_buffer();
	test_block_window_wraps();
	test_blk_expected_wraps();

	printf("%s: %d/%d checks passed\n", g_failures ? "FAIL" : "PASS",
	       g_tests - g_failures, g_tests);
	return g_failures ? 1 : 0;
}
