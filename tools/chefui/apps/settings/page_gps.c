/*
 * page_gps.c - the GPS pages: the switch with the off state (P11), the
 * searching card and sky plot (P12) or the fix card (P13) on one page, and
 * the satellites page (P14: sky plot, constellations, signal bars).
 *
 * The switch is gps-manager's (enable/disable, persisted by the manager).
 * While one of these pages is seen with the screen on, sys_gps_watch()
 * holds a "map" lease and reads gpsd's TPV and SKY into gps_view; leaving
 * or the screen going off releases it (the manager's 30 s grace covers
 * short trips). The position is only ever drawn: a capture masks it.
 *
 * Left out by decision (docs/next-steps/system-ui-2-handoff.md): the power
 * mode (Precise / Balanced / Saver) until a mode is measured to save power.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "fonts.h"
#include "gpsview.h"
#include "icons.h"
#include "pages.h"
#include "sysstate.h"

#define DOT " \xC2\xB7 "	/* " · " */
#define NONE "\xE2\x80\x93"	/* "–" */
#define PLUSMINUS "\xC2\xB1"
#define DEGREE "\xC2\xB0"

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

static bool gps_on(void)
{
	return sys_state.gps.state != SYS_GPS_UNAVAILABLE && sys_state.gps.state != SYS_GPS_DISABLED &&
	       sys_state.gps.enabled != 0;
}

/* Either GPS page seen with the screen on: lease and read gpsd. */
static void gps_sync(void)
{
	struct page *top = app_top_page();

	sys_gps_watch(app.pf.screen_is_on() &&
		      (top->kind == APP_PAGE_GPS || top->kind == APP_PAGE_GPS_SATS));
}

/* "CSS label" heading: Barlow Condensed 600 40, .12em, muted, upper case. */
static lv_obj_t *caps(lv_obj_t *parent, const char *t)
{
	lv_obj_t *l = kit_text(parent, &chefkit_bc600_40, 0, KIT_INK_MUT, t);

	lv_obj_set_style_text_letter_space(l, 5, 0);
	return l;
}

/* A column: heading (gap 4) over a big figure; right-aligned if right. */
static lv_obj_t *figure(lv_obj_t *parent, const char *key, const lv_font_t *f, int line,
			enum kit_ink ink, bool right, lv_obj_t **key_label)
{
	lv_obj_t *col = kit_box(parent), *k, *v;

	lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, right ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
			      right ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(col, 4, 0);
	k = caps(col, key);
	if (key_label)
		*key_label = k;
	v = kit_text(col, f, line, ink, "");
	return v;
}

/* ================================================================ sky plot */

struct sky {
	lv_obj_t *obj;
	int size, pad;
	lv_obj_t *pill[GPS_SATS_MAX];
	int n;
};

static void sky_ext(lv_event_t *e)
{
	lv_event_set_ext_draw_size(e, 72);	/* the compass letters */
}

static lv_obj_t *ring(lv_obj_t *parent, int inset, int size, lv_style_t *st)
{
	lv_obj_t *r = kit_box(parent);

	lv_obj_set_pos(r, inset, inset);
	lv_obj_set_size(r, size - 2 * inset, size - 2 * inset);
	lv_obj_add_style(r, st, 0);
	return r;
}

/* A compass letter: its box centred on (cx, cy) of the plot. */
static void compass(lv_obj_t *parent, const char *t, int cx, int cy)
{
	lv_obj_t *l = kit_text(parent, &chefkit_bc600_40, 0, KIT_INK_MUT, t), *box = lv_obj_get_parent(l);

	lv_obj_update_layout(box);
	lv_obj_set_pos(box, cx - lv_obj_get_width(box) / 2, cy - lv_obj_get_height(box) / 2);
}

/* size 720 (rings at 120 and 240, pad 50) or 840 (140, 280, pad 56);
 * all_points: N, E, S and W (P14), else N only (P12). */
