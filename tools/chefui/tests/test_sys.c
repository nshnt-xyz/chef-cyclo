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
#include <pthread.h>
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
#include "timeview.h"
#include "deviceinfo.h"
#include "storage_usage.h"
#include "feedback.h"

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
	{
		long up = 7, fix = 7;

		CHECK(sys_gps_parse_acq("acq 84000 80000", &up, &fix) == 0 && up == 84000 && fix == 80000);
		CHECK(sys_gps_parse_acq("acq 0 -1", &up, &fix) == 0 && up == 0 && fix == -1);
		CHECK(sys_gps_parse_acq("acq -1 -1", &up, &fix) == 0 && up == -1 && fix == -1);
		CHECK(sys_gps_parse_acq("acq 5", &up, &fix) == -1 && sys_gps_parse_acq("acq x y", &up, &fix) == -1);
		CHECK(sys_gps_parse_acq("acq 5 6 7", &up, &fix) == -1 && sys_gps_parse_acq("acq -2 1", &up, &fix) == -1);
		CHECK(sys_gps_parse_acq("state OFF idle", &up, &fix) == -1 && sys_gps_parse_acq("", &up, &fix) == -1);
	}
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
	/* a SKY without satellites (gpsd's DOP-only line) updates the DOPs and keeps the satellites of
	 * the last full one, twice in a row as gpsd sends them; an empty array replaces them; junk;
	 * not a SKY */
	CHECK(gps_parse_sky_view("{\"class\":\"SKY\",\"hdop\":2.5,\"uSat\":2}", &v) == 0 && v.n_sats == 3 &&
	      fabs(v.hdop - 2.5) < 1e-9 && isnan(v.pdop) && gps_sats_used(&v) == 2);
	CHECK(gps_parse_sky_view("{\"class\":\"SKY\",\"hdop\":2.6,\"pdop\":3}", &v) == 0 && v.n_sats == 3 &&
	      fabs(v.hdop - 2.6) < 1e-9 && fabs(v.pdop - 3) < 1e-9 && gps_sats_used(&v) == 2);
	CHECK(gps_parse_sky_view(SKY, &v) == 0 && v.n_sats == 3 && gps_sats_used(&v) == 2);
	CHECK(gps_parse_sky_view("{\"class\":\"SKY\",\"hdop\":1.1,\"satellites\":[]}", &v) == 0 && v.n_sats == 0);
	CHECK(gps_parse_sky_view(SKY, &v) == 0 && v.n_sats == 3);
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
	return gps_view.mode == 3 && !isnan(gps_view.lat);
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
	/* the manager's bring-up has been running 84 s and had its first fix after 80 s: those are
	 * the counter and the first-fix time, not the page's lease */
	say(mgr.cfd, "state RUNNING CID 1, fix\nacq 84000 80000\n");
	say(gpsd.cfd, TPV3);
	say(gpsd.cfd, "\n");
	CHECK(fakeloop_run_until(view_fix, NULL, 1000));
	CHECK(gps_view.start_ms > 0 && gps_view.first_fix_ms - gps_view.start_ms == 80000);
	CHECK(gps_view.start_ms < gps_view.lease_ms);	/* the bring-up began before this lease */
	{
		int64_t start = gps_view.start_ms, fixed = gps_view.first_fix_ms;

		/* the page is left and seen again inside the manager's grace: the same bring-up goes on */
		mgr.n = 0;
		mgr.in[0] = 0;
		sys_gps_watch(false);
		CHECK(mgr_wait(&mgr, "release map\n", 1000));
		CHECK(gps_view.start_ms == start && gps_view.first_fix_ms == fixed);
		fakeloop_run_for(5000);
		mgr.n = 0;
		mgr.in[0] = 0;
		sys_gps_watch(true);
		CHECK(mgr_wait(&mgr, "lease map\n", 1000));
		CHECK(gps_view.start_ms == start && gps_view.first_fix_ms == fixed && gps_view.lease_ms > start);
		say(mgr.cfd, "ok lease map 1\nstate RUNNING CID 1, fix\n");
		fakeloop_run_for(100);
		/* gpsd was closed with the page; it is read again for the sky and the fix */
		if (gpsd.cfd >= 0) {
			close(gpsd.cfd);
			gpsd.cfd = -1;
			gpsd.n = 0;
		}
		CHECK(fakeloop_run_until(accepted, &gpsd, 4000));
		CHECK(mgr_wait(&gpsd, "?WATCH", 1000));
		say(gpsd.cfd, SKY);
		say(gpsd.cfd, "\n");
		say(gpsd.cfd, TPV3);
		say(gpsd.cfd, "\n");
		CHECK(fakeloop_run_until(view_fix, NULL, 1000));
		CHECK(gps_view.start_ms == start && gps_view.first_fix_ms == fixed);
	}

	/* gpsd's DOP-only SKY lines between the full ones keep the satellites (the flicker defect
	 * of the 2026-10-16 window run); no full SKY for 10 s clears them */
	say(gpsd.cfd, "{\"class\":\"SKY\",\"hdop\":2.4,\"uSat\":2}\n{\"class\":\"SKY\",\"hdop\":2.3,\"uSat\":2}\n");
	fakeloop_run_for(200);
	CHECK(gps_view.n_sats == 3 && gps_sats_used(&gps_view) == 2 && fabs(gps_view.hdop - 2.3) < 1e-9);
	for (int i = 0; i < 8; i++) {	/* a full SKY and two DOP-only lines each second: kept, the timer restarts */
		say(gpsd.cfd, SKY);
		say(gpsd.cfd, "\n{\"class\":\"SKY\",\"hdop\":2.3}\n{\"class\":\"SKY\",\"hdop\":2.3}\n");
		fakeloop_run_for(1000);
		CHECK(gps_view.n_sats == 3);
	}
	say(gpsd.cfd, "{\"class\":\"SKY\",\"hdop\":2.3}\n");	/* only DOP-only lines from now on */
	fakeloop_run_for(8500);
	CHECK(gps_view.n_sats == 3);
	fakeloop_run_for(1500);
	CHECK(gps_view.n_sats == 0 && gps_view.mode == 3 && gps_view.sky);	/* satellites gone, the fix stays */
	say(gpsd.cfd, SKY);
	say(gpsd.cfd, "\n");
	fakeloop_run_for(200);
	CHECK(gps_view.n_sats == 3);

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
	say(mgr.cfd, "ok enabled 0\nenabled 0\nstate STOPPING released\nstate DISABLED disabled\nacq -1 -1\n");
	want = SYS_GPS_DISABLED;
	CHECK(fakeloop_run_until(gps_is, &want, 1000) && sys_state.gps.enabled == 0);
	CHECK(fakeloop_run_until(peer_closed, &gpsd, 1000));
	close(gpsd.cfd);
	gpsd.cfd = -1;
	gpsd.n = 0;
	CHECK(gps_view.n_sats == 0 && isnan(gps_view.lat) && gps_view.mode == 0);
	CHECK(gps_view.start_ms == 0 && gps_view.first_fix_ms == 0);	/* the manager's bring-up is over */
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

