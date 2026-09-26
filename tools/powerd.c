/*
 * powerd - battery policy, logging and charge throttle for chef-cyclo.
 *
 * Charging itself is entirely in the kernel we share with stock Android
 * (qpnp-smb2 + the Motorola mmi heartbeat temperature zones + qpnp-fg-gen3,
 * see docs/next-steps/battery-and-charging-plan.md). Stock userspace adds
 * three small policy layers, and this daemon is the combined clone of them:
 *
 *   BatteryService (framework)  low-battery warning at 15 %, critical at
 *                               5 %, clean shutdown when the battery is
 *                               empty and nothing powers the phone, and
 *                               shutdown at 68.0 C battery temperature.
 *   batt_health (vendor_pwric)  a CSV log of power_supply uevents. Stock
 *                               writes it to /data; ours goes to RAM,
 *                               /run/power/log.csv, bounded with one
 *                               rotation. The persist part (age/cycles in
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
 * Outputs, all under the run directory (default /run/power, tmpfs):
 *   state              key=value lines, rewritten atomically (tmp + rename)
 *                      on every sample; `powerd status` prints it
 *   log.csv(.1)        one row per uevent/poll sample (5 s throttle ticks
 *                      only when something changed or 60 s passed), with a
 *                      header; rotated to log.csv.1 at the size limit
 *   shutdown-pending   written with the reason just before a shutdown
 * and "powerd: ..." kmsg lines for transitions only (plug/unplug, status,
 * warn/critical, throttle level, shutdown), which fblog shows on the panel.
 *
 * Policy (stock-derived defaults; every threshold overridable, overrides
 * are logged loudly as TEST OVERRIDE at start and listed in the state file):
 *   - "Discharging" means battery/status reads exactly Discharging AND
 *     usb/online, pc_port/online (where an SDP port shows up; usb/online
 *     is 0 for it) and dc/online are each 0 or their psy is absent. An
 *     unreadable status or online counts as powered. Never the sign of
 *     current_now (positive = discharging,
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
 * Usage: powerd [-r SYSFS] [-d RUNDIR] [-x CMD] [-V VIB] [-K KMSG] [-v]
 *               [test overrides, see usage()]
 *        powerd [-d RUNDIR] status
 *
 * The pure parts (sysfs sample reader against any root, uevent filter,
 * policy step with injected hooks, state/CSV formatting and rotation,
 * poll interval) are covered by tools/tests/test_powerd.c, which includes
 * this file with POWERD_NO_MAIN and never powers anything off.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
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

#define POWERD_SYSFS        "/sys/class/power_supply"
#define POWERD_RUNDIR       "/run/power"
#define POWERD_VIB_PATH     "/sys/class/timed_output/vibrator/enable"
#define POWERD_KMSG_PATH    "/dev/kmsg"
#define POWERD_WARN_PCT     15
#define POWERD_CRIT_PCT     5
#define POWERD_REARM_PCT    20
#define POWERD_EMPTY_PCT    0
#define POWERD_EMPTY_MV     3300
#define POWERD_OVERTEMP_DC  680	/* deci-degrees C, like the psy temp */
#define POWERD_THR_SET_DC   440
#define POWERD_THR_CLR_DC   420
#define POWERD_CONFIRM_N    3
#define POWERD_CONFIRM_GAP_MS 5000
#define POWERD_STEP_MS      5000	/* SS-BATT-BATT sampling */
#define POWERD_POLL_MS      60000
#define POWERD_POLL_LOW_MS  10000
#define POWERD_ROW_IDLE_MS  30000	/* unchanged throttle ticks: a row every 30 s */
#define POWERD_LOG_MAX      (1024 * 1024)
#define POWERD_CRIT_BUZZ_MS 600
#define POWERD_NA           INT_MIN		/* unreadable, -22, not a number */
#define POWERD_ABSENT       (INT_MIN + 1)	/* online of a psy that does not exist */

static bool g_verbose;
static const char *g_kmsg_path = POWERD_KMSG_PATH;
static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_kick;

static void kmsg_note(const char *fmt, ...)
{
	char buf[320];
	va_list ap;
	int fd, n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (g_verbose)
		fprintf(stderr, "powerd: %s\n", buf);
	fd = open(g_kmsg_path, O_WRONLY | O_APPEND | O_CLOEXEC);
	if (fd < 0)
		return;
	dprintf(fd, "powerd: %s\n", buf);
	close(fd);
}

/* ------------------------------------------------------------- sample */

