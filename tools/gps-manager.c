/*
 * gps-manager - demand-driven GNSS for chef-cyclo.
 *
 * gps-up (inittab ::once:) is the boot-resident shared modem owner for GPS
 * and Wi-Fi; this daemon never starts, signals, reaps or restarts it and
 * never changes the modem's operating mode. What it owns is everything
 * GNSS-specific on top of that modem: one QMI LOC client id (CID) with LOC
 * session 1, gpsd, and the `qmicli --loc-follow-nmea | nmea-broker`
 * pipeline. They run only while someone holds a lease on
 * /run/gps-manager.sock, a line protocol in the buttond style:
 *
 *   client -> gps-manager   lease map | lease ride     (counted per connection)
 *                           release map | release ride
 *                           status
 *                           watch                      (stream state changes)
 *                           retry                      (only in FAILED)
 *   gps-manager -> client   ok lease map 1 | err ...
 *                           ok status state=RUNNING leases=map:1,ride:0 ...
 *                           state RUNNING <detail>     (watchers)
 *
 * A lease lives on its connection, so a client that dies releases it. The
 * first lease starts the bring-up; when the last lease goes (in STARTING,
 * ACQUIRING or RUNNING alike) a 30 s grace starts that a new lease cancels,
 * then the owned set is torn down. A lease during STOPPING lets the teardown
 * finish and then brings everything up again; a teardown is never aborted.
 *
 * States: OFF, STARTING, ACQUIRING, RUNNING, STOPPING, FAILED. RUNNING vs.
 * ACQUIRING is a hint only: RUNNING from a status-A RMC, back to ACQUIRING
 * after 10 s without one. Fix data and positions stay in gpsd; nothing here
 * logs or reports them.
 *
 * Bring-up (every qmicli step is a child bounded at 25 s, so the socket never
 * blocks; a step in flight is never killed early, so a CID it allocates is
 * always learnt and later released):
 *   1. wait for the modem owner, polled every 2 s (the C form of
 *      modem-owner.sh's modem_owner_pid: ready marker, QMUX socket, owner
 *      pid + start time, not a zombie): STARTING for up to 240 s, then
 *      FAILED "no modem owner" until it appears. gps-up runs once, so a
 *      missing owner normally lasts until reboot; no qmicli runs meanwhile.
 *   2. qmicli --loc-noop --client-no-release-cid   (CID; proves the bridge)
 *   3. qmicli --client-cid=N --client-no-release-cid
 *             --loc-set-nmea-types=gga|rmc|gsv|gsa|vtg
 *   4. qmicli ... --loc-start --loc-session-id=1
 *   5. gpsd -N -n -b udp://127.0.0.1:20175          (no -G: loopback only)
 *   6. qmicli ... --loc-follow-nmea | nmea-broker -n -t, both forked and
 *      exec'd directly; the broker's tee comes back to this daemon, which
 *      drains it in every state (NMEA silence, fix hint).
 * Teardown, in reverse: SIGINT the follower (TERM after 5 s, KILL after 8 s;
 * the broker exits at EOF and gets the same escalation), --loc-stop, release
 * the CID (--client-cid=N --loc-noop without --client-no-release-cid), TERM
 * gpsd (KILL after 5 s). loc-stop is sent whenever loc-start was attempted
 * (the modem may have started session 1 even if the step failed). A failed
 * loc-stop still releases; a failed release is logged and counted as leaked
 * (status), never retried. With the QMUX socket gone the qmicli steps are
 * skipped: the CID died with the bridge. One window cannot be closed: if the
 * allocation step is killed (its timeout, or the manager dying) after the
 * bridge allocated a CID but before qmicli printed it, that CID stays in
 * qmuxd-lite without a session (a bridge slot, no GNSS cost); a timed-out
 * allocation is therefore counted as (possibly) leaked.
 *
 * Only this daemon may talk on its CID: qmuxd-lite hands a CID's indications
 * to whoever spoke on it last, so another qmicli on it silences the
 * follower. Manual LOC commands must allocate their own CID.
 *
 * Supervision (from the 2026-09-18 ride image, which restarted only the
 * follower): an owned child that exits, 60 s without an NMEA line, or the
 * modem owner going away or being replaced tears the whole owned set down
 * as one unit. While a lease remains it then retries; with no lease it never
 * does. A run is healthy once NMEA has flowed for 120 s since the pipeline
 * started (gaps are bounded by the 60 s silence rule); reaching healthy
 * resets the fast-failure streak to 0. A failure before healthy is fast and
 * adds one to the streak; the retry waits 2, 4, 8, 16 s for streak 1..4 (2 s
 * after a slow failure), and a streak of 5 parks the manager in FAILED until
 * every lease is released or a client sends `retry` (both reset the streak).
 * A lost or replaced owner is not counted: FAILED waits for an owner.
 *
 * Respawn: init restarts this daemon if it dies. One instance at a time: an
 * flock on DIR/lock is taken before anything else and held for life; a
 * second instance exits. Children survive a SIGKILL of the manager (gpsd
 * drops privileges, which clears PR_SET_PDEATHSIG) and a CID stays allocated
 * in qmuxd-lite with LOC session 1 running until someone stops it, so
 * DIR/state (0600, atomic rename) records the CID with the owner that
 * served it as soon as the CID is known, each child's pid + start time
 * right after its fork, and the manager's own pid (for shells and tests,
 * which signal recorded pids only). A new instance kills the recorded
 * follower, broker and gpsd whose start time and name still match (others
 * are skipped and logged), waits for them, and then stops and releases the
 * recorded CID as soon as the same owner is ready, before any bring-up; if
 * the owner was replaced the CID died with the old bridge and is dropped.
 * Clients of the dead instance lost their leases with their connections
 * and lease again.
 *
 * Usage: gps-manager [-S SOCK] [-d DIR] [-q QMUX_SOCK] [-o OWNER_LOCK]
 *                    [-b BINDIR] [-g GRACE_S] [-v]               daemon
 *        gps-manager [-S SOCK] status | watch | retry | hold KIND...
 *   -b  run qmicli, gpsd and nmea-broker from BINDIR (tests); default PATH
 * Logs: kmsg "gps-manager:" lines (transitions, not polls); the children's
 * output in DIR (default /run/gps-manager): steps.log, gpsd.log, follow.log
 * and broker.log, each rotated to .1 at a bring-up so the run that failed is
 * kept next to its retry. RAM only.
 *
 * The core (state machine with injected operations), the lease server, the
 * owner check and the state-file parser are separated from the process
 * plumbing and covered by tools/tests/test_gps-manager.c, which includes
 * this file with GPSMGR_NO_MAIN; tools/tests/test_gps-manager.sh runs the
 * real binary with stub qmicli/gpsd, the real nmea-broker, kill -9 and a
 * respawn.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define GM_SOCK_PATH   "/run/gps-manager.sock"
#define GM_DIR         "/run/gps-manager"
#define GM_QMUX_SOCK   "/run/qmux_socket"
#define GM_OWNER_LOCK  "/run/gps-up.lock"
#define GM_NMEA_TYPES  "gga|rmc|gsv|gsa|vtg"
#define GM_GPSD_SOURCE "udp://127.0.0.1:20175"

#define GM_GRACE_MS        30000
#define GM_STEP_TIMEOUT_MS 25000
#define GM_OWNER_WAIT_MS   240000
#define GM_OWNER_POLL_MS   2000
#define GM_SILENCE_MS      60000
#define GM_HEALTHY_MS      120000
#define GM_MAX_FAST        5
#define GM_FIX_HOLD_MS     10000
#define GM_INT_TO_TERM_MS  5000
#define GM_TERM_TO_KILL_MS 3000
#define GM_GPSD_KILL_MS    5000
#define GM_TICK_MS         1000

#define GM_MAX_CLIENTS 16
#define GM_LINE_MAX    128
#define GM_DETAIL_MAX  112

/* retry delay for streak 0 (after a slow failure) and 1..4 */
static const int gm_backoff_s[] = { 2, 2, 4, 8, 16 };
#define GM_NBACKOFF ((int)(sizeof(gm_backoff_s) / sizeof(gm_backoff_s[0])))

