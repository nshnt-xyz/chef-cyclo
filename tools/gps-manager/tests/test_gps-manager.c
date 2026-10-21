/* Host tests for tools/gps-manager/: the core state machine
 * through fake operations that record every step/spawn/signal/save in a
 * trace (ordered bring-up and teardown, lease counting and grace, leases
 * changing mid STARTING and during STOPPING, failures, backoff, the
 * fast-failure streak and its reset, parking and retry, silence, owner
 * loss/replacement and the 2 s owner poll, stale CIDs from a previous
 * instance, leaked releases, a missing bridge, step timeouts and kill
 * escalation), the JSON line protocol over real socketpairs through
 * tools/common/linesrv (the hello, every reply, error and event,
 * per-connection leases, connection death, the client limit, a watcher
 * that does not read dropped with its leases and the lines after it in
 * order), the state-file record and the /proc and owner checks against a
 * fake tree.
 * Linked with core.c, server.c, owner.c and ../common; nothing is forked.
 * Build/run: see tools/gps-manager/Makefile.
 */
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include "common/chef_log.h"
#include "common/linesrv.h"
#include "../gps-manager.h"

static int g_failures;
static int g_tests;

#define CHECK(cond) do { \
	g_tests++; \
	if (!(cond)) { \
		g_failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

/* ---- fake operations ---- */

static char g_trace[8192];
static bool g_owner_ok = true;
static struct gm_owner g_owner_id = { 100, 5000 };
static int g_owner_calls;
static bool g_bridge = true;
static int g_run_rc;		/* run_step return */
static int g_saved_cid = -2;
static int g_changes;
static char g_last_log[256];
static int g_hint_logs;		/* ACQUIRING/RUNNING lines that reached the log */

static void tr(const char *fmt, ...)
{
	size_t off = strlen(g_trace);
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(g_trace + off, sizeof(g_trace) - off, fmt, ap);
	va_end(ap);
}

static void f_log(void *ctx, const char *msg)
{
	(void)ctx;
	snprintf(g_last_log, sizeof(g_last_log), "%s", msg);
	if (strncmp(msg, "ACQUIRING ", 10) == 0 || strncmp(msg, "RUNNING ", 8) == 0)
		g_hint_logs++;
}

static bool f_owner(void *ctx, struct gm_owner *o)
{
	(void)ctx;
	g_owner_calls++;
	*o = g_owner_ok ? g_owner_id : (struct gm_owner){ 0, 0 };
	return g_owner_ok;
}

static bool f_bridge(void *ctx)
{
	(void)ctx;
	return g_bridge;
}

static int f_run_step(void *ctx, char *const argv[])
{
	int i;

	(void)ctx;
	tr("step:");
	for (i = 1; argv[i]; i++)	/* skip "qmicli -d SOCK" */
		if (i > 2)
			tr("%s%s", i > 3 ? " " : "", argv[i]);
	tr("|");
	return g_run_rc;
}

static int g_gpsd_rc, g_pipe_rc;

static int f_spawn_gpsd(void *ctx)
{
	(void)ctx;
	tr("gpsd|");
	return g_gpsd_rc;
}

static int f_spawn_pipeline(void *ctx, int cid)
{
	(void)ctx;
	tr("pipe:%d|", cid);
	return g_pipe_rc;
}

static void f_signal(void *ctx, enum gm_child c, int sig)
{
	(void)ctx;
	tr("sig:%s:%d|", gm_child_names[c], sig);
}

static void f_save(void *ctx, const struct gm *m)
{
	(void)ctx;
	g_saved_cid = m->stale_cid > 0 ? m->stale_cid : m->cid;
	tr("save:%d|", g_saved_cid);
}

static void f_rotate(void *ctx)
{
	(void)ctx;
	tr("rotate|");
}

static void f_changed(void *ctx, const struct gm *m)
{
	(void)ctx;
	(void)m;
	g_changes++;
}

static void f_persist(void *ctx, bool on)
{
	(void)ctx;
	tr("persist:%d|", on);
}

static const struct gm_ops f_ops = {
	.ctx = NULL, .log = f_log, .owner = f_owner, .bridge = f_bridge, .run_step = f_run_step,
	.spawn_gpsd = f_spawn_gpsd, .spawn_pipeline = f_spawn_pipeline, .signal = f_signal,
	.save = f_save, .rotate = f_rotate, .changed = f_changed, .persist = f_persist,
};

static void reset(struct gm *m)
{
	g_trace[0] = '\0';
	g_owner_ok = true;
	g_owner_id = (struct gm_owner){ 100, 5000 };
	g_owner_calls = 0;
	g_bridge = true;
	g_run_rc = g_gpsd_rc = g_pipe_rc = 0;
	g_saved_cid = -2;
	g_hint_logs = 0;
	gm_init(m, &f_ops);
}

static const char ALLOC_OUT[] =
	"[/run/chef/qmux_socket] Client ID not released:\n\tService: 'loc'\n\t    CID: '3'\n";

/* lease at t, bring it all the way to ACQUIRING with CID 3; returns time */
static int64_t bring_up(struct gm *m, int64_t t)
{
	int i;

	gm_set_leases(m, t, 1);
	for (i = 0; i < 3 && m->phase != PH_ALLOC; i++)
		gm_tick(m, t);	/* idle -> owner -> alloc */
	gm_step_done(m, t + 100, 0, ALLOC_OUT);
	gm_step_done(m, t + 200, 0, "Successfully set NMEA types\n");
	gm_step_done(m, t + 300, 0, "Successfully started location tracking\n");
	return t + 300;
}

/* follower+broker exit, loc-stop, release, gpsd exit */
static void finish_teardown(struct gm *m, int64_t t)
{
	gm_child_exited(m, t, CH_FOLLOW, 0);
	gm_child_exited(m, t, CH_BROKER, 0);
	if (m->phase == PH_LOCSTOP)
		gm_step_done(m, t + 10, 0, "");
	if (m->phase == PH_RELEASE)
		gm_step_done(m, t + 20, 0, "");
	if (m->phase == PH_STOP_GPSD)
		gm_child_exited(m, t + 30, CH_GPSD, 0);
}

static bool held_nothing(const struct gm *m)
{
	int c;

	for (c = 0; c < CH_COUNT; c++)
		if (m->alive[c])
			return false;
	return m->cid < 0 && !m->session && m->phase == PH_IDLE;
}

/* ---- tests ---- */

static void test_bringup_teardown_order(void)
{
	struct gm m;
	int64_t t;

	reset(&m);
	CHECK(m.state == ST_OFF);
	gm_tick(&m, 0);
	CHECK(g_trace[0] == '\0');	/* unleased: nothing at all */
	t = bring_up(&m, 1000);
	CHECK(strcmp(g_trace,
		     "rotate|step:--client-no-release-cid --loc-noop|save:3|"
		     "step:--client-cid=3 --client-no-release-cid --loc-set-nmea-types=gga|rmc|gsv|gsa|vtg|"
		     "step:--client-cid=3 --client-no-release-cid --loc-start --loc-session-id=1|"
		     "gpsd|pipe:3|") == 0);
	CHECK(m.state == ST_ACQUIRING && m.cid == 3 && m.session);
	CHECK(m.alive[CH_GPSD] && m.alive[CH_FOLLOW] && m.alive[CH_BROKER]);
	CHECK(m.start_at > 0 && m.first_fix_at == 0);	/* the bring-up started, no fix yet */

	/* fix hint with hysteresis */
	gm_nmea_line(&m, t + 1000, "$GPGGA,120000.00,,,,,0,00,99.9,,,,,,*00");
	gm_tick(&m, t + 1000);
	CHECK(m.state == ST_ACQUIRING);
	gm_nmea_line(&m, t + 2000, "$GPRMC,120001.00,A,1234.5,N,01234.5,E,0.0,,041026,,,A*00");
	gm_tick(&m, t + 2000);
	CHECK(m.state == ST_RUNNING);
	CHECK(m.first_fix_at == t + 2000 && m.first_fix_at > m.start_at);	/* the first fix of this bring-up */
	gm_nmea_line(&m, t + 2100, "$GPRMC,120001.50,A,1234.5,N,01234.5,E,0.0,,041026,,,A*00");
	CHECK(m.first_fix_at == t + 2000);	/* later fixes do not move it */
	gm_nmea_line(&m, t + 3000, "$GPRMC,120002.00,V,,,,,,,041026,,,N*00");
	gm_tick(&m, t + 8000);
	CHECK(m.state == ST_RUNNING);	/* 6 s without A: held */
	gm_nmea_line(&m, t + 11000, "$GPGSA,A,1*00");
	gm_tick(&m, t + 12100);
	CHECK(m.state == ST_ACQUIRING);	/* 10 s without A */
	CHECK(strstr(m.detail, "1234") == NULL);	/* no positions */

	/* last release: grace, then the ordered teardown */
	g_trace[0] = '\0';
	t += 13000;
	gm_set_leases(&m, t, 0);
	gm_nmea_line(&m, t + 29000, "$GPGSA,A,1*00");
	gm_tick(&m, t + 29000);
	CHECK(m.state == ST_ACQUIRING && g_trace[0] == '\0');
	gm_tick(&m, t + 30000);
	CHECK(m.state == ST_STOPPING);
	CHECK(strcmp(g_trace, "sig:follower:2|") == 0);
	gm_child_exited(&m, t + 30100, CH_FOLLOW, 0);
	CHECK(m.phase == PH_STOP_PIPE);	/* the broker still drains */
	gm_child_exited(&m, t + 30200, CH_BROKER, 0);
	gm_step_done(&m, t + 30300, 0, "");
	gm_step_done(&m, t + 30400, 0, "");
	CHECK(m.phase == PH_STOP_GPSD);
	gm_child_exited(&m, t + 30500, CH_GPSD, 0);
	CHECK(strcmp(g_trace,
		     "sig:follower:2|"
		     "step:--client-cid=3 --client-no-release-cid --loc-stop --loc-session-id=1|"
		     "step:--client-cid=3 --loc-noop|save:-1|sig:gpsd:15|") == 0);
	CHECK(m.state == ST_OFF && held_nothing(&m));
	CHECK(m.start_at == 0 && m.first_fix_at == 0);	/* the bring-up is over */
}

static void test_grace_cancel_and_second_lease(void)
{
	struct gm m;
	int64_t t;

	reset(&m);
	t = bring_up(&m, 1000);
	gm_set_leases(&m, t, 2);
	gm_set_leases(&m, t + 10, 1);
	CHECK(m.grace_at == 0 && m.phase == PH_UP);
	gm_nmea_line(&m, t + 50, "$GPRMC,120001.00,A,1234.5,N,01234.5,E,0.0,,041026,,,A*00");
	{
		int64_t start = m.start_at, fix = m.first_fix_at;

		CHECK(start > 0 && fix == t + 50);
		gm_set_leases(&m, t + 100, 0);
		CHECK(m.grace_at == t + 100 + GM_GRACE_MS);
		gm_set_leases(&m, t + 20000, 1);
		CHECK(m.grace_at == 0);
		CHECK(m.start_at == start && m.first_fix_at == fix);	/* the same bring-up: its age and fix go on */
	}
	g_trace[0] = '\0';
	gm_nmea_line(&m, t + 59000, "$GPGSA,A,1*00");
	gm_tick(&m, t + 60000);
	CHECK(m.phase == PH_UP && g_trace[0] == '\0');
	CHECK(m.bringups == 1);
}

static void test_release_mid_starting(void)
{
	struct gm m;
	int64_t t = 0;

	/* released while the CID allocation runs: the step is not killed, the
	 * CID it allocates is released, nothing else is started */
	reset(&m);
	m.grace_ms = 5000;	/* expires while the step still runs */
	gm_set_leases(&m, t, 1);
	gm_tick(&m, t);
	CHECK(m.phase == PH_ALLOC);
	gm_set_leases(&m, t + 10, 0);
	gm_tick(&m, t + 10 + 5000);
	CHECK(m.state == ST_STOPPING && m.tearing);
	CHECK(strstr(g_trace, "sig:") == NULL);
	g_trace[0] = '\0';
	gm_step_done(&m, t + 10 + 5000 + 50, 0, ALLOC_OUT);
	CHECK(strcmp(g_trace, "save:3|step:--client-cid=3 --loc-noop|") == 0);
	gm_step_done(&m, t + 10 + 5000 + 60, 0, "");
	CHECK(m.state == ST_OFF && held_nothing(&m));

	/* released after LOC start was sent: loc-stop then release */
	reset(&m);
	gm_set_leases(&m, t, 1);
	gm_tick(&m, t);
	gm_step_done(&m, t + 100, 0, ALLOC_OUT);
	gm_step_done(&m, t + 200, 0, "");
	CHECK(m.phase == PH_START);
	m.grace_ms = 5000;
	gm_set_leases(&m, t + 210, 0);
	gm_tick(&m, t + 210 + 5000);
	g_trace[0] = '\0';
	gm_step_done(&m, t + 210 + 5000 + 10, 0, "");
	CHECK(strcmp(g_trace, "step:--client-cid=3 --client-no-release-cid --loc-stop --loc-session-id=1|") == 0);
	gm_step_done(&m, t + 210 + 5000 + 20, 0, "");
	gm_step_done(&m, t + 210 + 5000 + 30, 0, "");
	CHECK(strstr(g_trace, "gpsd") == NULL);
	CHECK(m.state == ST_OFF && held_nothing(&m));

	/* released while waiting for the owner: nothing to undo */
	reset(&m);
	g_owner_ok = false;
	gm_set_leases(&m, t, 1);
	gm_tick(&m, t);
	gm_set_leases(&m, t + 10, 0);
	gm_tick(&m, t + 10 + GM_GRACE_MS);
	CHECK(m.state == ST_OFF && held_nothing(&m));
	CHECK(strstr(g_trace, "step:") == NULL);
}

static void test_lease_during_stopping(void)
{
	struct gm m;
	int64_t t;

	reset(&m);
	t = bring_up(&m, 0);
	gm_set_leases(&m, t, 0);
	gm_nmea_line(&m, t + GM_GRACE_MS, "$GPGSA,A,1*00");
	gm_tick(&m, t + GM_GRACE_MS);
	CHECK(m.state == ST_STOPPING);
	gm_set_leases(&m, t + GM_GRACE_MS + 10, 1);
	CHECK(m.phase == PH_STOP_PIPE);	/* the teardown is not aborted */
	g_trace[0] = '\0';
	finish_teardown(&m, t + GM_GRACE_MS + 20);
	CHECK(strstr(g_trace, "--loc-stop") && strstr(g_trace, "--client-cid=3 --loc-noop"));
	CHECK(m.state == ST_STARTING && m.phase == PH_OWNER && m.bringups == 2);
	CHECK(m.cid == -1);
}

static void test_failure_backoff_park(void)
{
	struct gm m;
	int64_t t = 0;
	int i;
	static const int want[] = { 2, 4, 8, 16 };

	reset(&m);
	t = bring_up(&m, t);
	for (i = 0; i < GM_MAX_FAST; i++) {
		CHECK(m.phase == PH_UP);
		g_trace[0] = '\0';
		gm_child_exited(&m, t + 1000, CH_BROKER, 1);	/* fast: 1 s in */
		CHECK(m.state == ST_STOPPING);
		CHECK(strcmp(g_trace, "sig:follower:2|") == 0);
		gm_child_exited(&m, t + 1100, CH_FOLLOW, 0);
		gm_step_done(&m, t + 1200, 0, "");
		gm_step_done(&m, t + 1300, 0, "");
		gm_child_exited(&m, t + 1400, CH_GPSD, 0);
		CHECK(m.state == ST_FAILED && held_nothing(&m));
		CHECK(m.streak == i + 1);
		if (i + 1 < GM_MAX_FAST) {
			char d[32];

			snprintf(d, sizeof(d), "retry in %d s", want[i]);
			CHECK(strstr(m.detail, d) != NULL);
			CHECK(m.retry_at == t + 1400 + want[i] * 1000);
			gm_tick(&m, m.retry_at - 1);
			CHECK(m.phase == PH_IDLE);
			t = bring_up(&m, m.retry_at);	/* the lease is still held */
			CHECK(m.bringups == (unsigned long)i + 2);
		}
	}
	CHECK(m.parked && strstr(m.detail, "parked"));
	g_trace[0] = '\0';
	gm_tick(&m, t + 3600000);
	CHECK(m.phase == PH_IDLE && g_trace[0] == '\0');	/* parked: no retry */
	/* retry leaves it */
	CHECK(gm_retry(&m, t + 3600000) == 0);
	CHECK(m.streak == 0 && !m.parked);
	gm_tick(&m, t + 3600001);
	CHECK(m.phase == PH_OWNER);
	CHECK(gm_retry(&m, t + 3600002) < 0);	/* only in FAILED */
}

static void test_release_while_failed(void)
{
	struct gm m;
	int64_t t;

	reset(&m);
	t = bring_up(&m, 0);
	gm_child_exited(&m, t + 10, CH_GPSD, 1);
	finish_teardown(&m, t + 20);
	CHECK(m.state == ST_FAILED && held_nothing(&m));
	gm_set_leases(&m, t + 30, 0);
	CHECK(m.state == ST_OFF && held_nothing(&m) && m.streak == 0);
	g_trace[0] = '\0';
	gm_tick(&m, t + 600000);
	CHECK(g_trace[0] == '\0');	/* unleased: no retry, no qmicli */

	/* failure while unleased in grace: OFF, no retry */
	reset(&m);
	t = bring_up(&m, 0);
	gm_set_leases(&m, t, 0);
	gm_child_exited(&m, t + 10, CH_FOLLOW, 1);
	finish_teardown(&m, t + 20);
	CHECK(m.state == ST_OFF && held_nothing(&m));
	g_trace[0] = '\0';
	gm_tick(&m, t + 600000);
	CHECK(g_trace[0] == '\0');
}

static void test_silence_and_healthy(void)
{
	struct gm m;
	int64_t t, u;

	/* stall after a short run counts as fast */
	reset(&m);
	t = bring_up(&m, 0);
	gm_nmea_line(&m, t + 10000, "$GPGSA,A,1*00");
	gm_tick(&m, t + 10000 + GM_SILENCE_MS - 1);
	CHECK(m.phase == PH_UP);
	gm_tick(&m, t + 10000 + GM_SILENCE_MS);
	CHECK(m.state == ST_STOPPING && strstr(m.detail, "no NMEA"));
	finish_teardown(&m, t + 80000);
	CHECK(m.streak == 1 && strstr(m.detail, "retry in 2 s"));

	/* two fast failures, then a healthy run resets the streak */
	reset(&m);
	m.streak = 2;
	t = bring_up(&m, 0);
	for (u = t; u <= t + GM_HEALTHY_MS; u += 1000) {
		gm_nmea_line(&m, u, "$GPGSA,A,1*00");
		gm_tick(&m, u);
	}
	CHECK(m.healthy && m.streak == 0);
	gm_child_exited(&m, u, CH_FOLLOW, 1);
	finish_teardown(&m, u + 10);
	CHECK(m.streak == 0 && strstr(m.detail, "retry in 2 s"));
	CHECK(m.failures == 1);
}

static void test_owner(void)
{
	struct gm m;
	int64_t t;

	/* missing owner: STARTING, polled every 2 s, no qmicli, then FAILED */
	reset(&m);
	g_owner_ok = false;
	gm_set_leases(&m, 0, 1);
	for (t = 0; t < 20000; t += 1000)
		gm_tick(&m, t);
	CHECK(g_owner_calls == 10);
	CHECK(m.state == ST_STARTING && strstr(g_trace, "step:") == NULL);
	gm_tick(&m, GM_OWNER_WAIT_MS);
	CHECK(m.state == ST_FAILED && strstr(m.detail, "waiting for a modem owner"));
	CHECK(m.streak == 0 && held_nothing(&m));
	gm_tick(&m, GM_OWNER_WAIT_MS + 600000);
	CHECK(strstr(g_trace, "step:") == NULL);
	g_owner_ok = true;
	gm_tick(&m, GM_OWNER_WAIT_MS + 602000);
	CHECK(m.phase == PH_OWNER);
	gm_tick(&m, GM_OWNER_WAIT_MS + 603000);
	CHECK(m.phase == PH_ALLOC);

	/* owner replaced while up: teardown, FAILED waiting, not counted */
	reset(&m);
	t = bring_up(&m, 0);
	g_owner_id.start = 9999;
	gm_tick(&m, t + GM_OWNER_POLL_MS);
	CHECK(m.state == ST_STOPPING && strstr(m.detail, "owner gone"));
	finish_teardown(&m, t + 3000);
	CHECK(m.state == ST_FAILED && m.streak == 0 && m.owner_lost);
}

static void test_stale_cid(void)
{
	struct gm m;

	/* owner not ready at respawn: kept, then released once it is */
	reset(&m);
	g_owner_ok = false;
	m.stale_cid = 7;
	m.stale_owner = g_owner_id;
	gm_tick(&m, 0);
	CHECK(m.stale_cid == 7 && g_trace[0] == '\0');
	g_owner_ok = true;
	gm_tick(&m, 2000);
	CHECK(m.phase == PH_STALE_STOP);
	gm_step_done(&m, 2100, 0, "");
	gm_step_done(&m, 2200, 0, "");
	CHECK(strcmp(g_trace,
		     "step:--client-cid=7 --client-no-release-cid --loc-stop --loc-session-id=1|"
		     "step:--client-cid=7 --loc-noop|save:-1|") == 0);
	CHECK(m.state == ST_OFF && m.stale_cid < 0 && m.leaked == 0);

	/* owner replaced since: dropped without qmicli */
	reset(&m);
	m.stale_cid = 7;
	m.stale_owner = (struct gm_owner){ 55, 1 };
	gm_tick(&m, 0);
	CHECK(strcmp(g_trace, "save:-1|") == 0 && m.stale_cid < 0 && m.state == ST_OFF);

	/* leased while a stale CID is pending: it goes first, then a fresh CID */
	reset(&m);
	m.stale_cid = 7;
	m.stale_owner = g_owner_id;
	gm_set_leases(&m, 0, 1);
	gm_tick(&m, 0);
	CHECK(m.phase == PH_STALE_STOP && m.state == ST_STARTING);
	gm_step_done(&m, 100, 0, "");
	gm_step_done(&m, 200, 1, "");	/* release failed: leaked, not retried */
	CHECK(m.leaked == 1 && m.phase == PH_OWNER);
	gm_tick(&m, 300);
	gm_step_done(&m, 400, 0, ALLOC_OUT);
	CHECK(m.cid == 3 && strstr(g_trace, "--client-cid=7 --loc-noop|save:-1|step:--client-no-release-cid --loc-noop|save:3|"));
}

static void test_teardown_failures(void)
{
	struct gm m;
	int64_t t;

	/* loc-stop fails: still released; release fails: leaked */
	reset(&m);
	t = bring_up(&m, 0);
	gm_set_leases(&m, t, 0);
	gm_nmea_line(&m, t + GM_GRACE_MS, "$GPGSA,A,1*00");
	gm_tick(&m, t + GM_GRACE_MS);
	gm_child_exited(&m, t + GM_GRACE_MS, CH_FOLLOW, 0);
	gm_child_exited(&m, t + GM_GRACE_MS, CH_BROKER, 0);
	gm_step_done(&m, t + GM_GRACE_MS + 1, 1, "");
	CHECK(m.phase == PH_RELEASE);
	gm_step_done(&m, t + GM_GRACE_MS + 2, 1, "");
	CHECK(m.leaked == 1 && m.cid == -1 && m.phase == PH_STOP_GPSD);
	gm_child_exited(&m, t + GM_GRACE_MS + 3, CH_GPSD, 0);
	CHECK(m.state == ST_OFF);

	/* bridge gone: no qmicli steps */
	reset(&m);
	t = bring_up(&m, 0);
	g_bridge = false;
	gm_child_exited(&m, t + 10, CH_GPSD, 1);
	g_trace[0] = '\0';
	gm_child_exited(&m, t + 20, CH_FOLLOW, 0);
	gm_child_exited(&m, t + 20, CH_BROKER, 0);
	CHECK(strcmp(g_trace, "save:-1|") == 0 && m.state == ST_FAILED && m.leaked == 0);

	/* step timeout: TERM at 25 s, KILL 3 s later, then treated as failed */
	reset(&m);
	gm_set_leases(&m, 0, 1);
	gm_tick(&m, 0);
	g_trace[0] = '\0';
	gm_tick(&m, GM_STEP_TIMEOUT_MS - 1);
	CHECK(g_trace[0] == '\0');
	gm_tick(&m, GM_STEP_TIMEOUT_MS);
	CHECK(strcmp(g_trace, "sig:step:15|") == 0);
	gm_tick(&m, GM_STEP_TIMEOUT_MS + 1000);
	CHECK(strcmp(g_trace, "sig:step:15|") == 0);
	gm_tick(&m, GM_STEP_TIMEOUT_MS + GM_TERM_TO_KILL_MS);
	CHECK(strcmp(g_trace, "sig:step:15|sig:step:9|") == 0);
	gm_step_done(&m, GM_STEP_TIMEOUT_MS + GM_TERM_TO_KILL_MS + 10, 137, "");
	CHECK(m.state == ST_FAILED && held_nothing(&m) && strstr(m.detail, "allocation failed"));

	/* follower ignores INT: TERM at 5 s, KILL at 8 s; gpsd KILL at 5 s */
	reset(&m);
	t = bring_up(&m, 0);
	g_trace[0] = '\0';
	gm_child_exited(&m, t, CH_GPSD, 1);
	gm_tick(&m, t + GM_INT_TO_TERM_MS);
	gm_tick(&m, t + GM_INT_TO_TERM_MS + GM_TERM_TO_KILL_MS);
	CHECK(strcmp(g_trace, "sig:follower:2|sig:follower:15|sig:broker:15|sig:follower:9|sig:broker:9|") == 0);
	reset(&m);
	t = bring_up(&m, 0);
	gm_set_leases(&m, t, 0);
	gm_nmea_line(&m, t + GM_GRACE_MS, "$GPGSA,A,1*00");
	gm_tick(&m, t + GM_GRACE_MS);
	gm_child_exited(&m, t + GM_GRACE_MS, CH_FOLLOW, 0);
	gm_child_exited(&m, t + GM_GRACE_MS, CH_BROKER, 0);
	gm_step_done(&m, t + GM_GRACE_MS, 0, "");
	gm_step_done(&m, t + GM_GRACE_MS, 0, "");
	g_trace[0] = '\0';
	gm_tick(&m, t + GM_GRACE_MS + GM_GPSD_KILL_MS);
	CHECK(strcmp(g_trace, "sig:gpsd:9|") == 0);

	/* pipeline cannot start: one teardown with what exists */
	reset(&m);
	g_pipe_rc = -ENOENT;
	t = bring_up(&m, 0);
	CHECK(m.state == ST_STOPPING && m.alive[CH_GPSD] && !m.alive[CH_FOLLOW]);
	CHECK(strstr(g_trace, "--loc-stop") != NULL);
}

static void test_review_fixes(void)
{
	struct gm m;
	int64_t t;
	int i;

	/* loc-start fails after maybe starting session 1: loc-stop then release */
	reset(&m);
	gm_set_leases(&m, 0, 1);
	gm_tick(&m, 0);
	gm_step_done(&m, 100, 0, ALLOC_OUT);
	gm_step_done(&m, 200, 0, "");
	g_trace[0] = '\0';
	gm_step_done(&m, 300, 1, "error: timed out");
	CHECK(strcmp(g_trace, "step:--client-cid=3 --client-no-release-cid --loc-stop --loc-session-id=1|") == 0);
	gm_step_done(&m, 400, 0, "");
	gm_step_done(&m, 500, 0, "");
	CHECK(strstr(g_trace, "step:--client-cid=3 --loc-noop|save:-1|") != NULL);
	CHECK(m.state == ST_FAILED && held_nothing(&m) && strstr(m.detail, "LOC start failed"));

	/* ... and the same when loc-start hits its timeout */
	reset(&m);
	gm_set_leases(&m, 0, 1);
	gm_tick(&m, 0);
	gm_step_done(&m, 100, 0, ALLOC_OUT);
	gm_step_done(&m, 200, 0, "");
	gm_tick(&m, 200 + GM_STEP_TIMEOUT_MS);
	g_trace[0] = '\0';
	gm_step_done(&m, 300 + GM_STEP_TIMEOUT_MS, 143, "");
	CHECK(strstr(g_trace, "--loc-stop") != NULL && m.leaked == 0);

	/* a timed-out allocation may have left a CID in the bridge: counted */
	reset(&m);
	gm_set_leases(&m, 0, 1);
	gm_tick(&m, 0);
	gm_tick(&m, GM_STEP_TIMEOUT_MS);
	CHECK(m.leaked == 1);
	gm_step_done(&m, GM_STEP_TIMEOUT_MS + 10, 143, "");
	CHECK(m.state == ST_FAILED && m.leaked == 1 && held_nothing(&m));

	/* leased during an unleased stale cleanup: a real bring-up follows */
	reset(&m);
	m.stale_cid = 7;
	m.stale_owner = g_owner_id;
	gm_tick(&m, 0);
	CHECK(m.phase == PH_STALE_STOP && m.state == ST_OFF);
	gm_set_leases(&m, 10, 1);
	gm_step_done(&m, 100, 0, "");
	g_trace[0] = '\0';
	gm_step_done(&m, 200, 0, "");
	CHECK(strcmp(g_trace, "save:-1|rotate|") == 0);
	CHECK(m.bringups == 1 && m.phase == PH_OWNER && m.state == ST_STARTING);
	/* stale cleanup inside a bring-up: still one bring-up */
	reset(&m);
	m.stale_cid = 7;
	m.stale_owner = g_owner_id;
	gm_set_leases(&m, 0, 1);
	gm_tick(&m, 0);
	gm_step_done(&m, 100, 0, "");
	gm_step_done(&m, 200, 0, "");
	gm_tick(&m, 300);
	CHECK(m.bringups == 1 && m.phase == PH_ALLOC);

	/* hint flapping reaches watchers, the log only once per bring-up */
	reset(&m);
	t = bring_up(&m, 0);
	CHECK(g_hint_logs == 1);	/* the first ACQUIRING */
	for (i = 0; i < 10; i++) {
		gm_nmea_line(&m, t, "$GPRMC,1,A,");
		gm_tick(&m, t);
		t += GM_FIX_HOLD_MS + 1000;
		gm_nmea_line(&m, t, "$GPGSA,A,1*00");
		gm_tick(&m, t);
	}
	CHECK(g_hint_logs == 2);	/* plus the first RUNNING */
	CHECK(g_changes >= 20);
}

static void test_parsers(void)
{
	struct gm m;

	CHECK(gm_parse_cid(ALLOC_OUT) == 3);
	CHECK(gm_parse_cid("CID: '255'") == 255);
	CHECK(gm_parse_cid("CID: '0'") == -1);
	CHECK(gm_parse_cid("CID: '300'") == -1);
	CHECK(gm_parse_cid("error: couldn't allocate") == -1);
	reset(&m);
	gm_nmea_line(&m, 5, "not nmea");
	CHECK(m.nmea_lines == 0);
	gm_nmea_line(&m, 5, "$GNRMC,1,A,");
	CHECK(m.last_fix == 5);
	gm_nmea_line(&m, 6, "$GPRMC,,V,,");
	CHECK(m.last_fix == 5 && m.nmea_lines == 2);
}

/* ---- server over socketpairs ---- */

#define HELLO "{\"type\":\"hello\",\"daemon\":\"gps-manager\",\"proto\":1}\n"
#define OK_WATCH "{\"type\":\"ok\",\"cmd\":\"watch\"}\n"
#define SWITCH_ON "{\"type\":\"switch\",\"enabled\":true}\n"
#define SWITCH_OFF "{\"type\":\"switch\",\"enabled\":false}\n"
#define STATE_OFF_IDLE "{\"type\":\"state\",\"state\":\"OFF\",\"detail\":\"idle\",\"up_ms\":null,\"fix_ms\":null}\n"

static int64_t g_now;
static int64_t t_now(void)
{
	return g_now;
}

static char g_srv_log[256];

static void t_srv_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(g_srv_log, sizeof(g_srv_log), fmt, ap);
	va_end(ap);
}

