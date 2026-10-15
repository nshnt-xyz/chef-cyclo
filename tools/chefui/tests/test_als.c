/* test_als.c - the pure ambient light logic (sys/als.c) on synthetic sequences.
 * The sensor is on-change only: a sample is the light until the next one, and
 * holds and smoothing complete through als_tick() at als_next_deadline_ms(). */
#include <math.h>
#include <stdlib.h>

#include "als.h"
#include "check.h"
#include "settings.h"

#define STEP 200	/* ms between samples when a test feeds a steady 5 Hz */

struct run {
	int bright_events, theme_events, last_bright, ticks;
	bool last_light;
	int64_t last_held;
};

static void note(struct run *r, const struct als_event *ev)
{
	if (ev->bright_changed) {
		r->bright_events++;
		r->last_bright = ev->bright;
	}
	if (ev->theme_changed) {
		r->theme_events++;
		r->last_light = ev->light;
		r->last_held = ev->held_ms;
	}
}

/* Process every deadline up to and including t_end. */
static void run_to(struct als *a, int64_t t_end, struct run *r)
{
	struct als_event ev;

	for (int n = 0; n < 1000000; n++) {
		int64_t d = als_next_deadline_ms(a);

		if (d < 0 || d > t_end)
			return;
		als_tick(a, d, &ev);
		r->ticks++;
		note(r, &ev);
	}
	CHECK(0 && "deadline loop did not end");
}

/* One sample at t (after the deadlines before it). */
static void sample(struct als *a, double lux, int64_t t, struct run *r)
{
	struct als_event ev;

	run_to(a, t, r);
	als_feed(a, lux, t, &ev);
	note(r, &ev);
}

/* The same lux every STEP ms from t0 up to (excluding) t1. */
static void steady(struct als *a, double lux, int64_t t0, int64_t t1, struct run *r)
{
	for (int64_t t = t0; t < t1; t += STEP)
		sample(a, lux, t, r);
}

static void fresh(struct als *a)
{
	struct als_params p;

	als_params_default(&p);
	als_init(a, &p);
}

/* A stream whose claim was acknowledged at t: wake grace runs ALS_WAKE_GRACE_MS from t. */
static void fresh_warm(struct als *a, int64_t t)
{
	fresh(a);
	als_warmup_start(a, t);
}

static void test_curve(void)
{
	static const struct { double lux; int pct; } anchors[] = {
		{ 0, 5 }, { 1, 8 }, { 10, 14 }, { 50, 20 }, { 300, 30 }, { 1000, 45 }, { 5000, 75 }, { 20000, 100 },
	};
	int prev = 0;

	for (unsigned i = 0; i < sizeof(anchors) / sizeof(anchors[0]); i++)
		CHECK(als_curve_percent(anchors[i].lux) == anchors[i].pct);
	CHECK(als_curve_percent(-5) == 5);
	CHECK(als_curve_percent(1e6) == 100);
	CHECK(als_curve_percent(100) == 24);	/* log interpolation between 50 and 300 */
	for (double lux = 0; lux < 30000; lux = lux * 1.05 + 0.5) {
		int c = als_curve_percent(lux);

		CHECK(c >= prev && c >= 5 && c <= 100);
		prev = c;
	}
}

static void test_clamp(void)
{
	struct als a;
	struct run r = { 0 };

	fresh(&a);
	als_set_offset(&a, -50);
	sample(&a, 0, 0, &r);
	CHECK(r.last_bright == UI_BRIGHTNESS_MIN);
	fresh(&a);
	r = (struct run){ 0 };
	als_set_offset(&a, 50);
	sample(&a, 1e6, 0, &r);
	CHECK(r.last_bright == UI_BRIGHTNESS_MAX);
	fresh(&a);
	r = (struct run){ 0 };
	als_set_offset(&a, 10);
	sample(&a, 300, 0, &r);
	CHECK(r.last_bright == 40);
}

