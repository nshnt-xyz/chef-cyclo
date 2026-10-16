/*
 * test_app.c - the system UI kit and app on a headless LVGL display with
 * test-controlled time: generated fonts (tabular digits, icon glyphs, CSS
 * metrics), theme switching in place, the status bar mapping, tile
 * widths, hub subtitles, the page stack, the power-key policy and
 * auto-sleep, and Display page settings.
 */
#define _GNU_SOURCE
#include <math.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
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
#include "gpsview.h"
#include "pages.h"
#include "blesens.h"
#include "chefui.h"
#include "feedback.h"

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

static int level_calls;

static int pf_bright(int level)
{
	last_level = level;
	level_calls++;
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
	const lv_font_t *text[] = { &chefkit_b400_26, &chefkit_b400_36, &chefkit_b500_24,
		&chefkit_b500_32, &chefkit_bc600_48, &chefkit_bc600_68, &chefkit_bc600_76,
		&chefkit_bc600_150, &chefkit_b400_38, &chefkit_b400_40, &chefkit_b400_42,
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
			KIT_IC_PLUS, KIT_IC_CIRCLE_CHECK, KIT_IC_CIRCLE, KIT_IC_EYE, KIT_IC_EYE_OFF,
			KIT_IC_SATELLITE } },
		{ &chefkit_icon_80, { KIT_IC_WIFI } },
		{ &chefkit_icon_84, { KIT_IC_CHEVRON_LEFT, KIT_IC_X } },
		{ &chefkit_icon_200, { KIT_IC_WIFI_OFF, KIT_IC_LOCATE_OFF } },
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
	/* the GPS figures: every character the pages write in these fonts */
	for (const char *c = "No fixStartingFailed3D2D:.-\xC2\xB1 m0123456789"; *c; c++)
		if ((unsigned char)*c < 0x80)
			CHECK(lv_font_get_glyph_dsc(&chefkit_bc600_150, &g, (uint32_t)(unsigned char)*c, 0));
	CHECK(lv_font_get_glyph_dsc(&chefkit_bc600_150, &g, 0xB1, 0) &&
	      lv_font_get_glyph_dsc(&chefkit_bc600_150, &g, 0x2013, 0));
	for (const char *c = "0123456789 ./-ms"; *c; c++)
		CHECK(lv_font_get_glyph_dsc(&chefkit_bc600_76, &g, (uint32_t)*c, 0));
	for (const char *c = "0123456789 .NSEW"; *c; c++)
		CHECK(lv_font_get_glyph_dsc(&chefkit_bc600_68, &g, (uint32_t)*c, 0));
	CHECK(lv_font_get_glyph_dsc(&chefkit_bc600_68, &g, 0xB0, 0) &&
	      lv_font_get_glyph_dsc(&chefkit_bc600_68, &g, 0x2013, 0));
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
	CHECK(!strcmp(b, "Failed") && tile_gps_on(&s));		/* switched on */

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
	text_display(&u, -1, b, sizeof(b));
	CHECK(!strcmp(b, "Dark \xC2\xB7 70%"));
	u.auto_brightness = true;
	text_display(&u, 45, b, sizeof(b));
	CHECK(!strcmp(b, "Auto \xC2\xB7 45%"));
	text_display(&u, -1, b, sizeof(b));
	CHECK(!strcmp(b, "Auto \xC2\xB7 70%"));
	u.auto_brightness = false;
	s.storage.known = true;
	text_storage(&s, b, sizeof(b));
	CHECK(!strcmp(b, "RAM only"));
	s.storage.ours = true;
	s.storage.free_bytes = 18200000000ull;
	text_storage(&s, b, sizeof(b));
	CHECK(!strcmp(b, "18.2 GB free"));
	snprintf(s.clock.hhmm, sizeof(s.clock.hhmm), "14:16");
	snprintf(s.clock.zone, sizeof(s.clock.zone), "IST");
	text_time(&s, true, b, sizeof(b));
	CHECK(!strcmp(b, "14:16 \xC2\xB7 IST"));

	s.batt.known = true;
	s.batt.capacity = 10;
	s.batt.powered = true;
	status_from_state(&s, true, &st);
	CHECK(st.batt == 10 && st.powered && st.wifi == KIT_WIFI_CONNECTED && st.bt == KIT_BT_CONNECTED);
	CHECK(st.gps == KIT_GPS_SEARCH && !strcmp(st.time, "14:16"));	/* on, failed: muted */
	s.gps.state = SYS_GPS_DISABLED;
	status_from_state(&s, true, &st);
	text_gps(&s, b, sizeof(b));
	CHECK(st.gps == KIT_GPS_OFF && !strcmp(b, "Off") && !tile_gps_on(&s));
	s.gps.state = SYS_GPS_UNAVAILABLE;
	status_from_state(&s, true, &st);
	CHECK(st.gps == KIT_GPS_OFF && !tile_gps_on(&s));
	s.batt.known = false;
	status_from_state(&s, true, &st);
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
	CHECK(sys_als_applied() == -1);		/* before sys_als_start (fake runs): no level */
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
	CHECK(!lv_obj_is_disabled(d->auto_switch) && !kit_switch_get(d->auto_switch));
	CHECK(!lv_obj_is_disabled(d->theme.cell[2]));		/* Auto */
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
	/* Auto takes a tap: Dark until the light sensor decides */
	click(d->theme.cell[2]);
	CHECK(d->theme.active == 2 && app.set.theme == UI_THEME_AUTO && kit_theme_get() == KIT_THEME_DARK);
	click(d->theme.cell[1]);
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

	/* tiles and rows open their pages; the back target pops */
	click(app.tiles[0].obj);
	CHECK(app_depth() == 1 && app_top() == APP_PAGE_BT);
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

/* ------------------------------------------------------------- GPS pages */

static int64_t mono_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sat(int gnss, int svid, double az, double el, double ss, bool used)
{
	gps_view.sats[gps_view.n_sats++] = (struct gps_sat){ gnss, svid, az, el, ss, used };
}

static void gps_state_set(enum sys_gps st, int enabled)
{
	sys_state.gps.state = st;
	sys_state.gps.enabled = enabled;
	app_on_state(SYS_GPS | SYS_GPS_VIEW);
}

/* Is the GPS page's 1 s tick (its timer, found by user data) running? */
static bool page_ticking(void *page_priv)
{
	for (lv_timer_t *t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t))
		if (lv_timer_get_user_data(t) == page_priv)
			return !lv_timer_get_paused(t);
	return false;
}

