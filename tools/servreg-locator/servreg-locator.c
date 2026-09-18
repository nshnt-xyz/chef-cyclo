/* servreg-locator: userspace SERVREG_LOC (a.k.a. pd-mapper) server on this
 * vendor kernel's AF_MSM_IPC QMI transport -- service 0x40 instance 0x101.
 *
 * Why this exists: this device's modem firmware runs a servreg_locator
 * client that blocks the Hexagon "dog" startup-grace timer until it either
 * gets a GET_DOMAIN_LIST response or the timer's 40 s grace period expires,
 * at which point the modem raises ERR_FATAL (see README's 2026-09-16 (dog
 * timeout) entry and the two research reports it summarizes). Stock
 * Android runs a locator (`/vendor/bin/pd-mapper`) before PIL for exactly
 * this reason; this file is the minimal, from-scratch, Alpine-side
 * equivalent, serving the same *.jsn domain descriptors stock's pd-mapper
 * reads, so the modem's very first boot-time QMI exchange succeeds instead
 * of stalling. It answers only the two requests this modem firmware and
 * this kernel's own client are confirmed to send (INDICATION_REGISTER,
 * GET_DOMAIN_LIST), best-effort-acks the crash-report message (PFR) the
 * firmware strings show it capable of sending, and NACKs anything else
 * with QMI_ERR_NOT_SUPPORTED instead of leaving a caller to time out --
 * see servreg_loc.h and jsn.h for the wire/file format details and gps-up
 * for how this fits into modem bring-up (started after rmtfs, before
 * /dev/subsys_modem is opened; gps-up fails closed if this doesn't publish
 * 0x40 in time).
 *
 * This locator alone fixes the +40.02s ERR_FATAL (the Hexagon dog timer
 * above), but the 2026-09-17 stock-read probe found it is not sufficient by
 * itself to get the modem past its RFS boot writes -- see tools/tftp/
 * (service 0x1000, stock `tftp_server`/upstream `tqftpserv`), started
 * alongside this daemon in gps-up. Deliberately NOT provided here: dynamic
 * REGISTER_SERVICE_LIST handling (nothing on this device is known to send
 * it to us; stock's own pd-mapper logs it as "Unsupported request" too).
 *
 * Usage: servreg-locator [-v] [-d /firmware/image]
 */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "jsn.h"
#include "servreg_loc.h"

static volatile sig_atomic_t g_stop;

static void on_sigterm(int sig)
{
	(void)sig;
	g_stop = 1;
}

/* Every QMI header (see qrtr/qmi.c's private struct qmi_header) is
 * type:u8, txn_id:u16 LE, msg_id:u16 LE, msg_len:u16 LE, packed -- fixed
 * regardless of message content. Used only for the generic NOT_SUPPORTED
 * NACK path, which by definition has no per-message ei table to decode
 * the txn out of. Caller guarantees data_len >= 7 (checked once in
 * handle_locator()). */
static uint16_t qmi_header_txn(const void *data)
{
	const uint8_t *p = data;

	return (uint16_t)(p[1] | (p[2] << 8));
}