static void test_smoothing(void)
{
	struct als up, down;
	struct run r = { 0 };
	struct als_event ev;
	double x0 = log10(11.0), x1 = log10(5001.0), pu, pd;

	/* one sample, then silence: the level reaches the target through ticks only */
	fresh(&up);
	sample(&up, 10, 0, &r);
	CHECK(als_curve_now(&up) == 14 && r.last_bright == 14);
	run_to(&up, 100000, &r);
	CHECK(als_next_deadline_ms(&up) == -1);
	r = (struct run){ 0 };
	sample(&up, 5000, 100000, &r);
	run_to(&up, 105000, &r);			/* 5 tau */
	CHECK(abs(r.last_bright - 75) <= 2 && r.ticks > 10);
	run_to(&up, 200000, &r);
	CHECK(abs(als_curve_now(&up) - 75) <= 1 && als_next_deadline_ms(&up) == -1);

	/* brightening is further along than dimming after the same 2 s */
	fresh(&up);
	fresh(&down);
	r = (struct run){ 0 };
	sample(&up, 10, 0, &r);
	sample(&down, 5000, 0, &r);
	run_to(&up, 5000, &r);
	run_to(&down, 5000, &r);
	sample(&up, 5000, 5000, &r);
	sample(&down, 10, 5000, &r);
	als_tick(&up, 7000, &ev);
	als_tick(&down, 7000, &ev);
	pu = (up.xf - x0) / (x1 - x0);
	pd = (x1 - down.xf) / (x1 - x0);
	CHECK(pu > pd);
	CHECK(pu > 0.8 && pd < 0.45);
}

static void test_restart(void)
{
	struct als a;
	struct run r = { 0 };

	fresh(&a);
	steady(&a, 5000, 0, 3000, &r);
	run_to(&a, 60000, &r);
	CHECK(als_curve_now(&a) == 75);
	als_stream_restart(&a);
	CHECK(als_next_deadline_ms(&a) == -1);
	/* D8: the filter is kept across a restart (a long screen-off does not forget the light) */
	als_warmup_start(&a, 100000);
	sample(&a, 0, 100500, &r);
	CHECK(als_curve_now(&a) == 75 && a.applied == 75);
	CHECK(als_curve_now(&(struct als){ 0 }) == -1);
	/* only the very first sample of all sets the filter */
	fresh(&a);
	r = (struct run){ 0 };
	sample(&a, 0, 0, &r);
	CHECK(als_curve_now(&a) == 5 && r.last_bright == 5);
}

static void test_deadband(void)
{
	struct als a;
	struct run r = { 0 };

	fresh(&a);
	sample(&a, 300, 0, &r);
	run_to(&a, 2000, &r);
	CHECK(r.bright_events == 1 && r.last_bright == 30);
	r = (struct run){ 0 };
	/* +-1 % jitter and 290..310 lux, sample after sample: no change */
	for (int i = 0; i < 300; i++)
		sample(&a, i & 1 ? 303 : 297, 2000 + i * STEP, &r);
	for (int i = 0; i < 300; i++)
		sample(&a, i & 1 ? 310 : 290, 62000 + i * STEP, &r);
	run_to(&a, 200000, &r);
	CHECK(r.bright_events == 0);
	/* a one-point offset is inside the band, two points are not */
	als_set_offset(&a, 1);
	sample(&a, 300, 300000, &r);
	run_to(&a, 400000, &r);
	CHECK(r.bright_events == 0);
	als_set_offset(&a, 2);
	CHECK(als_next_deadline_ms(&a) >= 0);	/* unsettled again: a tick is due */
	run_to(&a, 500000, &r);
	CHECK(r.bright_events == 1 && r.last_bright == 32);
}

static void test_floor_ceiling(void)
{
	struct als a;
	struct run r = { 0 };

	fresh(&a);
	sample(&a, 0, 0, &r);
	CHECK(r.last_bright == 5);
	als_set_applied(&a, 6);
	r = (struct run){ 0 };
	run_to(&a, 1000, &r);
	CHECK(r.bright_events == 1 && r.last_bright == UI_BRIGHTNESS_MIN);
	/* the ceiling is reached exactly through the ticks and then holds */
	als_set_applied(&a, 99);
	r = (struct run){ 0 };
	sample(&a, 1e6, 2000, &r);
	run_to(&a, 60000, &r);
	CHECK(r.last_bright == UI_BRIGHTNESS_MAX && a.applied == UI_BRIGHTNESS_MAX);
	r = (struct run){ 0 };
	sample(&a, 1e6, 60200, &r);
	run_to(&a, 100000, &r);
	CHECK(r.bright_events == 0);
	/* forgetting the applied level re-applies at the next tick */
	als_set_applied(&a, -1);
	CHECK(als_next_deadline_ms(&a) >= 0);
	run_to(&a, 111000, &r);
	CHECK(r.bright_events == 1 && r.last_bright == 100);
}

static void test_steady_state(void)
{
	struct als a;
	struct run r = { 0 };

	fresh(&a);
	CHECK(als_next_deadline_ms(&a) == -1);	/* nothing known */
	sample(&a, 300, 0, &r);
	run_to(&a, 100000, &r);
	CHECK(als_next_deadline_ms(&a) == -1);	/* converged, no hold running: no timer */
	sample(&a, 300, 150000, &r);
	CHECK(als_next_deadline_ms(&a) == -1);
	sample(&a, 3000, 160000, &r);		/* Light hold running */
	CHECK(als_next_deadline_ms(&a) >= 160000);
	run_to(&a, 500000, &r);
	CHECK(als_next_deadline_ms(&a) == -1 && a.light);
}

