/*
 * test_sys.c - the system UI state sources against fakes, on fakeloop
 * with real time: line framing, the gps-manager and gpsd parsers, a fake
 * gps-manager socket (late start, partial and malformed lines, restart)
 * and a fake gpsd (connect only while RUNNING), battery sysfs fixtures
 * with uevents on a datagram socket, the clock under a POSIX TZ and the
 * fake-state file.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "check.h"
#include "fakeloop.h"
#include "linebuf.h"
#include "settings.h"
#include "sys.h"
#include "gpsview.h"
#include "sysstate.h"
#include "wifi.h"
#include "blesens.h"

static char tmp[256];
static unsigned changes;

static void on_change(unsigned what, void *user)
{
	(void)user;
	changes |= what;
}

/* ------------------------------------------------------------ linebuf */

static char got[8][64];
static int ngot;

static void collect(char *line, void *user)
{
	(void)user;
	if (ngot < 8)
		snprintf(got[ngot++], sizeof(got[0]), "%s", line);
}

static void test_linebuf(void)
{
	static struct linebuf b;
	char big[LINEBUF_SIZE + 100];

	ngot = 0;
	linebuf_feed(&b, "state ACQ", 9, collect, NULL);
	CHECK(ngot == 0);
	linebuf_feed(&b, "UIRING x\nok\r\nhal", 16, collect, NULL);
	CHECK(ngot == 2 && !strcmp(got[0], "state ACQUIRING x") && !strcmp(got[1], "ok"));
	linebuf_feed(&b, "f\n", 2, collect, NULL);
	CHECK(ngot == 3 && !strcmp(got[2], "half"));
	/* an over-long line is dropped whole, the next one survives */
	memset(big, 'x', sizeof(big));
	linebuf_feed(&b, big, sizeof(big), collect, NULL);
	linebuf_feed(&b, big, 50, collect, NULL);
	linebuf_feed(&b, "\nnext\n", 6, collect, NULL);
	CHECK(ngot == 4 && !strcmp(got[3], "next") && b.dropped == 1);
	linebuf_feed(&b, "\n", 1, collect, NULL);
	CHECK(ngot == 5 && got[4][0] == 0);
}

/* ------------------------------------------------------------ parsers */

static void test_parsers(void)
{
	enum sys_gps st;
	int mode;
	double eph;

	CHECK(sys_gps_parse_state("state OFF idle", &st) == 0 && st == SYS_GPS_OFF);
	CHECK(sys_gps_parse_state("state STOPPING -", &st) == 0 && st == SYS_GPS_OFF);
	CHECK(sys_gps_parse_state("state FAILED no modem", &st) == 0 && st == SYS_GPS_FAILED);
	CHECK(sys_gps_parse_state("state STARTING x", &st) == 0 && st == SYS_GPS_SEARCHING);
	CHECK(sys_gps_parse_state("state ACQUIRING", &st) == 0 && st == SYS_GPS_SEARCHING);
	CHECK(sys_gps_parse_state("state RUNNING fix", &st) == 0 && st == SYS_GPS_RUNNING);
	CHECK(sys_gps_parse_state("state DISABLED disabled", &st) == 0 && st == SYS_GPS_DISABLED);
	CHECK(sys_gps_parse_state("state RUNNINGX y", &st) == -1);
	CHECK(sys_gps_parse_state("state RUN", &st) == -1);
	CHECK(sys_gps_parse_state("ok watch", &st) == -1);
	CHECK(sys_gps_parse_state("err nope", &st) == -1);
	CHECK(sys_gps_parse_state("", &st) == -1);

	CHECK(sys_gps_parse_tpv("{\"class\":\"TPV\",\"device\":\"x\",\"mode\":3,\"lat\":1,\"eph\":2.7}",
				&mode, &eph) == 0 && mode == 3 && eph > 2.6 && eph < 2.8);
	CHECK(sys_gps_parse_tpv("{\"class\":\"TPV\",\"mode\":1}", &mode, &eph) == 0 && mode == 1 && eph < 0);
	CHECK(sys_gps_parse_tpv("{\"class\":\"SKY\",\"mode\":3}", &mode, &eph) == -1);
	CHECK(sys_gps_parse_tpv("{\"class\":\"TPV\"}", &mode, &eph) == -1);
	CHECK(sys_gps_parse_tpv("{\"class\":\"TPV\",\"mode\":9}", &mode, &eph) == -1);
	CHECK(sys_gps_parse_tpv("{\"class\":\"TPV\",\"mode\":2,\"eph\":-4}", &mode, &eph) == 0 && eph < 0);
	CHECK(sys_gps_parse_tpv("garbage", &mode, &eph) == -1);
}

/* ------------------------------------------------------- fake servers */

static int listen_unix(const char *path)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
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

struct acc {
	int lfd, cfd;
	char in[256];
	size_t n;
};

/* Accept (nonblocking) and read what the client sent. */
static bool server_poll(void *arg)
{
	struct acc *a = arg;
	struct pollfd p = { a->cfd >= 0 ? a->cfd : a->lfd, POLLIN, 0 };

	if (poll(&p, 1, 0) <= 0)
		return false;
	if (a->cfd < 0) {
		a->cfd = accept4(a->lfd, NULL, NULL, SOCK_CLOEXEC);
		return false;
	}
	ssize_t r = read(a->cfd, a->in + a->n, sizeof(a->in) - 1 - a->n);

	if (r > 0) {
		a->n += (size_t)r;
		a->in[a->n] = 0;
	}
	return a->n > 0 && strchr(a->in, '\n');
}

static bool accepted(void *arg)
{
	server_poll(arg);
	return ((struct acc *)arg)->cfd >= 0;
}

static bool gps_is(void *arg)
{
	return sys_state.gps.state == *(enum sys_gps *)arg;
}

static bool gps_mode3(void *arg)
{
	(void)arg;
	return sys_state.gps.mode == 3 && sys_state.gps.eph == 3;
}