struct sample {
	/* battery psy */
	char status[24];
	char health[24];
	char charge_type[24];
	int present;
	int capacity;		/* % */
	int temp;		/* deci-degrees C */
	int voltage_uv;
	int current_ua;		/* raw: + = discharging, - = charging (Q1) */
	int profile_fcc_ua;	/* constant_charge_current_max: the battery-profile
				 * FCC vote only, NOT the effective FCC; the
				 * throttle's THERMAL_DAEMON_VOTER vote never shows
				 * here (only in the mmi heartbeat's EFFECTIVE FCC) */
	int stl;		/* system_temp_level */
	int nstl;		/* num_system_temp_levels */
	/* bms psy (fuel gauge) */
	int ocv_uv;
	int charge_full_uah;
	int charge_counter_uah;	/* coulomb counter: rises while charging */
	int cycle_count;
	int soc_ready;
	/* usb / dc psy */
	int usb_online;
	char usb_type[24];
	int usb_current_max_ua;
	int usb_input_ua;
	int usb_voltage_uv;
	int input_settled_ua;	/* main/input_current_settled: post-AICL ICL */
	char typec_mode[48];
	int pc_online;		/* pc_port/online: the SDP / host-data-role input */
	int dc_online;
};

/*
 * Read one sysfs attribute into buf (trailing newline stripped). Returns
 * the length, or -errno (a qcom psy attribute whose getter fails reads as
 * -EINVAL, or as the literal text "-22" on some properties).
 */
static int read_attr(const char *root, const char *psy, const char *attr, char *buf, size_t len)
{
	char path[512];
	ssize_t n;
	int fd;

	if (snprintf(path, sizeof(path), "%s/%s/%s", root, psy, attr) >= (int)sizeof(path))
		return -ENAMETOOLONG;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	n = read(fd, buf, len - 1);
	if (n < 0) {
		int err = errno;

		close(fd);
		return -err;
	}
	close(fd);
	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
		buf[--n] = '\0';
	return (int)n;
}

/* Integer attribute, POWERD_NA when missing/unreadable/not a number. With
 * m22_na, the literal -22 (-EINVAL printed by the qcom getters) is also NA;
 * temp keeps it, since -2.2 C is a real reading. */
static int read_int(const char *root, const char *psy, const char *attr, bool m22_na)
{
	char buf[64], *end;
	long v;

	if (read_attr(root, psy, attr, buf, sizeof(buf)) <= 0)
		return POWERD_NA;
	errno = 0;
	v = strtol(buf, &end, 10);
	if (errno || end == buf || *end != '\0' || v <= INT_MIN || v > INT_MAX)
		return POWERD_NA;
	if (m22_na && v == -22)
		return POWERD_NA;
	return (int)v;
}

/* String attribute; "" when missing. Commas (the CSV separator) and
 * control characters become ';' / ' '. */
static void read_str(const char *root, const char *psy, const char *attr, char *out, size_t len)
{
	char buf[128];
	size_t i, n;

	out[0] = '\0';
	if (read_attr(root, psy, attr, buf, sizeof(buf)) <= 0)
		return;
	if (strcmp(buf, "-22") == 0)
		return;
	n = strlen(buf);
	if (n >= len)
		n = len - 1;	/* truncate */
	memcpy(out, buf, n);
	out[n] = '\0';
	for (i = 0; out[i]; i++) {
		if (out[i] == ',')
			out[i] = ';';
		else if ((unsigned char)out[i] < 0x20 || out[i] == '=')
			out[i] = ' ';
	}
}

/* An `online` attribute: POWERD_ABSENT when the psy itself does not exist
 * (a supply that cannot be attached), POWERD_NA when it exists but the
 * value cannot be read (unknown, never "off"). */
static int read_online(const char *root, const char *psy)
{
	char path[512];
	struct stat st;

	snprintf(path, sizeof(path), "%s/%s", root, psy);
	if (stat(path, &st) < 0 && errno == ENOENT)
		return POWERD_ABSENT;
	return read_int(root, psy, "online", true);
}

static void sample_read(const char *root, struct sample *s)
{
	read_str(root, "battery", "status", s->status, sizeof(s->status));
	read_str(root, "battery", "health", s->health, sizeof(s->health));
	read_str(root, "battery", "charge_type", s->charge_type, sizeof(s->charge_type));
	s->present = read_int(root, "battery", "present", true);
	s->capacity = read_int(root, "battery", "capacity", true);
	s->temp = read_int(root, "battery", "temp", false);
	s->voltage_uv = read_int(root, "battery", "voltage_now", true);
	s->current_ua = read_int(root, "battery", "current_now", true);
	s->profile_fcc_ua = read_int(root, "battery", "constant_charge_current_max", true);
	s->stl = read_int(root, "battery", "system_temp_level", true);
	s->nstl = read_int(root, "battery", "num_system_temp_levels", true);
	s->ocv_uv = read_int(root, "bms", "voltage_ocv", true);
	s->charge_full_uah = read_int(root, "bms", "charge_full", true);
	s->charge_counter_uah = read_int(root, "bms", "charge_counter", true);
	s->cycle_count = read_int(root, "bms", "cycle_count", true);
	s->soc_ready = read_int(root, "bms", "soc_reporting_ready", true);
	s->usb_online = read_online(root, "usb");
	read_str(root, "usb", "real_type", s->usb_type, sizeof(s->usb_type));
	s->usb_current_max_ua = read_int(root, "usb", "current_max", true);
	s->usb_input_ua = read_int(root, "usb", "input_current_now", true);
	s->usb_voltage_uv = read_int(root, "usb", "voltage_now", true);
	s->input_settled_ua = read_int(root, "main", "input_current_settled", true);
	read_str(root, "usb", "typec_mode", s->typec_mode, sizeof(s->typec_mode));
	s->pc_online = read_online(root, "pc_port");
	s->dc_online = read_online(root, "dc");
}

