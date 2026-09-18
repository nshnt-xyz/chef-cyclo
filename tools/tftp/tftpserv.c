/* tftp-server: bounded TFTP/RFS server for this device's modem, service
 * 0x1000 version 1 (wire instance field 0x00000001, i.e. QMI instance 0 --
 * see tftp_open_and_publish() below for why) on AF_MSM_IPC -- see REPORT.md
 * 8.1 for the full specification this implements and README.md's
 * 2026-09-17 (stock read + TFTP/RFS) log entry for why it exists (the modem's very first
 * boot-time RFS transactions, ~0.12-0.28s after Power/Clock-ready, are TFTP
 * WRQs to /readwrite/mot_rfs/imei_sv and /readwrite/server_check.txt; stock
 * Android serves them with its own tftp_server, and nothing on this project
 * did until now).
 *
 * This is a structural port of upstream linux-msm/tqftpserv (BSD-3-Clause,
 * see LICENSE) onto this vendor kernel's AF_MSM_IPC transport, the same way
 * tools/rmtfs/ ports linux-msm/rmtfs. Deviations from upstream, all
 * deliberate:
 *
 *  - No connect()+send()/recv() on the per-transfer socket. Upstream calls
 *    libc connect() with a `struct sockaddr_qrtr` and then plain send()/
 *    recv() on that fd, relying on real QRTR's AF_QIPCRTR socket family to
 *    understand that address. On this kernel the underlying socket is real
 *    AF_MSM_IPC, which only understands `struct sockaddr_msm_ipc`; only
 *    tools/msmipc.c knows how to build and parse that (msmipc.h defines no
 *    connect()-equivalent, and per this task's scope msmipc.c/.h -- owned
 *    by a separate work session -- are not touched here). Every send/recv
 *    in this file therefore goes through qrtr_sendto()/qrtr_recvfrom() with
 *    an explicit (node, port), and every read re-checks the sender against
 *    the expected peer (upstream does this check too, just redundantly
 *    with connect()'s implicit filtering -- here it's the *only* filter).
 *  - No QRTR_PORT_CTRL / BYE / DEL_CLIENT handling in the main loop.
 *    msmipc.c's qrtr_decode() always returns QRTR_TYPE_DATA (see its file
 *    header: "AF_MSM_IPC has no synthetic control port distinct from data
 *    traffic"), so that branch in upstream's main() is dead code on this
 *    transport and is omitted rather than ported unreachable.
 *  - A 0-byte qrtr_recvfrom() (QRTR_RECV_RESUME_TX, AF_MSM_IPC's
 *    flow-control signal -- msmipc.h) is treated as "ignore, keep waiting"
 *    everywhere a recv happens, the same way tools/servreg-locator/ already
 *    does; upstream has no equivalent case because real QRTR surfaces flow
 *    control as a distinct decoded packet type, not a 0-byte datagram.
 *  - translate.c is a from-scratch allowlist (RAM-backed, closed prefix
 *    set), not a port of upstream's remoteproc-scanning/real-directory
 *    translate.c -- see translate.c's file header.
 *  - Fixed-size client table (TFTP_MAX_CLIENTS slots) instead of upstream's
 *    intrusive linked list (tools/list.h is not vendored here), matching
 *    this tree's existing tools/rmtfs/storage.c MAX_CALLERS array idiom.
 *    Enforces REPORT.md 8.1 point 5's "max concurrent transfers" directly
 *    as the table size, logging stock's own "Max TFTP client limit
 *    reached. Dropping pkt" string (pulled/tftp_server.strings.txt) when
 *    full.
 *  - Per-transfer idle timeout is enforced here; upstream parses and
 *    echoes back "timeoutms" (it's the *client's* retransmit timer) but
 *    never times out a stalled transfer itself, so a peer that stops
 *    sending would otherwise leak an ephemeral socket and a client slot
 *    forever. main()'s select() therefore always uses a bounded timeout
 *    (see TICK_MS) and sweeps expired clients every tick.
 *  - A plain RFC 2349 "timeout" option (whole seconds) is accepted in
 *    addition to upstream's "timeoutms" -- REPORT.md 8.1 point 2 lists
 *    both as names the stock server itself understands
 *    (pulled/tftp_server.strings.txt has both strings).
 *  - "append" is parsed and actually applied (RAM shadow is truncated to
 *    empty for a fresh WRQ, left intact with writes starting at the
 *    current end for an "append" WRQ -- see ramfs_truncate()'s comment).
 *    Upstream's parse_options() doesn't even recognize this option name;
 *    REPORT.md 8.1 point 2 requires it (stock's Qualcomm semantics:
 *    "append = open O_APPEND").
 *  - The RFC-2349-style no-options WRQ path is fixed: upstream's
 *    handle_wrq(), when the client sends no options at all, calls
 *    tftp_send_data() (which pread()s from a write-only fd and always
 *    fails) instead of ACKing block 0 to tell the client to start sending.
 *    That looks like a copy-paste of handle_rrq()'s tail rather than
 *    intended behavior -- ported here as tftp_send_ack(sock, ..., 0), the
 *    correct RFC 1350 response that lets an un-negotiated WRQ actually
 *    proceed.
 *  - blksize is capped at 8192, not RFC 2348's 65464, per REPORT.md 8.1
 *    point 5.
 *  - One structured log line per request (opcode, path, options) at open
 *    and one at close (result, bytes), per REPORT.md 8.1 point 5; -v adds
 *    a line per packet.
 *
 * Round 1 review (stock-read-fable, /tmp/tftp-review-round1.md) found ten
 * further issues, each fixed at its own call site (search this file and
 * translate.c/protocol.c for "Round 1 review F<N>"): F1 the published
 * instance was wrong (wire field 0x101 instead of stock's 0x001); F2/F5
 * blksize/wsize weren't bounded against a client-controlled allocation;
 * F3 rsize/seek termination was wrong for an exact-multiple rsize and for
 * rsize exceeding what's left in the file; F4 the OACK didn't echo back
 * the RFC 2349 "timeout" option under its own name; F6 an RRQ client
 * closed before the client's closing ACK could arrive; F7 the idle
 * timeout equaled the client's own retransmit timer; F8 the RO firmware
 * path allowed nested paths and non-regular files; F9 added
 * tests/test_e2e.c, an in-memory loopback harness (tests/fake_qrtr.c)
 * covering the full RRQ/WRQ/OACK/DATA/ACK state machine, previously
 * untested since AF_MSM_IPC doesn't exist off the real device; F10 two
 * error paths were under-logged.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "protocol.h"
/* Only the qrtr_* prototypes are needed (msmipc.c is this file's only
 * real implementation of them); pulling in msmipc.h itself would drag in
 * the vendor kernel's <linux/msm_ipc.h> for no reason this file needs,
 * and Round 1 review F9's e2e test build links tests/fake_qrtr.c's
 * implementation instead of msmipc.c, so it must not need those headers
 * either. */
