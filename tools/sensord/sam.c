/*
 * sensord's SAM clients on the ADSP: QMAG_CAL (-Q) and the rotation
 * vector (-V), with their status fields and rotvec lines. See main.c,
 * sections 4 and 6.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "sensord.h"

/* The latest QMAG_CAL bias: device frame gauss (the magn samples' axis
 * map), the raw SMGR-frame Q16 words, accuracy as the DSP sends it. */
int sn_format_qmag_fields(const struct sam *q, char *buf, size_t len)
{
	double v[3];

	sn_convert_sample(CH_VEC3, q->bias, v);
	return snprintf(buf, len, "\"bias\":[%.6g,%.6g,%.6g],\"bias_raw\":[%" PRId32 ",%" PRId32
			",%" PRId32 "],\"accuracy\":%" PRIu32, v[0], v[1], v[2], q->bias[0], q->bias[1],
			q->bias[2], q->accuracy);
}

const char *sn_sam_state_name(const struct sam *q)
{
	if (!q->opt)
		return "off";
	if (q->busy)
		return q->msg == SNS_SAM_ENABLE ? "enabling" : q->msg == SNS_SAM_DISABLE ? "disabling"
										: "attributes";
	if (q->on)
		return "on";
	return q->failed ? "failed" : "idle";
}


/* The service's port is gone (send failed, socket reset, no answer):
 * look it up again next time its channel wants it. */
void sn_sam_forget(struct server *s, struct sam *q, const char *why)
{
	if (q->port)
		s->log("%s %u:%u lost (%s)%s", q->name, q->node, q->port, why,
		       q->on ? "; its instance is gone with it" : "");
	if (q->sock >= 0)
		qrtr_close(q->sock);
	q->sock = -1;
	q->node = q->port = 0;
	q->on = q->busy = q->have = false;
	q->seen[0] = '\0';
	q->missing_logged = false;
}

int sn_sam_send(struct server *s, struct sam *q, uint16_t msg, struct qmi_elem_info *ei,
		const void *req)
{
	ssize_t len;
	int rc;

	q->pkt.data = q->buf;
	q->pkt.data_len = sizeof(q->buf);
	s->txn = s->txn == 0xffff ? 1 : s->txn + 1;
	len = qmi_encode_message(&q->pkt, QMI_REQUEST, msg, s->txn, req, ei);
	if (len < 0) {
		s->log("encode %s msg 0x%02x failed: %s", q->name, msg, strerror((int)-len));
		return (int)len;
	}
	q->busy = true;
	q->txn = s->txn;
	q->msg = msg;
	q->sent = s->now_ms();
	q->tries = 1;
	sn_vlog("%s -> msg 0x%02x txn %u (%zd bytes)", q->name, msg, q->txn, len);
	rc = qrtr_sendto(q->sock, q->node, q->port, q->pkt.data, (unsigned)q->pkt.data_len);
	if (rc < 0) {
		sn_sam_forget(s, q, strerror(-rc));
		return rc;
	}
	return 0;
}

/* Rate-limited lookup of the service's instance 0x3201. 1 when found. */
static int sam_lookup(struct server *s, struct sam *q)
{
	struct msm_ipc_server_info info;
	int64_t now = s->now_ms();
	int n;

	if (now < q->next_lookup)
		return 0;
	q->next_lookup = now + SENSORD_LOOKUP_MS;
	if (q->sock < 0) {
		q->sock = qrtr_open(0);
		if (q->sock < 0) {
			s->log("%s socket: %s", q->name, strerror(errno));
			return -1;
		}
	}
	n = sn_svc_lookup(s, q->sock, q->svc, q->inst, q->seen, sizeof(q->seen), &info);
	if (n > 0) {
		q->node = info.node_id;
		q->port = info.port_id;
		q->missing_logged = false;
		s->log("%s service 0x%x found at %u:%u", q->name, q->svc, q->node, q->port);
		return 1;
	}
	if (!q->missing_logged) {
		q->missing_logged = true;
		s->log("%s service 0x%x inst 0x%x not registered; polling every %d ms while %s is claimed",
		       q->name, q->svc, q->inst, SENSORD_LOOKUP_MS, s->ch[q->ch].name);
	}
	return n;
}

bool sn_sam_wanted(const struct server *s, const struct sam *q)
{
	return q->opt && !q->failed && s->st == SMGR_READY && sn_channel_used(s, q->ch);
}

