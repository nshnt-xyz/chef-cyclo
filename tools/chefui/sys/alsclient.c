/*
 * alsclient.c - the ambient light claim on sensord (/run/chef/sensord.sock) for
 * Auto brightness and theme Auto, feeding the pure logic in als.c.
 *
 * Wanted state: set by the app (sys_als_want): the screen is on and either
 * Auto brightness or theme Auto is set. Wanted: connect, wait for the
 * hello line, send "claim illuminance 5" and feed every illuminance sample
 * line to als_feed(). Not wanted: close the connection (sensord drops the
 * claim with it) and cancel any retry. The socket missing or refused, EOF,
 * an error line, or a hung sensord (no claim acknowledgment within ALS_REPLY_MS
 * of the connect, or no status reply): close and retry with a backoff of 1 s
 * doubling to 30 s, only while wanted; a ready status resets it. A
 * reconnect restarts the stream, so the logic's first-sample rules apply.
 *
 * The DSP reports illuminance only on change (gaps of many seconds at
 * steady light), so silence is normal and never means "sensor gone".
 * Liveness is a "status" request on the same connection after the claim is
 * acknowledged and then every ALS_PROBE_MS; no reply within ALS_REPLY_MS
 * means sensord hung. The sensor is available while the claim is
 * acknowledged and the last status said smgr ready; sensord re-adds the
 * report itself when the DSP restarts. The pure logic is time-driven: one
 * one-shot timer runs it at its next deadline (theme holds, smoothing), no
 * timer at steady light.
 *
 * The connection is a ../common/lineclient on the chefui loop (lcwatch),
 * on chefui's LVGL tick like the timers here; sensord's lines (proto 2,
 * docs/features/sensors.md "Socket contract") are read with the shared
 * JSON reader by their "type": hello, ok (cmd claim), status, sample,
 * error; others ({"type":"dropped",...}) and unknown members are ignored.
 * A hello with another proto is logged once and the client carries on.
 * Nothing here logs a sample; an error line is logged once per distinct
 * reason.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "als.h"
#include "chefui.h"
#include "common/chef_paths.h"
#include "common/json.h"
#include "common/lineclient.h"
#include "lcwatch.h"
#include "sys.h"
#include "sysstate.h"

#define SENSORD_PROTO 2	/* the protocol this client speaks */
#define ALS_RATE_STR "5"	/* Hz */
#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 30000

static struct {
	bool running, want;
	bool claimed, acked, smgr_ready, status_pending;
	struct lineclient lc;
	struct lcwatch w;
	lv_timer_t *tick, *probe, *reply;
	struct als als;
	sys_als_cb cb;
	void *cb_user;
	char last_err[64];
	uint32_t tick_prev;
	int64_t tick_base;
	int64_t claim_ns;	/* CLOCK_MONOTONIC when the claim line was sent */
	bool stale_logged;	/* the dropped-sample line was logged for this claim */
	bool proto_warned;	/* a hello with another proto was logged */
} A = { .lc = { .fd = -1 }, .w = { .fd = -1 }, .als = { .applied = -1, .above_since = -1, .below_since = -1 } };

/* chefui monotonic ms, extended past the 32-bit LVGL tick wrap */
static int64_t now_ms(void)
{
	uint32_t t = lv_tick_get();

	if (t < A.tick_prev)
		A.tick_base += (int64_t)1 << 32;
	A.tick_prev = t;
	return A.tick_base + t;
}

/* ------------------------------------------------------------- parsers */

/* sensord stamps a sample with CLOCK_MONOTONIC ns (sensord's channel.c); -1 when "t" is absent */
static int64_t mono_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* A fresh claim first delivers the DSP's cached reading with an old stamp (6 to 302 s old
 * in the M5 visit); a sample stamped before the claim was sent (less the slack) is not
 * a reading of now. A sample without a stamp is taken as fresh. */
bool sys_als_sample_stale(int64_t t_ns, int64_t claim_ns)
{
	return t_ns >= 0 && claim_ns > 0 && t_ns + (int64_t)ALS_STALE_SLACK_MS * 1000000 < claim_ns;
}

int sys_als_parse_sample_t(const char *line, double *lux, int64_t *t_ns)
{
	struct json_obj o;
	char type[16], sensor[16];
	double v;
	long t;

	if (json_parse(&o, line, strlen(line)) || !json_get_string(&o, "type", type, sizeof(type)) ||
	    strcmp(type, "sample") || !json_get_string(&o, "sensor", sensor, sizeof(sensor)) ||
	    strcmp(sensor, "illuminance") || !json_get_double(&o, "illuminance", &v) || !isfinite(v) || v < 0)
		return -1;
	*lux = v;
	*t_ns = json_get_long(&o, "t", &t) && t >= 0 ? (int64_t)t : -1;
	return 0;
}

