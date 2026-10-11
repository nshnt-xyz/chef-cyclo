/*
 * widgets.h - chefkit widgets for the system UI (round-1 design): status
 * bar, page frame, section label, hub tile, list row, switch, segmented
 * control and brightness slider. Every colour comes from the shared
 * styles in theme.h, so kit_theme_set() restyles them in place.
 *
 * Sizes are device pixels taken from the design's CSS. CSS places text in
 * a line box of font-size x line-height; kit_text() reproduces that box so
 * stacked text lands where the design puts it.
 */
#ifndef CHEFKIT_WIDGETS_H
#define CHEFKIT_WIDGETS_H

#include <stdbool.h>

#include "lvgl.h"
#include "theme.h"

/* A plain container: no style, not scrollable, not clickable. */
lv_obj_t *kit_box(lv_obj_t *parent);

/* Text in a CSS line box line_px high (0: line-height "normal", which is
 * 1.2 em for Barlow). The label's baseline sits where CSS puts it and the
 * glyphs may overflow the box, as in CSS.
 * Returns the label; lv_obj_get_parent() of it is the line box, which is
 * what a layout positions or sizes. */
lv_obj_t *kit_text(lv_obj_t *parent, const lv_font_t *font, int line_px, enum kit_ink ink,
		   const char *text);
/* Make a kit_text() label fill its line box's parent width and end with
 * "..." when too long, on one line. */
void kit_text_single_line(lv_obj_t *label);

/* CSS metrics of kit fonts: font size, the line box of line-height
 * normal (1.2 em), the baseline within a line box (0 = normal), and the y
 * that puts an LVGL label's baseline at a given y. */
int kit_font_px(const lv_font_t *f);
int kit_css_line(const lv_font_t *f);
int32_t kit_css_baseline(const lv_font_t *f, int line_px);
int32_t kit_label_y(const lv_font_t *f, int32_t baseline);

/* Icon glyph centred in a box_px square. Returns the label. */
lv_obj_t *kit_icon(lv_obj_t *parent, const lv_font_t *font, int box_px, enum kit_ink ink,
		   const char *glyph);
/* Swap the ink style of a kit_text()/kit_icon() label. */
void kit_set_ink(lv_obj_t *label, enum kit_ink ink);

/* ------------------------------------------------------------ status bar */

enum kit_gps { KIT_GPS_OFF, KIT_GPS_SEARCH, KIT_GPS_FIX };
enum kit_bt { KIT_BT_OFF, KIT_BT_ON, KIT_BT_CONNECTED };
enum kit_wifi { KIT_WIFI_OFF, KIT_WIFI_ON, KIT_WIFI_CONNECTED };

struct kit_status {
	enum kit_gps gps;
	enum kit_bt bt;
	enum kit_wifi wifi;
	int batt;		/* 0..100, -1 unknown */
	bool powered;		/* a supply is online (zap; never warn) */
	char time[8];		/* "14:16" */
};

struct kit_statusbar {
	lv_obj_t *obj, *gps, *bt, *wifi;
	lv_obj_t *batt, *batt_fill, *batt_zap, *batt_pct, *time;
	struct kit_status shown;
	bool valid;
};

/* Across the top KIT_STATUS_H pixels of parent (normally chefui_root()),
 * in the two ears beside the notch. */
void kit_statusbar_create(struct kit_statusbar *sb, lv_obj_t *parent);
/* Update; only what changed is touched (no redraw when nothing did). */
void kit_statusbar_set(struct kit_statusbar *sb, const struct kit_status *st);

/* ------------------------------------------------------------- page frame */

struct kit_page {
	lv_obj_t *obj;		/* the whole page */
	lv_obj_t *header, *back, *title;
	lv_obj_t *body;		/* column, KIT_PAGE_PAD side padding */
};

/* A full-size page in parent (the content root). back_cb != NULL adds the
 * 168 px back target (chevron-left) before the title; it gets the page as
 * user data. */
void kit_page_create(struct kit_page *pg, lv_obj_t *parent, const char *title,
		     lv_event_cb_t back_cb, void *user);

/* Section label (upper case text expected) with CSS padding. */
lv_obj_t *kit_section(lv_obj_t *parent, const char *text, int pad_top, int pad_bottom);

/* ---------------------------------------------------------------- hub tile */

struct kit_tile {
	lv_obj_t *obj, *dot, *icon, *title, *sub;
};

/* One of the connection tiles (sized by kit_tiles_fit()). cb gets
 * LV_EVENT_CLICKED with user. */
void kit_tile_create(struct kit_tile *t, lv_obj_t *parent, const char *glyph, const char *title,
		     lv_event_cb_t cb, void *user);
/* Widths for n tiles side by side in width with gap, as CSS
 * grid-template-columns: repeat(n, 1fr) does it. */
#define KIT_TILES_MAX 4
void kit_tiles_fit(struct kit_tile *t, int n, int32_t width, int32_t gap);
/* on: accent frame, dot and accent icon; off: line2 frame, muted icon. */
void kit_tile_set(struct kit_tile *t, bool on, const char *glyph, const char *sub);

/* ---------------------------------------------------------------- list row */

struct kit_row {
	lv_obj_t *obj, *icon, *title, *sub, *chevron;
};

/* Hub row: 72 px icon, title + subtitle, chevron; 216 px plus a rule. */
void kit_row_create(struct kit_row *r, lv_obj_t *parent, const char *glyph, const char *title,
		    const char *sub, lv_event_cb_t cb, void *user);
void kit_row_set_sub(struct kit_row *r, const char *sub);

/* ----------------------------------------------------------------- switch */

/* 204x108 pill. LV_STATE_CHECKED = on; LV_STATE_DISABLED = muted, no input.
 * A tap toggles it and sends LV_EVENT_VALUE_CHANGED. */
lv_obj_t *kit_switch_create(lv_obj_t *parent);
void kit_switch_set(lv_obj_t *sw, bool on);
bool kit_switch_get(lv_obj_t *sw);

/* ------------------------------------------------------ segmented control */

#define KIT_SEG_MAX 4

struct kit_seg {
	lv_obj_t *obj;
	lv_obj_t *cell[KIT_SEG_MAX];
	int n, active;
};

/* n cells of 192 px; glyphs may be NULL (text only) or hold NULL entries.
 * A tap on an enabled cell makes it active and sends
 * LV_EVENT_VALUE_CHANGED on seg->obj (read seg->active). */
void kit_seg_create(struct kit_seg *s, lv_obj_t *parent, int n, const char *const *labels,
		    const char *const *glyphs);
void kit_seg_set_active(struct kit_seg *s, int idx);	/* -1: none */
void kit_seg_set_disabled(struct kit_seg *s, int idx, bool disabled);

/* ----------------------------------------------------- brightness slider */

struct kit_slider {
	lv_obj_t *obj;		/* the row: dim icon, track, bright icon */
	lv_obj_t *track, *fill, *knob;
	int min, max, value;
};

/* Track 144 px high between sun-dim and sun icons, drawn for 0..max (the
 * fill ends at value/max, as the design's width:v%); input is clamped to
 * min..max. Dragging or tapping the track sets the value and sends
 * LV_EVENT_VALUE_CHANGED on s->obj for each change (coalesce in the
 * handler). */
void kit_slider_create(struct kit_slider *s, lv_obj_t *parent, int min, int max, int value);
void kit_slider_set(struct kit_slider *s, int value);

#endif /* CHEFKIT_WIDGETS_H */