/* Any input known to be online. usb/online reads 0 for an SDP (or a PD
 * host data role) port: the kernel reports that input on pc_port/online
 * instead (qpnp-smb2.c usb/pc_port get_prop). */
static bool supply_online(const struct sample *s)
{
	return s->usb_online > 0 || s->pc_online > 0 || s->dc_online > 0;
}

static bool known_off(int online)
{
	return online == 0 || online == POWERD_ABSENT;
}

/*
 * The low-battery policy's "powered", the complement of "known to be
 * discharging": battery/status reads exactly Discharging (smb-lib derives
 * it from raw input presence, so a charger that is attached but not
 * charging reads "Not charging") and usb, pc_port and dc are each known to
 * be offline or do not exist. Anything unreadable counts as powered, so an
 * unknown value can never lead to a low-battery shutdown.
 */
static bool powered(const struct sample *s)
{
	return !(strcmp(s->status, "Discharging") == 0 && known_off(s->usb_online) &&
		 known_off(s->pc_online) && known_off(s->dc_online));
}

static const char *source_name(const struct sample *s)
{
	if (s->usb_online > 0 || s->pc_online > 0)
		return "usb";
	if (s->dc_online > 0)
		return "dc";
	if (powered(s))
		return "unknown";
	return "battery";
}

/* ------------------------------------------------------------- uevent */

/*
 * A kernel uevent datagram is "ACTION@DEVPATH\0KEY=VALUE\0...". udev's
 * rebroadcasts start with "libudev" and are not ours. True when it is a
 * power_supply event.
 */
static bool uevent_is_power_supply(const char *buf, size_t len)
{
	size_t off = 0;

	if (len == 0 || (len >= 7 && memcmp(buf, "libudev", 7) == 0))
		return false;
	if (!memchr(buf, '@', strnlen(buf, len)))
		return false;
	while (off < len) {
		size_t l = strnlen(buf + off, len - off);

		if (l == 22 && memcmp(buf + off, "SUBSYSTEM=power_supply", 22) == 0)
			return true;
		off += l + 1;
	}
	return false;
}

/* ------------------------------------------------------------- policy */

struct config {
	int warn_pct;
	int crit_pct;
	int rearm_pct;
	int empty_pct;
	int empty_mv;
	int overtemp_dc;
	int thr_set_dc;
	int thr_clr_dc;
	int confirm_n;
	int confirm_gap_ms;
	int step_ms;
	int poll_ms;
	int poll_low_ms;
	bool act_when_online;	/* --shutdown-when-online: test only */
	bool throttle;		/* false: never touch system_temp_level */
};

static void config_defaults(struct config *c)
{
	c->warn_pct = POWERD_WARN_PCT;
	c->crit_pct = POWERD_CRIT_PCT;
	c->rearm_pct = POWERD_REARM_PCT;
	c->empty_pct = POWERD_EMPTY_PCT;
	c->empty_mv = POWERD_EMPTY_MV;
	c->overtemp_dc = POWERD_OVERTEMP_DC;
	c->thr_set_dc = POWERD_THR_SET_DC;
	c->thr_clr_dc = POWERD_THR_CLR_DC;
	c->confirm_n = POWERD_CONFIRM_N;
	c->confirm_gap_ms = POWERD_CONFIRM_GAP_MS;
	c->step_ms = POWERD_STEP_MS;
	c->poll_ms = POWERD_POLL_MS;
	c->poll_low_ms = POWERD_POLL_LOW_MS;
	c->act_when_online = false;
	c->throttle = true;
}

struct confirm {
	int count;
	int64_t last;
};

struct powerd {
	struct config cfg;
	const char *sysfs;
	const char *dir;
	const char *vib;		/* NULL/"" = no buzz */
	char overrides[256];		/* "" in production */
	long log_max;

	bool have_prev;
	struct sample prev;
	bool warned;
	bool crit;
	struct confirm empty;
	struct confirm hot;
	bool shutdown_fired;
	bool shutdown_now;
	char shutdown_reason[160];
	int level;			/* the level we last wrote */
	bool level_broken;		/* a write failed: throttle off */
	bool level_evaluated;
	int64_t level_last;
	int64_t last_row;
	bool have_row;

