/*
 * app.c - chef-settings: the hub, the page stack and the screen policy
 * (see app.h; the pages are page_*.c behind pages.h).
 *
 * One LVGL screen: the status bar on the full-panel root across the safe
 * top, pages in the content root below it. The hub is built once and
 * stays; other pages are built on open and deleted on Back, so waking
 * the screen shows whatever page was open.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

#include "app.h"
#include "chefui.h"
#include "fonts.h"
#include "icons.h"
#include "pages.h"
#include "splash.h"
#include "sys.h"
#include "feedback.h"
#include "sysstate.h"
#include "text.h"

struct app app;

/* ------------------------------------------------------------ screen policy */

static void sleep_timer_cb(lv_timer_t *t);
static void als_want_sync(void);
static void als_note_arm(void);
static void notify_visible(struct page *p, bool visible);
static int touch_count;
static bool was_screen_on;

static void sleep_rearm(void)
{
	int64_t now = app.pf.now_ms(), next;
	bool on = app.pf.screen_is_on();

	if (splash_active()) {		/* its timer starts when the splash ends */
		lv_timer_pause(app.sleep_timer);
		return;
	}
	if (policy_should_sleep(&app.sleep, on, now)) {
		/* the screen callback pauses the timer */
		app.pf.screen_set(false);
		return;
	}
	next = policy_next_check_ms(&app.sleep, on, now);
	if (next < 0) {
		lv_timer_pause(app.sleep_timer);
		return;
	}
	lv_timer_set_period(app.sleep_timer, (uint32_t)next);
	lv_timer_reset(app.sleep_timer);
	lv_timer_resume(app.sleep_timer);
}

static void sleep_timer_cb(lv_timer_t *t)
{
	(void)t;
	sleep_rearm();
}

void app_on_touch(void)
{
	/* cheap: the timer re-checks the deadline when it fires */
	policy_activity(&app.sleep, app.pf.now_ms());
}

void app_on_touch_frame(const struct chefui_contact *contacts, int n)
{
	(void)contacts;
	if (splash_active())		/* swallowed: no activity, no pulse */
		return;
	app_on_touch();
	if (n > 0 && touch_count == 0 && app.set.haptics)
		sys_feedback_request();
	touch_count = n > 0 ? n : 0;
}

void app_on_screen(bool on)
{
	bool woke = on && !was_screen_on;
	was_screen_on = on;
	sys_feedback_screen_allowed(on);
	if (!on)
		touch_count = 0;
	if (on)
		policy_activity(&app.sleep, app.pf.now_ms());
	sleep_rearm();
	if (app.root)
		als_want_sync();
	if (app.root)
		notify_visible(app.stack[app.depth], on);
	if (woke && app.set.haptics)
		sys_feedback_request();
}

void app_splash_ended(void)
{
	policy_activity(&app.sleep, app.pf.now_ms());
	sleep_rearm();
}

bool app_on_button(const char *gesture)
{
	if (strcmp(gesture, "power.short"))
		return false;		/* nothing else is claimed in menus */
	if (splash_active())
		return false;		/* the default: screen off (and on again) */
	if (policy_power_short(app.pf.screen_is_on(), app.depth) == POWER_BACK) {
		/* the pulse belongs to the key: closes driven by state (a join
		 * finishing, a forgotten sensor) and the header target (whose touch
		 * already pulsed) do not buzz again */
		if (app.set.haptics)
			sys_feedback_request();
		app_back();
		policy_activity(&app.sleep, app.pf.now_ms());
		return true;
	}
	return false;			/* default: screen on/off */
}

/* ------------------------------------------------------------ settings */

static void settings_changed(void)
{
	if (app.persist)
		ui_store_changed(&app.set);
}

static void hub_rows_refresh(void);
static void hub_settings_refresh(void);
static enum kit_theme effective_theme(enum ui_theme_mode m);

/* ------------------------------------------------------------ ambient light */

/* The light sensor is wanted while the screen is on and either Auto
 * brightness or theme Auto is set (docs/features/system-ui.md). */
static void als_want_sync(void)
{
	bool want = app.pf.screen_is_on() && (app.set.auto_brightness || app.set.theme == UI_THEME_AUTO);

	if (want && !app.als_lost_since && !sys_state.als.available)
		app.als_lost_since = app.pf.now_ms() ? app.pf.now_ms() : 1;
	else if (!want)
		app.als_lost_since = 0;
	als_note_arm();
	sys_als_want(want);
}

