/* fake_smgr: stands in for the ADSP in tests/test_e2e.sh, over
 * tests/fake_ipc.c. Uses sensord's own message tables, so what it checks
 * is sensord's behaviour (registry answers, report bookkeeping, sample
 * conversion, reset recovery), not the wire layouts themselves; those
 * are pinned to the IDL dump by tests/test_sns_msgs.c.
 *
 *   fake_smgr mkreg FILE SIZE       synthetic registry: byte i = i*7+3
 *   fake_smgr serve [-P]            REG2 checks, then an SMGR server
 *   fake_smgr slow SOCK SECS        claim accel 200, stall, then report
 *
 * serve first acts like the DSP at boot: waits for REG2 (0x10f), reads a
 * group and some items, writes one, and prints "REG2 OK" or
 * "REG2 FAIL ...". Then it publishes SMGR 0x100 and answers the sensor
 * info and report requests, printing "ADD report R sensor S dt D rate N"
 * and "DELETE report R", and streams samples for each active report:
 * accel (SMGR frame) 0.5, 1.0, -9.80665 -> sensord x=1, y=0.5, z=9.80665;
 * light 123.5 lux; proximity near with raw 42. SIGUSR1 simulates a DSP
 * restart: the SMGR and QMAG ports close, every report and QMAG instance
 * is forgotten and the services come back on new ports a second later.
 * SIGUSR2 restarts only SMGR (QMAG stays), so sensord has a live QMAG
 * instance to disable when it notices SMGR is gone.
 *
 * QMAG_CAL (0x140 inst 0x3201, beside a 0x3202 decoy): prints "QMAG
 * ATTR", "QMAG ENABLE period none|N instance I" and "QMAG DISABLE
 * instance I [unknown]"; while an instance is on it sends a report
 * indication every 100 ms, bias (SMGR frame) 0.125, -0.25, 0.375 gauss
 * -> sensord device frame x=-0.25, y=0.125, z=-0.375, accuracy rising
 * 0..3, and one error indication (error 2) after the fifth report.
 * $FAKE_QMAG_ENABLE_DELAY_MS delays every ENABLE response (the instance
 * exists at once, reports start only after the response), for sensord's
 * no-resend and disable-at-exit rules. */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "msmipc.h"
#include "sns_msgs.h"

#define Q16(x) ((int32_t)((x) * 65536.0))

static volatile sig_atomic_t g_stop, g_restart, g_restart_smgr;

static void on_sig(int sig)
{
	if (sig == SIGUSR1)
		g_restart = 1;
	else if (sig == SIGUSR2)
		g_restart_smgr = 1;
	else
		g_stop = 1;
}

static int64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static uint32_t ticks(void)
{
	return (uint32_t)(now_ns() / 1000 * 32768 / 1000000);
}

static int mkreg(const char *path, long size)
{
	FILE *f = fopen(path, "wb");
	long i;

	if (!f)
		return 1;
	for (i = 0; i < size; i++)
		fputc((int)((i * 7 + 3) & 0xff), f);
	return fclose(f) ? 1 : 0;
}

/* ------------------------------------------------------ REG2 checks */

static uint16_t g_txn;

static int call(int sock, uint32_t node, uint32_t port, unsigned msg, const void *req,
		struct qmi_elem_info *req_ei, void *resp, struct qmi_elem_info *resp_ei)
{
	DEFINE_QRTR_PACKET(out, 2048);
	char buf[2048];
	struct qrtr_packet pkt;
	struct sockaddr_qrtr sq = { 0 };
	unsigned int t;
	uint32_t n, p;
	int rc, i;

	if (qmi_encode_message(&out, QMI_REQUEST, (int)msg, ++g_txn, req, req_ei) < 0)
		return -1;
	if (qrtr_sendto(sock, node, port, out.data, (unsigned)out.data_len) < 0)
		return -1;
	for (i = 0; i < 200; i++) {
		rc = qrtr_recvfrom(sock, buf, sizeof(buf), &n, &p);
		if (rc > 0) {
			qrtr_decode(&pkt, buf, (size_t)rc, &sq);
			return qmi_decode_message(resp, &t, &pkt, QMI_RESPONSE, (int)msg, resp_ei);
		}
		usleep(10000);
	}
	return -1;
}