/* The ENABLE request of each service. */
static int sam_enable(struct server *s, struct sam *q)
{
	int rc;

	if (q->kind == SAM_QMAG) {
		/* No TLV 0x10: its IDL entry (max_len 0, a u32 at offset 1 of
		 * a 2-byte struct) says no stock client can send it, and a DSP
		 * decoder built from that table could overrun its buffer. */
		struct sns_sam_enable_req r = { .period_valid = 0 };

		rc = sn_sam_send(s, q, SNS_SAM_ENABLE, sns_sam_enable_req_ei, &r);
		if (rc == 0)
			s->log("qmag enable (no report period TLV)");
		return rc;
	}
	{
		/* Exactly what the stock HAL sends by default (its
		 * "synchronous req"): a report per result (period 0), the
		 * sample rate, and suspend notification {apps processor, no
		 * indications in suspend}; no coordinate-system TLV (0x11). */
		struct sns_rotvec_enable_req r;

		memset(&r, 0, sizeof(r));
		r.period_q16 = 0;
		r.rate_valid = 1;
		r.rate_q16 = (uint32_t)SENSORD_ROTVEC_HZ << 16;
		r.notify_valid = 1;
		r.notify.proc_type = 0;
		r.notify.send_during_suspend = 0;
		rc = sn_sam_send(s, q, SNS_SAM_ENABLE, sns_rotvec_enable_req_ei, &r);
		if (rc == 0)
			s->log("rotvec enable (report period 0, sample rate %d Hz, suspend notify apps/no; no coordinate TLV, as the stock HAL)",
			       SENSORD_ROTVEC_HZ);
		return rc;
	}
}

/* Bring a SAM service in line with its channel: on while the channel is
 * used and SMGR is up, off otherwise. One request at a time; called
 * again when it is answered. */
static void sam_sync(struct server *s, struct sam *q)
{
	if (!q->opt)
		return;
	if (!sn_channel_used(s, q->ch))
		q->failed = false;
	if (q->busy)
		return;
	if (q->on) {
		if (!sn_sam_wanted(s, q)) {
			struct sns_sam_disable_req r = { .instance = q->instance };
			const char *why = s->st != SMGR_READY ? "smgr not ready" : "no claims";

			if (sn_sam_send(s, q, SNS_SAM_DISABLE, sns_sam_disable_req_ei, &r) == 0)
				s->log("%s disable instance %u (%s)", q->name, r.instance,
				       q->ch == CH_MAGN && s->st == SMGR_READY ? "magn no longer used" : why);
		}
		return;
	}
	if (!sn_sam_wanted(s, q))
		return;
	if (!q->port && sam_lookup(s, q) <= 0)
		return;
	if (!q->attr_done) {
		sn_sam_send(s, q, SNS_SAM_GET_ATTR, sns_empty_ei, NULL);
		return;
	}
	sam_enable(s, q);
}

void sn_sam_sync_all(struct server *s)
{
	sam_sync(s, &s->qmag);
	sam_sync(s, &s->rotvec);
}

/* A report for an instance that is not ours (e.g. one whose DISABLE the
 * DSP refused): counted; logged the first time and then at most once per
 * 5 s (the rotation vector reports at 20 Hz). */
static void sam_foreign(struct server *s, struct sam *q, uint8_t instance)
{
	int64_t now = s->now_ms();

	q->foreign++;
	if (q->foreign > 1 && now - q->last_foreign < 5000)
		return;
	q->last_foreign = now;
	s->log("%s report for instance %u ignored (%s %u; %lu such reports so far)", q->name, instance,
	       q->on ? "ours is" : "not enabled; last", q->instance, q->foreign);
}

