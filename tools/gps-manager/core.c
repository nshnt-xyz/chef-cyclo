/*
 * gps-manager's core: the state machine (states, phases, bring-up and
 * teardown order, leases and grace, supervision, backoff and parking, the
 * stale CID of a previous instance). It does nothing itself: every action
 * goes through the injected struct gm_ops, so the host tests drive it
 * with fakes and main.c with real children. See main.c's header.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gps-manager.h"

/* retry delay for streak 0 (after a slow failure) and 1..4 */
static const int gm_backoff_s[] = { 2, 2, 4, 8, 16 };
#define GM_NBACKOFF ((int)(sizeof(gm_backoff_s) / sizeof(gm_backoff_s[0])))

const char *const gm_state_names[] = {
	"OFF", "STARTING", "ACQUIRING", "RUNNING", "STOPPING", "FAILED", "DISABLED",
};
const char *const gm_child_names[] = { "follower", "broker", "gpsd", "step" };
const char *const gm_phase_names[] = {
	"idle", "stale-loc-stop", "stale-release", "owner", "alloc", "nmea-types", "loc-start",
	"up", "stop-pipeline", "loc-stop", "release", "stop-gpsd",
};

void gm_log(struct gm *m, const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (m->ops.log)
		m->ops.log(m->ops.ctx, buf);
}

void gm_set(struct gm *m, enum gm_state st, const char *fmt, ...)
{
	char d[GM_DETAIL_MAX];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(d, sizeof(d), fmt, ap);
	va_end(ap);
	if (st == ST_OFF && !m->enabled) {
		st = ST_DISABLED;	/* idle with the switch off */
		snprintf(d, sizeof(d), "disabled");
	}
	if (st == m->state && strcmp(d, m->detail) == 0)
		return;
	/* ACQUIRING <-> RUNNING is a hint that can flap every ~10 s at a
	 * marginal sky: watchers see every change, the log only the first fix */
	if (!((m->state == ST_ACQUIRING || m->state == ST_RUNNING) &&
	      (st == ST_ACQUIRING || st == ST_RUNNING) && (st == ST_ACQUIRING || m->fix_logged))) {
		gm_log(m, "%s %s", gm_state_names[st], d);
		if (st == ST_RUNNING)
			m->fix_logged = true;
	}
	m->state = st;
	snprintf(m->detail, sizeof(m->detail), "%s", d);
	if (m->ops.changed)
		m->ops.changed(m->ops.ctx, m);
}

void gm_init(struct gm *m, const struct gm_ops *ops)
{
	memset(m, 0, sizeof(*m));
	m->ops = *ops;
	m->qmicli = "qmicli";
	m->qmux_sock = GM_QMUX_SOCK;
	m->grace_ms = GM_GRACE_MS;
	m->cid = -1;
	m->stale_cid = -1;
	m->state = ST_OFF;
	m->enabled = m->loaded = true;
	snprintf(m->detail, sizeof(m->detail), "idle");
}

static void gm_phase(struct gm *m, enum gm_phase ph, int64_t now)
{
	m->phase = ph;
	m->phase_at = now;
	m->escalation = 0;
	m->step_killed = false;
}

bool gm_owner(struct gm *m, int64_t now, struct gm_owner *o)
{
	if (!m->owner_fresh || now - m->owner_at >= GM_OWNER_POLL_MS) {
		bool ok = m->ops.owner(m->ops.ctx, &m->owner_seen);

		if (ok != m->owner_ok || !m->owner_fresh)
			gm_log(m, ok ? "modem owner pid %d ready" : "no modem owner", m->owner_seen.pid);
		m->owner_ok = ok;
		m->owner_at = now;
		m->owner_fresh = true;
	}
	if (o)
		*o = m->owner_seen;
	return m->owner_ok;
}

static bool gm_same_owner(const struct gm_owner *a, const struct gm_owner *b)
{
	return a->pid == b->pid && a->start == b->start;
}

