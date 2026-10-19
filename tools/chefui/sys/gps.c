/*
 * gps.c - GPS state for the status bar and hub tile, and the GPS pages:
 * the chefui wrapper of gpsclient.h (the gps-manager and gpsd clients,
 * no LVGL), whose two connections run on the chefui loop through lcwatch.
 *
 * sys_state.gps: gps-manager's state mapped for the UI (DISABLED = the
 * switch is off, OFF/STOPPING = on and idle, FAILED = failed,
 * STARTING/ACQUIRING = searching, RUNNING = fix, none = unavailable), the
 * switch, and gpsd's TPV mode and eph while RUNNING ("3D fix · ±3 m").
 *
 * No lease, except while the GPS page asks (sys_gps_watch): then the
 * watch connection holds one "map" lease and gpsd feeds gps_view (also
 * while searching), released and forgotten when the page lets go. The
 * rules (reconnects, the switch, staleness) are gpsclient.h's. Nothing
 * here logs a position.
 */
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "chefui.h"
#include "gpsclient.h"
#include "gpsview.h"
#include "lcwatch.h"
#include "sys.h"
#include "sysstate.h"

static struct {
	struct gpsclient gc;
	struct lcwatch w[2];	/* the manager, gpsd */
	lv_timer_t *stale;	/* gpsclient_view_deadline(): the satellites go stale */
	int64_t stale_at;
	bool running, watch;
} G;

static enum sys_gps map_state(enum gpsc_state s)
{
	switch (s) {
	case GPSC_OFF:
	case GPSC_STOPPING:
		return SYS_GPS_OFF;
	case GPSC_FAILED:
		return SYS_GPS_FAILED;
	case GPSC_STARTING:
	case GPSC_ACQUIRING:
		return SYS_GPS_SEARCHING;
	case GPSC_RUNNING:
		return SYS_GPS_RUNNING;
	case GPSC_DISABLED:
		return SYS_GPS_DISABLED;
	default:
		return SYS_GPS_UNAVAILABLE;
	}
}

static void changed(struct gpsclient *gc, unsigned what, void *ctx)
{
	(void)ctx;
	if (what & GPSC_CH_STATE) {
		enum sys_gps st = map_state(gc->state);

		if (st != sys_state.gps.state || gc->mode != sys_state.gps.mode || gc->eph != sys_state.gps.eph ||
		    gc->enabled != sys_state.gps.enabled) {
			sys_state.gps.state = st;
			sys_state.gps.mode = gc->mode;
			sys_state.gps.eph = gc->eph;
			sys_state.gps.enabled = gc->enabled;
			sys_changed(SYS_GPS);
		}
	}
	if (what & GPSC_CH_VIEW) {
		gps_view = gc->view;
		gps_view.lease_ms = G.watch ? gc->lease_ms[GPSC_LEASE_MAP] : 0;
		sys_changed(SYS_GPS_VIEW);
	}
}

static void stale_fired(lv_timer_t *t);

/* After every call into the client: the fd watches and both timers. */
static void sync(void *user)
{
	int64_t d = gpsclient_view_deadline(&G.gc);

	(void)user;
	lcwatch_sync_all(G.w, 2);
	if (d == G.stale_at)
		return;
	if (G.stale) {
		lv_timer_delete(G.stale);
		G.stale = NULL;
	}
	G.stale_at = d;
	if (d >= 0) {
		int64_t ms = d - lcwatch_now();

		G.stale = lv_timer_create(stale_fired, ms > 0 ? (uint32_t)ms : 0, NULL);
		lv_timer_set_repeat_count(G.stale, 1);
	}
}

static void stale_fired(lv_timer_t *t)
{
	(void)t;
	G.stale = NULL;		/* one-shot: LVGL deletes it after this call */
	G.stale_at = -1;
	gpsclient_tick(&G.gc, lcwatch_now());
	sync(NULL);
}

static void log_line(const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	chefui_log("%s", buf);
}

int sys_gps_start(const char *sock, int gpsd_port)
{
	struct gpsclient_config cfg = {
		.mgr_path = sock, .gpsd_port = gpsd_port, .changed = changed, .log = log_line,
	};

	sys_gps_stop();
	gpsclient_init(&G.gc, &cfg);
	lcwatch_init(&G.w[0], &G.gc.mgr, NULL, sync, NULL);
	lcwatch_init(&G.w[1], &G.gc.gpsd, NULL, sync, NULL);
	G.stale_at = -1;
	G.running = true;
	G.watch = false;
	sys_state.gps.enabled = -1;
	gps_view = G.gc.view;
	gpsclient_start(&G.gc, lcwatch_now());
	sync(NULL);
	return 0;
}

void sys_gps_stop(void)
{
	if (!G.running)
		return;
	G.running = false;
	G.watch = false;
	gpsclient_stop(&G.gc, lcwatch_now());
	lcwatch_fini(&G.w[0]);
	lcwatch_fini(&G.w[1]);
	if (G.stale)
		lv_timer_delete(G.stale);
	G.stale = NULL;
	G.stale_at = -1;
	gps_view = G.gc.view;
	sys_changed(SYS_GPS_VIEW);
}

void sys_gps_watch(bool watch)
{
	int64_t now = lcwatch_now();

	if (watch == G.watch)
		return;
	G.watch = watch;
	if (!G.running) {
		if (!watch) {
			/* not started: nothing feeds the view, so nothing is kept */
			memset(&gps_view, 0, sizeof(gps_view));
			gps_view.eph = gps_view.alt = gps_view.speed = gps_view.lat = gps_view.lon = NAN;
			gps_view.hdop = gps_view.pdop = NAN;
			sys_changed(SYS_GPS_VIEW);
		}
		return;
	}
	gpsclient_want_lease(&G.gc, GPSC_LEASE_MAP, watch, now);
	gpsclient_want_view(&G.gc, watch, now);
	changed(&G.gc, GPSC_CH_VIEW, NULL);	/* the page's lease time */
	sync(NULL);
}

bool sys_gps_watching(void)
{
	return G.watch;
}

int sys_gps_set_enabled(bool on)
{
	int rc;

	if (!G.running)
		return -ENOTCONN;
	rc = gpsclient_set_enabled(&G.gc, on, lcwatch_now());
	sync(NULL);
	return rc;
}
