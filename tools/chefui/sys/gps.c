/*
 * gps.c - GPS state for the status bar and hub tile, and the GPS pages.
 *
 * gps-manager (docs/features/gps.md "Socket contract", JSON lines read
 * with ../common/json.h): a `watch` connection on /run/gps-manager.sock.
 * Its "state" lines: DISABLED = the switch is off, OFF/STOPPING = on and
 * idle, FAILED = failed, STARTING/ACQUIRING = searching, RUNNING = fix,
 * with the bring-up's age and first fix; "switch" lines carry the switch.
 * Reconnected with backoff (1 s doubling to 8 s) when the manager is not
 * there or goes away.
 *
 * No lease, except while the GPS page asks (sys_gps_watch): then the same
 * connection holds one "map" lease (sent again after a reconnect or when
 * the switch comes back on; an error with reason "disabled" means none is
 * held), released when the page lets go. A lease lives on its connection,
 * so a crash of the app releases it too.
 *
 * gpsd (127.0.0.1:2947): ?WATCH JSON while RUNNING for TPV mode and eph
 * ("3D fix · ±3 m"), and while the page watches also during ACQUIRING,
 * for the page's TPV and SKY (gps_view, gpsview.h). Disconnected when
 * neither applies.
 *
 * Both sockets are nonblocking (connect included) and read at most a few
 * buffers per wakeup. Nothing here logs a position.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <math.h>
#include <time.h>

#include "chefui.h"
#include "common/json.h"
#include "gpsview.h"
#include "linebuf.h"
#include "sys.h"
#include "sysstate.h"

#define MGR_SOCK "/run/gps-manager.sock"
#define MGR_PROTO 1
#define GPSD_PORT 2947
#define BACKOFF_MIN_MS 1000
#define MGR_BACKOFF_MAX_MS 8000
#define GPSD_BACKOFF_MAX_MS 4000
#define READS_PER_WAKEUP 4

struct conn {
	const char *name;
	int fd;
	bool connecting;
	int backoff_ms;
	int max_ms;
	lv_timer_t *retry;
	struct linebuf lb;
	void (*start)(void);
};

static struct {
	char sock[108];
	int port;
	bool running;		/* started (not stopped) */
	struct conn mgr, gpsd;
	bool watch;		/* the GPS page wants a lease and the view */
	bool lease_sent;	/* "lease map" sent on this connection, not refused */
	bool watching;		/* "watch" answered on this connection */
	bool proto_warned;	/* an unknown proto was logged once */
	int64_t acq_start_ms, acq_fix_ms;	/* the manager's bring-up (monotonic ms, 0 none) */
	lv_timer_t *sats_stale;	/* no full SKY for GPS_SATS_STALE_MS: the satellites are cleared */
} G;

static int64_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------- parsers */