/* qmicli -d SOCK [--client-cid=N [--client-no-release-cid]] A1 [A2] */
static char *const *gm_qmicli_argv(struct gm *m, int cid, bool keep_cid, const char *a1, const char *a2)
{
	int n = 0;

#define GM_ARG(...) (snprintf(m->argv_buf[n], sizeof(m->argv_buf[n]), __VA_ARGS__), \
		     m->argv[n] = m->argv_buf[n], n++)
	GM_ARG("%s", m->qmicli);
	GM_ARG("-d");
	GM_ARG("%s", m->qmux_sock);
	if (cid > 0)
		GM_ARG("--client-cid=%d", cid);
	if (keep_cid)
		GM_ARG("--client-no-release-cid");
	if (a1)
		GM_ARG("%s", a1);
	if (a2)
		GM_ARG("%s", a2);
#undef GM_ARG
	m->argv[n] = NULL;
	return m->argv;
}

static void gm_step(struct gm *m, enum gm_phase ph, int64_t now, char *const argv[])
{
	int rc;

	gm_phase(m, ph, now);
	m->alive[CH_STEP] = true;
	rc = m->ops.run_step(m->ops.ctx, argv);
	if (rc < 0) {
		gm_log(m, "cannot run %s: %s", argv[0], strerror(-rc));
		gm_step_done(m, now, 127, "");	/* as a failed step */
	}
}

static void gm_bringup(struct gm *m, int64_t now)
{
	m->bringups++;
	m->failing = m->fail_fast = m->owner_lost = m->healthy = m->fix_logged = false;
	m->up_at = m->last_fix = m->retry_at = 0;
	m->start_at = now;
	m->first_fix_at = 0;
	if (m->ops.rotate)
		m->ops.rotate(m->ops.ctx);
	gm_phase(m, PH_OWNER, now);
	gm_set(m, ST_STARTING, "waiting for the modem owner");
}

void gm_fail(struct gm *m, int64_t now, bool owner_lost, const char *fmt, ...)
{
	va_list ap;

	if (m->failing)
		return;	/* already coming down for a failure */
	va_start(ap, fmt);
	vsnprintf(m->fail_reason, sizeof(m->fail_reason), fmt, ap);
	va_end(ap);
	m->failing = true;
	m->owner_lost = owner_lost;
	m->fail_fast = !m->healthy;
	m->failures++;
	gm_log(m, "failure: %s (%s)", m->fail_reason,
	       owner_lost ? "owner" : m->fail_fast ? "fast" : "after a healthy run");
	gm_teardown(m, now);
}

static void gm_after_pipe(struct gm *m, int64_t now);
static void gm_stop_gpsd(struct gm *m, int64_t now);

static void gm_finish(struct gm *m, int64_t now)
{
	gm_phase(m, PH_IDLE, now);
	m->session = false;
	m->up_at = 0;
	m->start_at = m->first_fix_at = 0;	/* the bring-up is over; a restart sets them again */
	m->tearing = false;
	if (!m->failing) {
		if (m->leases > 0) {
			gm_log(m, "leased again while stopping: starting");
			gm_bringup(m, now);
		} else {
			gm_set(m, ST_OFF, "idle");
		}
		return;
	}
	if (m->leases == 0) {
		m->streak = 0;
		m->failing = false;
		gm_set(m, ST_OFF, "idle after failure: %s", m->fail_reason);
		return;
	}
	if (m->owner_lost) {
		gm_set(m, ST_FAILED, "%s; waiting for a modem owner", m->fail_reason);
		return;
	}
	m->streak = m->fail_fast ? m->streak + 1 : 0;
	if (m->streak >= GM_MAX_FAST) {
		m->parked = true;
		gm_set(m, ST_FAILED, "%s; %d fast failures in a row, parked until retry or release",
		       m->fail_reason, m->streak);
		return;
	}
	{
		int s = gm_backoff_s[m->streak < GM_NBACKOFF ? m->streak : GM_NBACKOFF - 1];

		m->retry_at = now + (int64_t)s * 1000;
		gm_set(m, ST_FAILED, "%s; retry in %d s", m->fail_reason, s);
	}
}

