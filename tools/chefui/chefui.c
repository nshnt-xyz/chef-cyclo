/*
 * chefui.c - application API, the poll() main loop, buttond client glue,
 * lifecycle logging and CHEFUI_STATS. Backend independent: the display,
 * touch and screen state live in backend_fb.c (phone) or backend_sdl.c
 * (PC). See chefui.h and docs/archive/ui-platform.md.
 *
 * Main loop: lv_timer_handler() says when LVGL next needs to run; the
 * loop sleeps in poll() on the touch fd, the buttond socket, the
 * application's fds and a self-pipe for SIGTERM/SIGINT/SIGHUP until
 * then. A static page therefore wakes only for its own timers (LVGL's
 * refresh timer pauses itself when nothing is invalid).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "buttons.h"
#include "copy.h"
#include "chefui_internal.h"

#define CU_MAX_WATCH 16
#define CU_DEFAULT_REFRESH_MS 16
#define CU_DEFAULT_BRIGHTNESS 96

struct cu_stats cu_stats;
unsigned long cu_total_wakeups;
struct cu_test_paths cu_paths;

struct watch {
	int fd;
	short events;
	chefui_fd_cb cb;
	void *user;
};

static struct {
	bool inited;
	char app[32];
	struct chefui_config cfg;
	const struct cu_backend *be;
	lv_display_t *disp;
	lv_indev_t *pointer;
	lv_obj_t *content;
	int safe_top;
	uint32_t refresh_ms;

	bool quit;
	int quit_code;
	int sigpipe[2];

	struct watch w[CU_MAX_WATCH];
	int nw;

	chefui_button_cb button_cb;
	void *button_user;
	chefui_screen_cb screen_cb;
	void *screen_user;
	chefui_touch_cb touch_cb;
	void *touch_user;

	struct cu_buttons buttons;

	bool stats;
	char stats_text[256];
	double stats_cpu_ms, stats_wall_ms;
} G = { .sigpipe = { -1, -1 } };

static volatile sig_atomic_t g_signal;

/* ------------------------------------------------------------- helpers */

