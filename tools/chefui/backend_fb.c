/*
 * backend_fb.c - the phone backend: LVGL on the MDSS fbdev with chefui's
 * own multitouch reader. The fb rules live in fbscreen.c, the copy in
 * copy.c, the page plan in plan.c, touch in touch.c; this file wires them
 * to LVGL.
 *
 * Pixel path: LVGL renders XRGB8888 in DIRECT mode into one cached RAM
 * shadow at the logical (rotated) size, so the shadow always holds the
 * whole current frame and LVGL never touches the write-combined fb
 * mapping. On the last flush of a frame the plan's areas (this frame's
 * plus the previous frame's, or one full copy) are copied with the R/B
 * swap and rotation into the back page, then FBIOPAN_DISPLAY flips to it
 * (blocking until vsync).
 *
 * Screen off: POWERDOWN + flag (fbscreen), every contact released and
 * touch no longer read, invalidation disabled and the refresh and
 * animation timers paused (application timers and fds keep running).
 * Screen on: flag removed, UNBLANK, backlight, touch drained and
 * resynced, both pages marked stale and the whole display invalidated.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "chefui_internal.h"
#include "copy.h"
#include "fbscreen.h"
#include "plan.h"
#include "touch.h"

#define CU_TOUCH_RESCAN_MS 1000

static struct {
	struct cu_fbscreen fb;
	struct cu_geom geom;
	struct cu_plan plan;
	uint8_t *shadow;
	size_t sstride;
	lv_display_t *disp;
	lv_indev_t *indev;
	struct cu_touch touch;
	const char *input_dir;
	int64_t next_scan_ms;
	bool rendering;		/* invalidation enabled, timers running */
	bool pan_err_logged;
	bool brightness_pending;
	bool gestures_fed;	/* the recognizers have seen a contact */
} F;

static double now_ms_f(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* Rendering follows the screen: no invalidation, refresh or animation
 * while it is off. lv_display_enable_invalidation() counts, so it is
 * only called on a real change. */
static void set_rendering(bool on)
{
	lv_timer_t *refr = lv_display_get_refr_timer(F.disp), *anim = lv_anim_get_timer();

	if (on == F.rendering)
		return;
	F.rendering = on;
	lv_display_enable_invalidation(F.disp, on);
	if (on) {
		if (anim)
			lv_timer_resume(anim);
		cu_plan_invalidate_pages(&F.plan);
		/* the active screen spans the display, so this redraws every
		 * layer; the invalidation resumes the refresh timer */
		lv_obj_invalidate(lv_display_get_screen_active(F.disp));
		/* An existing full invalidation can make LVGL return before
		 * resuming its timer (off before the very first frame). */
		if (refr)
			lv_timer_resume(refr);
	} else {
		if (refr)
			lv_timer_pause(refr);
		if (anim)
			lv_timer_pause(anim);
	}
}

/* ------------------------------------------------------------- display */

static void flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px)
{
	struct cu_rect r = { area->x1, area->y1, area->x2, area->y2 };

	(void)px;
	cu_plan_add(&F.plan, &r);
	if (lv_display_flush_is_last(d)) {
		int page = cu_plan_finish(&F.plan), i, rc;
		double t0, t1, t2;

		if (!F.fb.on || F.fb.fb.fd < 0) {
			/* rendered while off (should not happen with
			 * invalidation disabled): nothing reaches the panel */
			cu_plan_invalidate_pages(&F.plan);
			lv_display_flush_ready(d);
			return;
		}
		t0 = now_ms_f();
		for (i = 0; i < F.plan.nout; i++)
			cu_stats.px += cu_copy_area(&F.geom, cu_fb_page(&F.fb, page), cu_fb_stride(&F.fb),
						    F.shadow, F.sstride, &F.plan.out[i]);
		if (F.brightness_pending) {
			int error = cu_fb_write_backlight(&F.fb);
			F.brightness_pending = false;
			if (error)
				chefui_log("backlight write failed: %s", strerror(-error));
		}
		t1 = now_ms_f();
		rc = cu_fb_pan(&F.fb, cu_plan_pan_yoffset(&F.plan, page), page);
		t2 = now_ms_f();
		if (rc == 0) {
			cu_plan_flipped(&F.plan);
		} else {
			cu_plan_pan_failed(&F.plan);
			if (!F.pan_err_logged) {
				chefui_log("FBIOPAN_DISPLAY page %d: %s", page, strerror(-rc));
				F.pan_err_logged = true;
			}
		}
		cu_stats.frames++;
		cu_stats.copy_ms += t1 - t0;
		cu_stats.pan_ms += t2 - t1;
		if (t2 - t1 > cu_stats.pan_max_ms)
			cu_stats.pan_max_ms = t2 - t1;
	}
	lv_display_flush_ready(d);
}