static void test_gps_pages(lv_obj_t *scr)
{
	void *gp;

	(void)scr;
	memset(&gps_view, 0, sizeof(gps_view));
	gps_view.eph = gps_view.alt = gps_view.speed = gps_view.lat = gps_view.lon = NAN;
	gps_view.hdop = gps_view.pdop = NAN;
	sys_state.gps.state = SYS_GPS_DISABLED;
	sys_state.gps.enabled = 0;
	app_on_state(SYS_GPS);
	CHECK(!strcmp(lv_label_get_text(app.tiles[1].sub), "Off"));
	CHECK(!strcmp(lv_label_get_text(app.sb.gps), KIT_IC_LOCATE_OFF));
	gps_state_set(SYS_GPS_OFF, 1);
	CHECK(!strcmp(lv_label_get_text(app.tiles[1].sub), "On"));
	CHECK(!strcmp(lv_label_get_text(app.sb.gps), KIT_IC_LOCATE));	/* on, idle: locate muted */
	CHECK(colour(app.sb.gps) == kit_palette_dark.mut);

	/* the tile opens the page; seen with the screen on: the lease */
	CHECK(app_depth() == 0 && !sys_gps_watching());
	click(app.tiles[1].obj);
	CHECK(app_top() == APP_PAGE_GPS && sys_gps_watching());
	pf_set(false);
	CHECK(!sys_gps_watching());
	pf_set(true);
	CHECK(sys_gps_watching());

	/* off: the empty state; the switch asks the manager (not connected
	 * here: it springs back) */
	gps_state_set(SYS_GPS_DISABLED, 0);
	CHECK(shown_text("No position") && shown_text("Off") && !shown_text("STATUS"));
	{
		lv_obj_t *master = lv_obj_get_child(app_top_page()->kp.body, 0);
		lv_obj_t *sw = lv_obj_get_child(master, -1);

		click(sw);
		CHECK(!kit_switch_get(sw));
	}

	/* searching: elapsed since the lease, satellites in view */
	gps_view.lease_ms = gps_view.start_ms = mono_now() - 24500;	/* the manager's bring-up began 24.5 s ago */
	sat(GNSS_GPS, 5, 40, 62, 44, false);
	sat(GNSS_GALILEO, 4, 130, 58, 42, false);
	sat(GNSS_BEIDOU, 11, 270, 65, 40, false);
	sat(GNSS_GLONASS, 8, -1, -1, 0, false);		/* not placed yet */
	gps_state_set(SYS_GPS_SEARCHING, 1);
	CHECK(shown_text("On \xC2\xB7 searching") && shown_text("No fix") && shown_text("0:24"));
	CHECK(shown_text("4 satellites visible, 0 in use. Stay still with a clear view of the sky."));
	CHECK(shown_text("G05") && shown_text("E04") && shown_text("C11") && !shown_text("R08"));
	advance(1000);					/* the 1 s tick */
	/* it ticks only while the page is seen: not with the screen off, not
	 * after a state update or a capture then, not under the satellites */
	gp = app_top_page()->priv;
	CHECK(page_ticking(gp));
	{
		struct gps_view keep = gps_view;

		pf_set(false);
		/* the screen off forgets the view (the lease went) */
		CHECK(gps_view.n_sats == 0 && gps_view.lease_ms == 0);
		gps_view = keep;		/* as the fake manager would refill it */
	}
	CHECK(!page_ticking(gp));
	gps_state_set(SYS_GPS_SEARCHING, 1);
	app_snapshot_guard(true);
	app_snapshot_guard(false);
	CHECK(!page_ticking(gp));
	pf_set(true);
	CHECK(page_ticking(gp));
	gps_state_set(SYS_GPS_FAILED, 1);
	CHECK(shown_text("Failed") && shown_text("On \xC2\xB7 failed"));

	/* locked */
	gps_view.mode = 3;
	gps_view.eph = 2.6;
	gps_view.alt = 41.4;
	gps_view.speed = 5.0;
	gps_view.lat = -33.85690;
	gps_view.lon = 151.21530;
	gps_view.hdop = 0.8;
	gps_view.pdop = 1.4;
	gps_view.first_fix_ms = gps_view.start_ms + 28000;
	gps_view.sats[0].used = gps_view.sats[1].used = true;
	gps_state_set(SYS_GPS_RUNNING, 1);
	CHECK(shown_text("On \xC2\xB7 locked") && shown_text("3D") && shown_text("\xC2\xB1" "3 m"));
	CHECK(shown_text("2/4") && shown_text("28 s") && shown_text("41 m") && shown_text("0.8") &&
	      shown_text("1.4") && shown_text("18.0"));
	CHECK(shown_text("33.85690\xC2\xB0 S  151.21530\xC2\xB0 E"));
	CHECK(shown_text("2 in use \xC2\xB7 4 systems") && !shown_text("No fix"));
	/* a capture never holds the position */
	app_snapshot_guard(true);
	CHECK(!shown_text("33.85690\xC2\xB0 S  151.21530\xC2\xB0 E"));
	app_snapshot_guard(false);
	CHECK(shown_text("33.85690\xC2\xB0 S  151.21530\xC2\xB0 E"));
	gps_view.mode = 2;
	app_on_state(SYS_GPS_VIEW);
	CHECK(shown_text("2D"));
	gps_view.mode = 3;

	/* the satellites page: same lease, counts per system, one bar each */
	CHECK(tap_text("2 in use \xC2\xB7 4 systems") && app_top() == APP_PAGE_GPS_SATS &&
	      sys_gps_watching());
	CHECK(shown_text("GLONASS") && shown_text("/1") && shown_text("dB-Hz"));
	CHECK(shown_text("44") && shown_text("R08"));	/* a bar for every satellite */
	CHECK(!page_ticking(gp));			/* the GPS page under it */
	gps_state_set(SYS_GPS_SEARCHING, 1);
	CHECK(!page_ticking(gp));
	gps_state_set(SYS_GPS_RUNNING, 1);
	/* fewer satellites, then more again: every bar comes back */
	gps_view.n_sats = 1;
	app_on_state(SYS_GPS_VIEW);
	CHECK(shown_text("44") && !shown_text("42") && !shown_text("R08"));
	gps_view.n_sats = 4;
	app_on_state(SYS_GPS_VIEW);
	CHECK(shown_text("44") && shown_text("42") && shown_text("40") && shown_text("R08"));
	CHECK(app_back() && app_top() == APP_PAGE_GPS && sys_gps_watching());
	advance(20);

	/* leaving: released, nothing kept */
	CHECK(app_back() && app_depth() == 0);
	CHECK(!sys_gps_watching() && gps_view.n_sats == 0 && isnan(gps_view.lat) && gps_view.lease_ms == 0);
	advance(20);
	for (int i = 0; i < 10; i++) {
		click(app.tiles[1].obj);
		app_back();
		advance(20);
	}
	CHECK(!sys_gps_watching() && lv_obj_get_child_count(app.content) == 1);
}


/* ------------------------------------------------------------- Bluetooth */
#define BA "AA:BB:CC:DD:EE:01"
#define BB "AA:BB:CC:DD:EE:02"
static int ble_peer;

static void ble_line(const char *line)
{
	CHECK(write(ble_peer, line, strlen(line)) == (ssize_t)strlen(line));
	CHECK(write(ble_peer, "\n", 1) == 1);
	advance(20);
	app_on_state(SYS_BLE);
}

static void ble_commands(const char *want)
{
	char b[512];
	ssize_t n = recv(ble_peer, b, sizeof(b) - 1, MSG_DONTWAIT);

	if (n < 0) n = 0;
	b[n] = 0;
	CHECK(!strcmp(b, want));
}

static void ble_wants(bool hold, bool scan)
{
	bool h, s;

	sys_ble_wants(&h, &s);
	CHECK(h == hold && s == scan);
}

static void bt_changed(unsigned what, void *user)
{
	(void)user;
	app_on_state(what);
}

