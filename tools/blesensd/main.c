/*
 * blesensd - the one owner of the phone's BLE sensors (heart rate,
 * cycling power, speed and cadence): it pairs them, keeps them connected
 * while Bluetooth is on, holds their notifications for its whole life and
 * serves the values to any number of clients (the settings app now, the
 * ride recorder later). BlueZ ends a client's notification sessions when
 * that client leaves the bus, so one long-lived owner is needed. See
 * docs/features/bluetooth.md ("BLE sensors").
 *
 * Socket protocol on /run/chef/blesensd.sock: the shared local socket protocol
 * (docs/features/local-sockets.md), proto 1; the requests, replies and
 * events are in server.c's header.
 *
 * A device may carry several sensor kinds (a power meter with cadence, or
 * the PC emulator with all three): one record, one connection, one value
 * stream per kind. Records (address, kinds, paired date, a pending-forget
 * flag, name) are saved through `chef-state blesens-save` and read with
 * `chef-state blesens-load` at start, both as child processes in their own
 * process group (storage never blocks this loop), bounded, the whole group
 * killed at the bound, when the leader exits and at exit.
 *
 * Connections: BlueZ reconnects LE devices on its own only for profiles
 * that ask for it (HID, audio), not for these services, so while the
 * adapter is powered a saved sensor that is not connected gets
 * Device1.Connect again (an LE create-connection that waits for the
 * sensor's advertisement), with backoff 5 s doubling to 30 s. Sensors are
 * off most of the time, and every attempt keeps the radio scanning, so
 * this runs only while a client holds the sensors (`hold on`: the settings
 * pages while seen, a ride) or for a while (-r, 5 min) after this daemon
 * starts, the adapter comes on, bluetoothd appears, a sensor is paired or
 * a link drops; otherwise no attempt is made ("waiting") until the next
 * hold. A connection a sensor makes is kept either way. Once its
 * services are resolved, StartNotify on its measurement characteristics,
 * once per connection (BlueZ answers success for a session it already
 * has); the battery level comes from BlueZ's Battery1 where it exists,
 * else one read per connection and an owned battery notification session
 * where offered. Only notifications on a session this
 * connection started count as values; cached Value properties do not.
 * Pairing uses this daemon's NoInputNoOutput agent, default only during
 * the attempt (requests are answered only for the device being paired) and bonds (Device1.Pair) only when
 * Connect or StartNotify is refused for authentication.
 *
 * Every asynchronous call carries its own identity (the pairing attempt,
 * or the sensor and its connection epoch); calls that no longer apply are
 * cancelled, and a late reply that slips through is checked against the
 * identity and dropped.
 *
 * Everything runs on one poll loop: the bus (sd-bus, static basu on the
 * phone), the socket and its clients (tools/common/linesrv.c), the
 * chef-state children and a 1 s housekeeping tick. Nothing here logs a
 * value; kmsg (chef_log, stderr too) gets transitions.
 *
 * Usage: blesensd [-S SOCK] [-s STATE_CMD] [-w WHEEL_MM] [-r RETRY_S] [-T] [-v]
 *        blesensd [-S SOCK] status | list | watch | scan | pair ADDR |
 *                 cancel ADDR | forget ADDR                     (client)
 * -T divides every timer by 10 (host tests only).
 *
 * Sources, with the internal header blesensd.h: state.c (the state D,
 * helpers, lookups), server.c (clients, requests, the event lines),
 * bus.c (bus calls, object cache, signals, the agent, bus life),
 * link.c (values, connections, scanning), pair.c (pairing and forget),
 * records.c (the chef-state children), main.c (the main loop, main)
 * and cli.c (the command-line client); blesens.c is the pure
 * measurement code.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "blesensd.h"
#include "common/chef_paths.h"

static volatile sig_atomic_t g_stop;
static int g_sigpipe[2] = { -1, -1 };

/* ------------------------------------------------------------ main loop */

static void tick(void)
{
	int64_t now = bd_now_ms();
	static bool wanted;

	if (wanted != bd_links_wanted()) {
		wanted = !wanted;
		if (!wanted)
			chef_log("no sensor holder: reconnection paused");
		bd_links_sync();
	}
	bd_children_tick(now);
	bd_save_start();
	if (!D.bus && now >= D.bus_retry_ms)
		bd_bus_open();
	bd_agent_register(now);
	for (int i = 0; i < MAX_SENSORS; i++) {
		struct sensor *s = &D.sensors[i];

		if (!s->used)
			continue;
		if (D.loaded) {
			bd_forget_step(s, now);
			if (s->used && D.bluez)
				bd_sensor_keep(s, now);
		}
		/* values with no notification for STALE_MS go to null once */
		for (int k = 0; s->used && k < BS_KINDS; k++)
			if (s->last_ms[k] && now - s->last_ms[k] >= STALE_MS) {
				s->last_ms[k] = 0;
				bd_value_event(s, k);
			}
	}
	bd_pair_check();
	bd_scan_sync();
}

static void on_stop(int sig)
{
	(void)sig;
	g_stop = 1;
	if (write(g_sigpipe[1], "x", 1) < 0) {
		/* a wakeup is already pending */
	}
}

static void on_chld(int sig)
{
	(void)sig;
	if (write(g_sigpipe[1], "c", 1) < 0) {
		/* as above */
	}
}