/* ------------------------------------------------------------------ core */

enum gm_state { ST_OFF, ST_STARTING, ST_ACQUIRING, ST_RUNNING, ST_STOPPING, ST_FAILED };
static const char *const gm_state_names[] = {
	"OFF", "STARTING", "ACQUIRING", "RUNNING", "STOPPING", "FAILED",
};

/* Owned processes; CH_STEP is the one qmicli setup/teardown step in flight. */
enum gm_child { CH_FOLLOW, CH_BROKER, CH_GPSD, CH_STEP, CH_COUNT };
static const char *const gm_child_names[] = { "follower", "broker", "gpsd", "step" };

enum gm_phase {
	PH_IDLE,	/* nothing owned: OFF, or FAILED waiting/parked/backing off */
	PH_STALE_STOP, PH_STALE_RELEASE,	/* a previous instance's CID */
	PH_OWNER,	/* STARTING: waiting for the modem owner */
	PH_ALLOC, PH_NMEA, PH_START,	/* STARTING: a qmicli step in flight */
	PH_UP,		/* ACQUIRING/RUNNING: gpsd + pipeline */
	PH_STOP_PIPE,	/* STOPPING: follower/broker exiting */
	PH_LOCSTOP, PH_RELEASE,	/* STOPPING: a qmicli step in flight */
	PH_STOP_GPSD,	/* STOPPING: gpsd exiting */
};
static const char *const gm_phase_names[] = {
	"idle", "stale-loc-stop", "stale-release", "owner", "alloc", "nmea-types", "loc-start",
	"up", "stop-pipeline", "loc-stop", "release", "stop-gpsd",
};

struct gm_owner {
	int pid;
	unsigned long long start;
};

struct gm;

struct gm_ops {
	void *ctx;
	void (*log)(void *ctx, const char *msg);
	/* current modem owner identity, false if there is none ready */
	bool (*owner)(void *ctx, struct gm_owner *o);
	bool (*bridge)(void *ctx);	/* QMUX socket present */
	/* start one qmicli step; its result arrives through gm_step_done() */
	int (*run_step)(void *ctx, char *const argv[]);
	int (*spawn_gpsd)(void *ctx);
	int (*spawn_pipeline)(void *ctx, int cid);
	void (*signal)(void *ctx, enum gm_child c, int sig);
	void (*save)(void *ctx, const struct gm *m);	/* CID/owner/stale changed */
	void (*rotate)(void *ctx);	/* a bring-up starts: keep the previous logs */
	void (*changed)(void *ctx, const struct gm *m);	/* state/detail changed */
};

struct gm {
	struct gm_ops ops;
	const char *qmicli, *qmux_sock;
	int grace_ms;
	enum gm_state state;
	char detail[GM_DETAIL_MAX];
	bool fix_logged;	/* first RUNNING of this bring-up went to the log */
	enum gm_phase phase;
	int64_t phase_at;	/* start of the current phase (timeouts, escalation) */
	int escalation;		/* signals sent so far in a stop phase */
	bool step_killed;
	int leases;		/* total over all clients and kinds */
	int cid;		/* -1: none allocated */
	struct gm_owner cid_owner;	/* the owner that served cid */
	int stale_cid;		/* a previous instance's CID, -1: none */
	struct gm_owner stale_owner;
	bool session;		/* LOC session 1 (maybe) started on cid */
	bool alive[CH_COUNT];
	bool tearing;		/* teardown requested while a bring-up step ran */
	bool failing;		/* the current/last teardown was caused by a failure */
	bool fail_fast;
	bool owner_lost;	/* FAILED waits for an owner; not counted as fast */
	bool parked;		/* FAILED after GM_MAX_FAST fast failures */
	bool stale_from_owner;	/* stale cleanup interrupted a bring-up in PH_OWNER */
	bool healthy;		/* this run has had GM_HEALTHY_MS of NMEA */
	bool owner_ok;		/* cached owner check */
	bool owner_fresh;	/* owner_ok/owner_seen/owner_at are valid */
	struct gm_owner owner_seen;
	int64_t owner_at;	/* when owner_ok was taken */
	char fail_reason[64];
	int streak;		/* consecutive fast failures */
	int64_t grace_at, retry_at, up_at, last_nmea, last_fix;
	unsigned long bringups, failures, nmea_lines, leaked;
	char argv_buf[8][96];
	char *argv[9];
};

static void gm_log(struct gm *m, const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (m->ops.log)
		m->ops.log(m->ops.ctx, buf);
}