static void test_theme_holds(void)
{
	struct als a;
	struct run r = { 0 };

	/* one 2600 lux sample, then silence: Light at exactly sample time + 5 s */
	fresh(&a);
	sample(&a, 1500, 0, &r);
	CHECK(r.theme_events == 0);
	sample(&a, 2600, 1000, &r);
	run_to(&a, 5999, &r);
	CHECK(r.theme_events == 0 && !a.light);
	run_to(&a, 6000, &r);
	CHECK(r.theme_events == 1 && r.last_light && r.last_held == 5000);

	/* the same with repeated samples */
	fresh(&a);
	r = (struct run){ 0 };
	sample(&a, 1500, 0, &r);
	steady(&a, 2600, 1000, 6000, &r);
	CHECK(r.theme_events == 0);
	sample(&a, 2600, 6000, &r);
	CHECK(r.theme_events == 1 && r.last_light && r.last_held == 5000);

	/* a dip to 2400 restarts the hold */
	fresh(&a);
	r = (struct run){ 0 };
	sample(&a, 1500, 0, &r);
	steady(&a, 2600, 1000, 5000, &r);
	sample(&a, 2400, 5000, &r);
	sample(&a, 2600, 5200, &r);
	run_to(&a, 10199, &r);
	CHECK(r.theme_events == 0);
	run_to(&a, 10200, &r);
	CHECK(r.theme_events == 1 && r.last_light);

	/* 1500 steady never flips, in either state */
	fresh(&a);
	r = (struct run){ 0 };
	sample(&a, 1500, 0, &r);
	run_to(&a, 1000000, &r);
	CHECK(r.theme_events == 0 && !a.light);
	fresh(&a);
	r = (struct run){ 0 };
	sample(&a, 3000, 0, &r);		/* no wake grace here: the normal hold applies */
	run_to(&a, 4999, &r);
	CHECK(r.theme_events == 0);
	run_to(&a, 5000, &r);
	CHECK(r.theme_events == 1 && a.light && r.last_held == 5000);
	sample(&a, 1500, 6000, &r);
	run_to(&a, 1000000, &r);
	CHECK(r.theme_events == 1 && a.light);

	/* one 700 lux sample after Light, then silence: Dark at +20 s, not before */
	fresh(&a);
	a.light = true;
	r = (struct run){ 0 };
	sample(&a, 700, 1000, &r);
	run_to(&a, 20999, &r);
	CHECK(r.theme_events == 0);
	run_to(&a, 21000, &r);
	CHECK(r.theme_events == 1 && !r.last_light && r.last_held == 20000);

	/* a sample between the thresholds resets the dark hold */
	fresh(&a);
	a.light = true;
	r = (struct run){ 0 };
	sample(&a, 700, 1000, &r);
	sample(&a, 900, 15000, &r);
	sample(&a, 700, 15200, &r);
	run_to(&a, 35199, &r);
	CHECK(r.theme_events == 0);
	run_to(&a, 35200, &r);
	CHECK(r.theme_events == 1);
}

/* A sample at t during a wake grace, with the deadlines before it processed. */
static const double WARM_SAMPLES_DARK[] = { 0, 0, 5, 10, 3, 3, 3, 3, 3 };

/* D8: a wake in a dark room from Light: the readings start at 0, so the theme must
 * stay Light through wake grace and turn Dark 20 s after its end, once. */
static void test_warmup_dark_room(void)
{
	struct als a;
	struct run r = { 0 };

	fresh_warm(&a, 0);
	a.light = true;
	for (unsigned i = 0; i < sizeof(WARM_SAMPLES_DARK) / sizeof(WARM_SAMPLES_DARK[0]); i++)
		sample(&a, WARM_SAMPLES_DARK[i], 500 + 1000 * (int64_t)i, &r);
	run_to(&a, ALS_WAKE_GRACE_MS + 19999, &r);
	CHECK(r.theme_events == 0 && a.light);	/* Light through wake grace and 20 s after it, minus 1 ms */
	run_to(&a, ALS_WAKE_GRACE_MS + 20000, &r);
	CHECK(r.theme_events == 1 && !r.last_light && r.last_held == 20000 && !r.last_light);
	run_to(&a, 1000000, &r);
	CHECK(r.theme_events == 1);
}

