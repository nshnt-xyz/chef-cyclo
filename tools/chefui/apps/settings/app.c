/*
 * app.c - chef-settings pages, page stack and screen policy (see app.h).
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
#include "fonts.h"
#include "icons.h"
#include "sys.h"
#include "sysstate.h"
#include "text.h"

struct app app;

struct page {
	enum app_page kind;
	struct kit_page kp;
	struct display_page *display;
};

/* ------------------------------------------------------------ screen policy */

static void sleep_timer_cb(lv_timer_t *t);

static void sleep_rearm(void)
{
	int64_t now = app.pf.now_ms(), next;
	bool on = app.pf.screen_is_on();

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

void app_on_screen(bool on)
{
	if (on)
		policy_activity(&app.sleep, app.pf.now_ms());
	sleep_rearm();
}

bool app_on_button(const char *gesture)
{
	if (strcmp(gesture, "power.short"))
		return false;		/* nothing else is claimed in menus */
	if (policy_power_short(app.pf.screen_is_on(), app.depth) == POWER_BACK) {
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
static enum kit_theme effective_theme(enum ui_theme_mode m);
static const int sleep_values[] = { 30, 60, 300, 0 };

void app_set_brightness(int percent)
{
	if (percent < UI_BRIGHTNESS_MIN)
		percent = UI_BRIGHTNESS_MIN;
	if (percent > UI_BRIGHTNESS_MAX)
		percent = UI_BRIGHTNESS_MAX;
	if (percent == app.set.brightness)
		return;
	app.set.brightness = percent;
	/* coalesced by the platform into the next frame */
	app.pf.brightness_set(ui_brightness_level(percent));
	settings_changed();
}

static enum kit_theme effective_theme(enum ui_theme_mode m)
{
	/* Auto needs the light sensor (not started at boot yet): dark */
	return m == UI_THEME_LIGHT ? KIT_THEME_LIGHT : KIT_THEME_DARK;
}

void app_set_theme(enum ui_theme_mode mode)
{
	if (mode == app.set.theme)
		return;
	app.set.theme = mode;
	kit_theme_set(effective_theme(mode));
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

/* Settings replaced from storage (a late /data): apply them live. */
void app_apply_settings(const struct ui_settings *set)
{
	struct display_page *d = app_display_page();
	int i;

	app.set = *set;
	app.pf.brightness_set(ui_brightness_level(set->brightness));
	kit_theme_set(effective_theme(set->theme));
	policy_set_sleep(&app.sleep, set->sleep_s * 1000, app.pf.now_ms());
	sleep_rearm();
	if (d) {
		char b[8];

		kit_slider_set(&d->slider, set->brightness);
		snprintf(b, sizeof(b), "%d%%", set->brightness);
		lv_label_set_text(d->value, b);
		kit_seg_set_active(&d->theme, set->theme == UI_THEME_LIGHT ? 1
				   : set->theme == UI_THEME_AUTO ? 2 : 0);
		for (i = 0; i < 4; i++)
			if (sleep_values[i] == set->sleep_s)
				kit_seg_set_active(&d->sleep, i);
	}
	if (app.depth == 0)
		hub_rows_refresh();
}

/* ------------------------------------------------------------ helpers */

static void help_text(lv_obj_t *parent, const char *s)
{
	/* padding:20px 0 0; 40 px muted */
	lv_obj_t *l = kit_text(parent, &chefkit_b400_40, 0, KIT_INK_MUT, s);
	lv_obj_t *box = lv_obj_get_parent(l);

	lv_obj_set_style_pad_top(box, 20, 0);
	lv_obj_set_height(box, 20 + kit_css_line(&chefkit_b400_40));
}

static void back_clicked(lv_event_t *e)
{
	(void)e;
	app_back();
}

/* ------------------------------------------------------------ Display page */

static void display_value_text(struct display_page *d)
{
	char b[8];

	snprintf(b, sizeof(b), "%d%%", app.set.brightness);
	lv_label_set_text(d->value, b);
}

static void slider_changed(lv_event_t *e)
{
	struct display_page *d = lv_event_get_user_data(e);

	app_set_brightness(d->slider.value);
	display_value_text(d);
}

static void theme_changed(lv_event_t *e)
{
	struct display_page *d = lv_event_get_user_data(e);

	app_set_theme(d->theme.active == 1 ? UI_THEME_LIGHT : d->theme.active == 2 ? UI_THEME_AUTO
		      : UI_THEME_DARK);
}

static void sleep_changed(lv_event_t *e)
{
	struct display_page *d = lv_event_get_user_data(e);

	if (d->sleep.active >= 0 && d->sleep.active < 4)
		app_set_sleep(sleep_values[d->sleep.active]);
}

static void build_display(struct page *p)
{
	static const char *const theme_labels[] = { "Dark", "Light", "Auto" };
	static const char *const theme_glyphs[] = { KIT_IC_MOON, KIT_IC_SUN, KIT_IC_SUN_MOON };
	static const char *const sleep_labels[] = { "30 s", "1 min", "5 min", "Never" };
	struct display_page *d = calloc(1, sizeof(*d));
	lv_obj_t *body = p->kp.body, *head, *l, *row, *col, *gap;
	int i;

	p->display = d;

	/* padding 64 0 28; label and value share a baseline (the 72 px
	 * value's: row height 72 + 0.2 em descent) */
	head = kit_box(body);
	lv_obj_set_size(head, lv_pct(100), 64 + 86 + 28);
	lv_obj_set_style_pad_top(head, 64, 0);
	l = lv_label_create(head);
	lv_label_set_text(l, "BRIGHTNESS");
	lv_obj_set_style_text_font(l, &chefkit_bc600_40, 0);
	lv_obj_set_style_text_letter_space(l, 5, 0);
	kit_set_ink(l, KIT_INK_MUT);
	lv_obj_set_y(l, kit_label_y(&chefkit_bc600_40, 72));
	d->value = lv_label_create(head);
	lv_obj_set_style_text_font(d->value, &chefkit_bc600_72, 0);
	kit_set_ink(d->value, KIT_INK_FG);
	lv_obj_align(d->value, LV_ALIGN_TOP_RIGHT, 0, kit_label_y(&chefkit_bc600_72, 72));
	display_value_text(d);

	kit_slider_create(&d->slider, body, UI_BRIGHTNESS_MIN, UI_BRIGHTNESS_MAX, app.set.brightness);
	lv_obj_add_event_cb(d->slider.obj, slider_changed, LV_EVENT_VALUE_CHANGED, d);

	/* margin-top 40; height 216 between two 3 px rules; gap 40 */
	gap = kit_box(body);
	lv_obj_set_size(gap, 1, 40);
	row = kit_box(body);
	lv_obj_set_size(row, lv_pct(100), KIT_ROW_H + 2 * KIT_LINE);
	lv_obj_add_style(row, &kit_st.rule_top_bottom, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 40, 0);
	col = kit_box(row);
	lv_obj_set_height(col, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(col, 1);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 6, 0);
	/* Auto brightness waits for sensord at boot: shown, muted, no input */
	kit_text(col, &chefkit_b500_56, 62, KIT_INK_MUT, "Auto brightness");
	kit_text(col, &chefkit_b400_42, 0, KIT_INK_MUT, "Follows the light sensor");
	d->auto_switch = kit_switch_create(row);
	kit_switch_set(d->auto_switch, false);
	lv_obj_set_disabled(d->auto_switch, true);
	lv_obj_set_clickable(d->auto_switch, false);

	kit_section(body, "THEME", 72, 28);
	kit_seg_create(&d->theme, body, 3, theme_labels, theme_glyphs);
	kit_seg_set_disabled(&d->theme, 2, true);	/* Auto: needs the light sensor */
	kit_seg_set_active(&d->theme, app.set.theme == UI_THEME_LIGHT ? 1
			   : app.set.theme == UI_THEME_AUTO ? 2 : 0);
	lv_obj_add_event_cb(d->theme.obj, theme_changed, LV_EVENT_VALUE_CHANGED, d);
	help_text(body, "Auto switches to Light in bright sun.");

	kit_section(body, "AUTO-SLEEP", 72, 28);
	kit_seg_create(&d->sleep, body, 4, sleep_labels, NULL);
	for (i = 0; i < 4; i++)
		if (sleep_values[i] == app.set.sleep_s)
			kit_seg_set_active(&d->sleep, i);
	lv_obj_add_event_cb(d->sleep.obj, sleep_changed, LV_EVENT_VALUE_CHANGED, d);
	help_text(body, "Power button wakes the screen.");
}

static void build_placeholder(struct page *p)
{
	/* the page's own slice will replace this */
	lv_obj_t *l = kit_text(p->kp.body, &chefkit_b400_42, 0, KIT_INK_MUT,
			       "Coming in a later update.");
	lv_obj_t *box = lv_obj_get_parent(l);

	lv_obj_set_style_pad_top(box, 64, 0);
	lv_obj_set_height(box, 64 + kit_css_line(&chefkit_b400_42));
}

/* ------------------------------------------------------------ page stack */

static void page_deleted(lv_event_t *e)
{
	struct page *p = lv_event_get_user_data(e);

	free(p->display);
	free(p);
}

void app_open(enum app_page kind, const char *title)
{
	struct page *p;

	if (kind == APP_PAGE_HUB || app.depth + 1 >= APP_MAX_DEPTH)
		return;
	p = calloc(1, sizeof(*p));
	if (!p)
		return;
	p->kind = kind;
	lv_obj_set_hidden(app.stack[app.depth]->kp.obj, true);
	kit_page_create(&p->kp, app.content, title, back_clicked, NULL);
	lv_obj_add_event_cb(p->kp.obj, page_deleted, LV_EVENT_DELETE, p);
	if (kind == APP_PAGE_DISPLAY)
		build_display(p);
	else
		build_placeholder(p);
	app.stack[++app.depth] = p;
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
	lv_obj_delete_async(p->kp.obj);
	lv_obj_set_hidden(app.stack[app.depth]->kp.obj, false);
	if (app.depth == 0)
		hub_rows_refresh();
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

struct display_page *app_display_page(void)
{
	struct page *p = app.stack[app.depth];

	return p && p->kind == APP_PAGE_DISPLAY ? p->display : NULL;
}

/* ------------------------------------------------------------ hub */

static void tile_clicked(lv_event_t *e)
{
	app_open(APP_PAGE_PLACEHOLDER, lv_event_get_user_data(e));
}

static void row_clicked(lv_event_t *e)
{
	const char *title = lv_event_get_user_data(e);

	app_open(strcmp(title, "Display") ? APP_PAGE_PLACEHOLDER : APP_PAGE_DISPLAY, title);
}

static void hub_tiles_refresh(unsigned what)
{
	const struct sys_state *s = &sys_state;
	struct kit_status st;
	char b[96];

	status_from_state(s, &st);
	if (what & SYS_BT) {
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
		text_time(s, b, sizeof(b));
		kit_row_set_sub(&app.row_time, b);
	}
	if (what & SYS_STORAGE) {
		text_storage(s, b, sizeof(b));
		kit_row_set_sub(&app.row_storage, b);
	}
}

/* On hub entry: settings may have changed, storage is re-read. */
static void hub_rows_refresh(void)
{
	char b[64];

	text_display(&app.set, b, sizeof(b));
	kit_row_set_sub(&app.row_display, b);
	if (!app.fake_state)
		sys_storage_refresh();		/* asynchronous */
}

void app_on_state(unsigned what)
{
	struct kit_status st;

	if (!app.root)
		return;		/* app_init() reads the state as it builds */
	status_from_state(&sys_state, &st);
	kit_statusbar_set(&app.sb, &st);
	hub_tiles_refresh(what);
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
	/* no Sound page yet: buttond's buzz is the only haptic, always on */
	kit_row_create(&app.row_sound, body, KIT_IC_VIBRATE, "Sound & haptics", "Haptics on",
		       row_clicked, "Sound & haptics");
	kit_row_create(&app.row_time, body, KIT_IC_CLOCK, "Time & date", "", row_clicked,
		       "Time & date");
	kit_row_create(&app.row_storage, body, KIT_IC_HARD_DRIVE, "Storage", "", row_clicked,
		       "Storage");
	if (uname(&u) == 0)
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
	app.set = *set;
	app.root = root;
	app.content = content;
	kit_theme_init(effective_theme(set->theme));
	lv_obj_add_style(root, &kit_st.screen, 0);
	kit_statusbar_create(&app.sb, root);
	build_hub();
	status_from_state(&sys_state, &st);
	kit_statusbar_set(&app.sb, &st);
	hub_tiles_refresh(~0u);
	hub_rows_refresh();
	app.sleep_timer = lv_timer_create(sleep_timer_cb, 1000, NULL);
	policy_set_sleep(&app.sleep, set->sleep_s * 1000, pf->now_ms());
	sleep_rearm();
}