static void test_timeview_parsers(void)
{
	char digits[8], full[24], meridiem[3];
	struct sys_time_report r;
	const char *gps = "47505300,GPS,1,1791448320.000,0,0,0,0,0,0,0,0,64,Normal\n";
	const char *net = "A1B2C3D4,192.0.2.1,2,1791448320.5,0,0,0,0,0,0,0,0,64,Normal\n";

	CHECK(sys_time_format(0, 0, false, digits, sizeof(digits), meridiem));
	CHECK(!strcmp(digits, "12:00") && !strcmp(meridiem, "am"));
	sys_time_full(12, 0, false, full, sizeof(full));
	CHECK(!strcmp(full, "12:00 pm"));
	CHECK(sys_time_format(23, 59, false, digits, sizeof(digits), meridiem));
	CHECK(!strcmp(digits, "11:59") && !strcmp(meridiem, "pm"));
	CHECK(sys_time_format(0, 0, true, digits, sizeof(digits), meridiem));
	CHECK(!strcmp(digits, "00:00") && !meridiem[0]);
	CHECK(!sys_time_format(24, 0, false, digits, sizeof(digits), meridiem));
	CHECK(!strcmp(digits, "--:--") && !meridiem[0]);
	CHECK(sys_time_parse_tracking(gps, strlen(gps), 1791448400, &r));
	CHECK(r.source == SYS_TIME_GPS && r.reference_epoch == 1791448320.0);
	CHECK(sys_time_parse_tracking(net, strlen(net), 1791448400, &r));
	CHECK(r.source == SYS_TIME_NETWORK);
	{
		const char *boot = "source=rtc-offset\nreason=restored\nsaved_age_s=42\n";
		const char *floor = "source=floor\nreason=first_boot\nsaved_age_s=na\n";
		const char *bad = "source=rtc-offset\nsource=floor\nreason=x\nsaved_age_s=1\n";
		CHECK(sys_time_parse_boot(boot, strlen(boot)) == SYS_TIME_RESTORED);
		CHECK(sys_time_parse_boot(floor, strlen(floor)) == SYS_TIME_FLOOR);
		CHECK(sys_time_parse_boot(bad, strlen(bad)) == SYS_TIME_UNKNOWN);
		CHECK(sys_time_parse_boot(boot, strlen(boot) - 1) == SYS_TIME_UNKNOWN);
	}
	{
		char bad[160];
		snprintf(bad, sizeof(bad), "%s", gps);
		bad[strlen(bad) - 1] = 0;
		CHECK(!sys_time_parse_tracking(bad, strlen(bad), 1791448400, &r));
		CHECK(!sys_time_parse_tracking(gps, strlen(gps) - 1, 1791448400, &r));
		snprintf(bad, sizeof(bad), "00000000,GPS,1,1791448320,0,0,0,0,0,0,0,0,64,Normal\n");
		CHECK(!sys_time_parse_tracking(bad, strlen(bad), 1791448400, &r));
		snprintf(bad, sizeof(bad), "47505300,GPS,1,nan,0,0,0,0,0,0,0,0,64,Normal\n");
		CHECK(!sys_time_parse_tracking(bad, strlen(bad), 1791448400, &r));
		snprintf(bad, sizeof(bad), "47505300,GPS,1,1791448320,0,0,0,0,0,0,0,0,64,Normal,extra\n");
		CHECK(!sys_time_parse_tracking(bad, strlen(bad), 1791448400, &r));
	}
	{
		/* the matrix of records that must not verify (each from a valid base) */
		static const char *const cases[] = {
			"47505300,NTP,1,1791448320.000,0,0,0,0,0,0,0,0,64,Normal\n",	/* GPS refid, other name */
			"7F7F0101,LOCL,1,1791448320.000,0,0,0,0,0,0,0,0,64,Normal\n",
			"7F7F0101,,1,1791448320.000,0,0,0,0,0,0,0,0,64,Normal\n",
			"7f7f0101,LOCL,1,1791448320.000,0,0,0,0,0,0,0,0,64,Normal\n",
			"7f7f0101,192.0.2.1,1,1791448320.000,0,0,0,0,0,0,0,0,64,Normal\n",
			"00000000,192.0.2.1,1,1791448320.000,0,0,0,0,0,0,0,0,64,Normal\n",
			"A1B2C3D4,192.0.2.1,1,1791448320.000,0,0,0,0,0,0,0,0,64,Not synchronised\n",
			"A1B2C3D4,192.0.2.1,0,1791448320.000,0,0,0,0,0,0,0,0,64,Normal\n",
			"A1B2C3D4,192.0.2.1,16,1791448320.000,0,0,0,0,0,0,0,0,64,Normal\n",
			"A1B2C3D4,192.0.2.1,2,1791448406.000,0,0,0,0,0,0,0,0,64,Normal\n",	/* after now + 5 */
			"A1B2C3D4,192.0.2.1,2,1767225599.000,0,0,0,0,0,0,0,0,64,Normal\n",	/* before 2026 */
			"A1B2C3D4,192.0.2.1,2,1791448320.000,nan,0,0,0,0,0,0,0,64,Normal\n",
			"A1B2C3D4,192.0.2.1,2,1791448320.000,0,inf,0,0,0,0,0,0,64,Normal\n",
			"A1B2C3D4,192.0.2.1,2,1791448320.000,0,0,0,0,0,0,0,Normal\n",		/* 13 columns */
			"A1B2C3D4,192.0.2.1,2,1791448320.000,0,0,0,0,0,0,0,0,0,64,Normal\n",	/* 15 columns */
			"A1B2C3D4,192.0.2.1,2,1791448320.000,0,0,0,0,0,0,0,0,64,Normal\n"
			"A1B2C3D4,192.0.2.1,2,1791448320.000,0,0,0,0,0,0,0,0,64,Normal\n",	/* two records */
		};
		char big[5000];

		for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
			CHECK(!sys_time_parse_tracking(cases[i], strlen(cases[i]), 1791448400, &r));
		/* a record longer than the 4096-byte cap */
		memset(big, 'a', sizeof(big));
		big[sizeof(big) - 1] = 0;
		memcpy(big, "A1B2C3D4,192.0.2.1,2,1791448320.000,0,0,0,0,0,0,0,0,64,Normal,", 63);
		big[sizeof(big) - 2] = '\n';
		CHECK(!sys_time_parse_tracking(big, strlen(big), 1791448400, &r));
		/* the valid network record next to them still passes (uppercase refid) */
		CHECK(sys_time_parse_tracking("A1B2C3D4,192.0.2.1,2,1791448320.000,0,0,0,0,0,0,0,0,64,Normal\n",
					      strlen("A1B2C3D4,192.0.2.1,2,1791448320.000,0,0,0,0,0,0,0,0,64,Normal\n"),
					      1791448400, &r) && r.source == SYS_TIME_NETWORK);
	}
}

