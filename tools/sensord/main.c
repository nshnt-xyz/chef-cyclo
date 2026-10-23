/*
 * sensord - on-board sensors (accelerometer, gyroscope, magnetometer,
 * ambient light, proximity) for chef-cyclo. See docs/research/
 * sensors.md for the research behind every choice here.
 *
 * On SDM636 the sensor core (Qualcomm "SMGR", its drivers and fusion
 * algorithms) runs inside the ADSP that audio-up boots. The ADSP talks
 * the Sensors1 QMI protocol over the IPC router (AF_MSM_IPC); stock
 * Android runs two apps-side pieces for it, which this one daemon
 * replaces, in one event loop:
 *
 *  1. REG2 server, QMI service 0x10f instance 0x0002 (stock: sensors.qti).
 *     The ADSP reads its configuration from it: driver UUIDs, I2C bus and
 *     address, axis orientation, calibration. Served from a RAM copy of
 *     persist's sns.reg (sensors-up copies it to /run and never leaves
 *     persist mounted), laid out by a map sns-reg-map.py extracted from
 *     the stock sensors.qti at image build time. Writes from the DSP
 *     change the RAM copy and are logged; 3 s after the last one, and on
 *     exit, the RAM copy is written back over the /run file (atomic
 *     rename, tmpfs/ramfs only), so the /run copy holds what the DSP
 *     wrote this boot (the ADSP keeps its own state across a sensord
 *     restart; the copy matters when the ADSP restarts or re-reads the
 *     registry). persist is never written. After each write-back,
 *     `chef-state mag-save` saves the magnetometer bias (group 2980) to
 *     /data, at most once a minute (-k, regserv.c).
 *  2. SMGR client, QMI service 0x100 on the ADSP (stock: libsensor1 +
 *     sensors.ssc.so). Enumerates the sensors (all-sensor-info,
 *     single-sensor-info), then adds a BUFFERING report (0x21, what the
 *     stock HAL sends) per claimed sensor at the highest rate any client
 *     asked for, deletes it when the last claim goes, and decodes the
 *     indications (0x22). -P uses the older periodic REPORT (0x02/0x03)
 *     instead, in case the live run shows buffering refused.
 *  3. Optional (-t) TIME2 server, 0x118 instance 0x3202 (stock:
 *     sensors.qti). Nothing shows the DSP needs it; it is here so the
 *     live run can turn it on if the DSP is seen looking it up.
 *  4. Optional (-Q) QMAG_CAL client, SAM service 0x140 instance 0x3201 on
 *     the ADSP: the DSP's dynamic hard-iron calibration of the
 *     magnetometer, for the calibration check in docs/research/
 *     sensors.md section 7. While magn is in use (claimed by a
 *     client, or internally by the heading channel) it queries the
 *     algorithm attributes (0x24, once per daemon) and enables the
 *     algorithm (0x02); it logs every report (0x05: bias, accuracy) and
 *     error (0x06) indication, adds the latest bias to magn lines and to
 *     status, and disables it (0x03) when magn is no longer used, when
 *     SMGR is lost and on exit. Off by default: production behaviour is
 *     unchanged without the flag.
 *  5. The compass (docs/research/sensors.md section 8): a virtual
 *     "heading" channel. Claiming it claims accel 50 Hz, anglvel 50 Hz and
 *     magn 20 Hz internally (the same refcounts and rate max as client
 *     claims) and runs compass.c on their samples; heading lines go out
 *     at the client's rate. "calibrated" comes from the registry: the
 *     bias the ADSP learns and writes to group 2980 (items 3903..3905),
 *     checked at load and after each DSP write. -M picks the mount, -D
 *     or the "declination" request adds true_heading.
 *  6. Optional (-V) rotation vector client, SAM service 0x112 instance
 *     0x3201: the ADSP's own 9-axis fusion as a "rotvec" channel for
 *     comparison, with the same safety rules as QMAG_CAL (4.): single
 *     ENABLE, stray instances disabled, disable on release, SMGR loss
 *     and exit.
 *
 * Data model (IIO channel names and units, so a later kernel IIO driver
 * or an iio-sensor-proxy facade changes the transport, not the data):
 *   accel        m/s^2   x y z
 *   anglvel      rad/s   x y z
 *   magn         gauss   x y z
 *   illuminance  lux     illuminance
 *   proximity    -       near (0/1) and proximity (d[1] as the DSP sends
 *                         it, unscaled: the stock HAL only logs it)
 *   heading      deg     heading pitch roll calibrated disturbed accuracy
 *                         [true_heading declination] (compass.c)
 *   rotvec       -       x y z w accuracy coord [heading pitch roll] (-V)
 * SMGR reports Q16 fixed point in its own axis frame; the stock HAL maps
 * it to the Android frame (x right, y up, z out of the screen) as
 * x = d[1], y = d[0], z = -d[2] for accel, gyro and mag, and so does
 * this (disassembly of sensors.ssc.so's processReportInd()s). Magnetic
 * data is Gauss (the HAL multiplies by 100 for uT), light is lux in d[0],
 * proximity is "near" when d[0] / 65536 truncates to non-zero (the HAL's
 * fcvtzs(d[0] * 2^-16) != 0). The HAL also smooths the magnetometer with
 * an 8-sample moving average before Android sees it; sensord publishes
 * the samples unsmoothed, so a compass consumer should filter them.
 *
 * Timestamps: SMGR stamps samples with the DSP's 32768 Hz tick counter
 * (u32). The kernel's /dev/sensors DSPS_IOCTL_READ_SLOW_TIMER returns the
 * same counter (QTimer * 16 / 9375, kernel/drivers/sensors/sensors_ssc.c),
 * so each indication is anchored by reading it back to back with
 * CLOCK_MONOTONIC. -T cntvct reads the QTimer directly instead; -T rx
 * pins the newest sample of each indication to its receive time.
 *
 * Socket API: /run/chef/sensord/sensord.sock, plain-text requests, one JSON
 * object per line back; the protocol is in server.c's header. A sensor is
 * powered (report added on the DSP) only while at least one connection claims
 * it; a claim lives on its connection.
 *
 * Usage: sensord [-r REG] [-m MAP] [-k CMD] [-S SOCK] [-T dsps|cntvct|rx]
 *                [-b HZ] [-s CH=ID:DT]... [-c CH=full|factory|raw]...
 *                [-i BASE] [-M MOUNT] [-D DEG] [-Q] [-V] [-P] [-t] [-R]
 *                [-F] [-v]
 *   -r  registry copy to serve (default /run/chef/sensord/sns.reg)
 *   -m  registry map (default /usr/share/sensord/sns_reg.map)
 *   -k  run `CMD mag-save` after a registry write-back, with
 *       CHEF_STATE_SENSORS_RUN (REG's directory) and CHEF_STATE_MAP (MAP)
 *       in its environment (default chef-state; "" turns it off)
 *   -S  socket path (default /run/chef/sensord/sensord.sock)
 *   -T  timestamp source (default dsps, falls back to rx if /dev/sensors
 *       cannot be opened)
 *   -b  cap the SMGR report rate (indications/s) below the sample rate,
 *       so samples arrive batched; default 0 = one report per sample, as
 *       the stock HAL does
 *   -s  map a channel to another SMGR sensor ID/data type (accel=0:0)
 *   -c  calibration SMGR applies to a channel's samples (the BUFFERING
 *       item's calibration field): full (0, default: factory plus any
 *       dynamic calibration), factory (1, what the stock HAL asks for its
 *       "uncalibrated" sensors), raw (2). Repeatable, one per channel.
 *   -i  SMGR report ID base: channel N uses report ID BASE+N+1 (default
 *       0). For a second SMGR client beside the main one (with -R), so
 *       the two never share a report ID even if the DSP keys reports by
 *       ID rather than per client
 *   -M  heading mount: portrait (default), landscape-left, landscape-right,
 *       flat, upright (compass.h)
 *   -D  declination in degrees (east positive): heading lines add
 *       true_heading; the "declination" request changes it at run time
 *   -Q  run the QMAG_CAL client (see 4. above)
 *   -V  run the rotation vector client (see 6. above)
 *   -P  use periodic REPORT (0x02) instead of BUFFERING (0x21)
 *   -t  also serve TIME2 (0x118 inst 0x3202) on the apps node
 *   -R  do not serve REG2 (for a run that tests SMGR without it)
 *   -F  allow claims on channels the sensor info did not list
 *   -v  log every REG2/TIME2 request and SMGR exchange to stderr
 * Client mode, for the telnet shell:
 *   sensord [-S SOCK] [-n COUNT] list | status | get SENSOR [HZ] | watch SENSOR [HZ]
 *                                | declination [DEG|off]
 *
 * Sources, with the internal header sensord.h: channel.c (channels,
 * sample lines, timestamps, logs), server.c (clients and the socket
 * protocol), heading.c (the compass glue, 5.), smgr.c (the SMGR client,
 * 2.), sam.c (the SAM clients, 4. and 6.), regserv.c (the REG2/TIME2
 * servers, 1. and 3.), main.c (options, the poll loop, exit) and cli.c
 * (the command-line client).
 *
 * Pure parts (channel/rate bookkeeping, sample conversion, client queue
 * and protocol) are covered by tests/test_sensord.c, which links every
 * source but main.c and cli.c; tests/test_e2e.sh runs the daemon against
 * a fake SMGR over a fake transport (tests/fake_ipc.c).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/chef_log.h"
#include "common/chef_paths.h"
#include "common/linesrv.h"
#include "sensord.h"

static volatile sig_atomic_t g_stop;

/* Self-pipe: the signal handler writes it so a TERM ends poll() at once,
 * and so does SIGCHLD (the bias save's child). */