int app_shown_brightness(void)
{
	int applied = sys_als_applied();

	return app.set.auto_brightness && applied >= 0 ? applied : app.set.brightness;
}

bool app_als_unavailable(void)
{
	return app.set.auto_brightness && app.als_lost_since && !sys_state.als.available &&
	       app.pf.now_ms() - app.als_lost_since >= 3000;
}

static void als_live_refresh(void)
{
	for (int i = 1; i <= app.depth; i++)
		if (app.stack[i]->ops->on_live)
			app.stack[i]->ops->on_live(app.stack[i]);
}

/* The hub's Display subtitle follows the live level, at most once a second,
 * with no periodic timer: a change inside the second arms a one-shot. */
static void hub_sub_refresh(void)
{
	char b[64];

	if (app.depth != 0 || !app.set.auto_brightness || !app.pf.screen_is_on())
		return;
	text_display(&app.set, sys_als_applied(), b, sizeof(b));
	if (strcmp(b, lv_label_get_text(app.row_display.sub)))	/* the label is the one truth */
		kit_row_set_sub(&app.row_display, b);
	app.hub_last_ms = app.pf.now_ms();
}

static void hub_timer_cb(lv_timer_t *t)
{
	(void)t;
	app.hub_timer = NULL;
	hub_sub_refresh();
}

static void hub_sub_changed(void)
{
	int64_t wait = app.hub_last_ms + 1000 - app.pf.now_ms();

	if (app.depth != 0 || !app.set.auto_brightness)
		return;
	if (wait <= 0) {
		hub_sub_refresh();
	} else if (!app.hub_timer) {
		app.hub_timer = lv_timer_create(hub_timer_cb, (uint32_t)wait, NULL);
		lv_timer_set_repeat_count(app.hub_timer, 1);
	}
}

/* "Light sensor not available" is shown 3 s after the sensor went: one one-shot. */
static void als_note_cb(lv_timer_t *t)
{
	(void)t;
	app.als_note_timer = NULL;
	als_live_refresh();
}

static void als_note_arm(void)
{
	if (app.als_note_timer) {
		lv_timer_delete(app.als_note_timer);
		app.als_note_timer = NULL;
	}
	if (app.als_lost_since && !sys_state.als.available) {
		int64_t wait = app.als_lost_since + 3000 - app.pf.now_ms();

		app.als_note_timer = lv_timer_create(als_note_cb, (uint32_t)(wait < 1 ? 1 : wait), NULL);
		lv_timer_set_repeat_count(app.als_note_timer, 1);
	}
}

static void als_changed(const struct als_event *ev, void *user)
{
	(void)user;
	if (ev->theme_changed && app.set.theme == UI_THEME_AUTO) {
		bool changed = kit_theme_get() != (ev->light ? KIT_THEME_LIGHT : KIT_THEME_DARK);

		kit_theme_set(ev->light ? KIT_THEME_LIGHT : KIT_THEME_DARK);
		/* every change of the effective theme under Auto, no line when it stays */
		if (changed && ev->warmup)
			chefui_log("theme auto -> %s (lux %.0f, wake grace)", ev->light ? "light" : "dark", ev->lux);
		else if (changed)
			chefui_log("theme auto -> %s (lux %.0f, held %.1f s)", ev->light ? "light" : "dark",
				   ev->lux, (double)ev->held_ms / 1000.0);
	}
	if (ev->bright_changed && app.set.auto_brightness) {
		app.pf.brightness_set(ui_brightness_level(ev->bright));
		als_live_refresh();
		hub_sub_changed();
	}
}

void app_set_auto_brightness(bool enabled)
{
	if (enabled == app.set.auto_brightness)
		return;
	app.set.auto_brightness = enabled;
	if (enabled) {
		sys_als_set_offset(app.set.auto_offset);
		sys_als_set_applied(-1);	/* applied at the first sample */
	} else {
		app.pf.brightness_set(ui_brightness_level(app.set.brightness));
	}
	als_want_sync();
	hub_settings_refresh();
	als_live_refresh();
	settings_changed();
}