static const char *gm_why(const struct gm *m)
{
	return m->failing ? m->fail_reason : "released";
}

void gm_teardown(struct gm *m, int64_t now)
{
	m->grace_at = 0;
	switch (m->phase) {
	case PH_OWNER:
		gm_set(m, ST_STOPPING, "%s", gm_why(m));
		gm_finish(m, now);
		return;
	case PH_ALLOC:
	case PH_NMEA:
	case PH_START:
		/* the step ends on its own or at its timeout; gm_step_done goes on */
		m->tearing = true;
		gm_set(m, ST_STOPPING, "%s", gm_why(m));
		return;
	case PH_UP:
		gm_set(m, ST_STOPPING, "%s", gm_why(m));
		gm_phase(m, PH_STOP_PIPE, now);
		if (m->alive[CH_FOLLOW])
			m->ops.signal(m->ops.ctx, CH_FOLLOW, SIGINT);
		else if (m->alive[CH_BROKER])
			m->ops.signal(m->ops.ctx, CH_BROKER, SIGTERM);
		if (!m->alive[CH_FOLLOW] && !m->alive[CH_BROKER])
			gm_after_pipe(m, now);
		return;
	default:
		return;	/* idle, stale cleanup, or already stopping */
	}
}

static void gm_drop_cid(struct gm *m)
{
	m->cid = -1;
	m->session = false;
	m->ops.save(m->ops.ctx, m);
}

static void gm_after_pipe(struct gm *m, int64_t now)
{
	if (m->cid > 0 && !m->ops.bridge(m->ops.ctx)) {
		gm_log(m, "QMUX socket gone: CID %d died with the bridge, skipping loc-stop/release", m->cid);
		gm_drop_cid(m);
	}
	if (m->session && m->cid > 0)
		gm_step(m, PH_LOCSTOP, now, gm_qmicli_argv(m, m->cid, true, "--loc-stop", "--loc-session-id=1"));
	else if (m->cid > 0)
		gm_step(m, PH_RELEASE, now, gm_qmicli_argv(m, m->cid, false, "--loc-noop", NULL));
	else
		gm_stop_gpsd(m, now);
}

static void gm_stop_gpsd(struct gm *m, int64_t now)
{
	gm_phase(m, PH_STOP_GPSD, now);
	if (m->alive[CH_GPSD])
		m->ops.signal(m->ops.ctx, CH_GPSD, SIGTERM);
	else
		gm_finish(m, now);
}

int gm_parse_cid(const char *out)
{
	const char *p = strstr(out, "CID: '");
	char *end;
	long v;

	if (!p)
		return -1;
	v = strtol(p + 6, &end, 10);
	if (end == p + 6 || *end != '\'' || v < 1 || v > 255)
		return -1;
	return (int)v;
}

/* After the stale CID is handled: back to whatever the leases want. */
static void gm_stale_done(struct gm *m, int64_t now)
{
	m->stale_cid = -1;
	m->ops.save(m->ops.ctx, m);
	if (m->leases > 0 && m->stale_from_owner) {
		gm_phase(m, PH_OWNER, now);	/* gm_bringup already ran */
		gm_set(m, ST_STARTING, "waiting for the modem owner");
	} else if (m->leases > 0) {
		gm_bringup(m, now);	/* leased while the stale CID was handled */
	} else {
		gm_phase(m, PH_IDLE, now);
		gm_set(m, ST_OFF, "idle");
	}
}

/* Stale CID from a previous instance: release it once its own owner is ready. */
static bool gm_stale_check(struct gm *m, int64_t now)
{
	struct gm_owner o;

	if (m->stale_cid < 0 || !gm_owner(m, now, &o))
		return false;
	if (!gm_same_owner(&o, &m->stale_owner) || !m->ops.bridge(m->ops.ctx)) {
		gm_log(m, "previous CID %d belonged to owner pid %d, now pid %d: it died with that bridge",
		       m->stale_cid, m->stale_owner.pid, o.pid);
		gm_stale_done(m, now);
		return true;
	}
	m->stale_from_owner = m->phase == PH_OWNER;
	gm_set(m, m->state == ST_OFF ? ST_OFF : ST_STARTING, "releasing the previous instance's CID %d", m->stale_cid);
	gm_step(m, PH_STALE_STOP, now, gm_qmicli_argv(m, m->stale_cid, true, "--loc-stop", "--loc-session-id=1"));
	return true;
}