static int reg2_checks(void)
{
	struct msm_ipc_server_info info[2];
	struct sns_reg2_id_req req;
	struct sns_reg2_group_read_resp gr;
	struct sns_reg2_item_read_resp ir;
	struct sns_reg2_group_write_req gw;
	struct sns_generic_resp g;
	int sock = qrtr_open(0), i, n = 0;
	const char *fail = NULL;

	for (i = 0; i < 500 && n <= 0 && !g_stop; i++) {
		n = msmipc_lookup(sock, SNS_REG2_SVC, 0, info, 2);
		if (n <= 0)
			usleep(20000);
	}
	if (n > 0 && info[0].instance != 2) {
		fail = "REG2 published with the wrong instance";
		goto out;
	}
	if (n <= 0) {
		fail = "REG2 not registered";
		goto out;
	}
	/* group 2695 in tests/fixtures/sns_reg.map: offset 256, 40 bytes */
	memset(&gr, 0, sizeof(gr));
	req.id = 2695;
	if (call(sock, 1, info[0].port_id, SNS_REG2_GROUP_READ, &req, sns_reg2_id_req_ei, &gr,
		 sns_reg2_group_read_resp_ei) < 0 || gr.resp.result || gr.id != 2695 ||
	    gr.data_len != 40) {
		fail = "group 2695 read";
		goto out;
	}
	for (i = 0; i < 40; i++)
		if (gr.data[i] != (uint8_t)((256 + i) * 7 + 3)) {
			fail = "group 2695 data";
			goto out;
		}
	/* item 2310: offset 292, 1 byte */
	memset(&ir, 0, sizeof(ir));
	req.id = 2310;
	if (call(sock, 1, info[0].port_id, SNS_REG2_ITEM_READ, &req, sns_reg2_id_req_ei, &ir,
		 sns_reg2_item_read_resp_ei) < 0 || ir.resp.result || ir.data_len != 1 ||
	    ir.data[0] != (uint8_t)(292 * 7 + 3)) {
		fail = "item 2310 read";
		goto out;
	}
	memset(&ir, 0, sizeof(ir));
	req.id = 9999;
	if (call(sock, 1, info[0].port_id, SNS_REG2_ITEM_READ, &req, sns_reg2_id_req_ei, &ir,
		 sns_reg2_item_read_resp_ei) < 0 || ir.resp.result != SNS_RESULT_FAILURE ||
	    ir.resp.err != SNS_ERR_BAD_PARAM) {
		fail = "missing item not refused";
		goto out;
	}
	/* write group 1000 (offset 0, 16 bytes), read item 700 (offset 0) back */
	memset(&gw, 0, sizeof(gw));
	gw.id = 1000;
	gw.data_len = 16;
	for (i = 0; i < 16; i++)
		gw.data[i] = (uint8_t)(0xa0 + i);
	memset(&g, 0, sizeof(g));
	if (call(sock, 1, info[0].port_id, SNS_REG2_GROUP_WRITE, &gw, sns_reg2_group_write_req_ei,
		 &g, sns_generic_resp_ei) < 0 || g.resp.result) {
		fail = "group 1000 write";
		goto out;
	}
	memset(&ir, 0, sizeof(ir));
	req.id = 700;
	if (call(sock, 1, info[0].port_id, SNS_REG2_ITEM_READ, &req, sns_reg2_id_req_ei, &ir,
		 sns_reg2_item_read_resp_ei) < 0 || ir.data_len != 1 || ir.data[0] != 0xa0) {
		fail = "item 700 after write";
		goto out;
	}
	memset(&g, 0, sizeof(g));
	if (call(sock, 1, info[0].port_id, SNS_REG2_MSG_06, NULL, sns_empty_ei, &g,
		 sns_generic_resp_ei) < 0 || g.resp.result != SNS_RESULT_FAILURE) {
		fail = "msg 0x06 not nacked";
		goto out;
	}
out:
	qrtr_close(sock);
	if (fail)
		printf("REG2 FAIL %s\n", fail);
	else
		printf("REG2 OK\n");
	fflush(stdout);
	return fail ? 1 : 0;
}