static void test_deviceinfo_parsers(void)
{
	struct sys_image_info image;
	char serial[64], mac[18];
	char stamp[200];
	const char *cmd = "console=tty androidboot.serialno=ABC-123 root=/dev/mmcblk0";
	const char *address = "aa:bb:cc:00:12:fe\n";
	int n = snprintf(stamp, sizeof(stamp), "chef-image-info 1\nname chef-cyclo\nversion dev\n"
			 "commit 0123456789abcdef0123456789abcdef01234567\ndirty 1\nepoch 1790812800\n");

	CHECK(sys_image_info_parse(stamp, (size_t)n, &image));
	CHECK(!strcmp(image.name, "chef-cyclo") && !strcmp(image.version, "dev") && image.dirty);
	CHECK(image.epoch == 1790812800);
	CHECK(!sys_image_info_parse(stamp, (size_t)n - 1, &image));
	CHECK(!sys_image_info_parse("chef-image-info 1\nname chef-cyclo\nversion dev\ncommit bad\n"
				    "dirty 0\nepoch 1790812800\n", 83, &image));
	CHECK(sys_device_serial_parse(cmd, strlen(cmd), serial, sizeof(serial)) && !strcmp(serial, "ABC-123"));
	CHECK(!sys_device_serial_parse("noteandroidboot.serialno=bad", 30, serial, sizeof(serial)));
	CHECK(!sys_device_serial_parse("androidboot.serialno=a androidboot.serialno=b", 47,
				       serial, sizeof(serial)));
	CHECK(sys_device_mac_parse(address, strlen(address), mac) && !strcmp(mac, "AA:BB:CC:00:12:FE"));
	CHECK(!sys_device_mac_parse("00:00:00:00:00:0z", 17, mac));
	{
		struct sys_device_info_request request = { .width = 1080, .height = 2246 };
		struct sys_device_info info;
		char root[] = "/tmp/chef-deviceinfo-test.XXXXXX", path[256];
		const char *metadata = "chef-image-info 1\nname chef-cyclo\nversion dev\n"
			"commit 0123456789abcdef0123456789abcdef01234567\ndirty 1\nepoch 1790812800\n";
		CHECK(mkdtemp(root) != NULL);
		snprintf(path, sizeof(path), "%s/image-info", root);
		strcpy(request.metadata_path, path);
		snprintf(path, sizeof(path), "%s/cmdline", root);
		strcpy(request.cmdline_path, path);
		snprintf(path, sizeof(path), "%s/bt", root);
		strcpy(request.bluetooth_path, path);
		snprintf(path, sizeof(path), "%s/wifi", root);
		strcpy(request.wifi_path, path);
		strcpy(request.lvgl, "9.6.0");
		snprintf(path, sizeof(path), "%s/image-info", root);
		FILE *f = fopen(path, "w"); CHECK(f != NULL); if (f) { fputs(metadata, f); fclose(f); }
		snprintf(path, sizeof(path), "%s/cmdline", root);
		f = fopen(path, "w"); CHECK(f != NULL); if (f) { fputs(cmd, f); fclose(f); }
		snprintf(path, sizeof(path), "%s/bt", root);
		f = fopen(path, "w"); CHECK(f != NULL); if (f) { fputs(address, f); fclose(f); }
		snprintf(path, sizeof(path), "%s/wifi", root);
		f = fopen(path, "w"); CHECK(f != NULL); if (f) { fputs("00:11:22:33:44:55\n", f); fclose(f); }
		CHECK(sys_device_info_collect(&request, &info) == 0);
		CHECK(info.metadata_known && !strcmp(info.image_name, "chef-cyclo") && info.dirty);
		CHECK(info.serial_known && !strcmp(info.serial, "ABC-123"));
		CHECK(info.bluetooth_known && !strcmp(info.bluetooth, "AA:BB:CC:00:12:FE"));
		CHECK(info.wifi_known && !strcmp(info.wifi, "00:11:22:33:44:55"));
		CHECK(!strcmp(info.resolution, "1080 x 2246") && !strcmp(info.lvgl, "9.6.0"));
		CHECK(info.kernel[0] && info.sampled_mono_ms > 0);
		/* a real sysfs attribute reports st_size 4096: it must still read (the
		 * addresses were "Unavailable" on the phone until this was checked) */
		if (access("/sys/class/net/lo/address", R_OK) == 0) {
			strcpy(request.wifi_path, "/sys/class/net/lo/address");
			CHECK(sys_device_info_collect(&request, &info) == 0);
			CHECK(info.wifi_known && !strcmp(info.wifi, "00:00:00:00:00:00"));
		}
		unlink(request.metadata_path); unlink(request.cmdline_path);
		unlink(request.bluetooth_path); unlink(request.wifi_path); rmdir(root);
	}
}