static void gm_set(struct gm *m, enum gm_state st, const char *fmt, ...)
{
	char d[GM_DETAIL_MAX];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(d, sizeof(d), fmt, ap);
	va_end(ap);
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

static void gm_init(struct gm *m, const struct gm_ops *ops)
{
	memset(m, 0, sizeof(*m));
	m->ops = *ops;
	m->qmicli = "qmicli";
	m->qmux_sock = GM_QMUX_SOCK;
	m->grace_ms = GM_GRACE_MS;
	m->cid = -1;
	m->stale_cid = -1;
	m->state = ST_OFF;
	snprintf(m->detail, sizeof(m->detail), "idle");
}

static void gm_phase(struct gm *m, enum gm_phase ph, int64_t now)
{
	m->phase = ph;
	m->phase_at = now;
	m->escalation = 0;
	m->step_killed = false;
}

/* The owner, checked at most every GM_OWNER_POLL_MS; *o gets its identity. */
static bool gm_owner(struct gm *m, int64_t now, struct gm_owner *o)
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

static void gm_step_done(struct gm *m, int64_t now, int rc, const char *out);

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
	if (m->ops.rotate)
		m->ops.rotate(m->ops.ctx);
	gm_phase(m, PH_OWNER, now);
	gm_set(m, ST_STARTING, "waiting for the modem owner");
}

static void gm_teardown(struct gm *m, int64_t now);

/* An owned part failed: one full teardown, then retry/park/wait. */
static void gm_fail(struct gm *m, int64_t now, bool owner_lost, const char *fmt, ...)
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

static void gm_teardown(struct gm *m, int64_t now)
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

/* "CID: '3'" in qmicli's "Client ID not released" block. */
static int gm_parse_cid(const char *out)
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

static void gm_step_done(struct gm *m, int64_t now, int rc, const char *out)
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

static void gm_child_exited(struct gm *m, int64_t now, enum gm_child c, int status)
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

/* One line of the broker's tee. Only counted and time-stamped. */
static void gm_nmea_line(struct gm *m, int64_t now, const char *line)
{
	const char *p;

	if (line[0] != '$')
		return;
	m->nmea_lines++;
	m->last_nmea = now;
	/* $xxRMC,hhmmss.ss,A,... */
	if (strlen(line) > 7 && strncmp(line + 3, "RMC,", 4) == 0) {
		p = strchr(line + 7, ',');
		if (p && p[1] == 'A' && p[2] == ',')
			m->last_fix = now;
	}
}

static void gm_set_leases(struct gm *m, int64_t now, int n)
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
		m->grace_at = now + m->grace_ms;
		gm_log(m, "last lease released: stopping in %d s unless leased again", m->grace_ms / 1000);
		break;
	default:
		break;	/* stale cleanup or stopping: it ends in OFF by itself */
	}
}

static int gm_retry(struct gm *m, int64_t now)
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

static void gm_tick(struct gm *m, int64_t now)
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

/* ---------------------------------------------------------- lease server */

enum gm_kind { LK_MAP, LK_RIDE, LK_COUNT };
static const char *const gm_kind_names[] = { "map", "ride" };

struct gm_client {
	int fd;			/* -1 = free slot */
	bool watch;
	bool discarding;
	int leases[LK_COUNT];
	char in[GM_LINE_MAX];
	size_t inlen;
};

struct gm_server {
	struct gm *m;
	struct gm_client clients[GM_MAX_CLIENTS];
	int64_t (*now)(void);
};

static int gm_kind_from_name(const char *s)
{
	int i;

	for (i = 0; s && i < LK_COUNT; i++)
		if (strcmp(s, gm_kind_names[i]) == 0)
			return i;
	return -1;
}

/* kind < 0: all kinds */
static int gm_server_count(const struct gm_server *s, int kind)
{
	int i, k, n = 0;

	for (i = 0; i < GM_MAX_CLIENTS; i++) {
		if (s->clients[i].fd < 0)
			continue;
		for (k = 0; k < LK_COUNT; k++)
			if (kind < 0 || kind == k)
				n += s->clients[i].leases[k];
	}
	return n;
}

static void gm_server_init(struct gm_server *s, struct gm *m, int64_t (*now)(void))
{
	int i;

	memset(s, 0, sizeof(*s));
	s->m = m;
	s->now = now;
	for (i = 0; i < GM_MAX_CLIENTS; i++)
		s->clients[i].fd = -1;
}

static void gm_server_sync(struct gm_server *s)
{
	int n = gm_server_count(s, -1);

	if (n != s->m->leases)
		gm_set_leases(s->m, s->now(), n);
}

static struct gm_client *gm_server_add(struct gm_server *s, int fd)
{
	int i;

	for (i = 0; i < GM_MAX_CLIENTS; i++) {
		struct gm_client *c = &s->clients[i];

		if (c->fd >= 0)
			continue;
		memset(c, 0, sizeof(*c));
		c->fd = fd;
		return c;
	}
	return NULL;
}

static void gm_server_drop(struct gm_server *s, struct gm_client *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	memset(c->leases, 0, sizeof(c->leases));
	c->watch = false;
	gm_server_sync(s);
}

/* Non-blocking; a client whose socket buffer is full is dropped. */
static int gm_send(int fd, const char *line)
{
	size_t len = strlen(line), off = 0;

	while (off < len) {
		ssize_t n = send(fd, line + off, len - off, MSG_NOSIGNAL | MSG_DONTWAIT);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		off += (size_t)n;
	}
	return 0;
}

static void gm_reply(struct gm_server *s, struct gm_client *c, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (gm_send(c->fd, buf))
		gm_server_drop(s, c);
}

static void gm_status_line(const struct gm_server *s, char *buf, size_t len)
{
	const struct gm *m = s->m;
	int i, watchers = 0, clients = 0;

	for (i = 0; i < GM_MAX_CLIENTS; i++) {
		if (s->clients[i].fd < 0)
			continue;
		clients++;
		watchers += s->clients[i].watch;
	}
	snprintf(buf, len,
		 "ok status state=%s leases=map:%d,ride:%d clients=%d watchers=%d cid=%d phase=%s nmea=%lu bringups=%lu failures=%lu streak=%d leaked=%lu grace=%s detail=%s\n",
		 gm_state_names[m->state], gm_server_count(s, LK_MAP), gm_server_count(s, LK_RIDE),
		 clients, watchers, m->cid, gm_phase_names[m->phase], m->nmea_lines, m->bringups,
		 m->failures, m->streak, m->leaked, m->grace_at ? "on" : "off", m->detail);
}