/* ------------------------------------------------------- SMGR server */

struct report {
	bool on;
	uint8_t sensor, dt;
	unsigned rate;
	int64_t next;
	uint32_t node, port;
};

static struct report reports[256];
static bool g_periodic;

static void reply(int sock, uint32_t node, uint32_t port, unsigned msg, uint16_t txn,
		  const void *resp, struct qmi_elem_info *ei)
{
	DEFINE_QRTR_PACKET(out, 2048);

	if (qmi_encode_message(&out, QMI_RESPONSE, (int)msg, txn, resp, ei) >= 0)
		qrtr_sendto(sock, node, port, out.data, (unsigned)out.data_len);
}

static void fill_dt(struct sns_smgr_dt_info *d, uint8_t id, uint8_t dt, const char *name,
		    const char *vendor, uint16_t max_hz)
{
	d->sensor_id = id;
	d->data_type = dt;
	d->name_len = (uint32_t)strlen(name);
	memcpy(d->name, name, d->name_len);
	d->vendor_len = (uint32_t)strlen(vendor);
	memcpy(d->vendor, vendor, d->vendor_len);
	d->version = 1;
	d->max_rate_hz = max_hz;
	d->max_range = (uint32_t)Q16(156.9);
	d->resolution = (uint32_t)Q16(0.0048);
}

static void smgr_request(int sock, struct qrtr_packet *pkt, uint32_t node, uint32_t port)
{
	const uint8_t *h = pkt->data;
	unsigned int msg, t;
	uint16_t txn = (uint16_t)(h[1] | h[2] << 8);

	if (qmi_decode_header(pkt, &msg) < 0 || h[0] != QMI_REQUEST)
		return;
	if (msg == SNS_SMGR_ALL_INFO) {
		static const struct { uint8_t id; const char *n; } s[] = {
			{ 0, "ACCEL" }, { 10, "GYRO" }, { 20, "MAG" }, { 40, "PROX_LIGHT" } };
		struct sns_smgr_all_info_resp r;
		unsigned i;

		memset(&r, 0, sizeof(r));
		r.n = 4;
		for (i = 0; i < 4; i++) {
			r.info[i].sensor_id = s[i].id;
			r.info[i].name_len = (uint32_t)strlen(s[i].n);
			memcpy(r.info[i].name, s[i].n, r.info[i].name_len);
		}
		reply(sock, node, port, msg, txn, &r, sns_smgr_all_info_resp_ei);
	} else if (msg == SNS_SMGR_SINGLE_INFO) {
		struct sns_smgr_single_info_req q = { 0 };
		static struct sns_smgr_single_info_resp r;

		qmi_decode_message(&q, &t, pkt, QMI_REQUEST, (int)msg, sns_smgr_single_info_req_ei);
		memset(&r, 0, sizeof(r));
		switch (q.sensor_id) {
		case 0:
			r.n = 2;
			fill_dt(&r.dt[0], 0, 0, "BMI160 Accelerometer", "BOSCH", 200);
			fill_dt(&r.dt[1], 0, 1, "BMI160 Temperature", "BOSCH", 25);
			break;
		case 10:
			r.n = 1;
			fill_dt(&r.dt[0], 10, 0, "BMI160 Gyroscope", "BOSCH", 200);
			break;
		case 20:
			r.n = 1;
			fill_dt(&r.dt[0], 20, 0, "AK09918 Magnetometer", "AKM", 50);
			break;
		case 40:
			r.n = 2;
			fill_dt(&r.dt[0], 40, 0, "EPL259x ALS/PS PROX", "Eminent", 10);
			fill_dt(&r.dt[1], 40, 1, "EPL259x ALS/PS ALS \"q\"", "Eminent", 10);
			break;
		default:
			r.resp.result = SNS_RESULT_FAILURE;
			r.resp.err = SNS_ERR_BAD_PARAM;
		}
		reply(sock, node, port, msg, txn, &r, sns_smgr_single_info_resp_ei);
	} else if (msg == SNS_SMGR_BUFFERING) {
		struct sns_smgr_buf_req q;
		struct sns_smgr_buf_resp r;
		struct report *rp;

		memset(&q, 0, sizeof(q));
		memset(&r, 0, sizeof(r));
		qmi_decode_message(&q, &t, pkt, QMI_REQUEST, (int)msg, sns_smgr_buf_req_ei);
		rp = &reports[q.report_id];
		if (q.action == SNS_SMGR_ACTION_ADD && q.n_items == 1) {
			rp->on = true;
			rp->sensor = q.item[0].sensor_id;
			rp->dt = q.item[0].data_type;
			rp->rate = q.item[0].sampling_rate_hz;
			rp->next = now_ns();
			rp->node = node;
			rp->port = port;
			printf("ADD report %u sensor %u dt %u rate %u report_rate %u notify %u cal %u dec %u\n",
			       q.report_id, rp->sensor, rp->dt, rp->rate, q.report_rate_q16 >> 16,
			       q.notify_valid, q.item[0].calibration, q.item[0].decimation);
		} else if (q.action == SNS_SMGR_ACTION_DELETE) {
			rp->on = false;
			printf("DELETE report %u\n", q.report_id);
		} else {
			r.resp.result = SNS_RESULT_FAILURE;
		}
		fflush(stdout);
		r.report_id_valid = r.ack_nak_valid = 1;
		r.report_id = q.report_id;
		reply(sock, node, port, msg, txn, &r, sns_smgr_buf_resp_ei);
	} else if (msg == SNS_SMGR_REPORT) {
		struct sns_smgr_rep_req q;
		struct sns_smgr_rep_resp r;
		struct report *rp;

		memset(&q, 0, sizeof(q));
		memset(&r, 0, sizeof(r));
		qmi_decode_message(&q, &t, pkt, QMI_REQUEST, (int)msg, sns_smgr_rep_req_ei);
		rp = &reports[q.report_id];
		if (q.action == SNS_SMGR_ACTION_ADD && q.n_items == 1) {
			rp->on = true;
			rp->sensor = q.item[0].sensor_id;
			rp->dt = q.item[0].data_type;
			rp->rate = q.report_rate_hz;
			rp->next = now_ns();
			rp->node = node;
			rp->port = port;
			printf("ADD report %u sensor %u dt %u rate %u periodic\n", q.report_id,
			       rp->sensor, rp->dt, rp->rate);
		} else {
			rp->on = false;
			printf("DELETE report %u\n", q.report_id);
		}
		fflush(stdout);
		r.report_id = q.report_id;
		reply(sock, node, port, msg, txn, &r, sns_smgr_rep_resp_ei);
	}
}

