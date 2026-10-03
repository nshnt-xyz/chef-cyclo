/* Host tests for plan.c: previous + current areas, containment pruning,
 * the full-screen shortcut, overflow forcing a full copy, stale pages,
 * page alternation and pan offsets. */
#include <string.h>

#include "check.h"
#include "../plan.h"

#define W 100
#define H 200

static struct cu_rect R(int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
	return (struct cu_rect){ x1, y1, x2, y2 };
}

static bool req(const struct cu_rect *a, struct cu_rect b)
{
	return a->x1 == b.x1 && a->y1 == b.y1 && a->x2 == b.x2 && a->y2 == b.y2;
}

static bool out_has(const struct cu_plan *p, struct cu_rect r)
{
	int i;

	for (i = 0; i < p->nout; i++)
		if (req(&p->out[i], r))
			return true;
	return false;
}

/* Run one frame: add areas, finish, "pan", flip. Returns the page. */
static int frame(struct cu_plan *p, const struct cu_rect *a, int n)
{
	int i, page;

	for (i = 0; i < n; i++)
		cu_plan_add(p, &a[i]);
	page = cu_plan_finish(p);
	cu_plan_flipped(p);
	return page;
}

/* A fresh plan with both pages brought up to date (two full frames). */
static void warm(struct cu_plan *p)
{
	struct cu_rect a = R(0, 0, 9, 9);

	cu_plan_init(p, W, H, 2246);
	frame(p, &a, 1);
	frame(p, &a, 1);
}

static void test_pages_and_offsets(void)
{
	struct cu_plan p;
	struct cu_rect a = R(10, 10, 20, 20);

	cu_plan_init(&p, W, H, 2246);
	CHECK(p.back == 1);
	/* first two frames: each page is stale once -> full copies */
	CHECK(frame(&p, &a, 1) == 1 && p.out_full && p.nout == 1 && req(&p.out[0], R(0, 0, W - 1, H - 1)));
	CHECK(cu_plan_pan_yoffset(&p, 1) == 2246);
	CHECK(frame(&p, &a, 1) == 0 && p.out_full);
	CHECK(cu_plan_pan_yoffset(&p, 0) == 0);
	/* then partial, alternating 1, 0, 1, 0 */
	CHECK(frame(&p, &a, 1) == 1 && !p.out_full);
	CHECK(frame(&p, &a, 1) == 0 && !p.out_full);
	CHECK(frame(&p, &a, 1) == 1);

	/* a failed pan (no flip) writes the same page again */
	cu_plan_add(&p, &a);
	CHECK(cu_plan_finish(&p) == 0);
	cu_plan_add(&p, &a);
	CHECK(cu_plan_finish(&p) == 0);
}

static void test_prev_union_cur(void)
{
	struct cu_plan p;
	struct cu_rect f1 = R(0, 0, 9, 9), f2 = R(50, 50, 60, 60), f3 = R(70, 0, 80, 5);

	warm(&p);
	frame(&p, &f1, 1);
	frame(&p, &f2, 1);
	CHECK(p.nout == 2 && out_has(&p, f1) && out_has(&p, f2));	/* prev f1 + cur f2 */
	frame(&p, &f3, 1);
	CHECK(p.nout == 2 && out_has(&p, f2) && out_has(&p, f3) && !out_has(&p, f1));
	/* a frame with no areas still copies the previous frame's */
	frame(&p, NULL, 0);
	CHECK(p.nout == 1 && out_has(&p, f3));
	frame(&p, NULL, 0);
	CHECK(p.nout == 0 && !p.out_full);
}

static void test_containment(void)
{
	struct cu_plan p;
	struct cu_rect big = R(10, 10, 90, 90), small = R(20, 20, 30, 30), other = R(0, 150, 5, 160);
	struct cu_rect two[2];

	warm(&p);
	frame(&p, &big, 1);
	frame(&p, &small, 1);		/* prev big contains cur small */
	CHECK(p.nout == 1 && out_has(&p, big));
	frame(&p, &big, 1);		/* cur big contains prev small */
	CHECK(p.nout == 1 && out_has(&p, big));
	frame(&p, &big, 1);		/* equal areas: one copy */
	CHECK(p.nout == 1);
	two[0] = small;
	two[1] = other;
	frame(&p, two, 2);		/* big + small + other -> big, other */
	CHECK(p.nout == 2 && out_has(&p, big) && out_has(&p, other) && !out_has(&p, small));
	/* within one frame too */
	two[0] = small;
	two[1] = big;
	frame(&p, two, 2);
	CHECK(p.nout == 2 && out_has(&p, big) && out_has(&p, other));
}

