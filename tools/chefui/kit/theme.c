/*
 * theme.c - chefkit palettes and shared styles (see theme.h).
 */
#include "theme.h"

/* Round-1 Industry palette. The warn colours are the design's oklch values
 * converted to sRGB: dark oklch(0.78 0.11 60), light oklch(0.52 0.13 50). */
const struct kit_palette kit_palette_dark = {
	.bg = 0x111213, .surf = 0x1d1f20, .fg = 0xf2f2f3, .mut = 0x98989b,
	.line = 0x2b2b2d, .line2 = 0x5d5d60,
	.acc = 0x94bce3, .acct = 0xb5d9fd, .onacc = 0x1d2d3d, .warn = 0xeba66d,
};
const struct kit_palette kit_palette_light = {
	.bg = 0xf2f2f3, .surf = 0xe7e7ea, .fg = 0x1d1f20, .mut = 0x5d5d60,
	.line = 0xd4d4d7, .line2 = 0x98989b,
	.acc = 0x5980a6, .acct = 0x416180, .onacc = 0xf5f5f8, .warn = 0xa24e10,
};

struct kit_styles kit_st;

static enum kit_theme cur_theme;
static bool inited;

const struct kit_palette *kit_palette(void)
{
	return cur_theme == KIT_THEME_LIGHT ? &kit_palette_light : &kit_palette_dark;
}

enum kit_theme kit_theme_get(void)
{
	return cur_theme;
}

/* Geometry: set once. */
static void shapes(void)
{
	struct kit_styles *s = &kit_st;
	lv_style_t *all[] = {
		&s->ink[0], &s->ink[1], &s->ink[2], &s->ink[3], &s->ink[4],
		&s->screen, &s->rule_bottom, &s->rule_top_bottom, &s->tile, &s->tile_off,
		&s->tile_icon, &s->tile_icon_off, &s->dot, &s->seg, &s->seg_cell,
		&s->seg_cell_on, &s->seg_cell_disabled, &s->track, &s->track_fill,
		&s->track_knob, &s->sw_on, &s->sw_off, &s->knob_on, &s->knob_off,
		&s->batt, &s->batt_warn, &s->batt_fill, &s->batt_fill_warn,
		&s->card, &s->outline, &s->frame, &s->cell_rule, &s->mark, &s->field,
		&s->field_idle, &s->kb, &s->key, &s->key_fn, &s->key_join, &s->key_pressed,
		&s->caret,
	};

	for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++)
		lv_style_init(all[i]);

	lv_style_set_bg_opa(&s->screen, LV_OPA_COVER);

	lv_style_set_border_side(&s->rule_bottom, LV_BORDER_SIDE_BOTTOM);
	lv_style_set_border_width(&s->rule_bottom, KIT_LINE);
	lv_style_set_border_side(&s->rule_top_bottom, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM);
	lv_style_set_border_width(&s->rule_top_bottom, KIT_LINE);

	lv_style_set_border_width(&s->tile, KIT_LINE);
	lv_style_set_radius(&s->tile, KIT_RADIUS);

	lv_style_set_radius(&s->dot, LV_RADIUS_CIRCLE);
	lv_style_set_bg_opa(&s->dot, LV_OPA_COVER);

	lv_style_set_border_width(&s->seg, KIT_LINE);
	lv_style_set_radius(&s->seg, KIT_RADIUS);
	lv_style_set_clip_corner(&s->seg, true);
	lv_style_set_border_side(&s->seg_cell, LV_BORDER_SIDE_LEFT);
	lv_style_set_bg_opa(&s->seg_cell_on, LV_OPA_COVER);

	lv_style_set_border_width(&s->track, KIT_LINE);
	lv_style_set_radius(&s->track, KIT_RADIUS);
	lv_style_set_clip_corner(&s->track, true);
	lv_style_set_bg_opa(&s->track_fill, LV_OPA_COVER);
	lv_style_set_bg_opa(&s->track_knob, LV_OPA_COVER);
	lv_style_set_radius(&s->track_knob, 14);

	lv_style_set_radius(&s->sw_on, LV_RADIUS_CIRCLE);
	lv_style_set_bg_opa(&s->sw_on, LV_OPA_COVER);
	lv_style_set_radius(&s->sw_off, LV_RADIUS_CIRCLE);
	lv_style_set_border_width(&s->sw_off, 4);
	lv_style_set_bg_opa(&s->sw_off, LV_OPA_TRANSP);
	lv_style_set_radius(&s->knob_on, LV_RADIUS_CIRCLE);
	lv_style_set_bg_opa(&s->knob_on, LV_OPA_COVER);
	lv_style_set_radius(&s->knob_off, LV_RADIUS_CIRCLE);
	lv_style_set_bg_opa(&s->knob_off, LV_OPA_COVER);

	lv_style_set_border_width(&s->batt, KIT_LINE);
	lv_style_set_radius(&s->batt, 12);
	lv_style_set_clip_corner(&s->batt, true);
	lv_style_set_bg_opa(&s->batt_fill, 56);		/* 22 % of 255 */

	lv_style_set_border_width(&s->card, KIT_LINE);
	lv_style_set_radius(&s->card, KIT_RADIUS_CARD);
	lv_style_set_border_width(&s->outline, KIT_LINE);
	lv_style_set_radius(&s->outline, KIT_RADIUS);
	lv_style_set_border_width(&s->frame, KIT_LINE);
	lv_style_set_radius(&s->frame, KIT_RADIUS_CARD);
	lv_style_set_border_side(&s->cell_rule, LV_BORDER_SIDE_LEFT);
	lv_style_set_border_width(&s->cell_rule, KIT_LINE);
	lv_style_set_bg_opa(&s->mark, LV_OPA_COVER);
	lv_style_set_border_width(&s->field, KIT_LINE);
	lv_style_set_radius(&s->field, KIT_RADIUS);
	lv_style_set_bg_opa(&s->field, LV_OPA_COVER);
	lv_style_set_bg_opa(&s->kb, LV_OPA_COVER);
	lv_style_set_border_side(&s->kb, LV_BORDER_SIDE_TOP);
	lv_style_set_border_width(&s->kb, KIT_LINE);
	lv_style_set_radius(&s->key, KIT_RADIUS_KEY);
	lv_style_set_bg_opa(&s->key, LV_OPA_COVER);
	lv_style_set_border_width(&s->key, KIT_LINE);	/* inset 3 px line */
	lv_style_set_bg_opa(&s->key_pressed, LV_OPA_COVER);
	lv_style_set_bg_opa(&s->caret, LV_OPA_COVER);
}