static void sample_for(uint8_t sensor, uint8_t dt, int32_t d[3])
{
	d[0] = d[1] = d[2] = 0;
	if (sensor == 40 && dt == 1) {
		d[0] = Q16(123.5);
	} else if (sensor == 40) {
		d[0] = Q16(1);
		d[1] = 42;		/* raw, unscaled */
	} else {
		d[0] = Q16(0.5);
		d[1] = Q16(1.0);
		d[2] = Q16(-9.80665);
	}
}

/* Two samples per indication, the second 1/rate after the first, so the
 * offset accumulation is exercised. */
static void stream(int sock)
{
	int64_t now = now_ns();
	unsigned r;

	for (r = 0; r < 256; r++) {
		struct report *rp = &reports[r];
		DEFINE_QRTR_PACKET(out, 2048);
		uint32_t period = 32768 / (rp->rate ? rp->rate : 1);

		if (!rp->on || now < rp->next)
			continue;
		rp->next = now + 2 * 1000000000LL / rp->rate;
		if (g_periodic) {
			struct sns_smgr_rep_ind ind;

			memset(&ind, 0, sizeof(ind));
			ind.report_id = (uint8_t)r;
			ind.current_rate = (uint16_t)rp->rate;
			ind.n_items = 1;
			ind.item[0].sensor_id = rp->sensor;
			ind.item[0].data_type = rp->dt;
			ind.item[0].ts = ticks();
			sample_for(rp->sensor, rp->dt, ind.item[0].data);
			if (qmi_encode_message(&out, QMI_INDICATION, SNS_SMGR_REPORT_IND, 0, &ind,
					       sns_smgr_rep_ind_ei) >= 0)
				qrtr_sendto(sock, rp->node, rp->port, out.data, (unsigned)out.data_len);
		} else {
			static struct sns_smgr_buf_ind ind;

			memset(&ind, 0, sizeof(ind));
			ind.report_id = (uint8_t)r;
			ind.n_index = 1;
			ind.index[0].sensor_id = rp->sensor;
			ind.index[0].data_type = rp->dt;
			ind.index[0].first = 0;
			ind.index[0].count = 2;
			ind.index[0].first_ts = ticks() - period;
			ind.index[0].rate_q16 = rp->rate << 16;
			ind.n_samples = 2;
			sample_for(rp->sensor, rp->dt, ind.samples[0].data);
			sample_for(rp->sensor, rp->dt, ind.samples[1].data);
			ind.samples[1].ts_offset = (uint16_t)period;
			if (qmi_encode_message(&out, QMI_INDICATION, SNS_SMGR_BUFFERING_IND, 0, &ind,
					       sns_smgr_buf_ind_ei) >= 0)
				qrtr_sendto(sock, rp->node, rp->port, out.data, (unsigned)out.data_len);
		}
	}
}