static void loop_round(void)
{
	struct pollfd pfd[3 + 1 + MAX_CLIENTS];	/* ours, then linesrv's */
	int n = 0, iload = -1, isrv, nsrv = 0, timeout = TICK_MS;
	static int64_t next_tick;
	uint64_t usec = UINT64_MAX;
	int64_t now = bd_now_ms();

	if (next_tick - now < timeout)
		timeout = next_tick > now ? (int)(next_tick - now) : 0;
	pfd[n++] = (struct pollfd){ .fd = g_sigpipe[0], .events = POLLIN };
	if (D.bus) {
		int ev = sd_bus_get_events(D.bus);

		pfd[n++] = (struct pollfd){ .fd = D.bus_fd, .events = (short)(ev < 0 ? POLLIN : ev) };
		if (sd_bus_get_timeout(D.bus, &usec) >= 0 && usec != UINT64_MAX) {
			struct timespec ts;
			uint64_t now_us;
			int t;

			clock_gettime(CLOCK_MONOTONIC, &ts);
			now_us = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
			t = usec <= now_us ? 0 : (int)((usec - now_us + 999) / 1000);
			if (t < timeout)
				timeout = t;
		}
	}
	if (D.load_fd >= 0) {
		iload = n;
		pfd[n++] = (struct pollfd){ .fd = D.load_fd, .events = POLLIN };
	}
	isrv = n;
	if (D.srv)
		nsrv = linesrv_pollfds(D.srv, pfd + n, (int)(sizeof(pfd) / sizeof(pfd[0])) - n);
	n += nsrv;
	if (poll(pfd, (nfds_t)n, timeout) < 0 && errno != EINTR)
		return;
	if (pfd[0].revents & POLLIN) {
		char junk[32];

		while (read(g_sigpipe[0], junk, sizeof(junk)) > 0)
			;
	}
	if (iload >= 0 && D.load_fd >= 0 && (pfd[iload].revents & (POLLIN | POLLHUP | POLLERR)))
		bd_load_read();
	bd_reap();
	if (D.bus) {
		for (int i = 0; i < 64; i++) {
			int r = sd_bus_process(D.bus, NULL);

			if (r < 0) {
				bd_bus_lost("lost");
				break;
			}
			if (r == 0)
				break;
		}
	}
	if (D.srv)
		linesrv_handle(D.srv, pfd + isrv, nsrv);
	if (bd_now_ms() >= next_tick) {
		next_tick = bd_now_ms() + TICK_MS;
		tick();
	}
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	int opt, rc;

	D.sock_path = CHEF_BLESENSD_SOCK;
	D.state_cmd = "chef-state";
	D.wheel_mm = 2105;
	while ((opt = getopt(argc, argv, "+S:s:w:r:Tv")) != -1) {
		switch (opt) {
		case 'S': D.sock_path = optarg; break;
		case 's': D.state_cmd = optarg; break;
		case 'w': D.wheel_mm = atoi(optarg); break;
		case 'r': bd_retry_s = atoi(optarg); break;
		case 'T': bd_scale = 10; break;
		case 'v': bd_verbose = true; break;
		default:
			fprintf(stderr, "usage: blesensd [-S SOCK] [-s STATE_CMD] [-w WHEEL_MM] [-r RETRY_S] [-T] [-v]\n"
				"       blesensd [-S SOCK] status|list|watch|scan|pair ADDR|cancel ADDR|forget ADDR\n");
			return 64;
		}
	}
	if (optind < argc)
		return bd_cli_main(D.sock_path, argc - optind, argv + optind);
	if (D.wheel_mm < 500 || D.wheel_mm > 4000) {
		fprintf(stderr, "blesensd: wheel circumference %d mm out of range (500 to 4000)\n", D.wheel_mm);
		return 64;
	}
	chef_log_init("blesensd", true);
	bd_self = getpid();
	if (pipe2(g_sigpipe, O_CLOEXEC | O_NONBLOCK) < 0)
		return 1;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_stop;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sa.sa_handler = on_chld;
	sa.sa_flags = SA_NOCLDSTOP | SA_RESTART;
	sigaction(SIGCHLD, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	rc = bd_server_start(D.sock_path);
	if (rc < 0) {
		chef_log("listen %s: %s; exiting in 5 s", D.sock_path, strerror(-rc));
		sleep(5);
		return 1;
	}
	bd_load_start();
	bd_bus_open();
	chef_log("ready on %s (wheel %d mm)", D.sock_path, D.wheel_mm);
	while (!g_stop)
		loop_round();
	chef_log("exiting");
	bd_server_fini();
	/* the last save, bounded; then nothing of ours outlives us */
	{
		int64_t until = bd_now_ms() + EXIT_SAVE_MS;

		while ((D.save.pgid > 0 || D.save_pending) && bd_now_ms() < until) {
			if (D.save.pgid <= 0)
				bd_save_start();
			loop_round();
		}
		if (D.save.pgid > 0 || D.save_pending)
			chef_log("exiting with the sensor save unfinished");
		bd_child_kill(&D.save);
		bd_child_kill(&D.load);
	}
	if (D.scan_on && D.bus && D.adapter[0]) {
		sd_bus_message *m = NULL;

		if (sd_bus_message_new_method_call(D.bus, &m, BLUEZ, D.adapter, ADAPTER_IF, "StopDiscovery") >= 0)
			sd_bus_call(D.bus, m, 500000, NULL, NULL);	/* bounded */
		sd_bus_message_unref(m);
	}
	if (D.bus) {
		bd_pend_cancel(true, 0, 0, true);
		sd_bus_flush(D.bus);
		sd_bus_close(D.bus);
		sd_bus_unref(D.bus);
	}
	return 0;
}