struct small_stack_scan { const char *root; struct sys_usage_result u; };

static bool fixture_mountpoint(const char *path);

static void *small_stack_scan_run(void *arg)
{
	struct small_stack_scan *j = arg;

	sys_storage_usage_scan(j->root, fixture_mountpoint, &j->u);
	return NULL;
}

static bool fixture_mountpoint(const char *path)
{
	(void)path;
	return true;
}

static int mountpoint_calls;
static bool disappearing_mountpoint(const char *path)
{
	(void)path;
	return ++mountpoint_calls == 1;
}

static int capacity_calls;
static bool changing_capacity(const char *root)
{
	if (++capacity_calls == 2) {
		char path[512];
		FILE *f;
		snprintf(path, sizeof(path), "%s/v1/maps/capacity-change", root);
		f = fopen(path, "w");
		if (f) {
			for (int i = 0; i < 4096; i++) fputs("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n", f);
			fclose(f);
		}
	}
	return true;
}

static void remove_victim(const char *name, void *user)
{
	char path[600];

	if (strcmp(name, "victim"))
		return;
	snprintf(path, sizeof(path), "%s/v1/maps/victim", (const char *)user);
	(void)unlink(path);
}

static void test_storage_usage_scan(void)
{
	struct sys_usage_result u;
	char root[400], path[500], target[500];
	FILE *f;

	snprintf(root, sizeof(root), "%s/usage-data", tmp);
	CHECK(mkdir(root, 0700) == 0);
	snprintf(path, sizeof(path), "%s/chef-layout", root);
	f = fopen(path, "w"); CHECK(f != NULL); if (f) { fputs("chef-cyclo-data-v1\n", f); fclose(f); }
	snprintf(path, sizeof(path), "%s/v1", root); CHECK(mkdir(path, 0700) == 0);
	for (const char *cat[] = { "maps", "rides", "routes" }, **p = cat; p < cat + 3; p++) {
		snprintf(path, sizeof(path), "%s/v1/%s", root, *p);
		CHECK(mkdir(path, 0700) == 0);
	}
	snprintf(path, sizeof(path), "%s/v1/maps/map.bin", root);
	f = fopen(path, "w"); CHECK(f != NULL); if (f) { fputs("allocated map metadata", f); fclose(f); }
	snprintf(target, sizeof(target), "%s/v1/rides/map-alias", root);
	CHECK(link(path, target) == 0);
	snprintf(path, sizeof(path), "%s/v1/routes/route.bin", root);
	f = fopen(path, "w"); CHECK(f != NULL); if (f) { fputs("route", f); fclose(f); }
	snprintf(path, sizeof(path), "%s/v1/maps/nested", root);
	CHECK(mkdir(path, 0700) == 0);
	strcat(path, "/inside.bin");
	f = fopen(path, "w"); CHECK(f != NULL); if (f) { fputs("nested", f); fclose(f); }
	snprintf(path, sizeof(path), "%s/v1/maps/link", root);
	CHECK(symlink("/etc/passwd", path) == 0);
	snprintf(path, sizeof(path), "%s/v1/rides/pipe", root);
	CHECK(mkfifo(path, 0600) == 0);
	sys_storage_usage_scan(root, fixture_mountpoint, &u);
	CHECK(u.known && u.ours && u.complete);
	CHECK(u.maps_bytes > 0 && u.routes_bytes > 0 && u.rides_bytes == 0);
	CHECK(u.total_bytes > 0 && u.free_bytes <= u.total_bytes);
	CHECK(u.root_ino > 0 && u.mount_id > 0 && u.sampled_mono_ms > 0);
	CHECK(u.maps_bytes + u.rides_bytes + u.routes_bytes + u.other_bytes <=
	      u.total_bytes - u.free_bytes);
	/* the scan runs on the settings worker: musl gives a thread 128 KB of
	 * stack, so it must not keep its inode table there (it crashed the
	 * app on the phone while glibc's 8 MB hid it) */
	{
		pthread_attr_t attr;
		pthread_t th;
		struct small_stack_scan job = { .root = root };

		pthread_attr_init(&attr);
		CHECK(pthread_attr_setstacksize(&attr, 128 * 1024) == 0);
		CHECK(pthread_create(&th, &attr, small_stack_scan_run, &job) == 0);
		pthread_join(th, NULL);
		pthread_attr_destroy(&attr);
		CHECK(job.u.known && job.u.ours && job.u.complete && job.u.maps_bytes > 0);
	}
	capacity_calls = 0;
	sys_storage_usage_scan(root, changing_capacity, &u);
	CHECK(u.known && !u.complete); /* capacity changed between statvfs samples */
	{
		const struct sys_usage_limits limits[] = {
			{ .max_depth = 1, .max_entries = 100, .max_keys = 100, .budget_ns = 1000000000LL },
			{ .max_depth = 40, .max_entries = 1, .max_keys = 100, .budget_ns = 1000000000LL },
			{ .max_depth = 40, .max_entries = 100, .max_keys = 1, .budget_ns = 1000000000LL },
			{ .max_depth = 40, .max_entries = 100, .max_keys = 100, .budget_ns = 1 },
		};
		for (size_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
			sys_storage_usage_scan_limited(root, fixture_mountpoint, &limits[i], &u);
			CHECK(u.known && !u.complete);
		}
		char nested[500];
		struct stat nested_st;
		snprintf(nested, sizeof(nested), "%s/v1/maps/nested", root);
		CHECK(stat(nested, &nested_st) == 0);
		struct sys_usage_limits mount_mismatch = {
			.max_depth = 40, .max_entries = 100, .max_keys = 100,
			.budget_ns = 1000000000LL, .mismatch_ino = (uint64_t)nested_st.st_ino,
		};
		sys_storage_usage_scan_limited(root, fixture_mountpoint, &mount_mismatch, &u);
		CHECK(u.known && !u.complete); /* foreign mount ID subtree is skipped, totals are approximate */
	}
	/* Sparse files are charged by allocated blocks, not logical length. */
	snprintf(path, sizeof(path), "%s/v1/maps/sparse", root);
	int sparse = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	CHECK(sparse >= 0);
	if (sparse >= 0) {
		CHECK(ftruncate(sparse, 16 * 1024 * 1024) == 0);
		close(sparse);
	}
	sys_storage_usage_scan(root, fixture_mountpoint, &u);
	CHECK(u.known && u.complete);
	CHECK(u.maps_bytes < 16 * 1024 * 1024);
	mountpoint_calls = 0;
	sys_storage_usage_scan(root, disappearing_mountpoint, &u);
	CHECK(!u.known && !u.ours && !u.complete);
	/* Ownership loss is never represented as a known exact result. */
	snprintf(path, sizeof(path), "%s/chef-layout", root);
	CHECK(unlink(path) == 0);
	sys_storage_usage_scan(root, fixture_mountpoint, &u);
	CHECK(!u.known && !u.ours);
	CHECK(symlink("/", path) == 0);
	sys_storage_usage_scan(root, fixture_mountpoint, &u);
	CHECK(!u.known && !u.ours);
	CHECK(unlink(path) == 0);
	/* Recreate ownership and prove excessive nesting is incomplete. */
	f = fopen(path, "w"); CHECK(f != NULL); if (f) { fputs("chef-cyclo-data-v1\n", f); fclose(f); }
	char deep[500];
	snprintf(deep, sizeof(deep), "%s/v1/routes", root);
	for (int i = 0; i < 35; i++) {
		size_t n = strlen(deep);
		CHECK(n + 8 < sizeof(deep));
		if (n + 8 >= sizeof(deep)) break;
		strcat(deep, "/d");
		CHECK(mkdir(deep, 0700) == 0);
	}
	sys_storage_usage_scan(root, fixture_mountpoint, &u);
	CHECK(u.known && !u.complete);
	{
		char empty[400];
		snprintf(empty, sizeof(empty), "%s/usage-empty", tmp);
		CHECK(mkdir(empty, 0700) == 0);
		snprintf(path, sizeof(path), "%s/chef-layout", empty);
		f = fopen(path, "w"); CHECK(f != NULL); if (f) { fputs("chef-cyclo-data-v1\n", f); fclose(f); }
		sys_storage_usage_scan(empty, fixture_mountpoint, &u);
		CHECK(u.known && u.ours && u.complete);
		CHECK(!u.maps_bytes && !u.rides_bytes && !u.routes_bytes);
	}
}