/* ------------------------------------------------------ QMAG server */

static struct {
	bool on;
	uint8_t instance, next_instance;
	uint32_t node, port;
	unsigned sent;
	int64_t next;
	/* a delayed ENABLE response */
	int64_t reply_at;
	uint16_t reply_txn;
	uint8_t reply_instance;
} qm = { .next_instance = 7 };

static int64_t enable_delay_ns(void)
{
	const char *e = getenv("FAKE_QMAG_ENABLE_DELAY_MS");

	return e ? atoll(e) * 1000000LL : 0;
}

static void qmag_enable_reply(int sock, uint16_t txn, uint8_t instance)
{
	struct sns_sam_instance_resp r;

	memset(&r, 0, sizeof(r));
	r.instance_valid = 1;
	r.instance = instance;
	reply(sock, qm.node, qm.port, SNS_SAM_ENABLE, txn, &r, sns_sam_instance_resp_ei);
}

static void qmag_request(int sock, struct qrtr_packet *pkt, uint32_t node, uint32_t port)
{
	const uint8_t *h = pkt->data;
	unsigned int msg, t;
	uint16_t txn = (uint16_t)(h[1] | h[2] << 8);

	if (qmi_decode_header(pkt, &msg) < 0 || h[0] != QMI_REQUEST)
		return;
	if (msg == SNS_SAM_GET_ATTR) {
		struct sns_sam_attr_resp r;
		int i;

		memset(&r, 0, sizeof(r));
		for (i = 0; i < 9; i++)
			r.attr[i] = (uint32_t)(100 + i);
		r.attr[5] = 10u << 16;	/* min sample rate 10 Hz */
		r.suid_valid = 1;
		r.suid = 0x1122334455667788ULL;
		printf("QMAG ATTR\n");
		reply(sock, node, port, msg, txn, &r, sns_sam_attr_resp_ei);
	} else if (msg == SNS_SAM_ENABLE) {
		struct sns_sam_enable_req q;
		struct sns_sam_instance_resp r;

		memset(&q, 0, sizeof(q));
		memset(&r, 0, sizeof(r));
		qmi_decode_message(&q, &t, pkt, QMI_REQUEST, (int)msg, sns_sam_enable_req_ei);
		qm.on = true;
		qm.instance = qm.next_instance++;
		qm.node = node;
		qm.port = port;
		qm.sent = 0;
		qm.next = now_ns() + 100000000LL + enable_delay_ns();
		if (q.period_valid)
			printf("QMAG ENABLE period %u instance %u\n", q.period_q16, qm.instance);
		else
			printf("QMAG ENABLE period none instance %u\n", qm.instance);
		(void)r;
		if (enable_delay_ns()) {
			qm.reply_at = now_ns() + enable_delay_ns();
			qm.reply_txn = txn;
			qm.reply_instance = qm.instance;
		} else {
			qmag_enable_reply(sock, txn, qm.instance);
		}
	} else if (msg == SNS_SAM_DISABLE) {
		struct sns_sam_disable_req q = { 0 };
		struct sns_sam_instance_resp r;
		bool known;

		memset(&r, 0, sizeof(r));
		qmi_decode_message(&q, &t, pkt, QMI_REQUEST, (int)msg, sns_sam_disable_req_ei);
		known = qm.on && q.instance == qm.instance;
		printf("QMAG DISABLE instance %u%s\n", q.instance, known ? "" : " unknown");
		if (known) {
			qm.on = false;
			r.instance_valid = 1;
			r.instance = q.instance;
		} else {
			r.resp.result = SNS_RESULT_FAILURE;
			r.resp.err = SNS_ERR_BAD_PARAM;
		}
		reply(sock, node, port, msg, txn, &r, sns_sam_instance_resp_ei);
	}
	fflush(stdout);
}