static bool peer_closed(void *arg)
{
	struct acc *a = arg;
	char c;
	struct pollfd p = { a->cfd, POLLIN, 0 };

	return poll(&p, 1, 0) > 0 && read(a->cfd, &c, 1) == 0;
}

static void say(int fd, const char *s)
{
	CHECK(write(fd, s, strlen(s)) == (ssize_t)strlen(s));
}

static void test_gps(void)
{
	char sock[300];
	struct acc mgr = { .lfd = -1, .cfd = -1 }, gpsd = { .lfd = -1, .cfd = -1 };
	enum sys_gps want;
	int port;

	snprintf(sock, sizeof(sock), "%s/gps-manager.sock", tmp);
	gpsd.lfd = listen_tcp(&port);
	/* the manager is not there yet: unavailable, retrying */
	sys_gps_start(sock, port);
	fakeloop_run_for(200);
	CHECK(sys_state.gps.state == SYS_GPS_UNAVAILABLE);

	mgr.lfd = listen_unix(sock);
	CHECK(fakeloop_run_until(accepted, &mgr, 3000));
	CHECK(fakeloop_run_until(server_poll, &mgr, 1000) && !strcmp(mgr.in, "watch\n"));

	/* partial line, then the rest; a malformed line changes nothing */
	say(mgr.cfd, "ok watch\nstate ACQ");
	fakeloop_run_for(100);
	CHECK(sys_state.gps.state == SYS_GPS_UNAVAILABLE);
	say(mgr.cfd, "UIRING sats 3\nbogus line\nstate WEIRD x\n");
	want = SYS_GPS_SEARCHING;
	CHECK(fakeloop_run_until(gps_is, &want, 1000));
	CHECK(gpsd.cfd < 0);	/* no gpsd until RUNNING */
	fakeloop_run_for(100);
	CHECK(!accepted(&gpsd));

	/* RUNNING: gpsd ?WATCH, TPV in pieces */
	say(mgr.cfd, "state RUNNING fix\n");
	want = SYS_GPS_RUNNING;
	CHECK(fakeloop_run_until(gps_is, &want, 1000));
	CHECK(fakeloop_run_until(accepted, &gpsd, 2000));
	CHECK(fakeloop_run_until(server_poll, &gpsd, 1000) && strstr(gpsd.in, "?WATCH={\"enable\":true"));
	say(gpsd.cfd, "{\"class\":\"VERSION\"}\n{\"class\":\"TPV\",\"mo");
	say(gpsd.cfd, "de\":3,\"eph\":2.8}\n{\"class\":\"SKY\",\"satellites\":[]}\n");
	CHECK(fakeloop_run_until(gps_mode3, NULL, 1000));

	/* leaving RUNNING drops gpsd and the fix detail */
	say(mgr.cfd, "state STOPPING lease released\n");
	want = SYS_GPS_OFF;
	CHECK(fakeloop_run_until(gps_is, &want, 1000));
	CHECK(sys_state.gps.mode == 0 && sys_state.gps.eph < 0);
	CHECK(fakeloop_run_until(peer_closed, &gpsd, 1000));
	close(gpsd.cfd);
	gpsd.cfd = -1;

	/* the manager restarts: unavailable, then reconnect and watch again */
	close(mgr.cfd);
	close(mgr.lfd);
	unlink(sock);
	mgr.cfd = -1;
	mgr.n = 0;
	want = SYS_GPS_UNAVAILABLE;
	CHECK(fakeloop_run_until(gps_is, &want, 1000));
	mgr.lfd = listen_unix(sock);
	CHECK(fakeloop_run_until(accepted, &mgr, 4000));
	CHECK(fakeloop_run_until(server_poll, &mgr, 1000) && !strcmp(mgr.in, "watch\n"));
	say(mgr.cfd, "ok watch\nstate FAILED no modem owner\n");
	want = SYS_GPS_FAILED;
	CHECK(fakeloop_run_until(gps_is, &want, 1000));

	sys_gps_stop();
	CHECK(fakeloop_watched() == 0);
	close(mgr.cfd);
	close(mgr.lfd);
	close(gpsd.lfd);
}

/* gpsd 3.27-shaped reports */
static const char SKY[] = "{\"class\":\"SKY\",\"device\":\"udp://127.0.0.1:20175\",\"hdop\":0.8,"
	"\"pdop\":1.4,\"nSat\":3,\"uSat\":2,\"satellites\":[{\"PRN\":5,\"el\":62.0,\"az\":40.0,"
	"\"ss\":44.0,\"used\":true,\"gnssid\":0,\"svid\":5},{\"PRN\":72,\"el\":55.0,\"az\":250.0,"
	"\"ss\":37.0,\"used\":true,\"gnssid\":6,\"svid\":8},{\"PRN\":306,\"el\":10.0,\"az\":95.0,"
	"\"ss\":0.0,\"used\":false,\"gnssid\":2,\"svid\":36}]}";
static const char TPV3[] = "{\"class\":\"TPV\",\"device\":\"x\",\"mode\":3,\"time\":\"2026-10-11\","
	"\"lat\":-33.8569,\"lon\":151.2153,\"altHAE\":64.2,\"altMSL\":41.5,\"eph\":3.1,"
	"\"speed\":1.5,\"track\":10.0}";