/* --------------------------------------------------------------- touch */

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
	struct cu_touch_state st;
	bool popped = cu_touch_pop(&F.touch, &st);
	int i;

	if (popped && st.nc > 0) {
		lv_indev_touch_data_t td[CU_TOUCH_SLOTS];
		uint32_t now = lv_tick_get();

		for (i = 0; i < st.nc; i++) {
			td[i].point.x = st.c[i].x;
			td[i].point.y = st.c[i].y;
			td[i].state = st.c[i].down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
			td[i].id = (uint8_t)st.c[i].slot;
			td[i].timestamp = now;
		}
		lv_indev_gesture_recognizers_update(indev, td, (uint16_t)st.nc);
		F.gestures_fed = true;
	}
	/* Every read: LVGL zeroes data first, so the recognizer state must be
	 * copied in again or an ongoing gesture would look ended. Only once
	 * the recognizers have seen a contact (before that their state is
	 * unallocated). It also sets data->state from the recognizers, which
	 * the primary pointer overrides below, as lv_evdev does. */
	if (F.gestures_fed)
		lv_indev_gesture_recognizers_set_data(indev, data);
	data->point.x = st.x;
	data->point.y = st.y;
	data->state = st.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
	/* honoured by LVGL only outside EVENT mode; the loop in
	 * fb_handle_pollfds() re-reads while frames are queued instead */
	data->continue_reading = st.more;

	if (popped) {
		struct chefui_contact c[CHEFUI_MAX_CONTACTS];
		int n = 0;

		for (i = 0; i < st.nc && n < CHEFUI_MAX_CONTACTS; i++)
			if (st.c[i].down)
				c[n++] = (struct chefui_contact){ st.c[i].id, st.c[i].x, st.c[i].y };
		cu_notify_touch(c, n);
	}
}

static void touch_scan(void)
{
	int r = cu_touch_open_scan(&F.touch, F.input_dir);

	F.next_scan_ms = cu_now_ms() + CU_TOUCH_RESCAN_MS;
	if (r == 0)
		chefui_log("touch %s: x %d..%d y %d..%d, %d slots", F.touch.path, F.touch.ax.minimum,
			   F.touch.ax.maximum, F.touch.ay.minimum, F.touch.ay.maximum, F.touch.nslots);
}

/*
 * Release every contact so no object stays pressed. Outside input
 * processing the all-up frame is read at once. Inside it (an event handler
 * turned the screen off, or a failed handoff from a click) a nested
 * lv_indev_read() would deliver the release twice and leave LVGL's outer
 * read with indev_act == NULL: instead the indev is reset (the outer read
 * stops, the object gets PRESS_LOST) and the queued frame is read by the
 * main loop, even with the screen off (fb_timeout_ms/fb_handle_pollfds).
 */
static void release_contacts(void)
{
	cu_touch_release_all(&F.touch);
	if (lv_indev_active() == NULL)
		lv_indev_read(F.indev);
	else
		lv_indev_reset(F.indev, NULL);
}

static void touch_lost(int err)
{
	chefui_log("touch %s lost (%s); rescanning every %d ms", F.touch.path, strerror(-err),
		   CU_TOUCH_RESCAN_MS);
	cu_touch_close(&F.touch);
	release_contacts();
	F.next_scan_ms = cu_now_ms() + CU_TOUCH_RESCAN_MS;
}

/* ------------------------------------------------------------- backend */