static void qmag_stream(int sock)
{
	DEFINE_QRTR_PACKET(out, 256);

	if (qm.reply_at && now_ns() >= qm.reply_at) {
		qm.reply_at = 0;
		printf("QMAG ENABLE REPLY instance %u\n", qm.reply_instance);
		fflush(stdout);
		qmag_enable_reply(sock, qm.reply_txn, qm.reply_instance);
	}

	if (!qm.on || now_ns() < qm.next)
		return;
	qm.next = now_ns() + 100000000LL;
	qm.sent++;
	{
		struct sns_sam_qmag_ind ind;

		memset(&ind, 0, sizeof(ind));
		ind.instance = qm.instance;
		ind.timestamp = ticks();
		ind.bias[0] = Q16(0.125);
		ind.bias[1] = Q16(-0.25);
		ind.bias[2] = Q16(0.375);
		ind.accuracy = qm.sent / 3 > 3 ? 3 : qm.sent / 3;
		if (qmi_encode_message(&out, QMI_INDICATION, SNS_SAM_REPORT_IND, 0, &ind,
				       sns_sam_qmag_ind_ei) >= 0)
			qrtr_sendto(sock, qm.node, qm.port, out.data, (unsigned)out.data_len);
	}
	if (qm.sent == 5) {
		struct sns_sam_error_ind e = { .error = 2, .instance = qm.instance };

		if (qmi_encode_message(&out, QMI_INDICATION, SNS_SAM_ERROR_IND, 0, &e,
				       sns_sam_error_ind_ei) >= 0)
			qrtr_sendto(sock, qm.node, qm.port, out.data, (unsigned)out.data_len);
	}
}

static void serve_one(int sock, void (*handle)(int, struct qrtr_packet *, uint32_t, uint32_t))
{
	char buf[2048];
	struct qrtr_packet pkt;
	struct sockaddr_qrtr sq = { 0 };
	uint32_t node, port;
	int rc = qrtr_recvfrom(sock, buf, sizeof(buf), &node, &port);

	if (rc > 0) {
		sq.sq_node = node;
		sq.sq_port = port;
		qrtr_decode(&pkt, buf, (size_t)rc, &sq);
		handle(sock, &pkt, node, port);
	}
}