struct peer {
	struct linesrv_client *c;
	int fd;		/* our end */
};

static void recv_all(int fd, char *buf, size_t len)
{
	ssize_t n = recv(fd, buf, len - 1, MSG_DONTWAIT);

	buf[n > 0 ? n : 0] = '\0';
}

/* One poll round of the server's sockets. */
static void pump(struct gm_server *s)
{
	struct pollfd pfd[1 + GM_MAX_CLIENTS];
	int n = linesrv_pollfds(s->srv, pfd, 1 + GM_MAX_CLIENTS);

	if (poll(pfd, (nfds_t)n, 0) > 0)
		linesrv_handle(s->srv, pfd, n);
}

/* A connected client; the hello is the first thing it reads. */
static struct peer connect_peer(struct gm_server *s)
{
	struct peer p;
	char buf[128];
	int sv[2];

	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	p.fd = sv[0];
	p.c = linesrv_adopt(s->srv, sv[1]);
	CHECK(p.c != NULL);
	recv_all(p.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, HELLO) == 0);
	return p;
}

static void say(struct gm_server *s, struct peer *p, const char *text)
{
	CHECK(send(p->fd, text, strlen(text), 0) == (ssize_t)strlen(text));
	pump(s);
}

/* Send TEXT and expect exactly REPLY back. */
static void ask(struct gm_server *s, struct peer *p, const char *text, const char *reply)
{
	char buf[2048];

	say(s, p, text);
	recv_all(p->fd, buf, sizeof(buf));
	CHECK(strcmp(buf, reply) == 0);
	if (strcmp(buf, reply))
		fprintf(stderr, "  %s-> %s", text, buf);
}