static void handle_get_domain_list(int sock, struct qrtr_packet *pkt,
				    const struct jsn_table *table, int verbose)
{
	struct servreg_loc_get_domain_list_req req;
	struct servreg_loc_get_domain_list_resp resp;
	DEFINE_QRTR_PACKET(resp_buf, SERVREG_LOC_RESP_BUF_LEN);
	unsigned int txn = 0;
	uint32_t offset = 0;
	uint32_t total = 0;
	size_t written = 0;
	ssize_t len;
	int ret;

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	ret = qmi_decode_message(&req, &txn, pkt, QMI_REQUEST,
				  SERVREG_LOC_GET_DOMAIN_LIST_REQ,
				  servreg_loc_get_domain_list_req_ei);
	if (ret < 0) {
		fprintf(stderr, "[SERVREG-LOC] malformed GET_DOMAIN_LIST_REQ, nacking\n");
		resp.resp.result = QMI_RESULT_FAILURE_V01;
		resp.resp.error = QMI_ERR_MALFORMED_MSG_V01;
		goto respond;
	}

	if (req.domain_offset_valid)
		offset = req.domain_offset;

	written = jsn_table_lookup(table, req.service_name, offset,
				    resp.domain_list, SERVREG_LOC_LIST_LENGTH, &total);

	resp.resp.result = QMI_RESULT_SUCCESS_V01;
	resp.resp.error = QMI_ERR_NONE_V01;
	resp.total_domains_valid = 1;
	resp.total_domains = (uint16_t)(total > 0xffffu ? 0xffffu : total);
	resp.db_rev_count_valid = 1;
	resp.db_rev_count = 1; /* constant for this daemon's lifetime, see jsn.h */
	resp.domain_list_valid = written > 0;
	resp.domain_list_len = (uint32_t)written;

	fprintf(stderr, "[SERVREG-LOC] GET_DOMAIN_LIST \"%s\" offset %u => %zu/%u domain(s)%s\n",
		req.service_name, offset, written, total,
		verbose && written == 0 ? " (no match)" : "");

respond:
	len = qmi_encode_message(&resp_buf, QMI_RESPONSE, SERVREG_LOC_GET_DOMAIN_LIST_REQ,
				  txn, &resp, servreg_loc_get_domain_list_resp_ei);
	if (len < 0) {
		fprintf(stderr, "[SERVREG-LOC] failed to encode GET_DOMAIN_LIST_RESP: %s\n",
			strerror((int)-len));
		return;
	}
	ret = qrtr_sendto(sock, pkt->node, pkt->port, resp_buf.data, resp_buf.data_len);
	if (ret < 0)
		fprintf(stderr, "[SERVREG-LOC] failed to send GET_DOMAIN_LIST_RESP: %s\n",
			strerror(-ret));
}

static void handle_indication_register(int sock, struct qrtr_packet *pkt, int verbose)
{
	struct servreg_loc_indication_register_req req;
	struct servreg_loc_generic_resp resp;
	DEFINE_QRTR_PACKET(resp_buf, 256);
	unsigned int txn = 0;
	ssize_t len;
	int ret;

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	ret = qmi_decode_message(&req, &txn, pkt, QMI_REQUEST,
				  SERVREG_LOC_INDICATION_REGISTER_REQ,
				  servreg_loc_indication_register_req_ei);
	if (ret < 0)
		fprintf(stderr, "[SERVREG-LOC] malformed INDICATION_REGISTER_REQ, acking anyway\n");
	else if (verbose)
		fprintf(stderr, "[SERVREG-LOC] INDICATION_REGISTER enable=%s\n",
			req.enable_database_updated_indication_valid
				? (req.enable_database_updated_indication ? "1" : "0")
				: "(unset)");

	/* We have no live database-update stream to offer, but ack success
	 * unconditionally -- stock pd-mapper does the same (this request is
	 * advisory: a client that never receives an indication just never
	 * sees a database change, which is accurate, since ours never
	 * changes after startup). */
	resp.resp.result = QMI_RESULT_SUCCESS_V01;
	resp.resp.error = QMI_ERR_NONE_V01;

	len = qmi_encode_message(&resp_buf, QMI_RESPONSE, SERVREG_LOC_INDICATION_REGISTER_REQ,
				  txn, &resp, servreg_loc_generic_resp_ei);
	if (len < 0) {
		fprintf(stderr, "[SERVREG-LOC] failed to encode INDICATION_REGISTER_RESP: %s\n",
			strerror((int)-len));
		return;
	}
	ret = qrtr_sendto(sock, pkt->node, pkt->port, resp_buf.data, resp_buf.data_len);
	if (ret < 0)
		fprintf(stderr, "[SERVREG-LOC] failed to send INDICATION_REGISTER_RESP: %s\n",
			strerror(-ret));
}

/* PFR ("service crashed") reports are diagnostically useful on their own,
 * so this always logs (not gated on -v), unlike the other handlers. The
 * request's exact TLV shape is our least-certain guess in this file (see
 * servreg_loc.h); decode failure still gets a success ack -- see the file
 * header and servreg_loc.h's struct comment for why silence is worse than
 * an unconditional ack here. */
