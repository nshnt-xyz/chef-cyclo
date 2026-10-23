/*
 * powerd's policy: the defaults, low-battery warn/critical, the shutdown
 * confirmations (empty, over-temperature), the charge throttle, the poll
 * cadence, and one whole sample (read, policy, state file, the row and
 * whether it is urgent). Actions go through the hooks in struct powerd,
 * so the tests never power anything off. See main.c's header.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common/chef_log.h"
#include "common/chef_paths.h"
#include "powerd.h"

/* ------------------------------------------------------------- policy */

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

/* The caller sets the action hooks: write_level, shutdown and buzz. */
void powerd_init(struct powerd *p)
{
	memset(p, 0, sizeof(*p));
	config_defaults(&p->cfg);
	p->sysfs = POWERD_SYSFS;
	p->dir = CHEF_POWER_DIR;
	p->vib = POWERD_VIB_PATH;
	p->log_max = POWERD_LOG_MAX;
	p->flush_ms = POWERD_FLUSH_MS;
	p->pending_max = POWERD_PENDING_MAX;
	p->flush_due = -1;
	p->log = chef_log;
	p->sync_fd = fsync;
}

const char *alert_name(const struct powerd *p)
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

const char *fmt_temp(char *buf, size_t len, int dc)
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
int next_poll_ms(const struct powerd *p, const struct sample *s)
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
int powerd_sample(struct powerd *p, const char *reason, int64_t now, double uptime)
{
	struct sample s;
	struct sample before = p->prev;
	bool had_prev = p->have_prev;
	const char *alert_before = alert_name(p);
	int level_before = p->level;
	char buf[2048], utc[32];
	bool urgent;

	memset(&s, 0, sizeof(s));
	sample_read(p->sysfs, &s);
	policy_step(p, &s, now);
	state_format(p, &s, uptime, buf, sizeof(buf));
	write_file_atomic(p->dir, "state", buf);
	/* rows worth having on /data at once (-l): after a crash or power loss
	 * these are what explain it */
	urgent = p->shutdown_now || strcmp(reason, "start") == 0 || !had_prev ||
		 supply_online(&s) != supply_online(&before) ||
		 strcmp(source_name(&s), source_name(&before)) != 0 ||
		 strcmp(s.status, before.status) != 0 || p->level != level_before ||
		 strcmp(alert_before, alert_name(p)) != 0;
	if (urgent || strcmp(reason, "tick") != 0 || !p->have_row ||
	    sample_changed(&s, &before) || now - p->last_row >= POWERD_ROW_IDLE_MS) {
		utc_now(utc, sizeof(utc));
		csv_format(p, &s, uptime, utc, reason, buf, sizeof(buf));
		log_row(p, buf, urgent, p->shutdown_now, now);
		p->last_row = now;
		p->have_row = true;
	} else {
		log_flush_if_due(p, now);
	}
	if (p->shutdown_now) {
		p->shutdown_now = false;
		p->shutdown(p);
	}
	return next_poll_ms(p, &s);
}
