/*
 * sensor-proxy - net.hadess.SensorProxy on the system bus for chef-cyclo,
 * a bridge to sensord (docs/features/sensors.md). Standard clients
 * (monitor-sensor, desktop environments, anything written for
 * iio-sensor-proxy) see the accelerometer as orientation and tilt, the
 * ambient light in lux, proximity and the compass heading; sensord stays
 * the one owner of the sensors and does not change.
 *
 * The bus side (bus.c's header): the name, the two objects, claims per bus
 * name. A kind's first claimant sends sensord's claim, its last one gone
 * the release (proxy.c's header has the rules, and what each sensord line
 * changes; orient.c the orientation and tilt).
 *
 * sensord: a client of /run/chef/sensord/sensord.sock through
 * tools/common/lineclient (reconnects 1 s doubling to 30 s, back to 1 s
 * on a list reply with SMGR ready) and the shared JSON reader. After each
 * connect `list`, then the claims for every kind that has claimants. While
 * sensord is unreachable every Has* property is false (one
 * PropertiesChanged each when that changes) and the claims stay
 * registered on the bus side. A sensord error line for a channel is
 * logged; the property keeps its value.
 *
 * If the bus connection drops, the daemon exits with status 1 and its
 * supervisor restarts it (the claims of the old bus are gone with it
 * anyway). The log (chef_log: syslog, stderr too with -v) gets the
 * connects, losses, sensord's errors and the claims and releases sent,
 * never a sample.
 *
 * Usage: sensor-proxy [-S SOCK] [-v]
 *   -S  sensord's socket (default /run/chef/sensord/sensord.sock)
 *   -v  log to stderr as well
 * The bus is sd_bus_open_system()'s: DBUS_SYSTEM_BUS_ADDRESS when set
 * (the host tests point it at a private dbus-daemon).
 *
 * Files: orient.c orientation and tilt, proxy.c the claim bookkeeping and
 * sensord's lines (pure, with injected hooks), bus.c the bus side, this
 * file the loop; sensor-proxy.h is their internal header.
 * tests/test_orient.c covers orient.c, tests/test_bus_vtable.c bus.c's
 * method flags; tests/test_sensor_proxy.py runs the binary on a private
 * dbus-daemon against a fake sensord.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common/chef_log.h"
#include "common/chef_paths.h"
#include "common/lineclient.h"
#include "sensor-proxy.h"

static volatile sig_atomic_t g_stop;
static struct lineclient g_lc;
static struct proxy g_proxy;

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int send_sensord(void *ctx, const char *line)
{
	(void)ctx;
	return lineclient_send(&g_lc, line, now_ms());
}

static void on_connected(struct lineclient *lc, int64_t now)
{
	(void)lc;
	sp_connected(&g_proxy, now);
}

static void on_line(struct lineclient *lc, char *line, size_t len, int64_t now)
{
	if (sp_line(&g_proxy, line, len, now))
		lineclient_healthy(lc);
}

static void on_lost(struct lineclient *lc, int64_t now)
{
	(void)lc, (void)now;
	sp_lost(&g_proxy);
}

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

/* The earliest of two deadlines in ms from now, -1 for none. */
static int earliest(int a, int64_t at, int64_t now)
{
	int b;

	if (at < 0)
		return a;
	b = at <= now ? 0 : at - now > 60000 ? 60000 : (int)(at - now);
	return a < 0 || b < a ? b : a;
}

int main(int argc, char **argv)
{
	const char *sock = CHEF_SENSORD_SOCK;
	const struct sp_ops ops = { .send = send_sensord, .changed = sp_bus_changed, .log = chef_log };
	struct lineclient_config cfg = {
		.backoff_min_ms = SP_BACKOFF_MIN_MS, .backoff_max_ms = SP_BACKOFF_MAX_MS,
		.on_connected = on_connected, .on_line = on_line, .on_lost = on_lost,
	};
	bool verbose = false;
	int opt, r, status = 0;

	while ((opt = getopt(argc, argv, "S:v")) != -1) {
		switch (opt) {
		case 'S': sock = optarg; break;
		case 'v': verbose = true; break;
		default:
			fprintf(stderr, "usage: sensor-proxy [-S SOCK] [-v]\n");
			return 64;
		}
	}
	if (optind < argc) {
		fprintf(stderr, "usage: sensor-proxy [-S SOCK] [-v]\n");
		return 64;
	}
	chef_log_init("sensor-proxy", verbose);
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);

	sp_init(&g_proxy, &ops);
	r = sp_bus_open(&g_proxy);
	if (r < 0) {
		chef_log("system bus: net.hadess.SensorProxy: %s; exiting in 5 s", strerror(-r));
		sleep(5);	/* keeps its supervisor's respawn from spinning */
		return 1;
	}
	cfg.path = sock;
	lineclient_init(&g_lc, &cfg);
	lineclient_start(&g_lc, now_ms());
	chef_log("ready: net.hadess.SensorProxy on the system bus, sensord at %s", sock);

	while (!g_stop) {
		struct pollfd pfd[2];
		int64_t now = now_ms();
		int timeout;

		pfd[0].fd = sp_bus_pollfd(&pfd[0].events, &timeout);
		pfd[1] = lineclient_pollfd(&g_lc);
		timeout = earliest(timeout, lineclient_deadline(&g_lc), now);
		timeout = earliest(timeout, sp_deadline(&g_proxy), now);
		if (poll(pfd, 2, timeout) < 0 && errno != EINTR) {
			chef_log("poll: %s; exiting for a respawn", strerror(errno));
			status = 1;
			break;
		}
		if (g_stop)
			break;
		r = sp_bus_process();
		if (r < 0) {
			chef_log("system bus lost (%s); exiting for a respawn", strerror(-r));
			status = 1;
			break;
		}
		now = now_ms();
		if (pfd[1].fd >= 0 && pfd[1].revents)
			lineclient_handle(&g_lc, pfd[1].revents, now);
		now = now_ms();
		if (lineclient_deadline(&g_lc) >= 0 && now >= lineclient_deadline(&g_lc))
			lineclient_tick(&g_lc, now);
		sp_tick(&g_proxy, now);
	}
	chef_log("exiting");
	sp_bus_close();		/* sensord drops the claims with the connection */
	return status;
}