static void test_bt_pages(void)
{
	char path[] = "/tmp/chef-bt-app.XXXXXX";
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int lfd, tmpfd = mkstemp(path);

	CHECK(tmpfd >= 0);
	close(tmpfd); unlink(path);
	lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
	CHECK(bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(lfd, 1) == 0);
	sys_on_change(bt_changed, NULL);
	sys_ble_start(path);
	advance(20);
	ble_peer = accept4(lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
	CHECK(ble_peer >= 0);
	ble_commands("watch\n");
	ble_line("{\"type\":\"ok\",\"cmd\":\"watch\"}");
	sys_state.bt.known = sys_state.bt.adapter = true;
	sys_state.bt.powered = false;
	app_on_state(SYS_BT);
	click(app.tiles[0].obj);
	CHECK(app_top() == APP_PAGE_BT && shown_text("Sensors are disconnected"));
	ble_wants(true, false);
	ble_commands("hold on\n");
	{
		lv_obj_t *master = lv_obj_get_child(app_top_page()->kp.body, 0);
		lv_obj_t *sw = lv_obj_get_child(master, -1);
		wifi_stub.result = 0; /* queued, later refused with no Powered signal */
		click(sw);
		CHECK(wifi_stub.bt_power_calls == 1 && wifi_stub.bt_power_last);
		CHECK(!kit_switch_get(sw));
		advance(1000); /* no confirmation: never show the requested state */
		CHECK(!kit_switch_get(sw));
		CHECK(shown_text("Sensors are disconnected"));
	}
	sys_state.bt.powered = true;
	app_on_state(SYS_BT);
	{
		lv_obj_t *sw = lv_obj_get_child(lv_obj_get_child(app_top_page()->kp.body, 0), -1);
		CHECK(kit_switch_get(sw));
		ble_commands("scan on\n");
		click(sw);
		CHECK(!wifi_stub.bt_power_last && kit_switch_get(sw));
		sys_state.bt.powered = false; /* confirmed off */
		app_on_state(SYS_BT);
		ble_commands("scan off\n");
		CHECK(!kit_switch_get(sw));
		click(sw);
		CHECK(wifi_stub.bt_power_last && !kit_switch_get(sw));
		sys_state.bt.powered = true; /* confirmed on */
		app_on_state(SYS_BT);
		CHECK(kit_switch_get(sw));
	}
	ble_wants(true, true);
	ble_commands("scan on\n");
	CHECK(shown_text("No sensors yet. Pair one below.") && shown_text("Wake a sensor nearby to find it."));
	ble_line("{\"type\":\"sensor\",\"addr\":\"" BA "\",\"name\":\"Combo\",\"kinds\":\"hr,power,csc\",\"state\":\"connected\",\"battery\":80,\"rssi\":-58,\"rssi_live\":false,\"paired\":\"2026-09-02\"}");
	ble_line("{\"type\":\"value\",\"addr\":\"" BA "\",\"kind\":\"hr\",\"value\":321}");
	ble_line("{\"type\":\"value\",\"addr\":\"" BA "\",\"kind\":\"power\",\"value\":-120,\"cadence\":90}");
	ble_line("{\"type\":\"value\",\"addr\":\"" BA "\",\"kind\":\"csc\",\"speed\":27.5,\"cadence\":80}");
	CHECK(shown_text("Heart rate") && shown_text("Power") && shown_text("Speed / cadence"));
	CHECK(shown_text("321") && shown_text("-120") && shown_text("27.5"));
	CHECK(shown_text("On \xC2\xB7 3 of 3 sensors connected"));
	CHECK(!strcmp(lv_label_get_text(app.tiles[0].sub), "3 of 3 connected"));
	pf_set(false);
	ble_wants(false, false);
	ble_commands("hold off\nscan off\n");
	pf_set(true);
	ble_commands("hold on\nscan on\n");

	/* Pair, Cancel, late completion, timeout, failure and success. */
	ble_line("{\"type\":\"found\",\"addr\":\"" BB "\",\"name\":\"New Sensor\",\"kinds\":\"hr\",\"rssi\":-60}");
	CHECK(tap_text("Pair"));
	ble_commands("pair " BB "\n");
	CHECK(shown_text("PAIRING\xE2\x80\xA6") && shown_text("Cancel") && ble_view.pair.st == BLE_PAIR_RUNNING);
	CHECK(page_ticking(app_top_page()->priv));
	pf_set(false);
	ble_commands("hold off\nscan off\n");
	CHECK(!page_ticking(app_top_page()->priv));
	ble_line("{\"type\":\"pair\",\"addr\":\"" BB "\",\"state\":\"resolving\"}");
	CHECK(!page_ticking(app_top_page()->priv)); /* a state update cannot resume it */
	pf_set(true);
	ble_commands("hold on\nscan on\n");
	CHECK(page_ticking(app_top_page()->priv));
	CHECK(tap_text("Cancel"));
	ble_commands("cancel " BB "\n");
	CHECK(!shown_text("Cancel") && ble_view.pair.st == BLE_PAIR_NONE);
	CHECK(!page_ticking(app_top_page()->priv));
	ble_line("{\"type\":\"pair\",\"addr\":\"" BB "\",\"state\":\"done\"}");
	CHECK(ble_view.pair.st == BLE_PAIR_NONE);
	CHECK(tap_text("Pair"));
	ble_commands("pair " BB "\n");
	ble_line("{\"type\":\"pair\",\"addr\":\"" BB "\",\"state\":\"failed\",\"reason\":\"timed out\"}");
	CHECK(shown_text("NOT PAIRED") && shown_text("The sensor did not answer. Wake it and try again."));
	CHECK(tap_text("Close") && ble_view.pair.st == BLE_PAIR_NONE);
	CHECK(tap_text("Pair"));
	ble_commands("pair " BB "\n");
	ble_line("{\"type\":\"pair\",\"addr\":\"" BB "\",\"state\":\"failed\",\"reason\":\"bond refused\"}");
	CHECK(shown_text("Pairing did not finish. Wake the sensor and try again."));
	CHECK(tap_text("Close") && tap_text("Pair"));
	ble_commands("pair " BB "\n");
	ble_line("{\"type\":\"pair\",\"addr\":\"" BB "\",\"state\":\"done\"}");
	CHECK(!shown_text("Cancel") && ble_view.pair.st == BLE_PAIR_NONE);

	/* Detail identity is addr+kind, independent of the list index. */
	CHECK(tap_text("Power") && app_top() == APP_PAGE_BT_SENSOR);
	ble_wants(true, false);
	ble_commands("scan off\n");
	CHECK(shown_text("BLE Cycling Power") && shown_text("90 rpm") && shown_text("-120"));
	CHECK(shown_text("\xE2\x88\x92" "58 dBm \xC2\xB7 strong (last seen)") && shown_text("80%") && shown_text("2 Sep 2026"));
	ble_line("{\"type\":\"value\",\"addr\":\"" BA "\",\"kind\":\"power\",\"value\":null,\"cadence\":null}");
	CHECK(!shown_text("-120") && !shown_text("90 rpm") && shown_text("\xE2\x80\x93"));
	CHECK(tap_text("Forget sensor"));
	ble_commands("");
	advance(4100);
	CHECK(shown_text("Forget sensor"));
	CHECK(tap_text("Forget sensor"));
	/* Add another device and swap slots while confirmation is armed. */
	ble_line("{\"type\":\"sensor\",\"addr\":\"" BB "\",\"name\":\"Other\",\"kinds\":\"power\",\"state\":\"connected\"}");
	{
		struct ble_sensor save = ble_view.s[0];
		ble_view.s[0] = ble_view.s[1]; ble_view.s[1] = save;
		app_on_state(SYS_BLE);
	}
	CHECK(shown_text("Combo") && !shown_text("Other"));
	CHECK(tap_text("Tap again to forget"));
	CHECK(app_top() == APP_PAGE_BT);
	ble_commands("forget " BA "\nscan on\n");
	advance(20);
	/* Forget notification from another client closes the right detail. */
	CHECK(tap_text("Heart rate"));
	ble_commands("scan off\n");
	ble_line("{\"type\":\"forgotten\",\"addr\":\"" BA "\"}");
	CHECK(app_top() == APP_PAGE_BT && !shown_text("Heart rate"));
	ble_commands("scan on\n");
	advance(20);
	CHECK(app_back());
	ble_wants(false, false);
	ble_commands("hold off\nscan off\n");
	advance(20);
	CHECK(lv_obj_get_child_count(app.content) == 1);
	sys_ble_stop();
	sys_on_change(NULL, NULL);
	close(ble_peer); close(lfd); unlink(path);
}


/* ------------------------------------------------------------- M4 pages */

static void set_clock(int hour, int minute)
{
	snprintf(sys_state.clock.hhmm, sizeof(sys_state.clock.hhmm), "%02d:%02d", hour, minute);
	sys_state.clock.hour = hour;
	sys_state.clock.minute = minute;
	sys_state.clock.valid = true;
	snprintf(sys_state.clock.zone, sizeof(sys_state.clock.zone), "IST");
	snprintf(sys_state.clock.date, sizeof(sys_state.clock.date), "Thursday 8 October 2026");
	sys_state.clock.utc_offset_min = 330;
}

static bool has_label(lv_obj_t *root, const char *text)
{
	return find_label(root, text, true) != NULL;
}

static int timer_count(void)
{
	int n = 0;

	for (lv_timer_t *t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t))
		n++;
	return n;
}

static void test_m4_fonts_text(void)
{
	lv_font_glyph_dsc_t g;
	struct sys_state st;
	char b[128];

	for (const char *c = "ampm"; *c; c++)
		CHECK(lv_font_get_glyph_dsc(&chefkit_b400_20, &g, (uint32_t)*c, 0));
	for (uint32_t c = 0x20; c <= 0x7E; c++)
		CHECK(lv_font_get_glyph_dsc(&chefkit_b400_52, &g, c, 0) &&
		      lv_font_get_glyph_dsc(&chefkit_b400_44, &g, c, 0) &&
		      lv_font_get_glyph_dsc(&chefkit_bc600_160, &g, c, 0));
	CHECK(lv_font_get_glyph_dsc(&chefkit_b400_44, &g, 0xB7, 0));
	for (const char *c = "0123456789 -.GB~"; *c; c++)
		CHECK(lv_font_get_glyph_dsc(&chefkit_bc600_64, &g, (uint32_t)*c, 0));
	for (const char *c = "0123456789-.~"; *c; c++)
		CHECK(lv_font_get_glyph_dsc(&chefkit_bc600_200, &g, (uint32_t)*c, 0));
	for (const char *c = "0123456789-:"; *c; c++)
		CHECK(lv_font_get_glyph_dsc(&chefkit_bc600_280, &g, (uint32_t)*c, 0));
	CHECK(kit_font_px(&chefkit_b400_52) == 52 && kit_font_px(&chefkit_bc600_280) == 280 &&
	      kit_font_px(&chefkit_bc600_64) == 64 && kit_font_px(&chefkit_b400_20) == 20);

	/* hub and status text in both presentations, including midnight and noon */
	memset(&st, 0, sizeof(st));
	set_clock(0, 5);
	memcpy(&st.clock, &sys_state.clock, sizeof(st.clock));
	text_time(&st, true, b, sizeof(b));
	CHECK(!strcmp(b, "00:05 \xC2\xB7 IST"));
	text_time(&st, false, b, sizeof(b));
	CHECK(!strcmp(b, "12:05 am \xC2\xB7 IST"));
	st.clock.hour = 12;
	text_time(&st, false, b, sizeof(b));
	CHECK(!strcmp(b, "12:05 pm \xC2\xB7 IST"));
	st.clock.valid = false;
	st.clock.hhmm[0] = 0;
	text_time(&st, false, b, sizeof(b));
	CHECK(!strcmp(b, "--:-- \xC2\xB7 IST"));

	/* the time source row: only a verified report says "Set by" */
	memset(&st, 0, sizeof(st));
	text_time_source(&st, b, sizeof(b));
	CHECK(!strcmp(b, "Time source unavailable"));
	st.timeview.boot_source = SYS_TIME_RESTORED;
	text_time_source(&st, b, sizeof(b));
	CHECK(!strcmp(b, "Restored at boot, not yet verified"));
	st.timeview.boot_source = SYS_TIME_FLOOR;
	text_time_source(&st, b, sizeof(b));
	CHECK(!strcmp(b, "Restored boot-time floor, not yet verified"));
	st.timeview.boot_source = SYS_TIME_NONE;
	text_time_source(&st, b, sizeof(b));
	CHECK(!strcmp(b, "Time not yet verified"));
	st.timeview.source = SYS_TIME_GPS;	/* not verified: no claim */
	text_time_source(&st, b, sizeof(b));
	CHECK(!strcmp(b, "Time not yet verified"));
	st.timeview.verified = true;
	text_time_source(&st, b, sizeof(b));
	CHECK(!strcmp(b, "Set by GPS"));
	st.timeview.source = SYS_TIME_NETWORK;
	text_time_source(&st, b, sizeof(b));
	CHECK(!strcmp(b, "Set by network"));

	/* last sync: the reference time, local zone, 12/24 hour; unknown otherwise */
	setenv("TZ", "IST-5:30", 1);
	tzset();
	st.timeview.source = SYS_TIME_GPS;
	st.timeview.reference_epoch = 1791448320;	/* 14:02 IST 8 October 2026 */
	st.clock.valid = false;				/* no "today": time only */
	text_time_sync(&st, true, b, sizeof(b));
	CHECK(!strcmp(b, "Last synced 14:02 via GPS."));
	text_time_sync(&st, false, b, sizeof(b));
	CHECK(!strcmp(b, "Last synced 2:02 pm via GPS."));
	st.timeview.reference_epoch = 0;
	text_time_sync(&st, true, b, sizeof(b));
	CHECK(!strcmp(b, "Last sync time unknown."));
	st.timeview.reference_epoch = 1791448320;
	st.timeview.verified = false;
	text_time_sync(&st, true, b, sizeof(b));
	CHECK(!strcmp(b, "Last sync time unknown."));

	/* zone row: the current local offset, not an assumption */
	memset(&st, 0, sizeof(st));
	text_time_zone(&st, b, sizeof(b));
	CHECK(!strcmp(b, "Unknown"));
	set_clock(14, 16);
	memcpy(&st.clock, &sys_state.clock, sizeof(st.clock));
	text_time_zone(&st, b, sizeof(b));
	CHECK(!strcmp(b, "IST \xC2\xB7 UTC+05:30"));
	st.clock.utc_offset_min = -210;
	snprintf(st.clock.zone, sizeof(st.clock.zone), "NST");
	text_time_zone(&st, b, sizeof(b));
	CHECK(!strcmp(b, "NST \xC2\xB7 UTC-03:30"));
	text_time_hint(&st, b, sizeof(b));
	CHECK(!strcmp(b, "14:16 instead of 2:16 pm"));

	text_gb(6400000000ULL, false, b, sizeof(b));
	CHECK(!strcmp(b, "6.4 GB"));
	text_gb(300000000ULL, true, b, sizeof(b));
	CHECK(!strcmp(b, "~0.3 GB"));
	text_uptime(11520, b, sizeof(b));
	CHECK(!strcmp(b, "3 h 12 min"));
	text_uptime(59, b, sizeof(b));
	CHECK(!strcmp(b, "0 min"));
	text_uptime(90000, b, sizeof(b));
	CHECK(!strcmp(b, "1 d 1 h"));
	memset(&st, 0, sizeof(st));
	st.storage.known = st.storage.ours = true;
	st.storage.total_bytes = 32000000000ULL;
	text_storage_unit(&st, b, sizeof(b));
	CHECK(!strcmp(b, "GB free of 32 GB"));
	st.storage.total_bytes = 29900000000ULL;
	text_storage_unit(&st, b, sizeof(b));
	CHECK(!strcmp(b, "GB free of 29.9 GB"));
	st.storage.ours = false;
	text_storage_unit(&st, b, sizeof(b));
	CHECK(!strcmp(b, "RAM only"));
}