/* What a watcher has been sent so far, expected to be exactly WANT. */
static void expect(struct peer *p, const char *want)
{
	char buf[2048];

	recv_all(p->fd, buf, sizeof(buf));
	CHECK(strcmp(buf, want) == 0);
	if (strcmp(buf, want))
		fprintf(stderr, "  got %s  want %s", buf, want);
}

/* The last whole line in BUF. */
static const char *last_line(const char *buf)
{
	size_t n = strlen(buf);

	if (n && buf[n - 1] == '\n')
		n--;
	while (n && buf[n - 1] != '\n')
		n--;
	return buf + n;
}

static struct gm_server *g_srv;

/* the shell's changed op */
static void f_changed_broadcast(void *ctx, const struct gm *m)
{
	f_changed(ctx, m);
	gm_server_broadcast(g_srv);
}

static void test_server(void)
{
	struct gm m;
	struct gm_server s;
	struct peer a, b, w;
	char buf[2048];
	int i;

	reset(&m);
	g_now = 1000;
	CHECK(gm_server_init(&s, &m, t_now, t_srv_log) == 0);
	a = connect_peer(&s);
	b = connect_peer(&s);

	ask(&s, &a, "lease map\nlease map\nlease ride\n",
	    "{\"type\":\"ok\",\"cmd\":\"lease\",\"kind\":\"map\",\"held\":1}\n"
	    "{\"type\":\"ok\",\"cmd\":\"lease\",\"kind\":\"map\",\"held\":2}\n"
	    "{\"type\":\"ok\",\"cmd\":\"lease\",\"kind\":\"ride\",\"held\":1}\n");
	CHECK(m.leases == 3 && m.state == ST_STARTING);
	ask(&s, &b, "lease bogus\n",
	    "{\"type\":\"error\",\"cmd\":\"lease\",\"reason\":\"unknown lease kind\",\"kind\":\"bogus\"}\n");
	ask(&s, &b, "release bogus\n",
	    "{\"type\":\"error\",\"cmd\":\"release\",\"reason\":\"unknown lease kind\",\"kind\":\"bogus\"}\n");
	ask(&s, &b, "release ride\n", "{\"type\":\"error\",\"cmd\":\"release\",\"reason\":\"no lease\",\"kind\":\"ride\"}\n");
	ask(&s, &b, "retry\n", "{\"type\":\"error\",\"cmd\":\"retry\",\"reason\":\"not failed\"}\n");
	ask(&s, &b, "frobnicate x\n", "{\"type\":\"error\",\"cmd\":\"frobnicate\",\"reason\":\"unknown command\"}\n");
	/* missing or extra words: nothing leased */
	ask(&s, &b, "lease\n", "{\"type\":\"error\",\"cmd\":\"lease\",\"reason\":\"bad arguments\"}\n");
	ask(&s, &b, "lease map ride\n", "{\"type\":\"error\",\"cmd\":\"lease\",\"reason\":\"bad arguments\"}\n");
	ask(&s, &b, "release\n", "{\"type\":\"error\",\"cmd\":\"release\",\"reason\":\"bad arguments\"}\n");
	ask(&s, &b, "lease 1 2 3 4 5 6 7 8 9\n", "{\"type\":\"error\",\"cmd\":\"lease\",\"reason\":\"bad arguments\"}\n");
	ask(&s, &b, "watch now\n", "{\"type\":\"error\",\"cmd\":\"watch\",\"reason\":\"bad arguments\"}\n");
	ask(&s, &b, "status all\n", "{\"type\":\"error\",\"cmd\":\"status\",\"reason\":\"bad arguments\"}\n");
	ask(&s, &b, "retry 1\n", "{\"type\":\"error\",\"cmd\":\"retry\",\"reason\":\"bad arguments\"}\n");
	ask(&s, &b, "enable 1\n", "{\"type\":\"error\",\"cmd\":\"enable\",\"reason\":\"bad arguments\"}\n");
	ask(&s, &b, "disable now\n", "{\"type\":\"error\",\"cmd\":\"disable\",\"reason\":\"bad arguments\"}\n");
	CHECK(m.leases == 3 && m.enabled && strstr(g_trace, "persist") == NULL);
	/* echoed words are escaped and cut at 32 bytes */
	ask(&s, &b, "fr\"ob\\x\x01\n",
	    "{\"type\":\"error\",\"cmd\":\"fr\\\"ob\\\\x\\u0001\",\"reason\":\"unknown command\"}\n");
	ask(&s, &b, "lease 0123456789abcdef0123456789abcdefXYZ\n",
	    "{\"type\":\"error\",\"cmd\":\"lease\",\"reason\":\"unknown lease kind\","
	    "\"kind\":\"0123456789abcdef0123456789abcdef\"}\n");
	say(&s, &b, "\n \t \r\n");	/* empty and blank lines: ignored */
	recv_all(b.fd, buf, sizeof(buf));
	CHECK(buf[0] == '\0');
	ask(&s, &b, "lease ride\r\n", "{\"type\":\"ok\",\"cmd\":\"lease\",\"kind\":\"ride\",\"held\":1}\n");
	CHECK(m.leases == 4);
	g_now = 1500;
	ask(&s, &b, "status\n",
	    "{\"type\":\"status\",\"state\":\"STARTING\",\"enabled\":true,\"leases\":{\"map\":2,\"ride\":2},"
	    "\"clients\":2,\"watchers\":0,\"cid\":null,\"phase\":\"owner\",\"nmea\":0,\"bringups\":1,"
	    "\"failures\":0,\"streak\":0,\"leaked\":0,\"grace\":false,\"up_ms\":500,\"fix_ms\":null,"
	    "\"detail\":\"waiting for the modem owner\"}\n");

	/* connection death releases its leases */
	close(a.fd);
	pump(&s);
	CHECK(linesrv_fd(a.c) == -1 && m.leases == 1 && m.grace_at == 0);
	ask(&s, &b, "release ride\n", "{\"type\":\"ok\",\"cmd\":\"release\",\"kind\":\"ride\",\"held\":0}\n");
	CHECK(m.leases == 0 && m.grace_at > 0);	/* STARTING: grace applies */

	/* watch: the switch and the state at once, then every change */
	w = connect_peer(&s);
	ask(&s, &w, "watch\n", OK_WATCH SWITCH_ON
	    "{\"type\":\"state\",\"state\":\"STARTING\",\"detail\":\"waiting for the modem owner\",\"up_ms\":500,\"fix_ms\":null}\n");
	gm_set(&m, ST_FAILED, "x");
	m.first_fix_at = m.start_at + 200;
	g_now = 2000;
	gm_server_broadcast(&s);
	expect(&w, "{\"type\":\"state\",\"state\":\"FAILED\",\"detail\":\"x\",\"up_ms\":1000,\"fix_ms\":200}\n");
	gm_set(&m, ST_FAILED, "a \"q\" \\ b\tc");	/* the detail is JSON-escaped */
	gm_server_broadcast(&s);
	expect(&w, "{\"type\":\"state\",\"state\":\"FAILED\",\"detail\":\"a \\\"q\\\" \\\\ b\\u0009c\",\"up_ms\":1000,\"fix_ms\":200}\n");
	ask(&s, &b, "status\n",
	    "{\"type\":\"status\",\"state\":\"FAILED\",\"enabled\":true,\"leases\":{\"map\":0,\"ride\":0},"
	    "\"clients\":2,\"watchers\":1,\"cid\":null,\"phase\":\"owner\",\"nmea\":0,\"bringups\":1,"
	    "\"failures\":0,\"streak\":0,\"leaked\":0,\"grace\":true,\"up_ms\":1000,\"fix_ms\":200,"
	    "\"detail\":\"a \\\"q\\\" \\\\ b\\u0009c\"}\n");
	m.cid = 7;
	say(&s, &b, "status\n");
	recv_all(b.fd, buf, sizeof(buf));
	CHECK(strstr(buf, ",\"cid\":7,") != NULL);
	m.cid = -1;

	/* an overlong line: one error, its tail discarded */
	memset(buf, 'x', 300);
	buf[300] = '\0';
	ask(&s, &b, buf, "{\"type\":\"error\",\"reason\":\"line too long\"}\n");
	pump(&s);
	pump(&s);
	say(&s, &b, "yyy\nstatus\n");
	recv_all(b.fd, buf, sizeof(buf));
	CHECK(strncmp(buf, "{\"type\":\"status\",", 17) == 0 && strchr(buf, '\n') == buf + strlen(buf) - 1);
	CHECK(linesrv_fd(b.c) >= 0);

	/* the client table is bounded: the 17th gets an error and is closed */
	{
		struct peer ps[GM_MAX_CLIENTS];
		int sv[2], n = 0;

		for (i = 0; i < GM_MAX_CLIENTS - 2; i++)
			ps[n++] = connect_peer(&s);
		CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
		CHECK(linesrv_adopt(s.srv, sv[1]) == NULL);
		recv_all(sv[0], buf, sizeof(buf));
		CHECK(strcmp(buf, "{\"type\":\"error\",\"reason\":\"too many clients\"}\n") == 0);
		CHECK(recv(sv[0], buf, 1, 0) == 0);	/* then closed */
		CHECK(strcmp(g_srv_log, "client limit (16) reached, refusing") == 0);
		close(sv[0]);
		for (i = 0; i < n; i++) {
			close(ps[i].fd);
			linesrv_drop(ps[i].c);
		}
	}
	close(b.fd);
	close(w.fd);
	gm_server_fini(&s);
}