	void (*log)(const char *fmt, ...);
	int (*write_level)(struct powerd *p, int level);
	void (*shutdown)(struct powerd *p);
	void (*buzz)(struct powerd *p, int ms);
};

static int sysfs_write_level(struct powerd *p, int level);
static void system_shutdown(struct powerd *p);
static void vib_buzz(struct powerd *p, int ms);

static void powerd_init(struct powerd *p)
{
	memset(p, 0, sizeof(*p));
	config_defaults(&p->cfg);
	p->sysfs = POWERD_SYSFS;
	p->dir = POWERD_RUNDIR;
	p->vib = POWERD_VIB_PATH;
	p->log_max = POWERD_LOG_MAX;
	p->log = kmsg_note;
	p->write_level = sysfs_write_level;
	p->shutdown = system_shutdown;
	p->buzz = vib_buzz;
}

static const char *alert_name(const struct powerd *p)
{
	if (p->shutdown_fired)
		return "shutdown";
	if (p->hot.count || p->empty.count)
		return "confirming";
	if (p->crit)
		return "critical";
	if (p->warned)
		return "low";
	return "none";
}

/* "4.10 V", "?" when unknown. */
static const char *fmt_volts(char *buf, size_t len, int uv)
{
	if (uv == POWERD_NA)
		snprintf(buf, len, "? V");
	else
		snprintf(buf, len, "%d.%02d V", uv / 1000000, (uv / 10000) % 100);
	return buf;
}

static const char *fmt_pct(char *buf, size_t len, int pct)
{
	if (pct == POWERD_NA)
		snprintf(buf, len, "?");
	else
		snprintf(buf, len, "%d", pct);
	return buf;
}

static const char *fmt_temp(char *buf, size_t len, int dc)
{
	if (dc == POWERD_NA)
		snprintf(buf, len, "?");
	else
		snprintf(buf, len, "%s%d.%d", dc < 0 ? "-" : "", abs(dc) / 10, abs(dc) % 10);
	return buf;
}

/*
 * One confirmation step. A sample without the condition resets the count;
 * a sample with it counts only if it is at least gap_ms after the last
 * counted one, so a uevent burst cannot confirm on its own. Returns true
 * when the count just reached n.
 */
static bool confirm_step(struct confirm *c, bool cond, int64_t now, int gap_ms, int n)
{
	if (!cond) {
		c->count = 0;
		return false;
	}
	if (c->count && now - c->last < gap_ms)
		return false;
	c->count++;
	c->last = now;
	return c->count >= n;
}

static void write_file_atomic(const char *dir, const char *name, const char *data)
{
	char tmp[512], path[512];
	int fd;
	size_t len = strlen(data);

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	snprintf(tmp, sizeof(tmp), "%s/.%s.tmp", dir, name);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return;
	if (write(fd, data, len) != (ssize_t)len) {
		close(fd);
		unlink(tmp);
		return;
	}
	close(fd);
	if (rename(tmp, path) < 0)
		unlink(tmp);
}

static void do_shutdown(struct powerd *p, const char *reason)
{
	char buf[256];

	p->shutdown_fired = true;
	snprintf(p->shutdown_reason, sizeof(p->shutdown_reason), "%s", reason);
	snprintf(buf, sizeof(buf), "%s\n", reason);
	write_file_atomic(p->dir, "shutdown-pending", buf);
	p->log("SHUTDOWN: %s", reason);
	p->shutdown_now = true;	/* powerd_sample runs it after the state/log write */
}

static void set_level(struct powerd *p, int level, const struct sample *s, const char *why)
{
	char t[16];
	int rc;

	if (p->level_broken || level == p->level)
		return;
	rc = p->write_level(p, level);
	if (rc < 0) {
		p->log("system_temp_level %d: %s; charge throttle disabled", level, strerror(-rc));
		p->level_broken = true;
		return;
	}
	p->level = level;
	p->log("charge throttle level %d of %d (battery %s C, %s)", level,
	       s->nstl == POWERD_NA ? 0 : s->nstl - 1, fmt_temp(t, sizeof(t), s->temp), why);
}

static void log_transitions(struct powerd *p, const struct sample *s)
{
	const struct sample *o = &p->prev;
	char c[16], v[16], t[16];

	if (!p->have_prev) {
		p->log("start: %s, %s %%, %s, battery %s C, source %s%s%s",
		       s->status[0] ? s->status : "?", fmt_pct(c, sizeof(c), s->capacity),
		       fmt_volts(v, sizeof(v), s->voltage_uv), fmt_temp(t, sizeof(t), s->temp),
		       source_name(s), supply_online(s) && s->usb_type[0] ? " " : "",
		       supply_online(s) ? s->usb_type : "");
		return;
	}
	if (strcmp(source_name(s), source_name(o)) != 0 ||
	    (supply_online(s) && strcmp(s->usb_type, o->usb_type) != 0)) {
		if (supply_online(s))
			p->log("power source %s (%s, %d mA max, typec %s)", source_name(s),
			       s->usb_type[0] ? s->usb_type : "?",
			       s->usb_current_max_ua == POWERD_NA ? -1 : s->usb_current_max_ua / 1000,
			       s->typec_mode[0] ? s->typec_mode : "?");
		else
			p->log("unplugged: on battery (%s %%, %s)", fmt_pct(c, sizeof(c), s->capacity),
			       fmt_volts(v, sizeof(v), s->voltage_uv));
	}
	if (strcmp(s->status, o->status) != 0)
		p->log("status %s -> %s", o->status[0] ? o->status : "?", s->status[0] ? s->status : "?");
}

