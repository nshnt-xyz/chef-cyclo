/*
 * sensord's SMGR client (QMI service 0x100 on the ADSP): lookup and
 * liveness, sensor info, one report per used channel, the indications,
 * request resends. See main.c, section 2.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "common/linesrv.h"
#include "sensord.h"

static struct pending *pend_find(struct server *s, uint16_t txn)
{
	int i;

	for (i = 0; i < SENSORD_MAX_PENDING; i++)
		if (s->pend[i].used && s->pend[i].txn == txn)
			return &s->pend[i];
	return NULL;
}

void sn_smgr_reset(struct server *s, const char *why)
{
	int i;

	if (s->st != SMGR_DOWN)
		s->log("smgr %u:%u lost (%s); re-looking up service 0x%x", s->smgr_node,
		       s->smgr_port, why, SNS_SMGR_SVC);
	s->smgr_resets++;
	if (s->smgr_sock >= 0)
		qrtr_close(s->smgr_sock);
	s->smgr_sock = -1;
	s->st = SMGR_DOWN;
	s->smgr_node = s->smgr_port = 0;
	s->smgr_missing_logged = false;
	s->next_lookup = s->now_ms() + SENSORD_LOOKUP_MS;
	for (i = 0; i < SENSORD_MAX_PENDING; i++)
		s->pend[i].used = false;
	for (i = 0; i < CH_SMGR_COUNT; i++) {
		s->ch[i].rate = 0;
		s->ch[i].busy = false;
	}
	/* no samples until the reports are back: start the compass afresh;
	 * the ADSP may have restarted and lost its learned bias, so the live
	 * calibration state is learned again */
	compass_reset(&s->heading.c);
	compass_magcal_init(&s->heading.mc);
	sn_cal_apply(s, "smgr lost");
	/* The SAM services are separate: disable them while SMGR is away
	 * (a no-op without -Q/-V, or if the DSP took them down too). */
	sn_sam_sync_all(s);
}

static int smgr_send(struct server *s, uint16_t msg, struct qmi_elem_info *ei, const void *req,
		     int ch, uint8_t sensor_id, unsigned rate)
{
	struct pending *p = NULL;
	ssize_t len;
	int i, rc;

	for (i = 0; i < SENSORD_MAX_PENDING; i++)
		if (!s->pend[i].used) {
			p = &s->pend[i];
			break;
		}
	if (!p) {
		s->log("too many SMGR requests in flight, dropping msg 0x%02x", msg);
		return -EBUSY;
	}
	memset(p, 0, sizeof(*p));
	p->pkt.data = p->buf;
	p->pkt.data_len = sizeof(p->buf);
	s->txn = s->txn == 0xffff ? 1 : s->txn + 1;
	len = qmi_encode_message(&p->pkt, QMI_REQUEST, msg, s->txn, req, ei);
	if (len < 0) {
		s->log("encode SMGR msg 0x%02x failed: %s", msg, strerror((int)-len));
		return (int)len;
	}
	p->used = true;
	p->txn = s->txn;
	p->msg = msg;
	p->ch = ch;
	p->sensor_id = sensor_id;
	p->rate = rate;
	p->sent = s->now_ms();
	p->tries = 1;
	sn_vlog("smgr -> msg 0x%02x txn %u (%zd bytes)", msg, p->txn, len);
	rc = qrtr_sendto(s->smgr_sock, s->smgr_node, s->smgr_port, p->pkt.data,
			 (unsigned)p->pkt.data_len);
	if (rc < 0) {
		sn_smgr_reset(s, strerror(-rc));
		return rc;
	}
	return 0;
}

static void smgr_request_info(struct server *s)
{
	if (s->info_next >= s->ninfo) {
		int i, n = 0;

		for (i = 0; i < CH_SMGR_COUNT; i++)
			n += s->ch[i].present && !s->ch[i].internal;
		s->ch[CH_HEADING].present = s->ch[CH_ACCEL].present && s->ch[CH_ANGLVEL].present &&
					    s->ch[CH_MAGN].present;
		s->st = SMGR_READY;
		s->log("smgr ready: %d sensor(s), %d of %d channels present", s->ninfo, n, CH_SMGR_COUNT - 1);
		sn_smgr_sync(s);
		return;
	}
	{
		struct sns_smgr_single_info_req req = { .sensor_id = s->info_ids[s->info_next] };

		smgr_send(s, SNS_SMGR_SINGLE_INFO, sns_smgr_single_info_req_ei, &req, -1,
			  req.sensor_id, 0);
	}
}