static void test_gps_view_parsers(void)
{
	struct gps_view v;
	int used[4], seen[4];
	char b[8];

	memset(&v, 0, sizeof(v));
	CHECK(gps_parse_sky_view(SKY, &v) == 0 && v.sky && v.n_sats == 3);
	CHECK(fabs(v.hdop - 0.8) < 1e-9 && fabs(v.pdop - 1.4) < 1e-9);
	CHECK(v.sats[0].gnss == GNSS_GPS && v.sats[0].svid == 5 && v.sats[0].used &&
	      v.sats[0].az == 40 && v.sats[0].el == 62 && v.sats[0].ss == 44);
	CHECK(v.sats[1].gnss == GNSS_GLONASS && v.sats[1].svid == 8);
	CHECK(v.sats[2].gnss == GNSS_GALILEO && !v.sats[2].used && v.sats[2].ss == 0);
	CHECK(gps_sats_used(&v) == 2);
	gps_sat_label(&v.sats[1], b, sizeof(b));
	CHECK(!strcmp(b, "R08"));
	gps_sat_label(&v.sats[2], b, sizeof(b));
	CHECK(!strcmp(b, "E36"));
	gps_constellations(&v, used, seen);
	CHECK(used[0] == 1 && seen[0] == 1 && used[1] == 1 && seen[2] == 1 && used[2] == 0 && seen[3] == 0);
	/* a SKY without satellites; junk; not a SKY */
	CHECK(gps_parse_sky_view("{\"class\":\"SKY\",\"hdop\":2.5}", &v) == 0 && v.n_sats == 0 &&
	      fabs(v.hdop - 2.5) < 1e-9 && isnan(v.pdop));
	CHECK(gps_parse_sky_view("{\"class\":\"TPV\"}", &v) == -1);
	CHECK(gps_parse_sky_view("{\"class\":\"SKY\",\"satellites\":[{\"PRN\":", &v) == 0 && v.n_sats == 0);
	CHECK(gps_parse_tpv_view(TPV3, &v) == 0 && v.mode == 3);
	CHECK(fabs(v.lat + 33.8569) < 1e-9 && fabs(v.lon - 151.2153) < 1e-9);
	CHECK(fabs(v.alt - 41.5) < 1e-9 && fabs(v.eph - 3.1) < 1e-9 && fabs(v.speed - 1.5) < 1e-9);
	/* no fix: nothing about a position is kept */
	CHECK(gps_parse_tpv_view("{\"class\":\"TPV\",\"mode\":1,\"lat\":1.0,\"lon\":2.0}", &v) == 0 &&
	      v.mode == 1 && isnan(v.lat) && isnan(v.lon) && isnan(v.alt));
	CHECK(gps_parse_tpv_view("{\"class\":\"TPV\",\"mode\":3,\"lat\":95,\"lon\":200}", &v) == 0 &&
	      isnan(v.lat) && isnan(v.lon));
}

/* wait until the manager side has received text t */
static bool mgr_wait(struct acc *a, const char *t, int ms)
{
	for (int i = 0; i < ms / 10; i++) {
		server_poll(a);
		if (strstr(a->in, t))
			return true;
		fakeloop_run_for(10);
	}
	return false;
}

static bool view_sats(void *arg)
{
	return gps_view.n_sats == *(int *)arg;
}

static bool view_fix(void *arg)
{
	(void)arg;
	return gps_view.mode == 3 && !isnan(gps_view.lat) && gps_view.first_fix_ms > 0;
}

static bool view_empty(void *arg)
{
	(void)arg;
	return gps_view.mode == 0 && gps_view.n_sats == 0 && !gps_view.sky && isnan(gps_view.lat);
}

static bool enabled_is(void *arg)
{
	return sys_state.gps.enabled == *(int *)arg;
}

