/*
 * test_app.c - the system UI kit and app on a headless LVGL display with
 * test-controlled time: generated fonts (tabular digits, icon glyphs, CSS
 * metrics), theme switching in place, the status bar mapping, tile
 * widths, hub subtitles, the page stack, the power-key policy and
 * auto-sleep, and Display page settings.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "check.h"
#include "fakeloop.h"
#include "fonts.h"
#include "icons.h"
#include "policy.h"
#include "sys.h"
#include "sysstate.h"
#include "text.h"
#include "theme.h"
#include "widgets.h"
#include "wifi.h"
#include "wifi_stub.h"
#include "pages.h"

/* ---------------------------------------------------- fake platform */

static int64_t t_now = 100000;
static bool screen_on = true;
static int screen_sets, last_level = -1;

static int64_t pf_now(void)
{
	return t_now;
}

static bool pf_on(void)
{
	return screen_on;
}

static int pf_set(bool on)
{
	screen_sets++;
	if (on != screen_on) {
		screen_on = on;
		app_on_screen(on);
	}
	return 0;
}

static int pf_bright(int level)
{
	last_level = level;
	return 0;
}

/* advance both clocks (the platform's and LVGL's) */
static void advance(int ms)
{
	while (ms > 0) {
		int step = ms > 10 ? 10 : ms;

		t_now += step;
		fakeloop_advance((uint32_t)step);
		ms -= step;
	}
}

static uint32_t colour(lv_obj_t *o)
{
	return lv_color_to_u32(lv_obj_get_style_text_color(o, 0)) & 0xffffff;
}

static uint32_t bg(lv_obj_t *o)
{
	return lv_color_to_u32(lv_obj_get_style_bg_color(o, 0)) & 0xffffff;
}

static uint32_t border(lv_obj_t *o)
{
	return lv_color_to_u32(lv_obj_get_style_border_color(o, 0)) & 0xffffff;
}

/* ------------------------------------------------------------- fonts */

static void test_fonts(void)
{
	const lv_font_t *text[] = { &chefkit_b400_38, &chefkit_b400_40, &chefkit_b400_42,
		&chefkit_b400_46, &chefkit_b400_48, &chefkit_b400_72, &chefkit_b500_42,
		&chefkit_b500_48, &chefkit_b500_52, &chefkit_b500_54, &chefkit_b500_56,
		&chefkit_b500_60, &chefkit_b500_64, &chefkit_bc600_32, &chefkit_bc600_40,
		&chefkit_bc600_44, &chefkit_bc600_60, &chefkit_bc600_72, &chefkit_bc600_80,
		&chefkit_bc600_96 };
	static const struct { const lv_font_t *f; const char *glyphs[16]; } icons[] = {
		{ &chefkit_icon_24, { KIT_IC_ZAP } },
		{ &chefkit_icon_36, { KIT_IC_LOCATE, KIT_IC_LOCATE_FIXED, KIT_IC_LOCATE_OFF, KIT_IC_BLUETOOTH,
			KIT_IC_BLUETOOTH_CONNECTED, KIT_IC_BLUETOOTH_OFF, KIT_IC_WIFI, KIT_IC_WIFI_OFF } },
		{ &chefkit_icon_44, { KIT_IC_REFRESH_CW } },
		{ &chefkit_icon_56, { KIT_IC_MOON, KIT_IC_SUN, KIT_IC_SUN_MOON, KIT_IC_LOCK } },
		{ &chefkit_icon_60, { KIT_IC_SUN_DIM, KIT_IC_TRASH_2 } },
		{ &chefkit_icon_64, { KIT_IC_CHEVRON_RIGHT, KIT_IC_ARROW_BIG_UP, KIT_IC_DELETE } },
		{ &chefkit_icon_72, { KIT_IC_SUN, KIT_IC_SUN_MEDIUM, KIT_IC_VIBRATE, KIT_IC_CLOCK,
			KIT_IC_HARD_DRIVE, KIT_IC_INFO, KIT_IC_WIFI_HIGH, KIT_IC_WIFI_LOW, KIT_IC_WIFI_ZERO,
			KIT_IC_PLUS, KIT_IC_CIRCLE_CHECK, KIT_IC_CIRCLE, KIT_IC_EYE, KIT_IC_EYE_OFF } },
		{ &chefkit_icon_80, { KIT_IC_WIFI } },
		{ &chefkit_icon_84, { KIT_IC_CHEVRON_LEFT, KIT_IC_X } },
		{ &chefkit_icon_200, { KIT_IC_WIFI_OFF } },
		{ &chefkit_icon_96, { KIT_IC_LOCATE, KIT_IC_LOCATE_FIXED, KIT_IC_LOCATE_OFF, KIT_IC_BLUETOOTH,
			KIT_IC_BLUETOOTH_CONNECTED, KIT_IC_BLUETOOTH_OFF, KIT_IC_WIFI, KIT_IC_WIFI_OFF } },
	};
	lv_font_glyph_dsc_t g;

	/* tabular figures: every digit advances the same in every text font */
	for (unsigned i = 0; i < sizeof(text) / sizeof(text[0]); i++) {
		uint16_t w0 = lv_font_get_glyph_width(text[i], '0', 0);
		bool same = w0 > 0;

		for (uint32_t d = '1'; d <= '9'; d++)
			same = same && lv_font_get_glyph_width(text[i], d, 0) == w0;
		CHECK(same);
	}
	/* the body ranges hold what the design writes */
	CHECK(lv_font_get_glyph_dsc(&chefkit_b400_38, &g, 0xB7, 0));	/* · */
	CHECK(lv_font_get_glyph_dsc(&chefkit_b400_38, &g, 0xB1, 0));	/* ± */
	CHECK(lv_font_get_glyph_dsc(&chefkit_b400_42, &g, 0x2026, 0));	/* … */
	CHECK(lv_font_get_glyph_dsc(&chefkit_b500_56, &g, 0x2212, 0));	/* − */
	CHECK(lv_font_get_glyph_dsc(&chefkit_bc600_44, &g, ':', 0));
	CHECK(lv_font_get_glyph_dsc(&chefkit_bc600_72, &g, '%', 0));
	CHECK(lv_font_get_glyph_dsc(&chefkit_b400_72, &g, 0x2022, 0));	/* password bullet */
	for (uint32_t c = 0x20; c <= 0x7E; c++)			/* every key's glyph */
		CHECK(lv_font_get_glyph_dsc(&chefkit_b500_64, &g, c, 0) &&
		      lv_font_get_glyph_dsc(&chefkit_b400_72, &g, c, 0));
	CHECK(lv_font_get_glyph_dsc(&chefkit_b400_48, &g, 0x2013, 0));	/* – (no value) */
	for (unsigned i = 0; i < sizeof(icons) / sizeof(icons[0]); i++)
		for (int k = 0; k < 16 && icons[i].glyphs[k]; k++) {
			const uint8_t *u = (const uint8_t *)icons[i].glyphs[k];
			uint32_t cp = (uint32_t)(u[0] & 0x0f) << 12 | (uint32_t)(u[1] & 0x3f) << 6 | (u[2] & 0x3f);

			CHECK(cp >= 0xE000 && lv_font_get_glyph_dsc(icons[i].f, &g, cp, 0));
		}
	/* CSS metrics: Barlow's 1.2 em content area, baseline 1.0 em down */
	CHECK(kit_font_px(&chefkit_b500_56) == 56 && kit_css_line(&chefkit_b500_56) == 67);
	CHECK(kit_css_baseline(&chefkit_bc600_96, 96) == 86);	/* line-height 1 */
	CHECK(kit_css_baseline(&chefkit_b400_42, 0) == 42);
}