static void test_m4_pages(void)
{
	lv_obj_t *content = app.content, *page, *l, *sw;
	struct ui_settings n;
	int timers;

	memset(&sys_state, 0, sizeof(sys_state));
	setenv("TZ", "IST-5:30", 1);
	tzset();
	set_clock(14, 16);
	sys_state.storage.known = sys_state.storage.ours = true;
	app.fake_state = true;
	app_on_state(~0u);
	pf_set(true);
	CHECK(app_depth() == 0);

	/* ---- status bar: 24 hour keeps the old geometry, 12 hour adds the marker below the digits */
	int col24_y1;
	CHECK(!strcmp(lv_label_get_text(app.sb.time), "14:16") && app.set.clock_24h);
	lv_obj_update_layout(app.sb.obj);
	CHECK(lv_obj_is_hidden(lv_obj_get_parent(app.sb.meridiem)));
	{
		lv_area_t col24;

		lv_obj_get_coords(app.sb.time_col, &col24);
		CHECK(col24.y2 - col24.y1 + 1 == 44);
		col24_y1 = col24.y1;
	}
	app_set_clock_24h(false);
	CHECK(!strcmp(lv_label_get_text(app.sb.time), "2:16"));
	CHECK(!strcmp(lv_label_get_text(app.sb.meridiem), "pm") &&
	      !lv_obj_is_hidden(lv_obj_get_parent(app.sb.meridiem)));
	CHECK(!strcmp(lv_label_get_text(app.row_time.sub), "2:16 pm \xC2\xB7 IST"));
	set_clock(12, 59);
	app_on_state(SYS_CLOCK);
	lv_obj_update_layout(app.sb.obj);
	CHECK(!strcmp(lv_label_get_text(app.sb.time), "12:59"));
	{
		lv_area_t bar, col, batt;

		lv_obj_get_coords(app.sb.obj, &bar);
		lv_obj_get_coords(app.sb.time_col, &col);
		lv_obj_get_coords(app.sb.batt, &batt);
		lv_area_t mer, gps;

		CHECK(col.y1 >= bar.y1 && col.y2 <= bar.y1 + KIT_STATUS_H - 1);	/* inside the ear */
		CHECK(col.y1 == col24_y1 && col.y2 - col.y1 + 1 == 44);	/* the digits did not move in 12 hour */
		CHECK(col.x2 <= bar.x2 - 36);	/* the corner inset: the box ends at x 1043, its ink at 1041 */
		CHECK(col.y1 >= bar.y1 + 26);	/* digit ink top at y 35 or lower, as in 24 hour */
		lv_obj_get_coords(lv_obj_get_parent(app.sb.meridiem), &mer);
		CHECK(mer.y1 >= col.y2 && mer.y2 <= bar.y1 + KIT_STATUS_H - 1);	/* the marker is under the digits, inside the bar */
		CHECK(mer.x2 <= col.x2 && mer.x2 >= col.x2 - 2);	/* right aligned with them */
		lv_obj_get_coords(app.sb.gps, &gps);
		CHECK(gps.x1 >= bar.x1 + 36);	/* the left inset */
		CHECK(batt.x1 >= 818);			/* right of the drawn notch */
		CHECK(batt.x2 - batt.x1 + 1 == 78 && batt.y2 - batt.y1 + 1 == 38);
	}
	set_clock(0, 5);
	app_on_state(SYS_CLOCK);
	CHECK(!strcmp(lv_label_get_text(app.sb.time), "12:05") &&
	      !strcmp(lv_label_get_text(app.sb.meridiem), "am"));
	app_set_clock_24h(true);
	CHECK(!strcmp(lv_label_get_text(app.sb.time), "00:05") &&
	      lv_obj_is_hidden(lv_obj_get_parent(app.sb.meridiem)));
	set_clock(14, 16);
	app_on_state(SYS_CLOCK);

	/* ---- every hub row reaches its page; Back returns */
	click(app.row_sound.obj);
	CHECK(app_depth() == 1 && app_top() == APP_PAGE_SOUND);
	app_back();
	advance(20);
	click(app.row_time.obj);
	CHECK(app_top() == APP_PAGE_TIME);
	app_back();
	advance(20);
	click(app.row_storage.obj);
	CHECK(app_top() == APP_PAGE_STORAGE);
	app_back();
	advance(20);
	click(app.row_about.obj);
	CHECK(app_top() == APP_PAGE_ABOUT);
	app_back();
	advance(20);
	CHECK(lv_obj_get_child_count(content) == 1);

	/* ---- P03 Sound */
	click(app.row_sound.obj);
	page = app_top_page()->kp.obj;
	CHECK(has_label(page, "ALERT VOLUME") && has_label(page, "60%"));
	CHECK(has_label(page, "Haptics") && has_label(page, "Alert sounds") &&
	      has_label(page, "Quiet when paused") && !has_label(page, "Touch sounds"));
	CHECK(has_label(page, "Volume rocker is reserved for ride pages."));
	{
		struct sound_probe { struct kit_slider *dummy; } *unused = NULL;
		(void)unused;
	}
	/* the first slider on the page is the volume; switches in row order */
	{
		lv_obj_t *sws[3] = { NULL }, *vol = NULL;
		int ns = 0;
		uint32_t cnt;
		lv_obj_t *stack[64];
		int sp = 0;

		lv_obj_update_layout(page);
		stack[sp++] = page;
		while (sp) {
			lv_obj_t *o = stack[--sp];

			cnt = lv_obj_get_child_count(o);
			if (lv_obj_get_width(o) == KIT_SWITCH_W && lv_obj_get_height(o) == KIT_SWITCH_H &&
			    ns < 3)
				sws[ns++] = o;
			if (!vol && lv_obj_get_height(o) == 144 && lv_obj_get_width(o) > 200)
				vol = o;
			for (uint32_t i = cnt; i > 0 && sp < 63; i--)
				stack[sp++] = lv_obj_get_child(o, (int32_t)(i - 1));
		}
		CHECK(ns == 3 && vol != NULL);
		/* order of discovery is document order: Haptics, Alert sounds, Quiet */
		CHECK(kit_switch_get(sws[0]) && kit_switch_get(sws[1]) && !kit_switch_get(sws[2]));
		click(sws[2]);
		CHECK(app.set.quiet_paused && kit_switch_get(sws[2]));
		click(sws[1]);
		CHECK(!app.set.alert_sounds);
		click(sws[0]);
		CHECK(!app.set.haptics);
		app_back();
		advance(20);
		CHECK(!strcmp(lv_label_get_text(app.row_sound.sub), "Haptics off"));
	}
	click(app.row_sound.obj);
	page = app_top_page()->kp.obj;
	app_set_alert_volume(-5);
	CHECK(app.set.alert_volume == 0);
	app_apply_settings(&app.set);
	CHECK(has_label(page, "0%"));
	app_set_alert_volume(250);
	CHECK(app.set.alert_volume == 100);
	app_set_alert_volume(60);
	app_set_haptics(true);
	app_set_alert_sounds(true);
	app_set_quiet_paused(false);
	/* settings replaced from storage refresh the open page */
	n = app.set;
	n.alert_volume = 25;
	n.haptics = false;
	n.quiet_paused = true;
	app_apply_settings(&n);
	CHECK(has_label(page, "25%") && !strcmp(lv_label_get_text(app.row_sound.sub), "Haptics off"));
	n.alert_volume = 60;
	n.haptics = true;
	n.quiet_paused = false;
	app_apply_settings(&n);
	CHECK(has_label(page, "60%"));
	app_back();
	advance(20);

	/* ---- P04 Time */
	click(app.row_time.obj);
	CHECK(app_top() == APP_PAGE_TIME);
	page = app_top_page()->kp.obj;
	CHECK(has_label(page, "14:16") && has_label(page, "Thursday 8 October 2026"));
	CHECK(has_label(page, "Time source") && has_label(page, "Time zone") &&
	      has_label(page, "24-hour clock") && !has_label(page, "Set automatically"));
	CHECK(has_label(page, "IST \xC2\xB7 UTC+05:30"));
	CHECK(has_label(page, "Time source unavailable") && has_label(page, "Last sync time unknown."));
	CHECK(has_label(page, "14:16 instead of 2:16 pm"));
	/* no chevron or switch on the zone row: exactly one switch (24-hour) */
	sys_state.timeview.verified = true;
	sys_state.timeview.source = SYS_TIME_GPS;
	sys_state.timeview.reference_epoch = 1791448320;
	app_on_state(SYS_TIME_VIEW);
	CHECK(has_label(page, "Set by GPS") && has_label(page, "Last synced 14:02 via GPS."));
	sys_state.timeview.verified = false;		/* query failed: no stale claim */
	sys_state.timeview.reference_epoch = 0;
	app_on_state(SYS_TIME_VIEW);
	CHECK(!has_label(page, "Set by GPS") && has_label(page, "Last sync time unknown."));
	sys_state.timeview.boot_source = SYS_TIME_RESTORED;
	app_on_state(SYS_TIME_VIEW);
	CHECK(has_label(page, "Restored at boot, not yet verified"));
	sys_state.timeview.verified = true;
	sys_state.timeview.source = SYS_TIME_NETWORK;
	sys_state.timeview.reference_epoch = 1791448320;
	app_on_state(SYS_TIME_VIEW);
	/* the switch: 12 hour everywhere at once */
	app_set_clock_24h(false);
	CHECK(has_label(page, "2:16") && has_label(page, "pm") &&
	      has_label(page, "Last synced 2:02 pm via network."));
	CHECK(!strcmp(lv_label_get_text(app.sb.meridiem), "pm"));
	app_set_clock_24h(true);
	CHECK(has_label(page, "14:16") && has_label(page, "Last synced 14:02 via network."));
	/* a minute passes */
	set_clock(14, 17);
	app_on_state(SYS_CLOCK);
	CHECK(has_label(page, "14:17"));
	/* date rolls with the same hh:mm */
	snprintf(sys_state.clock.date, sizeof(sys_state.clock.date), "Friday 9 October 2026");
	app_on_state(SYS_CLOCK);
	CHECK(has_label(page, "Friday 9 October 2026"));
	/* a legacy fixture with only hh:mm */
	sys_state.clock.valid = false;
	snprintf(sys_state.clock.hhmm, sizeof(sys_state.clock.hhmm), "09:30");
	sys_state.clock.date[0] = 0;
	app_set_clock_24h(false);
	CHECK(has_label(page, "9:30") && has_label(page, "Date unknown"));
	app_set_clock_24h(true);
	set_clock(14, 16);
	memset(&sys_state.timeview, 0, sizeof(sys_state.timeview));
	app_on_state(SYS_CLOCK | SYS_TIME_VIEW);
	/* screen off and back: the page survives */
	pf_set(false);
	pf_set(true);
	CHECK(app_top() == APP_PAGE_TIME);
	app_back();
	advance(20);

	/* ---- P05 Storage */
	sys_state.storage.known = sys_state.storage.ours = true;
	sys_state.storage.total_bytes = 32000000000ULL;
	sys_state.storage.free_bytes = 18200000000ULL;
	click(app.row_storage.obj);
	CHECK(app_top() == APP_PAGE_STORAGE);
	page = app_top_page()->kp.obj;
	CHECK(has_label(page, "18.2") && has_label(page, "GB free of 32 GB"));
	CHECK(has_label(page, "Calculating usage...") && !has_label(page, "System"));
	CHECK(has_label(page, "Maps") && has_label(page, "Rides") && has_label(page, "Routes") &&
	      has_label(page, "Other") && has_label(page, "Free") && has_label(page, "18.2 GB"));
	CHECK(!has_label(page, "6.4 GB"));	/* nothing exact before a coherent scan */
	sys_state.storage.usage_known = true;
	sys_state.storage.usage_complete = true;
	for (int i = 0; i < 3; i++)
		sys_state.storage.usage_cat[i] = USAGE_CAT_EXACT;
	sys_state.storage.usage_maps = 6400000000ULL;
	sys_state.storage.usage_rides = 3100000000ULL;
	sys_state.storage.usage_routes = 300000000ULL;
	sys_state.storage.usage_other = 4000000000ULL;
	sys_state.storage.usage_total_bytes = 32000000000ULL;
	sys_state.storage.usage_free_bytes = 18200000000ULL;
	app_on_state(SYS_STORAGE);
	CHECK(has_label(page, "6.4 GB") && has_label(page, "3.1 GB") && has_label(page, "0.3 GB") &&
	      has_label(page, "4.0 GB") && !has_label(page, "Calculating usage..."));
	CHECK(!has_label(page, "Usage estimate; storage changed or could not be fully scanned."));
	/* the bar: segments in order, inside the track, never wider than it */
	{
		lv_obj_t *bar = NULL;
		lv_area_t ba, sa;
		int shown_segs = 0, last_x2 = -1;

		for (uint32_t i = 0; i < lv_obj_get_child_count(lv_obj_get_child(page, 1)); i++) {
			lv_obj_t *c = lv_obj_get_child(lv_obj_get_child(page, 1), (int32_t)i);

			if (lv_obj_get_height(c) == 120 + 2 * KIT_LINE)
				bar = lv_obj_get_child(c, 0);
		}
		CHECK(bar != NULL);
		if (bar) {
			lv_obj_update_layout(bar);
			lv_obj_get_coords(bar, &ba);
			for (uint32_t i = 0; i < lv_obj_get_child_count(bar); i++) {
				lv_obj_t *c = lv_obj_get_child(bar, (int32_t)i);

				if (lv_obj_is_hidden(c))
					continue;
				lv_obj_get_coords(c, &sa);
				CHECK(sa.x1 >= ba.x1 && sa.x2 <= ba.x2);
				CHECK(sa.x1 > last_x2 || last_x2 < 0);
				last_x2 = sa.x2;
				shown_segs++;
			}
			CHECK(shown_segs == 4);
		}
	}
	/* an estimate: tilde values, the K7 note */
	sys_state.storage.usage_complete = false;
	app_on_state(SYS_STORAGE);
	CHECK(has_label(page, "~6.4 GB") && has_label(page, "~4.0 GB") &&
	      has_label(page, "Usage estimate; storage changed or could not be fully scanned."));
	/* categories above the capacity: no raw plot, no exact claim */
	sys_state.storage.usage_complete = true;
	sys_state.storage.usage_maps = 40000000000ULL;
	app_on_state(SYS_STORAGE);
	CHECK(has_label(page, "Calculating usage..."));
	sys_state.storage.usage_maps = 6400000000ULL;
	/* the cheap capacity changes: the old snapshot is not mixed in */
	sys_state.storage.free_bytes = 17000000000ULL;
	app_on_state(SYS_STORAGE);
	CHECK(has_label(page, "17.0") && has_label(page, "Calculating usage...") &&
	      !has_label(page, "6.4 GB"));
	sys_state.storage.free_bytes = 18200000000ULL;
	app_on_state(SYS_STORAGE);
	CHECK(has_label(page, "6.4 GB"));
	/* a foreign /data: RAM only, no capacity claim */
	sys_state.storage.ours = false;
	app_on_state(SYS_STORAGE);
	CHECK(has_label(page, "RAM only") && has_label(page, "Nothing is kept across reboots.") &&
	      !has_label(page, "18.2"));
	sys_state.storage.known = false;
	app_on_state(SYS_STORAGE);
	CHECK(has_label(page, "storage unavailable"));
	sys_state.storage.known = sys_state.storage.ours = true;
	app_back();
	advance(20);

	/* ---- P06 About */
	snprintf(sys_state.device.image_name, sizeof(sys_state.device.image_name), "chef-cyclo");
	click(app.row_about.obj);
	CHECK(app_top() == APP_PAGE_ABOUT);
	page = app_top_page()->kp.obj;
	CHECK(has_label(page, "chef-cyclo") && has_label(page, "Build information unavailable"));
	CHECK(has_label(page, "FIRMWARE") && has_label(page, "LVGL") && has_label(page, "Kernel") &&
	      has_label(page, "Display") && has_label(page, "Serial") && has_label(page, "Bluetooth") &&
	      has_label(page, "Wi-Fi") && has_label(page, "Uptime"));
	CHECK(!has_label(page, "403 ppi"));
	sys_state.device.metadata_known = true;
	snprintf(sys_state.device.version, sizeof(sys_state.device.version), "dev");
	snprintf(sys_state.device.commit, sizeof(sys_state.device.commit),
		 "71163bd000000000000000000000000000000000");
	snprintf(sys_state.device.build_date, sizeof(sys_state.device.build_date), "2026-10-08 UTC");
	sys_state.device.dirty = true;
	snprintf(sys_state.device.lvgl, sizeof(sys_state.device.lvgl), "9.6.0");
	snprintf(sys_state.device.kernel, sizeof(sys_state.device.kernel), "Linux 4.4");
	snprintf(sys_state.device.resolution, sizeof(sys_state.device.resolution), "1080 x 2246");
	snprintf(sys_state.device.serial, sizeof(sys_state.device.serial), "SER123");
	sys_state.device.serial_known = true;
	snprintf(sys_state.device.bluetooth, sizeof(sys_state.device.bluetooth), "A4:C1:38:0F:22:91");
	sys_state.device.bluetooth_known = true;
	sys_state.device.uptime_s = 11520;
	sys_state.device.sampled_mono_ms = 0;		/* fixture: no elapsed time */
	app_on_state(SYS_DEVICE_INFO);
	CHECK(has_label(page, "chef-cyclo dev") && has_label(page, "Build 71163bd+dirty \xC2\xB7 2026-10-08 UTC"));
	CHECK(has_label(page, "9.6.0") && has_label(page, "Linux 4.4") && has_label(page, "1080 x 2246") &&
	      has_label(page, "SER123") && has_label(page, "A4:C1:38:0F:22:91") &&
	      has_label(page, "3 h 12 min"));
	CHECK(has_label(page, "Unavailable"));	/* the Wi-Fi address is unknown */
	/* the uptime timer exists only while the page is visible */
	timers = timer_count();
	pf_set(false);
	CHECK(timer_count() <= timers);
	timers = timer_count();
	pf_set(true);
	CHECK(timer_count() == timers + 1);
	app_back();
	advance(20);
	CHECK(timer_count() <= timers);
	(void)l;
	(void)sw;
	CHECK(lv_obj_get_child_count(content) == 1);
}