void gm_step_done(struct gm *m, int64_t now, int rc, const char *out)
{
	enum gm_phase ph = m->phase;

	m->alive[CH_STEP] = false;
	if (rc != 0)
		gm_log(m, "step %s exited %d", gm_phase_names[ph], rc);
	switch (ph) {
	case PH_STALE_STOP:
		gm_step(m, PH_STALE_RELEASE, now, gm_qmicli_argv(m, m->stale_cid, false, "--loc-noop", NULL));
		return;
	case PH_STALE_RELEASE:
		if (rc != 0)
			m->leaked++;
		gm_log(m, "previous CID %d %s", m->stale_cid, rc ? "NOT released (leaked)" : "stopped and released");
		gm_stale_done(m, now);
		return;
	case PH_ALLOC:
		if (rc == 0) {
			int cid = gm_parse_cid(out);

			if (cid > 0) {
				m->cid = cid;
				m->cid_owner = m->owner_seen;
				m->ops.save(m->ops.ctx, m);	/* before anything else uses it */
				gm_log(m, "LOC CID %d", cid);
			} else {
				rc = -1;
			}
		}
		break;
	case PH_START:
		break;	/* session already counts as (maybe) started */
	case PH_LOCSTOP:
		m->session = false;
		gm_step(m, PH_RELEASE, now, gm_qmicli_argv(m, m->cid, false, "--loc-noop", NULL));
		return;
	case PH_RELEASE:
		if (rc != 0) {
			m->leaked++;
			gm_log(m, "CID %d NOT released (leaked: %lu)", m->cid, m->leaked);
		}
		gm_drop_cid(m);
		gm_stop_gpsd(m, now);
		return;
	case PH_NMEA:
		break;
	default:
		return;
	}
	/* bring-up steps */
	if (m->tearing) {
		m->tearing = false;
		gm_phase(m, PH_UP, now);	/* nothing of the pipeline runs yet */
		gm_after_pipe(m, now);
		return;
	}
	if (rc != 0) {
		gm_phase(m, PH_UP, now);	/* no step in flight, nothing spawned */
		gm_fail(m, now, false, "%s failed",
			ph == PH_ALLOC ? "LOC client allocation" : ph == PH_NMEA ? "setting NMEA types" : "LOC start");
		return;
	}
	if (ph == PH_ALLOC) {
		gm_set(m, ST_STARTING, "CID %d: setting NMEA types", m->cid);
		gm_step(m, PH_NMEA, now, gm_qmicli_argv(m, m->cid, true, "--loc-set-nmea-types=" GM_NMEA_TYPES, NULL));
	} else if (ph == PH_NMEA) {
		gm_set(m, ST_STARTING, "CID %d: starting LOC", m->cid);
		/* the modem may start session 1 even if the step then fails or
		 * times out: from here on teardown always sends loc-stop */
		m->session = true;
		gm_step(m, PH_START, now, gm_qmicli_argv(m, m->cid, true, "--loc-start", "--loc-session-id=1"));
	} else {
		int e;

		gm_phase(m, PH_UP, now);
		m->up_at = m->last_nmea = now;
		e = m->ops.spawn_gpsd(m->ops.ctx);
		if (e < 0) {
			gm_fail(m, now, false, "cannot start gpsd: %s", strerror(-e));
			return;
		}
		m->alive[CH_GPSD] = true;
		e = m->ops.spawn_pipeline(m->ops.ctx, m->cid);
		if (e < 0) {
			gm_fail(m, now, false, "cannot start the follower: %s", strerror(-e));
			return;
		}
		m->alive[CH_FOLLOW] = m->alive[CH_BROKER] = true;
		gm_set(m, ST_ACQUIRING, "CID %d, no fix", m->cid);
	}
}