static void test_gps_page(void)
{
	char sock[300];
	struct acc mgr = { .lfd = -1, .cfd = -1 }, gpsd = { .lfd = -1, .cfd = -1 };
	enum sys_gps want;
	int port, n, en;
	int64_t first;

	snprintf(sock, sizeof(sock), "%s/gps-manager2.sock", tmp);
	gpsd.lfd = listen_tcp(&port);
	mgr.lfd = listen_unix(sock);
	sys_gps_start(sock, port);
	CHECK(fakeloop_run_until(accepted, &mgr, 3000));
	CHECK(mgr_wait(&mgr, "watch\n", 1000));
	say(mgr.cfd, "ok watch\nenabled 1\nstate OFF idle\n");
	en = 1;
	CHECK(fakeloop_run_until(enabled_is, &en, 1000));
	want = SYS_GPS_OFF;
	CHECK(fakeloop_run_until(gps_is, &want, 1000));

	/* the page is seen: one map lease on the watch connection */
	mgr.n = 0;
	mgr.in[0] = 0;
	sys_gps_watch(true);
	CHECK(mgr_wait(&mgr, "lease map\n", 1000) && gps_view.lease_ms > 0);
	/* searching: gpsd already, for the sky */
	say(mgr.cfd, "ok lease map 1\nstate ACQUIRING CID 1, no fix\n");
	CHECK(fakeloop_run_until(accepted, &gpsd, 3000));
	CHECK(mgr_wait(&gpsd, "?WATCH", 1000));
	say(gpsd.cfd, SKY);
	say(gpsd.cfd, "\n{\"class\":\"TPV\",\"mode\":1}\n");
	n = 3;
	CHECK(fakeloop_run_until(view_sats, &n, 1000) && gps_view.mode == 1 && !gps_view.first_fix_ms);
	/* a fix: first fix time in this lease */
	say(mgr.cfd, "state RUNNING CID 1, fix\n");
	say(gpsd.cfd, TPV3);
	say(gpsd.cfd, "\n");
	CHECK(fakeloop_run_until(view_fix, NULL, 1000));
	CHECK(gps_view.first_fix_ms >= gps_view.lease_ms);

	/* gpsd goes away: nothing stale stays on the page until it is back,
	 * but this lease's first fix keeps its time */
	first = gps_view.first_fix_ms;
	close(gpsd.cfd);
	gpsd.cfd = -1;
	gpsd.n = 0;
	CHECK(fakeloop_run_until(view_empty, NULL, 1000));
	CHECK(gps_view.lease_ms > 0 && gps_view.first_fix_ms == first);	/* still leased */
	CHECK(fakeloop_run_until(accepted, &gpsd, 4000));
	CHECK(mgr_wait(&gpsd, "?WATCH", 1000));
	say(gpsd.cfd, SKY);
	say(gpsd.cfd, "\n");
	say(gpsd.cfd, TPV3);
	say(gpsd.cfd, "\n");
	CHECK(fakeloop_run_until(view_fix, NULL, 1000));
	CHECK(gps_view.first_fix_ms == first);	/* not the reconnect's fix */

	/* switched off: the manager tears down (DISABLED); gpsd closed,
	 * nothing kept; the switch goes out on the same connection */
	mgr.n = 0;
	mgr.in[0] = 0;
	CHECK(sys_gps_set_enabled(false) == 0);
	CHECK(mgr_wait(&mgr, "disable\n", 1000));
	say(mgr.cfd, "ok enabled 0\nenabled 0\nstate STOPPING released\nstate DISABLED disabled\n");
	want = SYS_GPS_DISABLED;
	CHECK(fakeloop_run_until(gps_is, &want, 1000) && sys_state.gps.enabled == 0);
	CHECK(fakeloop_run_until(peer_closed, &gpsd, 1000));
	close(gpsd.cfd);
	gpsd.cfd = -1;
	gpsd.n = 0;
	CHECK(gps_view.n_sats == 0 && isnan(gps_view.lat) && gps_view.mode == 0);
	/* the manager restarts while off: the lease is refused, and taken
	 * again once the switch is back on */
	close(mgr.cfd);
	mgr.cfd = -1;
	mgr.n = 0;
	mgr.in[0] = 0;
	CHECK(fakeloop_run_until(accepted, &mgr, 4000));
	CHECK(mgr_wait(&mgr, "watch\n", 1000));
	say(mgr.cfd, "ok watch\nenabled 0\nstate DISABLED disabled\n");
	fakeloop_run_for(200);
	server_poll(&mgr);
	CHECK(!strstr(mgr.in, "lease"));		/* off: not even asked */
	say(mgr.cfd, "enabled 1\nstate OFF idle\n");
	CHECK(mgr_wait(&mgr, "lease map\n", 1000));
	say(mgr.cfd, "err disabled\n");	/* a race with another disable */
	fakeloop_run_for(100);
	mgr.n = 0;
	mgr.in[0] = 0;
	say(mgr.cfd, "enabled 0\nenabled 1\n");
	CHECK(mgr_wait(&mgr, "lease map\n", 1000));	/* asked again */

	/* the manager goes while a fix is shown: forgotten at once */
	say(mgr.cfd, "state RUNNING CID 1, fix\n");
	CHECK(fakeloop_run_until(accepted, &gpsd, 4000));
	CHECK(mgr_wait(&gpsd, "?WATCH", 1000));
	say(gpsd.cfd, TPV3);
	say(gpsd.cfd, "\n");
	CHECK(fakeloop_run_until(view_fix, NULL, 1000));
	close(mgr.cfd);
	mgr.cfd = -1;
	want = SYS_GPS_UNAVAILABLE;
	CHECK(fakeloop_run_until(gps_is, &want, 1000));
	CHECK(gps_view.mode == 0 && isnan(gps_view.lat) && gps_view.n_sats == 0 && gps_view.lease_ms == 0 &&
	      gps_view.first_fix_ms == 0);		/* the lease ended with the connection */
	close(gpsd.cfd);
	gpsd.cfd = -1;
	gpsd.n = 0;
	mgr.n = 0;
	mgr.in[0] = 0;
	CHECK(fakeloop_run_until(accepted, &mgr, 4000));
	CHECK(mgr_wait(&mgr, "watch\n", 1000));
	say(mgr.cfd, "ok watch\nenabled 1\nstate OFF idle\n");
	CHECK(mgr_wait(&mgr, "lease map\n", 1000));	/* the page still wants it */

	/* the page goes: release, the view forgotten */
	mgr.n = 0;
	mgr.in[0] = 0;
	sys_gps_watch(false);
	CHECK(mgr_wait(&mgr, "release map\n", 1000));
	CHECK(gps_view.lease_ms == 0 && gps_view.n_sats == 0 && isnan(gps_view.lat));

	sys_gps_stop();
	CHECK(fakeloop_watched() == 0);
	CHECK(sys_gps_set_enabled(true) == -ENOTCONN);
	close(mgr.cfd);
	close(mgr.lfd);
	close(gpsd.lfd);
}

/* ------------------------------------------------------------ battery */

static void put(const char *dev, const char *attr, const char *val)
{
	char p[400];
	int fd;

	snprintf(p, sizeof(p), "%s/ps/%s", tmp, dev);
	mkdir(p, 0755);
	snprintf(p, sizeof(p), "%s/ps/%s/%s", tmp, dev, attr);
	fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	CHECK(fd >= 0);
	CHECK(write(fd, val, strlen(val)) == (ssize_t)strlen(val));
	close(fd);
}

static bool batt_is(void *arg)
{
	int *w = arg;

	return sys_state.batt.known && sys_state.batt.capacity == w[0] && sys_state.batt.powered == w[1];
}

static void uevent(int fd, const char *subsystem)
{
	char msg[256];
	int n = snprintf(msg, sizeof(msg), "change@/devices/x/power_supply/battery%cACTION=change%c"
			 "SUBSYSTEM=%s%cPOWER_SUPPLY_NAME=battery", 0, 0, subsystem, 0);

	CHECK(send(fd, msg, (size_t)n + 1, 0) == n + 1);
}