void app_set_brightness(int percent)
{
	int off;

	if (percent < UI_BRIGHTNESS_MIN)
		percent = UI_BRIGHTNESS_MIN;
	if (percent > UI_BRIGHTNESS_MAX)
		percent = UI_BRIGHTNESS_MAX;
	if (app.set.auto_brightness && sys_als_offset_for(percent, &off)) {
		/* a drag on Auto: an offset on the curve, the level applies now */
		if (off != app.set.auto_offset) {
			app.set.auto_offset = off;
			sys_als_set_offset(off);
			settings_changed();
		}
		sys_als_set_applied(percent);
		app.pf.brightness_set(ui_brightness_level(percent));
		return;
	}
	if (percent == app.set.brightness)
		return;
	app.set.brightness = percent;
	/* coalesced by the platform into the next frame */
	app.pf.brightness_set(ui_brightness_level(percent));
	settings_changed();
}

static enum kit_theme effective_theme(enum ui_theme_mode m)
{
	if (m == UI_THEME_AUTO)
		return sys_als_theme_light() ? KIT_THEME_LIGHT : KIT_THEME_DARK;
	return m == UI_THEME_LIGHT ? KIT_THEME_LIGHT : KIT_THEME_DARK;
}

void app_set_theme(enum ui_theme_mode mode)
{
	if (mode == app.set.theme)
		return;
	app.set.theme = mode;
	if (mode == UI_THEME_AUTO)
		sys_als_theme_reset();		/* Dark until the sensor decides, or at once from the held lux */
	{
		enum kit_theme before = kit_theme_get(), after = effective_theme(mode);

		kit_theme_set(after);
		if (mode == UI_THEME_AUTO && before != after) {
			const char *name = after == KIT_THEME_LIGHT ? "light" : "dark";

			if (sys_state.als.available)
				chefui_log("theme auto -> %s (lux %.0f, selected)", name, sys_state.als.lux);
			else
				chefui_log("theme auto -> %s (selected, no sample yet)", name);
		}
	}
	als_want_sync();
	settings_changed();
}

void app_set_sleep(int sleep_s)
{
	if (sleep_s == app.set.sleep_s)
		return;
	app.set.sleep_s = sleep_s;
	policy_set_sleep(&app.sleep, sleep_s * 1000, app.pf.now_ms());
	sleep_rearm();
	settings_changed();
}

void app_set_alert_volume(int volume)
{
	if (volume < 0) volume = 0;
	if (volume > 100) volume = 100;
	if (volume == app.set.alert_volume) return;
	app.set.alert_volume = volume;
	settings_changed();
}

void app_set_haptics(bool enabled)
{
	if (enabled == app.set.haptics) return;
	app.set.haptics = enabled;
	sys_feedback_enabled(enabled);
	hub_settings_refresh();
	settings_changed();
}

void app_set_alert_sounds(bool enabled)
{
	if (enabled == app.set.alert_sounds) return;
	app.set.alert_sounds = enabled;
	settings_changed();
}

void app_set_quiet_paused(bool enabled)
{
	if (enabled == app.set.quiet_paused) return;
	app.set.quiet_paused = enabled;
	settings_changed();
}

void app_set_clock_24h(bool enabled)
{
	if (enabled == app.set.clock_24h) return;
	app.set.clock_24h = enabled;
	app_on_state(SYS_CLOCK);
	settings_changed();
}

/* Settings replaced from storage (a late /data): apply them live. */
void app_apply_settings(const struct ui_settings *set)
{
	bool theme_auto_new = set->theme == UI_THEME_AUTO && app.set.theme != UI_THEME_AUTO;

	app.set = *set;
	sys_feedback_enabled(set->haptics);
	sys_als_set_offset(set->auto_offset);
	if (theme_auto_new)
		sys_als_theme_reset();
	/* Auto on keeps its applied level once a sample set one (a new offset settles
	 * through the ticks); otherwise, and before any sample, the manual level */
	if (!set->auto_brightness || sys_als_applied() < 0)
		app.pf.brightness_set(ui_brightness_level(set->brightness));
	kit_theme_set(effective_theme(set->theme));
	als_want_sync();
	policy_set_sleep(&app.sleep, set->sleep_s * 1000, app.pf.now_ms());
	sleep_rearm();
	for (int i = 1; i <= app.depth; i++)
		if (app.stack[i]->ops->on_settings)
			app.stack[i]->ops->on_settings(app.stack[i]);
	if (app.depth == 0)
		hub_rows_refresh();
	else
		hub_settings_refresh();
	app_on_state(SYS_CLOCK);	/* 12/24-hour may have changed */
}