/* ------------------------------------------------------ haptic gestures */

static int pulses(int fd)
{
	char b[64];
	int total = 0;
	struct timespec ts = { .tv_nsec = 80000000 };

	nanosleep(&ts, NULL);		/* the worker is a real thread */
	for (;;) {
		ssize_t n = read(fd, b, sizeof(b));

		if (n <= 0)
			break;
		total += (int)n;
	}
	return total / 3;		/* each pulse writes "10\n" */
}

static void gap(void)
{
	struct timespec ts = { .tv_nsec = 130000000 };	/* past the 100 ms rate limit */

	nanosleep(&ts, NULL);
}

static void test_m4_haptics(void)
{
	struct chefui_contact c[2] = { { 0 } };
	char path[256];
	int fd;

	snprintf(path, sizeof(path), "/tmp/chefui-test-vib-%d", (int)getpid());
	unlink(path);
	CHECK(mkfifo(path, 0600) == 0);
	fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	memset(&sys_state, 0, sizeof(sys_state));
	set_clock(14, 16);
	app_on_state(~0u);
	pf_set(true);
	app_set_haptics(true);
	CHECK(sys_feedback_start(path, true) == 0);
	sys_feedback_enabled(true);
	sys_feedback_screen_allowed(true);

	/* a gesture is one pulse at its start: not on drag, a second finger or release */
	app_on_touch_frame(c, 1);
	CHECK(pulses(fd) == 1);
	app_on_touch_frame(c, 1);
	app_on_touch_frame(c, 2);
	app_on_touch_frame(c, 1);
	app_on_touch_frame(c, 0);
	CHECK(pulses(fd) == 0);
	gap();
	app_on_touch_frame(c, 1);	/* a new gesture */
	CHECK(pulses(fd) == 1);
	app_on_touch_frame(c, 0);
	/* touch still counts as activity: auto-sleep restarts */
	app_on_touch_frame(NULL, 0);
	gap();

	/* Back from a page: one pulse; the hub's power.short (screen off) none */
	click(app.row_display.obj);
	CHECK(pulses(fd) == 0);		/* opening by tap is the gesture's own pulse */
	gap();
	CHECK(app_on_button("power.short"));
	CHECK(pulses(fd) == 1);
	advance(20);
	gap();
	CHECK(!app_on_button("power.short"));	/* hub: the platform turns the screen off */
	CHECK(pulses(fd) == 0);

	/* the wake pulse comes after the screen is on, once */
	pf_set(false);
	CHECK(pulses(fd) == 0);
	gap();
	pf_set(true);
	CHECK(pulses(fd) == 1);

	/* off: no pulses, and enabling does not preview */
	gap();
	app_set_haptics(false);
	app_on_touch_frame(c, 1);
	app_on_touch_frame(c, 0);
	CHECK(pulses(fd) == 0);
	app_set_haptics(true);
	CHECK(pulses(fd) == 0);
	gap();
	app_on_touch_frame(c, 1);
	app_on_touch_frame(c, 0);
	CHECK(pulses(fd) == 1);

	sys_feedback_stop();
	close(fd);
	unlink(path);
}