/* ------------------------------------------------------------- kit */

static void test_statusbar(lv_obj_t *scr)
{
	struct kit_statusbar sb;
	struct kit_status st = { .gps = KIT_GPS_FIX, .bt = KIT_BT_CONNECTED, .wifi = KIT_WIFI_CONNECTED,
				 .batt = 78, .powered = false, .time = "14:16" };

	kit_statusbar_create(&sb, scr);
	kit_statusbar_set(&sb, &st);
	CHECK(!strcmp(lv_label_get_text(sb.time), "14:16"));
	CHECK(!strcmp(lv_label_get_text(sb.batt_pct), "78"));
	CHECK(!strcmp(lv_label_get_text(sb.gps), KIT_IC_LOCATE_FIXED));
	CHECK(colour(sb.gps) == kit_palette()->fg);
	CHECK(lv_obj_is_hidden(lv_obj_get_parent(sb.batt_zap)));
	CHECK(!lv_obj_has_state(sb.batt, KIT_STATE_WARN));
	/* searching/off muted; low battery unpowered warns, powered does not */
	st.gps = KIT_GPS_SEARCH;
	st.bt = KIT_BT_ON;
	st.wifi = KIT_WIFI_OFF;
	st.batt = 15;
	kit_statusbar_set(&sb, &st);
	CHECK(!strcmp(lv_label_get_text(sb.gps), KIT_IC_LOCATE) && colour(sb.gps) == kit_palette()->mut);
	CHECK(!strcmp(lv_label_get_text(sb.bt), KIT_IC_BLUETOOTH) && colour(sb.bt) == kit_palette()->mut);
	CHECK(!strcmp(lv_label_get_text(sb.wifi), KIT_IC_WIFI_OFF));
	CHECK(lv_obj_has_state(sb.batt, KIT_STATE_WARN));
	CHECK(colour(sb.batt_pct) == kit_palette()->warn);
	st.powered = true;
	kit_statusbar_set(&sb, &st);
	CHECK(!lv_obj_has_state(sb.batt, KIT_STATE_WARN));
	CHECK(!lv_obj_is_hidden(lv_obj_get_parent(sb.batt_zap)));
	st.batt = 16;
	st.powered = false;
	kit_statusbar_set(&sb, &st);
	CHECK(!lv_obj_has_state(sb.batt, KIT_STATE_WARN));
	st.batt = -1;
	kit_statusbar_set(&sb, &st);
	CHECK(!strcmp(lv_label_get_text(sb.batt_pct), "--") && !lv_obj_has_state(sb.batt, KIT_STATE_WARN));
	lv_obj_delete(sb.obj);
}

