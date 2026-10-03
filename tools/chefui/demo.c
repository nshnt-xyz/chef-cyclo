/*
 * chefui-demo - verification vehicle for the chefui platform (not a
 * product): colour swatches, corner targets, a tap counter, a brightness
 * slider, one dot per active contact, gesture/screen/button lines, the
 * CHEFUI_STATS line, and a "switch" button that hands the screen to a
 * second instance (--peer) and back, for checking handoff blinks and lock
 * inheritance. The same source builds for the phone and the PC.
 *
 * usage: chefui-demo [-t secs] [-a] [-F] [-r 0|90|180|270] [-f 30|60]
 *                    [-b brightness] [--dark] [--peer]
 *                    [--safe-top pixels] [--calibrate-notch]
 *   -t  auto-exit after secs        -a  spinner + moving bar
 *   -F  full-screen invalidation every frame
 *   -r  rotation                    -f  frame target (default 60; 30 supported)
 *   --dark  the switch hands off with the screen off
 *   --peer  run as the second instance (set by the switch button)
 */
#define _GNU_SOURCE
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "chefui.h"

static struct {
	int secs, anim, full, rot, fps, bright, dark, peer, safe_top, calibrate;
} opt = { .rot = 0, .fps = 60, .bright = 96 };

static lv_obj_t *lbl_clock, *lbl_tap, *lbl_gest, *lbl_screen, *lbl_btn, *lbl_stats, *slider;
static lv_obj_t *dots[CHEFUI_MAX_CONTACTS];
static int taps;
static lv_obj_t *guide, *guide_text;

static void calibration_update(void);

static void usage(void)
{
	fprintf(stderr, "usage: chefui-demo [-t secs] [-a] [-F] [-r 0|90|180|270] [-f 30|60] "
		"[-b brightness] [--dark] [--peer] [--safe-top pixels] [--calibrate-notch]\n");
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, const char *text)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_set_style_text_font(l, font, 0);
	lv_label_set_text(l, text);
	return l;
}

/* ---------------------------------------------------------- callbacks */

static void tap_cb(lv_event_t *e)
{
	(void)e;
	lv_label_set_text_fmt(lbl_tap, "tapped %d", ++taps);
	chefui_log("tap %d", taps);
}

static void bright_cb(lv_event_t *e)
{
	int v = lv_slider_get_value(lv_event_get_target(e));

	chefui_brightness_set(v);
}

static void switch_cb(lv_event_t *e)
{
	static char self[256], rot[8], fps[8], secs[16], bright[8], inset[16];
	char *argv[24];
	int n = 0;
	ssize_t len;

	(void)e;
	len = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (len <= 0) {
		lv_label_set_text(lbl_btn, "switch: cannot find myself");
		return;
	}
	self[len] = '\0';
	snprintf(rot, sizeof(rot), "%d", opt.rot);
	snprintf(fps, sizeof(fps), "%d", opt.fps);
	snprintf(bright, sizeof(bright), "%d", chefui_brightness_get());
	argv[n++] = self;
	argv[n++] = "-r";
	argv[n++] = rot;
	argv[n++] = "-f";
	argv[n++] = fps;
	argv[n++] = "-b";
	argv[n++] = bright;
	snprintf(inset, sizeof(inset), "%d", chefui_safe_top_get());
	argv[n++] = "--safe-top";
	argv[n++] = inset;
	if (opt.calibrate)
		argv[n++] = "--calibrate-notch";
	if (opt.secs) {
		snprintf(secs, sizeof(secs), "%d", opt.secs);
		argv[n++] = "-t";
		argv[n++] = secs;
	}
	if (opt.anim)
		argv[n++] = "-a";
	if (opt.full)
		argv[n++] = "-F";
	if (opt.dark)
		argv[n++] = "--dark";
	if (!opt.peer)
		argv[n++] = "--peer";
	argv[n] = NULL;
	chefui_handoff_exec(self, argv, opt.dark != 0);
	lv_label_set_text(lbl_btn, "switch failed (see kmsg)");
}

static const char *dir_name(lv_dir_t d)
{
	switch (d) {
	case LV_DIR_LEFT: return "left";
	case LV_DIR_RIGHT: return "right";
	case LV_DIR_TOP: return "up";
	case LV_DIR_BOTTOM: return "down";
	default: return "?";
	}
}