static void qmag_report(struct server *s, const struct sns_sam_qmag_ind *ind)
{
	struct sam *q = &s->qmag;
	void (*note)(const char *fmt, ...) = s->elog;
	int64_t now = s->now_ms();
	double v[3];

	if (!q->on || ind->instance != q->instance) {
		sam_foreign(s, q, ind->instance);
		return;
	}
	q->inds++;
	/* Every report goes to stderr; the kernel log gets the first, any
	 * accuracy change, and at most one every 5 s otherwise. */
	if (q->inds == 1 || !q->have || ind->accuracy != q->accuracy ||
	    now - q->last_note >= 5000) {
		note = s->log;
		q->last_note = now;
	}
	memcpy(q->bias, ind->bias, sizeof(q->bias));
	q->accuracy = ind->accuracy;
	q->ts = ind->timestamp;
	q->have = true;
	sn_convert_sample(CH_VEC3, ind->bias, v);
	note("qmag report %lu: instance %u ts %" PRIu32 " bias raw %" PRId32 " %" PRId32 " %" PRId32
	     " (SMGR frame, Q16 %.5f %.5f %.5f gauss) device x %.5f y %.5f z %.5f gauss, accuracy %" PRIu32,
	     q->inds, ind->instance, ind->timestamp, ind->bias[0], ind->bias[1], ind->bias[2],
	     ind->bias[0] / 65536.0, ind->bias[1] / 65536.0, ind->bias[2] / 65536.0, v[0], v[1], v[2],
	     ind->accuracy);
}

/* A rotation vector line: the quaternion as the DSP sends it (x y z w,
 * Android's order), its accuracy and coordinate bytes, and the heading,
 * pitch and roll of that attitude with the compass's own forward rule
 * (read as body to East-North-Up, as an Android rotation vector). */
static int format_rotvec(const struct server *s, char *buf, size_t len, int64_t t)
{
	const struct sam *r = &s->rotvec;
	double q[4] = { r->q[3], r->q[0], r->q[1], r->q[2] }, n, h, p, ro;
	char c[4][24];
	int i, k;

	/* words that are not finite floats (the "not floats after all"
	 * case) print as null: JSON has no nan or inf */
	for (i = 0; i < 4; i++)
		if (isfinite(r->q[i]))
			snprintf(c[i], sizeof(c[i]), "%.6g", r->q[i]);
		else
			snprintf(c[i], sizeof(c[i]), "null");
	k = snprintf(buf, len, "{\"type\":\"sample\",\"sensor\":\"rotvec\",\"t\":%" PRId64
		     ",\"x\":%s,\"y\":%s,\"z\":%s,\"w\":%s,\"accuracy\":%" PRIu32 ",\"coord\":%u", t,
		     c[0], c[1], c[2], c[3], r->accuracy, r->coord);
	if (k < 0 || (size_t)k >= len)
		return -1;
	n = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	if (isfinite(n) && n > 0.5 && n < 1.5) {
		for (i = 0; i < 4; i++)
			q[i] /= n;
		if (compass_angles(q, s->heading.mount, &h, &p, &ro))
			k += snprintf(buf + k, len - (size_t)k, ",\"heading\":%.2f",
				      compass_wrap360(compass_round2(h)));
		if ((size_t)k < len)
			k += snprintf(buf + k, len - (size_t)k, ",\"pitch\":%.2f,\"roll\":%.2f",
				      compass_round2(p), compass_round2(ro));
	}
	if ((size_t)k < len)
		k += snprintf(buf + k, len - (size_t)k, "}\n");
	return (size_t)k < len ? k : -1;
}

static void rotvec_report(struct server *s, const struct sns_sam_rotvec_ind *ind)
{
	struct sam *r = &s->rotvec;
	int64_t now = s->now_ms(), a_ns;
	uint32_t a_ticks;
	char line[320];
	double n;
	int i, k;

	if (!r->on || ind->instance != r->instance) {
		sam_foreign(s, r, ind->instance);
		return;
	}
	r->inds++;
	for (i = 0; i < 4; i++)
		memcpy(&r->q[i], &ind->r.q[i], sizeof(float));
	n = sqrt((double)r->q[0] * r->q[0] + (double)r->q[1] * r->q[1] + (double)r->q[2] * r->q[2] +
		 (double)r->q[3] * r->q[3]);
	/* the first, accuracy or coordinate changes, and one per 5 s to the log */
	if (r->inds == 1 || !r->have || ind->r.accuracy != r->accuracy || ind->r.coord != r->coord ||
	    now - r->last_note >= 5000) {
		r->last_note = now;
		s->log("rotvec report %lu: instance %u ts %" PRIu32 " q x %.5f y %.5f z %.5f w %.5f (|q| %.4f) accuracy %u coord %u",
		       r->inds, ind->instance, ind->timestamp, r->q[0], r->q[1], r->q[2], r->q[3], n,
		       ind->r.accuracy, ind->r.coord);
	}
	if (!(isfinite(n) && n > 0.5 && n < 1.5) && !r->norm_logged) {
		r->norm_logged = true;
		s->log("rotvec quaternion norm %.4f: not a unit quaternion as floats; lines carry no heading", n);
	}
	r->accuracy = ind->r.accuracy;
	r->coord = ind->r.coord;
	r->ts = ind->timestamp;
	r->have = true;
	sn_time_anchor(s, ind->timestamp, &a_ticks, &a_ns);
	k = format_rotvec(s, line, sizeof(line), sn_ticks_to_ns(ind->timestamp, a_ticks, a_ns));
	if (k > 0 && sn_deliver(s, CH_ROTVEC, sn_ticks_to_ns(ind->timestamp, a_ticks, a_ns), line, (size_t)k))
		sn_smgr_sync(s);
}