static void test_battery(void)
{
	char ps[300];
	int sv[2], want[2];

	CHECK(sys_uevent_is_power_supply("a@b\0SUBSYSTEM=power_supply\0", 28));
	CHECK(!sys_uevent_is_power_supply("a@b\0SUBSYSTEM=power_supply_x\0", 30));
	CHECK(!sys_uevent_is_power_supply("a@b\0SUBSYSTEM=net\0", 19));

	snprintf(ps, sizeof(ps), "%s/ps", tmp);
	mkdir(ps, 0755);
	put("battery", "capacity", "78\n");
	put("battery", "status", "Discharging\n");
	put("usb", "online", "0\n");
	put("pc_port", "online", "0\n");
	/* no dc supply at all: counts as offline */
	CHECK(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) == 0);
	changes = 0;
	sys_battery_start(ps, sv[0]);
	CHECK(sys_state.batt.known && sys_state.batt.capacity == 78 && !sys_state.batt.powered);
	CHECK(changes & SYS_BATT);

	/* a non power_supply uevent does not re-read */
	put("battery", "capacity", "77\n");
	uevent(sv[1], "net");
	fakeloop_run_for(400);
	CHECK(sys_state.batt.capacity == 78);
	/* power_supply uevents: coalesced re-read */
	uevent(sv[1], "power_supply");
	uevent(sv[1], "power_supply");
	want[0] = 77, want[1] = 0;
	CHECK(fakeloop_run_until(batt_is, want, 1000));

	/* powered: a charger that is not charging, a PC port, garbage */
	put("battery", "status", "Not charging\n");
	uevent(sv[1], "power_supply");
	want[1] = 1;
	CHECK(fakeloop_run_until(batt_is, want, 1000));
	put("battery", "status", "Discharging\n");
	put("pc_port", "online", "1\n");
	uevent(sv[1], "power_supply");
	CHECK(fakeloop_run_until(batt_is, want, 1000));
	fakeloop_run_for(300);
	CHECK(sys_state.batt.powered);
	put("pc_port", "online", "x\n");
	sys_battery_refresh();
	CHECK(sys_state.batt.powered);
	put("pc_port", "online", "0\n");
	sys_battery_refresh();
	CHECK(!sys_state.batt.powered);
	/* unreadable status counts as powered; bad capacity is unknown */
	put("battery", "status", "Unknown\n");
	put("battery", "capacity", "abc\n");
	sys_battery_refresh();
	CHECK(!sys_state.batt.known && sys_state.batt.powered);
	put("battery", "capacity", "101\n");
	sys_battery_refresh();
	CHECK(!sys_state.batt.known);
	put("battery", "capacity", "15\n");
	sys_battery_refresh();
	CHECK(sys_state.batt.known && sys_state.batt.capacity == 15);

	sys_battery_stop();
	close(sv[1]);
}

/* -------------------------------------------------------------- clock */

static void test_clock(void)
{
	char want[8];
	time_t t;
	struct tm tm;

	changes = 0;
	CHECK(sys_clock_start("IST-5:30") == 0);
	t = time(NULL) + 5 * 3600 + 1800;
	gmtime_r(&t, &tm);
	strftime(want, sizeof(want), "%H:%M", &tm);
	/* a minute boundary between the two reads would differ by one */
	CHECK(!strcmp(sys_state.clock.hhmm, want) || sys_state.clock.hhmm[4] != want[4]);
	CHECK(!strcmp(sys_state.clock.zone, "IST"));
	CHECK(changes & SYS_CLOCK);
	sys_clock_set_tz("UTC0");
	t = time(NULL);
	gmtime_r(&t, &tm);
	strftime(want, sizeof(want), "%H:%M", &tm);
	CHECK(!strcmp(sys_state.clock.zone, "UTC"));
	CHECK(!strncmp(sys_state.clock.hhmm, want, 3));
	changes = 0;
	sys_clock_refresh();
	CHECK(!(changes & SYS_CLOCK));	/* no change, no notification */
	sys_clock_stop();
}

/* ------------------------------------------------------ storage, fake */

static void test_fake(void)
{
	char st[400];
	FILE *f;

	/* storage: tests/test_settings.c (it runs on the settings worker) */
	snprintf(st, sizeof(st), "%s/fake.state", tmp);
	f = fopen(st, "w");
	fputs("# design values\ntime 14:16\nzone IST\nbatt 78 unpowered\ngps running 3 3\n"
	      "wifi connected HomeNet\nbt connected 2\nstorage 18200000000\n", f);
	fclose(f);
	changes = 0;
	CHECK(sys_fake_load(st) == 0);
	CHECK(changes == (SYS_BATT | SYS_CLOCK | SYS_GPS | SYS_WIFI | SYS_BT | SYS_STORAGE |
			  SYS_WIFI_VIEW | SYS_GPS_VIEW | SYS_BLE));
	CHECK(!strcmp(sys_state.clock.hhmm, "14:16") && sys_state.batt.capacity == 78);
	CHECK(sys_state.gps.state == SYS_GPS_RUNNING && sys_state.gps.mode == 3);
	CHECK(sys_wifi_connected(&sys_state) && !strcmp(sys_state.wifi.ssid, "HomeNet"));
	CHECK(sys_state.bt.powered && sys_state.bt.connected == 2);
	CHECK(sys_state.storage.free_bytes == 18200000000ull);
	f = fopen(st, "w");
	fputs("gps sideways\n", f);
	fclose(f);
	CHECK(sys_fake_load(st) == -EINVAL);
	CHECK(sys_state.gps.state == SYS_GPS_RUNNING);	/* unchanged on error */
	CHECK(sys_fake_load("/nonexistent/x") == -ENOENT);

	/* Wi-Fi view lines: SSIDs with spaces, the active connection */
	f = fopen(st, "w");
	fputs("wifiactive wpa2 5180 82 192.168.1.42/24 192.168.1.1 full Home Net\n"
	      "wifinet wpa2/wpa3 2437 72 known Cafe Velo  2\nwifinet open 2462 30 - OpenRide\n"
	      "wifiscan 1\n", f);
	fclose(f);
	memset(&wifi_view, 0, sizeof(wifi_view));
	CHECK(sys_fake_load(st) == 0);
	CHECK(wifi_view.n == 2 && wifi_view.scanning);
	CHECK(!strcmp(wifi_view.nets[0].text, "Cafe Velo  2") && wifi_view.nets[0].known);
	CHECK(wifi_view.nets[0].sec == WIFI_SEC_WPA2_WPA3 && wifi_view.nets[0].freq == 2437);
	CHECK(wifi_view.nets[1].sec == WIFI_SEC_OPEN && !wifi_view.nets[1].known);
	CHECK(wifi_view.cur.valid && !strcmp(wifi_view.cur.text, "Home Net"));
	CHECK(!strcmp(wifi_view.cur.ip4, "192.168.1.42") && wifi_view.cur.prefix == 24);
	CHECK(wifi_view.cur.connectivity_known && wifi_view.cur.connectivity == 4);
	f = fopen(st, "w");
	fputs("wifinet wpa9 2437 72 - X\n", f);
	fclose(f);
	CHECK(sys_fake_load(st) == -EINVAL);
	f = fopen(st, "w");
	fputs("wifinet wpa2 2437 72 -\n", f);	/* no SSID */
	fclose(f);
	CHECK(sys_fake_load(st) == -EINVAL);
}