/* State change: one "state S detail" line to every watcher. */
static void gm_server_broadcast(struct gm_server *s)
{
	char buf[GM_DETAIL_MAX + 32];
	int i;

	snprintf(buf, sizeof(buf), "state %s %s\n", gm_state_names[s->m->state], s->m->detail);
	for (i = 0; i < GM_MAX_CLIENTS; i++) {
		struct gm_client *c = &s->clients[i];

		if (c->fd >= 0 && c->watch && gm_send(c->fd, buf))
			gm_server_drop(s, c);
	}
}

static void gm_handle_line(struct gm_server *s, struct gm_client *c, char *line)
{
	char *cmd, *arg, *save = NULL;

	cmd = strtok_r(line, " \t\r", &save);
	if (!cmd)
		return;
	arg = strtok_r(NULL, " \t\r", &save);
	if (strcmp(cmd, "lease") == 0 || strcmp(cmd, "release") == 0) {
		int k = gm_kind_from_name(arg);

		if (k < 0) {
			gm_reply(s, c, "err unknown lease kind %s\n", arg ? arg : "");
			return;
		}
		if (cmd[0] == 'l') {
			c->leases[k]++;
		} else if (c->leases[k] > 0) {
			c->leases[k]--;
		} else {
			gm_reply(s, c, "err no %s lease on this connection\n", gm_kind_names[k]);
			return;
		}
		gm_reply(s, c, "ok %s %s %d\n", cmd, gm_kind_names[k], c->leases[k]);
		if (c->fd >= 0)
			gm_server_sync(s);
	} else if (strcmp(cmd, "watch") == 0) {
		c->watch = true;
		gm_reply(s, c, "ok watch\nstate %s %s\n", gm_state_names[s->m->state], s->m->detail);
	} else if (strcmp(cmd, "status") == 0) {
		char buf[512];

		gm_status_line(s, buf, sizeof(buf));
		gm_reply(s, c, "%s", buf);
	} else if (strcmp(cmd, "retry") == 0) {
		if (gm_retry(s->m, s->now()) == 0)
			gm_reply(s, c, "ok retry\n");
		else
			gm_reply(s, c, "err retry only applies in FAILED\n");
	} else {
		gm_reply(s, c, "err unknown command %s\n", cmd);
	}
}

static void gm_client_input(struct gm_server *s, struct gm_client *c)
{
	ssize_t n;
	char *nl;

	n = recv(c->fd, c->in + c->inlen, sizeof(c->in) - 1 - c->inlen, MSG_DONTWAIT);
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
		gm_server_drop(s, c);
		return;
	}
	if (n < 0)
		return;
	c->inlen += (size_t)n;
	c->in[c->inlen] = '\0';
	while ((nl = memchr(c->in, '\n', c->inlen))) {
		size_t used = (size_t)(nl - c->in) + 1;

		*nl = '\0';
		if (c->discarding)
			c->discarding = false;
		else
			gm_handle_line(s, c, c->in);
		if (c->fd < 0)
			return;
		memmove(c->in, c->in + used, c->inlen - used);
		c->inlen -= used;
		c->in[c->inlen] = '\0';
	}
	if (c->discarding) {
		c->inlen = 0;
	} else if (c->inlen == sizeof(c->in) - 1) {
		gm_reply(s, c, "err line too long\n");
		c->inlen = 0;
		c->discarding = true;
	}
}

/* ------------------------------------------------- owner and state file */

