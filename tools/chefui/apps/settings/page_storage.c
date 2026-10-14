/*
 * page_storage.c - Storage (P05): capacity and free space of the owned
 * /data and what Maps, Rides and Routes use of it. The numbers come from
 * the global storage model (storage.c); the category scan runs on the
 * settings worker and is requested once on entry/wake when stale, never
 * per frame. The page shows only coherent data: while the detailed
 * snapshot is unknown or invalidated it shows the cheap capacity and
 * "Calculating usage...", with one neutral used segment.
 */
#include <stdio.h>
#include <stdlib.h>

#include "app.h"
#include "fonts.h"
#include "icons.h"
#include "pages.h"
#include "sys.h"
#include "text.h"

#define NOTE_ESTIMATE "Usage estimate; storage changed or could not be fully scanned."
#define NOTE_CALC "Calculating usage..."
#define NOTE_RAM "Nothing is kept across reboots."

enum { CAT_MAPS, CAT_RIDES, CAT_ROUTES, CAT_OTHER, CAT_N };

struct storage_page {
	lv_obj_t *number, *unit, *bar, *seg[CAT_N + 1], *value[CAT_N + 1], *note;
	int bar_w;
};

/* bytes -> pixels of the bar, checked 128-bit scaling; boundaries are
 * cumulative so rounding never makes the segments exceed the bar */
static int scaled(uint64_t part, uint64_t total, int width)
{
	unsigned __int128 v;

	if (!total)
		return 0;
	if (part > total)
		part = total;
	v = (unsigned __int128)part * (unsigned)width / total;
	return (int)v;
}

static void seg_set(struct storage_page *s, int i, int x0, int x1)
{
	lv_obj_t *o = s->seg[i];

	if (x1 <= x0) {
		lv_obj_set_hidden(o, true);
		return;
	}
	lv_obj_set_hidden(o, false);
	lv_obj_set_pos(o, x0, 0);
	lv_obj_set_size(o, x1 - x0, lv_pct(100));
}

static void storage_refresh(struct page *p)
{
	struct storage_page *s = p->priv;
	const struct sys_state *st = &sys_state;
	bool ours = st->storage.known && st->storage.ours, detail = false, estimate = false;
	uint64_t total = st->storage.total_bytes, free_b = st->storage.free_bytes;
	uint64_t use[CAT_N] = { 0 }, sum = 0, edge[CAT_N + 1] = { 0 };
	bool unknown[CAT_N] = { false }, partial[CAT_N] = { false };
	const char *note = "";
	char b[48];

	if (!s)
		return;
	if (ours && total && free_b <= total) {
		snprintf(b, sizeof(b), "%.1f", (double)free_b / 1e9);
		lv_label_set_text(s->number, b);
	} else {
		lv_label_set_text(s->number, "--");
	}
	text_storage_unit(st, b, sizeof(b));
	lv_label_set_text(s->unit, b);

	if (ours && total && free_b <= total && st->storage.usage_known &&
	    st->storage.usage_total_bytes == total && st->storage.usage_free_bytes == free_b) {
		use[CAT_MAPS] = st->storage.usage_maps;
		use[CAT_RIDES] = st->storage.usage_rides;
		use[CAT_ROUTES] = st->storage.usage_routes;
		use[CAT_OTHER] = st->storage.usage_other;
		/* a category the scan never reached is unknown, not zero, and so is
		 * Other; a cut-short one is an estimate */
		for (int i = 0; i < CAT_OTHER; i++) {
			enum sys_usage_cat cs = st->storage.usage_cat[i];

			if (cs == USAGE_CAT_UNSCANNED) {
				use[i] = 0;
				unknown[i] = true;
				unknown[CAT_OTHER] = true;
			} else if (cs == USAGE_CAT_PARTIAL) {
				partial[i] = true;
				partial[CAT_OTHER] = true;
			}
		}
		if (unknown[CAT_OTHER])
			use[CAT_OTHER] = 0;
		for (int i = 0; i < CAT_N; i++) {
			if (use[i] > total - sum) {		/* more than the capacity */
				sum = total + 1;
				break;
			}
			sum += use[i];
		}
		detail = sum <= total;
		estimate = !st->storage.usage_complete;
	}
	seg_set(s, CAT_N, 0, 0);
	if (!ours || !total || free_b > total) {
		note = ours ? "" : st->storage.known ? NOTE_RAM : "";
		for (int i = 0; i < CAT_N; i++)
			seg_set(s, i, 0, 0);
	} else if (detail && !unknown[CAT_OTHER]) {
		uint64_t cum = 0;

		for (int i = 0; i < CAT_N; i++) {
			cum += use[i];
			edge[i + 1] = cum;
		}
		for (int i = 0; i < CAT_N; i++)
			seg_set(s, i, scaled(edge[i], total, s->bar_w),
				scaled(edge[i + 1], total, s->bar_w));
		for (int i = 0; i < CAT_N; i++)
			estimate = estimate || partial[i];
		note = estimate ? NOTE_ESTIMATE : "";
	} else {
		/* one neutral used segment from the cheap capacity alone: no snapshot
		 * yet, categories that do not add up, or one the scan never reached */
		for (int i = 0; i < CAT_N; i++)
			seg_set(s, i, 0, 0);
		seg_set(s, CAT_N, 0, scaled(total - free_b, total, s->bar_w));
		note = detail ? NOTE_ESTIMATE : NOTE_CALC;
	}
	for (int i = 0; i < CAT_N; i++) {
		if (detail && !unknown[i])
			text_gb(use[i], estimate || partial[i], b, sizeof(b));
		else
			snprintf(b, sizeof(b), "--");
		lv_label_set_text(s->value[i], b);
	}
	if (ours && total && free_b <= total)
		text_gb(free_b, false, b, sizeof(b));
	else
		snprintf(b, sizeof(b), "--");
	lv_label_set_text(s->value[CAT_N], b);
	lv_label_set_text(s->note, note);
}