#include "qrtr/libqrtr.h"
#include "tftp.h"
#include "translate.h"

#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* select() wakes at least this often so idle-client sweeping (see file
 * header) has bounded latency regardless of traffic. */
#define TICK_MS 500

struct tftp_client {
	int in_use;
	int is_writer;

	uint32_t node;
	uint32_t port;
	int sock;

	struct tftp_file file;

	size_t blksize;
	size_t rsize;
	size_t wsize;
	unsigned int timeoutms;		/* client-facing value, echoed in the OACK */
	unsigned int idle_timeout_ms;	/* server-side sweep threshold, see sweep_timeouts() */
	off_t seek;

	/* RRQ only: set once the final (possibly empty) DATA block has been
	 * sent, so the client's closing ACK still has somewhere to land
	 * (Round 1 review F6) instead of hitting an already-closed socket. */
	int finishing;
	uint16_t final_block;

	/* WRQ only */
	off_t write_pos;
	uint16_t blk_expected;
	size_t blk_offset;

	uint8_t *blk_buf;	/* blksize + 4 */
	uint8_t *rw_buf;	/* blksize * wsize, WRQ window accumulator */
	size_t rw_buf_size;

	size_t total_bytes;
	char path[TFTP_MAX_PATH + 1];
	long long last_activity_ms;
};

static struct tftp_client clients[TFTP_MAX_CLIENTS];
static int g_verbose;

#ifdef TFTP_TEST_HOOKS
/* tests/test_e2e.c sets this directly to simulate time passing (idle
 * timeout sweeps) without a real clock or real waiting. */
long long tftp_test_clock_ms;
#endif