/* /proc/PID/stat: start time (field 22), zombie state and comm. */
static int gm_proc_start(const char *proc, int pid, unsigned long long *start, bool *zombie,
			 char *comm, size_t commlen)
{
	char path[128], buf[512], *p, *q;
	ssize_t n;
	int fd, field;
	char st;

	snprintf(path, sizeof(path), "%s/%d/stat", proc, pid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -EIO;
	buf[n] = '\0';
	p = strchr(buf, '(');
	q = strrchr(buf, ')');
	if (!p || !q || q < p || q[1] != ' ')
		return -EINVAL;
	if (comm)
		snprintf(comm, commlen, "%.*s", (int)(q - p - 1), p + 1);
	p = q + 2;	/* field 3: state */
	st = *p;
	for (field = 3; field < 22 && p; field++) {
		p = strchr(p, ' ');
		if (p)
			p++;
	}
	if (!p)
		return -EINVAL;
	*start = strtoull(p, NULL, 10);
	if (zombie)
		*zombie = st == 'Z';
	return 0;
}

struct gm_paths {
	const char *proc;	/* "/proc" */
	const char *owner_lock;	/* "/run/gps-up.lock" */
	const char *qmux_sock;	/* "/run/qmux_socket" */
};

static bool gm_bridge_present(const struct gm_paths *p)
{
	struct stat sb;

	return stat(p->qmux_sock, &sb) == 0 && S_ISSOCK(sb.st_mode);
}

/* modem-owner.sh's modem_owner_pid() in C. */
static bool gm_owner_check(const struct gm_paths *p, struct gm_owner *o)
{
	char path[256], buf[64];
	unsigned long long want, have;
	bool zombie;
	int fd, pid;
	ssize_t n;

	memset(o, 0, sizeof(*o));
	snprintf(path, sizeof(path), "%s/ready", p->owner_lock);
	if (access(path, F_OK) != 0 || !gm_bridge_present(p))
		return false;
	snprintf(path, sizeof(path), "%s/owner", p->owner_lock);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return false;
	buf[n] = '\0';
	if (sscanf(buf, "%d %llu", &pid, &want) != 2 || pid <= 1)
		return false;
	if (gm_proc_start(p->proc, pid, &have, &zombie, NULL, 0) || zombie || have != want)
		return false;
	o->pid = pid;
	o->start = have;
	return true;
}

/* DIR/state: "cid N", "owner PID START", "<child> PID START" lines. */
struct gm_record {
	int cid;
	struct gm_owner owner;
	int pid[CH_STEP];
	unsigned long long start[CH_STEP];
};

static int gm_child_from_name(const char *s)
{
	int i;

	for (i = 0; i < CH_STEP; i++)
		if (strcmp(s, gm_child_names[i]) == 0)
			return i;
	return -1;
}

static void gm_record_parse(struct gm_record *r, const char *text)
{
	const char *p = text;

	memset(r, 0, sizeof(*r));
	r->cid = -1;
	while (p && *p) {
		char name[16];
		int a;
		unsigned long long b;
		int k = sscanf(p, "%15s %d %llu", name, &a, &b);

		if (k >= 2 && strcmp(name, "cid") == 0) {
			if (a >= 1 && a <= 255)
				r->cid = a;
		} else if (k == 3 && a > 1 && strcmp(name, "owner") == 0) {
			r->owner.pid = a;
			r->owner.start = b;
		} else if (k == 3 && a > 1) {
			int c = gm_child_from_name(name);

			if (c >= 0) {
				r->pid[c] = a;
				r->start[c] = b;
			}
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
}

static int gm_record_format(const struct gm_record *r, char *buf, size_t len)
{
	size_t off = 0;
	int i;

	off += (size_t)snprintf(buf + off, len - off, "cid %d\n", r->cid);
	if (r->owner.pid > 0 && off < len)
		off += (size_t)snprintf(buf + off, len - off, "owner %d %llu\n", r->owner.pid, r->owner.start);
	for (i = 0; i < CH_STEP && off < len; i++)
		if (r->pid[i] > 0)
			off += (size_t)snprintf(buf + off, len - off, "%s %d %llu\n",
						gm_child_names[i], r->pid[i], r->start[i]);
	return off < len ? 0 : -ENOSPC;
}

/* Name each recorded child must still have to be signalled (comm, 15 chars). */
static const char *const gm_child_comm[] = { "qmicli", "nmea-broker", "gpsd" };

/* Is the recorded child still that same process (pid, start time, name)? */
static bool gm_record_match(const char *proc, const struct gm_record *r, int c)
{
	unsigned long long start;
	bool zombie;
	char comm[32];

	if (r->pid[c] <= 1)
		return false;
	if (gm_proc_start(proc, r->pid[c], &start, &zombie, comm, sizeof(comm)))
		return false;
	return !zombie && start == r->start[c] && strcmp(comm, gm_child_comm[c]) == 0;
}

#ifndef GPSMGR_NO_MAIN

/* ------------------------------------------------------------- the shell */

static bool g_verbose;
static volatile sig_atomic_t g_stop;
static int g_sigpipe[2] = { -1, -1 };
static pid_t g_self;

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void kmsg_note(const char *msg)
{
	char buf[320];
	int fd, n;

	n = snprintf(buf, sizeof(buf), "gps-manager: %s\n", msg);
	if (n >= (int)sizeof(buf))
		n = (int)sizeof(buf) - 1;
	if (g_verbose)
		fputs(buf, stderr);
	fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	if (write(fd, buf, (size_t)n) < 0) {
		/* nothing sensible to do */
	}
	close(fd);
}

static void notef(const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	kmsg_note(buf);
}

struct shell {
	struct gm m;
	struct gm_server srv;
	struct gm_paths paths;
	const char *dir;
	char bin[3][256];	/* qmicli, gpsd, nmea-broker */
	pid_t pid[CH_COUNT];
	unsigned long long start[CH_COUNT];
	int step_fd;		/* step stdout+stderr, -1 when closed */
	bool step_exited;
	int step_rc;
	char step_out[4096];
	size_t step_len;
	int nmea_fd;		/* broker tee, -1 when closed */
	char nmea_in[512];
	size_t nmea_len;
};

static void shell_save(struct shell *sh)
{
	struct gm_record r;
	char buf[256], path[256], tmp[256];
	int fd, i;

	memset(&r, 0, sizeof(r));
	/* a pending stale CID stays recorded until it is handled */
	if (sh->m.stale_cid > 0) {
		r.cid = sh->m.stale_cid;
		r.owner = sh->m.stale_owner;
	} else {
		r.cid = sh->m.cid;
		if (sh->m.cid > 0)
			r.owner = sh->m.cid_owner;
	}
	for (i = 0; i < CH_STEP; i++) {
		r.pid[i] = sh->pid[i];
		r.start[i] = sh->start[i];
	}
	if (gm_record_format(&r, buf, sizeof(buf)))
		return;
	/* for tests and shells: signal the manager by this pid, never by name */
	snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "manager %d\n", (int)g_self);
	snprintf(path, sizeof(path), "%s/state", sh->dir);
	snprintf(tmp, sizeof(tmp), "%s/state.tmp", sh->dir);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) {
		notef("cannot write %s: %s", tmp, strerror(errno));
		return;
	}
	if (write(fd, buf, strlen(buf)) != (ssize_t)strlen(buf) || fsync(fd) != 0) {
		notef("cannot write %s: %s; %s keeps its previous contents", tmp, strerror(errno), path);
		close(fd);
		unlink(tmp);
		return;
	}
	close(fd);
	if (rename(tmp, path) != 0) {
		notef("cannot rename %s: %s", tmp, strerror(errno));
		unlink(tmp);
	}
}

static void op_log(void *ctx, const char *msg)
{
	(void)ctx;
	kmsg_note(msg);
}

static bool op_owner(void *ctx, struct gm_owner *o)
{
	struct shell *sh = ctx;

	return gm_owner_check(&sh->paths, o);
}

static bool op_bridge(void *ctx)
{
	struct shell *sh = ctx;

	return gm_bridge_present(&sh->paths);
}

static int open_log(struct shell *sh, const char *name, bool trunc)
{
	char path[256];

	snprintf(path, sizeof(path), "%s/%s", sh->dir, name);
	return open(path, O_WRONLY | O_CREAT | O_CLOEXEC | (trunc ? O_TRUNC : O_APPEND), 0600);
}

static void op_rotate(void *ctx)
{
	static const char *const logs[] = { "steps.log", "gpsd.log", "follow.log", "broker.log" };
	struct shell *sh = ctx;
	char a[256], b[256];
	size_t i;

	for (i = 0; i < sizeof(logs) / sizeof(logs[0]); i++) {
		snprintf(a, sizeof(a), "%s/%s", sh->dir, logs[i]);
		snprintf(b, sizeof(b), "%s/%s.1", sh->dir, logs[i]);
		rename(a, b);
	}
}

/*
 * fork + exec, the given fds as stdin/stdout/stderr (-1 = /dev/null). Every
 * fd of ours is O_CLOEXEC, so the child inherits only its stdio. The child
 * dies with us (PR_SET_PDEATHSIG; if we died before the prctl it exits).
 */
static pid_t spawn(const char *path, char *const argv[], int in, int out, int err)
{
	pid_t pid = fork();

	if (pid < 0)
		return -errno;
	if (pid == 0) {
		int nul = open("/dev/null", O_RDWR | O_CLOEXEC);
		sigset_t none;

		prctl(PR_SET_PDEATHSIG, SIGTERM);
		if (getppid() != g_self)
			_exit(127);
		setpgid(0, 0);
		dup2(in >= 0 ? in : nul, 0);
		dup2(out >= 0 ? out : nul, 1);
		dup2(err >= 0 ? err : nul, 2);
		signal(SIGPIPE, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		signal(SIGCHLD, SIG_DFL);
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		execvp(path, argv);
		_exit(127);
	}
	return pid;
}

static void shell_started(struct shell *sh, enum gm_child c, pid_t pid)
{
	unsigned long long start = 0;

	sh->pid[c] = pid;
	gm_proc_start(sh->paths.proc, pid, &start, NULL, NULL, 0);
	sh->start[c] = start;
	if (c != CH_STEP)
		shell_save(sh);
}

static void log_line(struct shell *sh, const char *name, const char *text, size_t len)
{
	int fd = open_log(sh, name, false);

	if (fd < 0)
		return;
	if (write(fd, text, len) < 0) {
		/* best effort */
	}
	close(fd);
}

static int op_run_step(void *ctx, char *const argv[])
{
	struct shell *sh = ctx;
	char line[512];
	size_t off = 0;
	int p[2], i;
	pid_t pid;

	if (pipe2(p, O_CLOEXEC) < 0)
		return -errno;
	pid = spawn(sh->bin[0], argv, -1, p[1], p[1]);
	close(p[1]);
	if (pid < 0) {
		close(p[0]);
		return (int)pid;
	}
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	sh->step_fd = p[0];
	sh->step_exited = false;
	sh->step_rc = 0;
	sh->step_len = 0;
	sh->step_out[0] = '\0';
	shell_started(sh, CH_STEP, pid);
	for (i = 0; argv[i] && off < sizeof(line) - 2; i++)
		off += (size_t)snprintf(line + off, sizeof(line) - off, "%s%s", i ? " " : "$ ", argv[i]);
	if (off > sizeof(line) - 2)
		off = sizeof(line) - 2;
	line[off++] = '\n';
	log_line(sh, "steps.log", line, off);
	return 0;
}

static int op_spawn_gpsd(void *ctx)
{
	struct shell *sh = ctx;
	char *argv[] = { "gpsd", "-N", "-n", "-b", GM_GPSD_SOURCE, NULL };
	int logfd = open_log(sh, "gpsd.log", true);
	pid_t pid = spawn(sh->bin[1], argv, -1, logfd, logfd);

	if (logfd >= 0)
		close(logfd);
	if (pid < 0)
		return (int)pid;
	shell_started(sh, CH_GPSD, pid);
	return 0;
}

static int op_spawn_pipeline(void *ctx, int cid)
{
	struct shell *sh = ctx;
	char cidarg[32];
	char *follow[] = { "qmicli", "-d", (char *)sh->m.qmux_sock, cidarg, "--client-no-release-cid",
			   "--loc-follow-nmea", NULL };
	char *broker[] = { "nmea-broker", "-n", "-t", NULL };
	int a[2], b[2], flog, blog;
	pid_t fp = -1, bp;

	snprintf(cidarg, sizeof(cidarg), "--client-cid=%d", cid);
	if (pipe2(a, O_CLOEXEC) < 0)
		return -errno;
	if (pipe2(b, O_CLOEXEC) < 0) {
		int e = -errno;

		close(a[0]);
		close(a[1]);
		return e;
	}
	flog = open_log(sh, "follow.log", true);
	blog = open_log(sh, "broker.log", true);
	bp = spawn(sh->bin[2], broker, a[0], b[1], blog);
	if (bp > 0) {
		shell_started(sh, CH_BROKER, bp);
		fp = spawn(sh->bin[0], follow, -1, a[1], flog);
		if (fp > 0)
			shell_started(sh, CH_FOLLOW, fp);
	}
	close(a[0]);
	close(a[1]);
	close(b[1]);
	if (flog >= 0)
		close(flog);
	if (blog >= 0)
		close(blog);
	fcntl(b[0], F_SETFL, O_NONBLOCK);
	sh->nmea_fd = b[0];	/* drained even if the follower failed to start */
	sh->nmea_len = 0;
	if (bp < 0)
		return (int)bp;
	if (fp < 0) {
		/* the broker is recorded and reaped like any other child; the
		 * core only marks it alive once both started, so tell it now */
		sh->m.alive[CH_BROKER] = true;
		return (int)fp;
	}
	return 0;
}

static void op_signal(void *ctx, enum gm_child c, int sig)
{
	struct shell *sh = ctx;

	if (sh->pid[c] > 0)
		kill(sh->pid[c], sig);
}

static void op_save(void *ctx, const struct gm *m)
{
	(void)m;
	shell_save(ctx);
}

static void op_changed(void *ctx, const struct gm *m)
{
	struct shell *sh = ctx;

	(void)m;
	gm_server_broadcast(&sh->srv);
}

/* A step is finished once it has exited and its output pipe is at EOF. */
static void shell_step_check(struct shell *sh)
{
	if (!sh->step_exited || sh->step_fd >= 0)
		return;
	sh->step_exited = false;
	gm_step_done(&sh->m, now_ms(), sh->step_rc, sh->step_out);
}

static void shell_step_read(struct shell *sh)
{
	char buf[1024];
	ssize_t n = read(sh->step_fd, buf, sizeof(buf));

	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (n <= 0) {
		close(sh->step_fd);
		sh->step_fd = -1;
		shell_step_check(sh);
		return;
	}
	log_line(sh, "steps.log", buf, (size_t)n);
	if (sh->step_len < sizeof(sh->step_out) - 1) {
		size_t k = (size_t)n;

		if (k > sizeof(sh->step_out) - 1 - sh->step_len)
			k = sizeof(sh->step_out) - 1 - sh->step_len;
		memcpy(sh->step_out + sh->step_len, buf, k);
		sh->step_len += k;
		sh->step_out[sh->step_len] = '\0';
	}
}

/* Drain the broker's tee completely: it must never block on it. */
static void shell_nmea_read(struct shell *sh)
{
	char buf[1024];

	for (;;) {
		ssize_t n = read(sh->nmea_fd, buf, sizeof(buf));
		ssize_t i;

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
			return;
		if (n <= 0) {
			close(sh->nmea_fd);
			sh->nmea_fd = -1;
			return;
		}
		for (i = 0; i < n; i++) {
			if (buf[i] == '\n') {
				sh->nmea_in[sh->nmea_len] = '\0';
				gm_nmea_line(&sh->m, now_ms(), sh->nmea_in);
				sh->nmea_len = 0;
			} else if (sh->nmea_len < sizeof(sh->nmea_in) - 1) {
				sh->nmea_in[sh->nmea_len++] = buf[i];
			}	/* else: overlong junk, truncated */
		}
	}
}

static void shell_reap(struct shell *sh)
{
	int status, c;
	pid_t pid;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);

		for (c = 0; c < CH_COUNT; c++)
			if (sh->pid[c] == pid)
				break;
		if (c == CH_COUNT)
			continue;
		sh->pid[c] = 0;
		sh->start[c] = 0;
		if (c == CH_STEP) {
			sh->step_exited = true;
			sh->step_rc = rc;
			shell_step_check(sh);
		} else {
			notef("%s (pid %d) exited %d", gm_child_names[c], (int)pid, rc);
			shell_save(sh);
			gm_child_exited(&sh->m, now_ms(), (enum gm_child)c, rc);
		}
	}
}

