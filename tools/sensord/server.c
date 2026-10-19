/*
 * sensord's clients and the socket protocol: the claim bookkeeping, the
 * list and status lines, requests, delivery of samples to their claimants
 * and the get timeouts. The daemon as a whole is described in main.c.
 *
 * Socket API, /run/sensord.sock, one JSON object per line from sensord,
 * plain-text requests from the client:
 *   (on connect)          {"hello":"sensord","proto":1}
 *   list                  {"ok":"list","smgr":"ready","sensors":[...]}
 *   status                {"ok":"status",...}
 *   claim SENSOR [HZ]     {"ok":"claim","sensor":"accel","rate":50}
 *                         then {"sensor":"accel","t":NS,"x":..,"y":..,"z":..}
 *   release SENSOR        {"ok":"release","sensor":"accel"}
 *   get SENSOR [HZ]       one sample line, or {"err":"timeout",...}
 *   declination [DEG|off] {"ok":"declination","declination":DEG|null}
 *   errors                {"err":"unknown sensor","sensor":"x"} ...
 * A sensor is powered (report added on the DSP) only while at least one
 * connection claims it; a claim lives on its connection. Each client has
 * a bounded output queue: a slow reader loses samples, never stalls the
 * daemon, and is told how many with {"dropped":N} before the next line
 * that fits. A client that lets a reply overflow is disconnected.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "sensord.h"

void sn_server_init(struct server *s)
{
	int i;

	memset(s, 0, sizeof(*s));
	memcpy(s->ch, sn_channel_defaults, sizeof(s->ch));
	for (i = 0; i < SENSORD_MAX_CLIENTS; i++)
		s->clients[i].fd = -1;
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
	s->log = sn_kmsg_note;
	s->elog = sn_stderr_note;
}

int sn_channel_claims(const struct server *s, int c)
{
	int i, n = 0;

	for (i = 0; i < SENSORD_MAX_CLIENTS; i++)
		if (s->clients[i].fd >= 0 && s->clients[i].rate[c])
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
		if (s->clients[i].fd >= 0 && s->clients[i].rate[c] > want)
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

struct client *sn_server_add_client(struct server *s, int fd)
{
	int i;

	for (i = 0; i < SENSORD_MAX_CLIENTS; i++) {
		struct client *c = &s->clients[i];

		if (c->fd >= 0)
			continue;
		memset(c, 0, sizeof(*c));
		c->fd = fd;
		return c;
	}
	return NULL;
}


void sn_server_drop_client(struct server *s, struct client *c)
{
	int i;

	if (c->fd < 0)
		return;
	for (i = 0; i < CH_COUNT; i++)
		if (c->rate[i] && !c->get_only[i])
			s->log("release %s (client gone)", s->ch[i].name);
	close(c->fd);
	c->fd = -1;
	memset(c->rate, 0, sizeof(c->rate));
	memset(c->get, 0, sizeof(c->get));
	channels_idle_reset(s);
	sn_smgr_sync(s);
}

/* Push queued output; the socket is non-blocking. 0, or -errno (drop). */
static int client_flush(struct client *c)
{
	size_t off = 0;

	while (off < c->outlen) {
		ssize_t n = send(c->fd, c->out + off, c->outlen - off, MSG_NOSIGNAL | MSG_DONTWAIT);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			return -errno;
		}
		off += (size_t)n;
	}
	memmove(c->out, c->out + off, c->outlen - off);
	c->outlen -= off;
	return 0;
}

static bool client_room(const struct client *c, size_t n)
{
	return c->outlen + n <= sizeof(c->out);
}

static void client_append(struct client *c, const char *line, size_t n)
{
	memcpy(c->out + c->outlen, line, n);
	c->outlen += n;
}

/* Reply lines must get through: a client that lets them overflow is not
 * reading at all and is dropped. */
void sn_client_reply(struct server *s, struct client *c, const char *fmt, ...)
{
	char buf[2048];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= sizeof(buf))
		n = snprintf(buf, sizeof(buf), "{\"err\":\"internal\"}\n");
	if (!client_room(c, (size_t)n)) {
		s->log("client output full, disconnecting it");
		sn_server_drop_client(s, c);
		return;
	}
	client_append(c, buf, (size_t)n);
	if (client_flush(c))
		sn_server_drop_client(s, c);
}

/* Sample lines are best effort: when one does not fit it is counted, and
 * the count goes out ahead of the next line that does. */
static void client_sample(struct client *c, const char *line, size_t n)
{
	char note[64];
	int k = 0;

	if (c->dropped)
		k = snprintf(note, sizeof(note), "{\"dropped\":%lu}\n", c->dropped);
	if (!client_room(c, (size_t)k + n)) {
		c->dropped++;
		c->dropped_total++;
		return;
	}
	if (k) {
		client_append(c, note, (size_t)k);
		c->dropped = 0;
	}
	client_append(c, line, n);
}


