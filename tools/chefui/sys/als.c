/*
 * als.c - ambient light logic (see als.h).
 */
#include <math.h>
#include <stdlib.h>

#include "als.h"
#include "settings.h"

/* Lux -> percent anchors, piecewise linear in log10(lux + 1). A starting
 * point: a room at 250 to 320 lux measured live gives about 30 %. */
static const struct { double lux; int pct; } curve[] = {
	{ 0, 5 }, { 1, 8 }, { 10, 14 }, { 50, 20 }, { 300, 30 },
	{ 1000, 45 }, { 5000, 75 }, { 20000, 100 },
};
#define NCURVE ((int)(sizeof(curve) / sizeof(curve[0])))

static double lux_x(double lux)
{
	return log10((lux > 0 ? lux : 0) + 1.0);
}

static double curve_x(double x)
{
	if (x <= lux_x(curve[0].lux))
		return curve[0].pct;
	for (int i = 1; i < NCURVE; i++) {
		double x0 = lux_x(curve[i - 1].lux), x1 = lux_x(curve[i].lux);

		if (x <= x1)
			return curve[i - 1].pct + (curve[i].pct - curve[i - 1].pct) * (x - x0) / (x1 - x0);
	}
	return curve[NCURVE - 1].pct;
}

int als_curve_percent(double lux)
{
	return (int)lround(curve_x(lux_x(lux)));
}

void als_params_default(struct als_params *p)
{
	p->light_lux = ALS_LIGHT_LUX_DEFAULT;
	p->dark_lux = ALS_DARK_LUX_DEFAULT;
	p->light_hold_s = ALS_LIGHT_HOLD_S_DEFAULT;
	p->dark_hold_s = ALS_DARK_HOLD_S_DEFAULT;
}

bool als_params_valid(const struct als_params *p)
{
	return p->dark_lux >= 0 && p->dark_lux < p->light_lux &&
	       p->light_hold_s >= ALS_HOLD_S_MIN && p->light_hold_s <= ALS_HOLD_S_MAX &&
	       p->dark_hold_s >= ALS_HOLD_S_MIN && p->dark_hold_s <= ALS_HOLD_S_MAX;
}

void als_init(struct als *a, const struct als_params *p)
{
	*a = (struct als){ .p = *p, .applied = -1, .above_since = -1, .below_since = -1 };
}

void als_stream_restart(struct als *a)
{
	a->have_prev = false;
	a->above_since = a->below_since = -1;
}

void als_theme_reset(struct als *a)
{
	a->light = false;
	a->above_since = a->below_since = -1;
	/* decided at once from the held lux, if this stream has one */
	if (a->have_prev && a->cur_lux >= a->p.light_lux)
		a->light = true;
}

void als_set_applied(struct als *a, int pct)
{
	a->applied = pct < 0 ? -1 : pct;
}

void als_set_offset(struct als *a, int offset)
{
	a->offset = offset;
}

int als_curve_now(const struct als *a)
{
	return a->xf_known ? (int)lround(curve_x(a->xf)) : -1;
}

bool als_offset_for(const struct als *a, int slider, int *offset)
{
	int o;

	*offset = 0;
	if (!a->xf_known)
		return false;
	o = slider - als_curve_now(a);
	*offset = o < UI_AUTO_OFFSET_MIN ? UI_AUTO_OFFSET_MIN : o > UI_AUTO_OFFSET_MAX ? UI_AUTO_OFFSET_MAX : o;
	return true;
}

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static int target_of(const struct als *a, double x)
{
	return clampi((int)lround(curve_x(x)) + a->offset, UI_BRIGHTNESS_MIN, UI_BRIGHTNESS_MAX);
}

/* Would the applied level still change once the filter reached the held lux? */
static bool unsettled(const struct als *a)
{
	int t = target_of(a, lux_x(a->cur_lux));

	return a->applied < 0 || abs(t - a->applied) >= ALS_DEADBAND ||
	       ((t == UI_BRIGHTNESS_MIN || t == UI_BRIGHTNESS_MAX) && t != a->applied);
}