/* ---------------------------------------------- Back pulse (review F2) */

static void test_m4_back_pulse(void)
{
	struct chefui_contact c[1] = { { 0 } };
	unsigned base;

	memset(&sys_state, 0, sizeof(sys_state));
	set_clock(14, 16);
	app_on_state(~0u);
	pf_set(true);
	app_set_haptics(true);
	app_on_touch_frame(c, 0);
	/* a close driven by state (a join finishing, a forgotten sensor) is silent */
	click(app.row_sound.obj);
	base = sys_feedback_test_calls();
	CHECK(app_back());
	advance(20);
	CHECK(sys_feedback_test_calls() == base);
	/* the header Back target: the touch-down pulses, the close adds none */
	click(app.row_sound.obj);
	base = sys_feedback_test_calls();
	app_on_touch_frame(c, 1);
	click(app_top_page()->kp.back);
	app_on_touch_frame(c, 0);
	advance(20);
	CHECK(app_depth() == 0 && sys_feedback_test_calls() == base + 1);
	/* power.short on a page is Back with one pulse; on the hub it is the screen toggle */
	click(app.row_sound.obj);
	base = sys_feedback_test_calls();
	CHECK(app_on_button("power.short"));
	advance(20);
	CHECK(sys_feedback_test_calls() == base + 1);
	base = sys_feedback_test_calls();
	CHECK(!app_on_button("power.short"));
	CHECK(sys_feedback_test_calls() == base);
}

/* --------------------------------- Storage page: review F3 and F10 */

static lv_obj_t *storage_bar(lv_obj_t *page)
{
	lv_obj_t *body = lv_obj_get_child(page, 1);

	for (uint32_t i = 0; body && i < lv_obj_get_child_count(body); i++) {
		lv_obj_t *c = lv_obj_get_child(body, (int32_t)i);

		if (lv_obj_get_height(c) == 120 + 2 * KIT_LINE)
			return lv_obj_get_child(c, 0);
	}
	return NULL;
}

static int shown_segments(lv_obj_t *bar, uint32_t *colour)
{
	int n = 0;

	for (uint32_t i = 0; bar && i < lv_obj_get_child_count(bar); i++) {
		lv_obj_t *c = lv_obj_get_child(bar, (int32_t)i);

		if (!lv_obj_is_hidden(c)) {
			n++;
			if (colour)
				*colour = bg(c);
		}
	}
	return n;
}

static void test_m4_storage_review(void)
{
	struct ui_store_stat_result stat = { .known = true, .ours = true, .total_bytes = 32000000000ULL,
		.free_bytes = 18200000000ULL, .root_dev = 1, .root_ino = 2 };
	struct sys_usage_result u = { .known = true, .ours = true, .complete = true,
		.total_bytes = 32000000000ULL, .free_bytes = 17000000000ULL,
		.maps_bytes = 6400000000ULL, .rides_bytes = 3100000000ULL, .routes_bytes = 300000000ULL,
		.other_bytes = 5200000000ULL, .root_dev = 1, .root_ino = 2, .mount_id = 7 };
	lv_obj_t *page, *bar;
	uint32_t col = 0;

	memset(&sys_state, 0, sizeof(sys_state));
	set_clock(14, 16);
	app_on_state(~0u);
	pf_set(true);
	click(app.row_storage.obj);
	page = app_top_page()->kp.obj;
	sys_storage_test_apply_stat(&stat);
	for (int i = 0; i < 3; i++)
		u.cat_state[i] = USAGE_CAT_EXACT;
	/* the scan's capacity (newer than the STAT) is adopted, not dropped */
	sys_storage_test_apply_usage(&u);
	app_on_state(SYS_STORAGE);
	CHECK(has_label(page, "17.0") && has_label(page, "6.4 GB") && has_label(page, "5.2 GB") &&
	      !has_label(page, "Calculating usage..."));
	bar = storage_bar(page);
	CHECK(bar != NULL && shown_segments(bar, NULL) == 4);
	/* Maps cut short, Rides and Routes never reached: estimate, "--", one neutral segment */
	u.complete = false;
	u.cat_state[0] = USAGE_CAT_PARTIAL;
	u.cat_state[1] = u.cat_state[2] = USAGE_CAT_UNSCANNED;
	u.rides_bytes = u.routes_bytes = 0;
	sys_storage_test_apply_usage(&u);
	app_on_state(SYS_STORAGE);
	CHECK(has_label(page, "~6.4 GB") && !has_label(page, "3.1 GB") && !has_label(page, "0.3 GB") &&
	      !has_label(page, "~0.0 GB") && !has_label(page, "5.2 GB"));
	CHECK(has_label(page, "Usage estimate; storage changed or could not be fully scanned."));
	CHECK(shown_segments(bar, &col) == 1 && col == kit_palette_dark.mut);
	/* an error inside Rides only: complete is false, so every value is an estimate */
	u.cat_state[0] = USAGE_CAT_EXACT;
	u.cat_state[1] = USAGE_CAT_PARTIAL;
	u.cat_state[2] = USAGE_CAT_EXACT;
	u.rides_bytes = 3100000000ULL;
	u.routes_bytes = 300000000ULL;
	u.maps_bytes = 6400000000ULL;
	sys_storage_test_apply_usage(&u);
	app_on_state(SYS_STORAGE);
	CHECK(has_label(page, "~3.1 GB") && has_label(page, "~0.3 GB") && has_label(page, "~6.4 GB"));
	CHECK(shown_segments(bar, NULL) == 4);
	/* ownership lost: the cheap fields follow, RAM only, no snapshot */
	u.ours = false;
	sys_storage_test_apply_usage(&u);
	app_on_state(SYS_STORAGE);
	CHECK(has_label(page, "RAM only") && !has_label(page, "6.4 GB"));
	app_back();
	advance(20);
}