/* Outdoors: the first readings are low, then bright. From Light it never goes Dark. */
static void test_warmup_outdoors(void)
{
	struct als a;
	struct run r = { 0 };
	static const double s[] = { 0, 50, 4000, 30000 };

	fresh_warm(&a, 0);
	a.light = true;
	for (unsigned i = 0; i < 4; i++)
		sample(&a, s[i], 200 * (int64_t)(i + 1), &r);
	run_to(&a, 1000000, &r);
	CHECK(r.theme_events == 0 && a.light);

	/* from Dark: Light as soon as the wake grace maximum reaches the threshold, not later */
	fresh_warm(&a, 0);
	r = (struct run){ 0 };
	sample(&a, 0, 200, &r);
	sample(&a, 50, 400, &r);
	CHECK(r.theme_events == 0 && !a.light);
	{
		struct als_event ev;

		run_to(&a, 500, &r);
		als_feed(&a, 4000, 600, &ev);
		CHECK(ev.theme_changed && ev.light && ev.warmup && ev.lux == 4000 && ev.held_ms == 0);
	}
	/* a later low sample inside the wake grace does not undo it; with the light still low at
	 * the end, the normal 20 s Dark hold then runs from the end of wake grace */
	sample(&a, 10, 900, &r);
	run_to(&a, ALS_WAKE_GRACE_MS + 19999, &r);
	CHECK(a.light);
	run_to(&a, ALS_WAKE_GRACE_MS + 20000, &r);
	CHECK(!a.light);
	/* while bright at the end it stays Light */
	fresh_warm(&a, 0);
	r = (struct run){ 0 };
	sample(&a, 4000, 200, &r);
	sample(&a, 5000, 900, &r);
	run_to(&a, 1000000, &r);
	CHECK(a.light && r.theme_events == 1);
}

/* Warm-up: the level never decreases, then it dims with the normal rules. */
static void test_warmup_brightness(void)
{
	struct als a;
	struct run r = { 0 };
	int prev;

	fresh(&a);
	steady(&a, 5000, 0, 3000, &r);
	run_to(&a, 60000, &r);
	CHECK(a.applied == 75);
	als_stream_restart(&a);
	als_warmup_start(&a, 100000);
	prev = a.applied;
	r = (struct run){ 0 };
	for (int i = 0; i < 4; i++) {		/* all inside the grace window */
		struct als_event ev;

		run_to(&a, 100000 + 200 + 1000 * i, &r);
		als_feed(&a, i < 2 ? 0 : 20, 100200 + 1000 * i, &ev);
		note(&r, &ev);
		CHECK(a.applied >= prev);
		prev = a.applied;
	}
	run_to(&a, 100000 + ALS_WAKE_GRACE_MS - 1, &r);
	CHECK(a.applied == 75 && r.bright_events == 0 && a.warm);
	/* after wake grace the 20 lux readings dim the level with the 5 s tau */
	run_to(&a, 100000 + ALS_WAKE_GRACE_MS + 1, &r);
	CHECK(!a.warm);
	run_to(&a, 200000, &r);
	CHECK(r.bright_events > 0 && a.applied < 30);
}

/* The end of wake grace fires by deadline without any further sample. */
static void test_warmup_deadline(void)
{
	struct als a;
	struct run r = { 0 };

	fresh_warm(&a, 1000);
	sample(&a, 300, 1500, &r);
	CHECK(a.warm);
	run_to(&a, 1000 + ALS_WAKE_GRACE_MS - 1, &r);
	CHECK(a.warm);
	CHECK(als_next_deadline_ms(&a) <= 1000 + ALS_WAKE_GRACE_MS);
	run_to(&a, 1000 + ALS_WAKE_GRACE_MS, &r);
	CHECK(!a.warm);
	run_to(&a, 100000, &r);
	CHECK(als_next_deadline_ms(&a) == -1);
	/* a stream with no sample yet has no deadline at all */
	fresh_warm(&a, 0);
	CHECK(als_next_deadline_ms(&a) == -1);
}

/* In-between light keeps the theme across a restart and wake grace. */
static void test_theme_restart(void)
{
	struct als a;
	struct run r = { 0 };

	fresh_warm(&a, 0);
	a.light = true;
	sample(&a, 1500, 300, &r);
	run_to(&a, 300000, &r);
	CHECK(a.light && r.theme_events == 0);
	als_stream_restart(&a);
	als_warmup_start(&a, 400000);
	sample(&a, 1500, 400300, &r);
	run_to(&a, 700000, &r);
	CHECK(a.light && r.theme_events == 0);

	/* Dark and in between stays Dark */
	fresh_warm(&a, 0);
	sample(&a, 1500, 300, &r);
	run_to(&a, 300000, &r);
	CHECK(!a.light && r.theme_events == 0);

	/* an Auto theme reset decides at once from the held light, else it is Dark */
	fresh(&a);
	sample(&a, 3000, 0, &r);
	als_theme_reset(&a);
	CHECK(a.light);
	sample(&a, 1500, 100, &r);
	als_theme_reset(&a);
	CHECK(!a.light);
	fresh(&a);
	a.light = true;
	als_theme_reset(&a);
	CHECK(!a.light);
}

