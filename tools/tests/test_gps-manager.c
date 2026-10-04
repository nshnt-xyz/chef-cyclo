/* Host tests for tools/gps-manager.c: the core state machine through fake
 * operations that record every step/spawn/signal/save in a trace (ordered
 * bring-up and teardown, lease counting and grace, leases changing mid
 * STARTING and during STOPPING, failures, backoff, the fast-failure streak
 * and its reset, parking and retry, silence, owner loss/replacement and the
 * 2 s owner poll, stale CIDs from a previous instance, leaked releases, a
 * missing bridge, step timeouts and kill escalation), the line protocol over
 * real socketpairs (per-connection leases, connection death, errors, watch,
 * a watcher that does not read), the state-file record and the /proc and
 * owner checks against a fake tree. gps-manager.c is included with
 * GPSMGR_NO_MAIN; nothing is forked. Build/run: see tools/Makefile.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#define GPSMGR_NO_MAIN
#include "../gps-manager.c"

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

static const struct gm_ops f_ops = {
	.ctx = NULL, .log = f_log, .owner = f_owner, .bridge = f_bridge, .run_step = f_run_step,
	.spawn_gpsd = f_spawn_gpsd, .spawn_pipeline = f_spawn_pipeline, .signal = f_signal,
	.save = f_save, .rotate = f_rotate, .changed = f_changed,
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
	"[/run/qmux_socket] Client ID not released:\n\tService: 'loc'\n\t    CID: '3'\n";

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

	/* fix hint with hysteresis */
	gm_nmea_line(&m, t + 1000, "$GPGGA,120000.00,,,,,0,00,99.9,,,,,,*00");
	gm_tick(&m, t + 1000);
	CHECK(m.state == ST_ACQUIRING);
	gm_nmea_line(&m, t + 2000, "$GPRMC,120001.00,A,1234.5,N,01234.5,E,0.0,,041026,,,A*00");
	gm_tick(&m, t + 2000);
	CHECK(m.state == ST_RUNNING);
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
}

static void test_grace_cancel_and_second_lease(void)
{
	struct gm m;
	int64_t t;

	reset(&m);
	t = bring_up(&m, 0);
	gm_set_leases(&m, t, 2);
	gm_set_leases(&m, t + 10, 1);
	CHECK(m.grace_at == 0 && m.phase == PH_UP);
	gm_set_leases(&m, t + 100, 0);
	CHECK(m.grace_at == t + 100 + GM_GRACE_MS);
	gm_set_leases(&m, t + 20000, 1);
	CHECK(m.grace_at == 0);
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

static int64_t g_now;
static int64_t t_now(void)
{
	return g_now;
}

static void send_line(int fd, const char *s)
{
	if (write(fd, s, strlen(s)) < 0)
		perror("write");
}

static void recv_all(int fd, char *buf, size_t len)
{
	ssize_t n = recv(fd, buf, len - 1, MSG_DONTWAIT);

	buf[n > 0 ? n : 0] = '\0';
}

static void test_server(void)
{
	struct gm m;
	struct gm_server s;
	struct gm_client *a, *b, *w;
	int pa[2], pb[2], pw[2];
	char buf[1024];
	int i;

	reset(&m);
	g_now = 0;
	gm_server_init(&s, &m, t_now);
	socketpair(AF_UNIX, SOCK_STREAM, 0, pa);
	socketpair(AF_UNIX, SOCK_STREAM, 0, pb);
	a = gm_server_add(&s, pa[0]);
	b = gm_server_add(&s, pb[0]);

	send_line(pa[1], "lease map\nlease map\nlease ride\n");
	gm_client_input(&s, a);
	recv_all(pa[1], buf, sizeof(buf));
	CHECK(strcmp(buf, "ok lease map 1\nok lease map 2\nok lease ride 1\n") == 0);
	CHECK(m.leases == 3 && m.state == ST_STARTING);
	send_line(pb[1], "lease bogus\nrelease ride\nretry\nfrobnicate\nlease ride\nstatus\n");
	gm_client_input(&s, b);
	recv_all(pb[1], buf, sizeof(buf));
	CHECK(strstr(buf, "err unknown lease kind bogus\n") && strstr(buf, "err no ride lease on this connection\n"));
	CHECK(strstr(buf, "err retry only applies in FAILED\n") && strstr(buf, "err unknown command frobnicate\n"));
	CHECK(strstr(buf, "ok status state=STARTING leases=map:2,ride:2 clients=2") != NULL);
	CHECK(m.leases == 4);

	/* connection death releases its leases */
	close(pa[1]);
	gm_client_input(&s, a);
	CHECK(a->fd < 0 && m.leases == 1 && m.grace_at == 0);
	send_line(pb[1], "release ride\n");
	gm_client_input(&s, b);
	CHECK(m.leases == 0 && m.grace_at > 0);	/* STARTING: grace applies */

	/* watch: current state, then every change */
	socketpair(AF_UNIX, SOCK_STREAM, 0, pw);
	w = gm_server_add(&s, pw[0]);
	send_line(pw[1], "watch\n");
	gm_client_input(&s, w);
	recv_all(pw[1], buf, sizeof(buf));
	CHECK(strcmp(buf, "ok watch\nstate STARTING waiting for the modem owner\n") == 0);
	gm_set(&m, ST_FAILED, "x");
	gm_server_broadcast(&s);
	recv_all(pw[1], buf, sizeof(buf));
	CHECK(strcmp(buf, "state FAILED x\n") == 0);

	/* a watcher that never reads is dropped, not waited for */
	{
		int sz = 4096;

		setsockopt(pw[0], SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
		setsockopt(pw[1], SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
	}
	for (i = 0; i < 5000 && w->fd >= 0; i++) {
		gm_set(&m, ST_FAILED, "flood %d", i);
		gm_server_broadcast(&s);
	}
	CHECK(w->fd < 0);

	/* overlong line */
	memset(buf, 'x', 300);
	buf[300] = '\0';
	send_line(pb[1], buf);
	gm_client_input(&s, b);
	gm_client_input(&s, b);
	send_line(pb[1], "\nstatus\n");
	gm_client_input(&s, b);
	recv_all(pb[1], buf, sizeof(buf));
	CHECK(strstr(buf, "err line too long\n") && strstr(buf, "ok status") && b->fd >= 0);
	close(pb[1]);
	close(pw[1]);
	gm_client_input(&s, b);
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
	test_record_and_proc();
	printf("test-gps-manager: %d/%d checks passed\n", g_tests - g_failures, g_tests);
	return g_failures ? 1 : 0;
}