static int fb_screen_set(bool on)
{
	double t0 = now_ms_f();
	int r;

	if (on == F.fb.on)
		return 0;
	if (!on) {
		r = cu_fb_screen_off(&F.fb);
		F.brightness_pending = false;
		release_contacts();
		set_rendering(false);
		chefui_log("screen off (%.0f ms)", now_ms_f() - t0);
	} else {
		r = cu_fb_screen_on(&F.fb);
		if (r) {
			chefui_log("screen on failed: %s", strerror(-r));
			return r;
		}
		if (F.touch.fd >= 0)
			cu_touch_wake(&F.touch);
		set_rendering(true);
		chefui_log("screen on (%.0f ms)", now_ms_f() - t0);
	}
	cu_notify_screen(on);
	return r;
}

static bool fb_screen_is_on(void)
{
	return F.fb.on;
}

static int fb_brightness_set(int level)
{
	int clamped = level < 1 ? 1 : level > 255 ? 255 : level;
	lv_area_t pixel = { 0, 0, 0, 0 };

	if (clamped == F.fb.brightness)
		return 0;
	F.fb.brightness = clamped;
	if (F.fb.on) {
		/* Coalesce all input reports into the next scheduled frame. A
		 * one-pixel invalidation guarantees a brightness-only commit
		 * without an extra vsync pan in each input callback. */
		F.brightness_pending = true;
		lv_obj_invalidate_area(lv_display_get_screen_active(F.disp), &pixel);
	}
	return 0;
}

static int fb_brightness_get(void)
{
	return F.fb.brightness;
}