/*
 * The whole policy for one sample: transitions, low-battery warn/critical,
 * shutdown confirmation (empty, over-temperature) and the charge throttle.
 */
static void policy_step(struct powerd *p, const struct sample *s, int64_t now)
{
	const struct config *c = &p->cfg;
	bool online = supply_online(s);
	bool discharging = !powered(s) || c->act_when_online;
	bool battery_known = s->present != 0 && s->capacity != POWERD_NA;
	char v[16], t[16], reason[160];

	log_transitions(p, s);
	if (p->shutdown_fired)
		goto out;

	/* low battery (BatteryService) */
	if (!discharging) {
		p->warned = p->crit = false;
	} else if (battery_known) {
		if (s->capacity >= c->rearm_pct)
			p->warned = p->crit = false;
		if (!p->warned && s->capacity <= c->warn_pct) {
			p->warned = true;
			p->log("LOW BATTERY %d %% (%s)", s->capacity, fmt_volts(v, sizeof(v), s->voltage_uv));
		}
		if (!p->crit && s->capacity <= c->crit_pct) {
			p->crit = p->warned = true;
			p->log("CRITICAL BATTERY %d %% (%s): charge now, power-off at %d %% or %d mV",
			       s->capacity, fmt_volts(v, sizeof(v), s->voltage_uv), c->empty_pct, c->empty_mv);
			if (p->vib && p->vib[0])
				p->buzz(p, POWERD_CRIT_BUZZ_MS);
		}
	}

	/* over-temperature: regardless of the supply */
	{
		bool hot = s->temp != POWERD_NA && s->temp > c->overtemp_dc;
		int before = p->hot.count;

		if (confirm_step(&p->hot, hot, now, c->confirm_gap_ms, c->confirm_n)) {
			snprintf(reason, sizeof(reason), "battery over-temperature %s C > %s C (%d samples)",
				 fmt_temp(t, sizeof(t), s->temp), fmt_temp(v, sizeof(v), c->overtemp_dc), p->hot.count);
			do_shutdown(p, reason);
			goto out;
		}
		if (p->hot.count != before && p->hot.count)
			p->log("over-temperature %s C, confirming %d/%d", fmt_temp(t, sizeof(t), s->temp),
			       p->hot.count, c->confirm_n);
	}

	/* empty battery: only when unpowered */
	{
		/* every input known and trusted, or the count resets */
		bool known = s->present == 1 && s->soc_ready == 1 &&
			     s->capacity != POWERD_NA && s->voltage_uv != POWERD_NA;
		bool by_pct = known && s->capacity <= c->empty_pct;
		/* the voltage leg only near empty: ~168 mOhm lets load sag a good battery */
		bool by_mv = known && s->voltage_uv < c->empty_mv * 1000 && s->capacity <= c->crit_pct;
		bool empty = discharging && (by_pct || by_mv);
		int before = p->empty.count;

		if (confirm_step(&p->empty, empty, now, c->confirm_gap_ms, c->confirm_n)) {
			snprintf(reason, sizeof(reason), "battery empty: %s %%, %s (%s; %d samples)%s",
				 fmt_pct(t, sizeof(t), s->capacity),
				 fmt_volts(v, sizeof(v), s->voltage_uv),
				 by_pct ? "capacity" : "voltage", p->empty.count,
				 online ? " [TEST: supply online]" : "");
			do_shutdown(p, reason);
			goto out;
		}
		if (p->empty.count != before && p->empty.count)
			p->log("battery empty (%s %%, %s), confirming %d/%d",
			       fmt_pct(t, sizeof(t), s->capacity),
			       fmt_volts(v, sizeof(v), s->voltage_uv), p->empty.count, c->confirm_n);
	}

	/* charge throttle (thermal-engine SS-BATT-BATT) */
	if (c->throttle) {
		if (!online) {
			if (p->level != 0)
				set_level(p, 0, s, "unplugged");
		} else if (s->nstl != POWERD_NA && s->nstl > 1 && s->temp != POWERD_NA &&
			   (!p->level_evaluated || now - p->level_last >= c->step_ms)) {
			p->level_evaluated = true;
			p->level_last = now;
			if (s->temp >= c->thr_set_dc && p->level < s->nstl - 1)
				set_level(p, p->level + 1, s, "above set point");
			else if (s->temp <= c->thr_clr_dc && p->level > 0)
				set_level(p, p->level - 1, s, "below clear point");
		}
	}
out:
	p->prev = *s;
	p->have_prev = true;
}