/*
 * A previous instance died: kill its recorded follower, broker and gpsd
 * (only if pid, start time and name all still match), wait for them, and
 * hand its CID to the core as stale, to be stopped and released once the
 * same owner is ready.
 */
static void shell_cleanup_previous(struct shell *sh)
{
	char path[256], buf[512];
	struct gm_record r;
	ssize_t n;
	int fd, c, i;

	snprintf(path, sizeof(path), "%s/state", sh->dir);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n < 0)
		n = 0;
	buf[n] = '\0';
	gm_record_parse(&r, buf);
	for (c = 0; c < CH_STEP; c++) {
		if (!r.pid[c])
			continue;
		if (!gm_record_match(sh->paths.proc, &r, c)) {
			notef("previous %s pid %d: gone or no longer ours, not signalled", gm_child_names[c], r.pid[c]);
			continue;
		}
		notef("stopping the previous instance's %s (pid %d)", gm_child_names[c], r.pid[c]);
		kill(r.pid[c], c == CH_FOLLOW ? SIGINT : SIGTERM);
		for (i = 0; i < 30 && gm_record_match(sh->paths.proc, &r, c); i++)
			usleep(100000);
		if (gm_record_match(sh->paths.proc, &r, c)) {
			kill(r.pid[c], SIGKILL);
			for (i = 0; i < 30 && gm_record_match(sh->paths.proc, &r, c); i++)
				usleep(100000);
		}
	}
	if (r.cid > 0) {
		sh->m.stale_cid = r.cid;
		sh->m.stale_owner = r.owner;
		notef("previous instance held LOC CID %d (owner pid %d): releasing it when that owner is ready",
		      r.cid, r.owner.pid);
	}
}

