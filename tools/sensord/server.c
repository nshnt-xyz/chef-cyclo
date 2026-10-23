/*
 * sensord's clients and the socket protocol: the claim bookkeeping, the
 * list and status lines, requests, delivery of samples to their claimants
 * and the get timeouts. The daemon as a whole is described in main.c.
 *
 * Socket API, /run/chef/sensord/sensord.sock: requests are words, one
 * line each; every line sensord sends is one JSON object with a "type"
 * member (the shared envelope of docs/features/local-sockets.md, served
 * by tools/common/linesrv.c), proto 2, written down in
 * docs/features/sensors.md "Socket contract":
 *
 *   (connect)          {"type":"hello","daemon":"sensord","proto":2}
 *   list               {"type":"list","smgr":"ready","sensors":[...]}
 *   status             {"type":"status","smgr":"ready",...}
 *   claim SENSOR [HZ]  {"type":"ok","cmd":"claim","sensor":"accel","rate":50},
 *                      then {"type":"sample","sensor":"accel","t":NS,"x":..,
 *                      "y":..,"z":..} (illuminance, proximity, heading and
 *                      rotvec lines the same way, "type":"sample" first)
 *   release SENSOR     {"type":"ok","cmd":"release","sensor":"accel"}
 *   get SENSOR [HZ]    one sample line, or {"type":"error","cmd":"get",
 *                      "reason":"timeout","sensor":"accel"} after 5 s
 *   declination [DEG|off]
 *                      {"type":"ok","cmd":"declination","declination":1.50}
 *                      (null when off)
 *   errors             {"type":"error","cmd":"claim","reason":"unknown
 *                      sensor","sensor":"x"} (also get, release; the name
 *                      as sent, its first 32 bytes, JSON-escaped), "sensor
 *                      not present", "bad rate", "not enabled (sensord -V)"
 *                      (rotvec), "bad declination", "bad arguments" (list
 *                      and status take no word, claim and get 1 or 2,
 *                      release 1, declination 0 or 1), {"type":"error",
 *                      "cmd":"x","reason":"unknown command"} (the word as
 *                      above), and from the library "line too long" (over
 *                      127 bytes) and "too many clients" (the 17th
 *                      connection, then closed)
 *   events             {"type":"error","reason":"smgr refused report",
 *                      "sensor":"accel"} to its claimants when the DSP
 *                      refuses a report later (no cmd: it answers no
 *                      request; a heading claimant gets "sensor":"heading",
 *                      "input":"magn")
 *
 * A sensor is powered (report added on the DSP) only while at least one
 * connection claims it; a claim lives on its connection. Each client has
 * a 16384-byte output queue: sample lines are lossy (a slow reader loses
 * them, never stalls the daemon, and is told how many with
 * {"type":"dropped","count":N} before the next one that fits), every
 * other line must be delivered (a client that lets one overflow is
 * dropped, its claims with it).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "common/chef_log.h"
#include "common/json.h"
#include "common/linesrv.h"
#include "sensord.h"

void sn_server_init(struct server *s)
{
	memset(s, 0, sizeof(*s));
	memcpy(s->ch, sn_channel_defaults, sizeof(s->ch));
	s->serve_reg2 = true;
	s->dsps_fd = -1;
	s->reg_sock = s->time_sock = s->smgr_sock = -1;
	s->qmag = (struct sam){ .name = "qmag", .kind = SAM_QMAG, .svc = SNS_QMAG_SVC,
				.inst = (SNS_QMAG_INST << 8) | SNS_QMAG_VERS, .ch = CH_MAGN, .sock = -1 };
	s->rotvec = (struct sam){ .name = "rotvec", .kind = SAM_ROTVEC, .svc = SNS_ROTVEC_SVC,
				  .inst = (SNS_ROTVEC_INST << 8) | SNS_ROTVEC_VERS, .ch = CH_ROTVEC,
				  .sock = -1 };
	s->heading.mount = COMPASS_PORTRAIT;
	compass_init(&s->heading.c, COMPASS_PORTRAIT);
	s->st = SMGR_DOWN;
	s->now_ms = sn_real_now_ms;
	s->log = chef_log;
	s->elog = sn_stderr_note;
}

int sn_channel_claims(const struct server *s, int c)
{
	int i, n = 0;

	for (i = 0; i < SENSORD_MAX_CLIENTS; i++)
		if (s->clients[i].lc && s->clients[i].rate[c])
			n++;
	return n;
}

/* What sensord itself wants on an SMGR channel: the heading's inputs
 * while heading is claimed. */
