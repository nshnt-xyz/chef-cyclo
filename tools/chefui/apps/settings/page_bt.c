/*
 * page_bt.c - the Bluetooth pages: the switch with the off state (P07),
 * the sensors and the devices found (P08), the pairing modal over it
 * (P09) and a sensor's detail page (P10).
 *
 * The switch is BlueZ's adapter Powered (netbus). Sensors come from
 * blesensd (ble_view, blesens.h): while one of these pages is seen with
 * the screen on, the app holds the sensors (blesensd reconnects them) and,
 * on the list with Bluetooth on, scans. Leaving or the screen going off
 * lets go of both; blesensd keeps its connections either way.
 *
 * A device with several services (a power meter with cadence) is one
 * sensor with one row per kind: Heart rate, Power, Speed / cadence.
 * Forget on any of its rows forgets the device.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "blesens.h"
#include "fonts.h"
#include "icons.h"
#include "pages.h"
#include "sys.h"
#include "sysstate.h"

#define DOT " \xC2\xB7 "	/* " · " */
#define ELLIPSIS "\xE2\x80\xA6"	/* "…" */
#define NONE "\xE2\x80\x93"	/* "–": no value */
#define MINUS "\xE2\x88\x92"	/* "−" */
#define PAIR_MS 20000		/* blesensd's pairing bound */
#define FORGET_ARM_MS 4000
#define ROWS_MAX (BLE_SENSORS_MAX * BLE_KINDS)

static int64_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_text(lv_obj_t *l, const char *t)
{
	if (strcmp(lv_label_get_text(l), t))
		lv_label_set_text(l, t);
}

static const char *kind_glyph(int k)
{
	return k == BLE_HR ? KIT_IC_HEART_PULSE : k == BLE_POWER ? KIT_IC_ZAP : KIT_IC_GAUGE;
}

/* A found device's kind, as the design names it. */
static const char *found_kind(unsigned kinds)
{
	int n = 0, k = ble_first_kind(kinds);

	for (int i = 0; i < BLE_KINDS; i++)
		n += !!(kinds & (1u << i));
	if (n > 1)
		return n == 2 ? "2 sensors" : "3 sensors";
	return k == BLE_HR ? "Heart rate" : k == BLE_POWER ? "Power meter" : "Speed / cadence";
}

static bool bt_on(void)
{
	return sys_state.bt.known && sys_state.bt.adapter && sys_state.bt.powered;
}

static bool is_bt_page(enum app_page k)
{
	return k == APP_PAGE_BT || k == APP_PAGE_BT_SENSOR;
}

/* Seen with the screen on: hold the sensors; on the list (Bluetooth on)
 * also scan. */
static void bt_sync(void)
{
	struct page *top = app_top_page();
	bool seen = app.pf.screen_is_on() && is_bt_page(top->kind);

	sys_ble_want(seen, seen && top->kind == APP_PAGE_BT && bt_on());
}

/* A value and its unit for a kind; false when there is none. */
static bool value_text(const struct ble_sensor *s, int k, char *v, size_t vn, const char **unit)
{
	const struct ble_value *x = &s->val[k];

	if (k == BLE_CSC && x->has) {
		snprintf(v, vn, "%.1f", x->v);
		*unit = "km/h";
	} else if (k == BLE_CSC && x->has_cadence) {
		snprintf(v, vn, "%.0f", x->cadence);
		*unit = "rpm";
	} else if (k != BLE_CSC && x->has) {
		snprintf(v, vn, "%.0f", x->v);
		*unit = k == BLE_HR ? "bpm" : "W";
	} else {
		snprintf(v, vn, NONE);
		*unit = "";
		return false;
	}
	return true;
}

static const char *state_word(enum ble_state st)
{
	switch (st) {
	case BLE_ST_CONNECTED: return "connected";
	case BLE_ST_CONNECTING: return "looking" ELLIPSIS;
	case BLE_ST_OFF: return "off";
	case BLE_ST_FORGETTING: return "removing" ELLIPSIS;
	default: return "not connected";
	}
}