static void test_full_screen_shortcut(void)
{
	struct cu_plan p;
	struct cu_rect full = R(0, 0, W - 1, H - 1), over = R(-3, -3, W + 5, H + 5),
		       a = R(1, 1, 2, 2), both[2];

	warm(&p);
	both[0] = a;
	both[1] = full;
	frame(&p, both, 2);
	CHECK(p.out_full && p.nout == 1 && req(&p.out[0], full));
	frame(&p, &a, 1);		/* the previous frame was full */
	CHECK(p.out_full);
	frame(&p, &a, 1);
	CHECK(!p.out_full && p.nout == 1 && out_has(&p, a));
	frame(&p, &over, 1);		/* larger than the screen counts too */
	CHECK(p.out_full && req(&p.out[0], full));
}

static void test_overflow(void)
{
	struct cu_plan p;
	struct cu_rect a[CU_PLAN_MAX_AREAS + 1], b = R(5, 5, 6, 6);
	int i;

	warm(&p);
	for (i = 0; i <= CU_PLAN_MAX_AREAS; i++)
		a[i] = R(20 + 2 * i, 100, 20 + 2 * i, 100);	/* disjoint from warm()'s area */
	frame(&p, a, CU_PLAN_MAX_AREAS);	/* exactly the limit: still a list */
	CHECK(!p.out_full && p.nout == CU_PLAN_MAX_AREAS + 1);	/* + the previous frame's */
	frame(&p, a, CU_PLAN_MAX_AREAS + 1);	/* one more: full */
	CHECK(p.out_full && p.nout == 1);
	frame(&p, &b, 1);			/* the lost list forces full once more */
	CHECK(p.out_full);
	frame(&p, &b, 1);
	CHECK(!p.out_full && p.nout == 1);
}

static void test_invalidate_pages(void)
{
	struct cu_plan p;
	struct cu_rect a = R(1, 1, 2, 2);

	warm(&p);
	frame(&p, &a, 1);
	cu_plan_invalidate_pages(&p);	/* screen back on: both pages unknown */
	CHECK(frame(&p, &a, 1) >= 0 && p.out_full);
	CHECK(frame(&p, &a, 1) >= 0 && p.out_full);
	frame(&p, &a, 1);
	CHECK(!p.out_full);
}

static void test_empty_area_ignored(void)
{
	struct cu_plan p;
	struct cu_rect bad = R(5, 5, 4, 4);

	warm(&p);
	frame(&p, &bad, 1);
	frame(&p, NULL, 0);
	CHECK(p.nout == 0 && !p.out_full);
}

/*
 * Pixel model: an 8x1 screen, a shadow and two pages. Each frame dirties
 * some pixels, copies the plan's areas from the shadow into the back page
 * and pans; after every successful pan the shown page must equal the
 * shadow. pan_ok[i] == false injects a failed pan at frame i (the
 * reviewer's case: frames 1-2 full, 3 = px0, 4 = px1 failing, 5 = px2,
 * 6 = px3).
 */
static void model_run(const int *dirty, const bool *pan_ok, int nframes, int *bad_frames)
{
	struct cu_plan p;
	int shadow[8] = { 0 }, page[2][8], shown = 0, f, i, x;

	memset(page, 0xff, sizeof(page));	/* both pages hold garbage */
	cu_plan_init(&p, 8, 1, 1);
	*bad_frames = 0;
	for (f = 0; f < nframes; f++) {
		int pg;
		struct cu_rect a;

		if (dirty[f] < 0) {
			for (x = 0; x < 8; x++)
				shadow[x] = 100 * (f + 1) + x;
			a = R(0, 0, 7, 0);
		} else {
			shadow[dirty[f]] = 100 * (f + 1);
			a = R(dirty[f], 0, dirty[f], 0);
		}
		cu_plan_add(&p, &a);
		pg = cu_plan_finish(&p);
		for (i = 0; i < p.nout; i++)
			for (x = p.out[i].x1; x <= p.out[i].x2; x++)
				page[pg][x] = shadow[x];
		if (pan_ok[f]) {
			shown = pg;
			cu_plan_flipped(&p);
			if (memcmp(page[shown], shadow, sizeof(shadow)) != 0)
				(*bad_frames)++;
		} else {
			cu_plan_pan_failed(&p);
		}
	}
}

static void test_failed_pan_model(void)
{
	int dirty[] = { -1, -1, 0, 1, 2, 3, 4, 5, 1, 6 }, bad;
	bool ok[10] = { true, true, true, false, true, true, true, true, true, true };
	bool all[10] = { true, true, true, true, true, true, true, true, true, true };

	model_run(dirty, all, 10, &bad);
	CHECK(bad == 0);		/* the plan keeps both pages exact */
	model_run(dirty, ok, 10, &bad);
	CHECK(bad == 0);		/* and recovers from a failed pan */
	ok[4] = false;			/* two failures in a row */
	ok[8] = false;
	model_run(dirty, ok, 10, &bad);
	CHECK(bad == 0);
}

int main(void)
{
	test_failed_pan_model();
	test_pages_and_offsets();
	test_prev_union_cur();
	test_containment();
	test_full_screen_shortcut();
	test_overflow();
	test_invalidate_pages();
	test_empty_area_ignored();
	return check_report("test-plan");
}