void sn_server_list_line(const struct server *s, char *buf, size_t len)
{
	static const char *st[] = { "down", "info", "ready" };
	size_t off;
	int i;

	off = (size_t)snprintf(buf, len, "{\"ok\":\"list\",\"smgr\":\"%s\",\"sensors\":[", st[s->st]);
	for (i = 0; i < CH_SMGR_COUNT && off < len; i++) {
		const struct channel *ch = &s->ch[i];
		char hw[2 * sizeof(ch->hw)], vendor[2 * sizeof(ch->vendor)];

		if (ch->internal)
			continue;
		sn_json_escape(hw, sizeof(hw), ch->hw);
		sn_json_escape(vendor, sizeof(vendor), ch->vendor);
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
		if (s->clients[i].fd >= 0)
			nc++;
	if (q->have) {
		qb[0] = ',';
		sn_format_qmag_fields(q, qb + 1, sizeof(qb) - 1);
	}
	off = (size_t)snprintf(buf, len,
		 "{\"ok\":\"status\",\"proto\":%d,\"smgr\":\"%s\",\"smgr_node\":%u,\"smgr_port\":%u,"
		 "\"smgr_resets\":%lu,\"api\":\"%s\",\"reg2\":%s,\"reg2_reads\":%lu,"
		 "\"reg2_writes\":%lu,\"reg2_misses\":%lu,\"reg2_saves\":%lu,\"reg2_save_errors\":%lu,"
		 "\"time2\":%s,\"time2_reqs\":%lu,"
		 "\"unknown_reqs\":%lu,\"ts\":\"%s\",\"clients\":%d",
		 SENSORD_PROTO, st[s->st], s->smgr_node, s->smgr_port, s->smgr_resets,
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

/* One complete request line from a client (no trailing newline). */
void sn_server_handle_line(struct server *s, struct client *c, char *line)
{
	char *cmd, *name, *rate_s, *save = NULL;
	int ch;

	cmd = strtok_r(line, " \t\r", &save);
	if (!cmd)
		return;
	name = strtok_r(NULL, " \t\r", &save);
	rate_s = strtok_r(NULL, " \t\r", &save);

	if (strcmp(cmd, "list") == 0) {
		char buf[3072];

		sn_server_list_line(s, buf, sizeof(buf));
		sn_client_reply(s, c, "%s", buf);
		return;
	}
	if (strcmp(cmd, "status") == 0) {
		char buf[3072];

		sn_server_status_line(s, buf, sizeof(buf));
		sn_client_reply(s, c, "%s", buf);
		return;
	}
	if (strcmp(cmd, "declination") == 0) {
		struct heading *h = &s->heading;

		if (name && strcmp(name, "off") == 0) {
			if (h->have_decl)
				s->log("declination off (magnetic heading only)");
			h->have_decl = false;
		} else if (name) {
			char *end;
			double d = strtod(name, &end);

			if (*end || end == name || !isfinite(d) || d < -180 || d > 180) {
				sn_client_reply(s, c, "{\"err\":\"bad declination\"}\n");
				return;
			}
			if (!h->have_decl || h->decl != d)
				s->log("declination %.2f deg (true_heading = heading + declination)", d);
			h->have_decl = true;
			h->decl = d;
		}
		if (h->have_decl)
			sn_client_reply(s, c, "{\"ok\":\"declination\",\"declination\":%.2f}\n", h->decl);
		else
			sn_client_reply(s, c, "{\"ok\":\"declination\",\"declination\":null}\n");
		return;
	}
	if (strcmp(cmd, "claim") && strcmp(cmd, "release") && strcmp(cmd, "get")) {
		sn_client_reply(s, c, "{\"err\":\"unknown command\",\"command\":\"%.32s\"}\n", cmd);
		return;
	}
	ch = name ? sn_channel_from_name(name) : -1;
	if (ch < 0) {
		char esc[80];

		sn_json_escape(esc, sizeof(esc), name ? name : "");
		sn_client_reply(s, c, "{\"err\":\"unknown sensor\",\"sensor\":\"%s\"}\n", esc);
		return;
	}
	if (strcmp(cmd, "release") == 0) {
		if (c->rate[ch] && !c->get_only[ch])
			s->log("release %s", s->ch[ch].name);
		c->rate[ch] = 0;
		c->get[ch] = c->get_only[ch] = false;
		channels_idle_reset(s);
		sn_client_reply(s, c, "{\"ok\":\"release\",\"sensor\":\"%s\"}\n", s->ch[ch].name);
		sn_smgr_sync(s);
		return;
	}
	if (ch == CH_ROTVEC && !s->rotvec.opt) {
		sn_client_reply(s, c, "{\"err\":\"not enabled (sensord -V)\",\"sensor\":\"rotvec\"}\n");
		return;
	}
	if (ch != CH_ROTVEC && s->st == SMGR_READY && !s->ch[ch].present && !s->force) {
		sn_client_reply(s, c, "{\"err\":\"sensor not present\",\"sensor\":\"%s\"}\n",
			     s->ch[ch].name);
		return;
	}
	{
		unsigned rate = parse_rate(rate_s, s->ch[ch].default_hz);
		unsigned max = s->ch[ch].max_hz ? s->ch[ch].max_hz : 200;

		if (!rate) {
			sn_client_reply(s, c, "{\"err\":\"bad rate\",\"sensor\":\"%s\"}\n", s->ch[ch].name);
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
			sn_client_reply(s, c, "{\"ok\":\"claim\",\"sensor\":\"%s\",\"rate\":%u}\n",
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

/* Buffer bytes from a client fd and hand complete lines over. */
void sn_server_client_input(struct server *s, struct client *c)
{
	ssize_t n;
	char *nl;

	n = recv(c->fd, c->in + c->inlen, sizeof(c->in) - 1 - c->inlen, MSG_DONTWAIT);
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
		sn_server_drop_client(s, c);
		return;
	}
	if (n < 0)
		return;
	c->inlen += (size_t)n;
	c->in[c->inlen] = '\0';
	while ((nl = memchr(c->in, '\n', c->inlen))) {
		size_t used = (size_t)(nl - c->in) + 1;

		*nl = '\0';
		if (c->discarding)
			c->discarding = false;
		else
			sn_server_handle_line(s, c, c->in);
		if (c->fd < 0)
			return;
		memmove(c->in, c->in + used, c->inlen - used);
		c->inlen -= used;
		c->in[c->inlen] = '\0';
	}
	if (c->discarding) {
		c->inlen = 0;
	} else if (c->inlen == sizeof(c->in) - 1) {
		sn_client_reply(s, c, "{\"err\":\"line too long\"}\n");
		c->inlen = 0;
		c->discarding = true;
	}
}

/* One formatted line of channel chn, stamped t, to every client that
 * claims it: a pending get takes it (and ends a get-only claim), other
 * claims are decimated to the client's own rate. Returns true when a
 * get-only claim ended (the caller re-syncs the reports). */
bool sn_deliver(struct server *s, int chn, int64_t t, const char *line, size_t n)
{
	bool released = false;
	int i;

	for (i = 0; i < SENSORD_MAX_CLIENTS; i++) {
		struct client *c = &s->clients[i];

		if (c->fd < 0 || !c->rate[chn])
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

void sn_server_flush_all(struct server *s)
{
	int i;

	for (i = 0; i < SENSORD_MAX_CLIENTS; i++)
		if (s->clients[i].fd >= 0 && s->clients[i].outlen && client_flush(&s->clients[i]))
			sn_server_drop_client(s, &s->clients[i]);
}


/* Expire gets nobody answered in time. */
void sn_server_tick_clients(struct server *s)
{
	int64_t now = s->now_ms();
	int i, c;

	for (i = 0; i < SENSORD_MAX_CLIENTS; i++) {
		struct client *cl = &s->clients[i];

		if (cl->fd < 0)
			continue;
		for (c = 0; c < CH_COUNT; c++) {
			if (!cl->get[c] || now < cl->get_deadline[c])
				continue;
			cl->get[c] = false;
			if (cl->get_only[c]) {
				cl->rate[c] = 0;
				cl->get_only[c] = false;
			}
			sn_client_reply(s, cl, "{\"err\":\"timeout\",\"sensor\":\"%s\"}\n", s->ch[c].name);
			if (cl->fd < 0)
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
	for (i = 0; i < SENSORD_MAX_PENDING; i++)
		if (s->pend[i].used && s->pend[i].sent + SENSORD_REQ_TIMEOUT_MS < next)
			next = s->pend[i].sent + SENSORD_REQ_TIMEOUT_MS;
	for (i = 0; i < SENSORD_MAX_CLIENTS; i++)
		for (c = 0; s->clients[i].fd >= 0 && c < CH_COUNT; c++)
			if (s->clients[i].get[c] && s->clients[i].get_deadline[c] < next)
				next = s->clients[i].get_deadline[c];
	if (next <= now)
		return 0;
	return next - now > 60000 ? 60000 : (int)(next - now);
}