int sys_gps_parse_mgr(const char *line, struct sys_gps_mgr_line *out)
{
	static const struct { const char *name; enum sys_gps s; } map[] = {
		{ "OFF", SYS_GPS_OFF }, { "STOPPING", SYS_GPS_OFF }, { "FAILED", SYS_GPS_FAILED },
		{ "DISABLED", SYS_GPS_DISABLED },
		{ "STARTING", SYS_GPS_SEARCHING }, { "ACQUIRING", SYS_GPS_SEARCHING },
		{ "RUNNING", SYS_GPS_RUNNING },
	};
	struct json_obj o;
	char type[16], st[16];
	bool on;
	size_t i;

	memset(out, 0, sizeof(*out));
	out->proto = out->up_ms = out->fix_ms = -1;
	if (json_parse(&o, line, strlen(line)) || !json_get_string(&o, "type", type, sizeof(type)))
		return -1;
	json_get_string(&o, "cmd", out->cmd, sizeof(out->cmd));
	if (!strcmp(type, "hello")) {
		out->type = SYS_GPS_MGR_HELLO;
		json_get_long(&o, "proto", &out->proto);
	} else if (!strcmp(type, "ok")) {
		out->type = SYS_GPS_MGR_OK;
	} else if (!strcmp(type, "error")) {
		out->type = SYS_GPS_MGR_ERROR;
		json_get_string(&o, "reason", out->reason, sizeof(out->reason));
	} else if (!strcmp(type, "switch")) {
		if (!json_get_bool(&o, "enabled", &on))
			return -1;
		out->type = SYS_GPS_MGR_SWITCH;
		out->enabled = on;
	} else if (!strcmp(type, "state")) {
		if (!json_get_string(&o, "state", st, sizeof(st)))
			return -1;
		for (i = 0; i < sizeof(map) / sizeof(map[0]) && strcmp(st, map[i].name); i++)
			;
		if (i == sizeof(map) / sizeof(map[0]))
			return -1;
		/* null (or absent) when there is no bring-up or no fix yet */
		if (!json_get_long(&o, "up_ms", &out->up_ms) || out->up_ms < 0 || out->up_ms > 1000000000L)
			out->up_ms = -1;
		if (out->up_ms < 0 || !json_get_long(&o, "fix_ms", &out->fix_ms) || out->fix_ms < 0 ||
		    out->fix_ms > 1000000000L)
			out->fix_ms = -1;
		out->type = SYS_GPS_MGR_STATE;
		out->state = map[i].s;
	} else {
		out->type = SYS_GPS_MGR_OTHER;
	}
	return 0;
}

int sys_gps_parse_tpv(const char *line, int *mode, double *eph)
{
	const char *p;
	char *end;
	long m;

	if (line[0] != '{' || !strstr(line, "\"class\":\"TPV\""))
		return -1;
	p = strstr(line, "\"mode\":");
	if (!p)
		return -1;
	m = strtol(p + 7, &end, 10);
	if (end == p + 7 || m < 0 || m > 3)
		return -1;
	*mode = (int)m;
	*eph = -1;
	p = strstr(line, "\"eph\":");
	if (p) {
		double e = strtod(p + 6, &end);

		if (end != p + 6 && e >= 0 && e < 1e6)
			*eph = e;
	}
	return 0;
}

/* --------------------------------------------------------- connections */

static void conn_close(struct conn *c)
{
	if (c->fd >= 0) {
		chefui_unwatch_fd(c->fd);
		close(c->fd);
		c->fd = -1;
	}
	c->connecting = false;
	linebuf_reset(&c->lb);
}

static void retry_fired(lv_timer_t *t)
{
	struct conn *c = lv_timer_get_user_data(t);

	c->retry = NULL;
	if (G.running)
		c->start();
}

static void conn_retry_later(struct conn *c)
{
	conn_close(c);
	if (c->retry || !G.running)
		return;
	c->retry = lv_timer_create(retry_fired, c->backoff_ms, c);
	lv_timer_set_repeat_count(c->retry, 1);
	c->backoff_ms = c->backoff_ms * 2 > c->max_ms ? c->max_ms : c->backoff_ms * 2;
}

static void conn_cancel(struct conn *c)
{
	conn_close(c);
	if (c->retry) {
		lv_timer_delete(c->retry);
		c->retry = NULL;
	}
	c->backoff_ms = BACKOFF_MIN_MS;
}

static bool send_str(int fd, const char *s)
{
	size_t n = strlen(s);
	ssize_t w = send(fd, s, n, MSG_NOSIGNAL | MSG_DONTWAIT);

	return w == (ssize_t)n;	/* tiny requests: partial = broken */
}

/* Read what is there (bounded). false: EOF or error, connection closed. */
static bool conn_read(struct conn *c, linebuf_cb cb)
{
	char buf[4096];

	for (int i = 0; i < READS_PER_WAKEUP && c->fd >= 0; i++) {
		ssize_t n = recv(c->fd, buf, sizeof(buf), MSG_DONTWAIT);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return true;
		if (n <= 0)
			return false;
		linebuf_feed(&c->lb, buf, (size_t)n, cb, c);
	}
	return c->fd >= 0;
}

/* --------------------------------------------------------------- gpsd */

