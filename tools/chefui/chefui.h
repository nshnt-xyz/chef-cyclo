/*
 * chefui.h - the chef-cyclo UI platform: LVGL v9.6 on the MDSS fbdev
 * (device) or an SDL2 window (host), with the screen lock, screen-off
 * flag, multitouch, buttond gestures and app handoff handled here.
 * Specification: docs/next-steps/ui-platform.md.
 *
 * A fullscreen application:
 *
 *   static bool on_button(const char *gesture, void *user) { ... }
 *
 *   int main(void)
 *   {
 *           struct chefui_config cfg = { .app_name = "ride" };
 *
 *           if (!chefui_init(&cfg))
 *                   return 1;
 *           chefui_on_button(on_button, NULL);
 *           ... build the LVGL screen ...
 *           return chefui_run();
 *   }
 *
 * The same code builds for the phone (libchefui.a, fbdev backend) and the
 * PC (libchefui-host.a, SDL backend); nothing here needs an #ifdef.
 * Everything runs on one thread: callbacks are called from chefui_run().
 */
#ifndef CHEFUI_H
#define CHEFUI_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

enum chefui_backend {
	CHEFUI_BACKEND_AUTO = 0,	/* whichever this build has */
	CHEFUI_BACKEND_FBDEV,
	CHEFUI_BACKEND_SDL,
};

struct chefui_config {
	const char *app_name;		/* kmsg prefix chefui[<app>]; required */
	uint32_t refresh_ms;		/* 0 = 16 ms (~60 fps target); 33 for ~30 fps */
	int rotation;			/* 0, 90, 180, 270 (clockwise) */
	int brightness;			/* 1..255; 0 = 96 */
	/* Extra buttond gestures to claim, NULL-terminated, e.g.
	 * { "volup.short", "voldown.short", NULL }. power.short is always
	 * claimed; power.long only if listed (buttond's power-off default
	 * otherwise keeps working); power+voldown is reserved and refused. */
	const char *const *claims;
	enum chefui_backend backend;
	float host_zoom;		/* SDL window scale; 0 = 0.4 */
	int safe_top_px;		/* physical top: 0 = calibrated default; -1 = none */
};

/* User visually calibrated physical-top clearance on 2026-10-03. */
#define CHEFUI_DEFAULT_SAFE_TOP_PX 96

/* Set up LVGL (lv_init, monotonic tick), the display, touch and buttons.
 * Returns the display, or NULL on failure (already logged). */
lv_display_t *chefui_init(const struct chefui_config *cfg);

/* Full-panel active screen for backgrounds/diagnostics; optional transparent,
 * non-scrolling child for ordinary content inside the safe rectangle.
 * Physical top rotates with the copy transform; fb/input stay full-panel.
 * Roots belong to their active screen and are deleted with it. */
lv_obj_t *chefui_root(void);
lv_obj_t *chefui_content_root(void);
lv_area_t chefui_safe_area(void); /* inclusive logical bounds */
/* Runtime calibration: 0 = calibrated default, -1 = none, otherwise physical pixels;
 * values >= physical height or below -1 fail with -EINVAL. */
int chefui_safe_top_set(int pixels); /* 0 or -errno */
int chefui_safe_top_get(void);       /* resolved physical pixels */

/* Run the poll() loop until chefui_quit() or SIGTERM/SIGINT/SIGHUP; then
 * hand the screen back (POWERDOWN, close, unlock) and return the code
 * (128 + signal for a signal). */
int chefui_run(void);
void chefui_quit(int code);

/* Application file descriptors join the same loop. events as for poll();
 * the callback gets the revents. At most 16 at a time. 0 or -errno. */
typedef void (*chefui_fd_cb)(int fd, short revents, void *user);
int chefui_watch_fd(int fd, short events, chefui_fd_cb cb, void *user);
int chefui_unwatch_fd(int fd);

/* Screen off: POWERDOWN, flag created, rendering paused (application
 * timers and fds keep running). Screen on: the reverse, then a full
 * redraw. 0 or -errno. */
int chefui_screen_set(bool on);
bool chefui_screen_is_on(void);
typedef void (*chefui_screen_cb)(bool on, void *user);
void chefui_on_screen(chefui_screen_cb cb, void *user);

/* Requested backlight 1..255 (clamped). Device requests coalesce: the
 * latest value is written/committed at the next scheduled frame, including
 * on a static page. Setter does not wait for vsync; deferred write errors
 * are logged. While off, store the level for the first screen-on frame.
 * Getter returns the requested value. */
int chefui_brightness_set(int level);
int chefui_brightness_get(void);

/* Requested ms between frames; 0 = 16 ms (~60 fps target).
 * Animation cadence follows this period too. Use 33 for ~30 fps.
 * Actual throughput depends on rendering/copy/pan. */
void chefui_refresh_period_set(uint32_t ms);

/* Button gestures ("power.short", "volup.long", ...). Return true when
 * handled; an unhandled power.short toggles the screen. */
typedef bool (*chefui_button_cb)(const char *gesture, void *user);
void chefui_on_button(chefui_button_cb cb, void *user);

/* Active contacts after each touch frame (logical coordinates). On the
 * host the mouse is the only contact. */
struct chefui_contact {
	int id;
	int32_t x, y;
};
#define CHEFUI_MAX_CONTACTS 10
typedef void (*chefui_touch_cb)(const struct chefui_contact *c, int n, void *user);
void chefui_on_touch(chefui_touch_cb cb, void *user);

/* The pointer input device (gesture events arrive on objects as
 * LV_EVENT_GESTURE; see lv_indev_get_gesture_type()). */
lv_indev_t *chefui_pointer(void);

/*
 * Replace this process with `path` (argv as for execv) without letting
 * anything else take the screen in between: POWERDOWN, close fb0, keep
 * the screen lock across exec (CHEFUI_LOCK_FD). The screen-off flag is
 * removed first, or created when dark is true. Returns only on failure
 * (-errno), with the screen restored.
 */
int chefui_handoff_exec(const char *path, char *const argv[], bool dark);

/* Lifecycle note to /dev/kmsg as "chefui[<app>]: ..." (and stderr). */
void chefui_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* With CHEFUI_STATS=1 in the environment: the last one-second stats line
 * (also printed to stderr), else "". */
const char *chefui_stats_text(void);

#ifdef __cplusplus
}
#endif

#endif /* CHEFUI_H */
