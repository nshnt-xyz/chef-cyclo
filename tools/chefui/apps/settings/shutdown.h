/*
 * shutdown.h - the shutdown screen (B02, docs/next-steps/shutdown-screen-handoff.md):
 * the splash's cover (cover.h) above everything chef-settings draws,
 * shown for as long as /run/chef/shutting-down exists, with the six steps
 * of sys/shutsteps. It watches the run directory for the marker for the
 * whole life of the app, and starts at once when the marker is there.
 *
 * While it shows: touch is swallowed, a short Power press is ignored (the
 * screen stays lit) and auto-sleep does not run (app.c asks
 * shutdown_active()). It lights the panel if the screen is off, and ends a
 * running boot splash. The marker going away (a chef-reboot that gave up)
 * takes it down; so does shutdown_stop() (TERM).
 *
 * A frozen screen (cfg.frozen 0..6, the fake-state `shutdown K REASON`
 * key) shows K steps finished (5 and 6: step 6 shown, bar full) with no
 * sources, watches, timers or log lines, for captures.
 */
#ifndef CHEF_SETTINGS_SHUTDOWN_H
#define CHEF_SETTINGS_SHUTDOWN_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "shutsteps.h"

struct shutdown_config {
	lv_obj_t *parent;		/* lv_layer_sys(): nothing drawn later covers it */
	const char *openrc, *run;	/* shutsteps' roots and the marker's, NULL: defaults */
	const char *image_info;		/* NULL: /etc/chef/image-info */
	int frozen;			/* -1 live, 0..6: frozen with this many steps finished */
	enum shutsteps_reason frozen_reason;
	int64_t (*now)(void);		/* ms clock (also shutsteps'), NULL: CLOCK_MONOTONIC */
	int64_t (*boot_ms)(void);	/* ms since boot for the log, NULL: CLOCK_BOOTTIME */
	void (*log)(const char *fmt, ...) __attribute__((format(printf, 1, 2)));	/* NULL: chefui_log */
	bool (*screen_is_on)(void);
	int (*screen_set)(bool on);	/* the app's screen-on path */
	void (*changed)(void);		/* the screen came or went (not for a frozen one) */
};

/* Starts watching for the marker (and shows at once if it is there).
 * Registers its fds and timer with the chefui loop. False if already
 * started. */
bool shutdown_start(const struct shutdown_config *cfg);
/* The screen is showing. */
bool shutdown_active(void);
/* Stops watching and removes the screen (logged when it showed). */
void shutdown_stop(void);

#endif /* CHEF_SETTINGS_SHUTDOWN_H */