static void gps_set(enum sys_gps st, int mode, double eph)
{
	if (st != sys_state.gps.state || mode != sys_state.gps.mode || eph != sys_state.gps.eph) {
		sys_state.gps.state = st;
		sys_state.gps.mode = mode;
		sys_state.gps.eph = eph;
		sys_changed(SYS_GPS);
	}
}

/* Forget the reports (position included). The lease's own times stay
 * while it is held: the first fix of this lease does not move when gpsd
 * reconnects; whoever ends the lease resets them (lease_end). */
static void view_clear(void)
{
	int64_t lease = gps_view.lease_ms;

	if (G.sats_stale)
		lv_timer_pause(G.sats_stale);

	memset(&gps_view, 0, sizeof(gps_view));
	gps_view.eph = gps_view.alt = gps_view.speed = gps_view.lat = gps_view.lon = NAN;
	gps_view.hdop = gps_view.pdop = NAN;
	gps_view.lease_ms = G.watch ? lease : 0;
	gps_view.start_ms = G.acq_start_ms;	/* the manager's bring-up outlives the page's views */
	gps_view.first_fix_ms = G.acq_fix_ms;
}

static void lease_end(void)
{
	gps_view.lease_ms = 0;
}

/* gpsd's DOP-only SKY lines keep the satellites of the last full one; without a full one for
 * GPS_SATS_STALE_MS (the receiver lost its sky, gpsd stalled) the list goes, so the page never
 * shows satellites that are no longer reported. */
static void sats_stale_fired(lv_timer_t *t)
{
	lv_timer_pause(t);
	if (gps_view.n_sats) {
		gps_view.n_sats = 0;
		sys_changed(SYS_GPS_VIEW);
	}
}

static void sats_fresh(void)
{
	if (!G.sats_stale) {
		G.sats_stale = lv_timer_create(sats_stale_fired, GPS_SATS_STALE_MS, NULL);
	} else {
		lv_timer_reset(G.sats_stale);
		lv_timer_resume(G.sats_stale);
	}
}

static void gpsd_line(char *line, void *user)
{
	int mode;
	double eph;

	(void)user;
	if (G.watch && !gps_parse_tpv_view(line, &gps_view)) {
		sys_changed(SYS_GPS_VIEW);
	} else if (G.watch && !gps_parse_sky_view(line, &gps_view)) {
		if (strstr(line, "\"satellites\":["))
			sats_fresh();
		sys_changed(SYS_GPS_VIEW);
	}
	if (!sys_gps_parse_tpv(line, &mode, &eph)) {
		/* eph rounded to a metre so small wobbles do not redraw */
		if (eph >= 0)
			eph = (double)(long)(eph + 0.5);
		gps_set(sys_state.gps.state, mode, eph);
		G.gpsd.backoff_ms = BACKOFF_MIN_MS;
	}
}

/* gpsd went (or never came): retry, and show nothing stale meanwhile */
static void gpsd_lost(void)
{
	conn_retry_later(&G.gpsd);
	if (gps_view.sky || gps_view.mode) {
		view_clear();
		sys_changed(SYS_GPS_VIEW);
	}
}

static void gpsd_io(int fd, short revents, void *user)
{
	struct conn *c = &G.gpsd;

	(void)user;
	if (c->connecting) {
		int err = 0;
		socklen_t l = sizeof(err);

		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l) || err ||
		    (revents & (POLLERR | POLLHUP))) {
			gpsd_lost();
			return;
		}
		c->connecting = false;
		if (!send_str(fd, "?WATCH={\"enable\":true,\"json\":true};\n")) {
			gpsd_lost();
			return;
		}
		chefui_watch_fd(fd, POLLIN, gpsd_io, NULL);
		return;
	}
	if (!conn_read(c, gpsd_line))
		gpsd_lost();
}

/* gpsd while RUNNING (status), and while the page watches ACQUIRING too */
static bool gpsd_wanted(void)
{
	return sys_state.gps.state == SYS_GPS_RUNNING ||
	       (G.watch && sys_state.gps.state == SYS_GPS_SEARCHING);
}

