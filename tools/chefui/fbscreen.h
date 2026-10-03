/*
 * fbscreen.h - fb0 ownership and screen state for the device backend:
 * the screen lock, /run/fblog.off, FBIOBLANK, the page mapping, pans,
 * the backlight and exec handoff. No LVGL: covered by
 * tests/test_fbscreen.c with a wrapped ioctl() and temporary paths.
 *
 * Contract (tools/fbdev.h, docs/features/display-and-touch.md):
 *  - LOCK_EX on /run/fb0.lock before fb0 is opened, held until after the
 *    final POWERDOWN + close; or adopt the lock fd a previous chefui
 *    process handed over in CHEFUI_LOCK_FD (no unlocked gap, so fblog
 *    cannot resume in between).
 *  - open, FBIOBLANK UNBLANK, mmap; never FBIOPUT_VSCREENINFO (a format
 *    change power-cycles the panel outside the fb notifier and kills
 *    touch); require 32 bpp, R/G/B at 0/8/16, two pages.
 *  - the mapping is write-combined: it is only ever written.
 *  - a backlight write lands at the next pan (commit).
 *  - FBIOBLANK POWERDOWN before every close.
 *
 * /run/fblog.off is the system-wide "screen deliberately off" flag:
 *   off:   POWERDOWN, then create the flag
 *   on:    remove the flag, UNBLANK (or the first open), backlight
 *   start with the flag present: lock taken, fb0 not opened (the first
 *          open would light the panel) until the first screen-on
 *   exit:  POWERDOWN, close, unlock; the flag is left as it is
 *   handoff: flag removed (or created for a dark handoff), POWERDOWN,
 *          close, lock fd kept open across exec in CHEFUI_LOCK_FD
 */
#ifndef CHEFUI_FBSCREEN_H
#define CHEFUI_FBSCREEN_H

#include <stdbool.h>
#include <stdint.h>

#include "../fbdev.h"

#define CU_FB_PATH       "/dev/fb0"
#define CU_FB_MODES_PATH "/sys/class/graphics/fb0/modes"
#define CU_FB_FLAG_PATH  "/run/fblog.off"
#define CU_FB_BL_PATH    "/sys/class/leds/lcd-backlight/brightness"
#define CU_FB_LOCK_ENV   "CHEFUI_LOCK_FD"
#define CU_FB_LOCK_WAIT_MS 10000

typedef void (*cu_fb_log_fn)(const char *fmt, ...);

struct cu_fb_paths {
	const char *fb, *lock, *flag, *backlight, *modes;
};

struct cu_fbscreen {
	struct cu_fb_paths p;
	cu_fb_log_fn log;
	int lockfd;
	bool lock_inherited;
	struct fbdev fb;		/* fb.fd < 0 while closed */
	bool on;
	int brightness;			/* 1..255 */
	uint32_t xres, yres;		/* expected/validated visible size */
	int front;			/* page currently panned to */
};

/* Default paths, nothing opened. */
void cu_fb_init(struct cu_fbscreen *s, cu_fb_log_fn log);

/* "U:1080x2246p-60" (first line of the sysfs modes file) -> 1080, 2246. */
int cu_fb_parse_mode(const char *text, uint32_t *xres, uint32_t *yres);

/* Take the lock (or adopt CHEFUI_LOCK_FD, then unset it), then open the
 * screen unless the flag exists. 0 or -errno; on failure nothing is
 * held. *dark tells whether it started with the screen off. */
int cu_fb_start(struct cu_fbscreen *s, int brightness, bool *dark);

/* Open fb0 + check the layout + UNBLANK + map; used by start and by the
 * first screen-on after a dark start. */
int cu_fb_open(struct cu_fbscreen *s);

/* Visible page `page` in the mapping (write only!). */
uint8_t *cu_fb_page(struct cu_fbscreen *s, int page);
uint32_t cu_fb_stride(const struct cu_fbscreen *s);

/* FBIOPAN_DISPLAY to yoffset (blocks until vsync); 0 or -errno. */
int cu_fb_pan(struct cu_fbscreen *s, uint32_t yoffset, int page);

/* Screen off: POWERDOWN, then create the flag. 0 or -errno. */
int cu_fb_screen_off(struct cu_fbscreen *s);

/* Screen on: remove the flag, UNBLANK (opening fb0 if it never was),
 * write the backlight. The caller then redraws everything; the commit of
 * that frame applies the backlight. 0 or -errno (screen stays off). */
int cu_fb_screen_on(struct cu_fbscreen *s);

/* Write the stored brightness without a commit; next frame pan applies it. */
int cu_fb_write_backlight(struct cu_fbscreen *s);

/* Write the backlight (clamped 1..255) and commit by panning to the
 * current front page when the screen is on. 0 or -errno. */
int cu_fb_brightness_set(struct cu_fbscreen *s, int level);

/* POWERDOWN (if open), unmap, close fb0, release the lock. Leaves the
 * flag alone. Async-signal-safe enough for a fatal path: only syscalls. */
void cu_fb_shutdown(struct cu_fbscreen *s);

/*
 * Hand the screen to `path`: remove the flag (or create it when dark),
 * POWERDOWN, close fb0, clear CLOEXEC on the lock fd, export
 * CHEFUI_LOCK_FD and execv. Returns only on failure (-errno), with the
 * lock fd CLOEXEC again, the variable unset and the screen restored to
 * its previous state.
 */
int cu_fb_handoff_exec(struct cu_fbscreen *s, const char *path, char *const argv[], bool dark);

#endif /* CHEFUI_FBSCREEN_H */
