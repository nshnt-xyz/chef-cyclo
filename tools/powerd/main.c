/*
 * powerd - battery policy, logging and charge throttle for chef-cyclo.
 *
 * Charging itself is entirely in the kernel we share with stock Android
 * (qpnp-smb2 + the Motorola mmi heartbeat temperature zones + qpnp-fg-gen3,
 * see docs/features/battery-and-charging.md). Stock userspace adds
 * three small policy layers, and this daemon is the combined clone of them:
 *
 *   BatteryService (framework)  low-battery warning at 15 %, critical at
 *                               5 %, clean shutdown when the battery is
 *                               empty and nothing powers the phone, and
 *                               shutdown at 68.0 C battery temperature.
 *   batt_health (vendor_pwric)  a CSV log of power_supply uevents. Stock
 *                               writes it to /data; ours goes to
 *                               /run/chef/power/log.csv, or with -l to a log
 *                               directory on /data (its OpenRC service
 *                               passes /data/v1/power when /data is ours),
 *                               bounded with one rotation. The persist part (age/cycles in
 *                               /mnt/vendor/persist) is not cloned: persist
 *                               is never written from this project.
 *   thermal-engine SS-BATT-BATT set point 44 C / clear 42 C on the battery
 *                               temperature, acting on the `battery` psy's
 *                               system_temp_level (an index into the DT's
 *                               qcom,thermal-mitigation FCC table, 8 levels
 *                               3000..300 mA on chef), sampled every 5 s.
 *
 * Inputs: a NETLINK_KOBJECT_UEVENT socket (kernel-sent messages with
 * SUBSYSTEM=power_supply only; a burst is coalesced into one sample) and a
 * periodic poll: 60 s normally, 10 s once capacity is at or below the warn
 * level or the battery is at the throttle set point, 5 s while a supply is
 * online (the throttle cadence) or while a shutdown condition is being
 * confirmed. SIGUSR1 forces a sample.
 *
 * Outputs, under the run directory (default /run/chef/power, tmpfs):
 *   state              key=value lines, rewritten atomically (tmp + rename)
 *                      on every sample; `powerd status` prints it
 *   log.csv(.1)        (in the -l log directory if given) one row per
 *                      uevent/poll sample (5 s throttle ticks only when
 *                      something changed or 30 s passed), with a header;
 *                      rotated to log.csv.1 at the size limit, or when the
 *                      existing file starts with another header. The last
 *                      column, boot_id, is filled on the start row only.
 *                      The file is opened, appended and closed (never held
 *                      open). Without -l (tmpfs) each row is appended as it
 *                      is made; a failed append is logged once and retried
 *                      on the next row.
 *   log.pending        with -l only: rows wait here, on tmpfs, and reach
 *                      LOGDIR/log.csv in one batch every -F seconds
 *                      (default 600), so the eMMC is not woken for every
 *                      row (each dirty append cost a journal commit,
 *                      about 12 to 15 KiB of writes per 175-byte row). Rows that
 *                      matter after a crash flush at once: the start row
 *                      (with any rows a previous powerd of this boot left
 *                      here), plug/unplug, a status change, an alert or
 *                      throttle level change, and the shutdown row, which
 *                      is fsynced before the shutdown command runs. SIGTERM
 *                      (chef-state shutdown, on every orderly shutdown
 *                      path) flushes once before exit, fsynced. A failed flush keeps the
 *                      rows (logged once); the file is bounded (256 KiB) by
 *                      dropping its oldest rows. On abrupt power loss up to
 *                      one interval of ordinary rows is lost.
 *   shutdown-pending   written with the reason just before a shutdown
 * and "powerd: ..." log lines (chef_log: syslog, kmsg when syslogd cannot
 * take them) for transitions only (plug/unplug, status, warn/critical,
 * throttle level, shutdown).
 *
 * Policy (stock-derived defaults; every threshold overridable, overrides
 * are logged loudly as TEST OVERRIDE at start and listed in the state file):
 *   - "Discharging" means battery/status reads exactly Discharging AND
 *     usb/online, pc_port/online (where an SDP port shows up; usb/online
 *     is 0 for it) and dc/online are each 0 or their psy is absent. An
 *     unreadable status or online counts as powered. Never the sign of
 *     bms/current_now (positive = discharging,
 *     negative = charging on this FG, and a weak port can AICL-collapse
 *     to a net discharge while status still says Charging).
 *   - Unpowered: warn once at capacity <= 15 %, critical once at <= 5 %
 *     (plus a vibrator pulse); both re-arm when power is attached or the
 *     capacity is back at >= 20 %.
 *   - Shutdown when unpowered, battery/present == 1, bms/soc_reporting_ready
 *     == 1 and (capacity <= 0 %, or voltage_now < 3300 mV with capacity at
 *     or below the critical level: bms/resistance ~168 mOhm lets GPS/panel
 *     load sag a healthy battery). An unreadable/-22 capacity or voltage is
 *     unknown, never 0, and resets the confirmation. Also at battery
 *     temperature > 68.0 C (strict, as AOSP) whatever the supply state: the
 *     only shutdown allowed while powered, outside --shutdown-when-online.
 *     Each condition must hold on 3 consecutive samples at least 5 s apart. The shutdown is
 *     `sync` then busybox `poweroff` (init's orderly ::shutdown path, the
 *     same as buttond), falling back to SIGUSR2 to init and finally
 *     reboot(RB_POWER_OFF). -x CMD replaces all of that with `sh -c CMD`
 *     and no fallback, which is what the tests use.
 *   - Charge throttle only while powered: every 5 s, battery temp >= 44 C
 *     raises system_temp_level by one (to num_system_temp_levels - 1),
 *     <= 42 C lowers it by one. 0 is written at start, on unplug and on a
 *     clean exit, so no stale level survives.
 *   - Nothing else is written: no ICL/FCC/input_suspend/ship-mode/demo-mode
 *     knobs, no persist or EFS. The kernel's own charging stays in charge.
 *
 * Usage: powerd [-r SYSFS] [-d RUNDIR] [-l LOGDIR] [-F SECS] [-x CMD] [-V VIB] [-K KMSG] [-v]
 *               [test overrides, see usage()]
 *        powerd [-d RUNDIR] status
 *        powerd [-d RUNDIR] -l LOGDIR [-L LOG_MAX] flush
 *                      append the log.pending rows a powerd that is no
 *                      longer running left behind (chef-state shutdown's
 *                      fallback), fsynced, with the same rotation rules.
 *                      Only while no powerd daemon runs on RUNDIR: next to
 *                      one it could duplicate or reorder rows.
 *
 * Files: sample.c the sysfs sample reader (against any root), powered()
 * and the uevent filter, policy.c the policy step with injected hooks, the
 * poll interval and one whole sample, log.c the state file, the CSV
 * formatting and rotation and the batched -l log, this file the actions
 * (level write, buzz, shutdown), the loop, the options and the `status`
 * and `flush` subcommands; powerd.h is their internal header.
 * tests/test_powerd.c links sample.c, policy.c and log.c and never powers
 * anything off; tests/test_powerd.sh runs the host binary end to end.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/netlink.h>

#include "common/chef_log.h"
#include "common/chef_paths.h"
#include "powerd.h"

static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_kick;

/* ------------------------------------------------------------ actions */