static void row(lv_obj_t *body, struct storage_page *s, int i, const char *name)
{
	lv_obj_t *r = kit_box(body), *sw = kit_box(r), *l;
	bool free_row = i == CAT_N;

	lv_obj_set_size(r, lv_pct(100), 180 + KIT_LINE);
	lv_obj_add_style(r, &kit_st.rule_bottom, 0);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(r, 40, 0);
	lv_obj_set_size(sw, 56, 56);
	if (free_row) {
		lv_obj_add_style(sw, &kit_st.usage_swatch_free, 0);
	} else {
		lv_obj_add_style(sw, &kit_st.usage_swatch, 0);
		lv_obj_add_style(sw, &kit_st.usage_seg[i], 0);
	}
	l = kit_text(r, &chefkit_b500_56, 0, free_row ? KIT_INK_MUT : KIT_INK_FG, name);
	lv_obj_set_flex_grow(lv_obj_get_parent(l), 1);
	s->value[i] = kit_text(r, &chefkit_bc600_64, 0, free_row ? KIT_INK_MUT : KIT_INK_FG, "--");
}

static void build_storage(struct page *p, const void *arg)
{
	struct storage_page *s = calloc(1, sizeof(*s));
	lv_obj_t *body = p->kp.body, *head, *holder, *gap;

	(void)arg;
	if (!s)
		return;
	p->priv = s;
	/* padding 80 0 48; baseline row, gap 24 */
	head = kit_box(body);
	lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_top(head, 80, 0);
	lv_obj_set_style_pad_bottom(head, 48, 0);
	lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	lv_obj_set_style_pad_column(head, 24, 0);
	s->number = kit_text(head, &chefkit_bc600_200, 180, KIT_INK_FG, "--");
	s->unit = kit_text(head, &chefkit_b400_52, 0, KIT_INK_MUT, "");

	/* the bar: the design's corner marks are clipped by its overflow:hidden,
	 * so none are drawn */
	holder = kit_box(body);
	lv_obj_set_size(holder, lv_pct(100), 120 + 2 * KIT_LINE);
	s->bar = kit_box(holder);
	lv_obj_set_size(s->bar, lv_pct(100), lv_pct(100));
	lv_obj_add_style(s->bar, &kit_st.usage_bar, 0);
	for (int i = 0; i <= CAT_N; i++) {
		/* the last is the neutral used segment (a muted fill, not a category) */
		s->seg[i] = kit_box(s->bar);
		lv_obj_add_style(s->seg[i], i < CAT_N ? &kit_st.usage_seg[i] :
				 &kit_st.storage_neutral, 0);
		lv_obj_add_style(s->seg[i], &kit_st.usage_seg_edge, 0);
		lv_obj_set_hidden(s->seg[i], true);
	}
	lv_obj_update_layout(body);
	s->bar_w = lv_obj_get_content_width(s->bar);
	if (s->bar_w <= 0)
		s->bar_w = 1080 - 2 * KIT_PAGE_PAD - 2 * KIT_LINE;

	gap = kit_box(body);
	lv_obj_set_size(gap, 1, 64);
	row(body, s, CAT_MAPS, "Maps");
	row(body, s, CAT_RIDES, "Rides");
	row(body, s, CAT_ROUTES, "Routes");
	row(body, s, CAT_OTHER, "Other");
	row(body, s, CAT_N, "Free");
	s->note = page_help_text(body, "");
	lv_obj_set_style_pad_top(lv_obj_get_parent(s->note), 28, 0);
	/* the note may wrap: the line box fills the row, its height follows the text */
	lv_obj_set_width(lv_obj_get_parent(s->note), lv_pct(100));
	lv_obj_set_width(s->note, lv_pct(100));
	lv_label_set_long_mode(s->note, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_pos(s->note, 0, 0);
	lv_obj_set_height(lv_obj_get_parent(s->note), LV_SIZE_CONTENT);
	kit_page_scrollable(&p->kp);
	storage_refresh(p);
}

static void storage_state(struct page *p, unsigned what)
{
	if (what & SYS_STORAGE)
		storage_refresh(p);
}

static void storage_visible(struct page *p, bool visible)
{
	(void)p;
	if (!app.fake_state)
		sys_storage_usage_want(visible);
}

static void storage_destroy(struct page *p)
{
	if (!app.fake_state)
		sys_storage_usage_want(false);
	free(p->priv);
}

const struct page_ops page_storage_ops = {
	.build = build_storage,
	.destroy = storage_destroy,
	.on_state = storage_state,
	.on_visible = storage_visible,
};