static void test_storage_stat_invalidates_usage(void)
{
	struct ui_store_stat_result stat = {
		.known = true, .ours = true, .total_bytes = 1000, .free_bytes = 400,
		.root_dev = 1, .root_ino = 2,
	};
	sys_state.storage.known = true;
	sys_state.storage.ours = true;
	sys_state.storage.total_bytes = 1000;
	sys_state.storage.free_bytes = 400;
	sys_state.storage.root_dev = 1;
	sys_state.storage.root_ino = 2;
	sys_state.storage.usage_known = true;
	sys_state.storage.usage_total_bytes = 1000;
	sys_state.storage.usage_free_bytes = 400;
	sys_state.storage.usage_root_dev = 1;
	sys_state.storage.usage_root_ino = 2;
	sys_storage_test_apply_stat(&stat);
	CHECK(sys_state.storage.usage_known);
	stat.free_bytes--;
	sys_storage_test_apply_stat(&stat);
	CHECK(!sys_state.storage.usage_known && !sys_state.storage.usage_complete);
	sys_state.storage.usage_known = true;
	stat.free_bytes = 400;
	stat.root_ino++;
	sys_storage_test_apply_stat(&stat);
	CHECK(!sys_state.storage.usage_known);
}

/* --- review round 1: F3 (adopt the newest sample), F4 (mount point), F10 (per-category state) */

static struct sys_usage_result ok_usage(uint64_t total, uint64_t free_b)
{
	struct sys_usage_result u = { .known = true, .ours = true, .complete = true,
		.total_bytes = total, .free_bytes = free_b, .maps_bytes = 5, .root_dev = 1,
		.root_ino = 2, .mount_id = 7, .sampled_mono_ms = 100 };