static int g_sigpipe[2] = { -1, -1 };

static void on_child(int sig)
{
	int saved = errno;

	(void)sig;
	if (g_sigpipe[1] >= 0 && write(g_sigpipe[1], "", 1) < 0) {
		/* full pipe: a wakeup is already pending */
	}
	errno = saved;
}

static void on_signal(int sig)
{
	int saved = errno;

	(void)sig;
	g_stop = 1;
	if (g_sigpipe[1] >= 0 && write(g_sigpipe[1], "", 1) < 0) {
		/* full pipe: a wakeup is already pending */
	}
	errno = saved;
}

void sn_usage(void)
{
	fprintf(stderr,
		"usage: sensord [-r REG] [-m MAP] [-k CMD] [-S SOCK] [-T dsps|cntvct|rx] [-b HZ]\n"
		"               [-s CH=ID:DT]... [-c CH=full|factory|raw]... [-i BASE] [-M MOUNT] [-D DEG]\n"
		"               [-Q] [-V] [-P] [-t] [-R] [-F] [-v]\n"
		"       sensord [-S SOCK] [-n COUNT] list | status | get SENSOR [HZ] | watch SENSOR [HZ]\n"
		"                                    | declination [DEG|off]\n");
}

static int parse_override(struct server *s, const char *arg)
{
	char name[32];
	unsigned id, dt;
	int c;

	if (sscanf(arg, "%31[^=]=%u:%u", name, &id, &dt) != 3 || id > 255 || dt > 255)
		return -1;
	c = sn_channel_from_name(name);
	if (c < 0 || c >= CH_SMGR_COUNT)
		return -1;
	s->ch[c].smgr_id = (uint8_t)id;
	s->ch[c].dt = (uint8_t)dt;
	if (c == CH_MAGN) {
		s->ch[CH_MAGF].smgr_id = (uint8_t)id;
		s->ch[CH_MAGF].dt = (uint8_t)dt;
	}
	return 0;
}