/* ------------------------------------------------------------ helpers */

lv_obj_t *page_help_text(lv_obj_t *parent, const char *s)
{
	/* padding:20px 0 0; 40 px muted */
	lv_obj_t *l = kit_text(parent, &chefkit_b400_40, 0, KIT_INK_MUT, s);
	lv_obj_t *box = lv_obj_get_parent(l);

	lv_obj_set_style_pad_top(box, 20, 0);
	lv_obj_set_height(box, 20 + kit_css_line(&chefkit_b400_40));
	return l;
}

lv_obj_t *page_info_row(lv_obj_t *parent, const char *title, const char *subtitle,
			lv_obj_t **sw_out)
{
	lv_obj_t *row = kit_box(parent), *col, *sub;

	lv_obj_set_size(row, lv_pct(100), KIT_ROW_H + KIT_LINE);
	lv_obj_add_style(row, &kit_st.rule_bottom, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 40, 0);
	col = kit_box(row);
	lv_obj_set_height(col, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(col, 1);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 6, 0);
	kit_text(col, &chefkit_b500_56, 62, KIT_INK_FG, title);
	sub = kit_text(col, &chefkit_b400_42, 0, KIT_INK_MUT, subtitle);
	if (sw_out)
		*sw_out = kit_switch_create(row);
	return sub;
}

void page_screen_size(int *w, int *h)
{
	lv_display_t *d = lv_display_get_default();

	*w = d ? lv_display_get_horizontal_resolution(d) : 0;
	*h = d ? lv_display_get_vertical_resolution(d) : 0;
}

static void back_clicked(lv_event_t *e)
{
	(void)e;
	app_back();
}

/* ------------------------------------------------------------ placeholder */

static void build_placeholder(struct page *p, const void *arg)
{
	/* the page's own slice will replace this */
	lv_obj_t *l = kit_text(p->kp.body, &chefkit_b400_42, 0, KIT_INK_MUT,
			       "Coming in a later update.");
	lv_obj_t *box = lv_obj_get_parent(l);

	(void)arg;
	lv_obj_set_style_pad_top(box, 64, 0);
	lv_obj_set_height(box, 64 + kit_css_line(&chefkit_b400_42));
}

static const struct page_ops placeholder_ops = { .build = build_placeholder };

static const struct page_ops *ops_for(enum app_page kind)
{
	switch (kind) {
	case APP_PAGE_DISPLAY: return &page_display_ops;
	case APP_PAGE_WIFI: return &page_wifi_ops;
	case APP_PAGE_WIFI_PASSWORD: return &page_wifi_password_ops;
	case APP_PAGE_WIFI_HIDDEN: return &page_wifi_hidden_ops;
	case APP_PAGE_WIFI_DETAILS: return &page_wifi_details_ops;
	case APP_PAGE_GPS: return &page_gps_ops;
	case APP_PAGE_GPS_SATS: return &page_gps_sats_ops;
	case APP_PAGE_BT: return &page_bt_ops;
	case APP_PAGE_BT_SENSOR: return &page_bt_sensor_ops;
	case APP_PAGE_SOUND: return &page_sound_ops;
	case APP_PAGE_TIME: return &page_time_ops;
	case APP_PAGE_STORAGE: return &page_storage_ops;
	case APP_PAGE_ABOUT: return &page_about_ops;
	default: return &placeholder_ops;
	}
}

/* ------------------------------------------------------------ page stack */

bool app_page_visible(const struct page *p)
{
	return p && app.stack[app.depth] == p && app.pf.screen_is_on();
}

static void notify_visible(struct page *p, bool visible)
{
	if (p && p->ops && p->ops->on_visible)
		p->ops->on_visible(p, visible);
}

static void page_deleted(lv_event_t *e)
{
	struct page *p = lv_event_get_user_data(e);

	if (p->ops->destroy)
		p->ops->destroy(p);
	free(p);
}

