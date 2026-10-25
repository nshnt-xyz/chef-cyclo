/*
 * splash.c - the boot splash (splash.h).
 *
 * Layout in device pixels from the design (B01, 1080x2246; sizes follow
 * the panel height): the title block centred in the panel minus a 200 px
 * bottom padding, the progress block 120 px from both sides with its bar
 * bottom 360 px above the panel's bottom.
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
#include "deviceinfo.h"
#include "fonts.h"
#include "splash.h"
#include "sysstate.h"
#include "widgets.h"

#define SIDE_PAD 120
#define BAR_H 24
#define BAR_BOTTOM 360
#define BAR_GAP 32		/* between the text row and the bar */
#define TITLE_H 150		/* line-height 1 */
#define TITLE_GAP 20
#define COUNT_W 200
#define IMAGE_INFO_MAX 1024

static struct {
	struct splash_config cfg;
	bool active;
	struct bootsteps b;
	bool steps;			/* bootsteps in use (live) */
	int watched;			/* the fd registered with chefui, -1 none */
	lv_timer_t *timer;
	lv_obj_t *cover, *text, *count, *fill;
	int bar_w;
	int acked;			/* leading steps shown through (0..5) */
	int64_t hold_start, ready_at;	/* -1 none */
} S;

/* ------------------------------------------------------------ helpers */

static int64_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t now(void)
{
	return S.cfg.now ? S.cfg.now() : mono_ms();
}

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
	char buf[160];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (S.cfg.log)
		S.cfg.log("%s", buf);
	else
		chefui_log("%s", buf);
}

static void bootsteps_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void bootsteps_log(const char *fmt, ...)
{
	char buf[160];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	logf_("%s", buf);
}

static int64_t boot_now(void)
{
	struct timespec ts;

	if (S.cfg.boot_ms)
		return S.cfg.boot_ms();
	clock_gettime(CLOCK_BOOTTIME, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

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
		logf_("boot splash: %s: %s", p, strerror(errno));
		return;
	}
	fchmod(fd, 0644);	/* whatever the umask said */
	close(fd);
}

/* "<version> · <commit, 8 hex>[+dirty]", empty without image facts. */
static void version_line(char *out, size_t n)
{
	const char *version = NULL, *commit = NULL;
	bool dirty = false;
	struct sys_image_info info;
	char buf[IMAGE_INFO_MAX + 1];

	out[0] = 0;
	if (S.cfg.frozen >= 0) {
		if (!sys_state.device.metadata_known)
			return;
		version = sys_state.device.version;
		commit = sys_state.device.commit;
		dirty = sys_state.device.dirty;
	} else {
		int fd = open(S.cfg.image_info ? S.cfg.image_info : "/etc/chef/image-info",
			      O_RDONLY | O_CLOEXEC);
		ssize_t r;

		if (fd < 0)
			return;
		r = read(fd, buf, sizeof(buf));
		close(fd);
		if (r <= 0 || r > IMAGE_INFO_MAX || !sys_image_info_parse(buf, (size_t)r, &info))
			return;
		version = info.version;
		commit = info.commit;
		dirty = info.dirty;
	}
	snprintf(out, n, "%s \xC2\xB7 %.8s%s", version, commit, dirty ? "+dirty" : "");
}

/* ----------------------------------------------------------------- view */

static lv_obj_t *place(lv_obj_t *label, int x, int y)
{
	lv_obj_t *box = lv_obj_get_parent(label);

	lv_obj_set_pos(box, x, y);
	return box;
}

static void build_view(void)
{
	lv_obj_t *p = S.cfg.parent, *title, *version, *track;
	int w = lv_obj_get_width(p), h = lv_obj_get_height(p);
	int centre = (h - 200) / 2, ty = centre - (TITLE_H + TITLE_GAP + kit_css_line(&chefkit_b400_40)) / 2;
	int bar_y = h - BAR_BOTTOM - BAR_H, row_h = kit_css_line(&chefkit_b500_48);
	int row_y = bar_y - BAR_GAP - row_h;
	char vl[96];

	S.bar_w = w - 2 * SIDE_PAD;
	/* the whole panel, over the status bar and the hub, taking every touch */
	S.cover = lv_obj_create(p);
	lv_obj_remove_style_all(S.cover);
	lv_obj_set_size(S.cover, w, h);
	lv_obj_set_pos(S.cover, 0, 0);
	lv_obj_set_scrollable(S.cover, false);
	lv_obj_set_clickable(S.cover, true);
	lv_obj_add_style(S.cover, &kit_st.screen, 0);

	title = kit_text(S.cover, &chefkit_bc600_150, TITLE_H, KIT_INK_FG, "chef-cyclo");
	lv_obj_set_style_text_letter_space(title, 3, 0);	/* 0.02 em */
	version_line(vl, sizeof(vl));
	version = kit_text(S.cover, &chefkit_b400_40, 0, KIT_INK_MUT, vl);
	lv_obj_update_layout(S.cover);
	/* CSS letter-spacing also trails the last glyph: its box is 3 px wider, so
	 * the glyphs sit 1.5 px left of LVGL's centre (1 px matches the render) */
	place(title, (w - lv_obj_get_width(lv_obj_get_parent(title))) / 2 - 1, ty);
	place(version, (w - lv_obj_get_width(lv_obj_get_parent(version))) / 2, ty + TITLE_H + TITLE_GAP);

	S.text = kit_text(S.cover, &chefkit_b500_48, 0, KIT_INK_FG, "");
	place(S.text, SIDE_PAD, row_y);
	S.count = kit_text(S.cover, &chefkit_bc600_48, 0, KIT_INK_MUT, "");
	lv_obj_set_width(lv_obj_get_parent(S.count), COUNT_W);
	lv_obj_set_width(S.count, COUNT_W);
	lv_obj_set_style_text_align(S.count, LV_TEXT_ALIGN_RIGHT, 0);
	place(S.count, w - SIDE_PAD - COUNT_W, row_y);

	track = kit_box(S.cover);
	lv_obj_set_size(track, S.bar_w, BAR_H);
	lv_obj_set_pos(track, SIDE_PAD, bar_y);
	lv_obj_add_style(track, &kit_st.splash_track, 0);
	S.fill = kit_box(track);
	lv_obj_set_pos(S.fill, 0, 0);
	lv_obj_set_size(S.fill, 0, BAR_H);
	lv_obj_add_style(S.fill, &kit_st.splash_fill, 0);
}

