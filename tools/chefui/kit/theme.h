/*
 * theme.h - chefkit design tokens (round-1 system UI design, see
 * docs/next-steps/ui-and-ride-app.md "System UI design") and the shared
 * LVGL styles every kit widget takes its colours from.
 *
 * Widgets never set a colour on an object directly: they add the shared
 * styles below. kit_theme_set() rewrites the colours in those styles and
 * reports the change, so a theme switch restyles every existing object in
 * place without rebuilding a screen.
 */
#ifndef CHEFKIT_THEME_H
#define CHEFKIT_THEME_H

#include <stdint.h>

#include "lvgl.h"

enum kit_theme { KIT_THEME_DARK, KIT_THEME_LIGHT };

struct kit_palette {
	uint32_t bg, surf, fg, mut, line, line2, acc, acct, onacc, warn;
};
extern const struct kit_palette kit_palette_dark, kit_palette_light;

/* Sizes in device pixels (1080x2246 portrait). */
#define KIT_STATUS_H		96	/* status bar = the calibrated safe top */
#define KIT_STATUS_PAD_X	36	/* its side padding: the design's 24 plus 12 for the panel corners */
#define KIT_HEADER_H		192	/* page header, plus its 3 px rule */
#define KIT_BACK_W		168	/* back target in the header */
#define KIT_PAGE_PAD		60	/* page side padding */
#define KIT_LINE		3	/* rules and borders */
#define KIT_TARGET_MIN		192	/* smallest touch target (about 12 mm) */
#define KIT_ROW_H		216	/* list rows, plus their 3 px rule */
#define KIT_MASTER_H		240	/* master toggle rows */
#define KIT_SWITCH_W		204
#define KIT_SWITCH_H		108
#define KIT_RADIUS		36	/* cards, buttons, segmented controls */
#define KIT_RADIUS_KEY		24
#define KIT_RADIUS_CARD		40
#define KIT_KEY_H		168
#define KIT_TILE_H		330
#define KIT_GAP_TILES		24

/* Ink: the text/icon colour roles. */
enum kit_ink { KIT_INK_FG, KIT_INK_MUT, KIT_INK_ACCT, KIT_INK_ONACC, KIT_INK_WARN, KIT_INK_N };

/* Custom object states the kit uses besides CHECKED and DISABLED. */
#define KIT_STATE_OFF	LV_STATE_USER_1		/* tile/subsystem off: line2, muted */
#define KIT_STATE_WARN	LV_STATE_USER_2		/* battery low */

struct kit_styles {
	lv_style_t ink[KIT_INK_N];	/* text_color */
	lv_style_t screen;		/* bg colour, opaque */
	lv_style_t rule_bottom;		/* 3 px line under rows and headers */
	lv_style_t rule_top_bottom;
	lv_style_t tile, tile_off;	/* acc border / line2 border (KIT_STATE_OFF) */
	lv_style_t tile_icon, tile_icon_off;
	lv_style_t dot;			/* the tile's "on" dot */
	lv_style_t seg;			/* segmented control frame */
	lv_style_t seg_cell, seg_cell_on, seg_cell_disabled;
	lv_style_t track, track_fill, track_knob;	/* brightness slider */
	lv_style_t sw_on, sw_off, knob_on, knob_off;	/* switch */
	lv_style_t batt, batt_warn, batt_fill, batt_fill_warn;
	/* slice 2 */
	lv_style_t card;		/* acc frame, radius 40 (connected card) */
	lv_style_t outline;		/* line2 frame, radius 36 (buttons, eye) */
	lv_style_t frame;		/* line2 frame, radius 40 (steps) */
	lv_style_t cell_rule;		/* 3 px line on the left (step cells) */
	lv_style_t mark;		/* muted fill (corner crosses) */
	lv_style_t field, field_idle;	/* text field: acc frame on surf; line2 frame */
	lv_style_t kb;			/* keyboard: surf, line rule on top */
	lv_style_t key, key_fn, key_join, key_pressed;
	lv_style_t caret;		/* accent fill (text cursor) */
	/* GPS pages */
	lv_style_t ring, ring_in;	/* sky plot circles: line2, line */
	lv_style_t hair;		/* line fill: sky plot axes, cell rules */
	lv_style_t sat_on, sat_off;	/* satellite pill: in use (acc), visible */
	lv_style_t bar_on, bar_off;	/* signal bar: in use, visible */
	lv_style_t swatch_on, swatch_off;	/* legend squares */
	/* Bluetooth pages */
	lv_style_t pill;		/* acc frame, radius 60 (Pair) */
	lv_style_t scrim;		/* black at 60 % over the screen (a modal) */
	lv_style_t modal;		/* bg, line2 frame, radius 48 */
	lv_style_t progress, progress_fill;	/* line2 frame, radius 12; acc fill */
	/* Storage page: bar frame (radius 32), segment opacities (Maps,
	 * Rides, Routes, Other), the bg-coloured right edge between segments,
	 * and the legend swatches (radius 14; Free is an outline) */
	lv_style_t usage_bar, usage_seg[4], usage_seg_edge, usage_swatch, usage_swatch_free;
	lv_style_t storage_neutral;	/* the single used segment when no category split is known */
	/* boot splash: the progress track (line) and fill (acc), both radius 12 */
	lv_style_t splash_track, splash_fill;
};
extern struct kit_styles kit_st;

/* Initialise the styles (once, after lv_init) with a theme. */
void kit_theme_init(enum kit_theme theme);
/* Switch theme: rewrite the shared styles and refresh every object. */
void kit_theme_set(enum kit_theme theme);
enum kit_theme kit_theme_get(void);
const struct kit_palette *kit_palette(void);

#endif /* CHEFKIT_THEME_H */