static void gpsd_start(void)
{
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)G.port) };
	struct conn *c = &G.gpsd;
	int fd;

	if (c->fd >= 0 || !gpsd_wanted())
		return;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		gpsd_lost();
		return;
	}
	c->fd = fd;
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
		c->connecting = true;	/* finish in gpsd_io on POLLOUT */
	} else if (errno == EINPROGRESS) {
		c->connecting = true;
	} else {
		gpsd_lost();
		return;
	}
	chefui_watch_fd(fd, POLLOUT, gpsd_io, NULL);
}

/* ------------------------------------------------------- gps-manager */

/* The page's lease on the watch connection: held while wanted. */
static void lease_sync(void)
{
	struct conn *c = &G.mgr;

	if (c->fd < 0 || c->connecting || !G.watching)
		return;
	if (G.watch && !G.lease_sent && sys_state.gps.enabled != 0) {
		if (send_str(c->fd, "lease map\n")) {
			G.lease_sent = true;
			gps_view.lease_ms = mono_ms();
			sys_changed(SYS_GPS_VIEW);
		}
	} else if (!G.watch && G.lease_sent) {
		if (send_str(c->fd, "release map\n"))
			G.lease_sent = false;
	}
}

static void enabled_set(int on)
{
	if (sys_state.gps.enabled != on) {
		sys_state.gps.enabled = on;
		sys_changed(SYS_GPS);
	}
}

/* One line from gps-manager: the hello's proto checked, the watch reply,
 * a refused lease, the switch and the state (with the bring-up's times);
 * other replies and types need nothing. */
static void mgr_line(char *line, void *user)
{
	struct sys_gps_mgr_line l;
	enum sys_gps st;

	(void)user;
	if (sys_gps_parse_mgr(line, &l))
		return;		/* junk, unknown states */
	switch (l.type) {
	case SYS_GPS_MGR_HELLO:
		if (l.proto != MGR_PROTO && !G.proto_warned) {
			G.proto_warned = true;
			chefui_log("gps-manager: unknown protocol in %s (this client speaks proto %d); carrying on",
				   line, MGR_PROTO);
		}
		return;
	case SYS_GPS_MGR_OK:
		if (!strcmp(l.cmd, "watch")) {
			/* the lease waits for the switch line that follows */
			G.mgr.backoff_ms = BACKOFF_MIN_MS;
			G.watching = true;
		}
		return;
	case SYS_GPS_MGR_ERROR:
		if (!strcmp(l.cmd, "lease") && !strcmp(l.reason, "disabled"))
			G.lease_sent = false;	/* refused: none held */
		return;
	case SYS_GPS_MGR_SWITCH:
		enabled_set(l.enabled);
		lease_sync();		/* on again: lease again if it was refused */
		return;
	case SYS_GPS_MGR_STATE:
		break;
	default:
		return;
	}
	{
		int64_t now = mono_ms();

		G.acq_start_ms = l.up_ms >= 0 ? now - l.up_ms : 0;
		G.acq_fix_ms = l.up_ms >= 0 && l.fix_ms >= 0 ? G.acq_start_ms + l.fix_ms : 0;
		gps_view.start_ms = G.acq_start_ms;
		gps_view.first_fix_ms = G.acq_fix_ms;
		sys_changed(SYS_GPS_VIEW);
	}
	st = l.state;
	if (st == SYS_GPS_DISABLED)
		enabled_set(0);
	lease_sync();		/* a manager without switch lines */
	if (st == SYS_GPS_RUNNING) {
		gps_set(st, sys_state.gps.state == SYS_GPS_RUNNING ? sys_state.gps.mode : 0,
			sys_state.gps.state == SYS_GPS_RUNNING ? sys_state.gps.eph : -1);
	} else {
		gps_set(st, 0, -1);
	}
	if (gpsd_wanted()) {
		gpsd_start();
	} else {
		conn_cancel(&G.gpsd);
		if (G.watch && (gps_view.sky || gps_view.mode)) {
			view_clear();	/* no GNSS: nothing to show */
			sys_changed(SYS_GPS_VIEW);
		}
	}
}

