/*
 * backend_sdl.c - the PC backend: the same application code in an SDL2
 * window through LVGL's SDL driver. No device paths: no screen lock, no
 * fb0, no buttond, no evdev.
 *
 *   display  the logical (rotated) resolution, scaled by host_zoom
 *            (default 0.4, CHEFUI_ZOOM=... overrides); the mouse is the
 *            single touch pointer
 *   buttons  P / Shift+P / Ctrl+P = power.short / .long / .double,
 *            Up / Down = volup.short / voldown.short, Shift+Up/Down =
 *            .long; delivered through the same dispatch as buttond's
 *            (application first, then the power.short screen toggle)
 *   screen   off blanks the window and pauses rendering like the phone
 *   handoff  plain execv
 *
 * LVGL's SDL event handler would delete the display on a window close
 * and call lv_deinit() on SDL_QUIT; an SDL event filter catches both
 * first and turns them into chefui_quit(0), and swallows expose events
 * while the screen is off so the window stays black.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "chefui_internal.h"	/* lvgl.h -> lv_conf.h: LV_SDL_INCLUDE_PATH */
#include LV_SDL_INCLUDE_PATH

#define KEYQ 16

static struct {
	lv_display_t *disp;
	lv_indev_t *mouse;
	lv_indev_read_cb_t mouse_read;
	bool on, rendering;
	int brightness;
	char app[32];
	char title[96];
	const char *keyq[KEYQ];
	unsigned kq_head, kq_n;
	bool quit_req;
	bool last_pressed;
	lv_point_t last_point;
} S;

static void key_push(const char *g)
{
	if (S.kq_n == KEYQ)
		return;
	S.keyq[(S.kq_head + S.kq_n++) % KEYQ] = g;
}

/* Runs inside SDL_PollEvent (from LVGL's SDL timer), main thread. */
static int event_filter(void *user, SDL_Event *e)
{
	(void)user;
	if (e->type == SDL_QUIT ||
	    (e->type == SDL_WINDOWEVENT && e->window.event == SDL_WINDOWEVENT_CLOSE)) {
		S.quit_req = true;
		return 0;
	}
	if (e->type == SDL_WINDOWEVENT && !S.on &&
	    (e->window.event == SDL_WINDOWEVENT_EXPOSED
#if SDL_VERSION_ATLEAST(2, 0, 5)
	     || e->window.event == SDL_WINDOWEVENT_TAKE_FOCUS
#endif
	    ))
		return 0;
	if (e->type == SDL_KEYDOWN && !e->key.repeat) {
		Uint16 mod = e->key.keysym.mod;
		bool shift = (mod & KMOD_SHIFT) != 0, ctrl = (mod & KMOD_CTRL) != 0;

		switch (e->key.keysym.sym) {
		case SDLK_p:
			key_push(ctrl ? "power.double" : shift ? "power.long" : "power.short");
			break;
		case SDLK_UP:
			key_push(shift ? "volup.long" : "volup.short");
			break;
		case SDLK_DOWN:
			key_push(shift ? "voldown.long" : "voldown.short");
			break;
		default:
			break;
		}
	}
	return 1;
}

static void update_title(void)
{
	snprintf(S.title, sizeof(S.title), "%s%s (brightness %d)",
		 S.on ? "" : "[off] ", S.app, S.brightness);
	lv_sdl_window_set_title(S.disp, S.title);
}

/* The mouse as the one contact, reported like a touch frame. */
static void mouse_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
	bool pressed;

	S.mouse_read(indev, data);
	pressed = data->state == LV_INDEV_STATE_PRESSED;
	if (pressed != S.last_pressed ||
	    (pressed && (data->point.x != S.last_point.x || data->point.y != S.last_point.y))) {
		struct chefui_contact c = { 0, data->point.x, data->point.y };

		S.last_pressed = pressed;
		S.last_point = data->point;
		cu_notify_touch(&c, pressed ? 1 : 0);
	}
}

static void refr_ready_cb(lv_event_t *e)
{
	(void)e;
	cu_stats.frames++;
}

static void blank_window(void)
{
	SDL_Renderer *r = lv_sdl_window_get_renderer(S.disp);

	if (!r)
		return;
	SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
	SDL_RenderClear(r);
	SDL_RenderPresent(r);
}

