/*
 * app.h - chef-settings, the system UI app: status bar, settings hub
 * (P01), Display page (P02) and placeholder pages, with the page stack
 * and screen policy. main.c wires it to chefui and the state sources;
 * the tests drive it on a headless LVGL display.
 */
#ifndef CHEF_SETTINGS_APP_H
#define CHEF_SETTINGS_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "policy.h"
#include "settings.h"
#include "widgets.h"

#define APP_MAX_DEPTH 4

enum app_page { APP_PAGE_HUB, APP_PAGE_DISPLAY, APP_PAGE_PLACEHOLDER };

struct page;

/* What the app needs from the platform; main.c points these at chefui,
 * the tests at fakes. */
struct app_platform {
	int64_t (*now_ms)(void);
	bool (*screen_is_on)(void);
	int (*screen_set)(bool on);
	int (*brightness_set)(int level);
};

struct app {
	struct app_platform pf;
	struct ui_settings set;
	struct sleep_policy sleep;
	lv_timer_t *sleep_timer;
	bool persist;			/* save setting changes (ui_store) */

	lv_obj_t *root, *content;
	struct kit_statusbar sb;

	struct page *stack[APP_MAX_DEPTH];	/* [0] is the hub */
	int depth;				/* pages above the hub */

	struct kit_page hub;
	struct kit_tile tiles[3];		/* Bluetooth, GPS, Wi-Fi */
	struct kit_row row_display, row_sound, row_time, row_storage, row_about;
	char about[96];
	bool fake_state;			/* do not refresh storage from disk */
};

extern struct app app;

/* Build everything on root (full panel) and content (safe area). */
void app_init(const struct app_platform *pf, const struct ui_settings *set, lv_obj_t *root,
	      lv_obj_t *content);

/* Platform events. */
bool app_on_button(const char *gesture);	/* true: handled */
void app_on_touch(void);			/* any touch frame: activity */
void app_on_screen(bool on);
void app_on_state(unsigned what);		/* sysstate.h SYS_* bits */

/* Navigation (also used by taps). */
void app_open(enum app_page page, const char *title);
bool app_back(void);				/* false on the hub */
int app_depth(void);
enum app_page app_top(void);

/* Settings changes from the Display page (exposed for tests). */
void app_set_brightness(int percent);
void app_set_theme(enum ui_theme_mode mode);
void app_set_sleep(int sleep_s);
/* Settings replaced from storage (late /data): applied live. */
void app_apply_settings(const struct ui_settings *set);

/* Display page widgets while it is open (tests), else NULL. */
struct display_page *app_display_page(void);
struct display_page {
	struct kit_slider slider;
	lv_obj_t *value;
	lv_obj_t *auto_switch;
	struct kit_seg theme, sleep;
};

#endif /* CHEF_SETTINGS_APP_H */