static void mgr_lost(void)
{
	conn_cancel(&G.gpsd);
	G.lease_sent = G.watching = false;	/* the lease went with the connection */
	G.acq_start_ms = G.acq_fix_ms = 0;
	view_clear();				/* no manager: nothing to show */
	lease_end();
	sys_changed(SYS_GPS_VIEW);
	enabled_set(-1);
	gps_set(SYS_GPS_UNAVAILABLE, 0, -1);
	conn_retry_later(&G.mgr);
}

static void mgr_io(int fd, short revents, void *user)
{
	struct conn *c = &G.mgr;

	(void)user;
	if (c->connecting) {
		int err = 0;
		socklen_t l = sizeof(err);

		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l) || err ||
		    (revents & (POLLERR | POLLHUP))) {
			mgr_lost();
			return;
		}
		c->connecting = false;
		if (!send_str(fd, "watch\n")) {
			mgr_lost();
			return;
		}
		chefui_watch_fd(fd, POLLIN, mgr_io, NULL);
		return;
	}
	if (!conn_read(c, mgr_line))
		mgr_lost();
}

static void mgr_start(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	struct conn *c = &G.mgr;
	int fd;

	if (c->fd >= 0)
		return;
	memcpy(sa.sun_path, G.sock, sizeof(G.sock) < sizeof(sa.sun_path) ? sizeof(G.sock)
									     : sizeof(sa.sun_path));
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		mgr_lost();
		return;
	}
	c->fd = fd;
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) && errno != EINPROGRESS) {
		mgr_lost();	/* ENOENT/ECONNREFUSED/EAGAIN: not there yet */
		return;
	}
	c->connecting = true;
	chefui_watch_fd(fd, POLLOUT, mgr_io, NULL);
}

int sys_gps_start(const char *sock, int gpsd_port)
{
	if (G.sats_stale)
		lv_timer_delete(G.sats_stale);
	memset(&G, 0, sizeof(G));
	snprintf(G.sock, sizeof(G.sock), "%s", sock ? sock : MGR_SOCK);
	G.port = gpsd_port > 0 ? gpsd_port : GPSD_PORT;
	G.mgr = (struct conn){ .name = "gps-manager", .fd = -1, .backoff_ms = BACKOFF_MIN_MS,
			       .max_ms = MGR_BACKOFF_MAX_MS, .start = mgr_start };
	G.gpsd = (struct conn){ .name = "gpsd", .fd = -1, .backoff_ms = BACKOFF_MIN_MS,
				.max_ms = GPSD_BACKOFF_MAX_MS, .start = gpsd_start };
	G.running = true;
	sys_state.gps.enabled = -1;
	view_clear();
	mgr_start();
	return 0;
}

void sys_gps_stop(void)
{
	if (G.sats_stale) {
		lv_timer_delete(G.sats_stale);
		G.sats_stale = NULL;
	}
	G.running = false;
	conn_cancel(&G.gpsd);
	conn_cancel(&G.mgr);
	G.lease_sent = G.watching = false;
	G.acq_start_ms = G.acq_fix_ms = 0;
	view_clear();
	lease_end();
	sys_changed(SYS_GPS_VIEW);
}

void sys_gps_watch(bool watch)
{
	if (watch == G.watch)
		return;
	G.watch = watch;
	if (!watch) {
		view_clear();		/* positions are not kept off the page */
		lease_end();
		sys_changed(SYS_GPS_VIEW);
	}
	lease_sync();
	if (gpsd_wanted())
		gpsd_start();
	else if (sys_state.gps.state != SYS_GPS_RUNNING)
		conn_cancel(&G.gpsd);
}

bool sys_gps_watching(void)
{
	return G.watch;
}

int sys_gps_set_enabled(bool on)
{
	struct conn *c = &G.mgr;

	if (!G.running || c->fd < 0 || c->connecting || !G.watching)
		return -ENOTCONN;
	return send_str(c->fd, on ? "enable\n" : "disable\n") ? 0 : -EIO;
}