static long long now_ms(void)
{
#ifdef TFTP_TEST_HOOKS
	return tftp_test_clock_ms;
#else
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* Round 1 review F7: the server-side idle-sweep threshold must not equal
 * the client's own retransmit timer (timeoutms) -- this server never
 * retransmits, so one slow reply from the modem would otherwise kill the
 * transfer on the same schedule the *client* is still allowed to wait on.
 * Give ourselves a multiple of headroom over it, with a floor for a
 * client that negotiated an unrealistically short timeout. */
static unsigned int idle_timeout_for(unsigned int timeoutms)
{
	unsigned int tripled = timeoutms > UINT_MAX / 3 ? UINT_MAX : timeoutms * 3;

	return tripled > 5000 ? tripled : 5000;
}

static void log_line(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

static void log_verbose(const char *fmt, ...)
{
	va_list ap;

	if (!g_verbose)
		return;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

static ssize_t file_pread(struct tftp_file *f, void *buf, size_t nbyte, off_t offset)
{
	if (f->kind == TFTP_FILE_RO)
		return pread(f->fd, buf, nbyte, offset);
	return ramfs_pread(f->rf, buf, nbyte, offset);
}

static ssize_t file_pwrite(struct tftp_file *f, const void *buf, size_t nbyte, off_t offset)
{
	/* translate_open() never returns a TFTP_FILE_RO for a WRQ (RO
	 * firmware paths are RRQ-only, enforced in translate.c), so this is
	 * unreachable for anything but TFTP_FILE_RAM. */
	return ramfs_pwrite(f->rf, buf, nbyte, offset);
}

static struct tftp_client *client_alloc(void)
{
	int i;
	int active = 0;

	for (i = 0; i < TFTP_MAX_CLIENTS; i++)
		active += clients[i].in_use;

	if (active >= TFTP_MAX_CLIENTS)
		return NULL;

	for (i = 0; i < TFTP_MAX_CLIENTS; i++) {
		if (!clients[i].in_use)
			return &clients[i];
	}
	return NULL;
}

static void client_close_and_free(struct tftp_client *c, const char *result)
{
	log_line("[TFTP] %s %s node=%u port=%u result=%s bytes=%zu",
		 c->is_writer ? "WRQ" : "RRQ", c->path, c->node, c->port,
		 result, c->total_bytes);

	translate_close(&c->file);
	if (c->sock >= 0)
		qrtr_close(c->sock);
	free(c->blk_buf);
	free(c->rw_buf);
	memset(c, 0, sizeof(*c));
	c->sock = -1;
}

/* Thin socket-I/O wrappers around protocol.c's pure encoders -- see
 * protocol.h's file header for why the encoding itself lives there. */

static int tftp_send_error(int sock, uint32_t node, uint32_t port,
			    enum tftp_error code, const char *msg)
{
	uint8_t buf[4 + 128];
	ssize_t len = tftp_encode_error(buf, sizeof(buf), code, msg);

	if (len < 0)
		return -1;
	return qrtr_sendto(sock, node, port, buf, (unsigned int)len);
}

static void tftp_send_error_to(uint32_t node, uint32_t port,
				enum tftp_error code, const char *msg)
{
	int sock;

	sock = qrtr_open(0);
	if (sock < 0)
		return;

	tftp_send_error(sock, node, port, code, msg);
	qrtr_close(sock);
}

static int tftp_send_ack(int sock, uint32_t node, uint32_t port, uint16_t block)
{
	uint8_t buf[4];
	ssize_t len = tftp_encode_ack(buf, sizeof(buf), block);
	int ret;

	if (len < 0)
		return -1;

	ret = qrtr_sendto(sock, node, port, buf, (unsigned int)len);
	if (ret < 0)
		log_line("[TFTP] failed to send ACK block %u to node %u: %s",
			 block, node, strerror(-ret));
	return ret;
}

static int tftp_send_oack(int sock, uint32_t node, uint32_t port,
			   const struct tftp_options *opt)
{
	uint8_t buf[512];
	ssize_t len = tftp_encode_oack(buf, sizeof(buf), opt);
	int ret;

	if (len < 0)
		return -1;

	ret = qrtr_sendto(sock, node, port, buf, (unsigned int)len);
	if (ret < 0)
		log_line("[TFTP] failed to send OACK to node %u: %s", node, strerror(-ret));
	return ret;
}

/* Wraps tftp_parse_request(): on failure, sends the error it reports back
 * to the peer (protocol.c itself does no I/O -- see protocol.h) and logs
 * it, then returns -1. */
static int parse_request(const char *buf, size_t len, uint32_t node, uint32_t port,
			  const char **filename, size_t *opts_off)
{
	enum tftp_error err;
	const char *err_msg;

	if (tftp_parse_request(buf, len, filename, opts_off, &err, &err_msg) < 0) {
		log_line("[TFTP] request from node %u rejected: %s", node, err_msg);
		tftp_send_error_to(node, port, err, err_msg);
		return -1;
	}
	return 0;
}

static off_t file_size(struct tftp_file *f)
{
	if (f->kind == TFTP_FILE_RO) {
		off_t cur, end;

		cur = lseek(f->fd, 0, SEEK_CUR);
		end = lseek(f->fd, 0, SEEK_END);
		lseek(f->fd, cur, SEEK_SET);
		return end;
	}
	return (off_t)f->rf->len;
}

/* Sends one DATA packet for `block`, reading up to c->blksize bytes at
 * `offset` (capped to `response_size` payload bytes if nonzero -- the
 * "rsize" partial-read option). Returns the number of payload bytes
 * actually sent (0 is a valid, meaningful result: an exact-multiple-of-
 * blksize file's final block, or a read starting at/past EOF) on success,
 * -1 on error (already reported to the peer). Callers use "returned <
 * c->blksize" as the RFC 1350 end-of-transfer signal -- unlike upstream
 * tqftpserv, which checks its send()'s return value (the number of bytes
 * the transport layer accepted, always >= 4 for a nonempty header) against
 * 0 for this same purpose; that comparison can never be true, so upstream
 * effectively has no window-aware EOF check at all (see this file's
 * header). */
static ssize_t tftp_send_data(struct tftp_client *c, uint16_t block, off_t offset,
			       size_t response_size)
{
	uint8_t *p = c->blk_buf;
	ssize_t len;
	size_t payload_len;
	int ret;

	p[0] = 0;
	p[1] = TFTP_OP_DATA;
	p[2] = (uint8_t)(block >> 8);
	p[3] = (uint8_t)block;

	len = file_pread(&c->file, p + 4, c->blksize, offset);
	if (len < 0) {
		tftp_send_error(c->sock, c->node, c->port, TFTP_ERROR_UNDEF, "read error");
		return -1;
	}

	if (response_size != 0 && response_size < (size_t)len) {
		/* rsize asked for less than a full block here: truncate to
		 * exactly the requested amount. */
		payload_len = response_size;
	} else {
		/* Either no rsize cap on this block, or rsize asked for more
		 * than the file actually has left from `offset` (Round 1
		 * review F3b: previously an error -- "beyond EOF" is not a
		 * fault, the short/empty read here already *is* the correct
		 * end-of-transfer signal, exactly like a real EOF with no
		 * rsize involved at all). */
		payload_len = (size_t)len;
	}

	log_verbose("[TFTP] DATA block=%u len=%zu -> node %u", block, payload_len, c->node);

	ret = qrtr_sendto(c->sock, c->node, c->port, p, (unsigned int)(4 + payload_len));
	if (ret < 0) {
		log_line("[TFTP] %s failed to send DATA block %u: %s",
			 c->path, block, strerror(-ret));
		return -1;
	}

	c->total_bytes += payload_len;
	return (ssize_t)payload_len;
}

/* Sends a 0-byte DATA `block` without touching the file at all. Used only
 * for the Round 1 review F3a case: an "rsize" partial read whose requested
 * length is an exact multiple of blksize, where the last real block sent
 * was therefore full-size and can't itself serve as the RFC 1350
 * end-of-transfer signal. Returns 0 on success, -1 on error (already
 * reported to the peer). */
static int tftp_send_final_empty(struct tftp_client *c, uint16_t block)
{
	uint8_t hdr[4];
	int ret;

	hdr[0] = 0;
	hdr[1] = TFTP_OP_DATA;
	hdr[2] = (uint8_t)(block >> 8);
	hdr[3] = (uint8_t)block;

	ret = qrtr_sendto(c->sock, c->node, c->port, hdr, sizeof(hdr));
	if (ret < 0) {
		log_line("[TFTP] %s failed to send final empty DATA block %u: %s",
			 c->path, block, strerror(-ret));
		return -1;
	}
	log_verbose("[TFTP] DATA block=%u len=0 (rsize exact-multiple terminator) -> node %u",
		    block, c->node);
	return 0;
}

static void handle_rrq(const char *buf, size_t len, uint32_t node, uint32_t port)
{
	struct tftp_client *c;
	struct tftp_options opt;
	struct tftp_file file;
	const char *filename;
	size_t opts_off;
	off_t ram_start;
	int tftp_err;
	const char *err_msg;
	int do_oack = 0;
	int sock;

	if (parse_request(buf, len, node, port, &filename, &opts_off) < 0)
		return;

	if (opts_off < len) {
		do_oack = 1;
		if (tftp_parse_options(buf + opts_off, len - opts_off, &opt) < 0) {
			tftp_send_error_to(node, port, TFTP_ERROR_EOPTNEG,
					    "option negotiation failed");
			return;
		}
	} else {
		memset(&opt, 0, sizeof(opt));
	}

	log_line("[TFTP] RRQ %s opts=%s%s%s from node %u", filename,
		 opt.have_blksize ? "blksize " : "", opt.have_tsize ? "tsize " : "",
		 opt.have_rsize ? "rsize " : "", node);

	if (translate_open(filename, TFTP_OPEN_RRQ, 0, &file, &ram_start,
			    &tftp_err, &err_msg) < 0) {
		log_line("[TFTP] RRQ %s rejected: %s", filename, err_msg);
		tftp_send_error_to(node, port, (enum tftp_error)tftp_err, err_msg);
		return;
	}

	c = client_alloc();
	if (!c) {
		log_line("Max TFTP client limit reached. Dropping pkt");
		translate_close(&file);
		tftp_send_error_to(node, port, TFTP_ERROR_UNDEF,
				    "Max TFTP client limit reached");
		return;
	}

	sock = qrtr_open(0);
	if (sock < 0) {
		translate_close(&file);
		tftp_send_error_to(node, port, TFTP_ERROR_UNDEF, "resources unavailable");
		return;
	}

	memset(c, 0, sizeof(*c));
	c->in_use = 1;
	c->is_writer = 0;
	c->node = node;
	c->port = port;
	c->sock = sock;
	c->file = file;
	c->blksize = opt.have_blksize ? opt.blksize : TFTP_DEFAULT_BLKSIZE;
	c->rsize = opt.have_rsize ? opt.rsize : 0;
	c->wsize = opt.have_wsize ? opt.wsize : 1;
	c->timeoutms = opt.have_timeoutms ? opt.timeoutms : TFTP_DEFAULT_TIMEOUTMS;
	c->idle_timeout_ms = idle_timeout_for(c->timeoutms);
	c->seek = opt.have_seek ? opt.seek : 0;
	snprintf(c->path, sizeof(c->path), "%s", filename);
	c->last_activity_ms = now_ms();

	c->blk_buf = calloc(1, c->blksize + 4);
	if (!c->blk_buf) {
		client_close_and_free(c, "enomem");
		return;
	}

	/* RFC 2349: a "tsize" in an RRQ is the client asking for the file's
	 * size, conventionally sent as "0" -- reply with the real size
	 * (whatever the client sent is otherwise ignored for an RRQ). */
	if (opt.have_tsize)
		opt.tsize = file_size(&c->file);

	if (do_oack) {
		tftp_send_oack(c->sock, node, port, &opt);
	} else {
		tftp_send_data(c, 1, c->seek, 0);
	}
}

static void handle_wrq(const char *buf, size_t len, uint32_t node, uint32_t port)
{
	struct tftp_client *c;
	struct tftp_options opt;
	struct tftp_file file;
	const char *filename;
	size_t opts_off;
	off_t ram_start;
	int tftp_err;
	const char *err_msg;
	int do_oack = 0;
	int sock;

	if (parse_request(buf, len, node, port, &filename, &opts_off) < 0)
		return;

	if (opts_off < len) {
		do_oack = 1;
		if (tftp_parse_options(buf + opts_off, len - opts_off, &opt) < 0) {
			tftp_send_error_to(node, port, TFTP_ERROR_EOPTNEG,
					    "option negotiation failed");
			return;
		}
	} else {
		memset(&opt, 0, sizeof(opt));
	}

	log_line("[TFTP] WRQ %s opts=%s%s from node %u", filename,
		 opt.have_append ? "append " : "", opt.have_blksize ? "blksize " : "", node);

	if (translate_open(filename, TFTP_OPEN_WRQ, opt.have_append, &file, &ram_start,
			    &tftp_err, &err_msg) < 0) {
		log_line("[TFTP] WRQ %s rejected: %s", filename, err_msg);
		tftp_send_error_to(node, port, (enum tftp_error)tftp_err, err_msg);
		return;
	}

	c = client_alloc();
	if (!c) {
		log_line("Max TFTP client limit reached. Dropping pkt");
		translate_close(&file);
		tftp_send_error_to(node, port, TFTP_ERROR_UNDEF,
				    "Max TFTP client limit reached");
		return;
	}

	sock = qrtr_open(0);
	if (sock < 0) {
		translate_close(&file);
		tftp_send_error_to(node, port, TFTP_ERROR_UNDEF, "resources unavailable");
		return;
	}

	memset(c, 0, sizeof(*c));
	c->in_use = 1;
	c->is_writer = 1;
	c->node = node;
	c->port = port;
	c->sock = sock;
	c->file = file;
	c->blksize = opt.have_blksize ? opt.blksize : TFTP_DEFAULT_BLKSIZE;
	c->rsize = opt.have_rsize ? opt.rsize : 0;
	c->wsize = opt.have_wsize ? opt.wsize : 1;
	c->timeoutms = opt.have_timeoutms ? opt.timeoutms : TFTP_DEFAULT_TIMEOUTMS;
	c->idle_timeout_ms = idle_timeout_for(c->timeoutms);
	c->seek = opt.have_seek ? opt.seek : 0;
	c->write_pos = ram_start + c->seek;
	c->blk_expected = 1;
	c->rw_buf_size = c->blksize * c->wsize;
	snprintf(c->path, sizeof(c->path), "%s", filename);
	c->last_activity_ms = now_ms();

	c->blk_buf = calloc(1, c->blksize + 4);
	c->rw_buf = calloc(1, c->rw_buf_size);
	if (!c->blk_buf || !c->rw_buf) {
		client_close_and_free(c, "enomem");
		return;
	}

	if (do_oack)
		tftp_send_oack(c->sock, node, port, &opt);
	else
		tftp_send_ack(c->sock, node, port, 0);
}

/* Returns 1 (keep going), 0 (transfer complete, close normally), -1 (error,
 * close). */
static int handle_reader(struct tftp_client *c)
{
	uint8_t buf[128];
	uint32_t rnode, rport;
	ssize_t len;
	int opcode;
	uint16_t last, block;

	len = qrtr_recvfrom(c->sock, buf, sizeof(buf), &rnode, &rport);
	if (len == QRTR_RECV_RESUME_TX || len == -EAGAIN || len == -EWOULDBLOCK ||
	    len == -EINTR)
		return 1;
	if (len < 0) {
		log_line("[TFTP] %s recvfrom failed: %s", c->path, strerror((int)-len));
		return -1;
	}
	if (rnode != c->node || rport != c->port) {
		log_verbose("[TFTP] %s discarding packet from unexpected %u:%u",
			    c->path, rnode, rport);
		return 1;
	}
	if (len < 4) {
		log_line("[TFTP] %s short packet (%zd bytes)", c->path, len);
		return -1;
	}

	c->last_activity_ms = now_ms();

	opcode = (buf[0] << 8) | buf[1];
	if (opcode == TFTP_OP_ERROR) {
		log_line("[TFTP] %s: peer sent ERROR %d", c->path, (buf[2] << 8) | buf[3]);
		return -1;
	}
	if (opcode != TFTP_OP_ACK) {
		log_line("[TFTP] %s: expected ACK, got opcode %d", c->path, opcode);
		tftp_send_error(c->sock, c->node, c->port, TFTP_ERROR_EBADOP, "expected ACK");
		return -1;
	}

	last = (uint16_t)((buf[2] << 8) | buf[3]);
	log_verbose("[TFTP] %s ACK block=%u", c->path, last);

	/* Round 1 review F6: the final DATA block was already sent (either a
	 * natural short/empty block, or F3a's manufactured empty terminator
	 * below) and we're only still open to give this ACK somewhere to
	 * land. Round 2 review F12: only the ACK that actually matches
	 * final_block confirms that -- a duplicate/late ACK of an earlier
	 * block (e.g. a retransmit the client sent before our final block
	 * arrived) must not end the transfer as "ok" ahead of the real
	 * closing ACK. Anything else here is ignored (return 1, keep
	 * waiting for the idle timeout or the correct ACK) rather than
	 * re-entering the send logic below, which would otherwise treat it
	 * as a request for more data past what we already declared EOF. */
	if (c->finishing) {
		if (last == c->final_block)
			return 0;
		log_verbose("[TFTP] %s: ignoring ACK block=%u while finishing (want %u)",
			    c->path, last, c->final_block);
		return 1;
	}

	if (c->rsize != 0 && (size_t)last * c->blksize >= c->rsize) {
		/* Round 1 review F3a: every block sent so far was exactly
		 * blksize (rsize is a clean multiple of it, otherwise the
		 * short-block branch below would already have set
		 * c->finishing), so this ACK is for a full-size block that
		 * cannot itself serve as the RFC 1350 EOF signal. Send one
		 * more, explicitly empty, DATA block -- not read from the
		 * file at all -- and wait for its ACK instead of closing
		 * immediately (which would otherwise strand the client's
		 * next ACK at an already-closed port, per F6). */
		if (tftp_send_final_empty(c, (uint16_t)(last + 1)) < 0)
			return -1;
		c->finishing = 1;
		c->final_block = (uint16_t)(last + 1);
		return 1;
	}

	for (block = last; (uint16_t)(block - last) < c->wsize; block++) {
		off_t offset = c->seek + (off_t)block * (off_t)c->blksize;
		size_t response_size = 0;
		ssize_t sent;

		if (c->rsize != 0) {
			size_t already = (size_t)block * c->blksize;

			if (already >= c->rsize)
				break;
			if (c->rsize - already < c->blksize)
				response_size = c->rsize - already;
		}

		sent = tftp_send_data(c, (uint16_t)(block + 1), offset, response_size);
		if (sent < 0)
			return -1;
		if ((size_t)sent < c->blksize) {
			/* Short (or empty) block: natural EOF, or F3b's
			 * "rsize asked for more than the file has left"
			 * case. Wait for the client's closing ACK (F6)
			 * instead of closing this socket right now. */
			c->finishing = 1;
			c->final_block = (uint16_t)(block + 1);
			return 1;
		}
		if (c->rsize != 0 && (size_t)(block + 1) * c->blksize >= c->rsize)
			break;
	}

	return 1;
}

static int handle_writer(struct tftp_client *c)
{
	uint8_t *buf = c->blk_buf;
	uint32_t rnode, rport;
	ssize_t len;
	int opcode;
	uint16_t block;
	size_t payload;
	ssize_t wrote;

	len = qrtr_recvfrom(c->sock, buf, c->blksize + 4, &rnode, &rport);
	if (len == QRTR_RECV_RESUME_TX || len == -EAGAIN || len == -EWOULDBLOCK ||
	    len == -EINTR)
		return 1;
	if (len < 0) {
		log_line("[TFTP] %s recvfrom failed: %s", c->path, strerror((int)-len));
		return -1;
	}
	if (rnode != c->node || rport != c->port) {
		log_verbose("[TFTP] %s discarding packet from unexpected %u:%u",
			    c->path, rnode, rport);
		return 1;
	}
	if (len < 4) {
		log_line("[TFTP] %s short packet (%zd bytes)", c->path, len);
		return -1;
	}

	c->last_activity_ms = now_ms();

	opcode = (buf[0] << 8) | buf[1];
	block = (uint16_t)((buf[2] << 8) | buf[3]);
	if (opcode != TFTP_OP_DATA) {
		log_line("[TFTP] %s: expected DATA, got opcode %d", c->path, opcode);
		tftp_send_error(c->sock, c->node, c->port, TFTP_ERROR_EBADOP, "expected DATA");
		return -1;
	}

	payload = (size_t)len - 4;
	log_verbose("[TFTP] %s DATA block=%u len=%zu", c->path, block, payload);

	if (block != c->blk_expected) {
		/* Upstream tqftpserv computes a "rewind to the start of the
		 * current window" value here before returning -1, but its
		 * caller tears the whole transfer down on any negative
		 * return (client_close_and_free()), so that value is never
		 * used -- the window can't actually be retried within a
		 * still-open transfer. Simplified to match what actually
		 * happens: report and abort. */
		log_line("[TFTP] %s: block %u out of sequence (expected %u)",
			 c->path, block, c->blk_expected);
		tftp_send_error(c->sock, c->node, c->port, TFTP_ERROR_EBADOP,
				 "block number out of sequence");
		return -1;
	}

	c->blk_expected++;

	if (c->blk_offset + payload > c->rw_buf_size) {
		log_line("[TFTP] %s: window overflow, dropping transfer", c->path);
		tftp_send_error(c->sock, c->node, c->port, TFTP_ERROR_UNDEF,
				 "window overflow");
		return -1;
	}
	memcpy(c->rw_buf + c->blk_offset, buf + 4, payload);
	c->blk_offset += payload;

	if ((block % c->wsize) == 0 || payload < c->blksize) {
		wrote = file_pwrite(&c->file, c->rw_buf, c->blk_offset, c->write_pos);
		if (wrote < 0 || (size_t)wrote != c->blk_offset) {
			tftp_send_error(c->sock, c->node, c->port, TFTP_ERROR_ENOSPACE,
					 "disk full or write error");
			return -1;
		}
		c->total_bytes += c->blk_offset;
		c->write_pos += (off_t)c->blk_offset;
		c->blk_offset = 0;
		tftp_send_ack(c->sock, c->node, c->port, block);
	}

	return payload == c->blksize ? 1 : 0;
}

static void sweep_timeouts(void)
{
	long long now = now_ms();
	int i;

	for (i = 0; i < TFTP_MAX_CLIENTS; i++) {
		struct tftp_client *c = &clients[i];

		if (!c->in_use)
			continue;
		if (now - c->last_activity_ms > (long long)c->idle_timeout_ms) {
			log_line("[TFTP] %s: idle timeout after %lldms", c->path,
				 now - c->last_activity_ms);
			client_close_and_free(c, "timeout");
		}
	}
}

#ifndef TFTP_TEST_HOOKS
static volatile sig_atomic_t g_stop;

static void on_sigterm(int sig)
{
	(void)sig;
	g_stop = 1;
}
#endif

/* msmipc.c's qrtr_publish(sock, service, version, instance) encodes the
 * wire instance field as (instance << 8) | version -- see its
 * implementation and servreg-locator's own instance 0x101 == (1<<8)|1.
 * Stock's tftp_server registers service 0x1000 with wire field
 * 0x00000001 (REPORT.md 5's dump_servers dump: "0x00001000 |0x00000001|"),
 * i.e. QMI version 1, *instance 0* -- upstream tqftpserv's own
 * qrtr_publish(fd, 4096, 1, 0) call agrees. Round 1 review F1: this file
 * previously called qrtr_publish(ctrl, 0x1000, 1, 1), which actually
 * registers wire field 0x101 and would never match the modem's lookup for
 * field 1 (gps-up's wait_for_server only checks the service column, so it
 * would not have caught this). "Instance 1" in REPORT.md 8.1 refers to
 * the raw wire field value, not this function's `instance` parameter. */
#define TFTP_QMI_SERVICE	0x1000
#define TFTP_QMI_VERSION	1
#define TFTP_QMI_INSTANCE	0

#ifdef TFTP_TEST_HOOKS
int
#else
static int
#endif
tftp_open_and_publish(void)
{
	int ctrl;

	ctrl = qrtr_open(0);
	if (ctrl < 0) {
		log_line("[TFTP] failed to open control socket: %s", strerror(errno));
		return -1;
	}

	if (qrtr_publish(ctrl, TFTP_QMI_SERVICE, TFTP_QMI_VERSION, TFTP_QMI_INSTANCE) < 0) {
		log_line("[TFTP] failed to publish service 0x1000: %s", strerror(errno));
		qrtr_close(ctrl);
		return -1;
	}
	log_line("[TFTP] published service 0x1000 version %d instance %d (wire field 0x%08x)",
		 TFTP_QMI_VERSION, TFTP_QMI_INSTANCE,
		 (TFTP_QMI_INSTANCE << 8) | TFTP_QMI_VERSION);
	return ctrl;
}

/* Reads and dispatches exactly one packet from the control socket, or does
 * nothing if none is pending (EAGAIN/RESUME_TX). Split out of run_server()
 * so tests/test_e2e.c (Round 1 review F9) can inject a control-socket
 * packet through the real RRQ/WRQ dispatch path via tests/fake_qrtr.c
 * without a real select() -- exposed non-static under TFTP_TEST_HOOKS for
 * exactly that; production code (run_server() below) only ever calls it
 * after select() says data is actually there, since a real AF_MSM_IPC
 * socket is blocking and calling this speculatively in production could
 * stall the whole server. */
#ifdef TFTP_TEST_HOOKS
void
#else
static void
#endif
service_control(int ctrl)
{
	uint8_t buf[4096];
	uint32_t node, port;
	ssize_t len;
	int opcode;

	len = qrtr_recvfrom(ctrl, buf, sizeof(buf), &node, &port);
	if (len == QRTR_RECV_RESUME_TX || len == -EAGAIN || len == -EWOULDBLOCK ||
	    len == -EINTR) {
		return;
	}
	if (len < 0) {
		log_line("[TFTP] control recvfrom failed: %s", strerror((int)-len));
		return;
	}
	if (len < 2)
		return;

	opcode = (buf[0] << 8) | buf[1];
	switch (opcode) {
	case TFTP_OP_RRQ:
		handle_rrq((char *)buf, (size_t)len, node, port);
		break;
	case TFTP_OP_WRQ:
		handle_wrq((char *)buf, (size_t)len, node, port);
		break;
	case TFTP_OP_ERROR:
		log_line("[TFTP] control: ERROR from node %u", node);
		break;
	default:
		log_line("[TFTP] control: unhandled opcode %d from node %u", opcode, node);
		break;
	}
}

static void service_client(struct tftp_client *c)
{
	int r = c->is_writer ? handle_writer(c) : handle_reader(c);

	if (r <= 0)
		client_close_and_free(c, r == 0 ? "ok" : "error");
}

#ifdef TFTP_TEST_HOOKS
/* Services every currently-active client socket once (as production code
 * would after select() marks all of them ready simultaneously) and then
 * sweeps idle timeouts (Round 1 review F9). Test-only: safe to call
 * unconditionally only because handle_reader()/handle_writer() already
 * tolerate "nothing pending" (EAGAIN/RESUME_TX), which is true for the
 * fake transport's non-blocking qrtr_recvfrom() but would not be safe
 * against a real blocking socket with nothing ready -- production's
 * run_server() below still gates each service_client() call on select(),
 * which is why this isn't just reused there too. */
void tftp_service_clients_once(void)
{
	int i;

	for (i = 0; i < TFTP_MAX_CLIENTS; i++) {
		if (clients[i].in_use)
			service_client(&clients[i]);
	}
	sweep_timeouts();
}
#endif

#ifndef TFTP_TEST_HOOKS
static int run_server(void)
{
	int ctrl;
	int ret;

	ctrl = tftp_open_and_publish();
	if (ctrl < 0)
		return -1;

	while (!g_stop) {
		fd_set rfds;
		int nfds = ctrl;
		struct timeval tv;
		int i;

		FD_ZERO(&rfds);
		FD_SET(ctrl, &rfds);

		for (i = 0; i < TFTP_MAX_CLIENTS; i++) {
			if (clients[i].in_use) {
				FD_SET(clients[i].sock, &rfds);
				nfds = MAX(nfds, clients[i].sock);
			}
		}

		tv.tv_sec = TICK_MS / 1000;
		tv.tv_usec = (TICK_MS % 1000) * 1000;

		ret = select(nfds + 1, &rfds, NULL, NULL, &tv);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			log_line("[TFTP] select failed: %s", strerror(errno));
			break;
		}

		for (i = 0; i < TFTP_MAX_CLIENTS; i++) {
			struct tftp_client *c = &clients[i];

			if (c->in_use && FD_ISSET(c->sock, &rfds))
				service_client(c);
		}

		sweep_timeouts();

		if (FD_ISSET(ctrl, &rfds))
			service_control(ctrl);
	}

	for (int i = 0; i < TFTP_MAX_CLIENTS; i++) {
		if (clients[i].in_use)
			client_close_and_free(&clients[i], "shutdown");
	}
	qrtr_close(ctrl);
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-v] [-f /firmware/image] [-s /usr/share/rfs/msm/mpss]\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *firmware_dir = "/firmware/image";
	const char *seed_dir = "/usr/share/rfs/msm/mpss";
	struct sigaction action;
	int opt;
	int i;

	while ((opt = getopt(argc, argv, "vf:s:h")) != -1) {
		switch (opt) {
		case 'v':
			g_verbose = 1;
			break;
		case 'f':
			firmware_dir = optarg;
			break;
		case 's':
			seed_dir = optarg;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 2;
		}
	}

	for (i = 0; i < TFTP_MAX_CLIENTS; i++)
		clients[i].sock = -1;

	ramfs_init();

	if (translate_init(firmware_dir) < 0) {
		log_line("[TFTP] fatal: cannot open firmware dir %s: %s",
			 firmware_dir, strerror(errno));
		return 1;
	}

	if (translate_seed(seed_dir) < 0) {
		log_line("[TFTP] fatal: failed to seed RAM shadow from %s (fail-closed, "
			 "REPORT.md 8.1 point 3)", seed_dir);
		return 1;
	}
	log_line("[TFTP] RAM shadow seeded from %s (%zu bytes)", seed_dir, ramfs_total_size());

	memset(&action, 0, sizeof(action));
	action.sa_handler = on_sigterm;
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);

	return run_server() < 0 ? 1 : 0;
}
#endif /* !TFTP_TEST_HOOKS */