/* An ENABLE response we are not waiting for (after a give-up, or a
 * duplicate) that created an instance which is not ours: disable it at
 * once, fire and forget, so no DSP instance outlives its owner. */
static void sam_stray_enable(struct server *s, struct sam *q, struct qrtr_packet *pkt)
{
	struct sns_sam_instance_resp r;
	struct sns_sam_disable_req d;
	DEFINE_QRTR_PACKET(out, 64);
	unsigned int t;
	int rc;

	memset(&r, 0, sizeof(r));
	rc = qmi_decode_message(&r, &t, pkt, QMI_RESPONSE, SNS_SAM_ENABLE, sns_sam_instance_resp_ei);
	if (rc < 0 || r.resp.result != SNS_RESULT_SUCCESS || !r.instance_valid)
		return;
	if (q->on && r.instance == q->instance)
		return;
	d.instance = r.instance;
	s->txn = s->txn == 0xffff ? 1 : s->txn + 1;
	rc = (int)qmi_encode_message(&out, QMI_REQUEST, SNS_SAM_DISABLE, s->txn, &d,
				     sns_sam_disable_req_ei);
	if (rc >= 0)
		rc = qrtr_sendto(q->sock, q->node, q->port, out.data, (unsigned)out.data_len);
	s->log("%s late enable response created instance %u: disabling it%s", q->name, r.instance,
	       rc < 0 ? " failed" : "");
}

static void sam_handle_resp(struct server *s, struct sam *q, struct qrtr_packet *pkt, unsigned msg,
			    uint16_t txn)
{
	unsigned int t;
	int rc;

	if (!q->busy || txn != q->txn || msg != q->msg) {
		sn_vlog("%s <- stray response msg 0x%02x txn %u", q->name, msg, txn);
		if (msg == SNS_SAM_ENABLE)
			sam_stray_enable(s, q, pkt);
		return;
	}
	q->busy = false;
	switch (msg) {
	case SNS_SAM_GET_ATTR: {
		struct sns_sam_attr_resp r;

		memset(&r, 0, sizeof(r));
		q->attr_done = true;
		rc = qmi_decode_message(&r, &t, pkt, QMI_RESPONSE, msg, sns_sam_attr_resp_ei);
		if (rc < 0 || r.resp.result != SNS_RESULT_SUCCESS) {
			s->log("%s attributes: failed (decode %d, result %u err %u)", q->name, rc,
			       r.resp.result, r.resp.err);
			break;
		}
		s->log("%s attributes (TLV 0x03..0x0b): %" PRIu32 " %" PRIu32 " 0x%" PRIx32 " %" PRIu32
		       " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 "; suid %s0x%016" PRIx64
		       "; reserved %s%" PRIu32, q->name, r.attr[0], r.attr[1], r.attr[2], r.attr[3],
		       r.attr[4], r.attr[5], r.attr[6], r.attr[7], r.attr[8], r.suid_valid ? "" : "(absent) ",
		       r.suid, r.reserved_valid ? "" : "(absent) ", r.reserved);
		s->log("%s attributes read as: revision %" PRIu32 ", processor %" PRIu32
		       ", report modes 0x%" PRIx32 ", report rate %.4g..%.4g Hz, sample rate %.4g..%.4g Hz"
		       ", max batch %" PRIu32 ", power %.4g mA (rates/power as Q16)", q->name,
		       r.attr[0], r.attr[1], r.attr[2], r.attr[3] / 65536.0, r.attr[4] / 65536.0,
		       r.attr[5] / 65536.0, r.attr[6] / 65536.0, r.attr[7], r.attr[8] / 65536.0);
		break;
	}
	case SNS_SAM_ENABLE: {
		struct sns_sam_instance_resp r;

		memset(&r, 0, sizeof(r));
		rc = qmi_decode_message(&r, &t, pkt, QMI_RESPONSE, msg, sns_sam_instance_resp_ei);
		if (rc >= 0 && r.resp.result == SNS_RESULT_SUCCESS && r.instance_valid) {
			q->on = true;
			q->instance = r.instance;
			q->enables++;
			q->have = false;
			s->log("%s enabled: instance %u", q->name, r.instance);
		} else {
			q->failed = true;
			s->log("%s enable refused (decode %d, result %u err %u, instance %s); not retried until %s is released",
			       q->name, rc, r.resp.result, r.resp.err, r.instance_valid ? "given" : "missing",
			       s->ch[q->ch].name);
		}
		break;
	}
	case SNS_SAM_DISABLE: {
		struct sns_sam_instance_resp r;

		memset(&r, 0, sizeof(r));
		rc = qmi_decode_message(&r, &t, pkt, QMI_RESPONSE, msg, sns_sam_instance_resp_ei);
		if (rc < 0)
			s->log("%s disabled: instance %u (response did not decode: %d)", q->name,
			       q->instance, rc);
		else
			s->log("%s disabled: instance %u (result %u err %u)", q->name, q->instance,
			       r.resp.result, r.resp.err);
		q->on = false;
		q->have = false;
		break;
	}
	}
	sam_sync(s, q);
}

