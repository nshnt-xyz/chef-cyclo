/*
 * cover.h - the full-panel progress view the boot splash (B01) and the
 * shutdown screen (B02) both draw: the title, the build line and a step
 * row with its count and bar. The layout is the design's, in device pixels
 * from 1080x2246 (sizes follow the panel height): the title block centred
 * in the panel minus a 200 px bottom padding, the progress block 120 px
 * from both sides with its bar bottom 360 px above the panel's bottom.
 *
 * The cover is a clickable object over its whole parent, so it takes every
 * touch. Its owners (splash.c, shutdown.c) decide what feeds it, when it
 * appears and when it goes. cover_log() is their shared log sink.
 */
#ifndef CHEF_SETTINGS_COVER_H
#define CHEF_SETTINGS_COVER_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

struct cover {
	lv_obj_t *obj, *text, *count, *fill;
	int bar_w;
};

/* Builds the view on PARENT. The build line is "<version> · <commit>" from
 * IMAGE_INFO (NULL: /etc/chef/image-info), or, with FROM_SYS_STATE, from the
 * state sources' device facts (a fake state, for captures); empty without
 * image facts. */
void cover_new(struct cover *c, lv_obj_t *parent, const char *image_info, bool from_sys_state);
/* The step row: TEXT (with the ellipsis U+2026 after it when ELLIPSIS),
 * the count "k / N" with k = STEP + 1 (N when STEP is N or more), and the
 * bar filled FINISHED of N. */
void cover_set(struct cover *c, const char *text, bool ellipsis, int step, int n, int finished);
/* The view removed. */
void cover_delete(struct cover *c);

/* Registers FD (NULL callback user) with the chefui loop in place of the
 * one *WATCHED holds (-1 none) when they differ: a tracker's fd can close
 * or appear. CB is called when it is readable. */
void cover_follow_fd(int *watched, int fd, void (*cb)(int fd, short revents, void *user));

/* The clocks both covers default to (steptrack.h's CLOCK_MONOTONIC and
 * CLOCK_BOOTTIME) put in whichever of *NOW and *BOOT_MS is NULL. */
void cover_clock_defaults(int64_t (**now)(void), int64_t (**boot_ms)(void));
/* The cover's one timer set to fire at AT (ms on the NOW clock; at once if
 * that has passed), or paused when AT is negative. */
void cover_timer_at(lv_timer_t *timer, int64_t at, int64_t now);
/* One log line "<PREFIX>: <WHAT> at S.CC s<EXTRA>", the time being BOOT_MS. */
void cover_log_at(const char *prefix, const char *what, int64_t boot_ms, const char *extra);

/* The log sink the splash and the shutdown screen give their trackers:
 * LOG when cover_log_to() set one, else chefui_log. */
void cover_log_to(void (*log)(const char *fmt, ...) __attribute__((format(printf, 1, 2))));
void cover_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* CHEF_SETTINGS_COVER_H */
