/*
 * gpsclient.c - the gps-manager and gpsd clients (gpsclient.h). No LVGL:
 * built into chefui's state sources (gps.c is its wrapper) and the
 * LVGL-free host test. Nothing here logs a position.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "common/json.h"
#include "gpsclient.h"

#define BACKOFF_MIN_MS 1000
#define MGR_BACKOFF_MAX_MS 8000
#define GPSD_BACKOFF_MAX_MS 4000

static const char *const lease_names[GPSC_NLEASE] = { "map", "ride" };

/* ------------------------------------------------------------- parser */

int gpsclient_parse_mgr(const char *line, struct gpsc_mgr_line *out)
{
	static const char *const names[] = {
		[GPSC_OFF] = "OFF", [GPSC_STARTING] = "STARTING", [GPSC_ACQUIRING] = "ACQUIRING",
		[GPSC_RUNNING] = "RUNNING", [GPSC_STOPPING] = "STOPPING", [GPSC_FAILED] = "FAILED",
		[GPSC_DISABLED] = "DISABLED",
	};
	struct json_obj o;
	char type[16], st[16];
	bool on;
	size_t i;

	memset(out, 0, sizeof(*out));
	out->proto = out->up_ms = out->fix_ms = -1;
	if (json_parse(&o, line, strlen(line)) || !json_get_string(&o, "type", type, sizeof(type)))
		return -1;
	json_get_string(&o, "cmd", out->cmd, sizeof(out->cmd));
	json_get_string(&o, "kind", out->kind, sizeof(out->kind));
	if (!strcmp(type, "hello")) {
		out->type = GPSC_MGR_HELLO;
		json_get_long(&o, "proto", &out->proto);
	} else if (!strcmp(type, "ok")) {
		out->type = GPSC_MGR_OK;
	} else if (!strcmp(type, "error")) {
		out->type = GPSC_MGR_ERROR;
		json_get_string(&o, "reason", out->reason, sizeof(out->reason));
	} else if (!strcmp(type, "switch")) {
		if (!json_get_bool(&o, "enabled", &on))
			return -1;
		out->type = GPSC_MGR_SWITCH;
		out->enabled = on;
	} else if (!strcmp(type, "state")) {
		if (!json_get_string(&o, "state", st, sizeof(st)))
			return -1;
		for (i = GPSC_OFF; i < sizeof(names) / sizeof(names[0]) && strcmp(st, names[i]); i++)
			;
		if (i == sizeof(names) / sizeof(names[0]))
			return -1;
		/* null (or absent) when there is no bring-up or no fix yet */
		if (!json_get_long(&o, "up_ms", &out->up_ms) || out->up_ms < 0 || out->up_ms > 1000000000L)
			out->up_ms = -1;
		if (out->up_ms < 0 || !json_get_long(&o, "fix_ms", &out->fix_ms) || out->fix_ms < 0 ||
		    out->fix_ms > 1000000000L)
			out->fix_ms = -1;
		if (!json_get_string(&o, "detail", out->detail, sizeof(out->detail)))
			out->detail[0] = '\0';
		out->type = GPSC_MGR_STATE;
		out->state = (enum gpsc_state)i;
	} else {
		out->type = GPSC_MGR_OTHER;
	}
	return 0;
}

/* ------------------------------------------------------------- state */

static void notify(struct gpsclient *gc, unsigned what)
{
	if (gc->cfg.changed && what)
		gc->cfg.changed(gc, what, gc->cfg.ctx);
}

static void status_set(struct gpsclient *gc, enum gpsc_state st, int mode, double eph, const char *detail)
{
	if (st != gc->state || mode != gc->mode || eph != gc->eph || strcmp(detail, gc->detail)) {
		gc->state = st;
		gc->mode = mode;
		gc->eph = eph;
		snprintf(gc->detail, sizeof(gc->detail), "%s", detail);
		notify(gc, GPSC_CH_STATE);
	}
}

static void enabled_set(struct gpsclient *gc, int on)
{
	if (gc->enabled != on) {
		gc->enabled = on;
		notify(gc, GPSC_CH_STATE);
	}
}