/*
 * Milliseconds until the next sample: 5 s while a shutdown condition is
 * being confirmed or a supply is online (throttle cadence), 10 s at low
 * capacity or a hot battery, else 60 s.
 */
static int next_poll_ms(const struct powerd *p, const struct sample *s)
{
	const struct config *c = &p->cfg;
	int ms = c->poll_ms;

	if (p->empty.count || p->hot.count)
		ms = c->confirm_gap_ms;
	else if (supply_online(s) && c->throttle)
		ms = c->step_ms;
	else if ((s->capacity != POWERD_NA && s->capacity <= c->warn_pct) ||
		 (s->temp != POWERD_NA && s->temp >= c->thr_set_dc))
		ms = c->poll_low_ms;
	return ms;
}

/* ------------------------------------------------------------ outputs */

static void fmt_int(char *buf, size_t len, int v)
{
	if (v == POWERD_NA || v == POWERD_ABSENT)
		snprintf(buf, len, "na");
	else
		snprintf(buf, len, "%d", v);
}

/* uV/uA -> mV/mA, "na" kept. */
static void fmt_milli(char *buf, size_t len, int v)
{
	if (v == POWERD_NA)
		snprintf(buf, len, "na");
	else
		snprintf(buf, len, "%d", v / 1000);
}

static void fmt_tempc(char *buf, size_t len, int dc)
{
	if (dc == POWERD_NA)
		snprintf(buf, len, "na");
	else
		fmt_temp(buf, len, dc);
}

static size_t state_format(const struct powerd *p, const struct sample *s, double uptime,
			   char *buf, size_t len)
{
	char cap[16], mv[16], ma[16], tc[16], ocv[16], full[16], cyc[16], rdy[16];
	char umax[16], uin[16], nstl[16], stl[16], cc[16], uv[16], settled[16], fcc[16];

	fmt_int(cap, sizeof(cap), s->capacity);
	fmt_milli(mv, sizeof(mv), s->voltage_uv);
	fmt_milli(ma, sizeof(ma), s->current_ua);
	fmt_tempc(tc, sizeof(tc), s->temp);
	fmt_milli(ocv, sizeof(ocv), s->ocv_uv);
	fmt_int(full, sizeof(full), s->charge_full_uah);
	fmt_int(cyc, sizeof(cyc), s->cycle_count);
	fmt_int(rdy, sizeof(rdy), s->soc_ready);
	fmt_milli(umax, sizeof(umax), s->usb_current_max_ua);
	fmt_milli(uin, sizeof(uin), s->usb_input_ua);
	fmt_int(nstl, sizeof(nstl), s->nstl);
	fmt_int(cc, sizeof(cc), s->charge_counter_uah);
	fmt_int(stl, sizeof(stl), s->stl);
	fmt_milli(uv, sizeof(uv), s->usb_voltage_uv);
	fmt_milli(settled, sizeof(settled), s->input_settled_ua);
	fmt_milli(fcc, sizeof(fcc), s->profile_fcc_ua);
	return (size_t)snprintf(buf, len,
		"uptime=%.1f\n"
		"status=%s\n"
		"source=%s\n"
		"online=%d\n"
		"usb_type=%s\n"
		"capacity=%s\n"
		"voltage_mv=%s\n"
		"current_ma=%s\n"
		"temp_c=%s\n"
		"health=%s\n"
		"charge_type=%s\n"
		"ocv_mv=%s\n"
		"charge_full_uah=%s\n"
		"charge_counter_uah=%s\n"
		"cycle_count=%s\n"
		"soc_ready=%s\n"
		"usb_current_max_ma=%s\n"
		"usb_input_ma=%s\n"
		"usb_voltage_mv=%s\n"
		"input_settled_ma=%s\n"
		"profile_fcc_ma=%s\n"
		"typec_mode=%s\n"
		"throttle_level=%d\n"
		"system_temp_level=%s\n"
		"num_system_temp_levels=%s\n"
		"alert=%s\n"
		"shutdown_reason=%s\n"
		"overrides=%s\n",
		uptime, s->status[0] ? s->status : "na", source_name(s), supply_online(s) ? 1 : 0,
		s->usb_type[0] ? s->usb_type : "na", cap, mv, ma, tc,
		s->health[0] ? s->health : "na", s->charge_type[0] ? s->charge_type : "na",
		ocv, full, cc, cyc, rdy, umax, uin, uv, settled, fcc, s->typec_mode[0] ? s->typec_mode : "na",
		p->level, stl, nstl, alert_name(p), p->shutdown_reason, p->overrides);
}