static int parse_cal(struct server *s, const char *arg)
{
	char name[32], cal[16];
	int c, k;

	if (sscanf(arg, "%31[^=]=%15s", name, cal) != 2)
		return -1;
	c = sn_channel_from_name(name);
	if (c < 0 || c >= CH_SMGR_COUNT)
		return -1;
	for (k = 0; k <= SNS_SMGR_CAL_RAW; k++)
		if (strcmp(cal, sn_cal_names[k]) == 0 || (cal[0] == '0' + k && !cal[1])) {
			s->ch[c].cal = (uint8_t)k;
			return 0;
		}
	return -1;
}

static int parse_u32(const char *arg, uint32_t *out)
{
	char *end;
	unsigned long long v;

	errno = 0;
	v = strtoull(arg, &end, 0);
	if (errno || end == arg || *end || v > UINT32_MAX)
		return -1;
	*out = (uint32_t)v;
	return 0;
}

int main(int argc, char **argv)
{
	static struct server s;
	const char *sock_path = CHEF_SENSORD_SOCK, *reg_path = CHEF_SENSORS_REG;
	const char *map_path = SENSORD_MAP_PATH, *ts = "dsps", *mag_cmd = "chef-state";
	long count = 0;
	int opt, i, line = 0, rc;
	size_t got = 0;

	sn_server_init(&s);
	while ((opt = getopt(argc, argv, "r:m:k:S:T:b:s:c:i:M:D:QVn:PtRFv")) != -1) {
		switch (opt) {
		case 'r': reg_path = optarg; break;
		case 'm': map_path = optarg; break;
		case 'k': mag_cmd = optarg; break;
		case 'S': sock_path = optarg; break;
		case 'T': ts = optarg; break;
		case 'b': s.report_cap = (unsigned)atoi(optarg); break;
		case 's':
			if (parse_override(&s, optarg)) {
				sn_usage();
				return 64;
			}
			break;
		case 'c':
			if (parse_cal(&s, optarg)) {
				sn_usage();
				return 64;
			}
			break;
		case 'i': {
			uint32_t v;

			if (parse_u32(optarg, &v) || v > 255 - CH_SMGR_COUNT) {
				sn_usage();
				return 64;
			}
			s.report_base = v;
			break;
		}
		case 'M': {
			int m = compass_mount_parse(optarg);

			if (m < 0) {
				sn_usage();
				return 64;
			}
			s.heading.mount = (enum compass_mount)m;
			s.heading.c.mount = (enum compass_mount)m;
			break;
		}
		case 'D': {
			char *end;
			double d = strtod(optarg, &end);

			if (*end || end == optarg || !isfinite(d) || d < -180 || d > 180) {
				sn_usage();
				return 64;
			}
			s.heading.have_decl = true;
			s.heading.decl = d;
			break;
		}
		case 'Q': s.qmag.opt = true; break;
		case 'V': s.rotvec.opt = true; break;
		case 'n': count = atol(optarg); break;
		case 'P': s.periodic = true; break;
		case 't': s.serve_time2 = true; break;
		case 'R': s.serve_reg2 = false; break;
		case 'F': s.force = true; break;
		case 'v': sn_verbose = true; break;
		default: sn_usage(); return 64;
		}
	}
	if (optind < argc)
		return sn_cli_main(sock_path, count, argc - optind, argv + optind);
	chef_log_init("sensord", true);

	/* One daemon per socket: a second one would steal the socket and
	 * publish REG2 twice. A socket nobody answers on is stale and is
	 * replaced by linesrv_listen(). */
	{
		int fd = linesrv_connect(sock_path);

		if (fd >= 0) {
			close(fd);
			chef_log("another sensord is serving %s; refusing to start", sock_path);
			return 1;
		}
	}

	if (pipe2(g_sigpipe, O_CLOEXEC | O_NONBLOCK) < 0) {
		chef_log("pipe: %s", strerror(errno));
		return 1;
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);	/* a closed telnet session: exit cleanly too */
	signal(SIGPIPE, SIG_IGN);
	{
		struct sigaction sa = { .sa_handler = on_child, .sa_flags = SA_RESTART | SA_NOCLDSTOP };

		sigemptyset(&sa.sa_mask);
		sigaction(SIGCHLD, &sa, NULL);
	}

	if (strcmp(ts, "dsps") == 0) {
		s.ts_mode = TS_DSPS;
		s.dsps_fd = open(SENSORD_DSPS_DEV, O_RDONLY | O_CLOEXEC);
		if (s.dsps_fd < 0) {
			chef_log("%s: %s; timestamps fall back to receive time (-T rx)",
				 SENSORD_DSPS_DEV, strerror(errno));
			s.ts_mode = TS_RX;
		}
	} else if (strcmp(ts, "cntvct") == 0) {
#if defined(__aarch64__)
		s.ts_mode = TS_CNTVCT;
#else
		chef_log("-T cntvct needs aarch64; using rx");
		s.ts_mode = TS_RX;
#endif
	} else if (strcmp(ts, "rx") == 0) {
		s.ts_mode = TS_RX;
	} else {
		sn_usage();
		return 64;
	}

	if (s.serve_reg2) {
		rc = sns_reg_load_map(&s.reg, map_path, &line);
		if (rc) {
			chef_log("registry map %s: %s%s; exiting", map_path, strerror(-rc),
				 rc == -EINVAL ? " (bad line)" : "");
			return 1;
		}
		rc = sns_reg_load_data(&s.reg, reg_path, 0, &got);
		if (rc) {
			chef_log("registry %s: %s (%zu bytes, map wants %zu); exiting", reg_path,
				 strerror(-rc), got, s.reg.size);
			return 1;
		}
		chef_log("registry %s loaded: %zu bytes, %zu groups, %zu items (RAM copy; DSP writes are written back to this file, never to persist)",
			 reg_path, s.reg.size, s.reg.ngroups, s.reg.nitems);
		s.reg_path = reg_path;
		if (mag_cmd[0]) {
			static char dir[512];
			char *slash;

			snprintf(dir, sizeof(dir), "%s", reg_path);
			slash = strrchr(dir, '/');
			if (slash)
				*(slash == dir ? slash + 1 : slash) = '\0';
			else
				snprintf(dir, sizeof(dir), ".");
			s.mag_cmd = mag_cmd;
			s.mag_env_run = dir;
			s.mag_env_map = map_path;
		}
		sn_mag_cal_check(&s, "registry loaded");
		if (sn_publish(&s, &s.reg_sock, SNS_REG2_SVC, SNS_REG2_VERS, SNS_REG2_INST))
			return 1;
	} else {
		chef_log("not serving REG2 (-R)");
		sn_mag_cal_check(&s, "no registry (-R)");
	}
	if (s.serve_time2 &&
	    sn_publish(&s, &s.time_sock, SNS_TIME2_SVC, SNS_TIME2_VERS, SNS_TIME2_APPS_INST))
		return 1;

	rc = sn_server_start(&s);
	if (!rc)
		rc = linesrv_listen(s.srv, sock_path);
	if (rc) {
		chef_log("listen %s: %s; exiting", sock_path, strerror(-rc));
		return 1;
	}
	chef_log("ready: %s, %s API, timestamps %s%s%s%s%s; heading mount %s%s", sock_path,
		 s.periodic ? "periodic REPORT" : "BUFFERING",
		 s.ts_mode == TS_DSPS ? "dsps" : s.ts_mode == TS_CNTVCT ? "cntvct" : "rx",
		 s.report_cap ? ", report rate capped" : "", s.force ? ", -F" : "",
		 s.qmag.opt ? ", QMAG_CAL client (-Q)" : "",
		 s.rotvec.opt ? ", rotation vector client (-V)" : "",
		 compass_mount_name(s.heading.mount), s.heading.have_decl ? ", declination set (-D)" : "");
	if (s.report_base)
		chef_log("SMGR report IDs %u..%u (-i)", s.report_base + 1, s.report_base + CH_SMGR_COUNT);
	for (i = 0; i < CH_SMGR_COUNT; i++)
		if (s.ch[i].cal != SNS_SMGR_CAL_FULL && !s.ch[i].internal)
			chef_log("%s: calibration %s (-c)%s", s.ch[i].name, sn_cal_name(s.ch[i].cal),
				 s.periodic ? " has no effect with -P: REPORT has no calibration field" : "");

	while (!g_stop) {
		struct pollfd pfd[6 + 1 + SENSORD_MAX_CLIENTS];	/* ours, then linesrv's */
		struct sam *sams[2] = { &s.qmag, &s.rotvec };
		int n = 0, ireg = -1, itime = -1, ismgr = -1, isam[2] = { -1, -1 }, isrv, nsrv;
		int k;

		pfd[n++] = (struct pollfd){ .fd = g_sigpipe[0], .events = POLLIN };
		if (s.reg_sock >= 0) {
			ireg = n;
			pfd[n++] = (struct pollfd){ .fd = s.reg_sock, .events = POLLIN };
		}
		if (s.time_sock >= 0) {
			itime = n;
			pfd[n++] = (struct pollfd){ .fd = s.time_sock, .events = POLLIN };
		}
		if (s.smgr_sock >= 0 && s.st != SMGR_DOWN) {
			ismgr = n;
			pfd[n++] = (struct pollfd){ .fd = s.smgr_sock, .events = POLLIN };
		}
		for (k = 0; k < 2; k++)
			if (sams[k]->sock >= 0) {
				isam[k] = n;
				pfd[n++] = (struct pollfd){ .fd = sams[k]->sock, .events = POLLIN };
			}
		isrv = n;
		nsrv = linesrv_pollfds(s.srv, pfd + n, (int)(sizeof(pfd) / sizeof(pfd[0])) - n);
		n += nsrv;

		rc = poll(pfd, (nfds_t)n, sn_server_deadline_ms(&s));
		if (rc < 0 && errno != EINTR)
			break;
		if (g_stop)
			break;
		if (pfd[0].revents & POLLIN) {
			char drain[64];

			while (read(g_sigpipe[0], drain, sizeof(drain)) > 0)
				;
		}
		sn_mag_save_reap(&s);

		if (ireg >= 0 && (pfd[ireg].revents & (POLLIN | POLLERR))) {
			static char buf[SENSORD_QMI_BUF];
			struct qrtr_packet pkt;
			uint32_t node = 0, port = 0;

			rc = sn_ipc_recv(s.reg_sock, buf, sizeof(buf), &pkt, &node, &port);
			if (rc == -ENETRESET) {
				chef_log("reg2 socket reset by the IPC router; republishing");
				sn_publish(&s, &s.reg_sock, SNS_REG2_SVC, SNS_REG2_VERS, SNS_REG2_INST);
			} else if (rc > 0) {
				sn_reg2_rx(&s, &pkt, node, port);
			}
		}
		if (itime >= 0 && (pfd[itime].revents & (POLLIN | POLLERR))) {
			static char buf[SENSORD_QMI_BUF];
			struct qrtr_packet pkt;
			uint32_t node = 0, port = 0;

			rc = sn_ipc_recv(s.time_sock, buf, sizeof(buf), &pkt, &node, &port);
			if (rc == -ENETRESET)
				sn_publish(&s, &s.time_sock, SNS_TIME2_SVC, SNS_TIME2_VERS, SNS_TIME2_APPS_INST);
			else if (rc > 0)
				sn_time2_rx(&s, &pkt, node, port);
		}
		if (ismgr >= 0 && (pfd[ismgr].revents & (POLLIN | POLLERR))) {
			static char buf[SENSORD_QMI_BUF];
			struct qrtr_packet pkt;
			uint32_t node = 0, port = 0;

			rc = sn_ipc_recv(s.smgr_sock, buf, sizeof(buf), &pkt, &node, &port);
			if (rc == -ENETRESET)
				sn_smgr_reset(&s, "IPC router reset");
			else if (rc > 0)
				sn_smgr_rx(&s, &pkt, node, port);
		}
		for (k = 0; k < 2; k++) {
			struct sam *q = sams[k];
			static char buf[SENSORD_QMI_BUF];
			struct qrtr_packet pkt;
			uint32_t node = 0, port = 0;

			/* the socket may have been closed and reopened meanwhile */
			if (isam[k] < 0 || q->sock != pfd[isam[k]].fd ||
			    !(pfd[isam[k]].revents & (POLLIN | POLLERR)))
				continue;
			rc = sn_ipc_recv(q->sock, buf, sizeof(buf), &pkt, &node, &port);
			if (rc == -ENETRESET)
				sn_sam_forget(&s, q, "IPC router reset");
			else if (rc > 0)
				sn_sam_rx(&s, q, &pkt, node, port);
		}
		linesrv_handle(s.srv, pfd + isrv, nsrv);
		sn_smgr_tick(&s);
		sn_sam_tick(&s, &s.qmag);
		sn_sam_tick(&s, &s.rotvec);
		sn_reg_tick(&s);
		sn_mag_save_tick(&s);
		sn_server_tick_clients(&s);
	}

	chef_log("exiting");
	sn_server_drop_all(&s);
	/* Best effort: tell the DSP to stop the reports we own (the IPC
	 * router also tells it when our port closes). Not waited for. */
	if (s.st == SMGR_READY)
		for (i = 0; i < CH_SMGR_COUNT; i++)
			if (s.ch[i].rate && !s.ch[i].busy)
				sn_smgr_report(&s, i, 0);
	/* SAM services (QMAG_CAL, rotation vector): the clients are gone, so
	 * nothing wants them. Disable an enabled instance, and give a request
	 * in flight (an ENABLE above all, whose instance would otherwise be
	 * left behind) a short while to be answered, disabling what it
	 * created. Both services share the wait. */
	{
		struct sam *sams[2] = { &s.qmag, &s.rotvec };
		bool sent[2] = { false, false };
		int64_t end = sn_real_now_ms() + SENSORD_QMAG_EXIT_WAIT_MS;
		int k;

		for (;;) {
			struct pollfd pfd[2];
			int np = 0, idx[2] = { -1, -1 };
			int64_t left = end - sn_real_now_ms();

			for (k = 0; k < 2; k++) {
				struct sam *q = sams[k];

				if (q->sock >= 0 && q->on && !q->busy && !sent[k]) {
					struct sns_sam_disable_req r = { .instance = q->instance };

					sent[k] = true;
					if (sn_sam_send(&s, q, SNS_SAM_DISABLE, sns_sam_disable_req_ei, &r) == 0)
						chef_log("%s disable instance %u (exiting)", q->name, r.instance);
				}
				if (q->sock >= 0 && q->busy) {
					idx[k] = np;
					pfd[np++] = (struct pollfd){ .fd = q->sock, .events = POLLIN };
				}
			}
			if (!np)
				break;
			if (left <= 0) {
				for (k = 0; k < 2; k++)
					if (idx[k] >= 0)
						chef_log("%s msg 0x%02x unanswered at exit%s", sams[k]->name,
							 sams[k]->msg, sams[k]->msg == SNS_SAM_ENABLE ?
							 "; its instance, if any, lasts until the DSP drops our port" : "");
				break;
			}
			if (poll(pfd, (nfds_t)np, left > 100 ? 100 : (int)left) <= 0)
				continue;
			for (k = 0; k < 2; k++) {
				static char buf[SENSORD_QMI_BUF];
				struct qrtr_packet pkt;
				uint32_t node = 0, port = 0;

				if (idx[k] < 0 || !(pfd[idx[k]].revents & (POLLIN | POLLERR)))
					continue;
				rc = sn_ipc_recv(sams[k]->sock, buf, sizeof(buf), &pkt, &node, &port);
				if (rc == -ENETRESET)
					sn_sam_forget(&s, sams[k], "IPC router reset");
				else if (rc > 0)
					sn_sam_rx(&s, sams[k], &pkt, node, port);
			}
		}
		for (k = 0; k < 2; k++)
			if (sams[k]->sock >= 0)
				qrtr_close(sams[k]->sock);
	}
	sn_server_fini(&s);	/* the listener: closed, its path unlinked */
	if (s.smgr_sock >= 0)
		qrtr_close(s.smgr_sock);
	if (s.reg_sock >= 0)
		qrtr_close(s.reg_sock);
	if (s.time_sock >= 0)
		qrtr_close(s.time_sock);
	sn_mag_save_stop(&s);
	sn_reg_writeback(&s, "on exit");
	sns_reg_free(&s.reg);
	return 0;
}