#ifdef TFTP_TEST_HOOKS
/* tests/test_e2e.c's support functions: reset all daemon state between
 * test cases (this file's client table is otherwise all file-scope
 * static, same as ramfs.c/translate.c's own state, which tests reset via
 * their already-public ramfs_init()/translate_init()/translate_seed()),
 * and report how many client slots are currently occupied (client-limit
 * and idle-timeout-sweep tests, Round 1 review F9). */
void tftp_test_reset(void)
{
	int i;

	for (i = 0; i < TFTP_MAX_CLIENTS; i++) {
		/* Guard on in_use, not on a stale .sock value: the very
		 * first call happens before anything has ever initialized
		 * this BSS-zeroed array (production's main() does that
		 * init, which test builds skip -- see main()'s #ifndef
		 * TFTP_TEST_HOOKS above), so an unused slot's .sock is 0,
		 * not -1, until this function or a real client sets it. */
		if (clients[i].in_use) {
			free(clients[i].blk_buf);
			free(clients[i].rw_buf);
			if (clients[i].sock >= 0)
				qrtr_close(clients[i].sock);
		}
		memset(&clients[i], 0, sizeof(clients[i]));
		clients[i].sock = -1;
	}
}

int tftp_test_active_clients(void)
{
	int i, n = 0;

	for (i = 0; i < TFTP_MAX_CLIENTS; i++)
		n += clients[i].in_use;
	return n;
}
#endif /* TFTP_TEST_HOOKS */