static const char csv_header[] =
	"uptime,utc,reason,status,source,usb_type,capacity,voltage_mv,current_ma,temp_c,"
	"health,charge_type,ocv_mv,charge_full_uah,charge_counter_uah,cycle_count,soc_ready,usb_current_max_ma,"
	"usb_input_ma,usb_voltage_mv,input_settled_ma,profile_fcc_ma,typec_mode,pc_port_online,dc_online,system_temp_level,throttle_level,alert\n";

static size_t csv_format(const struct powerd *p, const struct sample *s, double uptime,
			 const char *utc, const char *reason, char *buf, size_t len)
{
	char cap[16], mv[16], ma[16], tc[16], ocv[16], full[16], cyc[16], rdy[16];
	char umax[16], uin[16], dc[16], pc[16], stl[16], cc[16], uv[16], settled[16], fcc[16];

	fmt_int(cap, sizeof(cap), s->capacity);
	fmt_milli(mv, sizeof(mv), s->voltage_uv);
	fmt_milli(ma, sizeof(ma), s->current_ua);
	fmt_tempc(tc, sizeof(tc), s->temp);
	fmt_milli(ocv, sizeof(ocv), s->ocv_uv);
	fmt_int(full, sizeof(full), s->charge_full_uah);
	fmt_int(cyc, sizeof(cyc), s->cycle_count);
	fmt_int(rdy, sizeof(rdy), s->soc_ready);
	fmt_milli(umax, sizeof(umax), s->usb_current_max_ua);
	fmt_milli(uin, sizeof(uin), s->usb_input_ua);
	fmt_int(dc, sizeof(dc), s->dc_online);
	fmt_int(pc, sizeof(pc), s->pc_online);
	fmt_int(cc, sizeof(cc), s->charge_counter_uah);
	fmt_int(stl, sizeof(stl), s->stl);
	fmt_milli(uv, sizeof(uv), s->usb_voltage_uv);
	fmt_milli(settled, sizeof(settled), s->input_settled_ua);
	fmt_milli(fcc, sizeof(fcc), s->profile_fcc_ua);
	return (size_t)snprintf(buf, len, "%.1f,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%d,%s\n",
		uptime, utc, reason, s->status, source_name(s), s->usb_type, cap, mv, ma, tc,
		s->health, s->charge_type, ocv, full, cc, cyc, rdy, umax, uin, uv, settled, fcc, s->typec_mode, pc, dc, stl,
		p->level, alert_name(p));
}

/*
 * Append one row to DIR/log.csv, starting a file with the header, and
 * rotating to log.csv.1 (replacing it) first if the row would take the
 * file past log_max.
 */
static int csv_append(const struct powerd *p, const char *row)
{
	char path[512], old[512];
	struct stat st;
	size_t rl = strlen(row);
	int fd;

	snprintf(path, sizeof(path), "%s/log.csv", p->dir);
	if (stat(path, &st) == 0 && st.st_size > 0 &&
	    (long)st.st_size + (long)rl > p->log_max) {
		snprintf(old, sizeof(old), "%s/log.csv.1", p->dir);
		if (rename(path, old) < 0)
			return -errno;
	}
	fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) == 0 && st.st_size == 0 &&
	    write(fd, csv_header, sizeof(csv_header) - 1) < 0) {
		int err = errno;

		close(fd);
		return -err;
	}
	if (write(fd, row, rl) != (ssize_t)rl) {
		int err = errno;

		close(fd);
		return -err;
	}
	close(fd);
	return 0;
}

/* Did anything a reader of the log cares about change since the last row? */
static bool sample_changed(const struct sample *a, const struct sample *b)
{
	return strcmp(a->status, b->status) || a->capacity != b->capacity ||
	       a->usb_online != b->usb_online || a->dc_online != b->dc_online ||
	       a->pc_online != b->pc_online ||
	       strcmp(a->usb_type, b->usb_type) || a->stl != b->stl ||
	       strcmp(a->health, b->health);
}

static void utc_now(char *buf, size_t len)
{
	time_t t = time(NULL);
	struct tm tm;

	if (gmtime_r(&t, &tm))
		strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
	else
		snprintf(buf, len, "na");
}

/*
 * Sample, run the policy, write the state file and (usually) a log row.
 * reason: "start", "uevent", "poll", "tick" (throttle/confirm cadence) or
 * "signal". Returns the ms until the next scheduled sample.
 */