static int serve(void)
{
	int sock = -1, qsock = -1, decoy, qdecoy;
	int64_t republish = 0, qrepublish = 0;

	if (reg2_checks())
		return 1;
	/* Another instance of 0x100 (the router lists every instance of a
	 * service for an instance-0 lookup): sensord must pick 0x3201, and
	 * anything sent here is a bug. */
	decoy = qrtr_open(0);
	if (decoy < 0 || qrtr_publish(decoy, SNS_SMGR_SVC, 2, 0x32) < 0)
		return 1;
	qdecoy = qrtr_open(0);
	if (qdecoy < 0 || qrtr_publish(qdecoy, SNS_QMAG_SVC, 2, 0x32) < 0)
		return 1;
	while (!g_stop) {
		struct pollfd pfd[2];

		if (g_restart || g_restart_smgr) {
			if (sock >= 0)
				qrtr_close(sock);
			sock = -1;
			memset(reports, 0, sizeof(reports));
			republish = now_ns() + 1000000000LL;
			if (g_restart) {
				if (qsock >= 0)
					qrtr_close(qsock);
				qsock = -1;
				qm.on = false;
				qrepublish = republish;
			}
			printf("%s\n", g_restart ? "RESTART" : "RESTART SMGR");
			fflush(stdout);
			g_restart = g_restart_smgr = 0;
		}
		if (qsock < 0 && now_ns() >= qrepublish) {
			qsock = qrtr_open(0);
			if (qsock < 0 || qrtr_publish(qsock, SNS_QMAG_SVC, SNS_QMAG_VERS, SNS_QMAG_INST) < 0)
				return 1;
			printf("QMAG UP\n");
			fflush(stdout);
		}
		if (sock < 0 && now_ns() >= republish) {
			sock = qrtr_open(0);
			if (sock < 0 || qrtr_publish(sock, SNS_SMGR_SVC, SNS_SMGR_VERS, SNS_SMGR_INST) < 0)
				return 1;
			printf("SMGR UP\n");
			fflush(stdout);
		}
		pfd[0] = (struct pollfd){ .fd = sock, .events = POLLIN };
		pfd[1] = (struct pollfd){ .fd = qsock, .events = POLLIN };
		if (poll(pfd, 2, 5) > 0) {
			if (sock >= 0 && (pfd[0].revents & POLLIN))
				serve_one(sock, smgr_request);
			if (qsock >= 0 && (pfd[1].revents & POLLIN))
				serve_one(qsock, qmag_request);
		}
		if (sock >= 0)
			stream(sock);
		if (qsock >= 0)
			qmag_stream(qsock);
		{
			char buf[64];

			if (qrtr_recvfrom(decoy, buf, sizeof(buf), NULL, NULL) > 0 ||
			    qrtr_recvfrom(qdecoy, buf, sizeof(buf), NULL, NULL) > 0) {
				printf("DECOY got a message\n");
				fflush(stdout);
			}
		}
	}
	if (sock >= 0)
		qrtr_close(sock);
	if (qsock >= 0)
		qrtr_close(qsock);
	qrtr_close(decoy);
	qrtr_close(qdecoy);
	return 0;
}

/* A client that claims accel at 200 Hz and then does not read. */
static int slow(const char *path, int secs)
{
	struct sockaddr_un sa;
	static char buf[1 << 20];
	ssize_t n;
	size_t total = 0;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0), sz = 4096;
	char *p;

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
		return 1;
	if (write(fd, "claim accel 200\n", 16) != 16)
		return 1;
	sleep((unsigned)secs);
	/* then read for one second: the stream never pauses by itself */
	for (int64_t end = now_ns() + 1000000000LL; now_ns() < end;) {
		struct pollfd pfd = { fd, POLLIN, 0 };

		if (poll(&pfd, 1, 100) < 0 || total >= sizeof(buf) - 1)
			break;
		if (!pfd.revents)
			continue;
		n = read(fd, buf + total, sizeof(buf) - 1 - total);
		if (n <= 0)
			break;
		total += (size_t)n;
	}
	buf[total] = '\0';
	p = strstr(buf, "{\"dropped\":");
	printf("slow client: %zu bytes, %s\n", total, p ? "dropped reported" : "no drop line");
	if (p) {
		char *e = strchr(p, '\n');

		if (e)
			*e = '\0';
		printf("%s\n", p);
	}
	close(fd);
	return p ? 0 : 1;
}

int main(int argc, char **argv)
{
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGUSR1, on_sig);
	signal(SIGUSR2, on_sig);
	if (argc == 4 && strcmp(argv[1], "mkreg") == 0)
		return mkreg(argv[2], atol(argv[3]));
	if (argc >= 2 && strcmp(argv[1], "serve") == 0) {
		g_periodic = argc == 3 && strcmp(argv[2], "-P") == 0;
		return serve();
	}
	if (argc == 4 && strcmp(argv[1], "slow") == 0)
		return slow(argv[2], atoi(argv[3]));
	fprintf(stderr, "usage: fake_smgr mkreg FILE SIZE | serve [-P] | slow SOCK SECS\n");
	return 64;
}