void app_open_arg(enum app_page kind, const char *title, const void *arg)
{
	struct page *p, *below;

	if (kind == APP_PAGE_HUB || app.depth + 1 >= APP_MAX_DEPTH)
		return;
	p = calloc(1, sizeof(*p));
	if (!p)
		return;
	p->kind = kind;
	p->ops = ops_for(kind);
	below = app.stack[app.depth];
	lv_obj_set_hidden(below->kp.obj, true);
	kit_page_create(&p->kp, app.content, title, back_clicked, NULL);
	lv_obj_add_event_cb(p->kp.obj, page_deleted, LV_EVENT_DELETE, p);
	p->ops->build(p, arg);
	app.stack[++app.depth] = p;
	notify_visible(below, false);
	notify_visible(p, app.pf.screen_is_on());
}

void app_open(enum app_page kind, const char *title)
{
	app_open_arg(kind, title, NULL);
}

bool app_back(void)
{
	struct page *p;

	if (app.depth == 0)
		return false;
	p = app.stack[app.depth];
	app.stack[app.depth--] = NULL;
	/* Hidden now, deleted after this event (Back may come from a click
	 * on the page itself); page_deleted() frees p. */
	lv_obj_set_hidden(p->kp.obj, true);
	notify_visible(p, false);
	lv_obj_delete_async(p->kp.obj);
	lv_obj_set_hidden(app.stack[app.depth]->kp.obj, false);
	if (app.depth == 0)
		hub_rows_refresh();
	notify_visible(app.stack[app.depth], app.pf.screen_is_on());
	return true;
}

int app_depth(void)
{
	return app.depth;
}

enum app_page app_top(void)
{
	return app.stack[app.depth]->kind;
}

struct page *app_top_page(void)
{
	return app.stack[app.depth];
}

void app_snapshot_guard(bool begin)
{
	for (int i = 1; i <= app.depth; i++)
		if (app.stack[i]->ops->on_snapshot)
			app.stack[i]->ops->on_snapshot(app.stack[i], begin);
}

/* ------------------------------------------------------------ hub */

static void tile_clicked(lv_event_t *e)
{
	const char *title = lv_event_get_user_data(e);

	app_open(!strcmp(title, "Wi-Fi") ? APP_PAGE_WIFI : !strcmp(title, "GPS") ? APP_PAGE_GPS
		 : !strcmp(title, "Bluetooth") ? APP_PAGE_BT : APP_PAGE_PLACEHOLDER, title);
}

static void row_clicked(lv_event_t *e)
{
	const char *title = lv_event_get_user_data(e);
	static const struct { const char *title; enum app_page page; } rows[] = {
		{ "Display", APP_PAGE_DISPLAY }, { "Sound & haptics", APP_PAGE_SOUND },
		{ "Time & date", APP_PAGE_TIME }, { "Storage", APP_PAGE_STORAGE },
		{ "About", APP_PAGE_ABOUT },
	};
	for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
		if (!strcmp(title, rows[i].title)) {
			app_open(rows[i].page, title);
			return;
		}
}

static void hub_tiles_refresh(unsigned what)
{
	const struct sys_state *s = &sys_state;
	struct kit_status st;
	char b[96];

	status_from_state(s, app.set.clock_24h, &st);
	if (what & (SYS_BT | SYS_BLE)) {
		text_bt(s, b, sizeof(b));
		kit_tile_set(&app.tiles[0], tile_bt_on(s), glyph_bt(st.bt), b);
	}
	if (what & SYS_GPS) {
		text_gps(s, b, sizeof(b));
		kit_tile_set(&app.tiles[1], tile_gps_on(s), glyph_gps(st.gps), b);
	}
	if (what & SYS_WIFI) {
		text_wifi(s, b, sizeof(b));
		kit_tile_set(&app.tiles[2], tile_wifi_on(s), glyph_wifi(st.wifi), b);
	}
	if (what & SYS_CLOCK) {
		text_time(s, app.set.clock_24h, b, sizeof(b));
		kit_row_set_sub(&app.row_time, b);
	}
	if (what & SYS_STORAGE) {
		text_storage(s, b, sizeof(b));
		kit_row_set_sub(&app.row_storage, b);
	}
}

static void hub_settings_refresh(void)
{
	char b[64];

	text_display(&app.set, sys_als_applied(), b, sizeof(b));
	kit_row_set_sub(&app.row_display, b);
	text_sound(&app.set, b, sizeof(b));
	kit_row_set_sub(&app.row_sound, b);
}