/* ------------------------------------------------------ Wi-Fi model */

static struct wifi_ap ap(const char *ssid, uint32_t flags, uint32_t wpa, uint32_t rsn,
			 uint32_t freq, int strength, const char *path)
{
	struct wifi_ap a = { .flags = flags, .wpa = wpa, .rsn = rsn, .freq = freq,
			     .strength = strength };

	a.ssid.len = (int)strlen(ssid);
	memcpy(a.ssid.b, ssid, (size_t)a.ssid.len);
	snprintf(a.path, sizeof(a.path), "%s", path);
	return a;
}

static void test_wifi_model(void)
{
	struct wifi_ap aps[8];
	struct wifi_net nets[8];
	struct wifi_ssid active = { .len = 7 }, known[1] = { { .len = 4 } }, raw;
	struct wifi_active cur = { .valid = true };
	char b[128];
	int n;

	/* security from NetworkManager's AP flags */
	CHECK(wifi_sec_from_flags(0, 0, 0) == WIFI_SEC_OPEN);
	CHECK(wifi_sec_from_flags(1, 0, 0) == WIFI_SEC_WEP);
	CHECK(wifi_sec_from_flags(1, 0x144, 0) == WIFI_SEC_WPA);
	CHECK(wifi_sec_from_flags(1, 0, 0x188) == WIFI_SEC_WPA2);
	CHECK(wifi_sec_from_flags(1, 0x144, 0x188) == WIFI_SEC_WPA2);
	CHECK(wifi_sec_from_flags(1, 0, 0x488) == WIFI_SEC_WPA3);
	CHECK(wifi_sec_from_flags(1, 0, 0x588) == WIFI_SEC_WPA2_WPA3);
	CHECK(wifi_sec_from_flags(1, 0, 0x288) == WIFI_SEC_ENTERPRISE);
	CHECK(wifi_sec_from_flags(1, 0, 0x2088) == WIFI_SEC_ENTERPRISE);
	CHECK(wifi_sec_from_flags(0, 0, 0x888) == WIFI_SEC_OWE);
	CHECK(wifi_sec_joinable(WIFI_SEC_OPEN) && wifi_sec_joinable(WIFI_SEC_WPA2_WPA3));
	CHECK(!wifi_sec_joinable(WIFI_SEC_WEP) && !wifi_sec_joinable(WIFI_SEC_ENTERPRISE) &&
	      !wifi_sec_joinable(WIFI_SEC_OWE));
	CHECK(!strcmp(wifi_sec_key_mgmt(WIFI_SEC_WPA3), "sae"));
	CHECK(!strcmp(wifi_sec_key_mgmt(WIFI_SEC_WPA2_WPA3), "wpa-psk"));
	CHECK(!wifi_sec_key_mgmt(WIFI_SEC_OPEN));
	CHECK(!strcmp(wifi_sec_long(WIFI_SEC_WPA2), "WPA2 Personal"));

	/* band and channel */
	CHECK(!strcmp(wifi_band(2412), "2.4 GHz") && wifi_channel(2412) == 1);
	CHECK(wifi_channel(2484) == 14 && wifi_channel(2472) == 13);
	CHECK(!strcmp(wifi_band(5180), "5 GHz") && wifi_channel(5180) == 36);
	CHECK(wifi_channel(5745) == 149);
	CHECK(!strcmp(wifi_band(5955), "6 GHz") && wifi_channel(5955) == 1);
	CHECK(!strcmp(wifi_band(0), "") && wifi_channel(0) == 0);

	/* signal */
	CHECK(wifi_signal_level(72) == 2 && wifi_signal_level(40) == 1 && wifi_signal_level(12) == 0);
	CHECK(!strcmp(wifi_signal_word(82), "strong") && !strcmp(wifi_signal_word(50), "good") &&
	      !strcmp(wifi_signal_word(30), "fair") && !strcmp(wifi_signal_word(5), "weak"));

	/* SSIDs that are not clean UTF-8 */
	raw.len = 6;
	memcpy(raw.b, "a\x01\xC3\xA9\xFFz", 6);
	wifi_ssid_text(&raw, b, sizeof(b));
	CHECK(!strcmp(b, "a?\xC3\xA9?z"));
	raw.len = 3;
	memcpy(raw.b, "\xC0\xAFx", 3);		/* overlong '/' */
	wifi_ssid_text(&raw, b, sizeof(b));
	CHECK(!strcmp(b, "??x"));
	raw.len = 4;
	memcpy(raw.b, "abcd", 4);
	wifi_ssid_text(&raw, b, 3);			/* truncated, terminated */
	CHECK(!strcmp(b, "ab"));

	/* grouping: strongest per SSID, hidden and active dropped, known
	 * marked, sorted by strength then name, capped */
	memcpy(active.b, "HomeNet", 7);
	memcpy(known[0].b, "Cafe", 4);
	aps[0] = ap("Cafe", 1, 0, 0x188, 2412, 40, "/ap/0");
	aps[1] = ap("Cafe", 1, 0, 0x188, 5180, 70, "/ap/1");
	aps[2] = ap("", 1, 0, 0x188, 2412, 90, "/ap/2");
	aps[3] = ap("HomeNet", 1, 0, 0x188, 5180, 90, "/ap/3");
	aps[4] = ap("Open", 0, 0, 0, 2462, 70, "/ap/4");
	aps[5] = ap("Weak", 1, 0, 0x188, 2462, 10, "/ap/5");
	n = wifi_group(aps, 6, &active, known, 1, nets, 8);
	CHECK(n == 3);
	CHECK(!strcmp(nets[0].text, "Cafe") && nets[0].strength == 70 && nets[0].freq == 5180 &&
	      !strcmp(nets[0].ap, "/ap/1") && nets[0].known);
	CHECK(!strcmp(nets[1].text, "Open") && !nets[1].known && nets[1].sec == WIFI_SEC_OPEN);
	CHECK(!strcmp(nets[2].text, "Weak"));
	CHECK(wifi_group(aps, 6, NULL, NULL, 0, nets, 2) == 2);
	CHECK(!strcmp(nets[0].text, "HomeNet"));	/* nothing active: listed */

	/* state reasons: no secrets, supplicant disconnect */
	CHECK(wifi_reason_bad_password(7) && wifi_reason_bad_password(8));
	CHECK(!wifi_reason_bad_password(11) && !wifi_reason_bad_password(0));

	/* texts */
	CHECK(wifi_group(aps, 6, &active, known, 1, nets, 8) == 3);
	wifi_net_subtitle(&nets[0], b, sizeof(b));
	CHECK(!strcmp(b, "Saved \xC2\xB7 WPA2 \xC2\xB7 5 GHz"));
	nets[1].freq = 0;
	wifi_net_subtitle(&nets[1], b, sizeof(b));
	CHECK(!strcmp(b, "Open"));
	nets[1].sec = WIFI_SEC_ENTERPRISE;
	wifi_net_subtitle(&nets[1], b, sizeof(b));
	CHECK(!strcmp(b, "Enterprise \xC2\xB7 not supported"));
	cur.dev_state = 100;
	cur.ap_known = true;
	cur.freq = 5180;
	wifi_active_subtitle(&cur, b, sizeof(b));
	CHECK(!strcmp(b, "Connected \xC2\xB7 5 GHz"));	/* no internet claim */
	cur.connectivity_known = true;
	cur.connectivity = 4;
	wifi_active_subtitle(&cur, b, sizeof(b));
	CHECK(!strcmp(b, "Connected \xC2\xB7 internet OK \xC2\xB7 5 GHz"));
	cur.connectivity = 2;
	wifi_active_subtitle(&cur, b, sizeof(b));
	CHECK(!strcmp(b, "Connected \xC2\xB7 sign-in needed \xC2\xB7 5 GHz"));
	cur.dev_state = 50;
	wifi_active_subtitle(&cur, b, sizeof(b));
	CHECK(!strcmp(b, "Connecting\xE2\x80\xA6"));

	/* the wipe */
	memcpy(b, "secret", 7);
	wifi_wipe(b, 7);
	CHECK(!memcmp(b, "\0\0\0\0\0\0\0", 7));
}