/* Forget the reports (position included); the bring-up's times stay. */
static void view_clear(struct gpsclient *gc)
{
	memset(&gc->view, 0, sizeof(gc->view));
	gc->view.eph = gc->view.alt = gc->view.speed = gc->view.lat = gc->view.lon = NAN;
	gc->view.hdop = gc->view.pdop = NAN;
	gc->view.start_ms = gc->start_ms;
	gc->view.first_fix_ms = gc->first_fix_ms;
	gc->sats_at = 0;
}

static bool view_fed(const struct gpsclient *gc)
{
	return gc->view.sky || gc->view.mode;
}

/* gpsd while RUNNING (the status), and while the view is wanted also
 * while searching. */
static bool gpsd_wanted(const struct gpsclient *gc)
{
	return gc->state == GPSC_RUNNING ||
	       (gc->want_view && (gc->state == GPSC_STARTING || gc->state == GPSC_ACQUIRING));
}

static void gpsd_sync(struct gpsclient *gc, int64_t now)
{
	if (gpsd_wanted(gc)) {
		lineclient_start(&gc->gpsd, now);
		return;
	}
	lineclient_stop(&gc->gpsd, now);
	if (view_fed(gc)) {
		view_clear(gc);	/* no GNSS: nothing to show */
		notify(gc, GPSC_CH_VIEW);
	}
}

/* ---------------------------------------------------------------- gpsd */

static void gpsd_connected(struct lineclient *lc, int64_t now)
{
	lineclient_send(lc, "?WATCH={\"enable\":true,\"json\":true};\n", now);
}

/* A SKY with the satellites array (not gpsd's DOP-only one). */
static bool sky_has_sats(const char *line)
{
	struct json_obj o;
	const struct json_member *m;

	if (json_parse(&o, line, strlen(line)))
		return false;
	m = json_find(&o, "satellites");
	return m && m->type == JSON_ARRAY;
}

static void gpsd_line(struct lineclient *lc, char *line, size_t len, int64_t now)
{
	struct gpsclient *gc = lineclient_ctx(lc);
	int mode;
	double eph;

	(void)len;
	if (gc->want_view && !gps_parse_tpv_view(line, &gc->view)) {
		notify(gc, GPSC_CH_VIEW);
	} else if (gc->want_view && !gps_parse_sky_view(line, &gc->view)) {
		if (sky_has_sats(line))
			gc->sats_at = now;
		notify(gc, GPSC_CH_VIEW);
	}
	if (!gps_parse_tpv_status(line, &mode, &eph)) {
		/* eph rounded to a metre so small wobbles change nothing */
		if (eph >= 0)
			eph = (double)(long)(eph + 0.5);
		status_set(gc, gc->state, mode, eph, gc->detail);
		lineclient_healthy(lc);
	}
}

/* gpsd went (or the caller let it go): show nothing stale */
static void gpsd_lost(struct lineclient *lc, int64_t now)
{
	struct gpsclient *gc = lineclient_ctx(lc);

	(void)now;
	if (view_fed(gc)) {
		view_clear(gc);
		notify(gc, GPSC_CH_VIEW);
	}
}

/* ---------------------------------------------------------- the manager */

static void fifo_push(struct gpsclient *gc, int kind)
{
	if (gc->nfifo < (int)(sizeof(gc->fifo) / sizeof(gc->fifo[0])))
		gc->fifo[gc->nfifo++] = kind;
}

/* The kind of the oldest lease request still unanswered, -1 none. */
static int fifo_pop(struct gpsclient *gc)
{
	int k;

	if (!gc->nfifo)
		return -1;
	k = gc->fifo[0];
	memmove(gc->fifo, gc->fifo + 1, (size_t)--gc->nfifo * sizeof(gc->fifo[0]));
	return k;
}

static bool fifo_has(const struct gpsclient *gc, int kind)
{
	for (int i = 0; i < gc->nfifo; i++)
		if (gc->fifo[i] == kind)
			return true;
	return false;
}

/* The wanted leases on the watch connection: asked once the watch reply
 * and a switch that is not off are in, released when no longer wanted.
 * Requests are answered in order, so a release need not wait for the
 * lease it ends. */