static int sock_listen(const char *path)
{
	struct sockaddr_un sa;
	int fd;

	if (strlen(path) >= sizeof(sa.sun_path))
		return -ENAMETOOLONG;
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	strcpy(sa.sun_path, path);
	unlink(path);	/* only ever while holding the instance lock */
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || chmod(path, 0660) < 0 || listen(fd, 8) < 0) {
		int err = errno;

		close(fd);
		return -err;
	}
	return fd;
}

static int sock_connect(const char *path)
{
	struct sockaddr_un sa;
	int fd;

	if (strlen(path) >= sizeof(sa.sun_path))
		return -ENAMETOOLONG;
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	strcpy(sa.sun_path, path);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		int err = errno;

		close(fd);
		return -err;
	}
	return fd;
}

static void wake(char c)
{
	int e = errno;

	if (g_sigpipe[1] >= 0 && write(g_sigpipe[1], &c, 1) < 0) {
		/* full: a wakeup is already pending */
	}
	errno = e;
}

static void on_stop(int sig)
{
	(void)sig;
	g_stop = 1;
	wake('s');
}

static void on_chld(int sig)
{
	(void)sig;
	wake('c');
}

static void usage(void)
{
	fprintf(stderr,
		"usage: gps-manager [-S SOCK] [-d DIR] [-q QMUX_SOCK] [-o OWNER_LOCK] [-b BINDIR] [-g GRACE_S] [-v]\n"
		"       gps-manager [-S SOCK] status | watch | retry | hold KIND...   (KIND: map, ride)\n");
}

static int client_main(const char *sock_path, int argc, char **argv)
{
	int fd;
	char buf[512];
	bool once = strcmp(argv[0], "status") == 0 || strcmp(argv[0], "retry") == 0;
	int i;

	if (!once && strcmp(argv[0], "watch") != 0 && strcmp(argv[0], "hold") != 0) {
		usage();
		return 64;
	}
	if (strcmp(argv[0], "hold") == 0 && argc < 2) {
		usage();
		return 64;
	}
	fd = sock_connect(sock_path);
	if (fd < 0) {
		fprintf(stderr, "gps-manager: connect %s: %s\n", sock_path, strerror(-fd));
		return 1;
	}
	if (strcmp(argv[0], "hold") == 0) {
		for (i = 1; i < argc; i++) {
			snprintf(buf, sizeof(buf), "lease %s\n", argv[i]);
			if (write(fd, buf, strlen(buf)) < 0)
				return 1;
		}
		if (write(fd, "watch\n", 6) < 0)
			return 1;
	} else {
		snprintf(buf, sizeof(buf), "%s\n", argv[0]);
		if (write(fd, buf, strlen(buf)) < 0)
			return 1;
	}
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));

		if (n <= 0)
			break;
		if (fwrite(buf, 1, (size_t)n, stdout) != (size_t)n)
			break;
		fflush(stdout);
		if (once && memchr(buf, '\n', (size_t)n)) {
			close(fd);
			return strncmp(buf, "ok", 2) == 0 ? 0 : 1;
		}
	}
	close(fd);
	return 0;
}

static struct shell g_sh;

static int64_t srv_now(void)
{
	return now_ms();
}

