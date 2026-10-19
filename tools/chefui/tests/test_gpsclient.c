/*
 * test_gpsclient.c - sys/gpsclient.c (the gps-manager and gpsd clients
 * without LVGL) with an injected clock and its own poll() loop, as the
 * ride recorder will run it, against a fake gps-manager on a real AF_UNIX
 * socket and a fake gpsd on TCP loopback: the manager's line parser, the
 * reconnect deadlines, the hello's proto logged once, leases asked only
 * after the watch reply and a switch that is not off, two kinds, the
 * "disabled" refusal and the lease asked again when the switch is back,
 * releases, gpsd only while RUNNING (or searching with the view wanted),
 * the bring-up's times in the caller's clock, the view and the
 * satellites going stale, the manager going away. Built without LVGL's
 * include path (Makefile: test-gpsclient), so an LVGL dependency of
 * gpsclient.c, gpsview.c or ../common/lineclient.c fails the build.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "check.h"
#include "gpsclient.h"

static void test_parse_mgr(void)
{
	struct gpsc_mgr_line l;

	CHECK(gpsclient_parse_mgr("{\"type\":\"hello\",\"daemon\":\"gps-manager\",\"proto\":1}", &l) == 0 &&
	      l.type == GPSC_MGR_HELLO && l.proto == 1);
	CHECK(gpsclient_parse_mgr("{\"type\":\"hello\",\"daemon\":\"gps-manager\"}", &l) == 0 &&
	      l.type == GPSC_MGR_HELLO && l.proto == -1);
	CHECK(gpsclient_parse_mgr("{\"type\":\"ok\",\"cmd\":\"watch\"}", &l) == 0 && l.type == GPSC_MGR_OK &&
	      !strcmp(l.cmd, "watch"));
	CHECK(gpsclient_parse_mgr("{\"type\":\"error\",\"cmd\":\"lease\",\"reason\":\"disabled\"}", &l) == 0 &&
	      l.type == GPSC_MGR_ERROR && !strcmp(l.cmd, "lease") && !strcmp(l.reason, "disabled"));
	CHECK(gpsclient_parse_mgr("{\"type\":\"ok\",\"cmd\":\"lease\",\"kind\":\"ride\",\"held\":1}", &l) == 0 &&
	      l.type == GPSC_MGR_OK && !strcmp(l.kind, "ride"));
	CHECK(gpsclient_parse_mgr("{\"type\":\"error\",\"reason\":\"too many clients\"}", &l) == 0 &&
	      l.type == GPSC_MGR_ERROR && l.cmd[0] == 0);
	CHECK(gpsclient_parse_mgr("{\"type\":\"switch\",\"enabled\":false}", &l) == 0 && l.type == GPSC_MGR_SWITCH &&
	      l.enabled == 0);
	CHECK(gpsclient_parse_mgr("{\"type\":\"switch\",\"enabled\":true,\"new\":[1]}", &l) == 0 && l.enabled == 1);
	CHECK(gpsclient_parse_mgr("{\"type\":\"switch\",\"enabled\":1}", &l) == -1);
	CHECK(gpsclient_parse_mgr("{\"type\":\"switch\"}", &l) == -1);
	CHECK(gpsclient_parse_mgr("{\"type\":\"state\",\"state\":\"RUNNING\",\"detail\":\"CID 1, fix\",\"up_ms\":84000,"
				"\"fix_ms\":80000}", &l) == 0 && l.type == GPSC_MGR_STATE && l.state == GPSC_RUNNING &&
	      l.up_ms == 84000 && l.fix_ms == 80000);
	CHECK(gpsclient_parse_mgr("{\"type\":\"state\",\"state\":\"ACQUIRING\",\"detail\":\"x\",\"up_ms\":0,\"fix_ms\":null}",
				&l) == 0 && l.state == GPSC_ACQUIRING && !strcmp(l.detail, "x") && l.up_ms == 0 && l.fix_ms == -1);
	CHECK(gpsclient_parse_mgr("{\"type\":\"state\",\"state\":\"OFF\",\"detail\":\"idle\",\"up_ms\":null,\"fix_ms\":null}",
				&l) == 0 && l.state == GPSC_OFF && !strcmp(l.detail, "idle") && l.up_ms == -1 && l.fix_ms == -1);
	/* a first fix without a bring-up, or out of range: none */
	CHECK(gpsclient_parse_mgr("{\"type\":\"state\",\"state\":\"OFF\",\"up_ms\":null,\"fix_ms\":5}", &l) == 0 &&
	      l.fix_ms == -1);
	CHECK(gpsclient_parse_mgr("{\"type\":\"state\",\"state\":\"OFF\",\"up_ms\":-2,\"fix_ms\":1}", &l) == 0 &&
	      l.up_ms == -1 && l.fix_ms == -1);
	CHECK(gpsclient_parse_mgr("{\"type\":\"state\",\"state\":\"STOPPING\"}", &l) == 0 && l.state == GPSC_STOPPING && l.detail[0] == 0);
	CHECK(gpsclient_parse_mgr("{\"type\":\"state\",\"state\":\"FAILED\"}", &l) == 0 && l.state == GPSC_FAILED);
	CHECK(gpsclient_parse_mgr("{\"type\":\"state\",\"state\":\"STARTING\"}", &l) == 0 && l.state == GPSC_STARTING);
	CHECK(gpsclient_parse_mgr("{\"type\":\"state\",\"state\":\"DISABLED\"}", &l) == 0 && l.state == GPSC_DISABLED);
	CHECK(gpsclient_parse_mgr("{\"type\":\"state\",\"state\":\"RUNNINGX\"}", &l) == -1);
	CHECK(gpsclient_parse_mgr("{\"type\":\"state\",\"state\":\"RUN\"}", &l) == -1);
	CHECK(gpsclient_parse_mgr("{\"type\":\"state\"}", &l) == -1);
	CHECK(gpsclient_parse_mgr("{\"type\":\"status\",\"state\":\"RUNNING\"}", &l) == 0 && l.type == GPSC_MGR_OTHER);
	CHECK(gpsclient_parse_mgr("{\"type\":\"future\"}", &l) == 0 && l.type == GPSC_MGR_OTHER);
	CHECK(gpsclient_parse_mgr("{\"state\":\"RUNNING\"}", &l) == -1);	/* no type */
	CHECK(gpsclient_parse_mgr("state RUNNING fix", &l) == -1);	/* the old text protocol */
	CHECK(gpsclient_parse_mgr("{\"type\":\"state\",\"state\":\"OFF\"} x", &l) == -1);
	CHECK(gpsclient_parse_mgr("", &l) == -1);

}