static void lease_sync(struct gpsclient *gc, int64_t now)
{
	char req[32];

	for (int k = 0; k < GPSC_NLEASE; k++) {
		if (!lineclient_up(&gc->mgr) || !gc->watching)
			return;	/* a failed send closed it */
		if (gc->want[k] && !gc->held[k] && gc->enabled != 0) {
			snprintf(req, sizeof(req), "lease %s\n", lease_names[k]);
			if (lineclient_send(&gc->mgr, req, now) == 0) {
				gc->held[k] = true;
				fifo_push(gc, k);
				gc->lease_ms[k] = now;
				notify(gc, GPSC_CH_VIEW);
			}
		} else if (!gc->want[k] && gc->held[k]) {
			snprintf(req, sizeof(req), "release %s\n", lease_names[k]);
			if (lineclient_send(&gc->mgr, req, now) == 0)
				gc->held[k] = false;
		}
	}
}

static void mgr_connected(struct lineclient *lc, int64_t now)
{
	lineclient_send(lc, "watch\n", now);
}

static void mgr_line(struct lineclient *lc, char *line, size_t len, int64_t now)
{
	struct gpsclient *gc = lineclient_ctx(lc);
	struct gpsc_mgr_line l;
	int k;

	(void)len;
	if (gpsclient_parse_mgr(line, &l))
		return;		/* junk, unknown states */
	switch (l.type) {
	case GPSC_MGR_HELLO:
		if (l.proto != GPSC_PROTO && !gc->proto_warned) {
			gc->proto_warned = true;
			if (gc->cfg.log)
				gc->cfg.log("gps-manager: unknown protocol in %s (this client speaks proto %d); carrying on",
					    line, GPSC_PROTO);
		}
		return;
	case GPSC_MGR_OK:
		if (!strcmp(l.cmd, "watch")) {
			/* the leases wait for the switch line that follows */
			lineclient_healthy(lc);
			gc->watching = true;
		} else if (!strcmp(l.cmd, "lease")) {
			fifo_pop(gc);
		}
		return;
	case GPSC_MGR_ERROR:
		if (!strcmp(l.cmd, "lease")) {
			/* refused ("disabled"): none held, unless asked again since */
			k = fifo_pop(gc);
			if (k >= 0 && !fifo_has(gc, k))
				gc->held[k] = false;
		}
		return;
	case GPSC_MGR_SWITCH:
		enabled_set(gc, l.enabled);
		lease_sync(gc, now);	/* on again: lease again if it was refused */
		return;
	case GPSC_MGR_STATE:
		break;
	default:
		return;
	}
	gc->start_ms = l.up_ms >= 0 ? now - l.up_ms : 0;
	gc->first_fix_ms = l.up_ms >= 0 && l.fix_ms >= 0 ? gc->start_ms + l.fix_ms : 0;
	gc->view.start_ms = gc->start_ms;
	gc->view.first_fix_ms = gc->first_fix_ms;
	notify(gc, GPSC_CH_VIEW);
	if (l.state == GPSC_DISABLED)
		enabled_set(gc, 0);
	lease_sync(gc, now);	/* a manager without switch lines */
	if (!lineclient_up(lc))
		return;		/* a send failed: lost */
	if (l.state == GPSC_RUNNING && gc->state == GPSC_RUNNING)
		status_set(gc, l.state, gc->mode, gc->eph, l.detail);
	else
		status_set(gc, l.state, 0, -1, l.detail);
	gpsd_sync(gc, now);
}

/* The connection's leases went with it; nothing about GNSS is known. */
static void mgr_reset(struct gpsclient *gc, int64_t now)
{
	lineclient_stop(&gc->gpsd, now);
	gc->watching = false;
	memset(gc->held, 0, sizeof(gc->held));
	gc->nfifo = 0;
	gc->start_ms = gc->first_fix_ms = 0;
	memset(gc->lease_ms, 0, sizeof(gc->lease_ms));
	view_clear(gc);
	notify(gc, GPSC_CH_VIEW);
}

static void mgr_lost(struct lineclient *lc, int64_t now)
{
	struct gpsclient *gc = lineclient_ctx(lc);

	mgr_reset(gc, now);
	enabled_set(gc, -1);
	status_set(gc, GPSC_NONE, 0, -1, "");
}

/* ----------------------------------------------------------------- API */