/* Colours: rewritten on every theme change. */
static void colours(const struct kit_palette *p)
{
	struct kit_styles *s = &kit_st;
	const uint32_t ink[KIT_INK_N] = { p->fg, p->mut, p->acct, p->onacc, p->warn };

	for (int i = 0; i < KIT_INK_N; i++)
		lv_style_set_text_color(&s->ink[i], lv_color_hex(ink[i]));
	lv_style_set_bg_color(&s->screen, lv_color_hex(p->bg));
	lv_style_set_text_color(&s->screen, lv_color_hex(p->fg));
	lv_style_set_border_color(&s->rule_bottom, lv_color_hex(p->line));
	lv_style_set_border_color(&s->rule_top_bottom, lv_color_hex(p->line));
	lv_style_set_border_color(&s->tile, lv_color_hex(p->acc));
	lv_style_set_border_color(&s->tile_off, lv_color_hex(p->line2));
	lv_style_set_text_color(&s->tile_icon, lv_color_hex(p->acct));
	lv_style_set_text_color(&s->tile_icon_off, lv_color_hex(p->mut));
	lv_style_set_bg_color(&s->dot, lv_color_hex(p->acc));
	lv_style_set_border_color(&s->seg, lv_color_hex(p->line2));
	lv_style_set_border_color(&s->seg_cell, lv_color_hex(p->line2));
	lv_style_set_text_color(&s->seg_cell, lv_color_hex(p->fg));
	lv_style_set_bg_color(&s->seg_cell_on, lv_color_hex(p->acc));
	lv_style_set_text_color(&s->seg_cell_on, lv_color_hex(p->onacc));
	lv_style_set_text_color(&s->seg_cell_disabled, lv_color_hex(p->mut));
	lv_style_set_border_color(&s->track, lv_color_hex(p->line2));
	lv_style_set_bg_color(&s->track_fill, lv_color_hex(p->acc));
	lv_style_set_bg_color(&s->track_knob, lv_color_hex(p->fg));
	lv_style_set_bg_color(&s->sw_on, lv_color_hex(p->acc));
	lv_style_set_border_color(&s->sw_off, lv_color_hex(p->line2));
	lv_style_set_bg_color(&s->knob_on, lv_color_hex(p->onacc));
	lv_style_set_bg_color(&s->knob_off, lv_color_hex(p->mut));
	lv_style_set_border_color(&s->batt, lv_color_hex(p->fg));
	lv_style_set_text_color(&s->batt, lv_color_hex(p->fg));
	lv_style_set_border_color(&s->batt_warn, lv_color_hex(p->warn));
	lv_style_set_text_color(&s->batt_warn, lv_color_hex(p->warn));
	lv_style_set_bg_color(&s->batt_fill, lv_color_hex(p->fg));
	lv_style_set_bg_color(&s->batt_fill_warn, lv_color_hex(p->warn));
	lv_style_set_border_color(&s->card, lv_color_hex(p->acc));
	lv_style_set_border_color(&s->outline, lv_color_hex(p->line2));
	lv_style_set_border_color(&s->frame, lv_color_hex(p->line2));
	lv_style_set_border_color(&s->cell_rule, lv_color_hex(p->line));
	lv_style_set_bg_color(&s->mark, lv_color_hex(p->mut));
	lv_style_set_border_color(&s->field, lv_color_hex(p->acc));
	lv_style_set_bg_color(&s->field, lv_color_hex(p->surf));
	lv_style_set_border_color(&s->field_idle, lv_color_hex(p->line2));
	lv_style_set_bg_color(&s->kb, lv_color_hex(p->surf));
	lv_style_set_border_color(&s->kb, lv_color_hex(p->line));
	lv_style_set_bg_color(&s->key, lv_color_hex(p->bg));
	lv_style_set_border_color(&s->key, lv_color_hex(p->line));
	lv_style_set_text_color(&s->key, lv_color_hex(p->fg));
	lv_style_set_bg_color(&s->key_fn, lv_color_hex(p->line));
	lv_style_set_bg_color(&s->key_join, lv_color_hex(p->acc));
	lv_style_set_text_color(&s->key_join, lv_color_hex(p->onacc));
	lv_style_set_bg_color(&s->key_pressed, lv_color_hex(p->line2));
	lv_style_set_bg_color(&s->caret, lv_color_hex(p->acct));
}

void kit_theme_init(enum kit_theme theme)
{
	if (!inited) {
		shapes();
		inited = true;
	}
	cur_theme = theme;
	colours(kit_palette());
}

void kit_theme_set(enum kit_theme theme)
{
	if (!inited) {
		kit_theme_init(theme);
		return;
	}
	if (theme == cur_theme)
		return;
	cur_theme = theme;
	colours(kit_palette());
	lv_obj_report_style_change(NULL);	/* every object, every style */
}