/* ---- fakes ---- */

static unsigned g_changes;	/* GPSC_CH_* seen */
static int g_logs;
static char g_log[256];

static void on_changed(struct gpsclient *gc, unsigned what, void *ctx)
{
	(void)gc;
	(void)ctx;
	g_changes |= what;
}

static void on_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(g_log, sizeof(g_log), fmt, ap);
	va_end(ap);
	g_logs++;
}

static int listen_unix(const char *path)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	snprintf(sa.sun_path, sizeof(sa.sun_path), "%.100s", path);
	unlink(path);
	CHECK(bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
	CHECK(listen(fd, 4) == 0);
	return fd;
}

static int listen_tcp(int *port)
{
	struct sockaddr_in sa = { .sin_family = AF_INET };
	socklen_t l = sizeof(sa);
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;

	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	CHECK(bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
	CHECK(listen(fd, 4) == 0);
	getsockname(fd, (struct sockaddr *)&sa, &l);
	*port = ntohs(sa.sin_port);
	return fd;
}

/* The recorder's loop: one poll round over both connections at NOW. */
static void pump(struct gpsclient *gc, int64_t now)
{
	struct pollfd pfd[2];

	for (int i = 0; i < 3; i++) {
		gpsclient_pollfds(gc, pfd);
		if (poll(pfd, 2, 50) <= 0)
			return;
		gpsclient_handle(gc, pfd, now);
	}
}

static int accept_one(int lfd)
{
	struct pollfd p = { lfd, POLLIN, 0 };

	if (poll(&p, 1, 1000) <= 0)
		return -1;
	return accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
}

static void say(int fd, const char *s)
{
	CHECK(send(fd, s, strlen(s), MSG_NOSIGNAL) == (ssize_t)strlen(s));
}

/* Everything the fake peer received so far. */
static void heard(int fd, char *buf, size_t len)
{
	struct pollfd p = { fd, POLLIN, 0 };
	size_t n = 0;

	while (n < len - 1 && poll(&p, 1, 20) > 0) {
		ssize_t r = recv(fd, buf + n, len - 1 - n, MSG_DONTWAIT);

		if (r <= 0)
			break;
		n += (size_t)r;
	}
	buf[n] = '\0';
}

#define HELLO "{\"type\":\"hello\",\"daemon\":\"gps-manager\",\"proto\":1}\n"
#define OK_WATCH "{\"type\":\"ok\",\"cmd\":\"watch\"}\n"
#define SWITCH(on) "{\"type\":\"switch\",\"enabled\":" on "}\n"
#define STATE(st, up, fix) "{\"type\":\"state\",\"state\":\"" st "\",\"detail\":\"d " st "\",\"up_ms\":" up ",\"fix_ms\":" fix "}\n"
#define DISABLED_ERR "{\"type\":\"error\",\"cmd\":\"lease\",\"reason\":\"disabled\"}\n"

static const char SKY[] = "{\"class\":\"SKY\",\"hdop\":0.8,\"satellites\":[{\"PRN\":5,\"el\":62.0,\"az\":40.0,"
	"\"ss\":44.0,\"used\":true,\"gnssid\":0,\"svid\":5},{\"PRN\":72,\"used\":false,\"gnssid\":6,\"svid\":8}]}\n";
static const char TPV3[] = "{\"class\":\"TPV\",\"mode\":3,\"lat\":-33.8569,\"lon\":151.2153,\"altMSL\":41.5,"
	"\"eph\":3.4,\"speed\":1.5}\n";

static void test_client(void)
{
	char dir[] = "/tmp/gpsclient-test.XXXXXX", sock[256], buf[1024];
	struct gpsclient gc;
	struct gpsclient_config cfg = { .changed = on_changed, .log = on_log };
	int lfd, glfd, port, m, g;
	int64_t t = 10000;

	CHECK(mkdtemp(dir) != NULL);
	snprintf(sock, sizeof(sock), "%s/gps-manager.sock", dir);
	glfd = listen_tcp(&port);
	cfg.mgr_path = sock;
	cfg.gpsd_port = port;
	gpsclient_init(&gc, &cfg);
	CHECK(gc.state == GPSC_NONE && gc.enabled == -1 && gc.eph == -1 && isnan(gc.view.lat));

	/* no manager yet: retries at 1, 2, 4 s */
	gpsclient_start(&gc, t);
	CHECK(gpsclient_deadline(&gc) == t + 1000);
	gpsclient_tick(&gc, t + 999);
	CHECK(gpsclient_deadline(&gc) == t + 1000);
	t += 1000;
	gpsclient_tick(&gc, t);
	CHECK(gpsclient_deadline(&gc) == t + 2000);

	/* the manager appears: watch, the hello's proto checked */
	lfd = listen_unix(sock);
	t += 2000;
	gpsclient_tick(&gc, t);
	pump(&gc, t);
	m = accept_one(lfd);
	CHECK(m >= 0);
	heard(m, buf, sizeof(buf));
	CHECK(strcmp(buf, "watch\n") == 0);
	/* the page wants a lease before anything is known: it waits */
	gpsclient_want_lease(&gc, GPSC_LEASE_MAP, true, t);
	gpsclient_want_view(&gc, true, t);
	say(m, "{\"type\":\"hello\",\"daemon\":\"gps-manager\",\"proto\":2}\n" OK_WATCH);
	pump(&gc, t);
	CHECK(g_logs == 1 && strstr(g_log, "unknown protocol") && strstr(g_log, "proto 1"));
	heard(m, buf, sizeof(buf));
	CHECK(buf[0] == '\0' && gc.lease_ms[GPSC_LEASE_MAP] == 0);	/* the switch is not known yet */
	g_changes = 0;
	say(m, SWITCH("true") STATE("OFF", "null", "null"));
	pump(&gc, t);
	heard(m, buf, sizeof(buf));
	CHECK(strcmp(buf, "lease map\n") == 0 && gc.lease_ms[GPSC_LEASE_MAP] == t);
	CHECK(gc.state == GPSC_OFF && gc.enabled == 1 && strcmp(gc.detail, "d OFF") == 0);
	CHECK((g_changes & GPSC_CH_STATE) && (g_changes & GPSC_CH_VIEW));
	CHECK(gpsclient_deadline(&gc) == -1);	/* up, gpsd not wanted, no sky */

	/* searching with the view wanted: gpsd at once; the bring-up's times
	 * in this clock */
	t += 500;
	say(m, "{\"type\":\"ok\",\"cmd\":\"lease\",\"kind\":\"map\",\"held\":1}\n" STATE("ACQUIRING", "4000", "null"));
	pump(&gc, t);
	CHECK(gc.state == GPSC_ACQUIRING && gc.start_ms == t - 4000 && gc.first_fix_ms == 0);
	CHECK(gc.view.start_ms == t - 4000);
	g = accept_one(glfd);
	CHECK(g >= 0);
	pump(&gc, t);
	heard(g, buf, sizeof(buf));
	CHECK(strncmp(buf, "?WATCH={\"enable\":true,\"json\":true};", 35) == 0);
	g_changes = 0;
	say(g, SKY);
	say(g, "{\"class\":\"TPV\",\"mode\":1}\n");
	pump(&gc, t);
	CHECK(gc.view.n_sats == 2 && gc.view.mode == 1 && gc.mode == 1 && (g_changes & GPSC_CH_VIEW));
	CHECK(gpsclient_view_deadline(&gc) == t + GPS_SATS_STALE_MS);
	CHECK(gpsclient_deadline(&gc) == t + GPS_SATS_STALE_MS);

	/* RUNNING: a fix; status mode kept only from RUNNING to RUNNING */
	t += 1000;
	say(m, STATE("RUNNING", "5000", "4800"));
	pump(&gc, t);
	CHECK(gc.state == GPSC_RUNNING && gc.mode == 0 && gc.eph == -1);
	CHECK(gc.start_ms == t - 5000 && gc.first_fix_ms == t - 200);
	say(g, TPV3);
	pump(&gc, t);
	CHECK(gc.mode == 3 && gc.eph == 3 && fabs(gc.view.eph - 3.4) < 1e-9 && fabs(gc.view.lat + 33.8569) < 1e-9);
	say(m, STATE("RUNNING", "6000", "4800"));
	pump(&gc, t);
	CHECK(gc.mode == 3 && gc.eph == 3);

	/* DOP-only SKY lines keep the satellites; no full one for 10 s clears them */
	say(g, "{\"class\":\"SKY\",\"hdop\":2.3}\n");
	pump(&gc, t);
	CHECK(gc.view.n_sats == 2 && fabs(gc.view.hdop - 2.3) < 1e-9);
	g_changes = 0;
	gpsclient_tick(&gc, t - 1000 + GPS_SATS_STALE_MS - 1);
	CHECK(gc.view.n_sats == 2 && !g_changes);
	gpsclient_tick(&gc, t - 1000 + GPS_SATS_STALE_MS);
	CHECK(gc.view.n_sats == 0 && gc.view.mode == 3 && (g_changes & GPSC_CH_VIEW));
	CHECK(gpsclient_view_deadline(&gc) == -1);

	/* the recorder's ride lease beside the page's map: both kinds */
	gpsclient_want_lease(&gc, GPSC_LEASE_RIDE, true, t);
	heard(m, buf, sizeof(buf));
	CHECK(strcmp(buf, "lease ride\n") == 0 && gc.lease_ms[GPSC_LEASE_RIDE] == t);
	say(m, "{\"type\":\"ok\",\"cmd\":\"lease\",\"kind\":\"ride\",\"held\":1}\n");
	pump(&gc, t);
	CHECK(gc.nfifo == 0);	/* every request answered */
	/* the view is let go: forgotten, gpsd stays for the status (RUNNING) */
	gpsclient_want_view(&gc, false, t);
	gpsclient_want_lease(&gc, GPSC_LEASE_MAP, false, t);
	heard(m, buf, sizeof(buf));
	CHECK(strcmp(buf, "release map\n") == 0 && gc.lease_ms[GPSC_LEASE_MAP] == 0);
	CHECK(isnan(gc.view.lat) && gc.view.mode == 0 && gc.view.start_ms == gc.start_ms);
	CHECK(lineclient_up(&gc.gpsd));
	say(g, TPV3);
	pump(&gc, t);
	CHECK(gc.view.mode == 0 && gc.mode == 3);	/* the status only */

	/* the switch goes off: GNSS down, gpsd closed; a lease asked meanwhile
	 * is refused and asked again when the switch is back */
	say(m, SWITCH("false") STATE("STOPPING", "null", "null") STATE("DISABLED", "null", "null"));
	pump(&gc, t);
	CHECK(gc.state == GPSC_DISABLED && gc.enabled == 0 && gc.start_ms == 0 && gc.mode == 0);
	CHECK(!lineclient_up(&gc.gpsd) && lineclient_deadline(&gc.gpsd) == -1);
	heard(g, buf, sizeof(buf));
	close(g);
	gpsclient_want_lease(&gc, GPSC_LEASE_MAP, true, t);
	heard(m, buf, sizeof(buf));
	CHECK(buf[0] == '\0');	/* off: not even asked */
	CHECK(gpsclient_set_enabled(&gc, true, t) == 0);
	heard(m, buf, sizeof(buf));
	CHECK(strcmp(buf, "enable\n") == 0);
	say(m, SWITCH("true") STATE("OFF", "null", "null"));
	pump(&gc, t);
	heard(m, buf, sizeof(buf));
	CHECK(strcmp(buf, "lease map\n") == 0);
	say(m, DISABLED_ERR);	/* a race with another disable */
	pump(&gc, t);
	CHECK(!gc.held[GPSC_LEASE_MAP] && gc.held[GPSC_LEASE_RIDE]);	/* ride was granted before */
	say(m, SWITCH("false") SWITCH("true"));
	pump(&gc, t);
	heard(m, buf, sizeof(buf));
	CHECK(strcmp(buf, "lease map\n") == 0);	/* asked again, ride not twice */

	/* the manager goes: everything about it forgotten, a retry in 1 s
	 * (the watch reply made it healthy) */
	t += 2000;
	close(m);
	pump(&gc, t);
	CHECK(gc.state == GPSC_NONE && gc.enabled == -1 && gc.detail[0] == '\0');
	CHECK(gc.lease_ms[GPSC_LEASE_MAP] == 0 && gc.lease_ms[GPSC_LEASE_RIDE] == 0 && !gc.watching);
	CHECK(gpsclient_set_enabled(&gc, false, t) == -ENOTCONN);
	CHECK(gpsclient_deadline(&gc) == t + 1000);
	/* back: watch, then both wanted leases again */
	t += 1000;
	gpsclient_tick(&gc, t);
	pump(&gc, t);
	m = accept_one(lfd);
	CHECK(m >= 0);
	say(m, HELLO OK_WATCH SWITCH("true") STATE("OFF", "null", "null"));
	pump(&gc, t);
	heard(m, buf, sizeof(buf));
	CHECK(strcmp(buf, "watch\nlease map\nlease ride\n") == 0);
	CHECK(g_logs == 1);	/* the unknown proto was logged once */

	gpsclient_stop(&gc, t);
	CHECK(gc.state == GPSC_NONE && gpsclient_deadline(&gc) == -1);
	CHECK(gc.mgr.fd < 0 && gc.gpsd.fd < 0);
	close(m);
	close(lfd);
	close(glfd);
	unlink(sock);
	rmdir(dir);
}

int main(void)
{
	test_parse_mgr();
	test_client();
	return check_report("test-gpsclient");
}
