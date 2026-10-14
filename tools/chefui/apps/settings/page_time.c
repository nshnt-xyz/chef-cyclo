/*
 * page_time.c - Time & date (P04): the local clock and date, where the
 * time came from, the fixed zone and the 12/24-hour switch. The source is
 * verified by timeview.c only while this page is visible; the page itself
 * holds no state besides its widgets.
 */
#include <stdio.h>
#include <stdlib.h>

#include "app.h"
#include "fonts.h"
#include "icons.h"
#include "pages.h"
#include "sys.h"
#include "text.h"

struct time_page {
	lv_obj_t *digits, *meridiem, *date;
	lv_obj_t *source, *zone, *hint, *sw, *sync;
};

static void time_refresh(struct page *p)
{
	struct time_page *t = p->priv;
	char digits[8], mer[3], b[96];

	if (!t)
		return;
	if (!sys_state.clock.valid ||
	    !sys_time_format(sys_state.clock.hour, sys_state.clock.minute, app.set.clock_24h, digits,
			     sizeof(digits), mer)) {
		/* a legacy fixture sets only hh:mm */
		int h, m;

		if (!sys_state.clock.valid && sscanf(sys_state.clock.hhmm, "%2d:%2d", &h, &m) == 2)
			sys_time_format(h, m, app.set.clock_24h, digits, sizeof(digits), mer);
		else {
			snprintf(digits, sizeof(digits), "--:--");
			mer[0] = 0;
		}
	}
	lv_label_set_text(t->digits, digits);
	lv_label_set_text(t->meridiem, mer);
	lv_obj_set_hidden(lv_obj_get_parent(t->meridiem), !mer[0]);
	lv_label_set_text(t->date, sys_state.clock.date[0] ? sys_state.clock.date : "Date unknown");
	text_time_source(&sys_state, b, sizeof(b));
	lv_label_set_text(t->source, b);
	text_time_zone(&sys_state, b, sizeof(b));
	lv_label_set_text(t->zone, b);
	text_time_hint(&sys_state, b, sizeof(b));
	lv_label_set_text(t->hint, b);
	text_time_sync(&sys_state, app.set.clock_24h, b, sizeof(b));
	lv_label_set_text(t->sync, b);
	kit_switch_set(t->sw, app.set.clock_24h);
}

static void clock_switch(lv_event_t *e)
{
	struct time_page *t = lv_event_get_user_data(e);

	app_set_clock_24h(kit_switch_get(t->sw));
}

static void build_time(struct page *p, const void *arg)
{
	struct time_page *t = calloc(1, sizeof(*t));
	lv_obj_t *body = p->kp.body, *card, *line;

	(void)arg;
	if (!t)
		return;
	p->priv = t;
	/* padding 96 0 72; gap 8; centred; rule below */
	card = kit_box(body);
	lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_add_style(card, &kit_st.rule_bottom, 0);
	lv_obj_set_style_pad_top(card, 96, 0);
	lv_obj_set_style_pad_bottom(card, 72, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(card, 8, 0);
	/* the digits (280 px, line-height .95) and, in 12-hour mode, the
	 * am/pm marker at their baseline end */
	line = kit_box(card);
	lv_obj_set_size(line, LV_SIZE_CONTENT, 266);
	lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(line, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	lv_obj_set_style_pad_column(line, 16, 0);
	t->digits = kit_text(line, &chefkit_bc600_280, 266, KIT_INK_FG, "00:00");
	lv_obj_set_style_text_letter_space(t->digits, -6, 0);
	t->meridiem = kit_text(line, &chefkit_b400_42, 0, KIT_INK_MUT, "pm");
	t->date = kit_text(card, &chefkit_b400_52, 0, KIT_INK_MUT, "");

	t->source = page_info_row(body, "Time source", "", NULL);
	t->zone = page_info_row(body, "Time zone", "", NULL);
	t->hint = page_info_row(body, "24-hour clock", "", &t->sw);
	lv_obj_add_event_cb(t->sw, clock_switch, LV_EVENT_VALUE_CHANGED, t);
	t->sync = page_help_text(body, "");
	lv_obj_set_style_pad_top(lv_obj_get_parent(t->sync), 28, 0);
	lv_obj_set_height(lv_obj_get_parent(t->sync), 28 + kit_css_line(&chefkit_b400_40));
	time_refresh(p);
}

static void time_state(struct page *p, unsigned what)
{
	if (what & (SYS_CLOCK | SYS_TIME_VIEW))
		time_refresh(p);
}

static void time_visible(struct page *p, bool visible)
{
	(void)p;
	if (!app.fake_state)
		sys_timeview_want(visible);
}

static void time_destroy(struct page *p)
{
	if (!app.fake_state)
		sys_timeview_want(false);
	free(p->priv);
}

const struct page_ops page_time_ops = {
	.build = build_time,
	.destroy = time_destroy,
	.on_state = time_state,
	.on_visible = time_visible,
	.on_settings = time_refresh,
};