/* "CSS label" heading: Barlow Condensed 600 40, .12em, upper case. */
static lv_obj_t *caps(lv_obj_t *parent, const char *t, enum kit_ink ink)
{
	lv_obj_t *l = kit_text(parent, &chefkit_bc600_40, 0, ink, t);

	lv_obj_set_style_text_letter_space(l, 5, 0);
	return l;
}

/* Wrapped text, centred, line-height pitch: the label (in its box). */
static lv_obj_t *wrapped(lv_obj_t *parent, const lv_font_t *f, int32_t pitch, int32_t width)
{
	lv_obj_t *tb = kit_box(parent), *l;
	int32_t space = pitch - f->line_height, y0;

	lv_obj_set_size(tb, width, LV_SIZE_CONTENT);
	l = lv_label_create(tb);
	lv_obj_set_event_bubble(l, true);
	lv_obj_set_width(l, lv_pct(100));
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(l, f, 0);
	lv_obj_set_style_text_line_space(l, space, 0);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	kit_set_ink(l, KIT_INK_MUT);
	lv_label_set_text(l, "");
	y0 = kit_label_y(f, kit_css_baseline(f, pitch));
	lv_obj_set_style_pad_top(tb, y0 > 0 ? y0 : 0, 0);
	lv_obj_set_style_pad_bottom(tb, space - y0 > 0 ? space - y0 : 0, 0);
	return l;
}

/* A figure (line box line px) and its unit (line-height normal) on one
 * baseline, as CSS align-items:baseline lays them out: the box is as high
 * as the larger ascent plus the larger descent. Returns the box. */
static lv_obj_t *figure(lv_obj_t *parent, const lv_font_t *vf, int line, const lv_font_t *uf, int gap,
			lv_obj_t **val, lv_obj_t **unit)
{
	lv_obj_t *box = kit_box(parent), *vb, *ub;
	int32_t bv = kit_css_baseline(vf, line), bu = kit_css_baseline(uf, 0), lu = kit_css_line(uf);
	int32_t up = bv > bu ? bv : bu, down = line - bv > lu - bu ? line - bv : lu - bu;

	lv_obj_set_size(box, LV_SIZE_CONTENT, up + down);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(box, gap, 0);
	vb = kit_box(box);
	lv_obj_set_size(vb, LV_SIZE_CONTENT, up + down);
	lv_obj_set_overflow_visible(vb, true);
	*val = lv_label_create(vb);
	lv_obj_set_style_text_font(*val, vf, 0);
	kit_set_ink(*val, KIT_INK_FG);
	lv_label_set_text(*val, "");
	lv_obj_set_y(*val, kit_label_y(vf, up));
	ub = kit_box(box);
	lv_obj_set_size(ub, LV_SIZE_CONTENT, up + down);
	lv_obj_set_overflow_visible(ub, true);
	*unit = lv_label_create(ub);
	lv_obj_set_style_text_font(*unit, uf, 0);
	kit_set_ink(*unit, KIT_INK_MUT);
	lv_label_set_text(*unit, "");
	lv_obj_set_y(*unit, kit_label_y(uf, up));
	return box;
}

/* =========================================================== list (P07/P08) */

struct sensor_row {
	lv_obj_t *obj, *icon, *title, *sub, *val, *unit, *fig;
	struct ble_row r;
	char addr[18];
};

struct found_row {
	lv_obj_t *obj, *icon, *title, *sub, *pair;
	char addr[18];
};

struct bt_page {
	struct page *p;
	struct kit_master master;
	struct kit_empty empty;
	lv_obj_t *list, *mine_none, *found_none, *no_service;
	lv_obj_t *mine_box, *found_box;
	struct kit_section_action avail;
	struct sensor_row rows[ROWS_MAX];
	int n_rows;
	struct found_row found[BLE_FOUND_MAX];
	int n_found;
	/* the pairing modal (P09), over the whole screen */
	lv_obj_t *scrim, *m_icon, *m_caps, *m_name, *m_text, *m_bar, *m_fill, *m_btn_label;
	lv_timer_t *tick;
};

static void master_changed(lv_event_t *e)
{
	struct bt_page *b = lv_event_get_user_data(e);
	bool on = kit_switch_get(b->master.sw);

	/* A queued Set is not confirmation, and refusal may emit no signal. */
	kit_switch_set(b->master.sw, bt_on());
	sys_bt_set_powered(on);
}