void gm_child_exited(struct gm *m, int64_t now, enum gm_child c, int status)
{
	if (c == CH_STEP || !m->alive[c])
		return;
	m->alive[c] = false;
	switch (m->phase) {
	case PH_UP:
		gm_fail(m, now, false, "%s exited (%d)", gm_child_names[c], status);
		break;
	case PH_STOP_PIPE:
		if (!m->alive[CH_FOLLOW] && !m->alive[CH_BROKER])
			gm_after_pipe(m, now);
		break;
	case PH_STOP_GPSD:
		if (c == CH_GPSD)
			gm_finish(m, now);
		break;
	default:
		break;	/* gpsd died during a teardown step: gone already */
	}
}

void gm_nmea_line(struct gm *m, int64_t now, const char *line)
{
	const char *p;

	if (line[0] != '$')
		return;
	m->nmea_lines++;
	m->last_nmea = now;
	/* $xxRMC,hhmmss.ss,A,... */
	if (strlen(line) > 7 && strncmp(line + 3, "RMC,", 4) == 0) {
		p = strchr(line + 7, ',');
		if (p && p[1] == 'A' && p[2] == ',') {
			m->last_fix = now;
			if (m->start_at && !m->first_fix_at)
				m->first_fix_at = now;
		}
	}
}

void gm_set_leases(struct gm *m, int64_t now, int n)
{
	int was = m->leases;

	m->leases = n;
	if (n > 0) {
		if (m->grace_at) {
			gm_log(m, "leased during the grace period: staying up");
			m->grace_at = 0;
		}
		if (was == 0 && m->phase == PH_IDLE && m->state == ST_OFF)
			gm_bringup(m, now);	/* a pending stale CID is handled in PH_OWNER */
		return;
	}
	if (was == 0)
		return;
	switch (m->phase) {
	case PH_IDLE:
		/* FAILED (waiting, backing off or parked) or a pending stale CID:
		 * nothing of ours is held, so OFF at once */
		m->retry_at = 0;
		m->parked = false;
		m->streak = 0;
		m->failing = false;
		gm_set(m, ST_OFF, "idle");
		break;
	case PH_OWNER:
	case PH_ALLOC:
	case PH_NMEA:
	case PH_START:
	case PH_UP:
		if (!m->enabled) {
			/* switched off: no grace */
			gm_log(m, "switched off: stopping now");
			gm_teardown(m, now);
			break;
		}
		m->grace_at = now + m->grace_ms;
		gm_log(m, "last lease released: stopping in %d s unless leased again", m->grace_ms / 1000);
		break;
	default:
		break;	/* stale cleanup or stopping: it ends in OFF by itself */
	}
}

int gm_retry(struct gm *m, int64_t now)
{
	if (m->state != ST_FAILED || m->phase != PH_IDLE)
		return -1;
	m->parked = false;
	m->streak = 0;
	m->owner_fresh = false;
	if (m->owner_lost)
		return 0;	/* bring-up starts as soon as an owner is there */
	m->retry_at = now;
	return 0;
}

