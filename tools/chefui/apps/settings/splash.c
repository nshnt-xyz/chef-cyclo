/*
 * splash.c - the boot splash (splash.h): the bootsteps wiring, the holds,
 * the marker and the end rules around the shared cover (cover.h).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "bootsteps.h"
#include "chefui.h"
#include "cover.h"
#include "splash.h"

static struct {
	struct splash_config cfg;
	bool active;
	struct bootsteps b;
	bool steps;			/* bootsteps in use (live) */
	int watched;			/* the fd registered with chefui, -1 none */
	lv_timer_t *timer;
	struct cover c;
	int acked;			/* leading steps shown through (0..5) */
	int64_t hold_start, ready_at;	/* -1 none */
} S;

/* ------------------------------------------------------------ helpers */

static void marker_path(char *out, size_t n)
{
	snprintf(out, n, "%s/splash-done", S.cfg.done_dir);
}

static bool marker_exists(void)
{
	char p[320];
	struct stat st;

	marker_path(p, sizeof(p));
	return stat(p, &st) == 0;
}

static void marker_create(void)
{
	char p[320];
	int fd;

	mkdir(S.cfg.done_dir, 0755);
	marker_path(p, sizeof(p));
	fd = open(p, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
	if (fd < 0) {
		cover_log("boot splash: %s: %s", p, strerror(errno));
		return;
	}
	fchmod(fd, 0644);	/* whatever the umask said */
	close(fd);
}

/* Text, count and fill: STEP is the 0-based step shown (BOOTSTEPS_N for
 * Ready), FINISHED the bar's fill out of BOOTSTEPS_N. */
static void show(const char *text, bool ellipsis, int step, int finished)
{
	cover_set(&S.c, text, ellipsis, step, BOOTSTEPS_N, finished);
}

static void show_step(int step, bool failed, int finished)
{
	if (failed)
		show(bootsteps_failed_label(step), false, step, finished);
	else
		show(bootsteps_label(step), true, step, finished);
}

/* ------------------------------------------------------------ lifecycle */

static void rearm(void);
static void update(void);

static void fd_cb(int fd, short revents, void *user)
{
	(void)fd;
	(void)revents;
	(void)user;
	bootsteps_handle(&S.b);
	update();
}

/* CAP is "" or " (cap)": the log line's words. BY_SHUTDOWN adds a second
 * line saying who ended it. */
static void end(bool by_shutdown, const char *cap)
{
	void (*ended)(void) = S.cfg.ended;

	if (S.watched >= 0)
		chefui_unwatch_fd(S.watched);
	S.watched = -1;
	if (S.steps)
		bootsteps_fini(&S.b);
	S.steps = false;
	if (S.timer)
		lv_timer_delete(S.timer);
	S.timer = NULL;
	cover_delete(&S.c);
	S.active = false;
	marker_create();
	cover_log_at("boot splash", "ended", S.cfg.boot_ms(), cap);
	if (by_shutdown)
		cover_log_at("boot splash", "ended by shutdown", S.cfg.boot_ms(), "");
	if (ended)
		ended();
}

static void timer_cb(lv_timer_t *t)
{
	(void)t;
	bootsteps_tick(&S.b);
	update();
}

static void rearm(void)
{
	int64_t at = bootsteps_deadline(&S.b), n = S.cfg.now();

	if (S.hold_start >= 0 && (at < 0 || S.hold_start + SPLASH_FAIL_HOLD_MS < at))
		at = S.hold_start + SPLASH_FAIL_HOLD_MS;
	if (S.ready_at >= 0 && (at < 0 || S.ready_at + SPLASH_READY_HOLD_MS < at))
		at = S.ready_at + SPLASH_READY_HOLD_MS;
	cover_timer_at(S.timer, at, n);
}

/* The state machine: which step is shown, the holds, the end. */
static void update(void)
{
	int64_t n = S.cfg.now();

	if (!S.active)
		return;
	if (S.b.t.capped) {
		end(false, " (cap)");
		return;
	}
	/* steps shown through: done ones at once, a failed one after its hold */
	while (S.acked < BOOTSTEPS_N - 1) {
		enum bootstep_state st = S.b.t.state[S.acked];

		if (st == BOOTSTEP_DONE) {
			S.acked++;
		} else if (st == BOOTSTEP_FAILED) {
			if (S.hold_start < 0) {
				S.hold_start = n;
				break;
			}
			if (n - S.hold_start < SPLASH_FAIL_HOLD_MS)
				break;
			S.hold_start = -1;
			S.acked++;
		} else {
			break;
		}
	}
	if (S.acked == BOOTSTEPS_N - 1) {
		if (S.ready_at < 0)
			S.ready_at = n;
		if (n - S.ready_at >= SPLASH_READY_HOLD_MS) {
			end(false, "");
			return;
		}
		show("Ready", false, BOOTSTEPS_N, BOOTSTEPS_N);
	} else {
		/* the bar is the walk's position, never ahead of the text */
		bool failed = S.b.t.state[S.acked] == BOOTSTEP_FAILED;

		show_step(S.acked, failed, S.acked + (failed ? 1 : 0));
	}
	cover_follow_fd(&S.watched, bootsteps_fd(&S.b), fd_cb);
	rearm();
}

bool splash_start(const struct splash_config *cfg)
{
	if (S.active)
		return false;
	memset(&S, 0, sizeof(S));
	S.cfg = *cfg;
	cover_clock_defaults(&S.cfg.now, &S.cfg.boot_ms);
	S.watched = -1;
	S.hold_start = S.ready_at = -1;
	if (cfg->frozen < 0 && marker_exists())
		return false;
	S.active = true;
	cover_log_to(cfg->log);
	cover_new(&S.c, cfg->parent, cfg->image_info, cfg->frozen >= 0);
	if (cfg->frozen >= 0) {
		int k = cfg->frozen;

		if (k >= BOOTSTEPS_N)
			show("Ready", false, BOOTSTEPS_N, BOOTSTEPS_N);
		else
			show_step(k, false, k);
		return true;
	}
	{
		struct bootsteps_config bc = {
			.openrc = cfg->openrc, .run = cfg->run, .now = S.cfg.now, .boot_ms = S.cfg.boot_ms,
			.log = cover_log,
		};

		bootsteps_init(&S.b, &bc);
		S.steps = true;
	}
	S.timer = lv_timer_create(timer_cb, 1000, NULL);
	lv_timer_pause(S.timer);
	update();
	return true;
}

bool splash_active(void)
{
	return S.active;
}

void splash_stop(void)
{
	if (!S.active)
		return;
	if (S.watched >= 0)
		chefui_unwatch_fd(S.watched);
	if (S.steps)
		bootsteps_fini(&S.b);
	if (S.timer)
		lv_timer_delete(S.timer);
	cover_delete(&S.c);
	memset(&S, 0, sizeof(S));
}

void splash_end_for_shutdown(void)
{
	if (S.active && S.steps)
		end(true, "");
}