static void sensor_clicked(lv_event_t *e)
{
	struct sensor_row *r = lv_event_get_user_data(e);
	struct ble_sensor *s = ble_sensor(&ble_view, r->addr);
	struct page_bt_sensor_arg a;

	if (!s || s->st == BLE_ST_FORGETTING)
		return;
	snprintf(a.addr, sizeof(a.addr), "%s", r->addr);
	a.kind = r->r.kind;
	app_open_arg(APP_PAGE_BT_SENSOR, ble_kind_title(r->r.kind), &a);
}

static void pair_clicked(lv_event_t *e)
{
	struct found_row *r = lv_event_get_user_data(e);

	for (int i = 0; i < ble_view.n_found; i++)
		if (!strcmp(ble_view.f[i].addr, r->addr)) {
			sys_ble_pair(&ble_view.f[i]);
			return;
		}
}

/* Row frame: 216 px + its rule, gap 40, 72 px icon, title + subtitle. */
static lv_obj_t *row_base(lv_obj_t *parent, lv_obj_t **icon, lv_obj_t **title, lv_obj_t **sub)
{
	lv_obj_t *row = kit_box(parent), *col;

	lv_obj_set_size(row, lv_pct(100), KIT_ROW_H + KIT_LINE);
	lv_obj_add_style(row, &kit_st.rule_bottom, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 40, 0);
	*icon = kit_icon(row, &chefkit_icon_72, 72, KIT_INK_MUT, KIT_IC_HEART_PULSE);
	col = kit_box(row);
	lv_obj_set_height(col, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(col, 1);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 6, 0);
	*title = kit_text(col, &chefkit_b500_56, 62, KIT_INK_FG, "");
	kit_text_single_line(*title);
	*sub = kit_text(col, &chefkit_b400_42, 0, KIT_INK_MUT, "");
	kit_text_single_line(*sub);
	return row;
}

static void sensor_rows_refresh(struct bt_page *b)
{
	struct ble_row rows[ROWS_MAX];
	int n = ble_rows(&ble_view, rows, ROWS_MAX), i;

	for (i = 0; i < n; i++) {
		const struct ble_sensor *s = &ble_view.s[rows[i].sensor];
		struct sensor_row *r = &b->rows[i];
		int k = rows[i].kind;
		bool on = s->st == BLE_ST_CONNECTED, has;
		const char *unit;
		char v[16], sub[96];

		if (i == b->n_rows) {
			r->obj = row_base(b->mine_box, &r->icon, &r->title, &r->sub);
			lv_obj_set_clickable(r->obj, true);
			lv_obj_set_event_bubble(r->obj, false);
			lv_obj_add_event_cb(r->obj, sensor_clicked, LV_EVENT_CLICKED, r);
			r->fig = figure(r->obj, &chefkit_bc600_88, 88, &chefkit_b400_38, 8, &r->val, &r->unit);
			b->n_rows++;
		}
		r->r = rows[i];
		snprintf(r->addr, sizeof(r->addr), "%s", s->addr);
		set_text(r->icon, kind_glyph(k));
		kit_set_ink(r->icon, on ? KIT_INK_ACCT : KIT_INK_MUT);
		set_text(r->title, ble_kind_title(k));
		snprintf(sub, sizeof(sub), "%s" DOT "%s", s->name, state_word(s->st));
		set_text(r->sub, sub);
		has = on && value_text(s, k, v, sizeof(v), &unit);
		if (!has) {
			snprintf(v, sizeof(v), NONE);
			unit = "";
		}
		set_text(r->val, v);
		kit_set_ink(r->val, has ? KIT_INK_FG : KIT_INK_MUT);
		set_text(r->unit, unit);
		lv_obj_set_hidden(lv_obj_get_parent(r->unit), !*unit);
		lv_obj_set_hidden(r->obj, false);
	}
	for (; i < b->n_rows; i++)
		lv_obj_set_hidden(b->rows[i].obj, true);
	lv_obj_set_hidden(b->mine_none, n > 0);
}