/* ------------------------------------------------ M5: ambient light */

static struct {
	int lfd, cfd, accepts;
	bool eof, drop, claimed;
	int status_seen, status_answered;
	char in[1024];
	size_t n;
} AS = { .lfd = -1, .cfd = -1 };

static void als_say(const char *t)
{
	if (AS.cfd >= 0)
		(void)!write(AS.cfd, t, strlen(t));
}

static void als_srv_io(void)
{
	struct pollfd p;
	char buf[256];

	if (AS.lfd >= 0 && AS.cfd < 0) {
		int fd = accept4(AS.lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);

		if (fd >= 0) {
			AS.accepts++;
			if (AS.drop) {
				close(fd);
				return;
			}
			AS.cfd = fd;
			AS.n = 0;
			AS.in[0] = 0;
			AS.eof = false;
			AS.claimed = false;
			AS.status_seen = AS.status_answered = 0;
			als_say("{\"hello\":\"sensord\",\"proto\":1}\n");
		}
	}
	if (AS.cfd >= 0) {
		p = (struct pollfd){ AS.cfd, POLLIN, 0 };
		if (poll(&p, 1, 0) > 0) {
			ssize_t r = read(AS.cfd, buf, sizeof(buf));

			if (r > 0 && AS.n + (size_t)r < sizeof(AS.in)) {
				memcpy(AS.in + AS.n, buf, (size_t)r);
				AS.n += (size_t)r;
				AS.in[AS.n] = 0;
				if (strstr(AS.in, "claim illuminance 5\n") && !AS.claimed) {
					AS.claimed = true;
					als_say("{\"ok\":\"claim\",\"sensor\":\"illuminance\",\"rate\":5}\n");
				}
				for (char *q = AS.in; (q = strstr(q, "status\n")); q += 7)
					AS.status_seen++;
				if (AS.status_seen > AS.status_answered) {
					AS.status_answered = AS.status_seen;
					als_say("{\"ok\":\"status\",\"smgr\":\"ready\"}\n");
				}
			} else if (r == 0) {
				AS.eof = true;
				close(AS.cfd);
				AS.cfd = -1;
			}
		}
	}
}

static void als_adv(int ms)
{
	for (int i = 0; i < ms; i += 10) {
		advance(10);
		als_srv_io();
	}
}

/* steady lux for ms at 5 Hz */
static void lux_feed(double lux, int ms)
{
	char line[160];

	for (int i = 0; i < ms; i += 200) {
		/* stamped now (CLOCK_MONOTONIC ns): a stamp before the claim is dropped as cached */
		snprintf(line, sizeof(line), "{\"sensor\":\"illuminance\",\"t\":%lld,\"illuminance\":%g}\n",
			 (long long)mono_now() * 1000000, lux);
		als_say(line);
		als_adv(200);
	}
}

static bool file_has(const char *path, const char *text)
{
	char buf[1024];
	ssize_t n;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return false;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return false;
	buf[n] = 0;
	return strstr(buf, text) != NULL;
}

static bool store_to_mount(const char *p)
{
	(void)p;
	return true;
}

static bool saved_gen(void *arg)
{
	(void)arg;
	return ui_store_persisted_gen() >= ui_store_wanted_gen();
}

static void m5_state(unsigned what, void *user)
{
	(void)user;
	app_on_state(what);
}