/* ------------------------------------------------------------ BLE sensors */
#define BA "AA:BB:CC:DD:EE:01"
#define BB "AA:BB:CC:DD:EE:02"
static const char BLE_SENSOR[] = "{\"type\":\"sensor\",\"addr\":\"" BA "\",\"kinds\":\"hr,power,csc\","
	"\"name\":\"Combo\",\"state\":\"connected\",\"battery\":80,\"rssi\":-58,\"rssi_live\":false}";
static const char BLE_VALUE[] = "{\"type\":\"value\",\"addr\":\"" BA "\",\"kind\":\"hr\",\"value\":321}";

static void test_ble_parser(void)
{
	struct ble_view v = {0}, keep;
	struct ble_row rows[24];
	const char *bad[] = {
		"{\"type\":\"adapter\",\"powered\":true}junk",
		"{\"type\":\"adapter\",\"powered\":true,}",
		"{\"type\":\"found\",\"addr\":\"" BB "\",\"name\":\"\\uXX00\"}",
		"{\"type\":\"found\",\"addr\":\"" BB "\",\"name\":\"\\u01\"}",
		"{\"type\":\"found\",\"addr\":\"" BB "\",\"name\":\"a\nb\"}",
		"{\"type\":\"value\",\"addr\":\"" BA "\",\"kind\":\"hr\",\"value\":NaN}",
		"{\"type\":\"value\",\"addr\":\"" BA "\",\"kind\":\"hr\",\"value\":1e999}",
		"{\"type\":\"value\",\"addr\":\"" BA "\",\"kind\":\"hr\",\"value\":01}",
		"{\"type\":\"value\",\"addr\":\"" BA "\",\"kind\":\"hr\",\"value\":1.}",
		"{\"type\":\"value\",\"addr\":\"" BA "\",\"kind\":\"hr\",\"value\":0x1}",
		"{\"type\":\"adapter\",\"type\":\"found\"}", "{", "[]", "", "{}junk",
	};
	CHECK(ble_apply_line(&v, BLE_SENSOR, 100));
	CHECK(v.n == 1 && v.s[0].kinds == 7 && v.s[0].battery == 80 && !v.s[0].rssi_live);
	CHECK(ble_apply_line(&v, BLE_VALUE, 100));
	CHECK(v.s[0].val[BLE_HR].has && v.s[0].val[BLE_HR].v == 321);
	CHECK(ble_apply_line(&v, "{\"type\":\"value\",\"addr\":\"" BA "\",\"kind\":\"power\",\"value\":-120,\"cadence\":90}", 100));
	CHECK(v.s[0].val[BLE_POWER].v == -120 && v.s[0].val[BLE_POWER].cadence == 90);
	CHECK(ble_rows(&v, rows, 24) == 3 && rows[0].kind == BLE_HR && rows[1].kind == BLE_POWER && rows[2].kind == BLE_CSC);
	CHECK(ble_rows_connected(&v) == 3);
	keep = v;
	for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
		CHECK(!ble_apply_line(&v, bad[i], 100));
		CHECK(!memcmp(&v, &keep, sizeof(v)));
	}
	CHECK(!ble_apply_line(&v, "{\"type\":\"mystery\"}", 100));
	CHECK(!ble_apply_line(&v, "{\"type\":\"value\",\"addr\":\"bad\",\"kind\":\"hr\",\"value\":1}", 100));
	CHECK(ble_apply_line(&v, "{\"type\":\"value\",\"addr\":\"" BA "\",\"kind\":\"hr\",\"value\":null}", 100));
	CHECK(!v.s[0].val[BLE_HR].has);
	CHECK(ble_apply_line(&v, "{\"type\":\"sensor\",\"addr\":\"" BA "\",\"kinds\":\"hr,power,csc\",\"state\":\"connecting\",\"battery\":\"bad\",\"rssi\":1e99}", 100));
	CHECK(!v.s[0].val[BLE_POWER].has && !v.s[0].val[BLE_POWER].has_cadence && v.s[0].battery == -1 && !v.s[0].has_rssi);
	CHECK(!ble_apply_line(&v, BLE_VALUE, 100)); /* delayed value without live link */
	CHECK(ble_apply_line(&v, "{\"type\":\"found\",\"addr\":\"" BB "\",\"name\":\"A\\\"B\\\\C\\/D\\n\\u0045\\u00e9\",\"kinds\":\"csc\"}", 100));
	CHECK(!strcmp(v.f[0].name, "A\"B\\C/DE?"));
	v.pair.st = BLE_PAIR_RUNNING;
	snprintf(v.pair.addr, sizeof(v.pair.addr), "%s", BA);
	CHECK(!ble_apply_line(&v, "{\"type\":\"pair\",\"addr\":\"" BB "\",\"state\":\"done\"}", 100));
	CHECK(ble_apply_line(&v, "{\"type\":\"pair\",\"addr\":\"" BA "\",\"state\":\"resolving\"}", 100));
	CHECK(ble_apply_line(&v, "{\"type\":\"pair\",\"addr\":\"" BA "\",\"state\":\"failed\",\"reason\":\"timed out\"}", 100));
	CHECK(v.pair.st == BLE_PAIR_FAILED && !strcmp(v.pair.reason, "timed out"));
	CHECK(!ble_apply_line(&v, "{\"type\":\"pair\",\"addr\":\"" BA "\",\"state\":\"done\"}", 100));
	CHECK(ble_apply_line(&v, "{\"type\":\"ok\",\"cmd\":\"watch\"}", 100));
	CHECK(v.known && !v.n && !v.n_found);
}