int sys_als_parse_sample(const char *line, double *lux)
{
	int64_t t;

	return sys_als_parse_sample_t(line, lux, &t);
}

/* ------------------------------------------------------ timers, state */

static void deliver(const struct als_event *ev)
{
	if ((ev->bright_changed || ev->theme_changed) && A.cb)
		A.cb(ev, A.cb_user);
}

static void tick_rearm(void);

static void tick_fired(lv_timer_t *t)
{
	struct als_event ev;

	(void)t;
	A.tick = NULL;
	als_tick(&A.als, now_ms(), &ev);
	deliver(&ev);
	tick_rearm();
}

/* One one-shot timer for the logic's earliest deadline; none at steady light. */
static void tick_rearm(void)
{
	int64_t d = als_next_deadline_ms(&A.als), wait;

	if (A.tick) {
		lv_timer_delete(A.tick);
		A.tick = NULL;
	}
	if (d < 0 || !A.running)
		return;
	wait = d - now_ms();
	A.tick = lv_timer_create(tick_fired, (uint32_t)(wait < 1 ? 1 : wait), NULL);
	lv_timer_set_repeat_count(A.tick, 1);
}

static void avail_update(void)
{
	bool on = lineclient_up(&A.lc) && A.claimed && A.acked && A.smgr_ready;

	if (sys_state.als.available != on) {
		sys_state.als.available = on;
		sys_changed(SYS_ALS);
	}
}

static void timer_del(lv_timer_t **t)
{
	if (*t) {
		lv_timer_delete(*t);
		*t = NULL;
	}
}

/* The connection went (whatever ended it): the stream with it. */
static void conn_lost(struct lineclient *lc, int64_t now)
{
	(void)lc;
	(void)now;
	A.claimed = A.acked = A.smgr_ready = A.status_pending = false;
	timer_del(&A.probe);
	timer_del(&A.reply);
	als_stream_restart(&A.als);	/* no stream: the held lux and its holds are gone */
	tick_rearm();
	avail_update();
}

/* Close; the retry follows the backoff, only while wanted. */
static void conn_retry_later(void)
{
	lineclient_close(&A.lc, now_ms());
}

static void conn_cancel(void)
{
	bool up = lineclient_up(&A.lc);

	lineclient_stop(&A.lc, now_ms());	/* backoff back to 1 s */
	if (!up)
		conn_lost(&A.lc, now_ms());	/* connecting or waiting: the same clean slate */
}

static void reply_fired(lv_timer_t *t)
{
	(void)t;
	A.reply = NULL;
	chefui_log("als: no answer from sensord in %d s; reconnecting", ALS_REPLY_MS / 1000);
	conn_retry_later();
	lcwatch_sync(&A.w);
}

static void send_status(void)
{
	if (!lineclient_up(&A.lc) || !A.acked || A.status_pending)
		return;
	if (lineclient_send(&A.lc, "status\n", now_ms()))
		return;	/* broken: closed, retried */
	A.status_pending = true;
	A.reply = lv_timer_create(reply_fired, ALS_REPLY_MS, NULL);
	lv_timer_set_repeat_count(A.reply, 1);
}

static void probe_fired(lv_timer_t *t)
{
	(void)t;
	send_status();
	lcwatch_sync(&A.w);
}