static void smgr_found(struct server *s, uint32_t node, uint32_t port)
{
	s->smgr_node = node;
	s->smgr_port = port;
	s->st = SMGR_INFO;
	s->ninfo = s->info_next = 0;
	s->next_check = s->now_ms() + SENSORD_CHECK_MS;
	s->log("smgr service 0x%x found at %u:%u, reading sensor info", SNS_SMGR_SVC, node, port);
	smgr_send(s, SNS_SMGR_ALL_INFO, sns_empty_ei, NULL, -1, 0, 0);
}

/* Add, change or delete the DSP report for one channel. */
void sn_smgr_report(struct server *s, int c, unsigned rate)
{
	struct channel *ch = &s->ch[c];
	uint8_t report_id = (uint8_t)(s->report_base + c + 1);
	int rc;

	if (s->periodic) {
		struct sns_smgr_rep_req req;

		memset(&req, 0, sizeof(req));
		req.report_id = report_id;
		req.action = rate ? SNS_SMGR_ACTION_ADD : SNS_SMGR_ACTION_DELETE;
		req.report_rate_hz = (uint16_t)rate;
		if (rate) {
			req.n_items = 1;
			req.item[0].sensor_id = ch->smgr_id;
			req.item[0].data_type = ch->dt;
			req.item[0].decimation = SNS_SMGR_DECIMATION_DEFAULT;
		}
		rc = smgr_send(s, SNS_SMGR_REPORT, sns_smgr_rep_req_ei, &req, c, 0, rate);
	} else {
		struct sns_smgr_buf_req req;
		unsigned report_hz = rate;

		if (s->report_cap && report_hz > s->report_cap)
			report_hz = s->report_cap;
		memset(&req, 0, sizeof(req));
		req.report_id = report_id;
		req.action = rate ? SNS_SMGR_ACTION_ADD : SNS_SMGR_ACTION_DELETE;
		req.report_rate_q16 = (uint32_t)report_hz << 16;
		if (rate) {
			req.n_items = 1;
			req.item[0].sensor_id = ch->smgr_id;
			req.item[0].data_type = ch->dt;
			req.item[0].decimation = SNS_SMGR_DECIMATION_DEFAULT;
			req.item[0].calibration = ch->cal;
			req.item[0].sampling_rate_hz = (uint16_t)rate;
			req.item[0].sample_quality = SNS_SMGR_SAMPLE_QUALITY_DEFAULT;
			req.notify_valid = 1;	/* apps processor, no wakeups in suspend */
		}
		rc = smgr_send(s, SNS_SMGR_BUFFERING, sns_smgr_buf_req_ei, &req, c, 0, rate);
	}
	if (rc == 0) {
		ch->busy = true;
		ch->want_sent = rate;
		s->log("%s report %u: %s %u Hz%s%s", rate ? (ch->rate ? "change" : "add") : "delete",
		       report_id, ch->name, rate ? rate : ch->rate,
		       rate && !s->periodic ? ", calibration " : "",
		       rate && !s->periodic ? sn_cal_name(ch->cal) : "");
	}
}


/* Bring every channel's DSP report in line with its claims. */
void sn_smgr_sync(struct server *s)
{
	int c;

	sn_heading_sync(s);
	for (c = 0; c < CH_SMGR_COUNT && s->st == SMGR_READY; c++) {
		struct channel *ch = &s->ch[c];
		unsigned want = sn_channel_want(s, c);

		if (ch->busy || want == ch->rate)
			continue;
		if (want && want == ch->failed)
			continue;
		if (want && !ch->present && !s->force)
			continue;
		sn_smgr_report(s, c, want);
	}
	sn_sam_sync_all(s);
}