static void test_tiles(lv_obj_t *scr)
{
	struct kit_tile t[3];
	lv_obj_t *row = kit_box(scr);
	int32_t w0, w1, w2;

	lv_obj_set_size(row, 960, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(row, 24, 0);
	kit_tile_create(&t[0], row, KIT_IC_BLUETOOTH, "Bluetooth", NULL, NULL);
	kit_tile_create(&t[1], row, KIT_IC_LOCATE, "GPS", NULL, NULL);
	kit_tile_create(&t[2], row, KIT_IC_WIFI, "Wi-Fi", NULL, NULL);
	kit_tiles_fit(t, 3, 960, 24);
	lv_obj_update_layout(row);
	w0 = lv_obj_get_width(t[0].obj);
	w1 = lv_obj_get_width(t[1].obj);
	w2 = lv_obj_get_width(t[2].obj);
	/* "Bluetooth" is wider than a third: CSS min-content wins */
	CHECK(w0 + w1 + w2 + 48 == 960);
	CHECK(w0 > w1 && w1 - w2 <= 1 && w2 - w1 <= 1);
	CHECK(w0 >= lv_obj_get_width(t[0].title) + 78);
	kit_tile_set(&t[1], false, KIT_IC_LOCATE_OFF, "Off");
	CHECK(lv_obj_has_state(t[1].obj, KIT_STATE_OFF) && lv_obj_is_hidden(t[1].dot));
	CHECK(border(t[1].obj) == kit_palette()->line2 && colour(t[1].icon) == kit_palette()->mut);
	kit_tile_set(&t[1], true, KIT_IC_LOCATE, "Searching");
	CHECK(border(t[1].obj) == kit_palette()->acc && colour(t[1].icon) == kit_palette()->acct);
	lv_obj_delete(row);
}

/* ------------------------------------------------------------- text */

static void test_text(void)
{
	struct sys_state s;
	struct ui_settings u;
	struct kit_status st;
	char b[96];

	memset(&s, 0, sizeof(s));
	text_bt(&s, b, sizeof(b));
	CHECK(!strcmp(b, "Unavailable"));
	s.bt.known = true;
	text_bt(&s, b, sizeof(b));
	CHECK(!strcmp(b, "No adapter"));
	s.bt.adapter = true;
	text_bt(&s, b, sizeof(b));
	CHECK(!strcmp(b, "Off") && !tile_bt_on(&s));
	s.bt.powered = true;
	text_bt(&s, b, sizeof(b));
	CHECK(!strcmp(b, "On") && tile_bt_on(&s));
	s.bt.connected = 2;
	text_bt(&s, b, sizeof(b));
	CHECK(!strcmp(b, "2 connected"));

	s.gps.state = SYS_GPS_RUNNING;
	s.gps.mode = 3;
	s.gps.eph = 3;
	text_gps(&s, b, sizeof(b));
	CHECK(!strcmp(b, "3D fix \xC2\xB7 \xC2\xB1" "3 m"));
	s.gps.eph = -1;
	text_gps(&s, b, sizeof(b));
	CHECK(!strcmp(b, "3D fix"));
	s.gps.mode = 0;
	text_gps(&s, b, sizeof(b));
	CHECK(!strcmp(b, "Fix"));
	s.gps.state = SYS_GPS_SEARCHING;
	text_gps(&s, b, sizeof(b));
	CHECK(!strcmp(b, "Searching\xE2\x80\xA6") && tile_gps_on(&s));
	s.gps.state = SYS_GPS_FAILED;
	text_gps(&s, b, sizeof(b));
	CHECK(!strcmp(b, "Failed") && !tile_gps_on(&s));

	text_wifi(&s, b, sizeof(b));
	CHECK(!strcmp(b, "Unavailable"));
	s.wifi.known = true;
	text_wifi(&s, b, sizeof(b));
	CHECK(!strcmp(b, "Off"));
	s.wifi.enabled = true;
	text_wifi(&s, b, sizeof(b));
	CHECK(!strcmp(b, "No device"));
	s.wifi.device = true;
	s.wifi.dev_state = SYS_NM_DEVICE_DISCONNECTED;
	text_wifi(&s, b, sizeof(b));
	CHECK(!strcmp(b, "Not connected"));
	s.wifi.dev_state = 70;
	text_wifi(&s, b, sizeof(b));
	CHECK(!strcmp(b, "Connecting\xE2\x80\xA6"));
	s.wifi.dev_state = SYS_NM_DEVICE_ACTIVATED;
	snprintf(s.wifi.ssid, sizeof(s.wifi.ssid), "HomeNet");
	text_wifi(&s, b, sizeof(b));
	CHECK(!strcmp(b, "HomeNet"));

	ui_settings_defaults(&u);
	u.brightness = 70;
	text_display(&u, b, sizeof(b));
	CHECK(!strcmp(b, "Dark \xC2\xB7 70%"));
	s.storage.known = true;
	text_storage(&s, b, sizeof(b));
	CHECK(!strcmp(b, "RAM only"));
	s.storage.ours = true;
	s.storage.free_bytes = 18200000000ull;
	text_storage(&s, b, sizeof(b));
	CHECK(!strcmp(b, "18.2 GB free"));
	snprintf(s.clock.hhmm, sizeof(s.clock.hhmm), "14:16");
	snprintf(s.clock.zone, sizeof(s.clock.zone), "IST");
	text_time(&s, b, sizeof(b));
	CHECK(!strcmp(b, "14:16 \xC2\xB7 IST"));

	s.batt.known = true;
	s.batt.capacity = 10;
	s.batt.powered = true;
	status_from_state(&s, &st);
	CHECK(st.batt == 10 && st.powered && st.wifi == KIT_WIFI_CONNECTED && st.bt == KIT_BT_CONNECTED);
	CHECK(st.gps == KIT_GPS_OFF && !strcmp(st.time, "14:16"));
	s.batt.known = false;
	status_from_state(&s, &st);
	CHECK(st.batt == -1 && !st.powered);
}

/* ------------------------------------------------------------- policy */

static void test_policy(void)
{
	struct sleep_policy p = { 0 };

	CHECK(policy_power_short(false, 0) == POWER_DEFAULT);
	CHECK(policy_power_short(false, 2) == POWER_DEFAULT);	/* off: wake, not Back */
	CHECK(policy_power_short(true, 0) == POWER_DEFAULT);	/* hub: screen off */
	CHECK(policy_power_short(true, 1) == POWER_BACK);

	policy_set_sleep(&p, 30000, 1000);
	CHECK(policy_next_check_ms(&p, true, 1000) == 30000);
	CHECK(!policy_should_sleep(&p, true, 30999));
	CHECK(policy_should_sleep(&p, true, 31000));
	CHECK(!policy_should_sleep(&p, false, 31000));
	CHECK(policy_next_check_ms(&p, false, 1000) == -1);
	policy_activity(&p, 20000);
	CHECK(!policy_should_sleep(&p, true, 31000));
	CHECK(policy_next_check_ms(&p, true, 31000) == 19000);
	CHECK(policy_next_check_ms(&p, true, 60000) == 0);
	policy_set_sleep(&p, 0, 0);		/* Never */
	CHECK(!policy_should_sleep(&p, true, 1 << 30));
	CHECK(policy_next_check_ms(&p, true, 5) == -1);
}

/* ---------------------------------------------------------------- app */

static void click(lv_obj_t *o)
{
	lv_obj_send_event(o, LV_EVENT_CLICKED, NULL);
}

static void test_app(lv_obj_t *scr)
{
	struct app_platform pf = { pf_now, pf_on, pf_set, pf_bright };
	struct ui_settings set;
	struct display_page *d;
	lv_obj_t *content, *row_title, *hub_obj;
	uint32_t fg_dark;

	ui_settings_defaults(&set);
	set.sleep_s = 30;
	content = kit_box(scr);
	lv_obj_set_size(content, 1080, 2246 - 96);
	lv_obj_set_y(content, 96);
	memset(&sys_state, 0, sizeof(sys_state));
	sys_state.storage.known = true;
	app.fake_state = true;
	app_init(&pf, &set, scr, content);
	CHECK(app_depth() == 0 && app_top() == APP_PAGE_HUB);
	hub_obj = app.hub.obj;
	row_title = app.row_display.title;
	fg_dark = colour(row_title);
	CHECK(fg_dark == kit_palette_dark.fg);
	CHECK(!strcmp(lv_label_get_text(app.row_display.sub), "Dark \xC2\xB7 40%"));

	/* state changes reach the bar and the tiles */
	sys_state.bt.known = sys_state.bt.adapter = sys_state.bt.powered = true;
	sys_state.bt.connected = 1;
	app_on_state(SYS_BT);
	CHECK(!strcmp(lv_label_get_text(app.tiles[0].sub), "1 connected"));
	/* a long name ends in "..." on one line (no wrap at '-') */
	sys_state.wifi.known = sys_state.wifi.enabled = sys_state.wifi.device = true;
	sys_state.wifi.dev_state = SYS_NM_DEVICE_ACTIVATED;
	snprintf(sys_state.wifi.ssid, sizeof(sys_state.wifi.ssid), "chef-storage-wifi-long-name");
	app_on_state(SYS_WIFI);
	/* every tile and every hub row: one line, "..." at the end */
	for (int i = 0; i < 3; i++)
		kit_tile_set(&app.tiles[i], true, KIT_IC_WIFI, "a-very-long-sub-title-with-hyphens-and more words");
	{
		struct kit_row *rows[] = { &app.row_display, &app.row_sound, &app.row_time,
					   &app.row_storage, &app.row_about };

		for (int i = 0; i < 5; i++)
			kit_row_set_sub(rows[i], "a-very-long-row-subtitle-with-hyphens-that does not fit the row at all");
		lv_obj_update_layout(app.hub.obj);
		for (int i = 0; i < 3; i++) {
			CHECK(lv_obj_get_height(app.tiles[i].sub) == lv_font_get_line_height(&chefkit_b400_38));
			CHECK(lv_label_get_long_mode(app.tiles[i].sub) == LV_LABEL_LONG_MODE_DOTS);
			CHECK(lv_obj_get_width(app.tiles[i].sub) < lv_obj_get_width(app.tiles[i].obj));
		}
		for (int i = 0; i < 5; i++) {
			CHECK(lv_obj_get_height(rows[i]->sub) == lv_font_get_line_height(&chefkit_b400_42));
			CHECK(lv_label_get_long_mode(rows[i]->sub) == LV_LABEL_LONG_MODE_DOTS);
			lv_area_t sa, ca;

			lv_obj_get_coords(rows[i]->sub, &sa);
			lv_obj_get_coords(rows[i]->chevron, &ca);
			CHECK(sa.x2 < ca.x1);	/* ends before the chevron */
		}
	}
	app_on_state(~0u);	/* real subtitles back */
	kit_row_set_sub(&app.row_sound, "Haptics on");
	kit_row_set_sub(&app.row_about, app.about);
	CHECK(!strcmp(lv_label_get_text(app.sb.bt), KIT_IC_BLUETOOTH_CONNECTED));

	/* power short on the hub: default (screen off) */
	CHECK(!app_on_button("power.short"));
	CHECK(!app_on_button("volup.short"));

	/* open Display from its row, Back with power short */
	click(app.row_display.obj);
	CHECK(app_depth() == 1 && app_top() == APP_PAGE_DISPLAY);
	CHECK(lv_obj_is_hidden(hub_obj));
	d = app_display_page();
	CHECK(d != NULL);
	CHECK(lv_obj_is_disabled(d->auto_switch) && !kit_switch_get(d->auto_switch));
	CHECK(lv_obj_is_disabled(d->theme.cell[2]));		/* Auto */
	CHECK(d->theme.active == 0 && d->sleep.active == 0);	/* Dark, 30 s */
	CHECK(!strcmp(lv_label_get_text(d->value), "40%"));

	/* brightness: platform level per change, label follows */
	kit_slider_set(&d->slider, 70);
	lv_obj_send_event(d->slider.obj, LV_EVENT_VALUE_CHANGED, NULL);
	CHECK(last_level == 179 && app.set.brightness == 70);
	CHECK(!strcmp(lv_label_get_text(d->value), "70%"));
	/* the track shows 0..100 (fill = value %), input stops at the minimum */
	lv_obj_update_layout(d->slider.track);
	{
		int32_t inner = lv_obj_get_content_width(d->slider.track);

		CHECK(lv_obj_get_width(d->slider.fill) == inner * 70 / 100);
		CHECK(lv_obj_get_x(d->slider.knob) == inner * 70 / 100 - 14);
		kit_slider_set(&d->slider, 1);
		CHECK(d->slider.value == UI_BRIGHTNESS_MIN);
		lv_obj_update_layout(d->slider.track);
		CHECK(lv_obj_get_width(d->slider.fill) == inner * UI_BRIGHTNESS_MIN / 100);
		kit_slider_set(&d->slider, 70);
	}

	/* theme: restyled in place, same objects */
	kit_seg_set_active(&d->theme, 1);
	lv_obj_send_event(d->theme.obj, LV_EVENT_VALUE_CHANGED, NULL);
	CHECK(app.set.theme == UI_THEME_LIGHT && kit_theme_get() == KIT_THEME_LIGHT);
	CHECK(app.row_display.title == row_title);
	CHECK(colour(row_title) == kit_palette_light.fg);
	CHECK(bg(scr) == kit_palette_light.bg);
	CHECK(bg(d->theme.cell[1]) == kit_palette_light.acc);
	CHECK(colour(app.sb.time) == kit_palette_light.fg);
	/* a disabled cell does not take a tap */
	click(d->theme.cell[2]);
	CHECK(d->theme.active == 1 && app.set.theme == UI_THEME_LIGHT);
	kit_seg_set_active(&d->theme, 0);
	lv_obj_send_event(d->theme.obj, LV_EVENT_VALUE_CHANGED, NULL);
	CHECK(colour(row_title) == fg_dark && bg(scr) == kit_palette_dark.bg);

	/* auto-sleep at 30 s; touch restarts it; wake shows the same page */
	screen_sets = 0;
	advance(29000);
	CHECK(screen_on && screen_sets == 0);
	app_on_touch();
	advance(29000);
	CHECK(screen_on);
	advance(1100);
	CHECK(!screen_on && screen_sets == 1);
	CHECK(!app_on_button("power.short"));	/* off: default wakes */
	pf_set(true);
	CHECK(app_depth() == 1 && app_top() == APP_PAGE_DISPLAY);
	advance(29000);
	CHECK(screen_on);			/* wake restarted the timeout */
	advance(1100);
	CHECK(!screen_on);
	pf_set(true);

	/* Never: no sleep */
	click(d->sleep.cell[3]);
	CHECK(app.set.sleep_s == 0);
	advance(400000);
	CHECK(screen_on);
	click(d->sleep.cell[1]);
	CHECK(app.set.sleep_s == 60);

	/* settings replaced from storage are applied live on the open page */
	{
		struct ui_settings n = app.set;

		n.brightness = 33;
		n.theme = UI_THEME_LIGHT;
		n.sleep_s = 300;
		app_apply_settings(&n);
		CHECK(last_level == ui_brightness_level(33) && d->slider.value == 33);
		CHECK(!strcmp(lv_label_get_text(d->value), "33%"));
		CHECK(kit_theme_get() == KIT_THEME_LIGHT && d->theme.active == 1 && d->sleep.active == 2);
		n.brightness = 70;
		n.theme = UI_THEME_DARK;
		n.sleep_s = 60;
		app_apply_settings(&n);
		CHECK(kit_theme_get() == KIT_THEME_DARK && d->sleep.active == 1);
	}

	/* Back: power short on a page is handled and returns to the hub */
	CHECK(app_on_button("power.short"));
	CHECK(app_depth() == 0 && !lv_obj_is_hidden(hub_obj));
	advance(20);				/* async delete of the page */
	CHECK(app_display_page() == NULL);
	CHECK(!strcmp(lv_label_get_text(app.row_display.sub), "Dark \xC2\xB7 70%"));
	CHECK(!app_back());

	/* placeholders from tiles and rows; the back target pops */
	click(app.tiles[1].obj);
	CHECK(app_depth() == 1 && app_top() == APP_PAGE_PLACEHOLDER);
	CHECK(app_back());
	advance(20);
	click(app.row_about.obj);
	CHECK(app_depth() == 1);
	CHECK(app_on_button("power.short") && app_depth() == 0);
	advance(20);
	/* open/close many times: nothing leaks into the content root */
	for (int i = 0; i < 20; i++) {
		click(app.row_display.obj);
		app_back();
		advance(20);
	}
	CHECK(lv_obj_get_child_count(content) == 1);	/* the hub only */
}

/* ------------------------------------------------------------- Wi-Fi pages */

static bool shown(lv_obj_t *o)
{
	for (; o; o = lv_obj_get_parent(o))
		if (lv_obj_is_hidden(o))
			return false;
	return true;
}

/* The first label under root whose text is text (only shown ones if
 * only_shown). */
static lv_obj_t *find_label(lv_obj_t *root, const char *text, bool only_shown)
{
	if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text) &&
	    (!only_shown || shown(root)))
		return root;
	for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
		lv_obj_t *f = find_label(lv_obj_get_child(root, (int32_t)i), text, only_shown);

		if (f)
			return f;
	}
	return NULL;
}