static int sysfs_write_level(struct powerd *p, int level)
{
	char path[512], buf[16];
	int fd, n;

	snprintf(path, sizeof(path), "%s/battery/system_temp_level", p->sysfs);
	n = snprintf(buf, sizeof(buf), "%d\n", level);
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (write(fd, buf, (size_t)n) != n) {
		int err = errno ? errno : EIO;

		close(fd);
		return -err;
	}
	close(fd);
	return 0;
}

static void vib_buzz(struct powerd *p, int ms)
{
	int fd = open(p->vib, O_WRONLY | O_CLOEXEC);

	if (fd < 0)
		return;
	dprintf(fd, "%d\n", ms);
	close(fd);
}

static const char *g_shutdown_cmd;	/* -x: replaces the whole chain */

/*
 * The orderly way down, as buttond does it: sync, then `poweroff` (busybox:
 * SIGUSR2 to init -> ::shutdown entries, SIGTERM/SIGKILL to everything,
 * reboot(RB_POWER_OFF)); if the applet fails, the same signal directly; if
 * init will not take it either, the syscall. That syscall needs
 * CAP_SYS_BOOT, which powerd does not have (only CAP_KILL, for the signal),
 * so it fails with EPERM; the line logged just before it says why init was
 * not reached. With -x CMD only `sh -c CMD` runs, with no fallback.
 */