/* An error about channel c to its claimants, and to the heading's when
 * c is one of its inputs. */
void sn_notify_claimants(struct server *s, int c, const char *err)
{
	int i;

	for (i = 0; i < SENSORD_MAX_CLIENTS; i++) {
		struct client *cl = &s->clients[i];

		if (cl->lc && cl->rate[c])
			linesrv_sendf(cl->lc, "{\"type\":\"error\",\"reason\":\"%s\",\"sensor\":\"%s\"}\n", err,
				      s->ch[c].name);
		if (cl->lc && cl->rate[CH_HEADING] && sn_internal_want(s, c) && !s->ch[c].internal)
			linesrv_sendf(cl->lc, "{\"type\":\"error\",\"reason\":\"%s\",\"sensor\":\"heading\",\"input\":\"%s\"}\n",
				      err, s->ch[c].name);
	}
}

static void smgr_report_done(struct server *s, struct pending *p, bool ok, unsigned ack,
			     const struct sns_resp *r, uint32_t nreasons,
			     const struct sns_smgr_reason *reasons)
{
	struct channel *ch = &s->ch[p->ch];
	uint32_t i;

	ch->busy = false;
	for (i = 0; i < nreasons && i < 10; i++)
		s->log("%s report reason: item %u reason %u", ch->name, reasons[i].item,
		       reasons[i].reason);
	if (ok) {
		ch->rate = p->rate;
		ch->failed = 0;
		if (ack)
			s->log("%s report accepted with changes (ack %u)", ch->name, ack);
	} else {
		s->log("%s report %s %u Hz refused: result %u err %u ack %u", ch->name,
		       p->rate ? "add" : "delete", p->rate, r->result, r->err, ack);
		if (p->rate) {
			ch->failed = p->rate;
			sn_notify_claimants(s, p->ch, "smgr refused report");
		} else {
			ch->rate = 0;	/* nothing more we can do about it */
		}
	}
	sn_smgr_sync(s);
}