unsigned sn_internal_want(const struct server *s, int c)
{
	if (!sn_channel_claims(s, CH_HEADING))
		return 0;
	switch (c) {
	case CH_ACCEL:
	case CH_ANGLVEL:
		return SENSORD_HEADING_IMU_HZ;
	case CH_MAGN:
		return SENSORD_HEADING_MAG_HZ;
	case CH_MAGF:
		/* only meaningful against a full-calibration magn */
		return s->ch[CH_MAGN].cal == SNS_SMGR_CAL_FULL ? SENSORD_HEADING_FAC_HZ : 0;
	case CH_ROTVEC:
		/* with -V the heading lines carry the ADSP's accuracy too */
		return s->rotvec.opt ? SENSORD_ROTVEC_HZ : 0;
	}
	return 0;
}

/* Highest rate any client (or the heading, internally) wants on a
 * channel, clamped to what the sensor reports it can do; 0 when nobody
 * wants it. */
unsigned sn_channel_want(const struct server *s, int c)
{
	const struct channel *ch = &s->ch[c];
	unsigned want = sn_internal_want(s, c), max = ch->max_hz ? ch->max_hz : 200;
	int i;

	for (i = 0; i < SENSORD_MAX_CLIENTS; i++)
		if (s->clients[i].lc && s->clients[i].rate[c] > want)
			want = s->clients[i].rate[c];
	if (want > max)
		want = max;
	return want;
}

/* Clients plus sensord's own use (heading inputs). */
int sn_channel_used(const struct server *s, int c)
{
	return sn_channel_claims(s, c) + (sn_internal_want(s, c) ? 1 : 0);
}

/* A refused rate is retried only once its channel was idle. */
static void channels_idle_reset(struct server *s)
{
	int i;

	for (i = 0; i < CH_SMGR_COUNT; i++)
		if (!sn_channel_used(s, i))
			s->ch[i].failed = 0;
}

static struct client *client_of(struct server *s, struct linesrv_client *lc)
{
	return &s->clients[linesrv_slot(lc)];
}

