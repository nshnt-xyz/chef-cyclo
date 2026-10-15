/*
 * alsclient.c - the ambient light claim on sensord (/run/sensord.sock) for
 * Auto brightness and theme Auto, feeding the pure logic in als.c.
 *
 * Wanted state: set by the app (sys_als_want): the screen is on and either
 * Auto brightness or theme Auto is set. Wanted: connect, wait for the
 * hello line, send "claim illuminance 5" and feed every sample line to
 * als_feed(). Not wanted: close the connection (sensord drops the claim
 * with it) and cancel any retry. The socket missing or refused, EOF, an
 * err line, or a hung sensord (no claim acknowledgment within ALS_REPLY_MS
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
 * Nonblocking connect and reads on the chefui loop, like gps.c. Nothing
 * here logs a sample; an err line is logged once per distinct text.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "als.h"
#include "chefui.h"
#include "linebuf.h"
#include "sys.h"
#include "sysstate.h"

#define SENSORD_SOCK "/run/sensord.sock"
#define ALS_RATE_STR "5"	/* Hz */
#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 30000
#define READS_PER_WAKEUP 4

static struct {
	char sock[108];
	bool running, want;
	int fd;
	bool connecting, claimed, acked, smgr_ready, status_pending;
	int backoff_ms;
	lv_timer_t *retry, *tick, *probe, *reply;
	struct linebuf lb;
	struct als als;
	sys_als_cb cb;
	void *cb_user;
	char last_err[64];
	uint32_t tick_prev;
	int64_t tick_base;
} A = { .fd = -1, .als = { .applied = -1, .above_since = -1, .below_since = -1 } };

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