static lv_obj_t *find_text(lv_obj_t *root, const char *text)
{
	return find_label(root, text, false);
}

static lv_obj_t *shown_text(const char *text)
{
	return find_label(lv_screen_active(), text, true);
}

/* Click what a finger on that text would hit. */
static bool tap_text(const char *text)
{
	lv_obj_t *o = shown_text(text);

	while (o && !lv_obj_is_clickable(o))
		o = lv_obj_get_parent(o);
	if (o)
		click(o);
	return o != NULL;
}

static struct wifi_net net(const char *ssid, enum wifi_sec sec, int strength, bool known)
{
	struct wifi_net n = { .sec = sec, .freq = 2412, .strength = strength, .known = known };

	n.ssid.len = (int)strlen(ssid);
	memcpy(n.ssid.b, ssid, (size_t)n.ssid.len);
	snprintf(n.text, sizeof(n.text), "%s", ssid);
	snprintf(n.ap, sizeof(n.ap), "/ap/%s", ssid);
	return n;
}

static void wifi_set(bool enabled, bool connected)
{
	sys_state.wifi.known = sys_state.wifi.device = true;
	sys_state.wifi.enabled = enabled;
	sys_state.wifi.dev_state = connected ? SYS_NM_DEVICE_ACTIVATED : SYS_NM_DEVICE_DISCONNECTED;
	snprintf(sys_state.wifi.ssid, sizeof(sys_state.wifi.ssid), "%s", connected ? "HomeNet" : "");
	memset(&wifi_view.cur, 0, sizeof(wifi_view.cur));
	if (connected) {
		struct wifi_active *c = &wifi_view.cur;

		c->valid = c->ap_known = true;
		c->dev_state = SYS_NM_DEVICE_ACTIVATED;
		c->sec = WIFI_SEC_WPA2;
		c->freq = 5180;
		c->strength = 82;
		snprintf(c->text, sizeof(c->text), "HomeNet");
		snprintf(c->ip4, sizeof(c->ip4), "192.168.1.42");
		snprintf(c->gw, sizeof(c->gw), "192.168.1.1");
		snprintf(c->conn, sizeof(c->conn), "/settings/1");
		snprintf(c->uuid, sizeof(c->uuid), "11111111-1111-4111-8111-111111111111");
	}
	app_on_state(SYS_WIFI | SYS_WIFI_VIEW);
}

