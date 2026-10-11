/*
 * fakeloop.h - a stand-in for the chefui loop in the system UI tests:
 * chefui_watch_fd()/chefui_unwatch_fd()/chefui_log() and a poll() loop
 * around lv_timer_handler(), with real or test-controlled LVGL time. No
 * display, no device paths.
 */
#ifndef FAKELOOP_H
#define FAKELOOP_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

/* lv_init() plus a tick: fake = test-controlled (fakeloop_advance). */
void fakeloop_init(bool fake_time);
void fakeloop_advance(uint32_t ms);	/* fake time: move and run timers */
/* Real time: run the loop until cond(arg) or timeout_ms. true if cond. */
bool fakeloop_run_until(bool (*cond)(void *), void *arg, int timeout_ms);
void fakeloop_run_for(int ms);
int fakeloop_watched(void);		/* fds currently watched */

/* chefui_log() lines are kept (last 64) for assertions. */
int fakeloop_log_count(const char *substring);
void fakeloop_log_clear(void);

/* A headless display (DIRECT, XRGB8888) for widget tests. */
lv_display_t *fakeloop_display(int w, int h);

#endif