/* A watcher that never reads is dropped once its queue is full, not
 * waited for; its leases go with it, and the count is synced after the
 * broadcast loop, so the other watchers see the lines in order. */
static void test_server_slow_watcher(void)
{
	struct gm m;
	struct gm_server s;
	struct peer w, b;
	char buf[8192], got[256] = "";
	int i, sz = 1;

	reset(&m);
	m.ops.changed = f_changed_broadcast;
	g_srv = &s;
	g_now = 1000;
	CHECK(gm_server_init(&s, &m, t_now, t_srv_log) == 0);
	w = connect_peer(&s);	/* the slot before b's */
	b = connect_peer(&s);
	ask(&s, &w, "lease map\n", "{\"type\":\"ok\",\"cmd\":\"lease\",\"kind\":\"map\",\"held\":1}\n");
	say(&s, &w, "watch\n");
	ask(&s, &b, "watch\n", OK_WATCH SWITCH_ON
	    "{\"type\":\"state\",\"state\":\"STARTING\",\"detail\":\"waiting for the modem owner\",\"up_ms\":0,\"fix_ms\":null}\n");
	gm_fail(&m, g_now, true, "no owner");	/* FAILED waiting for an owner, held by w alone */
	CHECK(m.state == ST_FAILED && m.phase == PH_IDLE && m.leases == 1);
	recv_all(b.fd, buf, sizeof(buf));
	setsockopt(linesrv_fd(w.c), SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
	for (i = 0; i < 100000 && linesrv_fd(w.c) >= 0; i++) {
		gm_set(&m, ST_FAILED, "flood %d", i);
		recv_all(b.fd, buf, sizeof(buf));
		if (buf[0])
			snprintf(got, sizeof(got), "%.200s", last_line(buf));
	}
	CHECK(linesrv_fd(w.c) == -1);
	CHECK(i > GM_OUTQ / 120);	/* the queue took its share first */
	CHECK(strcmp(g_srv_log, "client output full, disconnecting it") == 0);
	/* its lease went: OFF, sent after the flood line that dropped it */
	CHECK(m.leases == 0 && m.state == ST_OFF);
	CHECK(strcmp(got, STATE_OFF_IDLE) == 0);
	if (strcmp(got, STATE_OFF_IDLE))
		fprintf(stderr, "  last line: %s", got);
	{
		char flood[160];

		snprintf(flood, sizeof(flood), "\"detail\":\"flood %d\"", i - 1);
		CHECK(strstr(buf, flood) != NULL && strstr(buf, flood) < strstr(buf, "\"state\":\"OFF\""));
	}
	ask(&s, &b, "status\n",
	    "{\"type\":\"status\",\"state\":\"OFF\",\"enabled\":true,\"leases\":{\"map\":0,\"ride\":0},"
	    "\"clients\":1,\"watchers\":1,\"cid\":null,\"phase\":\"idle\",\"nmea\":0,\"bringups\":1,"
	    "\"failures\":1,\"streak\":0,\"leaked\":0,\"grace\":false,\"up_ms\":null,\"fix_ms\":null,"
	    "\"detail\":\"idle\"}\n");
	close(w.fd);
	close(b.fd);
	gm_server_fini(&s);
	g_srv = NULL;
}

/* ---- the GPS switch ---- */

static void test_switch(void)
{
	struct gm m;
	struct gm_server s;
	struct peer a, w;
	char buf[2048];
	int64_t t;

	reset(&m);
	g_now = 1000;
	CHECK(gm_server_init(&s, &m, t_now, t_srv_log) == 0);
	a = connect_peer(&s);
	w = connect_peer(&s);
	ask(&s, &w, "watch\n", OK_WATCH SWITCH_ON STATE_OFF_IDLE);

	/* disabled with nothing leased: DISABLED at once, saved, watchers told */
	ask(&s, &a, "disable\n", "{\"type\":\"ok\",\"cmd\":\"disable\",\"enabled\":false}\n");
	CHECK(m.state == ST_DISABLED && !m.enabled && strstr(g_trace, "persist:0|"));
	gm_server_broadcast(&s);	/* the shell's changed op does this */
	expect(&w, SWITCH_OFF "{\"type\":\"state\",\"state\":\"DISABLED\",\"detail\":\"disabled\",\"up_ms\":null,\"fix_ms\":null}\n");
	/* a new lease is refused; status says so */
	ask(&s, &a, "lease map\n", "{\"type\":\"error\",\"cmd\":\"lease\",\"reason\":\"disabled\"}\n");
	say(&s, &a, "status\n");
	recv_all(a.fd, buf, sizeof(buf));
	CHECK(strstr(buf, "\"state\":\"DISABLED\",\"enabled\":false,\"leases\":{\"map\":0,") != NULL);
	CHECK(m.leases == 0 && m.phase == PH_IDLE);
	/* enabled again: OFF, nothing starts without a lease */
	g_trace[0] = '\0';
	ask(&s, &a, "enable\n", "{\"type\":\"ok\",\"cmd\":\"enable\",\"enabled\":true}\n");
	CHECK(m.state == ST_OFF && strcmp(g_trace, "persist:1|") == 0);
	gm_server_broadcast(&s);
	expect(&w, SWITCH_ON STATE_OFF_IDLE);

	/* leased and up, then disabled: torn down at once (no grace), the
	 * lease held but parked; enabled again it comes back */
	ask(&s, &a, "lease map\n", "{\"type\":\"ok\",\"cmd\":\"lease\",\"kind\":\"map\",\"held\":1}\n");
	CHECK(m.state == ST_STARTING);
	gm_tick(&m, g_now);
	gm_step_done(&m, g_now + 100, 0, ALLOC_OUT);
	gm_step_done(&m, g_now + 200, 0, "");
	gm_step_done(&m, g_now + 300, 0, "");
	CHECK(m.phase == PH_UP);
	g_trace[0] = '\0';
	ask(&s, &a, "disable\n", "{\"type\":\"ok\",\"cmd\":\"disable\",\"enabled\":false}\n");
	CHECK(m.state == ST_STOPPING && m.grace_at == 0 && strstr(g_trace, "sig:follower:2|"));
	finish_teardown(&m, g_now + 400);
	CHECK(m.state == ST_DISABLED && held_nothing(&m) && m.leases == 0);
	say(&s, &a, "status\n");
	recv_all(a.fd, buf, sizeof(buf));
	CHECK(strstr(buf, "\"state\":\"DISABLED\",\"enabled\":false,\"leases\":{\"map\":1,") != NULL);	/* held */
	g_now += 60000;
	gm_tick(&m, g_now);
	CHECK(m.state == ST_DISABLED && m.phase == PH_IDLE);	/* nothing retries */
	/* released while disabled, then enabled: stays OFF */
	say(&s, &a, "release map\nenable\n");
	recv_all(a.fd, buf, sizeof(buf));
	CHECK(m.state == ST_OFF && m.leases == 0);
	/* held across disable/enable: bring-up again */
	say(&s, &a, "lease ride\ndisable\nenable\n");
	recv_all(a.fd, buf, sizeof(buf));
	gm_tick(&m, g_now);
	finish_teardown(&m, g_now);	/* the STARTING bring-up had nothing up */
	CHECK(m.leases == 1 && (m.state == ST_STARTING || m.phase == PH_OWNER || m.phase == PH_ALLOC));
	close(a.fd);
	close(w.fd);
	gm_server_fini(&s);

	/* disabled while FAILED (parked): DISABLED, streak reset */
	reset(&m);
	g_now = 0;
	CHECK(gm_server_init(&s, &m, t_now, t_srv_log) == 0);
	a = connect_peer(&s);
	t = bring_up(&m, 0);
	m.leases = 1;
	s.clients[linesrv_slot(a.c)].leases[LK_MAP] = 1;
	m.streak = 3;
	gm_fail(&m, t, false, "test");
	finish_teardown(&m, t + 100);
	CHECK(m.state == ST_FAILED);
	gm_server_set_enabled(&s, false, true);
	CHECK(m.state == ST_DISABLED && m.streak == 0 && m.retry_at == 0);
	gm_tick(&m, t + 60000);
	CHECK(m.state == ST_DISABLED && m.phase == PH_IDLE);
	gm_server_set_enabled(&s, true, true);
	CHECK(m.state == ST_STARTING);	/* the held lease */
	close(a.fd);
	gm_server_fini(&s);

	/* the saved switch not read yet: leases held, nothing starts; read as
	 * off: DISABLED without saving it again; read as on: up */
	reset(&m);
	g_now = 0;
	CHECK(gm_server_init(&s, &m, t_now, t_srv_log) == 0);
	a = connect_peer(&s);
	m.loaded = false;
	g_trace[0] = '\0';
	ask(&s, &a, "lease map\n", "{\"type\":\"ok\",\"cmd\":\"lease\",\"kind\":\"map\",\"held\":1}\n");
	CHECK(m.leases == 0 && m.state == ST_OFF);
	gm_tick(&m, 10000);
	CHECK(m.phase == PH_IDLE && g_trace[0] == '\0');
	gm_server_set_enabled(&s, false, false);
	CHECK(m.state == ST_DISABLED && m.loaded && g_trace[0] == '\0');	/* not persisted */
	close(a.fd);
	gm_server_fini(&s);
	reset(&m);
	CHECK(gm_server_init(&s, &m, t_now, t_srv_log) == 0);
	a = connect_peer(&s);
	m.loaded = false;
	ask(&s, &a, "lease map\n", "{\"type\":\"ok\",\"cmd\":\"lease\",\"kind\":\"map\",\"held\":1}\n");
	gm_server_set_enabled(&s, true, false);
	CHECK(m.state == ST_STARTING && m.leases == 1 && !strstr(g_trace, "persist"));
	/* shutdown: every client closed, no lease synced into the core */
	gm_server_fini(&s);
	CHECK(s.srv == NULL && m.leases == 1);
	CHECK(recv(a.fd, buf, 1, 0) == 0);
	gm_server_broadcast(&s);	/* the teardown's state changes go nowhere */
	/* the saved switch read back during the shutdown loops (gps-load
	 * still pending at SIGTERM): no connection counts any more */
	m.leases = 0;
	gm_server_set_enabled(&s, true, false);
	CHECK(m.enabled && m.leases == 0);
	gm_server_set_enabled(&s, false, false);
	CHECK(!m.enabled && m.leases == 0);
	gm_server_set_enabled(&s, true, false);
	CHECK(m.enabled && m.leases == 0);
	close(a.fd);
}

/* ---- state file and /proc ---- */

static void write_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");

	if (!f) {
		perror(path);
		return;
	}
	fputs(text, f);
	fclose(f);
}