static void found_rows_refresh(struct bt_page *b)
{
	int i;

	for (i = 0; i < ble_view.n_found; i++) {
		const struct ble_found *f = &ble_view.f[i];
		struct found_row *r = &b->found[i];
		char sub[96];

		if (i == b->n_found) {
			lv_obj_t *pill;

			r->obj = row_base(b->found_box, &r->icon, &r->title, &r->sub);
			/* height 120, padding 0 48, 3 px accent frame, radius 60 */
			pill = kit_box(r->obj);
			lv_obj_set_size(pill, LV_SIZE_CONTENT, 120);
			lv_obj_add_style(pill, &kit_st.pill, 0);
			lv_obj_set_style_pad_hor(pill, 48 - KIT_LINE, 0);
			lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
			lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
					      LV_FLEX_ALIGN_CENTER);
			lv_obj_set_clickable(pill, true);
			lv_obj_set_event_bubble(pill, false);
			lv_obj_add_event_cb(pill, pair_clicked, LV_EVENT_CLICKED, r);
			kit_text(pill, &chefkit_bc600_52, 0, KIT_INK_ACCT, "Pair");
			r->pair = pill;
			b->n_found++;
		}
		snprintf(r->addr, sizeof(r->addr), "%s", f->addr);
		set_text(r->icon, kind_glyph(ble_first_kind(f->kinds)));
		set_text(r->title, f->name);
		if (f->has_rssi)
			snprintf(sub, sizeof(sub), "%s" DOT "%s", found_kind(f->kinds), ble_signal_word(f->rssi));
		else
			snprintf(sub, sizeof(sub), "%s", found_kind(f->kinds));
		set_text(r->sub, sub);
		lv_obj_set_hidden(r->obj, false);
	}
	for (; i < b->n_found; i++)
		lv_obj_set_hidden(b->found[i].obj, true);
	lv_obj_set_hidden(b->found_none, ble_view.n_found > 0);
}

/* ------------------------------------------------------------ the modal */

static void modal_text(struct bt_page *b)
{
	const char *t;
	int k = ble_first_kind(ble_view.pair.kinds);

	if (ble_view.pair.st == BLE_PAIR_FAILED) {
		const char *r = ble_view.pair.reason;

		t = !strcmp(r, "timed out") ? "The sensor did not answer. Wake it and try again."
			: !strcmp(r, "bluetooth off") ? "Bluetooth was turned off."
			: !strcmp(r, "blesensd stopped") ? "The sensor service stopped. Try again."
			: "Pairing did not finish. Wake the sensor and try again.";
	} else {
		t = k == BLE_HR ? "Wear the strap so it wakes up. This takes up to 20 seconds."
			: k == BLE_POWER ? "Turn the cranks so it wakes up. This takes up to 20 seconds."
			: "Turn the wheel or the cranks so it wakes up. This takes up to 20 seconds.";
	}
	set_text(b->m_text, t);
}

static void bar_set(struct bt_page *b)
{
	int64_t el = ble_view.pair.started_ms ? mono_ms() - ble_view.pair.started_ms : 0;
	int pct = (int)(el * 100 / PAIR_MS);

	lv_obj_set_width(b->m_fill, lv_pct(pct < 0 ? 0 : pct > 100 ? 100 : pct));
}

static void tick_fired(lv_timer_t *t)
{
	bar_set(lv_timer_get_user_data(t));
}

static void modal_refresh(struct bt_page *b)
{
	enum ble_pair_state st = ble_view.pair.st;
	bool show = st == BLE_PAIR_RUNNING || st == BLE_PAIR_FAILED;

	if (st == BLE_PAIR_DONE)
		sys_ble_pair_clear();	/* the sensor shows in the list */
	lv_obj_set_hidden(b->scrim, !show);
	lv_timer_pause(b->tick);
	if (!show)
		return;
	set_text(b->m_icon, kind_glyph(ble_first_kind(ble_view.pair.kinds)));
	set_text(b->m_caps, st == BLE_PAIR_FAILED ? "NOT PAIRED" : "PAIRING" ELLIPSIS);
	kit_set_ink(b->m_caps, st == BLE_PAIR_FAILED ? KIT_INK_WARN : KIT_INK_ACCT);
	set_text(b->m_name, ble_view.pair.name);
	modal_text(b);
	lv_obj_set_hidden(b->m_bar, st != BLE_PAIR_RUNNING);
	set_text(b->m_btn_label, st == BLE_PAIR_FAILED ? "Close" : "Cancel");
	bar_set(b);
	if (st == BLE_PAIR_RUNNING && app_page_visible(b->p))
		lv_timer_resume(b->tick);
}