static void smgr_handle_resp(struct server *s, struct qrtr_packet *pkt, unsigned msg,
			     uint16_t txn)
{
	struct pending *p = pend_find(s, txn);
	unsigned int t;
	int rc;

	if (!p || p->msg != msg) {
		sn_vlog("smgr <- stray response msg 0x%02x txn %u", msg, txn);
		return;
	}
	p->used = false;

	switch (msg) {
	case SNS_SMGR_ALL_INFO: {
		struct sns_smgr_all_info_resp r;
		uint32_t i;

		memset(&r, 0, sizeof(r));
		rc = qmi_decode_message(&r, &t, pkt, QMI_RESPONSE, msg, sns_smgr_all_info_resp_ei);
		if (rc < 0 || r.resp.result != SNS_RESULT_SUCCESS) {
			s->log("all-sensor-info failed (decode %d, result %u err %u)", rc,
			       r.resp.result, r.resp.err);
			if (s->force) {
				s->st = SMGR_READY;
				s->log("smgr ready without sensor info (-F)");
				sn_smgr_sync(s);
			} else {
				sn_smgr_reset(s, "no sensor info");
			}
			return;
		}
		s->ninfo = 0;
		for (i = 0; i < r.n && i < SNS_SMGR_MAX_SENSORS; i++) {
			char name[SNS_SMGR_SHORT_NAME + 1];

			sn_copy_name(name, sizeof(name), r.info[i].name, r.info[i].name_len);
			s->log("smgr sensor id %u '%s'", r.info[i].sensor_id, name);
			s->info_ids[s->ninfo++] = r.info[i].sensor_id;
		}
		s->info_next = 0;
		smgr_request_info(s);
		return;
	}
	case SNS_SMGR_SINGLE_INFO: {
		struct sns_smgr_single_info_resp r;
		uint32_t i;
		int c;

		memset(&r, 0, sizeof(r));
		rc = qmi_decode_message(&r, &t, pkt, QMI_RESPONSE, msg, sns_smgr_single_info_resp_ei);
		if (rc < 0 || r.resp.result != SNS_RESULT_SUCCESS)
			s->log("sensor-info for id %u failed (decode %d, result %u err %u)",
			       p->sensor_id, rc, r.resp.result, r.resp.err);
		for (i = 0; rc >= 0 && i < r.n && i < SNS_SMGR_MAX_DT; i++) {
			const struct sns_smgr_dt_info *d = &r.dt[i];
			char name[SNS_SMGR_NAME + 1], vendor[SNS_SMGR_VENDOR + 1];

			sn_copy_name(name, sizeof(name), d->name, d->name_len);
			sn_copy_name(vendor, sizeof(vendor), d->vendor, d->vendor_len);
			s->log("smgr id %u dt %u '%s' (%s) v%u max %u Hz range %.4g res %.4g",
			       d->sensor_id, d->data_type, name, vendor, d->version, d->max_rate_hz,
			       d->max_range / 65536.0, d->resolution / 65536.0);
			for (c = 0; c < CH_SMGR_COUNT; c++) {
				struct channel *ch = &s->ch[c];

				if (ch->smgr_id != d->sensor_id || ch->dt != d->data_type)
					continue;
				ch->present = true;
				memcpy(ch->hw, name, sizeof(ch->hw));
				memcpy(ch->vendor, vendor, sizeof(ch->vendor));
				ch->max_hz = d->max_rate_hz;
				ch->range = d->max_range / 65536.0;
				ch->resolution = d->resolution / 65536.0;
			}
		}
		s->info_next++;
		smgr_request_info(s);
		return;
	}
	case SNS_SMGR_BUFFERING: {
		struct sns_smgr_buf_resp r;
		bool ok;

		memset(&r, 0, sizeof(r));
		rc = qmi_decode_message(&r, &t, pkt, QMI_RESPONSE, msg, sns_smgr_buf_resp_ei);
		ok = rc >= 0 && r.resp.result == SNS_RESULT_SUCCESS &&
		     (!r.ack_nak_valid || r.ack_nak <= 1);
		if (rc < 0)
			s->log("buffering response did not decode (%d)", rc);
		smgr_report_done(s, p, ok, r.ack_nak_valid ? r.ack_nak : 0, &r.resp,
				 r.reasons_valid ? r.n_reasons : 0, r.reasons);
		return;
	}
	case SNS_SMGR_REPORT: {
		struct sns_smgr_rep_resp r;
		bool ok;

		memset(&r, 0, sizeof(r));
		rc = qmi_decode_message(&r, &t, pkt, QMI_RESPONSE, msg, sns_smgr_rep_resp_ei);
		ok = rc >= 0 && r.resp.result == SNS_RESULT_SUCCESS && r.ack_nak <= 1;
		if (rc < 0)
			s->log("report response did not decode (%d)", rc);
		smgr_report_done(s, p, ok, r.ack_nak, &r.resp, r.n_reasons, r.reasons);
		return;
	}
	}
}

static int channel_by_report(struct server *s, uint8_t report_id, uint8_t sensor_id, uint8_t dt)
{
	int c = (int)report_id - (int)s->report_base - 1;

	if (c >= 0 && c < CH_SMGR_COUNT && s->ch[c].smgr_id == sensor_id && s->ch[c].dt == dt)
		return c;
	for (c = 0; c < CH_SMGR_COUNT; c++)
		if (s->ch[c].smgr_id == sensor_id && s->ch[c].dt == dt)
			return c;
	return -1;
}