	for (int i = 0; i < 3; i++)
		u.cat_state[i] = USAGE_CAT_EXACT;
	return u;
}

static void test_storage_usage_adopts_newest(void)
{
	struct ui_store_stat_result stat = { .known = true, .ours = true, .total_bytes = 1000,
		.free_bytes = 400, .root_dev = 1, .root_ino = 2 };
	struct sys_usage_result u = ok_usage(1000, 380);	/* capacity moved after the STAT */
	unsigned before;

	memset(&sys_state.storage, 0, sizeof(sys_state.storage));
	sys_storage_test_apply_stat(&stat);
	CHECK(sys_state.storage.free_bytes == 400);
	sys_storage_test_apply_usage(&u);
	CHECK(sys_state.storage.usage_known && sys_state.storage.free_bytes == 380 &&
	      sys_state.storage.usage_free_bytes == 380 && sys_state.storage.usage_complete);
	/* ownership lost: the cheap fields follow, no snapshot, and no job is asked for */
	sys_storage_usage_want(true);
	before = ui_store_test_usage_requests();
	u.ours = false;
	sys_storage_test_apply_usage(&u);
	CHECK(!sys_state.storage.ours && !sys_state.storage.usage_known &&
	      !sys_state.storage.usage_complete);
	CHECK(ui_store_test_usage_requests() == before);
	/* a STAT that differs invalidates an older snapshot and, with the page
	 * visible, asks for exactly one USAGE */
	u = ok_usage(1000, 380);
	sys_storage_test_apply_usage(&u);
	before = ui_store_test_usage_requests();
	stat.free_bytes = 300;
	sys_storage_test_apply_stat(&stat);
	CHECK(!sys_state.storage.usage_known);
	CHECK(ui_store_test_usage_requests() == before + 1);
	sys_storage_usage_want(false);
}

static void test_storage_usage_review(void)
{
	struct sys_usage_result u;
	struct sys_usage_limits lim = { .max_depth = 32, .max_entries = 10000, .max_keys = 10000,
		.budget_ns = 250000000LL };
	char root[400], path[500];
	struct stat st;
	FILE *f;

	/* F4: the production default (hook NULL) refuses a directory that is not a mount point */
	snprintf(root, sizeof(root), "%s/usage-notmount", tmp);
	CHECK(mkdir(root, 0700) == 0);
	snprintf(path, sizeof(path), "%s/chef-layout", root);
	f = fopen(path, "w");
	CHECK(f != NULL);
	if (f) {
		fputs("chef-cyclo-data-v1\n", f);
		fclose(f);
	}
	snprintf(path, sizeof(path), "%s/v1", root);
	CHECK(mkdir(path, 0700) == 0);
	snprintf(path, sizeof(path), "%s/v1/maps", root);
	CHECK(mkdir(path, 0700) == 0);
	snprintf(path, sizeof(path), "%s/v1/maps/a.bin", root);
	f = fopen(path, "w");
	CHECK(f != NULL);
	if (f) {
		fputs("map", f);
		fclose(f);
	}
	sys_storage_usage_scan(root, NULL, &u);
	CHECK(!u.known && !u.ours && !u.maps_bytes && !u.rides_bytes && !u.routes_bytes);

	/* F10: the entries bound inside Maps; Rides and Routes were never reached */
	snprintf(path, sizeof(path), "%s/v1/rides", root);
	CHECK(mkdir(path, 0700) == 0);
	snprintf(path, sizeof(path), "%s/v1/routes", root);
	CHECK(mkdir(path, 0700) == 0);
	for (int i = 0; i < 4; i++) {
		snprintf(path, sizeof(path), "%s/v1/maps/f%d", root, i);
		f = fopen(path, "w");
		CHECK(f != NULL);
		if (f) {
			fputs("x", f);
			fclose(f);
		}
	}
	lim.max_entries = 2;
	sys_storage_usage_scan_limited(root, fixture_mountpoint, &lim, &u);
	CHECK(u.known && !u.complete);
	CHECK(u.cat_state[0] == USAGE_CAT_PARTIAL && u.cat_state[1] == USAGE_CAT_UNSCANNED &&
	      u.cat_state[2] == USAGE_CAT_UNSCANNED);
	lim.max_entries = 10000;
	sys_storage_usage_scan_limited(root, fixture_mountpoint, &lim, &u);
	CHECK(u.known && u.complete);
	for (int i = 0; i < 3; i++)
		CHECK(u.cat_state[i] == USAGE_CAT_EXACT);

	/* an error inside Rides only (a foreign mount): Rides PARTIAL, Routes EXACT */
	snprintf(path, sizeof(path), "%s/v1/rides/foreign", root);
	f = fopen(path, "w");
	CHECK(f != NULL);
	if (f) {
		fputs("x", f);
		fclose(f);
	}
	CHECK(stat(path, &st) == 0);
	lim.mismatch_ino = (uint64_t)st.st_ino;
	sys_storage_usage_scan_limited(root, fixture_mountpoint, &lim, &u);
	CHECK(u.known && !u.complete);
	CHECK(u.cat_state[0] == USAGE_CAT_EXACT && u.cat_state[1] == USAGE_CAT_PARTIAL &&
	      u.cat_state[2] == USAGE_CAT_EXACT);
	lim.mismatch_ino = 0;
	CHECK(unlink(path) == 0);

	/* an entry deleted between readdir and open is not an error */
	snprintf(path, sizeof(path), "%s/v1/maps/victim", root);
	f = fopen(path, "w");
	CHECK(f != NULL);
	if (f) {
		fputs("x", f);
		fclose(f);
	}
	lim.before_open = remove_victim;
	lim.user = root;
	sys_storage_usage_scan_limited(root, fixture_mountpoint, &lim, &u);
	/* no category is cut short by it (complete itself may flip: the deletion
	 * moves the filesystem's free count during the walk) */
	CHECK(access(path, F_OK) != 0);
	CHECK(u.known);
	for (int i = 0; i < 3; i++)
		CHECK(u.cat_state[i] == USAGE_CAT_EXACT);
}