static void set_rendering(bool on)
{
	lv_timer_t *refr = lv_display_get_refr_timer(S.disp), *anim = lv_anim_get_timer();

	if (on == S.rendering)
		return;
	S.rendering = on;
	lv_display_enable_invalidation(S.disp, on);
	lv_indev_enable(S.mouse, on);
	if (on) {
		if (anim)
			lv_timer_resume(anim);
		lv_obj_invalidate(lv_display_get_screen_active(S.disp));
		/* A full invalidation queued before screen-off can be deduped
		 * before LVGL emits REFR_REQUEST. Resume explicitly like fbdev. */
		if (refr)
			lv_timer_resume(refr);
	} else {
		if (refr)
			lv_timer_pause(refr);
		if (anim)
			lv_timer_pause(anim);
	}
}

static int sdl_init(const struct chefui_config *cfg, lv_display_t **disp, lv_indev_t **pointer)
{
	int32_t w = 1080, h = 2246;

	memset(&S, 0, sizeof(S));
	snprintf(S.app, sizeof(S.app), "%s", cfg->app_name);
	if (cfg->rotation == 90 || cfg->rotation == 270) {
		w = 2246;
		h = 1080;
	} else if (cfg->rotation != 0 && cfg->rotation != 180) {
		chefui_log("rotation %d: must be 0, 90, 180 or 270", cfg->rotation);
		return -EINVAL;
	}
	S.disp = lv_sdl_window_create(w, h);
	if (!S.disp)
		return -ENODEV;
	{
		const char *z = getenv("CHEFUI_ZOOM");	/* override without a rebuild */
		float zoom = z ? strtof(z, NULL) : 0;

		lv_sdl_window_set_zoom(S.disp, zoom > 0.05f && zoom <= 4 ? zoom : cfg->host_zoom);
	}
	lv_timer_set_period(lv_display_get_refr_timer(S.disp), cu_refresh_period());
	lv_display_add_event_cb(S.disp, refr_ready_cb, LV_EVENT_RENDER_READY, NULL);
	SDL_SetEventFilter(event_filter, NULL);
	S.mouse = lv_sdl_mouse_create();
	S.mouse_read = lv_indev_get_read_cb(S.mouse);
	lv_indev_set_read_cb(S.mouse, mouse_read_cb);
	S.on = S.rendering = true;
	S.brightness = cfg->brightness;
	update_title();
	chefui_log("SDL window %dx%d at zoom %.2f (P: power, Up/Down: volume, Shift: long, Ctrl+P: double)",
		   (int)w, (int)h, (double)cfg->host_zoom);
	*disp = S.disp;
	*pointer = S.mouse;
	return 0;
}

static int sdl_screen_set(bool on)
{
	if (on == S.on)
		return 0;
	S.on = on;
	set_rendering(on);
	if (!on)
		blank_window();
	update_title();
	chefui_log("screen %s", on ? "on" : "off");
	cu_notify_screen(on);
	return 0;
}

static bool sdl_screen_is_on(void)
{
	return S.on;
}

static int sdl_brightness_set(int level)
{
	S.brightness = level < 1 ? 1 : level > 255 ? 255 : level;
	update_title();
	return 0;
}

static int sdl_brightness_get(void)
{
	return S.brightness;
}

static int sdl_handoff_exec(const char *path, char *const argv[], bool dark)
{
	int e;

	chefui_log("handoff to %s%s", path, dark ? " (dark: ignored on the host)" : "");
	execv(path, argv);
	e = errno;
	chefui_log("handoff to %s failed: %s", path, strerror(e));
	return -e;
}

static void sdl_shutdown(void)
{
}

static int sdl_add_pollfds(struct pollfd *pfd, int max)
{
	(void)pfd;
	(void)max;
	return 0;
}

static int sdl_timeout_ms(int t)
{
	/* LVGL's SDL event timer runs every 5 ms; never sleep past it */
	return t < 0 || t > 5 ? 5 : t;
}

static void sdl_handle_pollfds(const struct pollfd *pfd, int n)
{
	(void)pfd;
	(void)n;
	while (S.kq_n) {
		const char *g = S.keyq[S.kq_head];

		S.kq_head = (S.kq_head + 1) % KEYQ;
		S.kq_n--;
		cu_dispatch_gesture(g);
	}
	if (S.quit_req) {
		S.quit_req = false;
		chefui_quit(0);
	}
}

const struct cu_backend cu_backend_sdl = {
	.name = "sdl",
	.kind = CHEFUI_BACKEND_SDL,
	.uses_buttond = false,
	.init = sdl_init,
	.screen_set = sdl_screen_set,
	.screen_is_on = sdl_screen_is_on,
	.brightness_set = sdl_brightness_set,
	.brightness_get = sdl_brightness_get,
	.handoff_exec = sdl_handoff_exec,
	.shutdown = sdl_shutdown,
	.add_pollfds = sdl_add_pollfds,
	.timeout_ms = sdl_timeout_ms,
	.handle_pollfds = sdl_handle_pollfds,
};
