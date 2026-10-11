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
#include "sysstate.h"

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
	CHECK(changes == (SYS_BATT | SYS_CLOCK | SYS_GPS | SYS_WIFI | SYS_BT | SYS_STORAGE));
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
	test_battery();
	test_clock();
	test_fake();
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp);
	if (system(cmd))
		fprintf(stderr, "cleanup of %s failed\n", tmp);
	return check_report("test-sys");
}