static void system_shutdown(struct powerd *p)
{
	pid_t pid;
	int st = -1;

	sync();
	pid = fork();
	if (pid == 0) {
		if (g_shutdown_cmd)
			execl("/bin/sh", "sh", "-c", g_shutdown_cmd, (char *)NULL);
		else
			execlp("poweroff", "poweroff", (char *)NULL);
		_exit(127);
	}
	if (pid > 0)
		while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
			;
	if (g_shutdown_cmd) {
		p->log("shutdown command exited (status %d)", st);
		return;
	}
	if (pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0)
		return;
	p->log("poweroff command failed (status %d); signalling init directly", st);
	if (kill(1, SIGUSR2) == 0)
		return;
	p->log("init would not take SIGUSR2 (%s); reboot(RB_POWER_OFF)", strerror(errno));
	/* no ::shutdown lines (chef-state's fallback) on this path: if the
	 * durable flush with the shutdown row failed, try once more */
	if (p->logdir && p->log_failed && log_flush(p, true) < 0)
		p->log("pending rows in %s/log.pending are lost", p->dir);
	sync();
	reboot(RB_POWER_OFF);
}

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static double uptime_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_BOOTTIME, &ts);
	return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static int uevent_open(void)
{
	struct sockaddr_nl sa;
	int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);

	if (fd < 0)
		return -errno;
	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	sa.nl_groups = 1;	/* the kernel's uevent multicast group */
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		int err = errno;

		close(fd);
		return -err;
	}
	return fd;
}

/* Drain the socket; true if any kernel-sent power_supply uevent arrived. */
static bool uevent_drain(int fd)
{
	char buf[4096];
	bool hit = false;

	for (;;) {
		struct sockaddr_nl from;
		socklen_t fl = sizeof(from);
		ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);

		if (n < 0) {
			/* overrun in an event storm: events were lost, so sample */
			if (errno == ENOBUFS)
				hit = true;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			if (errno == EINTR || errno == ENOBUFS)
				continue;
			break;
		}
		buf[n] = '\0';
		if (fl == sizeof(from) && from.nl_pid == 0 && uevent_is_power_supply(buf, (size_t)n))
			hit = true;
	}
	return hit;
}

static void on_signal(int sig)
{
	if (sig == SIGUSR1)
		g_kick = 1;
	else
		g_stop = 1;
}

static int mkdir_p(const char *dir)
{
	char buf[512];
	char *q;

	snprintf(buf, sizeof(buf), "%s", dir);
	for (q = buf + 1; *q; q++) {
		if (*q != '/')
			continue;
		*q = '\0';
		if (mkdir(buf, 0755) < 0 && errno != EEXIST)
			return -errno;
		*q = '/';
	}
	if (mkdir(buf, 0755) < 0 && errno != EEXIST)
		return -errno;
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: powerd [-r SYSFS] [-d RUNDIR] [-l LOGDIR] [-F SECS] [-x CMD] [-V VIB] [-K KMSG] [-L LOG_MAX] [-v] [TEST OPTIONS]\n"
		"       powerd [-d RUNDIR] status\n"
		"       powerd [-d RUNDIR] -l LOGDIR [-L LOG_MAX] flush\n"
		"           append the pending rows a stopped powerd left; only while no powerd\n"
		"           daemon runs on RUNDIR (it could duplicate or reorder rows)\n"
		"  -r  power_supply sysfs root (default " POWERD_SYSFS ", or $POWERD_SYSFS)\n"
		"  -d  run directory for state, shutdown-pending and log.csv (default " CHEF_POWER_DIR ")\n"
		"  -l  log directory for log.csv(.1) instead of the run directory; rows wait in\n"
		"      RUNDIR/log.pending and are appended in batches (see -F)\n"
		"  -F  with -l: flush pending rows at least every SECS seconds (default 600;\n"
		"      0 = every row); important rows always flush at once\n"
		"  -x  shutdown command (sh -c CMD) instead of sync + poweroff; no fallback\n"
		"  -V  vibrator for the critical buzz ('' = none; default " POWERD_VIB_PATH ")\n"
		"  -K  kmsg path for log lines syslogd cannot take (default /dev/kmsg)\n"
		"  -L  log.csv size limit in bytes before rotation (default 1048576)\n"
		"test options (each logged as TEST OVERRIDE):\n"
		"  --warn PCT --critical PCT --rearm PCT --empty-pct PCT --empty-mv MV\n"
		"  --overtemp C --throttle-set C --throttle-clear C --no-throttle\n"
		"  --confirm N --confirm-gap-ms MS --step-ms MS --poll-s S\n"
		"  --shutdown-when-online   run warn/critical/empty as if unpowered\n");
}