static bool ble_known(void *arg)
{
	return ble_view.known == *(bool *)arg;
}

static void test_ble_connection(void)
{
	char sock[300];
	struct acc a = { .lfd = -1, .cfd = -1 };
	bool known = true;
	struct ble_found f = { .addr = BA, .name = "Combo", .kinds = 7 };

	snprintf(sock, sizeof(sock), "%s/ble.sock", tmp);
	sys_ble_want(true, true);
	sys_ble_start(sock);
	fakeloop_run_for(100);
	CHECK(!ble_view.known && sys_ble_pair(&f) == -ENOTCONN);
	a.lfd = listen_unix(sock);
	CHECK(fakeloop_run_until(accepted, &a, 3000));
	CHECK(mgr_wait(&a, "watch\n", 1000));
	/* ack field order and whitespace are JSON, not a magic prefix */
	say(a.cfd, "{ \"cmd\": \"watch\", \"type\": \"ok\" }\n");
	CHECK(mgr_wait(&a, "scan on\n", 1000) && strstr(a.in, "hold on\n"));
	CHECK(fakeloop_run_until(ble_known, &known, 1000));
	say(a.cfd, BLE_SENSOR);
	say(a.cfd, "\n");
	say(a.cfd, BLE_VALUE);
	say(a.cfd, "\n");
	fakeloop_run_for(50);
	CHECK(ble_view.n == 1 && ble_view.s[0].val[BLE_HR].has);
	CHECK(sys_ble_pair(&f) == 0 && mgr_wait(&a, "pair " BA "\n", 1000));
	close(a.cfd); close(a.lfd); unlink(sock);
	a.cfd = -1; a.n = 0; a.in[0] = 0;
	known = false;
	CHECK(fakeloop_run_until(ble_known, &known, 1000));
	CHECK(!ble_view.n && !ble_view.n_found && !ble_view.powered && ble_view.pair.st == BLE_PAIR_FAILED);
	/* failed retries back off, and a new daemon gets wants again */
	fakeloop_run_for(1100);
	a.lfd = listen_unix(sock);
	fakeloop_run_for(200);
	CHECK(!accepted(&a));
	CHECK(fakeloop_run_until(accepted, &a, 4000));
	CHECK(mgr_wait(&a, "watch\n", 1000));
	say(a.cfd, "{\"type\":\"ok\",\"cmd\":\"watch\"}\n");
	CHECK(mgr_wait(&a, "scan on\n", 1000) && strstr(a.in, "hold on\n"));
	sys_ble_want(false, true); /* scan implies hold */
	CHECK(mgr_wait(&a, "scan off\n", 1000) && strstr(a.in, "hold off\n"));
	sys_ble_stop();
	CHECK(fakeloop_watched() == 0);
	close(a.cfd); close(a.lfd); unlink(sock);
}

int main(void)
{
	char cmd[300];

	snprintf(tmp, sizeof(tmp), "/tmp/chef-sys-test.XXXXXX");
	if (!mkdtemp(tmp))
		return 1;
	fakeloop_init(false);
	sys_on_change(on_change, NULL);
	test_linebuf();
	test_parsers();
	test_gps();
	test_gps_view_parsers();
	test_gps_page();
	test_battery();
	test_clock();
	test_fake();
	test_wifi_model();
	test_ble_parser();
	test_ble_connection();
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp);
	if (system(cmd))
		fprintf(stderr, "cleanup of %s failed\n", tmp);
	return check_report("test-sys");
}