/* One poll round: signals, children, step output, NMEA, clients, tick. */
static void shell_round(struct shell *sh, int lfd)
{
	struct pollfd pfd[4 + GM_MAX_CLIENTS];
	struct gm_client *cmap[GM_MAX_CLIENTS];
	int n = 0, nclients = 0, i, ilisten = -1, istep = -1, inmea = -1, iclients;

	pfd[n++] = (struct pollfd){ .fd = g_sigpipe[0], .events = POLLIN };
	if (lfd >= 0) {
		ilisten = n;
		pfd[n++] = (struct pollfd){ .fd = lfd, .events = POLLIN };
	}
	if (sh->step_fd >= 0) {
		istep = n;
		pfd[n++] = (struct pollfd){ .fd = sh->step_fd, .events = POLLIN };
	}
	if (sh->nmea_fd >= 0) {
		inmea = n;
		pfd[n++] = (struct pollfd){ .fd = sh->nmea_fd, .events = POLLIN };
	}
	iclients = n;
	for (i = 0; i < GM_MAX_CLIENTS; i++) {
		if (sh->srv.clients[i].fd < 0)
			continue;
		cmap[nclients++] = &sh->srv.clients[i];
		pfd[n++] = (struct pollfd){ .fd = sh->srv.clients[i].fd, .events = POLLIN };
	}
	if (poll(pfd, (nfds_t)n, lfd >= 0 ? GM_TICK_MS : 200) < 0 && errno != EINTR)
		return;
	if (pfd[0].revents & POLLIN) {
		char junk[64];

		while (read(g_sigpipe[0], junk, sizeof(junk)) > 0)
			;
	}
	if (inmea >= 0 && sh->nmea_fd >= 0 && (pfd[inmea].revents & (POLLIN | POLLHUP | POLLERR)))
		shell_nmea_read(sh);
	if (istep >= 0 && sh->step_fd >= 0 && (pfd[istep].revents & (POLLIN | POLLHUP | POLLERR)))
		shell_step_read(sh);
	shell_reap(sh);
	if (ilisten >= 0 && (pfd[ilisten].revents & POLLIN)) {
		int cfd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

		if (cfd >= 0 && !gm_server_add(&sh->srv, cfd)) {
			notef("client limit (%d) reached, refusing", GM_MAX_CLIENTS);
			close(cfd);
		}
	}
	for (i = 0; i < nclients; i++)
		if (cmap[i]->fd >= 0 && (pfd[iclients + i].revents & (POLLIN | POLLERR | POLLHUP)))
			gm_client_input(&sh->srv, cmap[i]);
	gm_tick(&sh->m, now_ms());
}

int main(int argc, char **argv)
{
	struct shell *sh = &g_sh;
	const char *sock_path = GM_SOCK_PATH, *bindir = NULL;
	struct gm_ops ops = {
		.ctx = sh, .log = op_log, .owner = op_owner, .bridge = op_bridge,
		.run_step = op_run_step, .spawn_gpsd = op_spawn_gpsd, .spawn_pipeline = op_spawn_pipeline,
		.signal = op_signal, .save = op_save, .rotate = op_rotate, .changed = op_changed,
	};
	struct sigaction sa;
	char path[256];
	int lfd, lockfd, opt, i, grace_s = GM_GRACE_MS / 1000;

	memset(sh, 0, sizeof(*sh));
	sh->dir = GM_DIR;
	sh->paths.proc = "/proc";
	sh->paths.owner_lock = GM_OWNER_LOCK;
	sh->paths.qmux_sock = GM_QMUX_SOCK;
	while ((opt = getopt(argc, argv, "S:d:q:o:b:g:v")) != -1) {
		switch (opt) {
		case 'S': sock_path = optarg; break;
		case 'd': sh->dir = optarg; break;
		case 'q': sh->paths.qmux_sock = optarg; break;
		case 'o': sh->paths.owner_lock = optarg; break;
		case 'b': bindir = optarg; break;
		case 'g': grace_s = atoi(optarg); break;
		case 'v': g_verbose = true; break;
		default: usage(); return 64;
		}
	}
	if (optind < argc)
		return client_main(sock_path, argc - optind, argv + optind);
	if (grace_s < 0) {
		usage();
		return 64;
	}
	snprintf(sh->bin[0], sizeof(sh->bin[0]), "%s%sqmicli", bindir ? bindir : "", bindir ? "/" : "");
	snprintf(sh->bin[1], sizeof(sh->bin[1]), "%s%sgpsd", bindir ? bindir : "", bindir ? "/" : "");
	snprintf(sh->bin[2], sizeof(sh->bin[2]), "%s%snmea-broker", bindir ? bindir : "", bindir ? "/" : "");
	g_self = getpid();

	/* one instance: the lock comes before the state file and the socket */
	mkdir(sh->dir, 0700);
	snprintf(path, sizeof(path), "%s/lock", sh->dir);
	lockfd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB) < 0) {
		int e = errno;

		notef("%s: %s; another gps-manager is running, exiting in 5 s", path, strerror(e));
		fprintf(stderr, "gps-manager: already running (%s: %s)\n", path, strerror(e));
		sleep(5);	/* keeps a respawning init from spinning */
		return 1;
	}

	sh->step_fd = sh->nmea_fd = -1;
	gm_init(&sh->m, &ops);
	sh->m.qmicli = "qmicli";
	sh->m.qmux_sock = sh->paths.qmux_sock;
	sh->m.grace_ms = grace_s * 1000;
	gm_server_init(&sh->srv, &sh->m, srv_now);

	if (pipe2(g_sigpipe, O_CLOEXEC | O_NONBLOCK) < 0) {
		notef("pipe: %s; exiting in 5 s", strerror(errno));
		sleep(5);
		return 1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_stop;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sa.sa_handler = on_chld;
	sa.sa_flags = SA_NOCLDSTOP | SA_RESTART;
	sigaction(SIGCHLD, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	shell_cleanup_previous(sh);
	shell_save(sh);

	lfd = sock_listen(sock_path);
	if (lfd < 0) {
		notef("listen %s: %s; exiting in 5 s", sock_path, strerror(-lfd));
		sleep(5);
		return 1;
	}
	notef("ready on %s, idle until leased", sock_path);

	while (!g_stop)
		shell_round(sh, lfd);

	/* orderly shutdown (init's SIGTERM): everything down, bounded */
	notef("exiting: tearing down");
	for (i = 0; i < GM_MAX_CLIENTS; i++)
		if (sh->srv.clients[i].fd >= 0) {
			close(sh->srv.clients[i].fd);
			sh->srv.clients[i].fd = -1;
		}
	close(lfd);
	unlink(sock_path);
	sh->m.leases = 0;
	sh->m.grace_at = 0;
	sh->m.failing = false;
	gm_teardown(&sh->m, now_ms());
	{
		int64_t until = now_ms() + 3 * GM_STEP_TIMEOUT_MS;

		while (sh->m.phase != PH_IDLE && now_ms() < until)
			shell_round(sh, -1);
	}
	if (sh->m.phase == PH_IDLE && sh->m.stale_cid < 0 && sh->m.cid < 0) {
		snprintf(path, sizeof(path), "%s/state", sh->dir);
		unlink(path);
	}
	notef("exited %s (%s)", gm_state_names[sh->m.state], gm_phase_names[sh->m.phase]);
	return 0;
}

#endif /* GPSMGR_NO_MAIN */