static void fb_sleep_ms(long ms)
{
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000 };

	nanosleep(&ts, NULL);
}

/* poll until cond holds, at most 2 s */
#define FB_WAIT(cond) ({ bool ok_ = false; \
	for (int i_ = 0; i_ < 2000 && !(ok_ = (cond)); i_++) fb_sleep_ms(1); ok_; })

/* A worker from an earlier start, held inside the sysfs open, can neither
 * clear the new worker's busy flag nor write its stale record. Every wait is
 * a poll on state, and the whole sequence stays inside the 100 ms expiry. */
static void test_feedback_restart_isolation(void)
{
	char fa[400], fb_[400], buf[16] = { 0 };
	int fda, fdb;
	uint64_t gen0;
	ssize_t n;

	CHECK(FB_WAIT(sys_feedback_test_live_workers() == 0));
	snprintf(fa, sizeof(fa), "%s/iso-a", tmp);
	snprintf(fb_, sizeof(fb_), "%s/iso-b", tmp);
	CHECK(mkfifo(fa, 0600) == 0);
	CHECK(mkfifo(fb_, 0600) == 0);
	unsigned opens0 = sys_feedback_test_opens();
	CHECK(sys_feedback_start(fa, true) == 0);
	CHECK(sys_feedback_request());
	CHECK(FB_WAIT(sys_feedback_test_opens() == opens0 + 1)); /* old worker is in open(A) */
	gen0 = sys_feedback_test_generation();
	sys_feedback_stop();
	CHECK(sys_feedback_start(fb_, true) == 0);
	CHECK(sys_feedback_test_generation() > gen0);
	sys_feedback_test_reset_rate();
	CHECK(sys_feedback_request());
	CHECK(FB_WAIT(sys_feedback_test_opens() == opens0 + 2)); /* new worker is in open(B) */
	fda = open(fa, O_RDONLY | O_NONBLOCK | O_CLOEXEC);	/* release the old worker */
	CHECK(fda >= 0);
	CHECK(FB_WAIT(sys_feedback_test_live_workers() == 1));
	CHECK(sys_feedback_test_busy());	/* its exit did not clear the new record's busy */
	fdb = open(fb_, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	CHECK(fdb >= 0);
	n = 0;
	for (int i = 0; i < 2000 && n < 3; i++) {
		ssize_t r = read(fdb, buf + n, 3 - (size_t)n);

		if (r > 0)
			n += r;
		else
			fb_sleep_ms(1);
	}
	CHECK(n == 3 && !strcmp(buf, "10\n"));
	CHECK(fda >= 0 && read(fda, buf, sizeof(buf)) == 0);	/* the old worker wrote nothing */
	if (fda >= 0)
		close(fda);
	if (fdb >= 0)
		close(fdb);
	sys_feedback_stop();
	CHECK(FB_WAIT(sys_feedback_test_live_workers() == 0));	/* worker state freed (LSan) */
}

static void test_feedback_worker(void)
{
	char path[400], fifo[400], target[400], buf[16] = { 0 };
	int fd;
	snprintf(path, sizeof(path), "%s/vibrator-enable", tmp);
	snprintf(target, sizeof(target), "%s/vibrator-file", tmp);
	snprintf(fifo, sizeof(fifo), "%s/vibrator-fifo", tmp);
	fd = open(target, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
	CHECK(fd >= 0);
	if (fd < 0) return;
	close(fd);
	CHECK(mkfifo(fifo, 0600) == 0);
	CHECK(symlink(target, path) == 0);
	CHECK(sys_feedback_start(path, true) == 0);
	for (int i = 0; i < 100 && !sys_feedback_test_signals_blocked(); i++) {
		struct timespec ts = { .tv_nsec = 1000000 };
		nanosleep(&ts, NULL);
	}
	CHECK(sys_feedback_test_signals_blocked());
	CHECK(sys_feedback_request());
	for (int i = 0; i < 100; i++) {
		struct timespec ts = { .tv_nsec = 10000000 };
		nanosleep(&ts, NULL);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd >= 0) { ssize_t n = read(fd, buf, sizeof(buf) - 1); close(fd); if (n > 0) break; }
	}
	CHECK(!strcmp(buf, "10\n"));
	CHECK(unlink(path) == 0);
	CHECK(symlink(fifo, path) == 0);
	{
		struct timespec ts = { .tv_nsec = 120000000 };
		nanosleep(&ts, NULL);
	}
	CHECK(sys_feedback_request());
	{
		struct timespec ts = { .tv_nsec = 120000000 };
		nanosleep(&ts, NULL);
	}
	CHECK(!sys_feedback_request()); /* the worker is held in FIFO open; no queued replay */
	sys_feedback_enabled(false);
	CHECK(!sys_feedback_request());
	sys_feedback_enabled(true);
	sys_feedback_screen_allowed(false);
	CHECK(!sys_feedback_request());
	fd = open(fifo, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	CHECK(fd >= 0);
	if (fd >= 0) {
		struct timespec ts = { .tv_nsec = 30000000 };
		nanosleep(&ts, NULL);
		memset(buf, 0, sizeof(buf));
		CHECK(read(fd, buf, sizeof(buf)) < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
		close(fd);
	}
	sys_feedback_screen_allowed(true);
	{
		struct timespec ts = { .tv_nsec = 120000000 };
		nanosleep(&ts, NULL);
	}
	sys_feedback_stop();
	CHECK(!sys_feedback_request());
	/* a failing node is logged once, not per request (N1) */
	{
		struct timespec gapt = { .tv_nsec = 130000000 };

		CHECK(sys_feedback_start("/nonexistent/vibrator/enable", true) == 0);
		CHECK(sys_feedback_request());		/* the worker's open fails */
		nanosleep(&gapt, NULL);
		(void)sys_feedback_request();		/* logs the recorded error */
		nanosleep(&gapt, NULL);
		(void)sys_feedback_request();
		nanosleep(&gapt, NULL);
		(void)sys_feedback_request();
		CHECK(fakeloop_log_count("haptics:") == 1);
		sys_feedback_stop();
	}
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
			  SYS_WIFI_VIEW | SYS_GPS_VIEW | SYS_BLE | SYS_TIME_VIEW | SYS_DEVICE_INFO | SYS_ALS));
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

	/* M4 fixtures: frozen clock, source model, storage usage, About facts */
	sys_clock_use_tz("IST-5:30");
	f = fopen(st, "w");
	fputs("storage 18200000000 32000000000\nstorageusage 6400000000 3100000000 300000000\n"
	      "clockepoch 1791449160\ntimesync gps 1791448320\n"
	      "about name chef-cyclo\nabout version 0.9.3\nabout serial CC-0193-4417\n"
	      "about commit 71163bd000000000000000000000000000000000\nabout builddate 8 Oct 2026\n"
	      "about bluetooth A4:C1:38:0F:22:91\nabout resolution 1080 x 2246\n"
	      "about uptime 11520\nabout dirty 1\n", f);
	fclose(f);
	CHECK(sys_fake_load(st) == 0);
	CHECK(sys_state.clock.valid && sys_state.clock.hour == 14 && sys_state.clock.minute == 16);
	CHECK(!strcmp(sys_state.clock.hhmm, "14:16") && !strcmp(sys_state.clock.zone, "IST"));
	CHECK(!strcmp(sys_state.clock.date, "Thursday 8 October 2026") &&
	      sys_state.clock.utc_offset_min == 330);
	CHECK(sys_state.timeview.verified && sys_state.timeview.source == SYS_TIME_GPS &&
	      sys_state.timeview.reference_epoch == 1791448320.0);
	CHECK(sys_state.storage.total_bytes == 32000000000ull && sys_state.storage.usage_known &&
	      sys_state.storage.usage_complete && sys_state.storage.usage_maps == 6400000000ull &&
	      sys_state.storage.usage_other == 4000000000ull);
	CHECK(sys_state.device.metadata_known && !strcmp(sys_state.device.version, "0.9.3") &&
	      sys_state.device.serial_known && sys_state.device.bluetooth_known &&
	      !sys_state.device.wifi_known && sys_state.device.uptime_s == 11520 &&
	      sys_state.device.dirty);
	/* non-verifying sources: only the boot provenance is set */
	f = fopen(st, "w");
	fputs("timesync restored 0\n", f);
	fclose(f);
	CHECK(sys_fake_load(st) == 0);
	CHECK(!sys_state.timeview.verified && sys_state.timeview.boot_source == SYS_TIME_RESTORED);
	/* a changed free size drops the old usage */
	f = fopen(st, "w");
	fputs("storage 1000 2000\n", f);
	fclose(f);
	CHECK(sys_fake_load(st) == 0);
	CHECK(!sys_state.storage.usage_known);
	/* bad lines change nothing */
	{
		static const char *const bad[] = {
			"clockepoch abc\n", "clockepoch 100\n", "timesync gps\n", "timesync moon 0\n",
			"timesync gps -5\n", "storageusage 1 2\n", "about colour red\n",
			"about bluetooth short\n", "about uptime x\n", "about dirty 2\n",
			"about name\n", "about version "
			"0123456789012345678901234567890123456789\n",
		};
		struct sys_state before = sys_state;

		for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
			f = fopen(st, "w");
			fputs(bad[i], f);
			fclose(f);
			CHECK(sys_fake_load(st) == -EINVAL);
			CHECK(!memcmp(&before.device, &sys_state.device, sizeof(before.device)));
		}
		CHECK(!memcmp(&before.timeview, &sys_state.timeview, sizeof(before.timeview)));
	}
	/* image facts come whole: a fixture with only some of them is rejected atomically */
	{
		struct sys_state keep = sys_state;

		memset(&sys_state.device, 0, sizeof(sys_state.device));
		f = fopen(st, "w");
		fputs("about name chef-cyclo\nabout version 0.9\nabout kernel Linux 4.4\n", f);
		fclose(f);
		CHECK(sys_fake_load(st) == -EINVAL);
		CHECK(!sys_state.device.metadata_known && !sys_state.device.image_name[0] &&
		      !sys_state.device.kernel[0]);
		f = fopen(st, "w");
		fputs("about kernel Linux 4.4\nabout lvgl 9.6.0\n", f);	/* no image facts at all */
		fclose(f);
		CHECK(sys_fake_load(st) == 0 && !sys_state.device.metadata_known &&
		      !strcmp(sys_state.device.kernel, "Linux 4.4"));
		sys_state = keep;
	}
	/* legacy "time" sets the 12-hour fields too, with no date */
	f = fopen(st, "w");
	fputs("time 09:30\n", f);
	fclose(f);
	CHECK(sys_fake_load(st) == 0);
	CHECK(sys_state.clock.valid && sys_state.clock.hour == 9 && sys_state.clock.minute == 30);

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
	test_timeview_parsers();
	test_deviceinfo_parsers();
	test_storage_usage_scan();
	test_storage_stat_invalidates_usage();
	test_storage_usage_adopts_newest();
	test_storage_usage_review();
	test_feedback_worker();
	test_feedback_restart_isolation();
	test_fake();
	test_wifi_model();
	test_ble_parser();
	test_ble_connection();
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp);
	if (system(cmd))
		fprintf(stderr, "cleanup of %s failed\n", tmp);
	return check_report("test-sys");
}
