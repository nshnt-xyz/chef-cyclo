/*
 * gps.c - GPS state for the status bar and hub tile.
 *
 * gps-manager (docs/features/gps.md): a `watch` connection on
 * /run/gps-manager.sock, never a lease (the settings app must not keep
 * the modem up). OFF/STOPPING = off, FAILED = failed, STARTING/ACQUIRING =
 * searching, RUNNING = fix. Reconnected with backoff (1 s doubling to 8 s)
 * when the manager is not there or goes away.
 *
 * gpsd (127.0.0.1:2947) only while RUNNING: ?WATCH JSON, TPV mode and eph
 * for "3D fix · ±3 m". Disconnected when the manager leaves RUNNING.
 *
 * Both sockets are nonblocking (connect included) and read at most a few
 * buffers per wakeup.
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

#include "chefui.h"
#include "linebuf.h"
#include "sys.h"
#include "sysstate.h"

#define MGR_SOCK "/run/gps-manager.sock"
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
} G;

/* ------------------------------------------------------------- parsers */

int sys_gps_parse_state(const char *line, enum sys_gps *out)
{
	static const struct { const char *name; enum sys_gps s; } map[] = {
		{ "OFF", SYS_GPS_OFF }, { "STOPPING", SYS_GPS_OFF }, { "FAILED", SYS_GPS_FAILED },
		{ "STARTING", SYS_GPS_SEARCHING }, { "ACQUIRING", SYS_GPS_SEARCHING },
		{ "RUNNING", SYS_GPS_RUNNING },
	};
	size_t n;

	if (strncmp(line, "state ", 6))
		return -1;
	line += 6;
	n = strcspn(line, " ");
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (strlen(map[i].name) == n && !strncmp(line, map[i].name, n)) {
			*out = map[i].s;
			return 0;
		}
	return -1;
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

static void gpsd_line(char *line, void *user)
{
	int mode;
	double eph;

	(void)user;
	if (!sys_gps_parse_tpv(line, &mode, &eph)) {
		/* eph rounded to a metre so small wobbles do not redraw */
		if (eph >= 0)
			eph = (double)(long)(eph + 0.5);
		gps_set(sys_state.gps.state, mode, eph);
		G.gpsd.backoff_ms = BACKOFF_MIN_MS;
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
			conn_retry_later(c);
			return;
		}
		c->connecting = false;
		if (!send_str(fd, "?WATCH={\"enable\":true,\"json\":true};\n")) {
			conn_retry_later(c);
			return;
		}
		chefui_watch_fd(fd, POLLIN, gpsd_io, NULL);
		return;
	}
	if (!conn_read(c, gpsd_line))
		conn_retry_later(c);
}

static void gpsd_start(void)
{
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)G.port) };
	struct conn *c = &G.gpsd;
	int fd;

	if (c->fd >= 0 || sys_state.gps.state != SYS_GPS_RUNNING)
		return;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		conn_retry_later(c);
		return;
	}
	c->fd = fd;
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
		c->connecting = true;	/* finish in gpsd_io on POLLOUT */
	} else if (errno == EINPROGRESS) {
		c->connecting = true;
	} else {
		conn_retry_later(c);
		return;
	}
	chefui_watch_fd(fd, POLLOUT, gpsd_io, NULL);
}

/* ------------------------------------------------------- gps-manager */

static void mgr_line(char *line, void *user)
{
	enum sys_gps st;

	(void)user;
	if (!strncmp(line, "ok watch", 8))
		G.mgr.backoff_ms = BACKOFF_MIN_MS;
	if (sys_gps_parse_state(line, &st))
		return;		/* ok/err replies, unknown states, junk */
	if (st == SYS_GPS_RUNNING) {
		gps_set(st, sys_state.gps.state == SYS_GPS_RUNNING ? sys_state.gps.mode : 0,
			sys_state.gps.state == SYS_GPS_RUNNING ? sys_state.gps.eph : -1);
		gpsd_start();
	} else {
		conn_cancel(&G.gpsd);
		gps_set(st, 0, -1);
	}
}

static void mgr_lost(void)
{
	conn_cancel(&G.gpsd);
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
	memset(&G, 0, sizeof(G));
	snprintf(G.sock, sizeof(G.sock), "%s", sock ? sock : MGR_SOCK);
	G.port = gpsd_port > 0 ? gpsd_port : GPSD_PORT;
	G.mgr = (struct conn){ .name = "gps-manager", .fd = -1, .backoff_ms = BACKOFF_MIN_MS,
			       .max_ms = MGR_BACKOFF_MAX_MS, .start = mgr_start };
	G.gpsd = (struct conn){ .name = "gpsd", .fd = -1, .backoff_ms = BACKOFF_MIN_MS,
				.max_ms = GPSD_BACKOFF_MAX_MS, .start = gpsd_start };
	G.running = true;
	mgr_start();
	return 0;
}

void sys_gps_stop(void)
{
	G.running = false;
	conn_cancel(&G.gpsd);
	conn_cancel(&G.mgr);
}