static void smgr_handle_buf_ind(struct server *s, struct qrtr_packet *pkt)
{
	static struct sns_smgr_buf_ind ind;	/* 1.7 KB, keep it off the stack */
	uint32_t a_ticks, newest = 0, i, k;
	int64_t a_ns;
	unsigned int t;
	int rc;

	memset(&ind, 0, sizeof(ind));
	rc = qmi_decode_message(&ind, &t, pkt, QMI_INDICATION, SNS_SMGR_BUFFERING_IND,
				sns_smgr_buf_ind_ei);
	if (rc < 0) {
		s->log("buffering indication did not decode (%d)", rc);
		return;
	}
	/* Stamps per index: first_ts plus the running sum of offsets,
	 * including the first sample's own offset (as the stock HAL). */
	for (i = 0; i < ind.n_index && i < SNS_SMGR_BUF_ITEMS; i++) {
		uint32_t ts = ind.index[i].first_ts;

		for (k = 0; k < ind.index[i].count; k++) {
			uint32_t j = ind.index[i].first + k;

			if (j >= ind.n_samples)
				break;
			ts += ind.samples[j].ts_offset;
			if (!newest || (int32_t)(ts - newest) > 0)
				newest = ts;
		}
	}
	sn_time_anchor(s, newest, &a_ticks, &a_ns);
	for (i = 0; i < ind.n_index && i < SNS_SMGR_BUF_ITEMS; i++) {
		const struct sns_smgr_buf_index *x = &ind.index[i];
		int c = channel_by_report(s, ind.report_id, x->sensor_id, x->data_type);
		uint32_t ts = x->first_ts;

		if (c < 0) {
			sn_vlog("smgr <- samples for unknown sensor %u dt %u", x->sensor_id, x->data_type);
			continue;
		}
		for (k = 0; k < x->count; k++) {
			uint32_t j = x->first + k;

			if (j >= ind.n_samples)
				break;
			ts += ind.samples[j].ts_offset;
			sn_server_publish(s, c, sn_ticks_to_ns(ts, a_ticks, a_ns), ind.samples[j].data);
		}
	}
}

static void smgr_handle_rep_ind(struct server *s, struct qrtr_packet *pkt)
{
	struct sns_smgr_rep_ind ind;
	uint32_t a_ticks, newest = 0, i;
	int64_t a_ns;
	unsigned int t;
	int rc;

	memset(&ind, 0, sizeof(ind));
	rc = qmi_decode_message(&ind, &t, pkt, QMI_INDICATION, SNS_SMGR_REPORT_IND,
				sns_smgr_rep_ind_ei);
	if (rc < 0) {
		s->log("report indication did not decode (%d)", rc);
		return;
	}
	for (i = 0; i < ind.n_items && i < SNS_SMGR_REP_ITEMS; i++)
		if (!newest || (int32_t)(ind.item[i].ts - newest) > 0)
			newest = ind.item[i].ts;
	sn_time_anchor(s, newest, &a_ticks, &a_ns);
	for (i = 0; i < ind.n_items && i < SNS_SMGR_REP_ITEMS; i++) {
		int c = channel_by_report(s, ind.report_id, ind.item[i].sensor_id,
					  ind.item[i].data_type);

		if (c >= 0)
			sn_server_publish(s, c, sn_ticks_to_ns(ind.item[i].ts, a_ticks, a_ns),
				       ind.item[i].data);
	}
}

void sn_smgr_rx(struct server *s, struct qrtr_packet *pkt, uint32_t node, uint32_t port)
{
	const uint8_t *h = pkt->data;
	unsigned int msg;
	uint16_t txn;

	if (pkt->data_len < SNS_QMI_HDR_LEN || qmi_decode_header(pkt, &msg) < 0) {
		s->log("malformed QMI message from %u:%u, dropped", node, port);
		return;
	}
	if (node != s->smgr_node || port != s->smgr_port) {
		sn_vlog("smgr socket: message from unexpected %u:%u, ignored", node, port);
		return;
	}
	txn = (uint16_t)(h[1] | h[2] << 8);
	if (h[0] == QMI_RESPONSE) {
		sn_vlog("smgr <- resp 0x%02x txn %u (%zu bytes)", msg, txn, pkt->data_len);
		smgr_handle_resp(s, pkt, msg, txn);
	} else if (h[0] == QMI_INDICATION && msg == SNS_SMGR_BUFFERING_IND) {
		smgr_handle_buf_ind(s, pkt);
	} else if (h[0] == QMI_INDICATION && msg == SNS_SMGR_REPORT_IND) {
		smgr_handle_rep_ind(s, pkt);
	} else {
		sn_vlog("smgr <- type %u msg 0x%02x ignored", h[0], msg);
	}
}