static void test_m5_als(void)
{
	char dir[64], sock[100], p[200], runp[200], file[220];
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	struct ui_store_paths paths = { .mountpoint = store_to_mount, .exit_wait_ms = 300,
					.probe_ms = 500, .load_wait_ms = 300 };
	struct ui_settings s;
	struct display_page *d;
	int before, calls;

	snprintf(dir, sizeof(dir), "/tmp/m5als.XXXXXX");
	CHECK(mkdtemp(dir) != NULL);
	snprintf(sock, sizeof(sock), "%s/s.sock", dir);
	snprintf(p, sizeof(p), "%s/data", dir);
	mkdir(p, 0755);
	snprintf(p, sizeof(p), "%s/data/v1", dir);
	mkdir(p, 0755);
	snprintf(p, sizeof(p), "%s/data/chef-layout", dir);
	{
		int fd = open(p, O_WRONLY | O_CREAT, 0644);

		CHECK(fd >= 0 && write(fd, "chef-cyclo-data-v1\n", 19) == 19);
		close(fd);
	}
	snprintf(p, sizeof(p), "%s/data", dir);
	snprintf(runp, sizeof(runp), "%s/run", dir);
	mkdir(runp, 0755);
	paths.data = p;
	paths.run = runp;
	snprintf(file, sizeof(file), "%s/data/v1/ui/settings", dir);
	ui_store_set_paths(&paths);
	ui_settings_defaults(&s);
	CHECK(ui_store_load(&s) == 0);

	/* the fake sensord */
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sock);
	AS.lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	CHECK(bind(AS.lfd, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(AS.lfd, 4) == 0);
	CHECK(sys_als_start(sock, NULL) == 0);
	sys_on_change(m5_state, NULL);

	/* clean start: defaults, never sleep, the hub */
	ui_settings_defaults(&s);
	s.sleep_s = 0;
	while (app_back())
		advance(20);
	advance(50);
	app.persist = false;
	app_apply_settings(&s);
	screen_on = true;
	app.persist = true;
	fakeloop_log_clear();

	/* both off: no connection at all */
	als_adv(5000);
	CHECK(!sys_als_wanted() && AS.accepts == 0);

	/* Auto brightness on: one claim, backlight stays manual until a sample */
	before = last_level;
	app_set_auto_brightness(true);
	CHECK(sys_als_wanted() && app.set.auto_brightness);
	als_adv(100);
	CHECK(AS.accepts == 1 && !strncmp(AS.in, "claim illuminance 5\n", 20) && last_level == before);
	CHECK(!strcmp(lv_label_get_text(app.row_display.sub), "Auto \xC2\xB7 40%"));
	{	/* L3: before any sample a late /data apply writes the manual level */
		struct ui_settings t = app.set;

		t.brightness = 60;
		app_apply_settings(&t);
		CHECK(last_level == ui_brightness_level(60));
		t.brightness = 40;
		app_apply_settings(&t);
		CHECK(last_level == ui_brightness_level(40));
	}
	lux_feed(300, 1000);
	CHECK(last_level == ui_brightness_level(30) && app_shown_brightness() == 30);
	CHECK(app.set.brightness == 40);	/* the manual value is untouched */
	als_adv(1100);
	CHECK(!strcmp(lv_label_get_text(app.row_display.sub), "Auto \xC2\xB7 30%"));
	/* brightening follows the light */
	lux_feed(3000, 8000);
	CHECK(app_shown_brightness() > 60 && last_level == ui_brightness_level(app_shown_brightness()));
	lux_feed(300, 60000);
	CHECK(abs(app_shown_brightness() - 30) <= 1);	/* inside the deadband */
	{	/* M1: the hub subtitle after a change seen off the hub */
		char want[32];

		als_adv(1500);
		click(app.row_display.obj);
		lux_feed(400, 30000);
		app_back();
		advance(50);
		lux_feed(300, 60000);
		als_adv(1500);
		snprintf(want, sizeof(want), "Auto \xC2\xB7 %d%%", app_shown_brightness());
		CHECK(!strcmp(lv_label_get_text(app.row_display.sub), want));
	}

	/* L3: a late /data apply keeps the Auto level once a sample set it */
	{
		struct ui_settings t = app.set;
		int calls0 = level_calls, last0 = last_level;

		t.brightness = 77;
		app_apply_settings(&t);
		CHECK(level_calls == calls0 && last_level == last0 && app.set.brightness == 77);
		t.brightness = 40;
		app_apply_settings(&t);
		CHECK(app.set.brightness == 40 && last_level == last0);
	}

	/* the Display page shows the live level; a drag sets the offset and persists */
	click(app.row_display.obj);
	d = app_display_page();
	CHECK(d && kit_switch_get(d->auto_switch) && d->slider.value == app_shown_brightness());
	{
		char t[8];

		snprintf(t, sizeof(t), "%d%%", app_shown_brightness());
		CHECK(!strcmp(lv_label_get_text(d->value), t));
	}
	CHECK(!strcmp(lv_label_get_text(d->auto_sub), "Follows the light sensor"));
	kit_slider_set(&d->slider, 50);
	lv_obj_send_event(d->slider.obj, LV_EVENT_VALUE_CHANGED, NULL);
	CHECK(app.set.auto_offset == 20 && app.set.brightness == 40);
	CHECK(last_level == ui_brightness_level(50));
	calls = level_calls;
	lux_feed(300, 3000);
	CHECK(level_calls == calls && last_level == ui_brightness_level(50));	/* the curve plus offset */
	{
		long t0 = (long)time(NULL);

		while (!saved_gen(NULL) && time(NULL) - t0 < 5) {
			advance(50);
			fakeloop_run_for(2);
		}
	}
	CHECK(saved_gen(NULL));
	CHECK(file_has(file, "auto_brightness 1\nauto_offset 20\n"));
	/* every slider position is curve + offset (-95..95): a drag to 100 on a 30 % curve
	 * is offset 70 and the level stays under the finger through the ticks (L2) */
	kit_slider_set(&d->slider, 100);
	lv_obj_send_event(d->slider.obj, LV_EVENT_VALUE_CHANGED, NULL);
	CHECK(app.set.auto_offset == 100 - sys_als_curve_now());
	CHECK(app.set.auto_offset > 50 && last_level == ui_brightness_level(100));
	calls = level_calls;
	lux_feed(300, 3000);
	CHECK(level_calls == calls && last_level == ui_brightness_level(100) && app_shown_brightness() == 100);
	kit_slider_set(&d->slider, 5);
	lv_obj_send_event(d->slider.obj, LV_EVENT_VALUE_CHANGED, NULL);
	CHECK(app.set.auto_offset == 5 - sys_als_curve_now() && last_level == ui_brightness_level(5));
	calls = level_calls;
	lux_feed(300, 3000);
	CHECK(level_calls == calls && app_shown_brightness() == 5);
	kit_slider_set(&d->slider, 50);
	lv_obj_send_event(d->slider.obj, LV_EVENT_VALUE_CHANGED, NULL);
	CHECK(app.set.auto_offset == 20);

	/* a drag in progress is never overridden by the live level */
	lv_obj_add_state(d->slider.track, LV_STATE_PRESSED);
	kit_slider_set(&d->slider, 77);
	lux_feed(3000, 3000);
	CHECK(d->slider.value == 77);
	lv_obj_remove_state(d->slider.track, LV_STATE_PRESSED);
	lux_feed(3000, 1200);
	CHECK(d->slider.value == app_shown_brightness());
	lux_feed(300, 60000);

	/* Auto off: the manual brightness at once, the offset kept, the claim gone */
	kit_switch_set(d->auto_switch, false);
	lv_obj_send_event(d->auto_switch, LV_EVENT_VALUE_CHANGED, NULL);
	CHECK(!app.set.auto_brightness && last_level == ui_brightness_level(40) && app.set.auto_offset == 20);
	CHECK(d->slider.value == 40 && !strcmp(lv_label_get_text(d->value), "40%"));
	als_adv(200);
	CHECK(AS.eof && !sys_als_wanted());
	CHECK(!strcmp(lv_label_get_text(app.row_display.sub), "Dark \xC2\xB7 40%"));
	/* and on again: the stored offset applies from the first sample */
	kit_switch_set(d->auto_switch, true);
	lv_obj_send_event(d->auto_switch, LV_EVENT_VALUE_CHANGED, NULL);
	als_adv(100);
	lux_feed(300, 600);
	CHECK(app_shown_brightness() == 50 && last_level == ui_brightness_level(50));

	/* Auto theme on top: still one claim, one connection */
	AS.accepts = 0;
	click(d->theme.cell[2]);
	CHECK(app.set.theme == UI_THEME_AUTO && kit_theme_get() == KIT_THEME_DARK);
	als_adv(200);
	CHECK(AS.accepts == 0 && sys_als_wanted());
	kit_switch_set(d->auto_switch, false);
	lv_obj_send_event(d->auto_switch, LV_EVENT_VALUE_CHANGED, NULL);
	als_adv(200);
	CHECK(sys_als_wanted());	/* theme Auto alone keeps the claim */

	/* L1: a sensor that stopped being wanted does not count as lost; turning Auto
	 * brightness on later gives the 3 s grace, not an instant "not available" */
	click(d->theme.cell[0]);
	als_adv(4000);
	CHECK(!sys_als_wanted());
	AS.drop = true;
	kit_switch_set(d->auto_switch, true);
	lv_obj_send_event(d->auto_switch, LV_EVENT_VALUE_CHANGED, NULL);
	CHECK(!app_als_unavailable() && !strcmp(lv_label_get_text(d->auto_sub), "Follows the light sensor"));
	als_adv(2000);
	CHECK(!app_als_unavailable());
	als_adv(1500);
	CHECK(app_als_unavailable());
	AS.drop = false;
	kit_switch_set(d->auto_switch, false);
	lv_obj_send_event(d->auto_switch, LV_EVENT_VALUE_CHANGED, NULL);
	als_adv(1000);

	/* selecting Auto from Light with no sample yet changes the effective theme: logged */
	click(d->theme.cell[1]);
	CHECK(kit_theme_get() == KIT_THEME_LIGHT);
	fakeloop_log_clear();
	click(d->theme.cell[2]);
	CHECK(kit_theme_get() == KIT_THEME_DARK &&
	      fakeloop_log_count("theme auto -> dark (selected, no sample yet)") == 1);
	click(d->theme.cell[0]);
	fakeloop_log_clear();
	click(d->theme.cell[2]);
	CHECK(fakeloop_log_count("theme auto ->") == 0);	/* Dark stays Dark: no line */

	/* theme Auto: 1500 lux holds Dark, 3000 lux for 5 s flips to Light, logged once */
	click(d->theme.cell[0]);
	click(d->theme.cell[2]);
	als_adv(2000);
	lux_feed(1500, 6000);
	CHECK(kit_theme_get() == KIT_THEME_DARK && fakeloop_log_count("theme auto") == 0);
	lux_feed(3000, 4600);
	CHECK(kit_theme_get() == KIT_THEME_DARK);
	lux_feed(3000, 800);
	CHECK(kit_theme_get() == KIT_THEME_LIGHT);
	CHECK(fakeloop_log_count("theme auto -> light (lux 3000, held 5.0 s)") == 1);
	lux_feed(3000, 20000);
	CHECK(fakeloop_log_count("theme auto") == 1);

	/* screen off drops the claim; a wake claims again, with a wake grace: no Dark flash */
	AS.accepts = 0;
	pf_set(false);
	als_adv(300);
	CHECK(AS.eof && !sys_als_wanted());
	pf_set(true);
	als_adv(200);
	CHECK(sys_als_wanted() && AS.accepts == 1);
	lux_feed(1500, 1200);
	CHECK(kit_theme_get() == KIT_THEME_LIGHT);	/* in between: kept */
	pf_set(false);
	als_adv(300);
	pf_set(true);
	als_adv(200);
	lux_feed(700, 1000);
	CHECK(kit_theme_get() == KIT_THEME_LIGHT);	/* wake grace: low readings do not decide Dark */
	lux_feed(700, 22000);
	CHECK(kit_theme_get() == KIT_THEME_LIGHT);	/* 5 s grace plus the 20 s hold are not over */
	lux_feed(700, 3000);
	CHECK(kit_theme_get() == KIT_THEME_DARK);
	CHECK(fakeloop_log_count("theme auto -> dark (lux 700, held 20.0 s)") == 1);

	/* Auto brightness again, then the sensor goes away: the note after 3 s, bounded retries */
	kit_switch_set(d->auto_switch, true);
	lv_obj_send_event(d->auto_switch, LV_EVENT_VALUE_CHANGED, NULL);
	als_adv(200);
	lux_feed(300, 1000);
	CHECK(!strcmp(lv_label_get_text(d->auto_sub), "Follows the light sensor"));
	AS.drop = true;
	if (AS.cfd >= 0) {
		close(AS.cfd);
		AS.cfd = -1;
	}
	AS.accepts = 0;
	als_adv(1500);
	CHECK(!strcmp(lv_label_get_text(d->auto_sub), "Follows the light sensor"));	/* not yet 3 s */
	als_adv(3000);
	CHECK(app_als_unavailable() && !strcmp(lv_label_get_text(d->auto_sub), "Light sensor not available"));
	als_adv(60000);
	CHECK(AS.accepts >= 4 && AS.accepts <= 7);	/* the 1, 2, 4, 8, 16, 30 s schedule */
	AS.drop = false;
	for (int i = 0; i < 600 && !(AS.cfd >= 0 && AS.n > 0); i++)
		als_adv(100);
	CHECK(AS.cfd >= 0 && AS.n > 0);
	lux_feed(300, 1000);
	CHECK(!app_als_unavailable() && !strcmp(lv_label_get_text(d->auto_sub), "Follows the light sensor"));

	/* clean up: all off, settings flushed */
	app_set_theme(UI_THEME_DARK);
	app_set_auto_brightness(false);
	als_adv(200);
	CHECK(!sys_als_wanted());
	CHECK(ui_store_flush() >= 0);
	app.persist = false;
	sys_als_stop();
	while (app_back())
		advance(20);
	advance(50);
	if (AS.cfd >= 0)
		close(AS.cfd);
	close(AS.lfd);
}

int main(void)
{
	lv_obj_t *scr;

	signal(SIGPIPE, SIG_IGN);	/* the fake sensord writes to clients that went away */
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
	test_gps_pages(scr);
	test_bt_pages();
	test_m4_fonts_text();
	test_m4_pages();
	test_m4_haptics();
	test_m4_back_pulse();
	test_m4_storage_review();
	test_m5_als();
	return check_report("test-app");
}