int sys_als_parse_sample(const char *line, double *lux)
{
	static const char head[] = "{\"sensor\":\"illuminance\"";
	const char *p;
	char *end;
	double v;

	if (strncmp(line, head, sizeof(head) - 1))
		return -1;
	p = strstr(line + sizeof(head) - 1, "\"illuminance\":");
	if (!p)
		return -1;
	p += 14;
	v = strtod(p, &end);
	if (end == p || !isfinite(v) || v < 0)
		return -1;
	*lux = v;
	return 0;
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
	bool on = A.fd >= 0 && !A.connecting && A.claimed && A.acked && A.smgr_ready;

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

static void conn_close(void)
{
	if (A.fd >= 0) {
		chefui_unwatch_fd(A.fd);
		close(A.fd);
		A.fd = -1;
	}
	A.connecting = A.claimed = A.acked = A.smgr_ready = A.status_pending = false;
	linebuf_reset(&A.lb);
	timer_del(&A.probe);
	timer_del(&A.reply);
	als_stream_restart(&A.als);	/* no stream: the held lux and its holds are gone */
	tick_rearm();
	avail_update();
}

static void connect_start(void);

static void retry_fired(lv_timer_t *t)
{
	(void)t;
	A.retry = NULL;
	if (A.running && A.want)
		connect_start();
}

static void conn_retry_later(void)
{
	conn_close();
	if (A.retry || !A.running || !A.want)
		return;
	A.retry = lv_timer_create(retry_fired, (uint32_t)A.backoff_ms, NULL);
	lv_timer_set_repeat_count(A.retry, 1);
	A.backoff_ms = A.backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : A.backoff_ms * 2;
}

static void conn_cancel(void)
{
	conn_close();
	timer_del(&A.retry);
	A.backoff_ms = BACKOFF_MIN_MS;
}

static void reply_fired(lv_timer_t *t)
{
	(void)t;
	A.reply = NULL;
	chefui_log("als: no answer from sensord in %d s; reconnecting", ALS_REPLY_MS / 1000);
	conn_retry_later();
}

static void send_status(void)
{
	static const char req[] = "status\n";

	if (A.fd < 0 || !A.acked || A.status_pending)
		return;
	if (send(A.fd, req, sizeof(req) - 1, MSG_NOSIGNAL | MSG_DONTWAIT) != (ssize_t)sizeof(req) - 1) {
		conn_retry_later();
		return;
	}
	A.status_pending = true;
	A.reply = lv_timer_create(reply_fired, ALS_REPLY_MS, NULL);
	lv_timer_set_repeat_count(A.reply, 1);
}

static void probe_fired(lv_timer_t *t)
{
	(void)t;
	send_status();
}

static void sensord_line(char *line, void *user)
{
	struct als_event ev;
	double lux;

	(void)user;
	if (!strncmp(line, "{\"hello\":", 9)) {
		static const char claim[] = "claim illuminance " ALS_RATE_STR "\n";

		if (!A.claimed && send(A.fd, claim, sizeof(claim) - 1, MSG_NOSIGNAL | MSG_DONTWAIT) ==
				  (ssize_t)sizeof(claim) - 1) {
			A.claimed = true;
			als_stream_restart(&A.als);
		} else if (!A.claimed) {
			conn_retry_later();
		}
		return;
	}
	if (!sys_als_parse_sample(line, &lux)) {
		sys_state.als.lux = lux;
		als_feed(&A.als, lux, now_ms(), &ev);
		deliver(&ev);
		tick_rearm();
		return;
	}
	if (!strncmp(line, "{\"ok\":\"claim\"", 13)) {
		if (A.claimed && !A.acked) {
			A.acked = true;
			timer_del(&A.reply);	/* the connect-to-ack deadline is met */
			A.probe = lv_timer_create(probe_fired, ALS_PROBE_MS, NULL);
			send_status();		/* is SMGR up: decides availability */
		}
		return;
	}
	if (!strncmp(line, "{\"ok\":\"status\"", 14)) {
		if (A.status_pending) {
			A.status_pending = false;
			timer_del(&A.reply);
			A.smgr_ready = strstr(line, "\"smgr\":\"ready\"") != NULL;
			if (A.smgr_ready)
				A.backoff_ms = BACKOFF_MIN_MS;
			avail_update();
		}
		return;
	}
	if (!strncmp(line, "{\"err\":", 7)) {
		char text[sizeof(A.last_err)] = "";
		const char *q = line + 7;

		if (*q == '"') {
			size_t n = strcspn(q + 1, "\"");

			snprintf(text, sizeof(text), "%.*s", (int)n, q + 1);
		}
		if (strcmp(text, A.last_err)) {
			snprintf(A.last_err, sizeof(A.last_err), "%s", text);
			chefui_log("als: sensord error: %s", text);
		}
		conn_retry_later();
	}
	/* {"dropped":N}, anything else: not ours to act on */
}

static void als_io(int fd, short revents, void *user)
{
	char buf[4096];

	(void)user;
	if (A.connecting) {
		int err = 0;
		socklen_t l = sizeof(err);

		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l) || err ||
		    (revents & (POLLERR | POLLHUP))) {
			conn_retry_later();
			return;
		}
		A.connecting = false;
		chefui_watch_fd(fd, POLLIN, als_io, NULL);
		return;
	}
	for (int i = 0; i < READS_PER_WAKEUP && A.fd >= 0; i++) {
		ssize_t n = recv(A.fd, buf, sizeof(buf), MSG_DONTWAIT);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		if (n <= 0) {
			conn_retry_later();
			return;
		}
		linebuf_feed(&A.lb, buf, (size_t)n, sensord_line, NULL);
	}
}

static void connect_start(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd;

	if (A.fd >= 0)
		return;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", A.sock);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		conn_retry_later();
		return;
	}
	A.fd = fd;
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) && errno != EINPROGRESS) {
		conn_retry_later();	/* ENOENT/ECONNREFUSED: not there yet */
		return;
	}
	A.connecting = true;
	chefui_watch_fd(fd, POLLOUT, als_io, NULL);
	/* the same deadline covers connect, hello and the claim acknowledgment */
	A.reply = lv_timer_create(reply_fired, ALS_REPLY_MS, NULL);
	lv_timer_set_repeat_count(A.reply, 1);
}

/* ----------------------------------------------------------- interface */

int sys_als_start(const char *sock, const struct als_params *p)
{
	struct als_params def;
	sys_als_cb cb = A.cb;
	void *cb_user = A.cb_user;

	memset(&A, 0, sizeof(A));
	A.cb = cb;
	A.cb_user = cb_user;
	snprintf(A.sock, sizeof(A.sock), "%s", sock ? sock : SENSORD_SOCK);
	if (!p) {
		als_params_default(&def);
		p = &def;
	}
	als_init(&A.als, p);
	A.fd = -1;
	A.backoff_ms = BACKOFF_MIN_MS;
	A.running = true;
	memset(&sys_state.als, 0, sizeof(sys_state.als));
	return 0;
}

void sys_als_stop(void)
{
	A.running = false;
	A.want = false;
	conn_cancel();
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