static void modal_button(lv_event_t *e)
{
	(void)e;
	if (ble_view.pair.st == BLE_PAIR_RUNNING)
		sys_ble_cancel();
	else
		sys_ble_pair_clear();
}

/* P09: a scrim over the whole screen, the card at the bottom. */
static void build_modal(struct bt_page *b)
{
	lv_obj_t *card, *col, *btn;

	b->scrim = kit_box(app.root);
	lv_obj_set_size(b->scrim, lv_pct(100), lv_pct(100));
	lv_obj_add_style(b->scrim, &kit_st.scrim, 0);
	lv_obj_set_clickable(b->scrim, true);		/* nothing below takes a tap */
	lv_obj_set_flex_flow(b->scrim, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(b->scrim, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(b->scrim, 60, 0);
	lv_obj_set_style_pad_bottom(b->scrim, 120, 0);
	/* bg, 3 px line2, radius 48, padding 72 60 60, gap 36, centred */
	card = kit_box(b->scrim);
	lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_add_style(card, &kit_st.modal, 0);
	kit_corner_marks(card);		/* placed from the border: no padding here */
	card = kit_box(card);
	lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_top(card, 72, 0);
	lv_obj_set_style_pad_hor(card, 60, 0);
	lv_obj_set_style_pad_bottom(card, 60, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(card, 36, 0);
	b->m_icon = kit_icon(card, &chefkit_icon_160, 160, KIT_INK_ACCT, KIT_IC_HEART_PULSE);
	col = kit_box(card);
	lv_obj_set_size(col, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(col, 12, 0);
	b->m_caps = caps(col, "", KIT_INK_ACCT);
	b->m_name = kit_text(col, &chefkit_bc600_96, 96, KIT_INK_FG, "");
	kit_text_single_line(b->m_name);
	lv_obj_set_style_text_align(b->m_name, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(lv_obj_get_parent(b->m_name), lv_pct(100));
	b->m_text = wrapped(card, &chefkit_b400_46, 62, 820);
	/* the 20 s bar: 24 px, 3 px line2 frame, radius 12, accent fill */
	b->m_bar = kit_box(card);
	lv_obj_set_size(b->m_bar, lv_pct(100), 24);
	lv_obj_add_style(b->m_bar, &kit_st.progress, 0);
	b->m_fill = kit_box(b->m_bar);
	lv_obj_set_height(b->m_fill, lv_pct(100));
	lv_obj_add_style(b->m_fill, &kit_st.progress_fill, 0);
	/* Cancel: 192 px, margin-top 12, line2 frame radius 36 */
	btn = kit_box(card);
	lv_obj_set_size(btn, lv_pct(100), 192 + 12);
	lv_obj_set_style_pad_top(btn, 12, 0);
	col = kit_box(btn);
	lv_obj_set_size(col, lv_pct(100), 192);
	lv_obj_add_style(col, &kit_st.outline, 0);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_clickable(col, true);
	lv_obj_set_event_bubble(col, false);
	lv_obj_add_event_cb(col, modal_button, LV_EVENT_CLICKED, b);
	b->m_btn_label = kit_text(col, &chefkit_bc600_60, 0, KIT_INK_FG, "Cancel");
	b->tick = lv_timer_create(tick_fired, 500, b);
	lv_timer_pause(b->tick);
	lv_obj_set_hidden(b->scrim, true);
}

/* ------------------------------------------------------------ the page */

static void bt_refresh(struct bt_page *b)
{
	const struct sys_state *s = &sys_state;
	bool on = bt_on(), service = ble_view.known;
	struct ble_row rows[ROWS_MAX];
	int n = ble_rows(&ble_view, rows, ROWS_MAX);
	char sub[96];

	if (!s->bt.known)
		snprintf(sub, sizeof(sub), "Unavailable");
	else if (!s->bt.adapter)
		snprintf(sub, sizeof(sub), "No adapter");
	else if (!on)
		snprintf(sub, sizeof(sub), "Off");
	else if (n > 0)
		snprintf(sub, sizeof(sub), "On" DOT "%d of %d sensor%s connected", ble_rows_connected(&ble_view), n,
			 n == 1 ? "" : "s");
	else
		snprintf(sub, sizeof(sub), "On");
	kit_master_set(&b->master, on, sub);
	lv_obj_set_disabled(b->master.sw, !s->bt.known || !s->bt.adapter);
	lv_obj_set_hidden(b->empty.obj, on);
	lv_obj_set_hidden(b->list, !on);
	modal_refresh(b);
	if (!on) {
		if (!s->bt.known)
			kit_empty_set(&b->empty, KIT_IC_BLUETOOTH_OFF, "Bluetooth unavailable",
				      "bluetoothd is not running.");
		else if (!s->bt.adapter)
			kit_empty_set(&b->empty, KIT_IC_BLUETOOTH_OFF, "No Bluetooth adapter",
				      "The Bluetooth chip is not ready yet.");
		else
			kit_empty_set(&b->empty, KIT_IC_BLUETOOTH_OFF, "Sensors are disconnected",
				      "Heart rate, power and cadence won't record until Bluetooth is on.");
		return;
	}
	lv_obj_set_hidden(b->no_service, service);
	lv_obj_set_hidden(b->mine_box, !service);
	lv_obj_set_hidden(b->avail.obj, !service);
	lv_obj_set_hidden(b->found_box, !service);
	kit_section_action_show(&b->avail, ble_view.scanning);
	sensor_rows_refresh(b);
	found_rows_refresh(b);
}

/* 40 px muted line under a section (padding-top 28). */
static lv_obj_t *help_line(lv_obj_t *parent, const char *s)
{
	lv_obj_t *l = kit_text(parent, &chefkit_b400_40, 0, KIT_INK_MUT, s);
	lv_obj_t *box = lv_obj_get_parent(l);

	lv_obj_set_style_pad_top(box, 28, 0);
	lv_obj_set_height(box, 28 + kit_css_line(&chefkit_b400_40));
	kit_text_single_line(l);
	return box;
}

static lv_obj_t *column(lv_obj_t *parent)
{
	lv_obj_t *c = kit_box(parent);

	lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	return c;
}

static void bt_build(struct page *p, const void *arg)
{
	struct bt_page *b = calloc(1, sizeof(*b));
	lv_obj_t *body = p->kp.body;

	(void)arg;
	p->priv = b;
	if (!b)
		return;
	b->p = p;
	kit_page_scrollable(&p->kp);
	kit_master_create(&b->master, body, "Bluetooth", master_changed, b);
	kit_empty_create(&b->empty, body, KIT_IC_BLUETOOTH_OFF, "", "");
	b->list = column(body);
	b->no_service = help_line(b->list, "The sensor service is not running.");
	b->mine_box = column(b->list);
	kit_section(b->mine_box, "MY SENSORS", 64, 8);
	b->mine_none = help_line(b->mine_box, "No sensors yet. Pair one below.");
	kit_section_action_create(&b->avail, b->list, "AVAILABLE", 64, 8, KIT_IC_LOADER, "Scanning");
	b->found_box = column(b->list);
	b->found_none = help_line(b->found_box, "Wake a sensor nearby to find it.");
	build_modal(b);
	bt_refresh(b);
}

static void bt_destroy(struct page *p)
{
	struct bt_page *b = p->priv;

	if (!b)
		return;
	/* leaving the page ends a pairing it started */
	if (ble_view.pair.st == BLE_PAIR_RUNNING)
		sys_ble_cancel();
	sys_ble_pair_clear();
	lv_timer_delete(b->tick);
	lv_obj_delete(b->scrim);
	free(b);
}

static void bt_state(struct page *p, unsigned what)
{
	if (!p->priv || !(what & (SYS_BT | SYS_BLE)))
		return;
	bt_refresh(p->priv);
	if (what & SYS_BT)
		bt_sync();		/* the scan follows the switch */
}

static void bt_visible(struct page *p, bool visible)
{
	struct bt_page *b = p->priv;

	bt_sync();
	if (b) {
		if (visible) {
			bt_refresh(b);		/* the modal too */
		} else {
			lv_obj_set_hidden(b->scrim, true);
			lv_timer_pause(b->tick);
		}
	}
}

const struct page_ops page_bt_ops = {
	.build = bt_build,
	.destroy = bt_destroy,
	.on_state = bt_state,
	.on_visible = bt_visible,
};

/* ============================================================ detail (P10) */

struct sensor_page {
	struct page *p;
	char addr[18];
	enum ble_kind kind;
	lv_obj_t *live, *val, *unit, *icon;
	lv_obj_t *status, *name, *signal, *battery, *paired, *cadence;
	lv_obj_t *forget;
	lv_timer_t *disarm;
};

static void date_text(const char *iso, char *b, size_t n)
{
	static const char *const mon[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep",
					     "Oct", "Nov", "Dec" };
	int y, m, d;

	if (sscanf(iso, "%4d-%2d-%2d", &y, &m, &d) == 3 && m >= 1 && m <= 12 && d >= 1 && d <= 31)
		snprintf(b, n, "%d %s %d", d, mon[m - 1], y);
	else
		snprintf(b, n, "Unknown");
}

static void sensor_refresh(struct sensor_page *d)
{
	const struct ble_sensor *s = ble_sensor(&ble_view, d->addr);
	const char *unit;
	char v[16], b[96];
	bool live;

	if (!s)
		return;
	live = s->st == BLE_ST_CONNECTED && value_text(s, d->kind, v, sizeof(v), &unit);
	if (!live) {
		snprintf(v, sizeof(v), NONE);
		unit = "";
	}
	set_text(d->val, v);
	set_text(d->unit, unit);
	kit_set_ink(d->live, live ? KIT_INK_ACCT : KIT_INK_MUT);
	kit_set_ink(d->icon, s->st == BLE_ST_CONNECTED ? KIT_INK_ACCT : KIT_INK_MUT);
	kit_kv_set(d->status, s->st == BLE_ST_CONNECTED ? "Connected" : s->st == BLE_ST_CONNECTING
		   ? "Looking" ELLIPSIS : s->st == BLE_ST_OFF ? "Bluetooth off"
		   : s->st == BLE_ST_FORGETTING ? "Removing" ELLIPSIS : "Not connected");
	kit_kv_set(d->name, s->name);
	if (s->has_rssi)
		snprintf(b, sizeof(b), "%s%d dBm" DOT "%s%s", s->rssi < 0 ? MINUS : "", abs(s->rssi),
			 ble_signal_word(s->rssi), s->rssi_live ? "" : " (last seen)");
	else
		snprintf(b, sizeof(b), "Unknown");
	kit_kv_set(d->signal, b);
	if (s->battery >= 0)
		snprintf(b, sizeof(b), "%d%%", s->battery);
	else
		snprintf(b, sizeof(b), "Unknown");
	kit_kv_set(d->battery, b);
	date_text(s->paired, b, sizeof(b));
	kit_kv_set(d->paired, b);
	if (d->cadence) {
		const struct ble_value *x = &s->val[d->kind];

		if (s->st == BLE_ST_CONNECTED && x->has_cadence)
			snprintf(b, sizeof(b), "%.0f rpm", x->cadence);
		else
			snprintf(b, sizeof(b), NONE);
		kit_kv_set(d->cadence, b);
	}
}

static void disarm_fired(lv_timer_t *t)
{
	struct sensor_page *d = lv_timer_get_user_data(t);

	d->disarm = NULL;
	lv_label_set_text(d->forget, "Forget sensor");
}

static void forget_clicked(lv_event_t *e)
{
	struct sensor_page *d = lv_event_get_user_data(e);

	/* two taps: forgetting removes the bond */
	if (!d->disarm) {
		lv_label_set_text(d->forget, "Tap again to forget");
		d->disarm = lv_timer_create(disarm_fired, FORGET_ARM_MS, d);
		lv_timer_set_repeat_count(d->disarm, 1);
		return;
	}
	lv_timer_delete(d->disarm);
	d->disarm = NULL;
	/* the sensor this page was opened for */
	if (sys_ble_forget(d->addr) < 0) {
		lv_label_set_text(d->forget, "Could not forget");
		return;
	}
	app_back();
}

/* A key/value row as the design's sensor rows: 156 px plus the rule. */
static lv_obj_t *kv(lv_obj_t *parent, const char *key, const char *value)
{
	lv_obj_t *v = kit_kv_row(parent, key, value);

	lv_obj_set_height(lv_obj_get_parent(lv_obj_get_parent(v)), 156 + KIT_LINE);
	return v;
}

static void sensor_build(struct page *p, const void *arg)
{
	const struct page_bt_sensor_arg *a = arg;
	struct sensor_page *d = calloc(1, sizeof(*d));
	lv_obj_t *body = p->kp.body, *card, *col, *btn, *gap;

	p->priv = d;
	if (!d || !a)
		return;
	d->p = p;
	snprintf(d->addr, sizeof(d->addr), "%s", a->addr);
	d->kind = a->kind;
	/* margin-top 72; 3 px line2 frame radius 40; padding 48 56; corner
	 * marks; LIVE and the figure left, the 160 px icon right */
	gap = kit_box(body);
	lv_obj_set_size(gap, 1, 72);
	card = kit_box(body);
	lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_add_style(card, &kit_st.frame, 0);
	kit_corner_marks(card);		/* placed from the border: no padding here */
	card = kit_box(card);
	lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_ver(card, 48, 0);
	lv_obj_set_style_pad_hor(card, 56, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	col = kit_box(card);
	lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 4, 0);
	d->live = caps(col, "LIVE", KIT_INK_ACCT);
	/* 300 px figure, line-height .9; the unit 56 px muted, gap 16 */
	figure(col, &chefkit_bc600_300, 270, &chefkit_b400_56, 16, &d->val, &d->unit);
	d->icon = kit_icon(card, &chefkit_icon_160, 160, KIT_INK_ACCT, kind_glyph(d->kind));
	gap = kit_box(body);
	lv_obj_set_size(gap, 1, 48);
	d->status = kv(body, "Status", "");
	d->name = kv(body, "Name", "");
	d->signal = kv(body, "Signal", "");
	d->battery = kv(body, "Battery", "");
	kv(body, "Service", ble_kind_service(d->kind));
	d->paired = kv(body, "Paired", "");
	if (d->kind != BLE_HR)
		d->cadence = kv(body, "Cadence", "");
	kit_spacer(body);
	btn = kit_button_create(body, KIT_IC_TRASH_2, "Forget sensor", forget_clicked, d);
	d->forget = lv_obj_get_child(lv_obj_get_child(btn, 1), 0);
	gap = kit_box(body);
	lv_obj_set_size(gap, 1, 96);
	sensor_refresh(d);
}

static void sensor_destroy(struct page *p)
{
	struct sensor_page *d = p->priv;

	if (d && d->disarm)
		lv_timer_delete(d->disarm);
	free(d);
}

static void sensor_state(struct page *p, unsigned what)
{
	struct sensor_page *d = p->priv;
	const struct ble_sensor *s;

	if (!d || !(what & SYS_BLE))
		return;
	s = ble_sensor(&ble_view, d->addr);
	/* forgotten (here or elsewhere), or blesensd gone: this page is about
	 * a sensor that is not there, so it closes */
	if (!s || !(s->kinds & (1u << d->kind)) || s->st == BLE_ST_FORGETTING) {
		if (d->disarm) {
			lv_timer_delete(d->disarm);
			d->disarm = NULL;
		}
		if (app_top_page() == p)
			app_back();
		return;
	}
	sensor_refresh(d);
}

static void sensor_visible(struct page *p, bool visible)
{
	bt_sync();
	if (visible && p->priv)
		sensor_refresh(p->priv);
}

const struct page_ops page_bt_sensor_ops = {
	.build = sensor_build,
	.destroy = sensor_destroy,
	.on_state = sensor_state,
	.on_visible = sensor_visible,
};