void gpsclient_init(struct gpsclient *gc, const struct gpsclient_config *cfg)
{
	struct lineclient_config m = {
		.path = cfg->mgr_path ? cfg->mgr_path : GPSC_MGR_SOCK,
		.backoff_min_ms = BACKOFF_MIN_MS, .backoff_max_ms = MGR_BACKOFF_MAX_MS,
		.on_connected = mgr_connected, .on_line = mgr_line, .on_lost = mgr_lost, .ctx = gc,
	};
	struct lineclient_config g = {
		.port = cfg->gpsd_port > 0 ? cfg->gpsd_port : GPSC_GPSD_PORT,
		.backoff_min_ms = BACKOFF_MIN_MS, .backoff_max_ms = GPSD_BACKOFF_MAX_MS,
		.on_connected = gpsd_connected, .on_line = gpsd_line, .on_lost = gpsd_lost, .ctx = gc,
	};

	memset(gc, 0, sizeof(*gc));
	gc->cfg = *cfg;
	lineclient_init(&gc->mgr, &m);
	lineclient_init(&gc->gpsd, &g);
	gc->state = GPSC_NONE;
	gc->enabled = -1;
	gc->eph = -1;
	view_clear(gc);
}

void gpsclient_start(struct gpsclient *gc, int64_t now)
{
	lineclient_start(&gc->mgr, now);
}

void gpsclient_stop(struct gpsclient *gc, int64_t now)
{
	lineclient_stop(&gc->gpsd, now);
	lineclient_stop(&gc->mgr, now);	/* mgr_lost when it was up */
	mgr_reset(gc, now);
}

void gpsclient_want_lease(struct gpsclient *gc, enum gpsc_lease kind, bool want, int64_t now)
{
	if (kind < 0 || kind >= GPSC_NLEASE || gc->want[kind] == want)
		return;
	gc->want[kind] = want;
	if (!want && gc->lease_ms[kind]) {
		gc->lease_ms[kind] = 0;
		notify(gc, GPSC_CH_VIEW);
	}
	lease_sync(gc, now);
}

void gpsclient_want_view(struct gpsclient *gc, bool want, int64_t now)
{
	if (gc->want_view == want)
		return;
	gc->want_view = want;
	if (!want) {
		view_clear(gc);	/* positions are not kept unwanted */
		notify(gc, GPSC_CH_VIEW);
	}
	gpsd_sync(gc, now);
}

int gpsclient_set_enabled(struct gpsclient *gc, bool on, int64_t now)
{
	if (!lineclient_up(&gc->mgr) || !gc->watching)
		return -ENOTCONN;
	return lineclient_send(&gc->mgr, on ? "enable\n" : "disable\n", now) ? -EIO : 0;
}

void gpsclient_pollfds(const struct gpsclient *gc, struct pollfd pfd[2])
{
	pfd[0] = lineclient_pollfd(&gc->mgr);
	pfd[1] = lineclient_pollfd(&gc->gpsd);
}

void gpsclient_handle(struct gpsclient *gc, const struct pollfd pfd[2], int64_t now)
{
	/* an entry whose connection went in the meantime is skipped */
	if (pfd[0].fd >= 0 && pfd[0].fd == gc->mgr.fd)
		lineclient_handle(&gc->mgr, pfd[0].revents, now);
	if (pfd[1].fd >= 0 && pfd[1].fd == gc->gpsd.fd)
		lineclient_handle(&gc->gpsd, pfd[1].revents, now);
}

int64_t gpsclient_view_deadline(const struct gpsclient *gc)
{
	return gc->want_view && gc->sats_at ? gc->sats_at + GPS_SATS_STALE_MS : -1;
}

int64_t gpsclient_deadline(const struct gpsclient *gc)
{
	int64_t d[3] = { lineclient_deadline(&gc->mgr), lineclient_deadline(&gc->gpsd),
			 gpsclient_view_deadline(gc) }, best = -1;

	for (int i = 0; i < 3; i++)
		if (d[i] >= 0 && (best < 0 || d[i] < best))
			best = d[i];
	return best;
}

void gpsclient_tick(struct gpsclient *gc, int64_t now)
{
	int64_t stale = gpsclient_view_deadline(gc);

	lineclient_tick(&gc->mgr, now);
	lineclient_tick(&gc->gpsd, now);
	/* gpsd's DOP-only SKY lines keep the satellites of the last full one;
	 * without a full one for GPS_SATS_STALE_MS (the receiver lost its
	 * sky, gpsd stalled) the list goes, so no satellite that is no longer
	 * reported is shown */
	if (stale >= 0 && now >= stale) {
		gc->sats_at = 0;
		if (gc->view.n_sats) {
			gc->view.n_sats = 0;
			notify(gc, GPSC_CH_VIEW);
		}
	}
}