static int fb_init(const struct chefui_config *cfg, lv_display_t **disp, lv_indev_t **pointer)
{
	bool dark = false;
	int r;

	memset(&F, 0, sizeof(F));
	cu_fb_init(&F.fb, chefui_log);
	if (cu_paths.fb) F.fb.p.fb = cu_paths.fb;
	if (cu_paths.lock) F.fb.p.lock = cu_paths.lock;
	if (cu_paths.flag) F.fb.p.flag = cu_paths.flag;
	if (cu_paths.backlight) F.fb.p.backlight = cu_paths.backlight;
	if (cu_paths.modes) F.fb.p.modes = cu_paths.modes;
	F.input_dir = cu_paths.input_dir ? cu_paths.input_dir : "/dev/input";
	F.touch.fd = -1;
	if (cfg->rotation != 0 && cfg->rotation != 90 && cfg->rotation != 180 && cfg->rotation != 270) {
		chefui_log("rotation %d: must be 0, 90, 180 or 270", cfg->rotation);
		return -EINVAL;
	}
	r = cu_fb_start(&F.fb, cfg->brightness, &dark);
	if (r)
		return r;
	cu_geom_init(&F.geom, cfg->rotation, (int32_t)F.fb.xres, (int32_t)F.fb.yres);

	F.disp = lv_display_create(F.geom.lw, F.geom.lh);
	if (!F.disp)
		return -ENOMEM;
	lv_display_set_color_format(F.disp, LV_COLOR_FORMAT_XRGB8888);
	F.sstride = (size_t)F.geom.lw * 4;
	F.shadow = aligned_alloc(64, (F.sstride * (size_t)F.geom.lh + 63) & ~(size_t)63);
	if (!F.shadow)
		return -ENOMEM;
	memset(F.shadow, 0, F.sstride * (size_t)F.geom.lh);
	lv_display_set_buffers_with_stride(F.disp, F.shadow, NULL,
					   (uint32_t)(F.sstride * (size_t)F.geom.lh), (uint32_t)F.sstride,
					   LV_DISPLAY_RENDER_MODE_DIRECT);
	lv_display_set_flush_cb(F.disp, flush_cb);
	lv_timer_set_period(lv_display_get_refr_timer(F.disp), cu_refresh_period());
	cu_plan_init(&F.plan, F.geom.lw, F.geom.lh, F.fb.yres);
	F.rendering = true;

	cu_touch_init(&F.touch, &F.geom);
	touch_scan();
	if (F.touch.fd < 0)
		chefui_log("no touchscreen under %s yet; rescanning every %d ms", F.input_dir,
			   CU_TOUCH_RESCAN_MS);
	F.indev = lv_indev_create();
	lv_indev_set_type(F.indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(F.indev, touch_read_cb);
	lv_indev_set_display(F.indev, F.disp);
	lv_indev_set_mode(F.indev, LV_INDEV_MODE_EVENT);
	/* LVGL 9.6's rotation recognizer forgets to initialize its default
	 * threshold, leaving zero: tiny angle noise steals pinch/swipe. */
	lv_indev_set_rotation_rad_threshold(F.indev,
		LV_INDEV_DEF_GESTURE_ROTATION_THRESHOLD / 1000.0f);

	if (dark)
		set_rendering(false);
	chefui_log("fbdev %ux%u, logical %dx%d, screen %s", F.fb.xres, F.fb.yres, (int)F.geom.lw,
		   (int)F.geom.lh, dark ? "off (flag present)" : "on");
	*disp = F.disp;
	*pointer = F.indev;
	return 0;
}

static int fb_handoff_exec(const char *path, char *const argv[], bool dark)
{
	int r;

	cu_touch_close(&F.touch);
	r = cu_fb_handoff_exec(&F.fb, path, argv, dark);
	/* only on failure: the screen is back as it was */
	touch_scan();
	release_contacts();
	if (F.rendering && F.fb.on) {
		cu_plan_invalidate_pages(&F.plan);
		lv_obj_invalidate(lv_display_get_screen_active(F.disp));
	}
	set_rendering(F.fb.on);
	return r;
}

static void fb_shutdown(void)
{
	cu_touch_close(&F.touch);
	cu_fb_shutdown(&F.fb);
	chefui_log("screen released");
}

static int fb_add_pollfds(struct pollfd *pfd, int max)
{
	if (max < 1 || !F.fb.on || F.touch.fd < 0)
		return 0;
	pfd[0] = (struct pollfd){ F.touch.fd, POLLIN, 0 };
	return 1;
}

static int fb_timeout_ms(int t)
{
	int lim = -1;

	if (!F.fb.on) {
		lv_timer_t *anim = lv_anim_get_timer();

		/* lv_anim resumes its own timer whenever an animation starts
		 * or ends; keep it paused while dark (best effort: one wakeup
		 * per such change, completions wait for screen-on) */
		if (anim)
			lv_timer_pause(anim);
		/* a release queued from inside an event handler */
		return F.touch.qcount > 0 ? 0 : t;
	}
	if (cu_touch_any_down(&F.touch))
		lim = (int)cu_refresh_period();	/* long-press, scroll, release timing */
	if (F.touch.fd < 0) {
		int64_t s = F.next_scan_ms - cu_now_ms();
		int st = s < 0 ? 0 : (int)s;

		if (lim < 0 || st < lim)
			lim = st;
	}
	if (lim >= 0 && (t < 0 || lim < t))
		t = lim;
	return t;
}

static void fb_handle_pollfds(const struct pollfd *pfd, int n)
{
	bool readable = n > 0 && pfd[0].revents != 0;

	if (!F.fb.on) {
		/* only the all-up frame queued by release_contacts() */
		while (F.touch.qcount > 0) {
			unsigned before = F.touch.qcount;

			lv_indev_read(F.indev);
			if (F.touch.qcount >= before)
				break;
		}
		return;
	}
	if (readable) {
		int r = cu_touch_read_fd(&F.touch);

		if (r < 0 || (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL))) {
			touch_lost(r < 0 ? r : -ENODEV);
			return;
		}
	}
	if (readable || cu_touch_any_down(&F.touch)) {
		/* one frame per read; stop if a read consumed nothing (LVGL
		 * skips input during a screen-load animation): the frames stay
		 * queued and the next refresh period retries */
		unsigned before;

		do {
			before = F.touch.qcount;
			lv_indev_read(F.indev);
		} while (F.touch.qcount > 0 && F.touch.qcount < before && F.fb.on);
	}
	if (F.touch.fd < 0 && cu_now_ms() >= F.next_scan_ms) {
		touch_scan();
		if (F.touch.fd >= 0)
			cu_touch_wake(&F.touch);
	}
}

const struct cu_backend cu_backend_fbdev = {
	.name = "fbdev",
	.kind = CHEFUI_BACKEND_FBDEV,
	.uses_buttond = true,
	.init = fb_init,
	.screen_set = fb_screen_set,
	.screen_is_on = fb_screen_is_on,
	.brightness_set = fb_brightness_set,
	.brightness_get = fb_brightness_get,
	.handoff_exec = fb_handoff_exec,
	.shutdown = fb_shutdown,
	.add_pollfds = fb_add_pollfds,
	.timeout_ms = fb_timeout_ms,
	.handle_pollfds = fb_handle_pollfds,
};