/* Text, count and fill: STEP is the 0-based step shown (BOOTSTEPS_N for
 * Ready), FINISHED the bar's fill out of BOOTSTEPS_N. */
static void show(const char *text, int step, int finished)
{
	char c[16];
	int fw = S.bar_w * finished / BOOTSTEPS_N;

	snprintf(c, sizeof(c), "%d / %d", step < BOOTSTEPS_N ? step + 1 : BOOTSTEPS_N, BOOTSTEPS_N);
	if (strcmp(lv_label_get_text(S.text), text))
		lv_label_set_text(S.text, text);
	if (strcmp(lv_label_get_text(S.count), c))
		lv_label_set_text(S.count, c);
	lv_obj_set_width(S.fill, fw);
	lv_obj_set_hidden(S.fill, fw == 0);
}

static void show_step(int step, bool failed, int finished)
{
	char t[64];

	if (failed)
		snprintf(t, sizeof(t), "%s", bootsteps_failed_label(step));
	else
		snprintf(t, sizeof(t), "%s\xE2\x80\xA6", bootsteps_label(step));
	show(t, step, finished);
}

/* ------------------------------------------------------------ lifecycle */

static void sync_watch(void);
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

/* The fd bootsteps has now (it may have closed or made one), registered
 * with the loop; the old registration goes first. */
static void sync_watch(void)
{
	int fd = S.steps ? bootsteps_fd(&S.b) : -1;

	if (fd == S.watched)
		return;
	if (S.watched >= 0)
		chefui_unwatch_fd(S.watched);
	S.watched = fd;
	if (fd >= 0 && chefui_watch_fd(fd, POLLIN, fd_cb, NULL))
		S.watched = -1;
}

static void end(bool capped)
{
	void (*ended)(void) = S.cfg.ended;
	int64_t t = boot_now();

	if (S.watched >= 0)
		chefui_unwatch_fd(S.watched);
	S.watched = -1;
	if (S.steps)
		bootsteps_fini(&S.b);
	S.steps = false;
	if (S.timer)
		lv_timer_delete(S.timer);
	S.timer = NULL;
	lv_obj_delete(S.cover);
	S.cover = NULL;
	S.active = false;
	marker_create();
	logf_("boot splash: ended at %lld.%02d s%s", (long long)(t / 1000), (int)(t % 1000 / 10),
	      capped ? " (cap)" : "");
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
	int64_t at = bootsteps_deadline(&S.b), n = now();

	if (S.hold_start >= 0 && (at < 0 || S.hold_start + SPLASH_FAIL_HOLD_MS < at))
		at = S.hold_start + SPLASH_FAIL_HOLD_MS;
	if (S.ready_at >= 0 && (at < 0 || S.ready_at + SPLASH_READY_HOLD_MS < at))
		at = S.ready_at + SPLASH_READY_HOLD_MS;
	if (at < 0) {
		lv_timer_pause(S.timer);
		return;
	}
	lv_timer_set_period(S.timer, at > n ? (uint32_t)(at - n) : 1);
	lv_timer_reset(S.timer);
	lv_timer_resume(S.timer);
}

/* The state machine: which step is shown, the holds, the end. */
static void update(void)
{
	int64_t n = now();

	if (!S.active)
		return;
	if (S.b.capped) {
		end(true);
		return;
	}
	/* steps shown through: done ones at once, a failed one after its hold */
	while (S.acked < BOOTSTEPS_N - 1) {
		enum bootstep_state st = S.b.state[S.acked];

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
			end(false);
			return;
		}
		show("Ready", BOOTSTEPS_N, BOOTSTEPS_N);
	} else {
		/* the bar is the walk's position, never ahead of the text */
		bool failed = S.b.state[S.acked] == BOOTSTEP_FAILED;

		show_step(S.acked, failed, S.acked + (failed ? 1 : 0));
	}
	sync_watch();
	rearm();
}

bool splash_start(const struct splash_config *cfg)
{
	if (S.active)
		return false;
	memset(&S, 0, sizeof(S));
	S.cfg = *cfg;
	S.watched = -1;
	S.hold_start = S.ready_at = -1;
	if (cfg->frozen < 0 && marker_exists())
		return false;
	S.active = true;
	build_view();
	if (cfg->frozen >= 0) {
		int k = cfg->frozen;

		if (k >= BOOTSTEPS_N)
			show("Ready", BOOTSTEPS_N, BOOTSTEPS_N);
		else
			show_step(k, false, k);
		return true;
	}
	{
		struct bootsteps_config bc = {
			.openrc = cfg->openrc, .run = cfg->run, .now = S.cfg.now, .boot_ms = S.cfg.boot_ms,
			.log = bootsteps_log,
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
	lv_obj_delete(S.cover);
	memset(&S, 0, sizeof(S));
}