static void gesture_cb(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_gesture_type_t type;

	if (code != LV_EVENT_GESTURE)
		return;
	type = lv_event_get_gesture_type(e);
	switch (type) {
	case LV_INDEV_GESTURE_PINCH:
		lv_label_set_text_fmt(lbl_gest, "gesture: pinch %.2f", (double)lv_event_get_pinch_scale(e));
		break;
	case LV_INDEV_GESTURE_ROTATE:
		lv_label_set_text_fmt(lbl_gest, "gesture: rotate %.2f rad", (double)lv_event_get_rotation(e));
		break;
	case LV_INDEV_GESTURE_TWO_FINGERS_SWIPE:
		lv_label_set_text_fmt(lbl_gest, "gesture: two-finger swipe %s",
				      dir_name(lv_event_get_two_fingers_swipe_dir(e)));
		break;
	default:
		lv_label_set_text_fmt(lbl_gest, "gesture: swipe %s",
				      dir_name(lv_indev_get_gesture_dir(lv_indev_active())));
		break;
	}
}

static void touch_cb(const struct chefui_contact *c, int n, void *user)
{
	int i;

	(void)user;
	for (i = 0; i < CHEFUI_MAX_CONTACTS; i++) {
		if (i < n) {
			lv_obj_set_pos(dots[i], c[i].x - 40, c[i].y - 40);
			lv_obj_set_hidden(dots[i], false);
		} else {
			lv_obj_set_hidden(dots[i], true);
		}
	}
}

static void screen_cb(bool on, void *user)
{
	(void)user;
	lv_label_set_text_fmt(lbl_screen, "screen: %s", on ? "on" : "off");
}

static bool button_cb(const char *g, void *user)
{
	int b = chefui_brightness_get();

	(void)user;
	lv_label_set_text_fmt(lbl_btn, "button: %s", g);
	if (strcmp(g, "volup.short") == 0 || strcmp(g, "voldown.short") == 0) {
		if (opt.calibrate) {
			int v = chefui_safe_top_get() + (g[3] == 'u' ? 4 : -4);
			if (v >= 0 && chefui_safe_top_set(v ? v : -1) == 0)
				calibration_update();
			return true;
		}
		b += g[3] == 'u' ? 25 : -25;
		b = b < 5 ? 5 : b > 255 ? 255 : b;
		chefui_brightness_set(b);
		lv_slider_set_value(slider, b, LV_ANIM_OFF);
		return true;
	}
	return false;	/* power.short: the default screen toggle */
}

