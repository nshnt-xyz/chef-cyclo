/*
 * widgets.c - chefkit widgets (see widgets.h). Geometry follows the
 * round-1 design CSS; comments give the CSS where LVGL needs a different
 * number (LVGL borders are inside the object box, CSS content-box adds
 * them outside).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "fonts.h"
#include "icons.h"
#include "widgets.h"

lv_obj_t *kit_box(lv_obj_t *parent)
{
	lv_obj_t *o = lv_obj_create(parent);

	lv_obj_remove_style_all(o);
	lv_obj_set_scrollable(o, false);
	lv_obj_set_clickable(o, false);
	lv_obj_set_event_bubble(o, true);
	return o;
}

void kit_set_ink(lv_obj_t *label, enum kit_ink ink)
{
	for (int i = 0; i < KIT_INK_N; i++)
		lv_obj_remove_style(label, &kit_st.ink[i], 0);
	lv_obj_add_style(label, &kit_st.ink[ink], 0);
}

/* The CSS font size of a kit font (Barlow's em). */
int kit_font_px(const lv_font_t *f)
{
	static const struct { const lv_font_t *f; int px; } map[] = {
		{ &chefkit_b400_38, 38 }, { &chefkit_b400_40, 40 }, { &chefkit_b400_42, 42 },
		{ &chefkit_b400_46, 46 }, { &chefkit_b400_48, 48 }, { &chefkit_b400_72, 72 },
		{ &chefkit_b500_42, 42 }, { &chefkit_b500_48, 48 }, { &chefkit_b500_52, 52 },
		{ &chefkit_b500_54, 54 }, { &chefkit_b500_56, 56 }, { &chefkit_b500_60, 60 },
		{ &chefkit_b500_64, 64 }, { &chefkit_bc600_32, 32 }, { &chefkit_bc600_40, 40 },
		{ &chefkit_bc600_44, 44 }, { &chefkit_bc600_60, 60 }, { &chefkit_bc600_72, 72 },
		{ &chefkit_bc600_80, 80 }, { &chefkit_bc600_96, 96 },
	};

	for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (map[i].f == f)
			return map[i].px;
	return lv_font_get_line_height(f);
}

/* Barlow and Barlow Condensed: ascender 1.0 em, descender 0.2 em, so the
 * CSS content area is 1.2 em with the baseline 1.0 em below its top, and
 * a line box of L puts the baseline at (L - 1.2 em) / 2 + 1.0 em. LVGL
 * draws a label's baseline line_height - base_line below its top, from
 * the glyph bounds lv_font_conv saw, so place the label by baseline. */
int32_t kit_label_y(const lv_font_t *f, int32_t baseline)
{
	return baseline - (f->line_height - f->base_line);
}

int32_t kit_css_baseline(const lv_font_t *f, int line_px)
{
	float s = (float)kit_font_px(f), l = line_px > 0 ? (float)line_px : 1.2f * s;

	return (int32_t)lroundf((l - 1.2f * s) / 2 + s);
}

int kit_css_line(const lv_font_t *f)
{
	return (int)lroundf(1.2f * (float)kit_font_px(f));
}

/* LVGL clips a child to its parent's area plus the parent's extra draw
 * size, even with overflow visible: a line box shorter than the glyphs
 * (line-height 1 titles, descenders) declares the overhang. */
static void line_box_ext(lv_event_t *e)
{
	lv_obj_t *box = lv_event_get_current_target(e);
	lv_obj_t *l = lv_obj_get_child(box, 0);
	const lv_font_t *f = l ? lv_obj_get_style_text_font(l, 0) : NULL;

	if (f)
		lv_event_set_ext_draw_size(e, f->line_height);
}