static void test_theme_flapping(void)
{
	struct als a;
	struct run r = { 0 };

	/* 2000..3000 lux square wave, period 6 s, 10 min: never held 5 s, either start */
	fresh(&a);
	for (int64_t t = 0; t < 600000; t += STEP)
		sample(&a, ((t / 3000) & 1) ? 2000 : 3000, t, &r);
	CHECK(r.theme_events == 0);
	fresh(&a);
	r = (struct run){ 0 };
	for (int64_t t = 0; t < 600000; t += STEP)
		sample(&a, ((t / 3000) & 1) ? 3000 : 2000, t, &r);
	CHECK(r.theme_events == 0);
	/* sine through the midpoint, same bounds */
	fresh(&a);
	r = (struct run){ 0 };
	for (int64_t t = 0; t < 600000; t += STEP)
		sample(&a, 2500 + 500 * sin(2 * M_PI * (double)t / 6000.0 - 0.5), t, &r);
	CHECK(r.theme_events == 0);
	/* 60 s period, 30 s at 3000 and 30 s at 700, ten cycles: every half cycle
	 * flips once (the first after its 5 s hold) */
	fresh(&a);
	r = (struct run){ 0 };
	for (int64_t t = 0; t < 600000; t += STEP)
		sample(&a, ((t / 30000) & 1) ? 700 : 3000, t, &r);
	CHECK(r.theme_events == 20);
	/* the same on-change only: one sample per edge */
	fresh(&a);
	r = (struct run){ 0 };
	for (int64_t t = 0; t < 600000; t += 30000)
		sample(&a, ((t / 30000) & 1) ? 700 : 3000, t, &r);
	run_to(&a, 700000, &r);
	CHECK(r.theme_events == 20);
}

static void test_offset(void)
{
	struct als a;
	struct run r = { 0 };
	int o;

	fresh(&a);
	CHECK(!als_offset_for(&a, 40, &o) && o == 0);
	sample(&a, 300, 0, &r);
	run_to(&a, 20000, &r);
	CHECK(als_offset_for(&a, 40, &o) && o == 10);
	CHECK(als_offset_for(&a, 100, &o) && o == 70);	/* every level 5..100 is curve + offset */
	CHECK(als_offset_for(&a, 5, &o) && o == -25);
	sample(&a, 0, 30000, &r);
	run_to(&a, 120000, &r);
	CHECK(als_offset_for(&a, 100, &o) && o == 95);
	CHECK(als_offset_for(&a, 5, &o) && o == 0);
	for (int lvl = 5; lvl <= 100; lvl++) {
		CHECK(als_offset_for(&a, lvl, &o) && als_curve_now(&a) + o == lvl);
		CHECK(o >= UI_AUTO_OFFSET_MIN && o <= UI_AUTO_OFFSET_MAX);
	}
	/* and at the top of the curve */
	sample(&a, 1e6, 130000, &r);
	run_to(&a, 400000, &r);
	CHECK(als_offset_for(&a, 5, &o) && o == -95);
}

static void test_params(void)
{
	struct als_params p;

	als_params_default(&p);
	CHECK(als_params_valid(&p) && p.light_lux == 2500 && p.dark_lux == 800 &&
	      p.light_hold_s == 5 && p.dark_hold_s == 20);
	p.dark_lux = 2500;
	CHECK(!als_params_valid(&p));
	als_params_default(&p);
	p.light_hold_s = 0;
	CHECK(!als_params_valid(&p));
	p.light_hold_s = 601;
	CHECK(!als_params_valid(&p));
	p.light_hold_s = 600;
	p.dark_hold_s = 1;
	CHECK(als_params_valid(&p));
}

int main(void)
{
	test_curve();
	test_clamp();
	test_smoothing();
	test_restart();
	test_deadband();
	test_floor_ceiling();
	test_steady_state();
	test_theme_holds();
	test_warmup_dark_room();
	test_warmup_outdoors();
	test_warmup_brightness();
	test_warmup_deadline();
	test_theme_restart();
	test_theme_flapping();
	test_offset();
	test_params();
	return check_report("test-als");
}