static void test_wifi_pages(lv_obj_t *scr)
{
	memset(&wifi_stub, 0, sizeof(wifi_stub));
	memset(&wifi_view, 0, sizeof(wifi_view));
	wifi_view.nets[0] = net("Cafe Velo", WIFI_SEC_WPA2, 72, false);
	wifi_view.nets[1] = net("Garage", WIFI_SEC_WPA2, 40, true);
	wifi_view.nets[2] = net("OpenRide", WIFI_SEC_OPEN, 30, false);
	wifi_view.nets[3] = net("Corp", WIFI_SEC_ENTERPRISE, 20, false);
	wifi_view.n = 4;
	wifi_set(false, false);

	/* the tile opens the page; while it is seen NetworkManager is
	 * followed, and scanned only with Wi-Fi on */
	CHECK(app_depth() == 0);
	click(app.tiles[2].obj);
	CHECK(app_top() == APP_PAGE_WIFI);
	CHECK(wifi_stub.watch && !wifi_stub.scan);
	CHECK(shown_text("Wi-Fi is off") && !shown_text("AVAILABLE"));
	CHECK(shown_text("Off"));
	/* the switch (last in the master row, the body's first child) asks
	 * NetworkManager; the page follows its answer */
	{
		lv_obj_t *master = lv_obj_get_child(app_top_page()->kp.body, 0);
		lv_obj_t *sw = lv_obj_get_child(master, -1);

		lv_obj_update_layout(master);
		CHECK(lv_obj_get_width(sw) == KIT_SWITCH_W);
		click(sw);
		CHECK(wifi_stub.enable_calls == 1 && wifi_stub.enabled);
		CHECK(shown_text("Wi-Fi is off"));	/* not on until NM says so */
	}
	wifi_set(true, true);
	CHECK(wifi_stub.watch && wifi_stub.scan);
	CHECK(!shown_text("Wi-Fi is off") && shown_text("AVAILABLE") && shown_text("CONNECTED"));
	CHECK(shown_text("HomeNet") && shown_text("Connected \xC2\xB7 5 GHz"));
	CHECK(shown_text("Cafe Velo") && shown_text("WPA2 \xC2\xB7 2.4 GHz"));
	CHECK(shown_text("Saved \xC2\xB7 WPA2 \xC2\xB7 2.4 GHz"));
	CHECK(shown_text("Enterprise \xC2\xB7 not supported"));
	CHECK(!shown_text("Scanning") && !shown_text("No networks found"));
	wifi_view.scanning = true;
	app_on_state(SYS_WIFI_VIEW);
	CHECK(shown_text("Scanning"));
	wifi_view.scanning = false;

	/* screen off: nothing followed or scanned; on again: both */
	pf_set(false);
	CHECK(!wifi_stub.watch && !wifi_stub.scan);
	pf_set(true);
	CHECK(wifi_stub.watch && wifi_stub.scan);

	/* taps: enterprise says why, saved and open networks join, a new
	 * secured one asks for its password */
	CHECK(tap_text("Corp"));
	CHECK(shown_text("Enterprise networks are not supported") && wifi_stub.join_calls == 0);
	CHECK(tap_text("Garage"));
	CHECK(wifi_stub.join_calls == 1 && !strcmp(wifi_stub.last.text, "Garage"));
	CHECK(shown_text("Joining Garage\xE2\x80\xA6"));
	CHECK(tap_text("OpenRide") && wifi_stub.join_calls == 1);	/* busy: ignored */
	wifi_view.join.st = WIFI_JOIN_FAILED;
	app_on_state(SYS_WIFI_VIEW);
	CHECK(shown_text("Could not join Garage") && wifi_view.join.st == WIFI_JOIN_IDLE);
	CHECK(tap_text("OpenRide") && wifi_stub.join_calls == 2);
	wifi_view.join.st = WIFI_JOIN_DONE;
	app_on_state(SYS_WIFI_VIEW);
	CHECK(shown_text("On") && wifi_view.join.st == WIFI_JOIN_IDLE);

	/* the password page: no scan while it is up, typed text only in the
	 * page's buffer, a short passphrase refused before any call */
	CHECK(tap_text("Cafe Velo"));
	CHECK(app_top() == APP_PAGE_WIFI_PASSWORD && app_depth() == 2);
	CHECK(wifi_stub.watch && !wifi_stub.scan);
	CHECK(shown_text("PASSWORD \xC2\xB7 WPA2") && shown_text("Cafe Velo"));
	CHECK(page_wifi_type("short1"));
	CHECK(shown_text("short1"));
	CHECK(page_wifi_type("\n"));			/* Join */
	CHECK(wifi_stub.join_new_calls == 0 && shown_text("Use 8 to 63 characters."));
	CHECK(page_wifi_type("\b\b\b\b\b\bRide fast! 2"));
	CHECK(shown_text("Ride fast! 2"));
	/* a capture shows bullets, then the text again */
	app_snapshot_guard(true);
	CHECK(!shown_text("Ride fast! 2"));
	CHECK(shown_text("\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2"
			 "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2"
			 "\xE2\x80\xA2\xE2\x80\xA2"));
	app_snapshot_guard(false);
	CHECK(shown_text("Ride fast! 2"));
	/* the eye hides it until pressed again */
	CHECK(tap_text(KIT_IC_EYE_OFF) && !shown_text("Ride fast! 2"));
	CHECK(tap_text(KIT_IC_EYE) && shown_text("Ride fast! 2"));
	CHECK(page_wifi_type("\n"));
	CHECK(wifi_stub.join_new_calls == 1 && wifi_stub.last_sec == WIFI_SEC_WPA2 &&
	      !wifi_stub.last_hidden && wifi_stub.last_psk_len == 12);
	CHECK(!strcmp(wifi_stub.psk_seen, "Ride fast! 2"));
	CHECK(!shown_text("Ride fast! 2") && shown_text("Joining\xE2\x80\xA6"));
	CHECK(!find_text(scr, "Ride fast! 2"));	/* no copy left in a label */
	/* refused: back to the field with an error */
	wifi_view.join.st = WIFI_JOIN_BAD_PASSWORD;
	app_on_state(SYS_WIFI_VIEW);
	CHECK(app_top() == APP_PAGE_WIFI_PASSWORD && shown_text("Wrong password. Try again."));
	CHECK(wifi_view.join.st == WIFI_JOIN_IDLE);
	CHECK(colour(shown_text("Wrong password. Try again.")) == kit_palette_dark.warn);
	/* joined: the page closes */
	CHECK(page_wifi_type("goodpassword\n") && wifi_stub.join_new_calls == 2);
	wifi_view.join.st = WIFI_JOIN_DONE;
	app_on_state(SYS_WIFI_VIEW);
	CHECK(app_top() == APP_PAGE_WIFI && app_depth() == 1);
	advance(20);
	CHECK(wifi_stub.watch && wifi_stub.scan);

	/* hidden network: name, security, password */
	CHECK(tap_text("Add hidden network"));
	CHECK(app_top() == APP_PAGE_WIFI_HIDDEN && !wifi_stub.scan);
	CHECK(page_wifi_type("\n") && shown_text("Enter the network name."));
	CHECK(page_wifi_type("Back Room"));
	CHECK(tap_text("Open"));
	CHECK(!shown_text("PASSWORD"));
	CHECK(page_wifi_type("\n"));
	CHECK(wifi_stub.join_new_calls == 3 && wifi_stub.last_hidden &&
	      wifi_stub.last_sec == WIFI_SEC_OPEN && wifi_stub.last_psk_len == 0 &&
	      wifi_stub.last_ssid.len == 9 && !memcmp(wifi_stub.last_ssid.b, "Back Room", 9));
	wifi_view.join.st = WIFI_JOIN_FAILED;
	app_on_state(SYS_WIFI_VIEW);
	CHECK(shown_text("Could not join. Try again."));
	CHECK(tap_text("WPA3") && shown_text("PASSWORD"));
	CHECK(app_back());
	advance(20);

	/* details: steps, values, Forget takes two taps */
	CHECK(tap_text("HomeNet"));
	CHECK(app_top() == APP_PAGE_WIFI_DETAILS && wifi_stub.watch && !wifi_stub.scan);
	CHECK(shown_text("Joined") && shown_text("IP address") && !shown_text("Internet"));
	CHECK(shown_text("82% \xC2\xB7 strong") && shown_text("5 GHz \xC2\xB7 channel 36"));
	CHECK(shown_text("WPA2 Personal") && shown_text("192.168.1.42") && shown_text("192.168.1.1"));
	CHECK(tap_text("Forget network") && wifi_stub.forget_calls == 0);
	CHECK(shown_text("Tap again to forget"));
	advance(4100);
	CHECK(shown_text("Forget network"));
	CHECK(tap_text("Forget network") && tap_text("Tap again to forget"));
	CHECK(wifi_stub.forget_calls == 1 && app_top() == APP_PAGE_WIFI);
	CHECK(!strcmp(wifi_stub.forget_uuid, "11111111-1111-4111-8111-111111111111"));
	advance(20);
	/* the Internet step follows NetworkManager's check */
	CHECK(tap_text("HomeNet") && app_top() == APP_PAGE_WIFI_DETAILS && !shown_text("Internet"));
	wifi_view.cur.connectivity_known = true;
	wifi_view.cur.connectivity = 4;
	app_on_state(SYS_WIFI_VIEW);
	CHECK(shown_text("Internet"));
	wifi_view.cur.connectivity_known = false;
	app_on_state(SYS_WIFI_VIEW);
	CHECK(!shown_text("Internet"));
	/* another connection replaces this one during the confirmation: the
	 * page closes and nothing is forgotten */
	CHECK(tap_text("Forget network") && shown_text("Tap again to forget"));
	snprintf(wifi_view.cur.uuid, sizeof(wifi_view.cur.uuid), "22222222-2222-4222-8222-222222222222");
	snprintf(wifi_view.cur.text, sizeof(wifi_view.cur.text), "Other");
	app_on_state(SYS_WIFI_VIEW);
	CHECK(app_top() == APP_PAGE_WIFI && wifi_stub.forget_calls == 1);
	advance(4100);				/* its disarm timer is gone with it */
	CHECK(!shown_text("Tap again to forget") && wifi_stub.forget_calls == 1);
	/* the same while the screen is off: closed when the view changes */
	CHECK(tap_text("Other") && app_top() == APP_PAGE_WIFI_DETAILS);
	CHECK(tap_text("Forget network"));
	pf_set(false);
	wifi_set(true, true);			/* HomeNet again */
	pf_set(true);
	CHECK(app_top() == APP_PAGE_WIFI && wifi_stub.forget_calls == 1);
	advance(20);
	CHECK(tap_text("HomeNet") && app_top() == APP_PAGE_WIFI_DETAILS);
	/* disconnected meanwhile: the page closes */
	wifi_set(true, false);
	CHECK(app_top() == APP_PAGE_WIFI && !shown_text("CONNECTED"));
	advance(20);

	/* theme switch restyles the pages in place */
	kit_theme_set(KIT_THEME_LIGHT);
	CHECK(colour(shown_text("Cafe Velo")) == kit_palette_light.fg);
	kit_theme_set(KIT_THEME_DARK);

	/* leaving the Wi-Fi pages stops following NetworkManager */
	CHECK(app_on_button("power.short") && app_depth() == 0);
	CHECK(!wifi_stub.watch && !wifi_stub.scan);
	advance(20);
	for (int i = 0; i < 10; i++) {
		click(app.tiles[2].obj);
		CHECK(tap_text("Cafe Velo"));
		app_back();
		app_back();
		advance(20);
	}
	CHECK(!wifi_stub.watch && lv_obj_get_child_count(app.content) == 1);
}

int main(void)
{
	lv_obj_t *scr;

	fakeloop_init(true);
	fakeloop_display(1080, 2246);
	scr = lv_screen_active();
	kit_theme_init(KIT_THEME_DARK);
	test_fonts();
	test_statusbar(scr);
	test_tiles(scr);
	test_text();
	test_policy();
	test_app(scr);
	test_wifi_pages(scr);
	return check_report("test-app");
}