/* libqrtr does not enforce mandatory TLVs: 1 when TLVs first..last are
 * all present in the message (walked with bounds checks). */
static bool tlvs_present(const struct qrtr_packet *pkt, unsigned first, unsigned last)
{
	const uint8_t *p = (const uint8_t *)pkt->data + SNS_QMI_HDR_LEN;
	size_t left = pkt->data_len - SNS_QMI_HDR_LEN;
	uint32_t seen = 0, want = 0;
	unsigned k;

	for (k = first; k <= last && k < 32; k++)
		want |= 1u << k;
	while (left >= 3) {
		size_t len = (size_t)(p[1] | p[2] << 8);

		if (len > left - 3)
			return false;
		if (p[0] < 32)
			seen |= 1u << p[0];
		p += 3 + len;
		left -= 3 + len;
	}
	return (seen & want) == want;
}

/* Length of the first TLV of that type, or -1 (walked with bounds
 * checks). The codec accepts a struct TLV shorter than the struct (it
 * stops at the TLV's end), so fixed-size ones are checked here. */
static int tlv_len(const struct qrtr_packet *pkt, uint8_t type)
{
	const uint8_t *p = (const uint8_t *)pkt->data + SNS_QMI_HDR_LEN;
	size_t left = pkt->data_len - SNS_QMI_HDR_LEN;

	while (left >= 3) {
		size_t len = (size_t)(p[1] | p[2] << 8);

		if (len > left - 3)
			return -1;
		if (p[0] == type)
			return (int)len;
		p += 3 + len;
		left -= 3 + len;
	}
	return -1;
}

#define SNS_ROTVEC_RESULT_LEN	18	/* u32[4] + u8 + u8, packed */

static void sam_report_ind(struct server *s, struct sam *q, struct qrtr_packet *pkt, unsigned msg)
{
	unsigned int t;
	int rc;

	if (q->kind == SAM_QMAG) {
		struct sns_sam_qmag_ind ind;

		memset(&ind, 0, sizeof(ind));
		rc = qmi_decode_message(&ind, &t, pkt, QMI_INDICATION, (int)msg, sns_sam_qmag_ind_ei);
		if (rc < 0)
			s->log("qmag report indication did not decode (%d)", rc);
		else if (!tlvs_present(pkt, 0x01, 0x04))
			s->log("qmag report indication without all of TLVs 0x01..0x04, ignored");
		else
			qmag_report(s, &ind);
		return;
	}
	{
		struct sns_sam_rotvec_ind ind;

		memset(&ind, 0, sizeof(ind));
		rc = qmi_decode_message(&ind, &t, pkt, QMI_INDICATION, (int)msg, sns_sam_rotvec_ind_ei);
		if (rc < 0)
			s->log("rotvec report indication did not decode (%d)", rc);
		else if (!tlvs_present(pkt, 0x01, 0x03) || tlv_len(pkt, 0x03) != SNS_ROTVEC_RESULT_LEN)
			s->log("rotvec report indication without TLVs 0x01..0x03 or with a %d-byte result, ignored",
			       tlv_len(pkt, 0x03));
		else
			rotvec_report(s, &ind);
	}
}