static int powerd_sample(struct powerd *p, const char *reason, int64_t now, double uptime)
{
	struct sample s;
	struct sample before = p->prev;
	bool had_prev = p->have_prev;
	const char *alert_before = alert_name(p);
	int level_before = p->level;
	char buf[2048], utc[32];

	memset(&s, 0, sizeof(s));
	sample_read(p->sysfs, &s);
	policy_step(p, &s, now);
	state_format(p, &s, uptime, buf, sizeof(buf));
	write_file_atomic(p->dir, "state", buf);
	if (p->shutdown_now || strcmp(reason, "tick") != 0 || !had_prev || !p->have_row ||
	    sample_changed(&s, &before) || p->level != level_before ||
	    strcmp(alert_before, alert_name(p)) != 0 || now - p->last_row >= POWERD_ROW_IDLE_MS) {
		utc_now(utc, sizeof(utc));
		csv_format(p, &s, uptime, utc, reason, buf, sizeof(buf));
		csv_append(p, buf);
		p->last_row = now;
		p->have_row = true;
	}
	if (p->shutdown_now) {
		p->shutdown_now = false;
		p->shutdown(p);
	}
	return next_poll_ms(p, &s);
}

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
 * init will not take it either, the syscall. With -x CMD only `sh -c CMD`
 * runs, with no fallback.
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
	sync();
	reboot(RB_POWER_OFF);
}

#ifndef POWERD_NO_MAIN

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
		"usage: powerd [-r SYSFS] [-d RUNDIR] [-x CMD] [-V VIB] [-K KMSG] [-L LOG_MAX] [-v] [TEST OPTIONS]\n"
		"       powerd [-d RUNDIR] status\n"
		"  -r  power_supply sysfs root (default " POWERD_SYSFS ", or $POWERD_SYSFS)\n"
		"  -d  run directory for state, log.csv, shutdown-pending (default " POWERD_RUNDIR ")\n"
		"  -x  shutdown command (sh -c CMD) instead of sync + poweroff; no fallback\n"
		"  -V  vibrator for the critical buzz ('' = none; default " POWERD_VIB_PATH ")\n"
		"  -K  kmsg path (default " POWERD_KMSG_PATH ")\n"
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
	bool ok = true;

	powerd_init(&p);
	env = getenv("POWERD_SYSFS");
	if (env && *env)
		p.sysfs = env;
	while ((opt = getopt_long(argc, argv, "r:d:x:V:K:L:v", longopts, NULL)) != -1) {
		switch (opt) {
		case 'r': p.sysfs = optarg; break;
		case 'd': p.dir = optarg; break;
		case 'x': g_shutdown_cmd = optarg; add_override(&p, "shutdown-cmd", NULL); break;
		case 'V': p.vib = optarg; break;
		case 'K': g_kmsg_path = optarg; break;
		case 'L': p.log_max = atol(optarg); ok = p.log_max >= 1024; break;
		case 'v': g_verbose = true; break;
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
	if (optind < argc) {
		if (strcmp(argv[optind], "status") == 0 && optind + 1 == argc)
			return status_main(p.dir);
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
		kmsg_note("mkdir %s: %s; exiting in 5 s", p.dir, strerror(-rc));
		sleep(5);
		return 1;
	}
	{
		char path[512];

		snprintf(path, sizeof(path), "%s/shutdown-pending", p.dir);
		unlink(path);
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	ufd = uevent_open();
	if (ufd < 0)
		kmsg_note("uevent socket: %s; polling only", strerror(-ufd));
	if (p.overrides[0])
		kmsg_note("TEST OVERRIDE: %s", p.overrides);
	if (c->act_when_online)
		kmsg_note("TEST OVERRIDE: --shutdown-when-online: low-battery policy acts with a supply attached");
	kmsg_note("ready: %s -> %s, warn %d %%, critical %d %%, off at %d %% / %d mV / %d.%d C, throttle %s",
		  p.sysfs, p.dir, c->warn_pct, c->crit_pct, c->empty_pct, c->empty_mv,
		  c->overtemp_dc / 10, abs(c->overtemp_dc) % 10,
		  c->throttle ? "on" : "off");

	/* no stale level from an earlier run survives */
	if (c->throttle) {
		rc = p.write_level(&p, 0);
		if (rc < 0 && rc != -ENOENT)
			kmsg_note("system_temp_level reset: %s", strerror(-rc));
	}

	ms = powerd_sample(&p, "start", now_ms(), uptime_s());
	next_at = now_ms() + ms;
	while (!g_stop) {
		struct pollfd pfd = { .fd = ufd, .events = POLLIN };
		int64_t now = now_ms(), due = next_at;
		int timeout;

		if (uevent_at >= 0 && uevent_at < due)
			due = uevent_at;
		timeout = due > now ? (int)(due - now) : 0;
		if (g_kick)
			timeout = 0;
		rc = poll(&pfd, ufd >= 0 ? 1 : 0, timeout);
		if (rc < 0 && errno != EINTR) {
			kmsg_note("poll: %s; exiting for a respawn", strerror(errno));
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
		}
	}
	if (c->throttle && !p.shutdown_fired && p.level != 0)
		p.write_level(&p, 0);
	kmsg_note("exiting");
	return 0;
}

#endif /* POWERD_NO_MAIN */