lv_obj_t *kit_text(lv_obj_t *parent, const lv_font_t *font, int line_px, enum kit_ink ink,
		   const char *text)
{
	lv_obj_t *box = kit_box(parent), *l;
	int h = line_px > 0 ? line_px : kit_css_line(font);

	lv_obj_set_size(box, LV_SIZE_CONTENT, h);
	lv_obj_set_overflow_visible(box, true);
	l = lv_label_create(box);
	lv_obj_set_event_bubble(l, true);
	lv_label_set_text(l, text);
	lv_obj_set_style_text_font(l, font, 0);
	kit_set_ink(l, ink);
	lv_obj_set_pos(l, 0, kit_label_y(font, kit_css_baseline(font, h)));
	lv_obj_add_event_cb(box, line_box_ext, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
	lv_obj_refresh_ext_draw_size(box);
	return l;
}

void kit_text_single_line(lv_obj_t *label)
{
	/* fixed width and one line of height: LVGL then ends long text with
	 * "..." instead of wrapping (at a '-', say) into a clipped line */
	lv_obj_set_width(lv_obj_get_parent(label), lv_pct(100));
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_height(label, lv_font_get_line_height(lv_obj_get_style_text_font(label, 0)));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
}

lv_obj_t *kit_icon(lv_obj_t *parent, const lv_font_t *font, int box_px, enum kit_ink ink,
		   const char *glyph)
{
	lv_obj_t *box = kit_box(parent), *l;

	lv_obj_set_size(box, box_px, box_px);
	lv_obj_set_overflow_visible(box, true);
	l = lv_label_create(box);
	lv_obj_set_event_bubble(l, true);
	lv_label_set_text(l, glyph);
	lv_obj_set_style_text_font(l, font, 0);
	kit_set_ink(l, ink);
	lv_obj_center(l);
	return l;
}

static lv_obj_t *flex_row(lv_obj_t *parent, int gap)
{
	lv_obj_t *o = kit_box(parent);

	lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(o, gap, 0);
	return o;
}

static void set_text_if(lv_obj_t *label, const char *text)
{
	if (strcmp(lv_label_get_text(label), text))
		lv_label_set_text(label, text);
}

static void set_glyph(lv_obj_t *label, const char *glyph, enum kit_ink ink)
{
	set_text_if(label, glyph);
	kit_set_ink(label, ink);
}

/* ------------------------------------------------------------ status bar */

void kit_statusbar_create(struct kit_statusbar *sb, lv_obj_t *parent)
{
	lv_obj_t *bar, *l, *r, *inner;

	memset(sb, 0, sizeof(*sb));
	/* height:96px; padding:0 24px; space-between; items centred */
	sb->obj = bar = kit_box(parent);
	lv_obj_set_size(bar, lv_pct(100), KIT_STATUS_H);
	lv_obj_set_pos(bar, 0, 0);
	lv_obj_set_style_pad_hor(bar, 24, 0);
	lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	l = flex_row(bar, 12);
	sb->gps = kit_icon(l, &chefkit_icon_36, 36, KIT_INK_MUT, KIT_IC_LOCATE_OFF);
	sb->bt = kit_icon(l, &chefkit_icon_36, 36, KIT_INK_MUT, KIT_IC_BLUETOOTH_OFF);
	sb->wifi = kit_icon(l, &chefkit_icon_36, 36, KIT_INK_MUT, KIT_IC_WIFI_OFF);

	r = flex_row(bar, 12);
	/* height:38px; min-width:78px; border 3; radius 12; padding 0 6px;
	 * content centred, gap 2. The fill is absolute, battW % of the
	 * padding box (inside the border). */
	sb->batt = kit_box(r);
	lv_obj_add_style(sb->batt, &kit_st.batt, 0);
	lv_obj_add_style(sb->batt, &kit_st.batt_warn, KIT_STATE_WARN);
	lv_obj_set_size(sb->batt, LV_SIZE_CONTENT, 38);
	sb->batt_fill = kit_box(sb->batt);
	lv_obj_set_floating(sb->batt_fill, true);
	lv_obj_add_style(sb->batt_fill, &kit_st.batt_fill, 0);
	lv_obj_add_style(sb->batt_fill, &kit_st.batt_fill_warn, KIT_STATE_WARN);
	lv_obj_set_pos(sb->batt_fill, 0, 0);
	lv_obj_set_size(sb->batt_fill, lv_pct(4), lv_pct(100));
	inner = flex_row(sb->batt, 2);
	lv_obj_set_height(inner, lv_pct(100));
	lv_obj_set_style_min_width(inner, 78 - 2 * KIT_LINE, 0);
	lv_obj_set_style_pad_hor(inner, 6, 0);
	lv_obj_set_flex_align(inner, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	sb->batt_zap = kit_icon(inner, &chefkit_icon_24, 24, KIT_INK_FG, KIT_IC_ZAP);
	lv_obj_remove_style(sb->batt_zap, &kit_st.ink[KIT_INK_FG], 0);	/* batt colour */
	lv_obj_set_hidden(lv_obj_get_parent(sb->batt_zap), true);
	sb->batt_pct = kit_text(inner, &chefkit_bc600_32, 32, KIT_INK_FG, "--");
	lv_obj_remove_style(sb->batt_pct, &kit_st.ink[KIT_INK_FG], 0);
	/* Barlow Condensed 600 44, line-height 1 */
	sb->time = kit_text(r, &chefkit_bc600_44, 44, KIT_INK_FG, "--:--");
}

static const char *gps_glyph(enum kit_gps g)
{
	return g == KIT_GPS_FIX ? KIT_IC_LOCATE_FIXED : g == KIT_GPS_SEARCH ? KIT_IC_LOCATE
		: KIT_IC_LOCATE_OFF;
}

static const char *bt_glyph(enum kit_bt b)
{
	return b == KIT_BT_CONNECTED ? KIT_IC_BLUETOOTH_CONNECTED : b == KIT_BT_ON ? KIT_IC_BLUETOOTH
		: KIT_IC_BLUETOOTH_OFF;
}

static const char *wifi_glyph(enum kit_wifi w)
{
	return w == KIT_WIFI_OFF ? KIT_IC_WIFI_OFF : KIT_IC_WIFI;
}

void kit_statusbar_set(struct kit_statusbar *sb, const struct kit_status *st)
{
	const struct kit_status *old = sb->valid ? &sb->shown : NULL;

	/* Muted icon = searching or off (design StatusBar.dc.html). */
	if (!old || old->gps != st->gps)
		set_glyph(sb->gps, gps_glyph(st->gps), st->gps == KIT_GPS_FIX ? KIT_INK_FG : KIT_INK_MUT);
	if (!old || old->bt != st->bt)
		set_glyph(sb->bt, bt_glyph(st->bt),
			  st->bt == KIT_BT_CONNECTED ? KIT_INK_FG : KIT_INK_MUT);
	if (!old || old->wifi != st->wifi)
		set_glyph(sb->wifi, wifi_glyph(st->wifi),
			  st->wifi == KIT_WIFI_CONNECTED ? KIT_INK_FG : KIT_INK_MUT);
	if (!old || old->batt != st->batt || old->powered != st->powered) {
		char pct[8];
		int w = st->batt < 0 ? 0 : st->batt < 4 ? 4 : st->batt > 100 ? 100 : st->batt;

		if (st->batt < 0)
			snprintf(pct, sizeof(pct), "--");
		else
			snprintf(pct, sizeof(pct), "%d", st->batt > 100 ? 100 : st->batt);
		set_text_if(sb->batt_pct, pct);
		lv_obj_set_width(sb->batt_fill, lv_pct(w));
		lv_obj_set_hidden(lv_obj_get_parent(sb->batt_zap), !st->powered);
		/* warn only for a known low battery that is not on a supply */
		lv_obj_set_state(sb->batt, KIT_STATE_WARN,
				 st->batt >= 0 && st->batt <= 15 && !st->powered);
		lv_obj_set_state(sb->batt_fill, KIT_STATE_WARN,
				 st->batt >= 0 && st->batt <= 15 && !st->powered);
	}
	if (!old || strcmp(old->time, st->time))
		set_text_if(sb->time, st->time);
	sb->shown = *st;
	sb->valid = true;
}

/* ------------------------------------------------------------- page frame */

void kit_page_create(struct kit_page *pg, lv_obj_t *parent, const char *title,
		     lv_event_cb_t back_cb, void *user)
{
	memset(pg, 0, sizeof(*pg));
	pg->obj = kit_box(parent);
	lv_obj_set_size(pg->obj, lv_pct(100), lv_pct(100));
	lv_obj_set_flex_flow(pg->obj, LV_FLEX_FLOW_COLUMN);

	/* height:192px + border-bottom 3 (content-box); padding 0 60px, or
	 * 0 36px with gap 8 when a 168 px back target leads */
	pg->header = kit_box(pg->obj);
	lv_obj_set_size(pg->header, lv_pct(100), KIT_HEADER_H + KIT_LINE);
	lv_obj_add_style(pg->header, &kit_st.rule_bottom, 0);
	lv_obj_set_style_pad_hor(pg->header, back_cb ? 36 : KIT_PAGE_PAD, 0);
	lv_obj_set_style_pad_column(pg->header, 8, 0);
	lv_obj_set_flex_flow(pg->header, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(pg->header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	if (back_cb) {
		pg->back = kit_box(pg->header);
		lv_obj_set_size(pg->back, KIT_BACK_W, KIT_BACK_W);
		lv_obj_set_clickable(pg->back, true);
		lv_obj_set_event_bubble(pg->back, false);
		lv_obj_set_ext_click_area(pg->back, 12);
		kit_icon(pg->back, &chefkit_icon_84, KIT_BACK_W, KIT_INK_FG, KIT_IC_CHEVRON_LEFT);
		lv_obj_add_event_cb(pg->back, back_cb, LV_EVENT_CLICKED, user);
	}
	pg->title = kit_text(pg->header, &chefkit_bc600_96, 96, KIT_INK_FG, title);

	pg->body = kit_box(pg->obj);
	lv_obj_set_width(pg->body, lv_pct(100));
	lv_obj_set_flex_grow(pg->body, 1);
	lv_obj_set_style_pad_hor(pg->body, KIT_PAGE_PAD, 0);
	lv_obj_set_flex_flow(pg->body, LV_FLEX_FLOW_COLUMN);
}

lv_obj_t *kit_section(lv_obj_t *parent, const char *text, int pad_top, int pad_bottom)
{
	/* Barlow Condensed 600 40, letter-spacing .12em, muted */
	lv_obj_t *l = kit_text(parent, &chefkit_bc600_40, 0, KIT_INK_MUT, text);
	lv_obj_t *box = lv_obj_get_parent(l);

	lv_obj_set_style_text_letter_space(l, 5, 0);
	lv_obj_set_style_pad_top(box, pad_top, 0);
	lv_obj_set_style_pad_bottom(box, pad_bottom, 0);
	lv_obj_set_height(box, pad_top + kit_css_line(&chefkit_bc600_40) + pad_bottom);
	return l;
}

/* ---------------------------------------------------------------- hub tile */

void kit_tile_create(struct kit_tile *t, lv_obj_t *parent, const char *glyph, const char *title,
		     lv_event_cb_t cb, void *user)
{
	lv_obj_t *col;

	memset(t, 0, sizeof(*t));
	/* height:330px border-box; border 3 acc; radius 36; padding 36;
	 * column, space-between */
	t->obj = kit_box(parent);
	lv_obj_set_height(t->obj, KIT_TILE_H);
	lv_obj_add_style(t->obj, &kit_st.tile, 0);
	lv_obj_add_style(t->obj, &kit_st.tile_off, KIT_STATE_OFF);
	lv_obj_set_style_pad_all(t->obj, 36, 0);	/* LVGL adds the border, as CSS border-box */
	lv_obj_set_flex_flow(t->obj, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(t->obj, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_START);
	lv_obj_set_clickable(t->obj, true);
	lv_obj_set_event_bubble(t->obj, false);
	if (cb)
		lv_obj_add_event_cb(t->obj, cb, LV_EVENT_CLICKED, user);

	/* 24 px dot at top:28 right:28 of the padding box; LVGL aligns to
	 * the content box, 36 inside it */
	t->dot = kit_box(t->obj);
	lv_obj_set_floating(t->dot, true);
	lv_obj_set_size(t->dot, 24, 24);
	lv_obj_add_style(t->dot, &kit_st.dot, 0);
	lv_obj_align(t->dot, LV_ALIGN_TOP_RIGHT, 36 - 28, 28 - 36);

	t->icon = kit_icon(t->obj, &chefkit_icon_96, 96, KIT_INK_ACCT, glyph);
	lv_obj_remove_style(t->icon, &kit_st.ink[KIT_INK_ACCT], 0);
	lv_obj_add_style(t->icon, &kit_st.tile_icon, 0);
	lv_obj_add_style(t->icon, &kit_st.tile_icon_off, KIT_STATE_OFF);

	/* column gap 6: 54 px Medium line-height 1.1, 38 px muted 1.2 */
	col = kit_box(t->obj);
	lv_obj_set_size(col, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 6, 0);
	t->title = kit_text(col, &chefkit_b500_54, 59, KIT_INK_FG, title);
	t->sub = kit_text(col, &chefkit_b400_38, 46, KIT_INK_MUT, "");
	kit_text_single_line(t->sub);
}

void kit_tiles_fit(struct kit_tile *t, int n, int32_t width, int32_t gap)
{
	int32_t minw[KIT_TILES_MAX], w[KIT_TILES_MAX], left = width - gap * (n - 1);
	bool fixed[KIT_TILES_MAX] = { false };
	int free_n = n, i;
	bool again = true;

	if (n > KIT_TILES_MAX)
		n = KIT_TILES_MAX;
	/* CSS grid 1fr = minmax(auto, 1fr): a column never gets narrower than
	 * its min-content, the title (the subtitle ellipsizes instead) plus
	 * padding and border */
	lv_obj_update_layout(t[0].obj);
	for (i = 0; i < n; i++)
		minw[i] = lv_obj_get_width(t[i].title) + 2 * 36 + 2 * KIT_LINE;
	while (again && free_n > 0) {
		int32_t share = left / free_n;

		again = false;
		for (i = 0; i < n; i++)
			if (!fixed[i] && minw[i] > share) {
				fixed[i] = true;
				w[i] = minw[i];
				left -= minw[i];
				free_n--;
				again = true;
			}
	}
	for (i = 0; i < n; i++) {
		if (!fixed[i]) {
			w[i] = left / free_n;	/* the last free column takes the rest */
			left -= w[i];
			free_n--;
		}
		lv_obj_set_width(t[i].obj, w[i]);
	}
}

void kit_tile_set(struct kit_tile *t, bool on, const char *glyph, const char *sub)
{
	lv_obj_set_state(t->obj, KIT_STATE_OFF, !on);
	lv_obj_set_state(t->icon, KIT_STATE_OFF, !on);
	lv_obj_set_hidden(t->dot, !on);
	set_text_if(t->icon, glyph);
	set_text_if(t->sub, sub);
}

/* ---------------------------------------------------------------- list row */

void kit_row_create(struct kit_row *r, lv_obj_t *parent, const char *glyph, const char *title,
		    const char *sub, lv_event_cb_t cb, void *user)
{
	lv_obj_t *col;

	memset(r, 0, sizeof(*r));
	/* height:216px + border-bottom 3; gap 44; items centred */
	r->obj = kit_box(parent);
	lv_obj_set_size(r->obj, lv_pct(100), KIT_ROW_H + KIT_LINE);
	lv_obj_add_style(r->obj, &kit_st.rule_bottom, 0);
	lv_obj_set_flex_flow(r->obj, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r->obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(r->obj, 44, 0);
	lv_obj_set_clickable(r->obj, true);
	lv_obj_set_event_bubble(r->obj, false);
	if (cb)
		lv_obj_add_event_cb(r->obj, cb, LV_EVENT_CLICKED, user);

	r->icon = kit_icon(r->obj, &chefkit_icon_72, 72, KIT_INK_FG, glyph);
	col = kit_box(r->obj);
	lv_obj_set_height(col, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(col, 1);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 6, 0);
	/* 56 px Medium line-height 1.1; 42 px muted, normal */
	r->title = kit_text(col, &chefkit_b500_56, 62, KIT_INK_FG, title);
	r->sub = kit_text(col, &chefkit_b400_42, 0, KIT_INK_MUT, sub);
	kit_text_single_line(r->sub);
	r->chevron = kit_icon(r->obj, &chefkit_icon_64, 64, KIT_INK_MUT, KIT_IC_CHEVRON_RIGHT);
}

void kit_row_set_sub(struct kit_row *r, const char *sub)
{
	set_text_if(r->sub, sub);
}

/* ----------------------------------------------------------------- switch */

static void switch_clicked(lv_event_t *e)
{
	lv_obj_t *sw = lv_event_get_current_target(e);

	if (lv_obj_is_disabled(sw))
		return;
	kit_switch_set(sw, !kit_switch_get(sw));
	lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, NULL);
}

lv_obj_t *kit_switch_create(lv_obj_t *parent)
{
	lv_obj_t *sw = kit_box(parent), *knob;

	/* 204x108, radius 54. On: acc fill, padding 14, onacc knob at the
	 * end. Off: 4 px line2 border, padding 10, muted knob at the start. */
	lv_obj_set_size(sw, KIT_SWITCH_W, KIT_SWITCH_H);
	lv_obj_add_style(sw, &kit_st.sw_off, 0);
	lv_obj_add_style(sw, &kit_st.sw_on, LV_STATE_CHECKED);
	lv_obj_set_clickable(sw, true);
	lv_obj_set_event_bubble(sw, false);
	lv_obj_set_ext_click_area(sw, 40);	/* 108 px high: reach the 192 px target */
	knob = kit_box(sw);
	lv_obj_set_size(knob, 80, 80);
	lv_obj_add_style(knob, &kit_st.knob_off, 0);
	lv_obj_add_style(knob, &kit_st.knob_on, LV_STATE_CHECKED);
	lv_obj_set_state_trickle(sw, true);
	lv_obj_add_event_cb(sw, switch_clicked, LV_EVENT_CLICKED, NULL);
	kit_switch_set(sw, false);
	return sw;
}

void kit_switch_set(lv_obj_t *sw, bool on)
{
	lv_obj_t *knob = lv_obj_get_child(sw, 0);

	lv_obj_set_checked(sw, on);
	lv_obj_set_checked(knob, on);
	/* relative to the content box: on, padding 14 and no border; off,
	 * padding 10 inside the 4 px border */
	if (on)
		lv_obj_align(knob, LV_ALIGN_RIGHT_MID, -14, 0);
	else
		lv_obj_align(knob, LV_ALIGN_LEFT_MID, 10, 0);
}

bool kit_switch_get(lv_obj_t *sw)
{
	return lv_obj_is_checked(sw);
}

/* ------------------------------------------------------ segmented control */

static void seg_clicked(lv_event_t *e)
{
	struct kit_seg *s = lv_event_get_user_data(e);
	lv_obj_t *cell = lv_event_get_current_target(e);

	for (int i = 0; i < s->n; i++)
		if (s->cell[i] == cell && i != s->active && !lv_obj_is_disabled(cell)) {
			kit_seg_set_active(s, i);
			lv_obj_send_event(s->obj, LV_EVENT_VALUE_CHANGED, NULL);
		}
}

void kit_seg_create(struct kit_seg *s, lv_obj_t *parent, int n, const char *const *labels,
		    const char *const *glyphs)
{
	memset(s, 0, sizeof(*s));
	s->n = n > KIT_SEG_MAX ? KIT_SEG_MAX : n;
	s->active = -1;
	/* border 3 line2, radius 36, overflow hidden; cells flex 1, height
	 * 192, border-left 3 line2 after the first; 52 px Medium; icon 56 +
	 * gap 20 */
	s->obj = kit_box(parent);
	lv_obj_set_size(s->obj, lv_pct(100), 192 + 2 * KIT_LINE);
	lv_obj_add_style(s->obj, &kit_st.seg, 0);
	lv_obj_set_flex_flow(s->obj, LV_FLEX_FLOW_ROW);
	for (int i = 0; i < s->n; i++) {
		lv_obj_t *c = kit_box(s->obj), *l;

		s->cell[i] = c;
		lv_obj_set_height(c, lv_pct(100));
		lv_obj_set_flex_grow(c, 1);
		lv_obj_add_style(c, &kit_st.seg_cell, 0);
		lv_obj_add_style(c, &kit_st.seg_cell_on, LV_STATE_CHECKED);
		lv_obj_add_style(c, &kit_st.seg_cell_disabled, LV_STATE_DISABLED);
		lv_obj_set_style_border_width(c, i ? KIT_LINE : 0, 0);
		lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
				      LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(c, 20, 0);
		lv_obj_set_clickable(c, true);
		lv_obj_set_event_bubble(c, false);
		lv_obj_add_event_cb(c, seg_clicked, LV_EVENT_CLICKED, s);
		if (glyphs && glyphs[i]) {
			l = kit_icon(c, &chefkit_icon_56, 56, KIT_INK_FG, glyphs[i]);
			lv_obj_remove_style(l, &kit_st.ink[KIT_INK_FG], 0);	/* inherit */
		}
		l = kit_text(c, &chefkit_b500_52, 0, KIT_INK_FG, labels[i]);
		lv_obj_remove_style(l, &kit_st.ink[KIT_INK_FG], 0);
	}
}

void kit_seg_set_active(struct kit_seg *s, int idx)
{
	s->active = idx;
	for (int i = 0; i < s->n; i++)
		lv_obj_set_checked(s->cell[i], i == idx);
}

void kit_seg_set_disabled(struct kit_seg *s, int idx, bool disabled)
{
	if (idx >= 0 && idx < s->n) {
		lv_obj_set_disabled(s->cell[idx], disabled);
		lv_obj_set_clickable(s->cell[idx], !disabled);
	}
}

/* ----------------------------------------------------- brightness slider */

static void slider_place(struct kit_slider *s)
{
	int32_t inner, x;

	lv_obj_update_layout(s->track);
	/* fill width:v% and knob left:v% - 14 of the padding box: the track
	 * shows 0..max; min only limits what can be chosen */
	inner = lv_obj_get_content_width(s->track);
	x = (int32_t)((int64_t)inner * s->value / s->max);
	lv_obj_set_width(s->fill, x);
	lv_obj_set_x(s->knob, x - 14);
}

static void slider_input(lv_event_t *e)
{
	struct kit_slider *s = lv_event_get_user_data(e);
	lv_indev_t *in = lv_indev_active();
	lv_area_t a;
	lv_point_t p;
	int32_t inner, v;

	if (!in)
		return;
	lv_indev_get_point(in, &p);
	lv_obj_get_content_coords(s->track, &a);
	inner = lv_area_get_width(&a);
	if (inner <= 0)
		return;
	v = (int32_t)(((int64_t)(p.x - a.x1) * s->max + inner / 2) / inner);
	if (v < s->min)
		v = s->min;
	if (v > s->max)
		v = s->max;
	if (v != s->value) {
		kit_slider_set(s, v);
		lv_obj_send_event(s->obj, LV_EVENT_VALUE_CHANGED, NULL);
	}
}

void kit_slider_create(struct kit_slider *s, lv_obj_t *parent, int min, int max, int value)
{
	memset(s, 0, sizeof(*s));
	s->min = min;
	s->max = max > min ? max : min + 1;
	/* row: sun-dim 60 muted, gap 28, track flex 1 x 144 (border-box,
	 * border 3 line2, radius 36), gap 28, sun 72 */
	s->obj = kit_box(parent);
	lv_obj_set_size(s->obj, lv_pct(100), 144);
	lv_obj_set_flex_flow(s->obj, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(s->obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(s->obj, 28, 0);
	kit_icon(s->obj, &chefkit_icon_60, 60, KIT_INK_MUT, KIT_IC_SUN_DIM);
	s->track = kit_box(s->obj);
	lv_obj_set_height(s->track, 144);
	lv_obj_set_flex_grow(s->track, 1);
	lv_obj_add_style(s->track, &kit_st.track, 0);
	lv_obj_set_clickable(s->track, true);
	lv_obj_set_event_bubble(s->track, false);
	lv_obj_set_ext_click_area(s->track, 24);
	lv_obj_add_event_cb(s->track, slider_input, LV_EVENT_PRESSED, s);
	lv_obj_add_event_cb(s->track, slider_input, LV_EVENT_PRESSING, s);
	s->fill = kit_box(s->track);
	lv_obj_add_style(s->fill, &kit_st.track_fill, 0);
	lv_obj_set_pos(s->fill, 0, 0);
	lv_obj_set_height(s->fill, lv_pct(100));
	/* knob: top/bottom 14 of the padding box, 28 wide, radius 14 */
	s->knob = kit_box(s->track);
	lv_obj_add_style(s->knob, &kit_st.track_knob, 0);
	lv_obj_set_size(s->knob, 28, 144 - 2 * KIT_LINE - 2 * 14);
	lv_obj_set_y(s->knob, 14);
	kit_icon(s->obj, &chefkit_icon_72, 72, KIT_INK_FG, KIT_IC_SUN);
	s->value = value < s->min ? s->min : value > s->max ? s->max : value;
	slider_place(s);
}

void kit_slider_set(struct kit_slider *s, int value)
{
	value = value < s->min ? s->min : value > s->max ? s->max : value;
	s->value = value;
	slider_place(s);
}