int64_t cu_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static double ms_of(clockid_t c)
{
	struct timespec ts;

	clock_gettime(c, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static uint32_t tick_cb(void)
{
	return (uint32_t)cu_now_ms();
}

void cu_set_test_paths(const struct cu_test_paths *p)
{
	cu_paths = *p;
}

void chefui_log(const char *fmt, ...)
{
	char buf[320];
	va_list ap;
	int fd, n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	fprintf(stderr, "chefui[%s]: %s\n", G.app[0] ? G.app : "?", buf);
	if (!cu_paths.kmsg && G.be && G.be->kind == CHEFUI_BACKEND_SDL)
		return;		/* host: no device paths, stderr only */
	fd = open(cu_paths.kmsg ? cu_paths.kmsg : "/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	dprintf(fd, "chefui[%s]: %s\n", G.app[0] ? G.app : "?", buf);
	close(fd);
}

static void on_signal(int sig)
{
	int saved = errno;

	g_signal = sig;
	if (G.sigpipe[1] >= 0)
		(void)!write(G.sigpipe[1], "s", 1);
	errno = saved;
}

/* ------------------------------------------------------------- buttons */

static bool app_button(const char *g, void *user)
{
	(void)user;
	return G.button_cb ? G.button_cb(g, G.button_user) : false;
}

static void default_power_short(const char *g, void *user)
{
	(void)g;
	(void)user;
	chefui_log("power.short: screen %s", chefui_screen_is_on() ? "off" : "on");
	(void)chefui_screen_set(!chefui_screen_is_on());
}

void cu_dispatch_gesture(const char *gesture)
{
	cu_buttons_dispatch(&G.buttons, gesture);
}

void cu_notify_touch(const struct chefui_contact *c, int n)
{
	if (G.touch_cb)
		G.touch_cb(c, n, G.touch_user);
}

void cu_notify_screen(bool on)
{
	if (G.screen_cb)
		G.screen_cb(on, G.screen_user);
}

uint32_t cu_refresh_period(void)
{
	return G.refresh_ms;
}

/* --------------------------------------------------------------- stats */

static void stats_tick(lv_timer_t *t)
{
	double cpu = ms_of(CLOCK_PROCESS_CPUTIME_ID), wall = ms_of(CLOCK_MONOTONIC);
	double dw = wall - G.stats_wall_ms;

	(void)t;
	snprintf(G.stats_text, sizeof(G.stats_text),
		 "fps %u | cpu %.1f%% | copy %.2f Mpx %.1f ms | pan avg %.1f max %.1f ms | wake %u",
		 cu_stats.frames, dw > 0 ? (cpu - G.stats_cpu_ms) * 100.0 / dw : 0.0,
		 cu_stats.px / 1e6, cu_stats.copy_ms,
		 cu_stats.frames ? cu_stats.pan_ms / cu_stats.frames : 0.0, cu_stats.pan_max_ms,
		 cu_stats.wakeups);
	fprintf(stderr, "chefui[%s] stats: %s\n", G.app, G.stats_text);
	G.stats_cpu_ms = cpu;
	G.stats_wall_ms = wall;
	memset(&cu_stats, 0, sizeof(cu_stats));
}

const char *chefui_stats_text(void)
{
	return G.stats_text;
}

/* ----------------------------------------------------------------- API */

static const struct cu_backend *pick_backend(enum chefui_backend want)
{
	int i;

	for (i = 0; cu_backends[i]; i++)
		if (want == CHEFUI_BACKEND_AUTO || cu_backends[i]->kind == want)
			return cu_backends[i];
	return NULL;
}

lv_display_t *chefui_init(const struct chefui_config *cfg)
{
	struct sigaction sa;
	const char *st;
	int r;

	if (G.inited || !cfg || !cfg->app_name)
		return NULL;
	G.cfg = *cfg;
	snprintf(G.app, sizeof(G.app), "%s", cfg->app_name);
	G.refresh_ms = cfg->refresh_ms ? cfg->refresh_ms : CU_DEFAULT_REFRESH_MS;
	if (!G.cfg.brightness)
		G.cfg.brightness = CU_DEFAULT_BRIGHTNESS;
	if (!G.cfg.host_zoom)
		G.cfg.host_zoom = 0.4f;
	G.be = pick_backend(cfg->backend);
	if (!G.be) {
		chefui_log("backend %d not built into this binary", (int)cfg->backend);
		return NULL;
	}
	chefui_log("start (pid %d, %s backend, %u ms frames, rotation %d)", (int)getpid(),
		   G.be->name, G.refresh_ms, cfg->rotation);

	if (pipe2(G.sigpipe, O_CLOEXEC | O_NONBLOCK) < 0) {
		chefui_log("pipe: %s", strerror(errno));
		return NULL;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	lv_init();
	lv_tick_set_cb(tick_cb);
	/* Animation cadence is independent of the display refresh timer.
	 * Keep it matched: a fixed 33ms animation tick caps -f60 at ~31fps. */
	if (lv_anim_get_timer())
		lv_timer_set_period(lv_anim_get_timer(), G.refresh_ms);

	cu_buttons_init(&G.buttons, cu_paths.buttond_sock, cfg->claims, chefui_log);
	G.buttons.app_cb = app_button;
	G.buttons.default_cb = default_power_short;

	r = G.be->init(&G.cfg, &G.disp, &G.pointer);
	if (r) {
		chefui_log("display init failed: %s", strerror(-r));
		G.be->shutdown();
		lv_deinit();
		close(G.sigpipe[0]);
		close(G.sigpipe[1]);
		G.sigpipe[0] = G.sigpipe[1] = -1;
		return NULL;
	}
	if (G.be->uses_buttond && cu_buttons_connect(&G.buttons, cu_now_ms()) < 0)
		chefui_log("buttond not reachable at %s; retrying every %d ms", G.buttons.sock_path,
			   CU_BTN_RETRY_MS);

	st = getenv("CHEFUI_STATS");
	G.stats = st && strcmp(st, "1") == 0;
	if (G.stats) {
		G.stats_cpu_ms = ms_of(CLOCK_PROCESS_CPUTIME_ID);
		G.stats_wall_ms = ms_of(CLOCK_MONOTONIC);
		lv_timer_create(stats_tick, 1000, NULL);
	}
	G.inited = true;
	if (chefui_safe_top_set(cfg->safe_top_px)) {
		chefui_log("invalid safe top inset %d", cfg->safe_top_px);
		cu_buttons_close(&G.buttons);
		G.be->shutdown();
		lv_deinit();
		close(G.sigpipe[0]);
		close(G.sigpipe[1]);
		G.sigpipe[0] = G.sigpipe[1] = -1;
		G.inited = false;
		return NULL;
	}
	return G.disp;
}

lv_obj_t *chefui_root(void)
{
	return G.inited ? lv_display_get_screen_active(G.disp) : NULL;
}

lv_area_t chefui_safe_area(void)
{
	struct cu_geom g;
	struct cu_rect r;
	int w, h;
	if (!G.inited)
		return (lv_area_t){ 0, 0, -1, -1 };
	w = lv_display_get_horizontal_resolution(G.disp);
	h = lv_display_get_vertical_resolution(G.disp);
	cu_geom_init(&g, G.cfg.rotation,
		G.cfg.rotation % 180 ? h : w, G.cfg.rotation % 180 ? w : h);
	r = cu_safe_area(&g, G.safe_top);
	return (lv_area_t){ r.x1, r.y1, r.x2, r.y2 };
}

static void content_deleted(lv_event_t *e)
{
	if (G.content == lv_event_get_target(e))
		G.content = NULL;
}

static lv_obj_t *find_content(lv_obj_t *root)
{
	for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
		lv_obj_t *child = lv_obj_get_child(root, i);
		for (uint32_t j = 0; j < lv_obj_get_event_count(child); j++)
			if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(child, j)) == content_deleted)
				return child;
	}
	return NULL;
}

static void content_position(void)
{
	lv_area_t a = chefui_safe_area();
	G.content = find_content(chefui_root());
	if (G.content) {
		lv_obj_set_pos(G.content, a.x1, a.y1);
		lv_obj_set_size(G.content, a.x2 - a.x1 + 1, a.y2 - a.y1 + 1);
	}
}

lv_obj_t *chefui_content_root(void)
{
	lv_obj_t *root = chefui_root();
	if (!root)
		return NULL;
	G.content = find_content(root);
	if (!G.content) {
		G.content = lv_obj_create(root);
		lv_obj_remove_style_all(G.content);
		lv_obj_set_scrollable(G.content, false);
		lv_obj_set_clickable(G.content, false);
		lv_obj_add_event_cb(G.content, content_deleted, LV_EVENT_DELETE, NULL);
	}
	content_position();
	return G.content;
}

int chefui_safe_top_set(int pixels)
{
	int ph, inset = pixels == 0 ? CHEFUI_DEFAULT_SAFE_TOP_PX : pixels;
	if (!G.inited)
		return -ENODEV;
	ph = G.cfg.rotation % 180 ? lv_display_get_horizontal_resolution(G.disp)
		: lv_display_get_vertical_resolution(G.disp);
	if (pixels < -1 || inset >= ph)
		return -EINVAL;
	G.safe_top = inset < 0 ? 0 : inset;
	content_position();
	return 0;
}

int chefui_safe_top_get(void)
{
	return G.inited ? G.safe_top : 0;
}

void chefui_quit(int code)
{
	G.quit = true;
	G.quit_code = code;
}

int chefui_watch_fd(int fd, short events, chefui_fd_cb cb, void *user)
{
	int i;

	if (fd < 0 || !cb)
		return -EINVAL;
	for (i = 0; i < G.nw; i++)
		if (G.w[i].fd == fd) {
			G.w[i].events = events;
			G.w[i].cb = cb;
			G.w[i].user = user;
			return 0;
		}
	if (G.nw == CU_MAX_WATCH)
		return -ENOSPC;
	G.w[G.nw++] = (struct watch){ fd, events, cb, user };
	return 0;
}

int chefui_unwatch_fd(int fd)
{
	int i;

	for (i = 0; i < G.nw; i++)
		if (G.w[i].fd == fd) {
			G.w[i] = G.w[--G.nw];
			return 0;
		}
	return -ENOENT;
}

int chefui_screen_set(bool on)
{
	if (!G.be)
		return -ENODEV;
	return G.be->screen_set(on);
}

bool chefui_screen_is_on(void)
{
	return G.be ? G.be->screen_is_on() : false;
}

void chefui_on_screen(chefui_screen_cb cb, void *user)
{
	G.screen_cb = cb;
	G.screen_user = user;
}

int chefui_brightness_set(int level)
{
	return G.be ? G.be->brightness_set(level) : -ENODEV;
}

int chefui_brightness_get(void)
{
	return G.be ? G.be->brightness_get() : 0;
}

void chefui_refresh_period_set(uint32_t ms)
{
	lv_timer_t *t;

	if (!ms)
		ms = CU_DEFAULT_REFRESH_MS;
	G.refresh_ms = ms;
	if (G.disp && (t = lv_display_get_refr_timer(G.disp)))
		lv_timer_set_period(t, ms);
	if ((t = lv_anim_get_timer()))
		lv_timer_set_period(t, ms);
}

void chefui_on_button(chefui_button_cb cb, void *user)
{
	G.button_cb = cb;
	G.button_user = user;
}

void chefui_on_touch(chefui_touch_cb cb, void *user)
{
	G.touch_cb = cb;
	G.touch_user = user;
}

lv_indev_t *chefui_pointer(void)
{
	return G.pointer;
}

int chefui_handoff_exec(const char *path, char *const argv[], bool dark)
{
	int r;

	if (!G.be)
		return -ENODEV;
	cu_buttons_close(&G.buttons);	/* CLOEXEC anyway; buttond drops the claims now */
	r = G.be->handoff_exec(path, argv, dark);
	if (G.be->uses_buttond)
		(void)cu_buttons_connect(&G.buttons, cu_now_ms());
	return r;
}

/* ---------------------------------------------------------------- loop */

#define MAX_PFD (8 + 1 + 1 + CU_MAX_WATCH)

int chefui_run(void)
{
	struct pollfd pfd[MAX_PFD];
	struct watch snap[CU_MAX_WATCH];

	if (!G.inited)
		return 1;
	while (!G.quit && !g_signal) {
		uint32_t next = lv_timer_handler();
		int n = 0, nbe, ib = -1, isig, iw, nsnap, timeout, i, r;
		int64_t now;

		if (G.quit || g_signal)
			break;
		pfd[n] = (struct pollfd){ G.sigpipe[0], POLLIN, 0 };
		isig = n++;
		nbe = G.be->add_pollfds(pfd + n, 8);
		n += nbe;
		if (G.buttons.fd >= 0) {
			pfd[n] = (struct pollfd){ G.buttons.fd, POLLIN, 0 };
			ib = n++;
		}
		iw = n;
		nsnap = G.nw;
		memcpy(snap, G.w, sizeof(snap[0]) * (size_t)nsnap);
		for (i = 0; i < nsnap; i++)
			pfd[n++] = (struct pollfd){ snap[i].fd, snap[i].events, 0 };

		timeout = next == LV_NO_TIMER_READY ? -1 : (int)next;
		timeout = G.be->timeout_ms(timeout);
		if (G.be->uses_buttond) {
			int bt = cu_buttons_timeout_ms(&G.buttons, cu_now_ms());

			if (bt >= 0 && (timeout < 0 || bt < timeout))
				timeout = bt;
		}

		r = poll(pfd, (nfds_t)n, timeout);
		cu_stats.wakeups++;
		cu_total_wakeups++;
		if (r < 0) {
			if (errno == EINTR)
				continue;
			chefui_log("poll: %s", strerror(errno));
			G.quit_code = 1;
			break;
		}
		if (pfd[isig].revents) {
			char b[16];

			while (read(G.sigpipe[0], b, sizeof(b)) > 0)
				;
		}
		if (g_signal)
			break;
		G.be->handle_pollfds(pfd + 1, nbe);
		now = cu_now_ms();
		if (ib >= 0 && pfd[ib].revents)
			cu_buttons_input(&G.buttons, now);
		if (G.be->uses_buttond)
			cu_buttons_tick(&G.buttons, now);
		for (i = 0; i < nsnap && !G.quit; i++) {
			int j;

			if (!pfd[iw + i].revents)
				continue;
			/* still watched? (an earlier callback may have unwatched it) */
			for (j = 0; j < G.nw; j++)
				if (G.w[j].fd == snap[i].fd && G.w[j].cb == snap[i].cb)
					break;
			if (j < G.nw)
				G.w[j].cb(snap[i].fd, pfd[iw + i].revents, G.w[j].user);
		}
	}
	if (g_signal) {
		G.quit_code = 128 + g_signal;
		chefui_log("exit on signal %d", (int)g_signal);
	} else {
		chefui_log("exit (code %d)", G.quit_code);
	}
	G.be->shutdown();
	cu_buttons_close(&G.buttons);
	return G.quit_code;
}