static void handle_pfr(int sock, struct qrtr_packet *pkt)
{
	struct servreg_loc_pfr_req req;
	struct servreg_loc_generic_resp resp;
	DEFINE_QRTR_PACKET(resp_buf, 256);
	unsigned int txn = 0;
	ssize_t len;
	int ret;

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));

	ret = qmi_decode_message(&req, &txn, pkt, QMI_REQUEST, SERVREG_LOC_PFR_REQ,
				  servreg_loc_pfr_req_ei);
	if (ret < 0)
		fprintf(stderr, "[SERVREG-LOC] PFR notification received (body did not match "
				"the expected shape, see servreg_loc.h), acking\n");
	else
		fprintf(stderr, "[SERVREG-LOC] PFR: service '%s' crashed, reason '%s'\n",
			req.service, req.reason);

	resp.resp.result = QMI_RESULT_SUCCESS_V01;
	resp.resp.error = QMI_ERR_NONE_V01;

	len = qmi_encode_message(&resp_buf, QMI_RESPONSE, SERVREG_LOC_PFR_REQ,
				  txn, &resp, servreg_loc_generic_resp_ei);
	if (len < 0) {
		fprintf(stderr, "[SERVREG-LOC] failed to encode PFR_RESP: %s\n", strerror((int)-len));
		return;
	}
	ret = qrtr_sendto(sock, pkt->node, pkt->port, resp_buf.data, resp_buf.data_len);
	if (ret < 0)
		fprintf(stderr, "[SERVREG-LOC] failed to send PFR_RESP: %s\n", strerror(-ret));
}

static void handle_unsupported(int sock, struct qrtr_packet *pkt, unsigned int msg_id, int verbose)
{
	struct servreg_loc_generic_resp resp;
	DEFINE_QRTR_PACKET(resp_buf, 256);
	unsigned int txn = qmi_header_txn(pkt->data);
	ssize_t len;
	int ret;

	memset(&resp, 0, sizeof(resp));

	if (verbose)
		fprintf(stderr, "[SERVREG-LOC] unsupported request 0x%04x, nacking\n", msg_id);

	resp.resp.result = QMI_RESULT_FAILURE_V01;
	resp.resp.error = QMI_ERR_NOT_SUPPORTED_V01;

	len = qmi_encode_message(&resp_buf, QMI_RESPONSE, (int)msg_id, txn, &resp,
				  servreg_loc_generic_resp_ei);
	if (len < 0) {
		fprintf(stderr, "[SERVREG-LOC] failed to encode NOT_SUPPORTED nack for 0x%04x: %s\n",
			msg_id, strerror((int)-len));
		return;
	}
	ret = qrtr_sendto(sock, pkt->node, pkt->port, resp_buf.data, resp_buf.data_len);
	if (ret < 0)
		fprintf(stderr, "[SERVREG-LOC] failed to send NOT_SUPPORTED nack for 0x%04x: %s\n",
			msg_id, strerror(-ret));
}

static void handle_locator(int sock, struct qrtr_packet *pkt,
			    const struct jsn_table *table, int verbose)
{
	unsigned int msg_id;
	uint8_t type;

	/* qmi_decode_header() dereferences a 7-byte header unconditionally;
	 * bound-check before it does, since pkt->data_len is whatever a peer
	 * sent (see servreg_loc.h's codec-caveat note for the same class of
	 * issue in the string decoder). */
	if (pkt->data_len < 7) {
		fprintf(stderr, "[SERVREG-LOC] short QMI message (%zu bytes), dropping\n",
			pkt->data_len);
		return;
	}

	type = ((const uint8_t *)pkt->data)[0];

	if (qmi_decode_header(pkt, &msg_id) < 0) {
		fprintf(stderr, "[SERVREG-LOC] malformed QMI header, dropping\n");
		return;
	}
	if (type != QMI_REQUEST)
		return; /* we never issue requests, so never expect a response/indication */

	switch (msg_id) {
	case SERVREG_LOC_GET_DOMAIN_LIST_REQ:
		handle_get_domain_list(sock, pkt, table, verbose);
		break;
	case SERVREG_LOC_INDICATION_REGISTER_REQ:
		handle_indication_register(sock, pkt, verbose);
		break;
	case SERVREG_LOC_PFR_REQ:
		handle_pfr(sock, pkt);
		break;
	default:
		handle_unsupported(sock, pkt, msg_id, verbose);
		break;
	}
}

