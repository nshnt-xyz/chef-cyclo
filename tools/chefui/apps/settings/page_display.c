/*
 * page_display.c - the Display page (P02): brightness, theme and
 * auto-sleep, stored in the UI settings through app.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "fonts.h"
#include "icons.h"
#include "pages.h"

static const int sleep_values[] = { 30, 60, 300, 0 };

static void display_value_text(struct display_page *d)
{
	char b[8];

	snprintf(b, sizeof(b), "%d%%", app_shown_brightness());
	if (strcmp(lv_label_get_text(d->value), b))
		lv_label_set_text(d->value, b);
}

static void slider_changed(lv_event_t *e)
{
	struct display_page *d = lv_event_get_user_data(e);

	app_set_brightness(d->slider.value);
	display_value_text(d);
}

static void auto_switch_changed(lv_event_t *e)
{
	struct display_page *d = lv_event_get_user_data(e);

	app_set_auto_brightness(kit_switch_get(d->auto_switch));
}

/* The live level, the switch and the sensor note follow Auto brightness; a
 * drag in progress is never overridden. */
static void display_live(struct page *p)
{
	struct display_page *d = p->priv;

	int shown = app_shown_brightness();
	const char *note = app_als_unavailable() ? "Light sensor not available" : "Follows the light sensor";

	if (!lv_obj_has_state(d->slider.track, LV_STATE_PRESSED) && d->slider.value != shown)
		kit_slider_set(&d->slider, shown);
	display_value_text(d);
	if (kit_switch_get(d->auto_switch) != app.set.auto_brightness)
		kit_switch_set(d->auto_switch, app.set.auto_brightness);
	if (strcmp(lv_label_get_text(d->auto_sub), note))
		lv_label_set_text(d->auto_sub, note);
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

static void build_display(struct page *p, const void *arg)
{
	static const char *const theme_labels[] = { "Dark", "Light", "Auto" };
	static const char *const theme_glyphs[] = { KIT_IC_MOON, KIT_IC_SUN, KIT_IC_SUN_MOON };
	static const char *const sleep_labels[] = { "30 s", "1 min", "5 min", "Never" };
	struct display_page *d = calloc(1, sizeof(*d));
	lv_obj_t *body = p->kp.body, *head, *l, *row, *col, *gap;
	int i;

	(void)arg;
	p->priv = d;

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
	d->auto_title = kit_text(col, &chefkit_b500_56, 62, KIT_INK_FG, "Auto brightness");
	d->auto_sub = kit_text(col, &chefkit_b400_42, 0, KIT_INK_MUT, "Follows the light sensor");
	d->auto_switch = kit_switch_create(row);
	kit_switch_set(d->auto_switch, app.set.auto_brightness);
	lv_obj_add_event_cb(d->auto_switch, auto_switch_changed, LV_EVENT_VALUE_CHANGED, d);

	kit_section(body, "THEME", 72, 28);
	kit_seg_create(&d->theme, body, 3, theme_labels, theme_glyphs);
	kit_seg_set_active(&d->theme, app.set.theme == UI_THEME_LIGHT ? 1
			   : app.set.theme == UI_THEME_AUTO ? 2 : 0);
	lv_obj_add_event_cb(d->theme.obj, theme_changed, LV_EVENT_VALUE_CHANGED, d);
	page_help_text(body, "Auto switches to Light in bright sun.");

	kit_section(body, "AUTO-SLEEP", 72, 28);
	kit_seg_create(&d->sleep, body, 4, sleep_labels, NULL);
	for (i = 0; i < 4; i++)
		if (sleep_values[i] == app.set.sleep_s)
			kit_seg_set_active(&d->sleep, i);
	lv_obj_add_event_cb(d->sleep.obj, sleep_changed, LV_EVENT_VALUE_CHANGED, d);
	page_help_text(body, "Power button wakes the screen.");
	display_live(p);
}

/* The settings were replaced from storage: show them. */
static void display_settings(struct page *p)
{
	struct display_page *d = p->priv;
	int i;

	display_live(p);
	kit_seg_set_active(&d->theme, app.set.theme == UI_THEME_LIGHT ? 1
			   : app.set.theme == UI_THEME_AUTO ? 2 : 0);
	for (i = 0; i < 4; i++)
		if (sleep_values[i] == app.set.sleep_s)
			kit_seg_set_active(&d->sleep, i);
}

static void display_destroy(struct page *p)
{
	free(p->priv);
}

const struct page_ops page_display_ops = {
	.build = build_display,
	.destroy = display_destroy,
	.on_settings = display_settings,
	.on_live = display_live,
};

struct display_page *app_display_page(void)
{
	struct page *p = app_top_page();

	return p && p->kind == APP_PAGE_DISPLAY ? p->priv : NULL;
}