/* On hub entry: settings may have changed, storage is re-read. */
static void hub_rows_refresh(void)
{
	hub_settings_refresh();
	if (!app.fake_state)
		sys_storage_refresh();		/* asynchronous */
}

void app_on_state(unsigned what)
{
	struct kit_status st;

	if (!app.root)
		return;		/* app_init() reads the state as it builds */
	status_from_state(&sys_state, app.set.clock_24h, &st);
	kit_statusbar_set(&app.sb, &st);
	hub_tiles_refresh(what);
	if (what & SYS_ALS) {
		app.als_lost_since = sys_state.als.available || !sys_als_wanted() ? 0
			: (app.pf.now_ms() ? app.pf.now_ms() : 1);
		als_note_arm();
		als_live_refresh();
	}
	for (int i = 1; i <= app.depth; i++)
		if (app.stack[i]->ops->on_state)
			app.stack[i]->ops->on_state(app.stack[i], what);
}

static void build_hub(void)
{
	static struct page hub_page = { .kind = APP_PAGE_HUB };
	lv_obj_t *body, *grid;
	struct utsname u;

	kit_page_create(&app.hub, app.content, "Settings", NULL, NULL);
	hub_page.kp = app.hub;
	app.stack[0] = &hub_page;
	body = app.hub.body;

	kit_section(body, "CONNECTIONS", 64, 28);
	grid = kit_box(body);
	lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(grid, KIT_GAP_TILES, 0);
	kit_tile_create(&app.tiles[0], grid, KIT_IC_BLUETOOTH_OFF, "Bluetooth", tile_clicked,
			"Bluetooth");
	kit_tile_create(&app.tiles[1], grid, KIT_IC_LOCATE_OFF, "GPS", tile_clicked, "GPS");
	kit_tile_create(&app.tiles[2], grid, KIT_IC_WIFI_OFF, "Wi-Fi", tile_clicked, "Wi-Fi");
	lv_obj_update_layout(body);
	kit_tiles_fit(app.tiles, 3, lv_obj_get_content_width(body), KIT_GAP_TILES);

	kit_section(body, "DEVICE", 72, 8);
	kit_row_create(&app.row_display, body, KIT_IC_SUN_MEDIUM, "Display", "", row_clicked,
		       "Display");
	kit_row_create(&app.row_sound, body, KIT_IC_VIBRATE, "Sound & haptics", "",
		       row_clicked, "Sound & haptics");
	kit_row_create(&app.row_time, body, KIT_IC_CLOCK, "Time & date", "", row_clicked,
		       "Time & date");
	kit_row_create(&app.row_storage, body, KIT_IC_HARD_DRIVE, "Storage", "", row_clicked,
		       "Storage");
	if (app.fake_state)	/* a fixture, never the host's kernel */
		snprintf(app.about, sizeof(app.about), "%.90s",
			 sys_state.device.kernel[0] ? sys_state.device.kernel : "Linux");
	else if (uname(&u) == 0)
		snprintf(app.about, sizeof(app.about), "Linux %s", u.release);
	else
		snprintf(app.about, sizeof(app.about), "Linux");
	kit_row_create(&app.row_about, body, KIT_IC_INFO, "About", app.about, row_clicked, "About");
}

void app_init(const struct app_platform *pf, const struct ui_settings *set, lv_obj_t *root,
	      lv_obj_t *content)
{
	struct kit_status st;

	app.pf = *pf;
	was_screen_on = pf->screen_is_on();
	touch_count = 0;
	app.set = *set;
	sys_feedback_enabled(set->haptics);
	sys_feedback_screen_allowed(pf->screen_is_on());
	app.root = root;
	app.content = content;
	kit_theme_init(effective_theme(set->theme));
	lv_obj_add_style(root, &kit_st.screen, 0);
	kit_statusbar_create(&app.sb, root);
	build_hub();
	status_from_state(&sys_state, app.set.clock_24h, &st);
	kit_statusbar_set(&app.sb, &st);
	hub_tiles_refresh(~0u);
	hub_rows_refresh();
	sys_als_on_change(als_changed, NULL);
	sys_als_set_offset(set->auto_offset);
	app.sleep_timer = lv_timer_create(sleep_timer_cb, 1000, NULL);
	policy_set_sleep(&app.sleep, set->sleep_s * 1000, pf->now_ms());
	sleep_rearm();
	als_want_sync();
}