static int run_locator(const struct jsn_table *table, int verbose)
{
	int sock;
	int ret = 0;

	sock = qrtr_open(0);
	if (sock < 0) {
		fprintf(stderr, "[SERVREG-LOC] failed to create socket: %s\n", strerror(errno));
		return -1;
	}

	ret = qrtr_publish(sock, SERVREG_LOC_SERVICE_ID, SERVREG_LOC_SERVICE_VERSION,
			    SERVREG_LOC_SERVICE_INSTANCE);
	if (ret < 0) {
		fprintf(stderr, "[SERVREG-LOC] failed to publish service 0x%02x: %s\n",
			SERVREG_LOC_SERVICE_ID, strerror(errno));
		qrtr_close(sock);
		return -1;
	}
	fprintf(stderr, "[SERVREG-LOC] published service 0x%02x instance 0x%03x (%zu entries loaded)\n",
		SERVREG_LOC_SERVICE_ID,
		((unsigned)SERVREG_LOC_SERVICE_INSTANCE << 8) | SERVREG_LOC_SERVICE_VERSION,
		table->count);

	while (!g_stop) {
		struct qrtr_packet pkt;
		struct sockaddr_qrtr sq;
		char buf[4096];
		uint32_t node, port;
		fd_set rfds;

		FD_ZERO(&rfds);
		FD_SET(sock, &rfds);

		ret = select(sock + 1, &rfds, NULL, NULL, NULL);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "[SERVREG-LOC] select failed: %s\n", strerror(errno));
			break;
		}
		if (!FD_ISSET(sock, &rfds))
			continue;

		ret = qrtr_recvfrom(sock, buf, sizeof(buf), &node, &port);
		if (ret == QRTR_RECV_RESUME_TX ||
		    ret == -EAGAIN || ret == -EWOULDBLOCK || ret == -EINTR)
			continue;
		if (ret < 0) {
			if (ret == -ENETRESET) {
				fprintf(stderr, "[SERVREG-LOC] IPC router reset, rebinding\n");
				break;
			}
			fprintf(stderr, "[SERVREG-LOC] recvfrom failed: %s\n", strerror(-ret));
			continue;
		}

		sq.sq_family = AF_QIPCRTR;
		sq.sq_node = node;
		sq.sq_port = port;

		if (qrtr_decode(&pkt, buf, (size_t)ret, &sq) < 0) {
			fprintf(stderr, "[SERVREG-LOC] unable to decode qrtr packet\n");
			continue;
		}

		if (pkt.type == QRTR_TYPE_DATA)
			handle_locator(sock, &pkt, table, verbose);
	}

	qrtr_close(sock);
	return ret;
}

int main(int argc, char **argv)
{
	struct jsn_table table;
	const char *dir = "/firmware/image";
	int verbose = 0;
	int opt;
	int ret;
	struct sigaction action;

	while ((opt = getopt(argc, argv, "vd:")) != -1) {
		switch (opt) {
		case 'v':
			verbose = 1;
			break;
		case 'd':
			dir = optarg;
			break;
		default:
			fprintf(stderr, "usage: %s [-v] [-d /firmware/image]\n", argv[0]);
			return 2;
		}
	}

	memset(&table, 0, sizeof(table));
	jsn_load_dir(dir, &table, verbose);
	fprintf(stderr, "[SERVREG-LOC] loaded %zu (domain,service) entries from %s\n",
		table.count, dir);

	memset(&action, 0, sizeof(action));
	action.sa_handler = on_sigterm;
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);

	do {
		ret = run_locator(&table, verbose);
	} while (ret == -ENETRESET && !g_stop);

	return 0;
}
