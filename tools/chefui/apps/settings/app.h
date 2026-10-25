/*
 * app.h - chef-settings, the system UI app: status bar, settings hub
 * (P01), the settings pages and placeholder pages, with the page stack
 * and screen policy. main.c wires it to chefui and the state sources;
 * the tests drive it on a headless LVGL display. Pages live in page_*.c
 * behind pages.h.
 */
#ifndef CHEF_SETTINGS_APP_H
#define CHEF_SETTINGS_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "policy.h"
#include "settings.h"
#include "widgets.h"

struct chefui_contact;

#define APP_MAX_DEPTH 4

enum app_page {
	APP_PAGE_HUB,
	APP_PAGE_DISPLAY,
	APP_PAGE_WIFI,			/* P15 off, P16 list */
	APP_PAGE_WIFI_PASSWORD,		/* P17: arg a struct wifi_net */
	APP_PAGE_WIFI_HIDDEN,		/* name, security and password */
	APP_PAGE_WIFI_DETAILS,		/* P18 */
	APP_PAGE_GPS,			/* P11 off, P12 searching, P13 locked */
	APP_PAGE_GPS_SATS,		/* P14 */
	APP_PAGE_BT,			/* P07 off, P08 sensors, P09 pairing (modal) */
	APP_PAGE_BT_SENSOR,		/* P10: arg a struct page_bt_sensor_arg */
	APP_PAGE_SOUND,
	APP_PAGE_TIME,
	APP_PAGE_STORAGE,
	APP_PAGE_ABOUT,
	APP_PAGE_PLACEHOLDER,
};

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
	int64_t als_lost_since;			/* ms the wanted light sensor went (or began) unavailable; 0 none */
	lv_timer_t *hub_timer, *als_note_timer;	/* one-shots: hub subtitle rate limit, the 3 s sensor note */
	int64_t hub_last_ms;
};

extern struct app app;

/* Build everything on root (full panel) and content (safe area). */
void app_init(const struct app_platform *pf, const struct ui_settings *set, lv_obj_t *root,
	      lv_obj_t *content);

/* Platform events. */
bool app_on_button(const char *gesture);	/* true: handled */
void app_on_touch(void);			/* any touch frame: activity */
void app_on_touch_frame(const struct chefui_contact *contacts, int n);
void app_on_screen(bool on);
/* The boot splash is gone: auto-sleep counts from now. */
void app_splash_ended(void);
void app_on_state(unsigned what);		/* sysstate.h SYS_* bits */

/* Navigation (also used by taps). arg is the page's own (copied by it). */
void app_open(enum app_page page, const char *title);
void app_open_arg(enum app_page page, const char *title, const void *arg);
bool app_back(void);				/* false on the hub */
int app_depth(void);
enum app_page app_top(void);
struct page *app_top_page(void);

/* Captures: hide what must not reach a file (typed passphrases) while
 * a snapshot is taken (begin true), and restore it after. */
void app_snapshot_guard(bool begin);

/* Type text on the open Wi-Fi password or hidden-network page's keyboard
 * (host captures and tests); false if no such page or a key is missing. */
bool page_wifi_type(const char *text);

/* Settings changes from the Display page (exposed for tests). */
void app_set_brightness(int percent);
void app_set_theme(enum ui_theme_mode mode);
void app_set_sleep(int sleep_s);
void app_set_alert_volume(int volume);
void app_set_haptics(bool enabled);
void app_set_alert_sounds(bool enabled);
void app_set_quiet_paused(bool enabled);
void app_set_clock_24h(bool enabled);
void app_set_auto_brightness(bool enabled);
/* The percent the Display page and hub show: the live Auto brightness level
 * when Auto is on and a level was applied, else the manual brightness. */
int app_shown_brightness(void);
/* Auto brightness is on but the light sensor has not answered for 3 s. */
bool app_als_unavailable(void);
/* Settings replaced from storage (late /data): applied live. */
void app_apply_settings(const struct ui_settings *set);

/* Display page widgets while it is open (tests), else NULL. */
struct display_page *app_display_page(void);
struct display_page {
	struct kit_slider slider;
	lv_obj_t *value;
	lv_obj_t *auto_switch, *auto_title, *auto_sub;
	struct kit_seg theme, sleep;
};

#endif /* CHEF_SETTINGS_APP_H */