static void test_record_and_proc(void)
{
	char dir[] = "/tmp/gps-manager-test.XXXXXX", p[256], buf[256];
	struct gm_record r, r2;
	struct gm_paths paths;
	struct gm_owner o;
	struct sockaddr_un sa;
	int fd;

	memset(&r, 0, sizeof(r));
	r.cid = 9;
	r.owner = (struct gm_owner){ 812, 1234 };
	r.pid[CH_GPSD] = 900;
	r.start[CH_GPSD] = 4444;
	r.pid[CH_FOLLOW] = 901;
	r.start[CH_FOLLOW] = 4445;
	CHECK(gm_record_format(&r, buf, sizeof(buf)) == 0);
	CHECK(strcmp(buf, "cid 9\nowner 812 1234\nfollower 901 4445\ngpsd 900 4444\n") == 0);
	gm_record_parse(&r2, buf);
	CHECK(memcmp(&r, &r2, sizeof(r)) == 0);
	gm_record_parse(&r2, "cid -1\nbogus 3 4\nbroker 1 5\n\ngpsd x y\n");
	CHECK(r2.cid == -1 && r2.pid[CH_BROKER] == 0 && r2.pid[CH_GPSD] == 0);

	CHECK(mkdtemp(dir) != NULL);
	snprintf(p, sizeof(p), "%s/proc", dir);
	mkdir(p, 0700);
	snprintf(p, sizeof(p), "%s/proc/900", dir);
	mkdir(p, 0700);
	snprintf(p, sizeof(p), "%s/proc/900/stat", dir);
	/* field 22 = 4444; a comm with spaces and parens */
	write_file(p, "900 (gpsd) S 1 900 900 0 -1 4194560 1 0 0 0 0 0 0 0 20 0 1 0 4444 1000 100\n");
	snprintf(p, sizeof(p), "%s/proc", dir);
	CHECK(gm_record_match(p, &r, CH_GPSD));
	r.start[CH_GPSD] = 4443;	/* pid reused */
	CHECK(!gm_record_match(p, &r, CH_GPSD));
	r.start[CH_GPSD] = 4444;
	snprintf(buf, sizeof(buf), "%s/proc/900/stat", dir);
	write_file(buf, "900 (sh) S 1 900 900 0 -1 4194560 1 0 0 0 0 0 0 0 20 0 1 0 4444 1000 100\n");
	CHECK(!gm_record_match(p, &r, CH_GPSD));	/* not gpsd any more */
	write_file(buf, "900 (gpsd) Z 1 900 900 0 -1 4194560 1 0 0 0 0 0 0 0 20 0 1 0 4444 1000 100\n");
	CHECK(!gm_record_match(p, &r, CH_GPSD));	/* zombie */
	CHECK(!gm_record_match(p, &r, CH_FOLLOW));	/* no such pid */
	{
		unsigned long long st;
		char comm[32];

		write_file(buf, "900 (a) b) c) R 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 777 1\n");
		CHECK(gm_proc_start(p, 900, &st, NULL, comm, sizeof(comm)) == 0 && st == 777 && strcmp(comm, "a) b) c") == 0);
	}

	/* owner check against a fake lock dir, /proc and a real socket */
	paths.proc = p;
	snprintf(buf, sizeof(buf), "%s/lock", dir);
	mkdir(buf, 0700);
	paths.owner_lock = strdup(buf);
	snprintf(buf, sizeof(buf), "%s/qmux", dir);
	paths.qmux_sock = strdup(buf);
	snprintf(buf, sizeof(buf), "%s/proc/812", dir);
	mkdir(buf, 0700);
	snprintf(buf, sizeof(buf), "%s/proc/812/stat", dir);
	write_file(buf, "812 (gps-up) S 1 812 812 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 1234 1 1\n");
	snprintf(buf, sizeof(buf), "%s/lock/owner", dir);
	write_file(buf, "812 1234\n");
	CHECK(!gm_owner_check(&paths, &o));	/* no ready marker */
	snprintf(buf, sizeof(buf), "%s/lock/ready", dir);
	write_file(buf, "");
	CHECK(!gm_owner_check(&paths, &o) && !gm_bridge_present(&paths));	/* no socket */
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", paths.qmux_sock);
	CHECK(bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
	CHECK(gm_owner_check(&paths, &o) && o.pid == 812 && o.start == 1234);
	snprintf(buf, sizeof(buf), "%s/lock/owner", dir);
	write_file(buf, "812 1235\n");	/* a different gps-up with the same pid */
	CHECK(!gm_owner_check(&paths, &o));
	write_file(buf, "1 1234\n");
	CHECK(!gm_owner_check(&paths, &o));
	close(fd);
	free((char *)paths.owner_lock);
	free((char *)paths.qmux_sock);

	snprintf(buf, sizeof(buf), "rm -rf '%s'", dir);
	if (system(buf) != 0)
		fprintf(stderr, "cleanup of %s failed\n", dir);
}

int main(void)
{
	chef_log_set_socket("/nonexistent/chef-log-socket");	/* never the host's /dev/log */
	test_bringup_teardown_order();
	test_grace_cancel_and_second_lease();
	test_release_mid_starting();
	test_lease_during_stopping();
	test_failure_backoff_park();
	test_release_while_failed();
	test_silence_and_healthy();
	test_owner();
	test_stale_cid();
	test_teardown_failures();
	test_review_fixes();
	test_parsers();
	test_server();
	test_server_slow_watcher();
	test_switch();
	test_record_and_proc();
	printf("test-gps-manager: %d/%d checks passed\n", g_tests - g_failures, g_tests);
	return g_failures ? 1 : 0;
}