static void bright_apply(struct als *a, struct als_event *ev)
{
	int t = target_of(a, a->xf);

	if (a->applied < 0 || abs(t - a->applied) >= ALS_DEADBAND ||
	    ((t == UI_BRIGHTNESS_MIN || t == UI_BRIGHTNESS_MAX) && t != a->applied)) {
		a->applied = t;
		ev->bright_changed = true;
		ev->bright = t;
	}
}

static void theme_set(struct als *a, bool light, double lux, int64_t held, struct als_event *ev)
{
	a->above_since = a->below_since = -1;
	if (a->light == light)
		return;
	a->light = light;
	ev->theme_changed = true;
	ev->light = light;
	ev->lux = lux;
	ev->held_ms = held;
}

/* Everything the held lux implies up to now. */
static void advance(struct als *a, int64_t now, struct als_event *ev)
{
	if (!a->have_prev)
		return;
	if (now > a->t_xf) {
		double x = lux_x(a->cur_lux);
		double tau = x > a->xf ? ALS_TAU_UP_MS : ALS_TAU_DOWN_MS;

		a->xf += (x - a->xf) * (1.0 - exp(-(double)(now - a->t_xf) / tau));
		a->t_xf = now;
	}
	bright_apply(a, ev);
	if (a->cur_lux >= a->p.light_lux && a->above_since >= 0 && !a->light &&
	    now - a->above_since >= (int64_t)a->p.light_hold_s * 1000)
		theme_set(a, true, a->cur_lux, now - a->above_since, ev);
	else if (a->cur_lux <= a->p.dark_lux && a->below_since >= 0 && a->light &&
		 now - a->below_since >= (int64_t)a->p.dark_hold_s * 1000)
		theme_set(a, false, a->cur_lux, now - a->below_since, ev);
}

void als_tick(struct als *a, int64_t now_ms, struct als_event *ev)
{
	*ev = (struct als_event){ 0 };
	advance(a, now_ms, ev);
}

void als_feed(struct als *a, double lux, int64_t t_ms, struct als_event *ev)
{
	bool first = !a->have_prev;

	*ev = (struct als_event){ 0 };
	advance(a, t_ms, ev);		/* the previous lux held until now */
	a->cur_lux = lux;
	a->have_prev = true;
	if (first) {
		a->xf = lux_x(lux);
		a->t_xf = t_ms;
		a->xf_known = true;
		bright_apply(a, ev);
	}
	if (lux >= a->p.light_lux) {
		a->below_since = -1;
		if (a->above_since < 0)
			a->above_since = t_ms;
	} else if (lux <= a->p.dark_lux) {
		a->above_since = -1;
		if (a->below_since < 0)
			a->below_since = t_ms;
	} else {
		a->above_since = a->below_since = -1;
	}
	if (first) {
		/* the first sample of a stream decides: in between keeps the theme */
		if (lux >= a->p.light_lux)
			theme_set(a, true, lux, 0, ev);
		else if (lux <= a->p.dark_lux)
			theme_set(a, false, lux, 0, ev);
	}
}

int64_t als_next_deadline_ms(const struct als *a)
{
	int64_t d = -1, c;

	if (!a->have_prev)
		return -1;
	if (!a->light && a->cur_lux >= a->p.light_lux && a->above_since >= 0)
		d = a->above_since + (int64_t)a->p.light_hold_s * 1000;
	else if (a->light && a->cur_lux <= a->p.dark_lux && a->below_since >= 0)
		d = a->below_since + (int64_t)a->p.dark_hold_s * 1000;
	if (unsettled(a)) {
		c = a->t_xf + ALS_TICK_MS;
		if (d < 0 || c < d)
			d = c;
	}
	return d;
}