static void on_connect(struct linesrv *srv, struct linesrv_client *lc)
{
	struct server *s = linesrv_ctx(srv);
	struct client *c = client_of(s, lc);
	int sndbuf = SENSORD_OUTQ;

	memset(c, 0, sizeof(*c));
	c->lc = lc;
	/* Keep what a stalled client can have in flight bounded in the
	 * kernel too, not just in the output queue. */
	setsockopt(linesrv_fd(lc), SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
	linesrv_sendf(lc, "{\"type\":\"hello\",\"daemon\":\"sensord\",\"proto\":%d}\n", SENSORD_PROTO);
}

/* Exactly once per connection, whatever ended it: its claims go. Inside a
 * loop over the claimants the reports are synced once the loop is over
 * (sn_deliver's caller), and not at all when sensord is exiting. */
static void on_close(struct linesrv *srv, struct linesrv_client *lc)
{
	struct server *s = linesrv_ctx(srv);
	struct client *c = client_of(s, lc);
	int i;

	if (s->closing) {
		memset(c, 0, sizeof(*c));
		return;
	}
	for (i = 0; i < CH_COUNT; i++)
		if (c->rate[i] && !c->get_only[i])
			s->log("release %s (client gone)", s->ch[i].name);
	memset(c, 0, sizeof(*c));
	channels_idle_reset(s);
	if (s->delivering)
		s->sync_pending = true;
	else
		sn_smgr_sync(s);
}

static void on_line(struct linesrv *srv, struct linesrv_client *lc, int argc, char **argv);

int sn_server_start(struct server *s)
{
	struct linesrv_config cfg = {
		.max_clients = SENSORD_MAX_CLIENTS,
		.line_max = SENSORD_LINE_MAX,
		.outq_size = SENSORD_OUTQ,
		.on_connect = on_connect,
		.on_line = on_line,
		.on_close = on_close,
		.ctx = s,
		.log = s->log,
	};

	s->srv = linesrv_new(&cfg);
	return s->srv ? 0 : -ENOMEM;
}

void sn_server_drop_all(struct server *s)
{
	int i;

	s->closing = true;
	for (i = 0; s->srv && i < SENSORD_MAX_CLIENTS; i++)
		if (s->clients[i].lc)
			linesrv_drop(s->clients[i].lc);
}

void sn_server_fini(struct server *s)
{
	if (!s->srv)
		return;
	s->closing = true;
	linesrv_close_all(s->srv);
	s->srv = NULL;
	s->closing = false;
}

struct client *sn_server_add_client(struct server *s, int fd)
{
	struct linesrv_client *lc = linesrv_adopt(s->srv, fd);

	return lc ? client_of(s, lc) : NULL;
}

/* Sample lines may be lost: one that does not fit is counted, and the
 * library sends the count ahead of the next one that fits. */
static void client_sample(struct client *c, const char *line, size_t n)
{
	if (linesrv_send_lossy(c->lc, line, n) == 1)
		c->dropped_total++;
}

void sn_server_list_line(const struct server *s, char *buf, size_t len)
{
	static const char *st[] = { "down", "info", "ready" };
	size_t off;
	int i;

	off = (size_t)snprintf(buf, len, "{\"type\":\"list\",\"smgr\":\"%s\",\"sensors\":[", st[s->st]);
	for (i = 0; i < CH_SMGR_COUNT && off < len; i++) {
		const struct channel *ch = &s->ch[i];
		char hw[6 * sizeof(ch->hw)], vendor[6 * sizeof(ch->vendor)];

		if (ch->internal)
			continue;
		json_escape(hw, sizeof(hw), ch->hw);
		json_escape(vendor, sizeof(vendor), ch->vendor);
		off += (size_t)snprintf(buf + off, len - off,
			"%s{\"sensor\":\"%s\",\"unit\":\"%s\",\"smgr_id\":%u,\"data_type\":%u,"
			"\"present\":%s,\"name\":\"%s\",\"vendor\":\"%s\",\"max_hz\":%u,"
			"\"range\":%.6g,\"resolution\":%.6g,\"rate\":%u,\"claims\":%d,"
			"\"calibration\":\"%s\"}",
			i ? "," : "", ch->name, ch->unit, ch->smgr_id, ch->dt,
			ch->present ? "true" : "false", hw, vendor, ch->max_hz,
			ch->range, ch->resolution, ch->rate, sn_channel_claims(s, i), sn_cal_name(ch->cal));
	}
	if (off < len)
		off += (size_t)snprintf(buf + off, len - off,
			",{\"sensor\":\"heading\",\"unit\":\"deg\",\"virtual\":true,"
			"\"inputs\":\"accel %d Hz, anglvel %d Hz, magn %d Hz\",\"present\":%s,"
			"\"max_hz\":%u,\"rate\":%u,\"claims\":%d,\"mount\":\"%s\",\"calibrated\":%s}",
			SENSORD_HEADING_IMU_HZ, SENSORD_HEADING_IMU_HZ, SENSORD_HEADING_MAG_HZ,
			s->ch[CH_HEADING].present ? "true" : "false", s->ch[CH_HEADING].max_hz,
			sn_channel_want(s, CH_HEADING), sn_channel_claims(s, CH_HEADING),
			compass_mount_name(s->heading.mount), s->heading.calibrated ? "true" : "false");
	if (off < len)
		off += (size_t)snprintf(buf + off, len - off,
			",{\"sensor\":\"rotvec\",\"unit\":\"quaternion\",\"virtual\":true,"
			"\"present\":%s,\"max_hz\":%u,\"rate\":%u,\"claims\":%d,\"state\":\"%s\"}",
			s->rotvec.opt ? "true" : "false", s->ch[CH_ROTVEC].max_hz,
			s->rotvec.opt && sn_channel_claims(s, CH_ROTVEC) ? SENSORD_ROTVEC_HZ : 0,
			sn_channel_claims(s, CH_ROTVEC), sn_sam_state_name(&s->rotvec));
	if (off < len)
		snprintf(buf + off, len - off, "]}\n");
}


void sn_server_status_line(const struct server *s, char *buf, size_t len)
{
	static const char *st[] = { "down", "info", "ready" };
	const struct sam *q = &s->qmag, *r = &s->rotvec;
	char qb[192] = "";
	int i, nc = 0;
	size_t off;

	for (i = 0; i < SENSORD_MAX_CLIENTS; i++)
		if (s->clients[i].lc)
			nc++;
	if (q->have) {
		qb[0] = ',';
		sn_format_qmag_fields(q, qb + 1, sizeof(qb) - 1);
	}
	off = (size_t)snprintf(buf, len,
		 "{\"type\":\"status\",\"smgr\":\"%s\",\"smgr_node\":%u,\"smgr_port\":%u,"
		 "\"smgr_resets\":%lu,\"api\":\"%s\",\"reg2\":%s,\"reg2_reads\":%lu,"
		 "\"reg2_writes\":%lu,\"reg2_misses\":%lu,\"reg2_saves\":%lu,\"reg2_save_errors\":%lu,"
		 "\"time2\":%s,\"time2_reqs\":%lu,"
		 "\"unknown_reqs\":%lu,\"ts\":\"%s\",\"clients\":%d",
		 st[s->st], s->smgr_node, s->smgr_port, s->smgr_resets,
		 s->periodic ? "report" : "buffering",
		 s->serve_reg2 ? "true" : "false", s->reg.reads, s->reg.writes, s->reg.misses,
		 s->reg_saves, s->reg_save_errors,
		 s->serve_time2 ? "true" : "false", s->time2_reqs, s->unknown_reqs,
		 s->ts_mode == TS_DSPS ? "dsps" : s->ts_mode == TS_CNTVCT ? "cntvct" : "rx", nc);
	if (off < len)
		off += (size_t)sn_format_heading_status(s, buf + off, len - off);
	if (off < len)
		off += (size_t)snprintf(buf + off, len - off,
			 ",\"rotvec\":\"%s\",\"rotvec_instance\":%d,\"rotvec_enables\":%lu,"
			 "\"rotvec_inds\":%lu,\"rotvec_errors\":%lu,\"rotvec_last_error\":%d",
			 sn_sam_state_name(r), r->on ? r->instance : -1, r->enables, r->inds, r->errors,
			 r->had_error ? r->last_error : -1);
	if (off < len)
		snprintf(buf + off, len - off,
			 ",\"qmag\":\"%s\",\"qmag_instance\":%d,\"qmag_enables\":%lu,"
			 "\"qmag_inds\":%lu,\"qmag_errors\":%lu,\"qmag_last_error\":%d%s}\n",
			 sn_sam_state_name(q), q->on ? q->instance : -1, q->enables, q->inds, q->errors,
			 q->had_error ? q->last_error : -1, qb);
}

static unsigned parse_rate(const char *arg, unsigned dflt)
{
	char *end;
	unsigned long v;

	if (!arg)
		return dflt;
	v = strtoul(arg, &end, 10);
	if (*end || v == 0 || v > 1000)
		return 0;
	return (unsigned)v;
}

static void bad_arguments(struct linesrv_client *lc, const char *cmd)
{
	linesrv_sendf(lc, "{\"type\":\"error\",\"cmd\":\"%s\",\"reason\":\"bad arguments\"}\n", cmd);
}

/* The declination request: DEG, off, or no word (a query). */
static void declination_request(struct server *s, struct linesrv_client *lc, const char *arg)
{
	struct heading *h = &s->heading;

	if (arg && strcmp(arg, "off") == 0) {
		if (h->have_decl)
			s->log("declination off (magnetic heading only)");
		h->have_decl = false;
	} else if (arg) {
		char *end;
		double d = strtod(arg, &end);

		if (*end || end == arg || !isfinite(d) || d < -180 || d > 180) {
			linesrv_sendf(lc, "{\"type\":\"error\",\"cmd\":\"declination\",\"reason\":\"bad declination\"}\n");
			return;
		}
		if (!h->have_decl || h->decl != d)
			s->log("declination %.2f deg (true_heading = heading + declination)", d);
		h->have_decl = true;
		h->decl = d;
	}
	if (h->have_decl)
		linesrv_sendf(lc, "{\"type\":\"ok\",\"cmd\":\"declination\",\"declination\":%.2f}\n", h->decl);
	else
		linesrv_sendf(lc, "{\"type\":\"ok\",\"cmd\":\"declination\",\"declination\":null}\n");
}

/* One request, already split into words by the library. A reply that
 * drops the client clears its slot (on_close): nothing is written to the
 * slot after a reply. */
static void on_line(struct linesrv *srv, struct linesrv_client *lc, int argc, char **argv)
{
	struct server *s = linesrv_ctx(srv);
	struct client *c = client_of(s, lc);
	const char *cmd = argv[0];
	char esc[SENSORD_ECHO_MAX * 6 + 1];
	int ch;

	if (strcmp(cmd, "list") == 0 || strcmp(cmd, "status") == 0) {
		char buf[3072];

		if (argc != 1) {
			bad_arguments(lc, cmd);
			return;
		}
		if (cmd[0] == 'l')
			sn_server_list_line(s, buf, sizeof(buf));
		else
			sn_server_status_line(s, buf, sizeof(buf));
		linesrv_send(lc, buf, strlen(buf));
		return;
	}
	if (strcmp(cmd, "declination") == 0) {
		if (argc > 2)
			bad_arguments(lc, cmd);
		else
			declination_request(s, lc, argc == 2 ? argv[1] : NULL);
		return;
	}
	if (strcmp(cmd, "claim") && strcmp(cmd, "release") && strcmp(cmd, "get")) {
		json_escape_n(esc, sizeof(esc), cmd, SENSORD_ECHO_MAX);
		linesrv_sendf(lc, "{\"type\":\"error\",\"cmd\":\"%s\",\"reason\":\"unknown command\"}\n", esc);
		return;
	}
	if (argc < 2 || argc > (strcmp(cmd, "release") == 0 ? 2 : 3)) {
		bad_arguments(lc, cmd);
		return;
	}
	ch = sn_channel_from_name(argv[1]);
	if (ch < 0) {
		json_escape_n(esc, sizeof(esc), argv[1], SENSORD_ECHO_MAX);
		linesrv_sendf(lc, "{\"type\":\"error\",\"cmd\":\"%s\",\"reason\":\"unknown sensor\",\"sensor\":\"%s\"}\n",
			      cmd, esc);
		return;
	}
	if (strcmp(cmd, "release") == 0) {
		if (c->rate[ch] && !c->get_only[ch])
			s->log("release %s", s->ch[ch].name);
		c->rate[ch] = 0;
		c->get[ch] = c->get_only[ch] = false;
		channels_idle_reset(s);
		linesrv_sendf(lc, "{\"type\":\"ok\",\"cmd\":\"release\",\"sensor\":\"%s\"}\n", s->ch[ch].name);
		sn_smgr_sync(s);
		return;
	}
	if (ch == CH_ROTVEC && !s->rotvec.opt) {
		linesrv_sendf(lc, "{\"type\":\"error\",\"cmd\":\"%s\",\"reason\":\"not enabled (sensord -V)\",\"sensor\":\"rotvec\"}\n",
			      cmd);
		return;
	}
	if (ch != CH_ROTVEC && s->st == SMGR_READY && !s->ch[ch].present && !s->force) {
		linesrv_sendf(lc, "{\"type\":\"error\",\"cmd\":\"%s\",\"reason\":\"sensor not present\",\"sensor\":\"%s\"}\n",
			      cmd, s->ch[ch].name);
		return;
	}
	{
		unsigned rate = parse_rate(argc == 3 ? argv[2] : NULL, s->ch[ch].default_hz);
		unsigned max = s->ch[ch].max_hz ? s->ch[ch].max_hz : 200;

		if (!rate) {
			linesrv_sendf(lc, "{\"type\":\"error\",\"cmd\":\"%s\",\"reason\":\"bad rate\",\"sensor\":\"%s\"}\n",
				      cmd, s->ch[ch].name);
			return;
		}
		if (rate > max)
			rate = max;
		if (strcmp(cmd, "claim") == 0) {
			if (!c->rate[ch] || c->get_only[ch])
				s->log("claim %s %u Hz", s->ch[ch].name, rate);
			c->rate[ch] = rate;
			c->get_only[ch] = false;
			c->next_t[ch] = 0;
			linesrv_sendf(lc, "{\"type\":\"ok\",\"cmd\":\"claim\",\"sensor\":\"%s\",\"rate\":%u}\n",
				      s->ch[ch].name, rate);
		} else {
			if (!c->rate[ch]) {
				c->rate[ch] = rate;
				c->get_only[ch] = true;
			}
			c->get[ch] = true;
			c->get_deadline[ch] = s->now_ms() + SENSORD_GET_TIMEOUT_MS;
		}
		sn_smgr_sync(s);
	}
}

/* One formatted line of channel chn, stamped t, to every client that
 * claims it: a pending get takes it (and ends a get-only claim), other
 * claims are decimated to the client's own rate. Returns true when a
 * get-only claim ended or a client was dropped meanwhile (the caller
 * re-syncs the reports, once the loop is over). */
bool sn_deliver(struct server *s, int chn, int64_t t, const char *line, size_t n)
{
	bool released = false;
	int i;

	s->delivering++;
	for (i = 0; i < SENSORD_MAX_CLIENTS; i++) {
		struct client *c = &s->clients[i];

		if (!c->lc || !c->rate[chn])
			continue;
		if (c->get[chn]) {
			client_sample(c, line, n);
			c->get[chn] = false;
			if (c->get_only[chn]) {
				c->rate[chn] = 0;
				c->get_only[chn] = false;
				released = true;
			}
			continue;
		}
		if (c->get_only[chn])
			continue;
		/* Deliver at the client's own rate when another client drives
		 * the report faster: one sample per period, 5 % early allowed
		 * for jitter, re-phased after a gap. */
		{
			int64_t period = 1000000000LL / c->rate[chn];

			if (c->next_t[chn] && t + period / 20 < c->next_t[chn])
				continue;
			c->next_t[chn] = c->next_t[chn] && t - c->next_t[chn] < period
					 ? c->next_t[chn] + period : t + period;
		}
		client_sample(c, line, n);
	}
	if (--s->delivering == 0 && s->sync_pending) {
		s->sync_pending = false;
		released = true;
	}
	return released;
}

/* One converted sample to every client that wants it. */
void sn_server_publish(struct server *s, int chn, int64_t t, const int32_t d[3])
{
	struct channel *ch = &s->ch[chn];
	bool released;
	char line[384];
	int n;

	ch->samples++;
	released = sn_heading_feed(s, chn, t, d);
	if (ch->internal) {
		if (released)
			sn_smgr_sync(s);
		return;
	}
	n = sn_format_sample(line, sizeof(line), ch, t, d);
	if (n <= 0 || (size_t)n >= sizeof(line))
		return;
	/* With -Q: the latest QMAG_CAL bias rides along on magn lines. */
	if (chn == CH_MAGN && s->qmag.have && n >= 2) {
		char ex[192];
		int k;

		ex[0] = ',';
		k = sn_format_qmag_fields(&s->qmag, ex + 1, sizeof(ex) - 1) + 1;
		if (k > 1 && (size_t)k < sizeof(ex) && (size_t)(n - 2 + k + 2) < sizeof(line))
			n = n - 2 + snprintf(line + n - 2, sizeof(line) - (size_t)(n - 2), "%s}\n", ex);
	}
	if (sn_deliver(s, chn, t, line, (size_t)n) || released)
		sn_smgr_sync(s);
}

/* Expire gets nobody answered in time. */
void sn_server_tick_clients(struct server *s)
{
	int64_t now = s->now_ms();
	int i, c;

	for (i = 0; i < SENSORD_MAX_CLIENTS; i++) {
		struct client *cl = &s->clients[i];

		if (!cl->lc)
			continue;
		for (c = 0; c < CH_COUNT; c++) {
			if (!cl->get[c] || now < cl->get_deadline[c])
				continue;
			cl->get[c] = false;
			if (cl->get_only[c]) {
				cl->rate[c] = 0;
				cl->get_only[c] = false;
			}
			/* a dropped client's claims were synced by on_close */
			if (linesrv_sendf(cl->lc, "{\"type\":\"error\",\"cmd\":\"get\",\"reason\":\"timeout\",\"sensor\":\"%s\"}\n",
					  s->ch[c].name))
				break;
			sn_smgr_sync(s);
		}
	}
}

/* poll() timeout: until the next lookup/liveness check, request
 * timeout or get deadline, so an idle daemon sleeps for seconds. */
int sn_server_deadline_ms(struct server *s)
{
	int64_t now = s->now_ms();
	int64_t next = s->st == SMGR_DOWN ? s->next_lookup : s->next_check;
	int i, c;

	next = sn_sam_deadline(s, &s->qmag, next);
	next = sn_sam_deadline(s, &s->rotvec, next);
	if (s->reg_dirty && s->reg_path && sn_reg_save_due(s) < next)
		next = sn_reg_save_due(s);
	next = sn_mag_save_due(s, next);
	for (i = 0; i < SENSORD_MAX_PENDING; i++)
		if (s->pend[i].used && s->pend[i].sent + SENSORD_REQ_TIMEOUT_MS < next)
			next = s->pend[i].sent + SENSORD_REQ_TIMEOUT_MS;
	for (i = 0; i < SENSORD_MAX_CLIENTS; i++)
		for (c = 0; s->clients[i].lc && c < CH_COUNT; c++)
			if (s->clients[i].get[c] && s->clients[i].get_deadline[c] < next)
				next = s->clients[i].get_deadline[c];
	if (next <= now)
		return 0;
	return next - now > 60000 ? 60000 : (int)(next - now);
}