static int status_main(const char *dir)
{
	char path[512], buf[4096];
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path), "%s/state", dir);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "powerd: %s: %s (is powerd running?)\n", path, strerror(errno));
		return 1;
	}
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		if (fwrite(buf, 1, (size_t)n, stdout) != (size_t)n)
			break;
	close(fd);
	return 0;
}

/* Parse "44" / "44.5" / "-3.5" degrees C into deci-degrees. */
static bool parse_deci(const char *s, int *out)
{
	char *end;
	double v = strtod(s, &end);

	if (end == s || *end || !isfinite(v) || v < -100 || v > 200)
		return false;
	*out = (int)(v * 10 + (v < 0 ? -0.5 : 0.5));
	return true;
}

static bool parse_int(const char *s, int lo, int hi, int *out)
{
	char *end;
	long v = strtol(s, &end, 10);

	if (end == s || *end || v < lo || v > hi)
		return false;
	*out = (int)v;
	return true;
}

enum {
	OPT_WARN = 256, OPT_CRIT, OPT_REARM, OPT_EMPTY_PCT, OPT_EMPTY_MV, OPT_OVERTEMP,
	OPT_THR_SET, OPT_THR_CLR, OPT_NO_THROTTLE, OPT_CONFIRM, OPT_CONFIRM_GAP,
	OPT_STEP, OPT_POLL, OPT_WHEN_ONLINE,
};

static void add_override(struct powerd *p, const char *name, const char *val)
{
	size_t off = strlen(p->overrides);

	snprintf(p->overrides + off, sizeof(p->overrides) - off, "%s%s%s%s",
		 off ? " " : "", name, val ? "=" : "", val ? val : "");
}