void sn_sam_rx(struct server *s, struct sam *q, struct qrtr_packet *pkt, uint32_t node,
	       uint32_t port)
{
	const uint8_t *h = pkt->data;
	unsigned int msg, t;
	uint16_t txn;
	int rc;

	if (pkt->data_len < SNS_QMI_HDR_LEN || qmi_decode_header(pkt, &msg) < 0) {
		s->log("%s: malformed QMI message from %u:%u, dropped", q->name, node, port);
		return;
	}
	if (!q->port || node != q->node || port != q->port) {
		sn_vlog("%s socket: message from unexpected %u:%u, ignored", q->name, node, port);
		return;
	}
	txn = (uint16_t)(h[1] | h[2] << 8);
	if (h[0] == QMI_RESPONSE) {
		sn_vlog("%s <- resp 0x%02x txn %u (%zu bytes)", q->name, msg, txn, pkt->data_len);
		sam_handle_resp(s, q, pkt, msg, txn);
	} else if (h[0] == QMI_INDICATION && msg == SNS_SAM_REPORT_IND) {
		sam_report_ind(s, q, pkt, msg);
	} else if (h[0] == QMI_INDICATION && msg == SNS_SAM_ERROR_IND) {
		struct sns_sam_error_ind ind;

		memset(&ind, 0, sizeof(ind));
		rc = qmi_decode_message(&ind, &t, pkt, QMI_INDICATION, (int)msg, sns_sam_error_ind_ei);
		if (rc < 0 || !tlvs_present(pkt, 0x01, 0x02)) {
			s->log("%s error indication malformed (decode %d), ignored", q->name, rc);
		} else if (q->on && ind.instance == q->instance) {
			/* Counted and shown in status; what an error means for
			 * the instance is unknown, so it stays enabled. */
			q->errors++;
			q->last_error = ind.error;
			q->had_error = true;
			s->log("%s error indication: error %u for our instance %u", q->name, ind.error,
			       ind.instance);
		} else {
			s->log("%s error indication: error %u instance %u (not ours: %s%u)", q->name,
			       ind.error, ind.instance, q->on ? "" : "none enabled, last ", q->instance);
		}
	} else {
		sn_vlog("%s <- type %u msg 0x%02x ignored", q->name, h[0], msg);
	}
}

static int64_t sam_timeout_at(const struct sam *q)
{
	return q->sent + (q->msg == SNS_SAM_ENABLE ? SENSORD_QMAG_ENABLE_TIMEOUT_MS
						   : SENSORD_REQ_TIMEOUT_MS);
}

/* Resends and give-ups for the request in flight (ENABLE: never
 * resent; forgetting the port closes our socket, which tells the DSP
 * this client is gone); lookup retries. */
void sn_sam_tick(struct server *s, struct sam *q)
{
	int64_t now = s->now_ms();

	if (!q->opt)
		return;
	if (q->busy && now >= sam_timeout_at(q)) {
		if (q->tries >= SENSORD_REQ_TRIES || q->msg == SNS_SAM_ENABLE) {
			uint16_t msg = q->msg;

			s->log("%s msg 0x%02x txn %u: no response after %d tries, giving up", q->name,
			       msg, q->txn, q->tries);
			q->busy = false;
			if (msg == SNS_SAM_GET_ATTR) {
				q->attr_done = true;
			} else {
				if (msg == SNS_SAM_ENABLE)
					q->failed = true;
				sn_sam_forget(s, q, "no response");
			}
		} else {
			int rc;

			q->tries++;
			q->sent = now;
			s->log("%s msg 0x%02x txn %u: no response, resending (try %d)", q->name, q->msg,
			       q->txn, q->tries);
			rc = qrtr_sendto(q->sock, q->node, q->port, q->pkt.data,
					 (unsigned)q->pkt.data_len);
			if (rc < 0)
				sn_sam_forget(s, q, strerror(-rc));
		}
	}
	sam_sync(s, q);
}

/* When sn_sam_tick has something to do next, for the poll() timeout. */
int64_t sn_sam_deadline(const struct server *s, const struct sam *q, int64_t next)
{
	if (q->busy && sam_timeout_at(q) < next)
		next = sam_timeout_at(q);
	else if (!q->busy && !q->on && !q->port && sn_sam_wanted(s, q) && q->next_lookup < next)
		next = q->next_lookup;
	return next;
}
