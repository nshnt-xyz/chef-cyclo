/*
 * shutdown.c - the shutdown screen (shutdown.h): the marker watch, the
 * shutsteps wiring and the screen and input rules around the shared cover
 * (cover.h).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "chefui.h"
#include "cover.h"
#include "dirwatch.h"
#include "shutdown.h"
#include "splash.h"

static struct {
	struct shutdown_config cfg;
	bool started, shown, frozen;
	const char *run;
	struct dirwatch_dir mdir;	/* the marker's directory */
	struct dirwatch mw;
	int mwatched;			/* the marker watch's fd registered with chefui, -1 none */
	struct shutsteps st;
	bool steps;
	int swatched;
	struct cover c;
	enum shutsteps_reason reason;
	lv_timer_t *timer;
} S;

/* One log line "shutdown screen: <what> at S.CC s<extra>". */
static void log_at(const char *what, const char *extra)
{
	cover_log_at("shutdown screen", what, S.cfg.boot_ms(), extra);
}

/* ------------------------------------------------------------ the view */

/* The step shown and the bar's fill, from the tracker (or K frozen). */
static void show_steps(int current, int finished)
{
	cover_set(&S.c, shutsteps_label(current, S.reason), true, current, SHUTSTEPS_N, finished);
}

static void update(void);
static void rearm(void);
static void check(void);
static void marker_fd_cb(int fd, short revents, void *user);

static void steps_fd_cb(int fd, short revents, void *user)
{
	(void)fd;
	(void)revents;
	(void)user;
	shutsteps_handle(&S.st);
	update();
}

/* The marker's directory watched again if it is not (it went away, or was
 * never there), its fd followed, and the marker looked at. */
static void marker_watch(void)
{
	dirwatch_arm(&S.mw, S.cfg.now());
	cover_follow_fd(&S.mwatched, S.mw.fd, marker_fd_cb);
	check();
}

static void marker_fd_cb(int fd, short revents, void *user)
{
	(void)fd;
	(void)revents;
	(void)user;
	dirwatch_drain(&S.mw);
	marker_watch();
	rearm();
}

/* The tracker's state on the screen. */
static void update(void)
{
	if (!S.shown)
		return;
	show_steps(shutsteps_current(&S.st), shutsteps_finished(&S.st));
	cover_follow_fd(&S.swatched, shutsteps_fd(&S.st), steps_fd_cb);
	rearm();
}

/* The next retry: the marker's directory, the tracker's directories. */
static void rearm(void)
{
	int64_t at = S.mw.retry_at, d = S.shown ? shutsteps_deadline(&S.st) : -1, n = S.cfg.now();

	if (d >= 0 && (at < 0 || d < at))
		at = d;
	cover_timer_at(S.timer, at, n);
}

static void timer_cb(lv_timer_t *t)
{
	(void)t;
	if (S.mw.retry_at >= 0 && S.cfg.now() >= S.mw.retry_at)
		marker_watch();
	if (S.shown) {
		shutsteps_tick(&S.st);
		update();
	}
	rearm();
}

static void show(enum shutsteps_reason reason)
{
	struct shutsteps_config sc = {
		.openrc = S.cfg.openrc, .now = S.cfg.now, .boot_ms = S.cfg.boot_ms, .log = cover_log,
	};

	/* the splash ends first, so its cover is gone and its hook has run */
	splash_end_for_shutdown();
	S.reason = reason;
	S.shown = true;
	cover_new(&S.c, S.cfg.parent, S.cfg.image_info, false);
	{
		char how[32];

		snprintf(how, sizeof(how), " (%s)", shutsteps_reason_name(reason));
		log_at("shown", how);
	}
	shutsteps_init(&S.st, &sc);
	S.steps = true;
	/* lit after the view exists, with the sleep timer already held off */
	if (S.cfg.screen_is_on && !S.cfg.screen_is_on() && S.cfg.screen_set)
		S.cfg.screen_set(true);
	update();
	if (S.cfg.changed)
		S.cfg.changed();
}

static void hide(void)
{
	cover_follow_fd(&S.swatched, -1, steps_fd_cb);
	shutsteps_fini(&S.st);
	S.steps = false;
	cover_delete(&S.c);
	S.shown = false;
	log_at("ended", " (marker removed)");
	rearm();
	if (S.cfg.changed)
		S.cfg.changed();
}

/* The marker against the screen: up when it is there, down when it is not. */
static void check(void)
{
	bool present;
	enum shutsteps_reason r = shutsteps_read_reason(S.run, &present);

	if (present && !S.shown)
		show(r);
	else if (!present && S.shown)
		hide();
}

/* ------------------------------------------------------------ lifecycle */

bool shutdown_start(const struct shutdown_config *cfg)
{
	if (S.started)
		return false;
	memset(&S, 0, sizeof(S));
	S.cfg = *cfg;
	S.started = true;
	S.mwatched = S.swatched = -1;
	cover_log_to(cfg->log);
	cover_clock_defaults(&S.cfg.now, &S.cfg.boot_ms);
	if (cfg->frozen >= 0) {
		int k = cfg->frozen;

		S.frozen = S.shown = true;
		S.reason = cfg->frozen_reason;
		cover_new(&S.c, cfg->parent, cfg->image_info, true);
		/* 5 and 6: step 6 shown, as live once 1 to 5 have finished */
		show_steps(k < SHUTSTEPS_N - 1 ? k : SHUTSTEPS_N - 1, k < SHUTSTEPS_N - 1 ? k : SHUTSTEPS_N);
		return true;
	}
	S.run = cfg->run ? cfg->run : SHUTSTEPS_RUN_DEFAULT;
	S.mdir = (struct dirwatch_dir){ S.run, DIRWATCH_DIR_EVENTS };
	dirwatch_init(&S.mw, &S.mdir, 1);
	S.timer = lv_timer_create(timer_cb, 1000, NULL);
	lv_timer_pause(S.timer);
	/* the watch first, so a marker made after the check is an event */
	marker_watch();
	rearm();
	return true;
}

bool shutdown_active(void)
{
	return S.shown;
}

void shutdown_stop(void)
{
	if (!S.started)
		return;
	if (S.shown && !S.frozen)
		log_at("ended", " (terminated)");
	cover_follow_fd(&S.swatched, -1, steps_fd_cb);
	cover_follow_fd(&S.mwatched, -1, marker_fd_cb);
	if (S.steps)
		shutsteps_fini(&S.st);
	dirwatch_close(&S.mw);
	if (S.timer)
		lv_timer_delete(S.timer);
	if (S.shown)
		cover_delete(&S.c);
	memset(&S, 0, sizeof(S));
}