static void sky_create(struct sky *k, lv_obj_t *parent, int size, int step, int pad, bool all_points)
{
	lv_obj_t *o;
	int h = 48;	/* the letters' line box */

	memset(k, 0, sizeof(*k));
	k->size = size;
	k->pad = pad;
	k->obj = kit_box(parent);
	lv_obj_set_size(k->obj, size, size);
	lv_obj_set_overflow_visible(k->obj, true);
	lv_obj_add_event_cb(k->obj, sky_ext, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
	lv_obj_refresh_ext_draw_size(k->obj);
	ring(k->obj, 0, size, &kit_st.ring);
	ring(k->obj, step, size, &kit_st.ring_in);
	ring(k->obj, 2 * step, size, &kit_st.ring_in);
	/* axes: 3 px, margin -1 from the centre line */
	o = kit_box(k->obj);
	lv_obj_add_style(o, &kit_st.hair, 0);
	lv_obj_set_pos(o, size / 2 - 1, 0);
	lv_obj_set_size(o, KIT_LINE, size);
	o = kit_box(k->obj);
	lv_obj_add_style(o, &kit_st.hair, 0);
	lv_obj_set_pos(o, 0, size / 2 - 1);
	lv_obj_set_size(o, size, KIT_LINE);
	/* N: top -64 (box top), centred; S: bottom -64; E: right -56; W: left -60 */
	compass(k->obj, "N", size / 2, -64 + h / 2);
	if (all_points) {
		compass(k->obj, "S", size / 2, size + 64 - h / 2);
		compass(k->obj, "E", size + 56 - 10, size / 2);
		compass(k->obj, "W", -60 + 12, size / 2);
	}
}

static void sky_set(struct sky *k, const struct gps_view *v)
{
	double r0 = k->size / 2.0;
	int i, shown = 0;

	for (i = 0; i < v->n_sats; i++) {
		const struct gps_sat *s = &v->sats[i];
		double r, a;
		lv_obj_t *p;
		char lab[8];

		if (s->el < 0 || s->az < 0)
			continue;	/* not placed in the sky (yet) */
		if (shown == k->n) {
			lv_obj_t *l;

			p = k->pill[k->n++] = kit_box(k->obj);
			lv_obj_set_size(p, 100, 56);
			lv_obj_set_flex_flow(p, LV_FLEX_FLOW_ROW);
			lv_obj_set_flex_align(p, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
					      LV_FLEX_ALIGN_CENTER);
			l = kit_text(p, &chefkit_b500_32, 0, KIT_INK_FG, "");
			lv_obj_remove_style(l, &kit_st.ink[KIT_INK_FG], 0);	/* the pill's colour */
		}
		p = k->pill[shown++];
		lv_obj_remove_style(p, &kit_st.sat_on, 0);
		lv_obj_remove_style(p, &kit_st.sat_off, 0);
		lv_obj_add_style(p, s->used ? &kit_st.sat_on : &kit_st.sat_off, 0);
		gps_sat_label(s, lab, sizeof(lab));
		set_text(lv_obj_get_child(lv_obj_get_child(p, 0), 0), lab);
		r = (90.0 - s->el) / 90.0 * (r0 - k->pad);
		a = s->az * M_PI / 180.0;
		lv_obj_set_pos(p, (int32_t)lround(r0 + r * sin(a)) - 50, (int32_t)lround(r0 - r * cos(a)) - 28);
		lv_obj_set_hidden(p, false);
	}
	for (i = shown; i < k->n; i++)
		lv_obj_set_hidden(k->pill[i], true);
}

/* ================================================================ GPS page */

struct gps_page {
	struct page *p;
	struct kit_master master;
	struct kit_empty empty;
	/* searching (P12) */
	lv_obj_t *search, *s_status, *s_time, *s_text;
	struct sky sky;
	/* locked (P13) */
	lv_obj_t *locked, *l_fix, *l_acc, *l_val[6], *l_pos;
	struct kit_row sats;
	lv_timer_t *tick;
	bool masked;
};

static void gps_refresh(struct gps_page *g);

static void elapsed(int64_t since, char *b, size_t n)
{
	int64_t s = since ? (mono_ms() - since) / 1000 : 0;

	if (s < 0)
		s = 0;
	snprintf(b, n, "%d:%02d", (int)(s / 60), (int)(s % 60));
}

static void tick_fired(lv_timer_t *t)
{
	struct gps_page *g = lv_timer_get_user_data(t);
	char b[16];

	elapsed(gps_view.lease_ms, b, sizeof(b));
	set_text(g->s_time, b);
}

static void master_changed(lv_event_t *e)
{
	struct gps_page *g = lv_event_get_user_data(e);
	bool on = kit_switch_get(g->master.sw);

	if (sys_gps_set_enabled(on) < 0)
		kit_switch_set(g->master.sw, !on);	/* no manager to ask */
}

static void sats_clicked(lv_event_t *e)
{
	(void)e;
	app_open(APP_PAGE_GPS_SATS, "Satellites");
}

static void fmt_pos(char *b, size_t n, double lat, double lon)
{
	snprintf(b, n, "%.5f" DEGREE " %c  %.5f" DEGREE " %c", fabs(lat), lat < 0 ? 'S' : 'N', fabs(lon),
		 lon < 0 ? 'W' : 'E');
}

static void gps_refresh(struct gps_page *g)
{
	const struct sys_state *s = &sys_state;
	const struct gps_view *v = &gps_view;
	bool on = gps_on(), fix = s->gps.state == SYS_GPS_RUNNING && v->mode >= 2;
	int used = gps_sats_used(v);
	char b[96];

	kit_master_set(&g->master, s->gps.enabled == 1 ||
		       (s->gps.enabled < 0 && s->gps.state != SYS_GPS_UNAVAILABLE &&
			s->gps.state != SYS_GPS_DISABLED),
		       s->gps.state == SYS_GPS_UNAVAILABLE ? "Unavailable" : !on ? "Off"
		       : s->gps.state == SYS_GPS_FAILED ? "On" DOT "failed"
		       : fix ? "On" DOT "locked" : "On" DOT "searching");
	lv_obj_set_disabled(g->master.sw, s->gps.state == SYS_GPS_UNAVAILABLE);
	lv_obj_set_hidden(g->empty.obj, on);
	lv_obj_set_hidden(g->search, !on || fix);
	lv_obj_set_hidden(g->locked, !on || !fix);
	if (g->tick)
		lv_timer_pause(g->tick);
	if (!on) {
		if (s->gps.state == SYS_GPS_UNAVAILABLE)
			kit_empty_set(&g->empty, KIT_IC_LOCATE_OFF, "GPS unavailable",
				      "gps-manager is not running.");
		else
			kit_empty_set(&g->empty, KIT_IC_LOCATE_OFF, "No position",
				      "Rides record without a track or map.");
		return;
	}
	if (!fix) {
		set_text(g->s_status, s->gps.state == SYS_GPS_FAILED ? "Failed"
			 : s->gps.state == SYS_GPS_OFF && !v->lease_ms ? "Starting" : "No fix");
		if (s->gps.state == SYS_GPS_FAILED)
			snprintf(b, sizeof(b), "The GPS did not start. It tries again on its own.");
		else
			snprintf(b, sizeof(b), "%d satellite%s visible, %d in use. Stay still with a "
				 "clear view of the sky.", v->n_sats, v->n_sats == 1 ? "" : "s", used);
		set_text(g->s_text, b);
		tick_fired(g->tick);
		if (g->tick && app_page_visible(g->p))
			lv_timer_resume(g->tick);	/* ticks only while seen */
		sky_set(&g->sky, v);
		return;
	}
	set_text(g->l_fix, v->mode == 3 ? "3D" : "2D");
	if (isnan(v->eph))
		snprintf(b, sizeof(b), NONE);
	else
		snprintf(b, sizeof(b), PLUSMINUS "%.0f m", v->eph);
	set_text(g->l_acc, b);
	snprintf(b, sizeof(b), "%d/%d", used, v->n_sats);
	set_text(g->l_val[0], b);
	if (v->first_fix_ms && v->lease_ms)
		snprintf(b, sizeof(b), "%d s", (int)((v->first_fix_ms - v->lease_ms + 500) / 1000));
	else
		snprintf(b, sizeof(b), NONE);
	set_text(g->l_val[1], b);
	if (isnan(v->alt))
		snprintf(b, sizeof(b), NONE);
	else
		snprintf(b, sizeof(b), "%.0f m", v->alt);
	set_text(g->l_val[2], b);
	snprintf(b, sizeof(b), isnan(v->hdop) ? NONE : "%.1f", v->hdop);
	set_text(g->l_val[3], b);
	snprintf(b, sizeof(b), isnan(v->pdop) ? NONE : "%.1f", v->pdop);
	set_text(g->l_val[4], b);
	snprintf(b, sizeof(b), isnan(v->speed) ? NONE : "%.1f", v->speed * 3.6);
	set_text(g->l_val[5], b);
	if (g->masked || isnan(v->lat) || isnan(v->lon))
		snprintf(b, sizeof(b), NONE);
	else
		fmt_pos(b, sizeof(b), v->lat, v->lon);
	set_text(g->l_pos, b);
	{
		int u[4], seen[4], systems = 0;

		gps_constellations(v, u, seen);
		for (int i = 0; i < 4; i++)
			systems += seen[i] > 0;
		snprintf(b, sizeof(b), "%d in use" DOT "%d system%s", used, systems, systems == 1 ? "" : "s");
		kit_row_set(&g->sats, NULL, NULL, b, KIT_IC_CHEVRON_RIGHT);
	}
}

/* P12: dashed (here solid) card: STATUS / SEARCHING figures, the text. */
static void build_search(struct gps_page *g, lv_obj_t *body)
{
	lv_obj_t *card, *row, *tb, *l;
	const lv_font_t *f = &chefkit_b400_46;
	int32_t pitch = 62, space = pitch - f->line_height, y0;

	g->search = kit_box(body);
	lv_obj_set_size(g->search, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(g->search, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(g->search, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_top(g->search, 72, 0);
	/* border 3 (dashed in the design) line2, radius 40, padding 48 56, gap 36 */
	card = kit_box(g->search);
	lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_add_style(card, &kit_st.frame, 0);
	lv_obj_set_style_pad_ver(card, 48, 0);
	lv_obj_set_style_pad_hor(card, 56, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(card, 36, 0);
	row = kit_box(card);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	g->s_status = figure(row, "STATUS", &chefkit_bc600_150, 143, KIT_INK_FG, false, NULL);
	g->s_time = figure(row, "SEARCHING", &chefkit_bc600_150, 143, KIT_INK_FG, true, NULL);
	/* 46 px muted, line-height 1.35, wrapped */
	tb = kit_box(card);
	lv_obj_set_size(tb, lv_pct(100), LV_SIZE_CONTENT);
	l = lv_label_create(tb);
	lv_obj_set_event_bubble(l, true);
	lv_obj_set_width(l, lv_pct(100));
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(l, f, 0);
	lv_obj_set_style_text_line_space(l, space, 0);
	kit_set_ink(l, KIT_INK_MUT);
	lv_label_set_text(l, "");
	y0 = kit_label_y(f, kit_css_baseline(f, pitch));
	lv_obj_set_style_pad_top(tb, y0 > 0 ? y0 : 0, 0);
	lv_obj_set_style_pad_bottom(tb, space - y0 > 0 ? space - y0 : 0, 0);
	g->s_text = l;
	/* the sky plot, 720 px, margin-top 80 */
	l = kit_box(g->search);
	lv_obj_set_size(l, 1, 80);
	sky_create(&g->sky, g->search, 720, 120, 50, false);
}

/* P13: the fix card with corner marks, then the Satellites row. */
static void build_locked(struct gps_page *g, lv_obj_t *body)
{
	static const char *const keys[6] = { "Satellites", "First fix", "Altitude", "HDOP", "PDOP",
		"Speed km/h" };
	lv_obj_t *card, *head, *grid, *cell, *pos, *gap;
	int i;

	g->locked = kit_box(body);
	lv_obj_set_size(g->locked, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(g->locked, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_top(g->locked, 72, 0);
	card = kit_box(g->locked);
	lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_add_style(card, &kit_st.frame, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	kit_corner_marks(card);
	/* padding 48 56: FIX left (accent), ACCURACY right */
	head = kit_box(card);
	lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_ver(head, 48, 0);
	lv_obj_set_style_pad_hor(head, 56, 0);
	lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	g->l_fix = figure(head, "FIX", &chefkit_bc600_150, 143, KIT_INK_ACCT, false, NULL);
	g->l_acc = figure(head, "ACCURACY", &chefkit_bc600_150, 143, KIT_INK_FG, true, NULL);
	/* 3 x 2 grid, a rule on top; cells padding 36 40, rules left and below */
	grid = kit_box(card);
	lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_add_style(grid, &kit_st.rule_top_bottom, 0);
	lv_obj_set_style_border_side(grid, LV_BORDER_SIDE_TOP, 0);
	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_COLUMN);
	for (i = 0; i < 6; i++) {
		static lv_obj_t *line;

		if (i % 3 == 0) {
			line = kit_box(grid);
			lv_obj_set_size(line, lv_pct(100), LV_SIZE_CONTENT);
			lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
		}
		cell = kit_box(line);
		lv_obj_set_height(cell, LV_SIZE_CONTENT);
		lv_obj_set_flex_grow(cell, 1);		/* repeat(3, 1fr) */
		lv_obj_add_style(cell, &kit_st.cell_rule, 0);
		lv_obj_set_style_border_side(cell, LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_BOTTOM, 0);
		lv_obj_set_style_pad_ver(cell, 36, 0);
		lv_obj_set_style_pad_hor(cell, 40, 0);
		lv_obj_set_style_pad_left(cell, 40 - KIT_LINE, 0);
		lv_obj_set_style_pad_bottom(cell, 36 - KIT_LINE, 0);
		lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_style_pad_row(cell, 6, 0);
		kit_text(cell, &chefkit_b400_36, 0, KIT_INK_MUT, keys[i]);
		g->l_val[i] = kit_text(cell, &chefkit_bc600_76, 76, KIT_INK_FG, "");
	}
	/* Position: padding 36 40 40 */
	pos = kit_box(card);
	lv_obj_set_size(pos, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_top(pos, 36, 0);
	lv_obj_set_style_pad_hor(pos, 40, 0);
	lv_obj_set_style_pad_bottom(pos, 40, 0);
	lv_obj_set_flex_flow(pos, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(pos, 6, 0);
	kit_text(pos, &chefkit_b400_36, 0, KIT_INK_MUT, "Position");
	g->l_pos = kit_text(pos, &chefkit_bc600_68, 71, KIT_INK_FG, "");
	/* the Satellites row, margin-top 72 (the power mode is left out) */
	gap = kit_box(g->locked);
	lv_obj_set_size(gap, 1, 72);
	kit_list_row_create(&g->sats, g->locked, KIT_IC_SATELLITE, "Satellites", "", &chefkit_icon_64,
			    KIT_IC_CHEVRON_RIGHT, sats_clicked, g);
	lv_obj_add_style(g->sats.obj, &kit_st.rule_top_bottom, 0);
	lv_obj_set_height(g->sats.obj, KIT_ROW_H + 2 * KIT_LINE);
}

static void gps_build(struct page *p, const void *arg)
{
	struct gps_page *g = calloc(1, sizeof(*g));
	lv_obj_t *body = p->kp.body;

	(void)arg;
	p->priv = g;
	if (!g)
		return;
	g->p = p;
	kit_master_create(&g->master, body, "GPS", master_changed, g);
	kit_empty_create(&g->empty, body, KIT_IC_LOCATE_OFF, "", "");
	build_search(g, body);
	build_locked(g, body);
	g->tick = lv_timer_create(tick_fired, 1000, g);
	lv_timer_pause(g->tick);
	gps_refresh(g);
}

static void gps_destroy(struct page *p)
{
	struct gps_page *g = p->priv;

	if (g && g->tick)
		lv_timer_delete(g->tick);
	free(g);
}

static void gps_state(struct page *p, unsigned what)
{
	if (p->priv && (what & (SYS_GPS | SYS_GPS_VIEW)))
		gps_refresh(p->priv);
}

static void gps_visible(struct page *p, bool visible)
{
	struct gps_page *g = p->priv;

	gps_sync();
	if (g) {
		if (visible)
			gps_refresh(g);
		else if (g->tick)
			lv_timer_pause(g->tick);
	}
}

static void gps_snapshot(struct page *p, bool begin)
{
	struct gps_page *g = p->priv;

	if (g) {
		g->masked = begin;	/* no position in a file */
		gps_refresh(g);
	}
}

const struct page_ops page_gps_ops = {
	.build = gps_build,
	.destroy = gps_destroy,
	.on_state = gps_state,
	.on_visible = gps_visible,
	.on_snapshot = gps_snapshot,
};

/* ============================================================ satellites */

#define BARS_LABELS_MAX 24	/* more bars than this: no numbers under them */

struct sats_page {
	struct sky sky;
	lv_obj_t *cons_used[4], *cons_seen[4];
	lv_obj_t *bars, *labels;
	lv_obj_t *bar[GPS_SATS_MAX], *bar_val[GPS_SATS_MAX], *bar_lab[GPS_SATS_MAX];
	int n_bars;
};

static int sat_cmp(const void *a, const void *b)
{
	static const int rank[8] = { 0, 4, 2, 3, 6, 5, 1, 7 };	/* G R E C, then the rest */
	const struct gps_sat *x = a, *y = b;
	int rx = x->gnss >= 0 && x->gnss < 8 ? rank[x->gnss] : 9;
	int ry = y->gnss >= 0 && y->gnss < 8 ? rank[y->gnss] : 9;

	return rx != ry ? rx - ry : x->svid - y->svid;
}

static void bars_set(struct sats_page *sp, const struct gps_view *v)
{
	struct gps_sat sorted[GPS_SATS_MAX];
	bool labels = v->n_sats <= BARS_LABELS_MAX;
	int i, n = v->n_sats;

	memcpy(sorted, v->sats, (size_t)n * sizeof(sorted[0]));
	qsort(sorted, (size_t)n, sizeof(sorted[0]), sat_cmp);
	for (i = 0; i < n; i++) {
		const struct gps_sat *s = &sorted[i];
		double ss = s->ss < 0 ? 0 : s->ss;
		int h = (int)lround(ss / 50.0 * 300.0);
		char b[16];

		if (i == sp->n_bars) {
			lv_obj_t *col = kit_box(sp->bars);

			lv_obj_set_height(col, lv_pct(100));
			lv_obj_set_flex_grow(col, 1);
			lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
			lv_obj_set_flex_align(col, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
			lv_obj_set_style_pad_row(col, 8, 0);
			sp->bar_val[i] = kit_text(col, &chefkit_b400_26, 0, KIT_INK_MUT, "");
			/* the bar runs 12 px below the column, which clips its lower
			 * rounded corners: rounded on top only, as the design */
			sp->bar[i] = kit_box(col);
			lv_obj_set_width(sp->bar[i], lv_pct(100));
			lv_obj_set_style_translate_y(sp->bar[i], 12, 0);
			sp->bar_lab[i] = kit_text(sp->labels, &chefkit_b500_24, 0, KIT_INK_MUT, "");
			lv_obj_set_flex_grow(lv_obj_get_parent(sp->bar_lab[i]), 1);
			lv_obj_set_width(sp->bar_lab[i], lv_pct(100));
			lv_obj_set_style_text_align(sp->bar_lab[i], LV_TEXT_ALIGN_CENTER, 0);
			sp->n_bars++;
		}
		lv_obj_remove_style(sp->bar[i], &kit_st.bar_on, 0);
		lv_obj_remove_style(sp->bar[i], &kit_st.bar_off, 0);
		lv_obj_add_style(sp->bar[i], s->used ? &kit_st.bar_on : &kit_st.bar_off, 0);
		lv_obj_set_height(sp->bar[i], (h < 0 ? 0 : h > 300 ? 300 : h) + 12);
		snprintf(b, sizeof(b), "%.0f", ss);
		set_text(sp->bar_val[i], b);
		lv_obj_set_hidden(lv_obj_get_parent(sp->bar_val[i]), !labels);
		gps_sat_label(s, b, sizeof(b));
		set_text(sp->bar_lab[i], b);
		lv_obj_set_hidden(lv_obj_get_parent(sp->bar[i]), false);	/* its column */
		lv_obj_set_hidden(lv_obj_get_parent(sp->bar_lab[i]), !labels);
	}
	for (; i < sp->n_bars; i++) {
		lv_obj_set_hidden(lv_obj_get_parent(sp->bar[i]), true);
		lv_obj_set_hidden(lv_obj_get_parent(sp->bar_lab[i]), true);
	}
}

static void sats_refresh(struct sats_page *sp)
{
	const struct gps_view *v = &gps_view;
	int used[4], seen[4];
	char b[16];

	sky_set(&sp->sky, v);
	gps_constellations(v, used, seen);
	for (int i = 0; i < 4; i++) {
		snprintf(b, sizeof(b), "%d", used[i]);
		set_text(sp->cons_used[i], b);
		snprintf(b, sizeof(b), "/%d", seen[i]);
		set_text(sp->cons_seen[i], b);
	}
	bars_set(sp, v);
}

static lv_obj_t *swatch(lv_obj_t *parent, lv_style_t *st, const char *t)
{
	lv_obj_t *item = kit_box(parent), *sq;

	lv_obj_set_size(item, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(item, 14, 0);
	sq = kit_box(item);
	lv_obj_set_size(sq, 36, 36);
	lv_obj_add_style(sq, st, 0);
	return kit_text(item, &chefkit_b400_36, 0, KIT_INK_MUT, t);
}

static void sats_build(struct page *p, const void *arg)
{
	static const char *const names[4] = { "GPS", "GLONASS", "Galileo", "BeiDou" };
	struct sats_page *sp = calloc(1, sizeof(*sp));
	lv_obj_t *body = p->kp.body, *o, *row, *cell, *num;
	const lv_font_t *big = &chefkit_bc600_76, *small = &chefkit_bc600_48;
	int32_t base = kit_css_baseline(big, 76);

	(void)arg;
	p->priv = sp;
	if (!sp)
		return;
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	o = kit_box(body);
	lv_obj_set_size(o, 1, 120);
	sky_create(&sp->sky, body, 840, 140, 56, true);
	/* legend: margin-top 96, centred, gap 40 */
	o = kit_box(body);
	lv_obj_set_size(o, 1, 96);
	row = kit_box(body);
	lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(row, 40, 0);
	swatch(row, &kit_st.swatch_on, "In use");
	swatch(row, &kit_st.swatch_off, "Visible");
	/* constellations: margin-top 64, rules above and below, 4 columns */
	o = kit_box(body);
	lv_obj_set_size(o, 1, 64);
	row = kit_box(body);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_add_style(row, &kit_st.rule_top_bottom, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	for (int i = 0; i < 4; i++) {
		lv_obj_t *l;

		cell = kit_box(row);
		lv_obj_set_height(cell, LV_SIZE_CONTENT);
		lv_obj_set_flex_grow(cell, 1);
		lv_obj_add_style(cell, &kit_st.cell_rule, 0);
		lv_obj_set_style_pad_ver(cell, 32, 0);
		lv_obj_set_style_pad_left(cell, 28 - KIT_LINE, 0);
		lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_style_pad_row(cell, 4, 0);
		kit_text(cell, &chefkit_b400_36, 0, KIT_INK_MUT, names[i]);
		/* "5" then a smaller "/6" on the same baseline (line-height 1) */
		num = kit_box(cell);
		lv_obj_set_size(num, LV_SIZE_CONTENT, 76);
		lv_obj_set_flex_flow(num, LV_FLEX_FLOW_ROW);
		sp->cons_used[i] = kit_text(num, big, 76, KIT_INK_FG, "");
		o = kit_box(num);
		lv_obj_set_size(o, LV_SIZE_CONTENT, 76);
		lv_obj_set_overflow_visible(o, true);
		l = lv_label_create(o);
		lv_obj_set_style_text_font(l, small, 0);
		kit_set_ink(l, KIT_INK_MUT);
		lv_label_set_text(l, "");
		lv_obj_set_y(l, kit_label_y(small, base));
		sp->cons_seen[i] = l;
	}
	/* SIGNAL ... dB-Hz: padding 56 0 24 */
	row = kit_box(body);
	lv_obj_set_size(row, lv_pct(100), 56 + 48 + 24);
	lv_obj_set_style_pad_top(row, 56, 0);
	lv_obj_set_style_pad_bottom(row, 24, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	caps(row, "SIGNAL");
	kit_text(row, &chefkit_b400_36, 0, KIT_INK_MUT, "dB-Hz");
	/* bars: 380 px, gap 8, line2 rule under them; labels below, gap 8 */
	sp->bars = kit_box(body);
	lv_obj_set_size(sp->bars, lv_pct(100), 380 + KIT_LINE);
	lv_obj_add_style(sp->bars, &kit_st.ring, 0);	/* line2 border colour */
	lv_obj_set_style_radius(sp->bars, 0, 0);
	lv_obj_set_style_border_side(sp->bars, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_flex_flow(sp->bars, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(sp->bars, 8, 0);
	sp->labels = kit_box(body);
	lv_obj_set_size(sp->labels, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_top(sp->labels, 12, 0);
	lv_obj_set_flex_flow(sp->labels, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(sp->labels, 8, 0);
	sats_refresh(sp);
}

static void sats_destroy(struct page *p)
{
	free(p->priv);
}

static void sats_state(struct page *p, unsigned what)
{
	if (p->priv && (what & SYS_GPS_VIEW))
		sats_refresh(p->priv);
}

static void sats_visible(struct page *p, bool visible)
{
	gps_sync();
	if (visible && p->priv)
		sats_refresh(p->priv);
}

const struct page_ops page_gps_sats_ops = {
	.build = sats_build,
	.destroy = sats_destroy,
	.on_state = sats_state,
	.on_visible = sats_visible,
};