static void sensord_line(struct lineclient *lc, char *line, size_t len, int64_t now)
{
	struct json_obj o;
	struct als_event ev;
	char type[16], cmd[16];
	double lux;
	int64_t t_ns;

	(void)now;
	if (json_parse(&o, line, len) || !json_get_string(&o, "type", type, sizeof(type)))
		return;		/* not one JSON object with a type: not ours to act on */
	if (!strcmp(type, "hello")) {
		long proto = -1;

		if ((!json_get_long(&o, "proto", &proto) || proto != SENSORD_PROTO) && !A.proto_warned) {
			A.proto_warned = true;
			chefui_log("als: sensord speaks proto %ld, this client %d; carrying on", proto, SENSORD_PROTO);
		}
		if (!A.claimed && !lineclient_send(lc, "claim illuminance " ALS_RATE_STR "\n", now_ms())) {
			A.claimed = true;
			A.claim_ns = mono_ns();
			A.stale_logged = false;
			als_stream_restart(&A.als);
		}	/* a failed send closed the connection: retried */
		return;
	}
	if (!strcmp(type, "sample")) {
		if (sys_als_parse_sample_t(line, &lux, &t_ns))
			return;		/* another sensor, or no usable reading */
		if (sys_als_sample_stale(t_ns, A.claim_ns)) {
			if (!A.stale_logged) {
				A.stale_logged = true;
				chefui_log("als: dropped a cached sample (%.0f lux, %.1f s older than the claim)", lux,
					   (double)(A.claim_ns - t_ns) / 1e9);
			}
			return;
		}
		sys_state.als.lux = lux;
		als_feed(&A.als, lux, now_ms(), &ev);
		deliver(&ev);
		tick_rearm();
		return;
	}
	if (!strcmp(type, "ok") && json_get_string(&o, "cmd", cmd, sizeof(cmd)) && !strcmp(cmd, "claim")) {
		if (A.claimed && !A.acked) {
			A.acked = true;
			timer_del(&A.reply);	/* the connect-to-ack deadline is met */
			als_warmup_start(&A.als, now_ms());	/* wake grace: low readings right after a wake are not trusted */
			A.probe = lv_timer_create(probe_fired, ALS_PROBE_MS, NULL);
			send_status();		/* is SMGR up: decides availability */
		}
		return;
	}
	if (!strcmp(type, "status")) {
		if (A.status_pending) {
			char smgr[16];

			A.status_pending = false;
			timer_del(&A.reply);
			A.smgr_ready = json_get_string(&o, "smgr", smgr, sizeof(smgr)) && !strcmp(smgr, "ready");
			if (A.smgr_ready)
				lineclient_healthy(lc);
			avail_update();
		}
		return;
	}
	if (!strcmp(type, "error")) {
		/* the reason (cut to fit), "" when it is not a string */
		const struct json_member *m = json_find(&o, "reason");
		char raw[256], text[sizeof(A.last_err)];

		text[0] = '\0';
		if (m && m->type == JSON_STRING && json_span_string(m->type, m->val, raw, sizeof(raw)))
			snprintf(text, sizeof(text), "%.*s", (int)sizeof(text) - 1, raw);
		if (strcmp(text, A.last_err)) {
			snprintf(A.last_err, sizeof(A.last_err), "%s", text);
			chefui_log("als: sensord error: %s", text);
		}
		conn_retry_later();
	}
	/* {"type":"dropped",...}, other types: not ours to act on */
}

/* Connected: the same deadline covers the hello and the claim acknowledgment. */
static void sensord_connected(struct lineclient *lc, int64_t now)
{
	(void)lc;
	(void)now;
	timer_del(&A.reply);
	A.reply = lv_timer_create(reply_fired, ALS_REPLY_MS, NULL);
	lv_timer_set_repeat_count(A.reply, 1);
}

static void connect_start(void)
{
	lineclient_start(&A.lc, now_ms());
	lcwatch_sync(&A.w);
}

/* ----------------------------------------------------------- interface */

int sys_als_start(const char *sock, const struct als_params *p)
{
	struct als_params def;
	sys_als_cb cb = A.cb;
	void *cb_user = A.cb_user;

	struct lineclient_config lcfg = {
		.path = sock ? sock : CHEF_SENSORD_SOCK, .backoff_min_ms = BACKOFF_MIN_MS,
		.backoff_max_ms = BACKOFF_MAX_MS, .on_connected = sensord_connected,
		.on_line = sensord_line, .on_lost = conn_lost,
	};

	if (A.running)
		sys_als_stop();
	memset(&A, 0, sizeof(A));
	A.cb = cb;
	A.cb_user = cb_user;
	if (!p) {
		als_params_default(&def);
		p = &def;
	}
	als_init(&A.als, p);
	lineclient_init(&A.lc, &lcfg);
	lcwatch_init(&A.w, &A.lc, now_ms, NULL, NULL);
	A.running = true;
	memset(&sys_state.als, 0, sizeof(sys_state.als));
	return 0;
}

void sys_als_stop(void)
{
	if (!A.running)
		return;
	A.running = false;
	A.want = false;
	conn_cancel();
	lcwatch_fini(&A.w);
	timer_del(&A.tick);
}

void sys_als_want(bool want)
{
	if (want == A.want)
		return;
	A.want = want;
	if (!A.running)
		return;
	if (want)
		connect_start();
	else
		conn_cancel();
	lcwatch_sync(&A.w);
}

bool sys_als_wanted(void)
{
	return A.want;
}

void sys_als_on_change(sys_als_cb cb, void *user)
{
	A.cb = cb;
	A.cb_user = user;
}

void sys_als_set_offset(int offset)
{
	als_set_offset(&A.als, offset);
	tick_rearm();
}

/* A drag: bring the filter up to now first, so the curve it is measured on is current. */
bool sys_als_offset_for(int slider, int *offset)
{
	struct als_event ev;

	als_tick(&A.als, now_ms(), &ev);
	deliver(&ev);
	tick_rearm();
	return als_offset_for(&A.als, slider, offset);
}

int sys_als_curve_now(void)
{
	return als_curve_now(&A.als);
}

void sys_als_set_applied(int pct)
{
	als_set_applied(&A.als, pct);
	tick_rearm();
}

void sys_als_theme_reset(void)
{
	als_theme_reset(&A.als);
	tick_rearm();
}

bool sys_als_theme_light(void)
{
	return A.als.light;
}

int sys_als_applied(void)
{
	return A.als.applied;
}