/* Find a DSP service instance (SMGR 0x100 or QMAG_CAL 0x140, both
 * instance 0x3201). msmipc_lookup() always sends lookup_mask 0, and the
 * router matches a server only when (instance & mask) == requested
 * instance (kernel/net/ipc_router/ipc_router_core.c, the LOOKUP_SERVER
 * handler), so the only instance that works is 0 = "all instances of the
 * service"; pick the wanted one out of that list here. Logs the instance
 * set whenever it changes (other instances would be a surprise worth
 * seeing); seen[] holds the last logged set.
 * Returns 1 with *out filled, 0 if not registered, -1 on error. */
int sn_svc_lookup(struct server *s, int sock, uint32_t svc, uint32_t want, char *seen_prev,
		  size_t seen_sz, struct msm_ipc_server_info *out)
{
	struct msm_ipc_server_info info[8];
	char seen[160];
	size_t o = 0;
	int n, k, hit = -1;

	n = msmipc_lookup(sock, svc, 0, info, 8);
	if (n < 0)
		return -1;
	seen[0] = '\0';
	for (k = 0; k < n; k++) {
		if (info[k].instance == want && hit < 0)
			hit = k;
		if (o < sizeof(seen))
			o += (size_t)snprintf(seen + o, sizeof(seen) - o, "%s0x%x@%u:%u", k ? " " : "",
					      info[k].instance, info[k].node_id, info[k].port_id);
	}
	if (strcmp(seen, seen_prev)) {
		snprintf(seen_prev, seen_sz, "%s", seen);
		s->log("lookup 0x%x (all instances): %s", svc, n ? seen : "none");
	}
	if (hit < 0)
		return 0;
	*out = info[hit];
	return 1;
}

static int smgr_lookup(struct server *s, struct msm_ipc_server_info *out)
{
	return sn_svc_lookup(s, s->smgr_sock, SNS_SMGR_SVC, (SNS_SMGR_INST << 8) | SNS_SMGR_VERS,
			  s->smgr_seen, sizeof(s->smgr_seen), out);
}

void sn_smgr_tick(struct server *s)
{
	int64_t now = s->now_ms();
	int i;

	if (s->st == SMGR_DOWN && now >= s->next_lookup) {
		struct msm_ipc_server_info info;
		int n;

		s->next_lookup = now + SENSORD_LOOKUP_MS;
		if (s->smgr_sock < 0) {
			s->smgr_sock = qrtr_open(0);
			if (s->smgr_sock < 0) {
				s->log("smgr socket: %s", strerror(errno));
				return;
			}
		}
		n = smgr_lookup(s, &info);
		if (n > 0) {
			smgr_found(s, info.node_id, info.port_id);
		} else if (!s->smgr_missing_logged) {
			s->smgr_missing_logged = true;
			s->log("smgr service 0x%x not registered yet, polling every %d ms",
			       SNS_SMGR_SVC, SENSORD_LOOKUP_MS);
		}
		return;
	}
	if (s->st != SMGR_DOWN && now >= s->next_check) {
		struct msm_ipc_server_info info;
		int n, same;

		s->next_check = now + SENSORD_CHECK_MS;
		n = smgr_lookup(s, &info);
		same = n > 0 && info.node_id == s->smgr_node && info.port_id == s->smgr_port;
		if (!same) {
			sn_smgr_reset(s, n > 0 ? "service moved" : "service gone");
			return;
		}
	}
	for (i = 0; i < SENSORD_MAX_PENDING; i++) {
		struct pending *p = &s->pend[i];
		int rc;

		if (!p->used || now - p->sent < SENSORD_REQ_TIMEOUT_MS)
			continue;
		if (p->tries >= SENSORD_REQ_TRIES) {
			char why[48];

			snprintf(why, sizeof(why), "no response to msg 0x%02x", p->msg);
			sn_smgr_reset(s, why);
			return;
		}
		p->tries++;
		p->sent = now;
		s->log("smgr msg 0x%02x txn %u: no response, resending (try %d)", p->msg, p->txn,
		       p->tries);
		rc = qrtr_sendto(s->smgr_sock, s->smgr_node, s->smgr_port, p->pkt.data,
				 (unsigned)p->pkt.data_len);
		if (rc < 0) {
			sn_smgr_reset(s, strerror(-rc));
			return;
		}
	}
}