void gm_tick(struct gm *m, int64_t now)
{
	if (m->grace_at && now >= m->grace_at) {
		m->grace_at = 0;
		if (m->leases == 0) {
			m->failing = false;
			gm_log(m, "grace period over: stopping");
			gm_teardown(m, now);
		}
	}
	switch (m->phase) {
	case PH_IDLE:
		if (gm_stale_check(m, now))
			break;
		if (m->leases > 0) {
			if (m->state == ST_FAILED && m->owner_lost) {
				if (gm_owner(m, now, NULL))
					gm_bringup(m, now);
			} else if (m->state == ST_FAILED && !m->parked && m->retry_at && now >= m->retry_at) {
				gm_bringup(m, now);
			}
		}
		break;
	case PH_OWNER:
		if (m->stale_cid > 0 && gm_owner(m, now, NULL)) {
			gm_stale_check(m, now);	/* then back to PH_OWNER */
			break;
		}
		if (gm_owner(m, now, NULL)) {
			gm_set(m, ST_STARTING, "allocating a LOC client");
			gm_step(m, PH_ALLOC, now, gm_qmicli_argv(m, -1, true, "--loc-noop", NULL));
		} else if (now - m->phase_at >= GM_OWNER_WAIT_MS) {
			gm_fail(m, now, true, "no modem owner after %d s", GM_OWNER_WAIT_MS / 1000);
		}
		break;
	case PH_STALE_STOP:
	case PH_STALE_RELEASE:
	case PH_ALLOC:
	case PH_NMEA:
	case PH_START:
	case PH_LOCSTOP:
	case PH_RELEASE:
		if (m->alive[CH_STEP] && now - m->phase_at >= GM_STEP_TIMEOUT_MS) {
			if (!m->step_killed) {
				gm_log(m, "step %s timed out after %d s", gm_phase_names[m->phase], GM_STEP_TIMEOUT_MS / 1000);
				m->step_killed = true;
				/* the bridge may have allocated a CID qmicli never printed */
				if (m->phase == PH_ALLOC)
					m->leaked++;
				m->ops.signal(m->ops.ctx, CH_STEP, SIGTERM);
			} else if (m->escalation == 0 && now - m->phase_at >= GM_STEP_TIMEOUT_MS + GM_TERM_TO_KILL_MS) {
				m->escalation = 1;
				m->ops.signal(m->ops.ctx, CH_STEP, SIGKILL);
			}
		}
		break;
	case PH_UP: {
		struct gm_owner o;

		if (!gm_owner(m, now, &o) || !gm_same_owner(&o, &m->cid_owner)) {
			gm_fail(m, now, true, "modem owner gone");
			break;
		}
		if (now - m->last_nmea >= GM_SILENCE_MS) {
			gm_fail(m, now, false, "no NMEA for %d s", GM_SILENCE_MS / 1000);
			break;
		}
		if (!m->healthy && m->last_nmea - m->up_at >= GM_HEALTHY_MS) {
			m->healthy = true;
			if (m->streak)
				gm_log(m, "healthy for %d s: failure streak %d reset", GM_HEALTHY_MS / 1000, m->streak);
			m->streak = 0;
		}
		if (m->last_fix && now - m->last_fix < GM_FIX_HOLD_MS)
			gm_set(m, ST_RUNNING, "CID %d, fix", m->cid);
		else
			gm_set(m, ST_ACQUIRING, "CID %d, no fix", m->cid);
		break;
	}
	case PH_STOP_PIPE:
		if (m->escalation == 0 && now - m->phase_at >= GM_INT_TO_TERM_MS) {
			m->escalation = 1;
			gm_log(m, "follower/broker still running 5 s after INT: TERM");
			if (m->alive[CH_FOLLOW])
				m->ops.signal(m->ops.ctx, CH_FOLLOW, SIGTERM);
			if (m->alive[CH_BROKER])
				m->ops.signal(m->ops.ctx, CH_BROKER, SIGTERM);
		} else if (m->escalation == 1 && now - m->phase_at >= GM_INT_TO_TERM_MS + GM_TERM_TO_KILL_MS) {
			m->escalation = 2;
			gm_log(m, "follower/broker still running after TERM: KILL");
			if (m->alive[CH_FOLLOW])
				m->ops.signal(m->ops.ctx, CH_FOLLOW, SIGKILL);
			if (m->alive[CH_BROKER])
				m->ops.signal(m->ops.ctx, CH_BROKER, SIGKILL);
		}
		break;
	case PH_STOP_GPSD:
		if (m->escalation == 0 && now - m->phase_at >= GM_GPSD_KILL_MS) {
			m->escalation = 1;
			gm_log(m, "gpsd still running 5 s after TERM: KILL");
			m->ops.signal(m->ops.ctx, CH_GPSD, SIGKILL);
		}
		break;
	}
}
