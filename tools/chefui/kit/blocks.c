/*
 * blocks.c - chefkit blocks the slice 2 pages share (see widgets.h):
 * master switch row, section with an action, accent card, list rows,
 * key/value rows, steps card, outline button and empty state. Geometry
 * from the round-1 design CSS, as in widgets.c.
 */
#include <string.h>

#include "fonts.h"
#include "icons.h"
#include "widgets.h"

static void set_text_if(lv_obj_t *label, const char *text)
{
	if (strcmp(lv_label_get_text(label), text))
		lv_label_set_text(label, text);
}

/* ------------------------------------------------------------- page extras */

void kit_page_set_back_glyph(struct kit_page *pg, const char *glyph)
{
	lv_obj_t *box = pg->back ? lv_obj_get_child(pg->back, 0) : NULL;

	if (box)
		set_text_if(lv_obj_get_child(box, 0), glyph);
}

void kit_page_title_fit(struct kit_page *pg)
{
	kit_text_single_line(pg->title);
	lv_obj_set_flex_grow(lv_obj_get_parent(pg->title), 1);
}

void kit_page_scrollable(struct kit_page *pg)
{
	lv_obj_set_scrollable(pg->body, true);
	lv_obj_set_scroll_dir(pg->body, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(pg->body, LV_SCROLLBAR_MODE_OFF);
}

/* ------------------------------------------------------------- master row */

void kit_master_create(struct kit_master *m, lv_obj_t *parent, const char *title,
		       lv_event_cb_t cb, void *user)
{
	lv_obj_t *col;

	memset(m, 0, sizeof(*m));
	/* height:240px + border-bottom 3; gap 40; centred */
	m->obj = kit_box(parent);
	lv_obj_set_size(m->obj, lv_pct(100), KIT_MASTER_H + KIT_LINE);
	lv_obj_add_style(m->obj, &kit_st.rule_bottom, 0);
	lv_obj_set_flex_flow(m->obj, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(m->obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(m->obj, 40, 0);
	/* column gap 8: 64 px Medium line-height 1.1, 42 px muted */
	col = kit_box(m->obj);
	lv_obj_set_height(col, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(col, 1);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 8, 0);
	m->title = kit_text(col, &chefkit_b500_64, 70, KIT_INK_FG, title);
	m->sub = kit_text(col, &chefkit_b400_42, 0, KIT_INK_MUT, "");
	kit_text_single_line(m->sub);
	m->sw = kit_switch_create(m->obj);
	if (cb)
		lv_obj_add_event_cb(m->sw, cb, LV_EVENT_VALUE_CHANGED, user);
}

void kit_master_set(struct kit_master *m, bool on, const char *sub)
{
	if (kit_switch_get(m->sw) != on)
		kit_switch_set(m->sw, on);
	set_text_if(m->sub, sub);
}

/* ------------------------------------------------- section with an action */

void kit_section_action_create(struct kit_section_action *sa, lv_obj_t *parent,
			       const char *text, int pad_top, int pad_bottom, const char *glyph,
			       const char *action)
{
	lv_obj_t *l;
	int line = kit_css_line(&chefkit_bc600_40);

	memset(sa, 0, sizeof(*sa));
	/* padding pad_top 0 pad_bottom; space-between; centred */
	sa->obj = kit_box(parent);
	lv_obj_set_size(sa->obj, lv_pct(100), pad_top + line + pad_bottom);
	lv_obj_set_style_pad_top(sa->obj, pad_top, 0);
	lv_obj_set_style_pad_bottom(sa->obj, pad_bottom, 0);
	lv_obj_set_flex_flow(sa->obj, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(sa->obj, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	sa->label = kit_text(sa->obj, &chefkit_bc600_40, 0, KIT_INK_MUT, text);
	lv_obj_set_style_text_letter_space(sa->label, 5, 0);
	/* 44 px glyph, gap 16, 40 px accent text */
	sa->action = kit_box(sa->obj);
	lv_obj_set_size(sa->action, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(sa->action, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(sa->action, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(sa->action, 16, 0);
	kit_icon(sa->action, &chefkit_icon_44, 44, KIT_INK_ACCT, glyph);
	l = kit_text(sa->action, &chefkit_b400_40, 0, KIT_INK_ACCT, action);
	(void)l;
}

void kit_section_action_show(struct kit_section_action *sa, bool show)
{
	lv_obj_set_hidden(sa->action, !show);
}

/* ------------------------------------------------------------- accent card */

void kit_card_create(struct kit_card *c, lv_obj_t *parent, const char *glyph, lv_event_cb_t cb,
		     void *user)
{
	lv_obj_t *col;

	memset(c, 0, sizeof(*c));
	/* height:240px + 3 px border each side (content-box); radius 40;
	 * padding 0 48; gap 40 */
	c->obj = kit_box(parent);
	lv_obj_set_size(c->obj, lv_pct(100), 240 + 2 * KIT_LINE);
	lv_obj_add_style(c->obj, &kit_st.card, 0);
	lv_obj_set_style_pad_hor(c->obj, 48, 0);
	lv_obj_set_flex_flow(c->obj, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(c->obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(c->obj, 40, 0);
	lv_obj_set_clickable(c->obj, true);
	lv_obj_set_event_bubble(c->obj, false);
	if (cb)
		lv_obj_add_event_cb(c->obj, cb, LV_EVENT_CLICKED, user);
	c->icon = kit_icon(c->obj, &chefkit_icon_80, 80, KIT_INK_ACCT, glyph);
	/* column gap 6: 60 px Medium line-height 1.1, 42 px muted */
	col = kit_box(c->obj);
	lv_obj_set_height(col, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(col, 1);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 6, 0);
	c->title = kit_text(col, &chefkit_b500_60, 66, KIT_INK_FG, "");
	kit_text_single_line(c->title);
	c->sub = kit_text(col, &chefkit_b400_42, 0, KIT_INK_MUT, "");
	kit_text_single_line(c->sub);
	c->chevron = kit_icon(c->obj, &chefkit_icon_64, 64, KIT_INK_MUT, KIT_IC_CHEVRON_RIGHT);
}

void kit_card_set(struct kit_card *c, const char *glyph, const char *title, const char *sub)
{
	set_text_if(c->icon, glyph);
	set_text_if(c->title, title);
	set_text_if(c->sub, sub);
}

/* ------------------------------------------------------------- list rows */

static void row_frame(struct kit_row *r, lv_obj_t *parent, int gap, lv_event_cb_t cb, void *user)
{
	memset(r, 0, sizeof(*r));
	r->obj = kit_box(parent);
	lv_obj_set_size(r->obj, lv_pct(100), KIT_ROW_H + KIT_LINE);
	lv_obj_add_style(r->obj, &kit_st.rule_bottom, 0);
	lv_obj_set_flex_flow(r->obj, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r->obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(r->obj, gap, 0);
	lv_obj_set_clickable(r->obj, true);
	lv_obj_set_event_bubble(r->obj, false);
	if (cb)
		lv_obj_add_event_cb(r->obj, cb, LV_EVENT_CLICKED, user);
}

void kit_list_row_create(struct kit_row *r, lv_obj_t *parent, const char *glyph, const char *title,
			 const char *sub, const lv_font_t *trail_font, const char *trail_glyph,
			 lv_event_cb_t cb, void *user)
{
	lv_obj_t *col;

	/* height:216px + border-bottom 3; gap 40; 72 px icon; column flex 1
	 * gap 6 (56 px Medium 1.1, 42 px muted); optional trailing glyph */
	row_frame(r, parent, 40, cb, user);
	r->icon = kit_icon(r->obj, &chefkit_icon_72, 72, KIT_INK_FG, glyph);
	col = kit_box(r->obj);
	lv_obj_set_height(col, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(col, 1);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 6, 0);
	r->title = kit_text(col, &chefkit_b500_56, 62, KIT_INK_FG, title);
	kit_text_single_line(r->title);
	if (sub) {
		r->sub = kit_text(col, &chefkit_b400_42, 0, KIT_INK_MUT, sub);
		kit_text_single_line(r->sub);
	}
	if (trail_font) {
		int px = trail_font == &chefkit_icon_56 ? 56 : 64;

		r->chevron = kit_icon(r->obj, trail_font, px, KIT_INK_MUT, trail_glyph ? trail_glyph : "");
		lv_obj_set_hidden(lv_obj_get_parent(r->chevron), !trail_glyph);
	}
}

void kit_action_row_create(struct kit_row *r, lv_obj_t *parent, const char *glyph,
			   const char *title, lv_event_cb_t cb, void *user)
{
	/* the same row in the accent text colour, 56 px Medium, no subtitle */
	row_frame(r, parent, 40, cb, user);
	r->icon = kit_icon(r->obj, &chefkit_icon_72, 72, KIT_INK_ACCT, glyph);
	r->title = kit_text(r->obj, &chefkit_b500_56, 0, KIT_INK_ACCT, title);
}

void kit_row_set(struct kit_row *r, const char *glyph, const char *title, const char *sub,
		 const char *trail_glyph)
{
	if (glyph)
		set_text_if(r->icon, glyph);
	if (title)
		set_text_if(r->title, title);
	if (sub && r->sub)
		set_text_if(r->sub, sub);
	if (r->chevron) {
		lv_obj_set_hidden(lv_obj_get_parent(r->chevron), !trail_glyph);
		if (trail_glyph)
			set_text_if(r->chevron, trail_glyph);
	}
}

/* ---------------------------------------------------------- key/value rows */

lv_obj_t *kit_kv_row(lv_obj_t *parent, const char *key, const char *value)
{
	lv_obj_t *row = kit_box(parent), *v;

	/* height:132px + border-bottom 3; space-between; gap 32; 48 px */
	lv_obj_set_size(row, lv_pct(100), 132 + KIT_LINE);
	lv_obj_add_style(row, &kit_st.rule_bottom, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 32, 0);
	kit_text(row, &chefkit_b400_48, 0, KIT_INK_MUT, key);
	v = kit_text(row, &chefkit_b500_48, 0, KIT_INK_FG, value);
	kit_text_single_line(v);
	lv_obj_set_flex_grow(lv_obj_get_parent(v), 1);
	lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
	return v;
}

void kit_kv_set(lv_obj_t *value_label, const char *value)
{
	set_text_if(value_label, value);
}

/* ---------------------------------------------------------------- steps */

static void frame_ext(lv_event_t *e)
{
	lv_event_set_ext_draw_size(e, 18);	/* the corner marks */
}

static void corner_mark(lv_obj_t *frame, lv_align_t align, int dx, int dy)
{
	lv_obj_t *m = kit_box(frame), *bar;

	lv_obj_set_floating(m, true);
	lv_obj_set_size(m, 36, 36);
	lv_obj_set_overflow_visible(m, true);
	lv_obj_align(m, align, dx, dy);
	/* linear-gradient bars: 3 px wide, centred */
	bar = kit_box(m);
	lv_obj_add_style(bar, &kit_st.mark, 0);
	lv_obj_set_size(bar, KIT_LINE, 36);
	lv_obj_set_pos(bar, 16, 0);
	bar = kit_box(m);
	lv_obj_add_style(bar, &kit_st.mark, 0);
	lv_obj_set_size(bar, 36, KIT_LINE);
	lv_obj_set_pos(bar, 0, 16);
}

void kit_steps_create(struct kit_steps *st, lv_obj_t *parent, int margin_top, int n,
		      const char *const *labels)
{
	lv_obj_t *gap;

	memset(st, 0, sizeof(*st));
	st->n = n > KIT_STEPS_MAX ? KIT_STEPS_MAX : n;
	gap = kit_box(parent);
	lv_obj_set_size(gap, 1, margin_top);
	/* grid repeat(n, 1fr); border 3 line2; radius 40; marks at -18 */
	st->obj = kit_box(parent);
	lv_obj_set_size(st->obj, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_add_style(st->obj, &kit_st.frame, 0);
	lv_obj_set_flex_flow(st->obj, LV_FLEX_FLOW_ROW);
	lv_obj_set_overflow_visible(st->obj, true);
	lv_obj_add_event_cb(st->obj, frame_ext, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
	lv_obj_refresh_ext_draw_size(st->obj);
	for (int i = 0; i < st->n; i++) {
		/* padding 40 0; column, centred, gap 16; inset 3 px line left */
		lv_obj_t *c = kit_box(st->obj);

		lv_obj_set_height(c, LV_SIZE_CONTENT);
		lv_obj_set_flex_grow(c, 1);
		lv_obj_add_style(c, &kit_st.cell_rule, 0);
		lv_obj_set_style_pad_ver(c, 40, 0);
		lv_obj_set_style_pad_left(c, 0, 0);
		lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_row(c, 16, 0);
		st->icon[i] = kit_icon(c, &chefkit_icon_72, 72, KIT_INK_MUT, KIT_IC_CIRCLE);
		st->label[i] = kit_text(c, &chefkit_b500_42, 0, KIT_INK_FG, labels[i]);
	}
	/* CSS top/left -18 of the padding box: LVGL aligns inside the border
	 * too */
	corner_mark(st->obj, LV_ALIGN_TOP_LEFT, -18, -18);
	corner_mark(st->obj, LV_ALIGN_TOP_RIGHT, 18, -18);
	corner_mark(st->obj, LV_ALIGN_BOTTOM_LEFT, -18, 18);
	corner_mark(st->obj, LV_ALIGN_BOTTOM_RIGHT, 18, 18);
}

void kit_steps_set(struct kit_steps *st, int i, bool done)
{
	if (i < 0 || i >= st->n)
		return;
	set_text_if(st->icon[i], done ? KIT_IC_CIRCLE_CHECK : KIT_IC_CIRCLE);
	kit_set_ink(st->icon[i], done ? KIT_INK_ACCT : KIT_INK_MUT);
}

void kit_steps_show(struct kit_steps *st, int i, bool show)
{
	if (i >= 0 && i < st->n)
		lv_obj_set_hidden(lv_obj_get_parent(lv_obj_get_parent(st->icon[i])), !show);
}

/* --------------------------------------------------------- outline button */

lv_obj_t *kit_button_create(lv_obj_t *parent, const char *glyph, const char *text,
			    lv_event_cb_t cb, void *user)
{
	lv_obj_t *b = kit_box(parent);

	/* height:192px border-box; border 3 line2; radius 36; centred, gap 24 */
	lv_obj_set_size(b, lv_pct(100), 192);
	lv_obj_add_style(b, &kit_st.outline, 0);
	lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(b, 24, 0);
	lv_obj_set_clickable(b, true);
	lv_obj_set_event_bubble(b, false);
	if (cb)
		lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
	kit_icon(b, &chefkit_icon_60, 60, KIT_INK_FG, glyph);
	kit_text(b, &chefkit_bc600_60, 0, KIT_INK_FG, text);
	return b;
}

/* ------------------------------------------------------------ empty state */

void kit_empty_create(struct kit_empty *em, lv_obj_t *parent, const char *glyph,
		      const char *title, const char *text)
{
	const lv_font_t *f = &chefkit_b400_46;
	lv_obj_t *box = kit_box(parent), *tb, *l;
	int32_t pitch = 62, space, y0;	/* line-height 1.35 of 46 px */

	/* flex 1; column, centred both ways; gap 40; padding-bottom 240 */
	lv_obj_set_width(box, lv_pct(100));
	lv_obj_set_flex_grow(box, 1);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(box, 40, 0);
	lv_obj_set_style_pad_bottom(box, 240, 0);
	memset(em, 0, sizeof(*em));
	em->obj = box;
	em->icon = kit_icon(box, &chefkit_icon_200, 200, KIT_INK_MUT, glyph);
	em->title = kit_text(box, &chefkit_bc600_80, 88, KIT_INK_FG, title);
	/* wrapped lines 62 px apart: LVGL adds line_space between lines only,
	 * so the box pads the first baseline in and the last line out */
	tb = kit_box(box);
	lv_obj_set_size(tb, 820, LV_SIZE_CONTENT);
	l = lv_label_create(tb);
	lv_obj_set_event_bubble(l, true);
	lv_obj_set_width(l, lv_pct(100));
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(l, f, 0);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	space = pitch - f->line_height;
	lv_obj_set_style_text_line_space(l, space, 0);
	kit_set_ink(l, KIT_INK_MUT);
	lv_label_set_text(l, text);
	y0 = kit_label_y(f, kit_css_baseline(f, pitch));
	lv_obj_set_style_pad_top(tb, y0 > 0 ? y0 : 0, 0);
	lv_obj_set_style_pad_bottom(tb, space - y0 > 0 ? space - y0 : 0, 0);
	em->text = l;
}

void kit_empty_set(struct kit_empty *em, const char *glyph, const char *title, const char *text)
{
	set_text_if(em->icon, glyph);
	set_text_if(em->title, title);
	set_text_if(em->text, text);
}

lv_obj_t *kit_spacer(lv_obj_t *parent)
{
	lv_obj_t *s = kit_box(parent);

	lv_obj_set_size(s, 1, 1);
	lv_obj_set_flex_grow(s, 1);
	return s;
}