int main(int argc, char **argv)
{
	static const struct option longopts[] = {
		{ "warn", required_argument, NULL, OPT_WARN },
		{ "critical", required_argument, NULL, OPT_CRIT },
		{ "rearm", required_argument, NULL, OPT_REARM },
		{ "empty-pct", required_argument, NULL, OPT_EMPTY_PCT },
		{ "empty-mv", required_argument, NULL, OPT_EMPTY_MV },
		{ "overtemp", required_argument, NULL, OPT_OVERTEMP },
		{ "throttle-set", required_argument, NULL, OPT_THR_SET },
		{ "throttle-clear", required_argument, NULL, OPT_THR_CLR },
		{ "no-throttle", no_argument, NULL, OPT_NO_THROTTLE },
		{ "confirm", required_argument, NULL, OPT_CONFIRM },
		{ "confirm-gap-ms", required_argument, NULL, OPT_CONFIRM_GAP },
		{ "step-ms", required_argument, NULL, OPT_STEP },
		{ "poll-s", required_argument, NULL, OPT_POLL },
		{ "shutdown-when-online", no_argument, NULL, OPT_WHEN_ONLINE },
		{ NULL, 0, NULL, 0 },
	};
	struct powerd p;
	struct config *c = &p.cfg;
	struct sigaction sa;
	const char *env;
	int64_t next_at, uevent_at = -1, last_uevent_sample = INT64_MIN / 2;
	int opt, ufd, ms, rc;
	char logdesc[600];
	bool ok = true, verbose = false;

	powerd_init(&p);
	p.write_level = sysfs_write_level;
	p.shutdown = system_shutdown;
	p.buzz = vib_buzz;
	env = getenv("POWERD_SYSFS");
	if (env && *env)
		p.sysfs = env;
	while ((opt = getopt_long(argc, argv, "r:d:l:F:x:V:K:L:v", longopts, NULL)) != -1) {
		switch (opt) {
		case 'r': p.sysfs = optarg; break;
		case 'd': p.dir = optarg; break;
		case 'l': p.logdir = optarg; break;
		case 'F':
			ok = parse_int(optarg, 0, 86400, &p.flush_ms);
			p.flush_ms *= 1000;
			break;
		case 'x': g_shutdown_cmd = optarg; add_override(&p, "shutdown-cmd", NULL); break;
		case 'V': p.vib = optarg; break;
		case 'K': chef_log_set_kmsg(optarg); break;
		case 'L': p.log_max = atol(optarg); ok = p.log_max >= 1024; break;
		case 'v': verbose = true; break;
		case OPT_WARN: ok = parse_int(optarg, 0, 100, &c->warn_pct); break;
		case OPT_CRIT: ok = parse_int(optarg, 0, 100, &c->crit_pct); break;
		case OPT_REARM: ok = parse_int(optarg, 0, 101, &c->rearm_pct); break;
		case OPT_EMPTY_PCT: ok = parse_int(optarg, -1, 100, &c->empty_pct); break;
		case OPT_EMPTY_MV: ok = parse_int(optarg, 0, 5000, &c->empty_mv); break;
		case OPT_OVERTEMP: ok = parse_deci(optarg, &c->overtemp_dc); break;
		case OPT_THR_SET: ok = parse_deci(optarg, &c->thr_set_dc); break;
		case OPT_THR_CLR: ok = parse_deci(optarg, &c->thr_clr_dc); break;
		case OPT_NO_THROTTLE: c->throttle = false; break;
		case OPT_CONFIRM: ok = parse_int(optarg, 1, 100, &c->confirm_n); break;
		case OPT_CONFIRM_GAP: ok = parse_int(optarg, 0, 600000, &c->confirm_gap_ms); break;
		case OPT_STEP: ok = parse_int(optarg, 100, 600000, &c->step_ms); break;
		case OPT_POLL:
			ok = parse_int(optarg, 1, 3600, &c->poll_ms);
			c->poll_ms *= 1000;
			if (c->poll_low_ms > c->poll_ms)
				c->poll_low_ms = c->poll_ms;
			break;
		case OPT_WHEN_ONLINE: c->act_when_online = true; break;
		default: usage(); return 64;
		}
		if (!ok) {
			fprintf(stderr, "powerd: bad value for option: %s\n", optarg ? optarg : "");
			usage();
			return 64;
		}
		/* longopts[] is in OPT_* order */
		if (opt >= OPT_WARN)
			add_override(&p, longopts[opt - OPT_WARN].name,
				     longopts[opt - OPT_WARN].has_arg ? optarg : NULL);
	}
	chef_log_init("powerd", verbose);
	if (optind < argc) {
		if (strcmp(argv[optind], "status") == 0 && optind + 1 == argc)
			return status_main(p.dir);
		if (strcmp(argv[optind], "flush") == 0 && optind + 1 == argc && p.logdir)
			return flush_main(&p);
		usage();
		return 64;
	}
	if (c->thr_clr_dc >= c->thr_set_dc) {
		fprintf(stderr, "powerd: throttle clear point must be below the set point\n");
		return 64;
	}
	if (c->crit_pct > c->warn_pct || c->rearm_pct <= c->warn_pct) {
		fprintf(stderr, "powerd: need critical <= warn < rearm (%d, %d, %d)\n",
			c->crit_pct, c->warn_pct, c->rearm_pct);
		return 64;
	}

	rc = mkdir_p(p.dir);
	if (rc < 0) {
		chef_log("mkdir %s: %s; exiting in 5 s", p.dir, strerror(-rc));
		sleep(5);
		return 1;
	}
	if (p.logdir) {
		rc = mkdir_p(p.logdir);
		if (rc < 0) {
			chef_log("mkdir %s: %s; logging to %s", p.logdir, strerror(-rc), p.dir);
			p.logdir = NULL;
		}
	}
	{
		char path[512];
		ssize_t n;
		int fd;

		snprintf(path, sizeof(path), "%s/shutdown-pending", p.dir);
		unlink(path);
		fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
		n = fd < 0 ? -1 : read(fd, p.boot_id, sizeof(p.boot_id) - 1);
		if (fd >= 0)
			close(fd);
		p.boot_id[n > 0 ? n : 0] = '\0';
		p.boot_id[strcspn(p.boot_id, "\n,")] = '\0';
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	ufd = uevent_open();
	if (ufd < 0)
		chef_log("uevent socket: %s; polling only", strerror(-ufd));
	if (p.overrides[0])
		chef_log("TEST OVERRIDE: %s", p.overrides);
	if (c->act_when_online)
		chef_log("TEST OVERRIDE: --shutdown-when-online: low-battery policy acts with a supply attached");
	if (p.logdir)
		snprintf(logdesc, sizeof(logdesc), "%s (batched every %d s)", p.logdir, p.flush_ms / 1000);
	else
		snprintf(logdesc, sizeof(logdesc), "%s", p.dir);
	chef_log("ready: %s -> %s, log %s, warn %d %%, critical %d %%, off at %d %% / %d mV / %d.%d C, throttle %s",
		  p.sysfs, p.dir, logdesc, c->warn_pct, c->crit_pct, c->empty_pct, c->empty_mv,
		  c->overtemp_dc / 10, abs(c->overtemp_dc) % 10,
		  c->throttle ? "on" : "off");

	/* no stale level from an earlier run survives */
	if (c->throttle) {
		rc = p.write_level(&p, 0);
		if (rc < 0 && rc != -ENOENT)
			chef_log("system_temp_level reset: %s", strerror(-rc));
	}

	ms = powerd_sample(&p, "start", now_ms(), uptime_s());
	next_at = now_ms() + ms;
	while (!g_stop) {
		struct pollfd pfd = { .fd = ufd, .events = POLLIN };
		int64_t now = now_ms(), due = next_at;
		int timeout;

		if (uevent_at >= 0 && uevent_at < due)
			due = uevent_at;
		if (p.logdir && p.flush_due >= 0 && p.flush_due < due)
			due = p.flush_due;
		timeout = due > now ? (int)(due - now) : 0;
		if (g_kick)
			timeout = 0;
		rc = poll(&pfd, ufd >= 0 ? 1 : 0, timeout);
		if (rc < 0 && errno != EINTR) {
			chef_log("poll: %s; exiting for a respawn", strerror(errno));
			break;
		}
		now = now_ms();
		if (rc > 0 && (pfd.revents & POLLIN) && uevent_drain(ufd) && uevent_at < 0) {
			/* coalesce a burst: 200 ms, and at most one uevent sample a second */
			uevent_at = now + 200;
			if (uevent_at < last_uevent_sample + 1000)
				uevent_at = last_uevent_sample + 1000;
		}
		if (g_stop)
			break;
		if (g_kick) {
			g_kick = 0;
			ms = powerd_sample(&p, "signal", now, uptime_s());
			next_at = now + ms;
		} else if (uevent_at >= 0 && now >= uevent_at) {
			uevent_at = -1;
			last_uevent_sample = now;
			ms = powerd_sample(&p, "uevent", now, uptime_s());
			next_at = now + ms;
		} else if (now >= next_at) {
			bool fast = p.empty.count || p.hot.count ||
				    (p.have_prev && supply_online(&p.prev) && c->throttle);

			ms = powerd_sample(&p, fast ? "tick" : "poll", now, uptime_s());
			next_at = now + ms;
		} else {
			log_flush_if_due(&p, now);
		}
	}
	/* every orderly shutdown path (init's ::shutdown lines: chef-state
	 * shutdown TERMs powerd) gets the pending rows onto the disk here,
	 * before /data goes read-only: one append and its fsync, no waiting */
	if (p.logdir)
		log_flush_now(&p, true, now_ms());
	if (c->throttle && !p.shutdown_fired && p.level != 0)
		p.write_level(&p, 0);
	chef_log("exiting");
	return 0;
}