static void clock_tick(lv_timer_t *t)
{
	time_t now = time(NULL);
	struct tm tm;
	const char *st = chefui_stats_text();

	(void)t;
	localtime_r(&now, &tm);
	lv_label_set_text_fmt(lbl_clock, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
	if (st[0])
		lv_label_set_text(lbl_stats, st);
}

static void quit_tick(lv_timer_t *t)
{
	(void)t;
	chefui_quit(0);
}

static void full_tick(lv_timer_t *t)
{
	(void)t;
	lv_obj_invalidate(lv_screen_active());
}

static void anim_x(void *o, int32_t v)
{
	lv_obj_set_x(o, v);
}

/* ----------------------------------------------------------------- UI */

static void calibration_update(void)
{
	lv_area_t a = chefui_safe_area();
	int n = chefui_safe_top_get();
	int w = lv_display_get_horizontal_resolution(lv_display_get_default());
	int h = lv_display_get_vertical_resolution(lv_display_get_default());
	/* Green boundary marks where safe content starts. Always full-panel,
	 * including in landscape; the purple strip is the reserved clearance. */
	switch (opt.rot) {
	case 90: lv_obj_set_pos(guide, 0, 0); lv_obj_set_size(guide, n, h); break;
	case 180: lv_obj_set_pos(guide, 0, a.y2 + 1); lv_obj_set_size(guide, w, n); break;
	case 270: lv_obj_set_pos(guide, a.x2 + 1, 0); lv_obj_set_size(guide, n, h); break;
	default: lv_obj_set_pos(guide, 0, 0); lv_obj_set_size(guide, w, n); break;
	}
	lv_obj_set_hidden(guide, n == 0);
	lv_label_set_text_fmt(guide_text,
		"physical top clearance: %d px\nUp/Down: +/-4 px; place content below notch\ndefault 96 px: visually calibrated 2026-10-03", n);
	chefui_log("notch calibration: --safe-top %d physical pixels (not saved)", n);
}

static void build(void)
{
	lv_obj_t *scr = chefui_root(), *content = chefui_content_root(), *o, *l;
	lv_area_t safe = chefui_safe_area();
	int32_t w = safe.x2 - safe.x1 + 1;
	static const uint32_t sw[] = { 0xff0000, 0x00ff00, 0x0000ff };
	static const lv_align_t corners[] = { LV_ALIGN_TOP_LEFT, LV_ALIGN_TOP_RIGHT,
					      LV_ALIGN_BOTTOM_LEFT, LV_ALIGN_BOTTOM_RIGHT };
	int i;

	lv_obj_set_style_bg_color(scr, opt.peer ? lv_color_hex(0x102040) : lv_color_black(), 0);
	lv_obj_set_style_text_color(scr, lv_color_white(), 0);
	lv_obj_set_scrollable(scr, false);
	lv_obj_add_event_cb(scr, gesture_cb, LV_EVENT_GESTURE, NULL);

	lbl_clock = label(content, &lv_font_montserrat_48, "--:--:--");
	lv_obj_align(lbl_clock, LV_ALIGN_TOP_MID, 0, 100);

	/* colour check: must read red, green, blue */
	for (i = 0; i < 3; i++) {
		o = lv_obj_create(content);
		lv_obj_set_size(o, 220, 100);
		lv_obj_set_style_bg_color(o, lv_color_hex(sw[i]), 0);
		lv_obj_set_style_border_width(o, 0, 0);
		lv_obj_set_style_radius(o, 0, 0);
		lv_obj_align(o, LV_ALIGN_TOP_MID, (i - 1) * 260, 190);
		lv_obj_set_clickable(o, false);
	}

	o = lv_button_create(content);
	lv_obj_set_size(o, 520, 160);
	lv_obj_align(o, LV_ALIGN_TOP_MID, 0, 320);
	lbl_tap = label(o, &lv_font_montserrat_48, "TAP ME");
	lv_obj_center(lbl_tap);
	lv_obj_add_event_cb(o, tap_cb, LV_EVENT_CLICKED, NULL);

	slider = lv_slider_create(content);
	lv_obj_set_size(slider, w > 900 ? 800 : w - 200, 50);
	lv_slider_set_range(slider, 5, 255);
	lv_slider_set_value(slider, opt.bright, LV_ANIM_OFF);
	lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, 530);
	lv_obj_add_event_cb(slider, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);

	o = lv_button_create(content);
	lv_obj_set_size(o, 440, 110);
	lv_obj_align(o, LV_ALIGN_TOP_MID, 0, 630);
	lv_obj_set_style_bg_color(o, lv_color_hex(0x606060), 0);
	l = label(o, &lv_font_montserrat_28, opt.peer ? "switch back" : "switch to peer");
	lv_obj_center(l);
	lv_obj_add_event_cb(o, switch_cb, LV_EVENT_CLICKED, NULL);

	l = label(content, &lv_font_montserrat_28, opt.peer ? "chefui-demo (peer)" : "chefui-demo");
	lv_obj_align(l, LV_ALIGN_TOP_LEFT, 110, 770);
	lbl_gest = label(content, &lv_font_montserrat_28, "gesture: -");
	lv_obj_align(lbl_gest, LV_ALIGN_TOP_LEFT, 110, 815);
	lbl_screen = label(content, &lv_font_montserrat_28, "screen: on");
	lv_obj_align(lbl_screen, LV_ALIGN_TOP_LEFT, 110, 860);
	lbl_btn = label(content, &lv_font_montserrat_28, "button: -");
	lv_obj_align(lbl_btn, LV_ALIGN_TOP_LEFT, 110, 905);
	lbl_stats = label(content, &lv_font_montserrat_14, "");
	lv_obj_align(lbl_stats, LV_ALIGN_BOTTOM_MID, 0, -100);

	if (opt.calibrate) {
		guide = lv_obj_create(scr);
		lv_obj_remove_style_all(guide);
		lv_obj_set_style_bg_color(guide, lv_color_hex(0xc040c0), 0);
		lv_obj_set_style_bg_opa(guide, LV_OPA_40, 0);
		lv_obj_set_style_border_color(guide, lv_color_hex(0x00ff00), 0);
		lv_obj_set_style_border_width(guide, 3, 0);
		lv_obj_set_clickable(guide, false);
		lv_obj_set_scrollable(guide, false);
		guide_text = label(content, &lv_font_montserrat_28, "");
		lv_obj_align(guide_text, LV_ALIGN_TOP_MID, 0, 4);
		calibration_update();
	}

	/* corner targets for the touch mapping check */
	for (i = 0; i < 4; i++) {
		o = lv_obj_create(scr);
		lv_obj_set_size(o, 90, 90);
		lv_obj_set_style_bg_color(o, lv_color_hex(0xffff00), 0);
		lv_obj_set_style_border_width(o, 0, 0);
		lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
		lv_obj_align(o, corners[i], 0, 0);
		lv_obj_set_clickable(o, false);
	}

	if (opt.anim) {
		lv_anim_t a;

		o = lv_spinner_create(content);
		lv_obj_set_size(o, 200, 200);
		lv_obj_align(o, LV_ALIGN_BOTTOM_RIGHT, -140, -380);
		o = lv_obj_create(content);
		lv_obj_set_size(o, 200, 120);
		lv_obj_set_style_bg_color(o, lv_color_hex(0x00c0ff), 0);
		lv_obj_set_style_border_width(o, 0, 0);
		lv_obj_set_clickable(o, false);
		lv_obj_align(o, LV_ALIGN_BOTTOM_LEFT, 0, -200);
		lv_anim_init(&a);
		lv_anim_set_var(&a, o);
		lv_anim_set_exec_cb(&a, anim_x);
		lv_anim_set_values(&a, 0, w - 200);
		lv_anim_set_duration(&a, 1500);
		lv_anim_set_reverse_duration(&a, 1500);
		lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
		lv_anim_start(&a);
	}

	/* one dot per active contact, above everything else */
	for (i = 0; i < CHEFUI_MAX_CONTACTS; i++) {
		dots[i] = lv_obj_create(lv_layer_top());
		lv_obj_set_size(dots[i], 80, 80);
		lv_obj_set_style_radius(dots[i], LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(dots[i], lv_palette_main((lv_palette_t)(i % 16)), 0);
		lv_obj_set_style_border_width(dots[i], 0, 0);
		lv_obj_set_clickable(dots[i], false);
		lv_obj_set_hidden(dots[i], true);
	}
}

int main(int argc, char **argv)
{
	static const struct option lopts[] = {
		{ "dark", no_argument, NULL, 'D' },
		{ "peer", no_argument, NULL, 'P' },
		{ "safe-top", required_argument, NULL, 'S' },
		{ "calibrate-notch", no_argument, NULL, 'C' },
		{ NULL, 0, NULL, 0 },
	};
	static const char *const claims[] = { "volup.short", "voldown.short", NULL };
	struct chefui_config cfg;
	int c;

	while ((c = getopt_long(argc, argv, "t:aFr:f:b:", lopts, NULL)) != -1) {
		switch (c) {
		case 't': opt.secs = atoi(optarg); break;
		case 'a': opt.anim = 1; break;
		case 'F': opt.full = 1; break;
		case 'r': opt.rot = atoi(optarg); break;
		case 'f': opt.fps = atoi(optarg); break;
		case 'b': opt.bright = atoi(optarg); break;
		case 'D': opt.dark = 1; break;
		case 'P': opt.peer = 1; break;
		case 'S': {
			char *end;
			long value = strtol(optarg, &end, 10);
			if (!*optarg || *end || value < 0 || value > 2245) {
				usage(); return 64;
			}
			opt.safe_top = value ? (int)value : -1;
			break;
		}
		case 'C': opt.calibrate = 1; break;
		default: usage(); return 64;
		}
	}
	if ((opt.fps != 30 && opt.fps != 60) || opt.bright < 1 || opt.bright > 255 || opt.secs < 0) {
		usage();
		return 64;
	}
	memset(&cfg, 0, sizeof(cfg));
	cfg.app_name = opt.peer ? "demo-peer" : "demo";
	cfg.refresh_ms = opt.fps == 60 ? 16 : 33;
	cfg.rotation = opt.rot;
	cfg.brightness = opt.bright;
	cfg.claims = claims;
	cfg.safe_top_px = opt.safe_top;
	if (!chefui_init(&cfg))
		return 1;
	chefui_on_button(button_cb, NULL);
	chefui_on_touch(touch_cb, NULL);
	chefui_on_screen(screen_cb, NULL);
	build();
	screen_cb(chefui_screen_is_on(), NULL);
	clock_tick(NULL);
	lv_timer_create(clock_tick, 1000, NULL);
	if (opt.secs)
		lv_timer_create(quit_tick, (uint32_t)opt.secs * 1000, NULL);
	if (opt.full)
		lv_timer_create(full_tick, cfg.refresh_ms, NULL);
	return chefui_run();
}
