/* Host unit tests for the pure parts of tools/fbtouch.c: pixel packing
 * against the MDSS fb0 bitfield layout, the test pattern, the multitouch
 * slot decoder (MT protocol B as the NT36xxx driver emits it, plus the
 * single-touch fallback) and the ABS-to-screen mapping. No framebuffer,
 * no evdev -- fbtouch.c is included with FBTOUCH_NO_MAIN so main() and
 * the syscall-level code are compiled out.
 * Build/run: see tools/Makefile ("make test").
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define FBTOUCH_NO_MAIN
#include "../fbtouch.c"

static int g_failures;
static int g_tests;

#define CHECK(cond) do { \
	g_tests++; \
	if (!(cond)) { \
		g_failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

/* The layout mdss_fb_register() sets for MDP_RGBA_8888 (fb0 on chef). */
static struct fb_var_screeninfo rgba8888_var(void)
{
	struct fb_var_screeninfo v;

	memset(&v, 0, sizeof(v));
	v.bits_per_pixel = 32;
	v.red.offset = 0;    v.red.length = 8;
	v.green.offset = 8;  v.green.length = 8;
	v.blue.offset = 16;  v.blue.length = 8;
	v.transp.offset = 24; v.transp.length = 8;
	return v;
}

static struct fb_var_screeninfo rgb565_var(void)
{
	struct fb_var_screeninfo v;

	memset(&v, 0, sizeof(v));
	v.bits_per_pixel = 16;
	v.blue.offset = 0;   v.blue.length = 5;
	v.green.offset = 5;  v.green.length = 6;
	v.red.offset = 11;   v.red.length = 5;
	return v;
}

/* ---- pixels ---- */

static void test_pack_rgba8888(void)
{
	struct fb_var_screeninfo v = rgba8888_var();
	uint32_t px = pack_pixel(&v, 0x11, 0x22, 0x33);
	uint8_t bytes[4];

	/* red at bit 0 -> first byte in memory on little-endian */
	memcpy(bytes, &px, 4);
	CHECK(bytes[0] == 0x11);
	CHECK(bytes[1] == 0x22);
	CHECK(bytes[2] == 0x33);
	CHECK(bytes[3] == 0xff); /* opaque alpha */
	CHECK(pack_pixel(&v, 255, 255, 255) == 0xffffffffu);
	CHECK(pack_pixel(&v, 0, 0, 0) == 0xff000000u);
}

static void test_pack_rgb565(void)
{
	struct fb_var_screeninfo v = rgb565_var();

	CHECK(pack_pixel(&v, 255, 0, 0) == 0xf800);
	CHECK(pack_pixel(&v, 0, 255, 0) == 0x07e0);
	CHECK(pack_pixel(&v, 0, 0, 255) == 0x001f);
	CHECK(pack_pixel(&v, 255, 255, 255) == 0xffff);
	/* no alpha field: nothing above bit 15 */
	CHECK((pack_pixel(&v, 255, 255, 255) & 0xffff0000u) == 0);
}

static void test_put_pixel_bounds_and_stride(void)
{
	struct fb_var_screeninfo v = rgba8888_var();
	/* 4x3 surface with a padded stride (20 bytes instead of 16) */
	uint8_t buf[20 * 3 + 16];
	struct surface s = { .base = buf, .xres = 4, .yres = 3,
			     .line_length = 20, .bpp = 4, .var = &v };
	uint32_t px = pack_pixel(&v, 1, 2, 3);

	memset(buf, 0xaa, sizeof(buf));
	put_pixel(&s, 3, 2, px);
	CHECK(memcmp(buf + 2 * 20 + 3 * 4, &px, 4) == 0);
	/* the padding bytes and the sentinel after the surface are untouched */
	CHECK(buf[2 * 20 + 16] == 0xaa);
	CHECK(buf[20 * 3] == 0xaa);
	/* out of range writes are dropped, never wrap or overflow */
	put_pixel(&s, 4, 0, px);
	put_pixel(&s, 0, 3, px);
	put_pixel(&s, -1, 0, px);
	put_pixel(&s, 0, -1, px);
	CHECK(buf[20 * 3] == 0xaa && buf[20 * 3 + 15] == 0xaa);
	CHECK(buf[0] == 0xaa);
}

static void test_pattern_colours(void)
{
	uint8_t r, g, b;

	/* eight bars over the top 55 %: leftmost white, rightmost black */
	pattern_rgb(1080, 2246, 0, 0, &r, &g, &b);
	CHECK(r == 255 && g == 255 && b == 255);
	pattern_rgb(1080, 2246, 1079, 0, &r, &g, &b);
	CHECK(r == 0 && g == 0 && b == 0);
	/* bar 5 (x in [675,810)) is pure red, bar 6 pure blue */
	pattern_rgb(1080, 2246, 700, 100, &r, &g, &b);
	CHECK(r == 255 && g == 0 && b == 0);
	pattern_rgb(1080, 2246, 900, 100, &r, &g, &b);
	CHECK(r == 0 && g == 0 && b == 255);
	/* the grey ramp below: black on the left, white on the right */
	pattern_rgb(1080, 2246, 0, 2000, &r, &g, &b);
	CHECK(r == 0 && g == 0 && b == 0);
	pattern_rgb(1080, 2246, 1079, 2000, &r, &g, &b);
	CHECK(r == 255 && g == 255 && b == 255);
	pattern_rgb(1080, 2246, 540, 2000, &r, &g, &b);
	CHECK(r == g && g == b && r > 120 && r < 135);
}

static void test_draw_pattern_full_surface(void)
{
	struct fb_var_screeninfo v = rgba8888_var();
	enum { W = 64, H = 40 };
	static uint8_t buf[W * 4 * H + 4];
	struct surface s = { .base = buf, .xres = W, .yres = H,
			     .line_length = W * 4, .bpp = 4, .var = &v };
	uint32_t white = pack_pixel(&v, 255, 255, 255);
	uint32_t px;

	memset(buf, 0x55, sizeof(buf));
	draw_pattern(&s);
	/* border is white on every edge */
	memcpy(&px, buf, 4);
	CHECK(px == white);
	memcpy(&px, buf + ((H - 1) * W + W - 1) * 4, 4);
	CHECK(px == white);
	/* nothing written past the end */
	CHECK(buf[W * 4 * H] == 0x55);
	/* inside the border, bar 7 (rightmost) is black */
	memcpy(&px, buf + (10 * W + W - 8) * 4, 4);
	CHECK(px == pack_pixel(&v, 0, 0, 0));
}

static void test_fill_circle_clipped(void)
{
	struct fb_var_screeninfo v = rgba8888_var();
	enum { W = 16, H = 16 };
	static uint8_t buf[W * 4 * H + 4];
	struct surface s = { .base = buf, .xres = W, .yres = H,
			     .line_length = W * 4, .bpp = 4, .var = &v };
	uint32_t red = pack_pixel(&v, 255, 0, 0), px;

	memset(buf, 0, sizeof(buf));
	/* centre on the corner: three quarters of the disc fall outside */
	fill_circle(&s, 0, 0, 3, red);
	memcpy(&px, buf, 4);
	CHECK(px == red);
	memcpy(&px, buf + 3 * 4, 4);		/* (3,0) on the rim */
	CHECK(px == red);
	memcpy(&px, buf + 4 * 4, 4);		/* (4,0) outside */
	CHECK(px == 0);
	memcpy(&px, buf + (3 * W + 3) * 4, 4);	/* (3,3): 18 > 9, outside */
	CHECK(px == 0);
	CHECK(buf[W * 4 * H] == 0);		/* sentinel */
}

/* ---- multitouch decode ---- */

static struct input_event mkev(uint16_t type, uint16_t code, int32_t value)
{
	struct input_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.code = code;
	ev.value = value;
	return ev;
}

#define FEED(t, ev) mt_tracker_feed((t), &(ev), out)

static void test_mt_b_single_contact(void)
{
	struct mt_tracker t;
	struct mt_event out[MT_MAX_SLOTS];
	struct input_event e;
	int n;

	mt_tracker_init(&t, true);
	/* the NT36xxx driver's report order: slot, id, x, y, major, pressure, BTN_TOUCH, SYN */
	e = mkev(EV_ABS, ABS_MT_SLOT, 0);          CHECK(FEED(&t, e) == 0);
	e = mkev(EV_ABS, ABS_MT_TRACKING_ID, 0);   CHECK(FEED(&t, e) == 0);
	e = mkev(EV_ABS, ABS_MT_POSITION_X, 500);  CHECK(FEED(&t, e) == 0);
	e = mkev(EV_ABS, ABS_MT_POSITION_Y, 1200); CHECK(FEED(&t, e) == 0);
	e = mkev(EV_ABS, ABS_MT_TOUCH_MAJOR, 9);   CHECK(FEED(&t, e) == 0);
	e = mkev(EV_ABS, ABS_MT_PRESSURE, 30);     CHECK(FEED(&t, e) == 0);
	e = mkev(EV_KEY, BTN_TOUCH, 1);            CHECK(FEED(&t, e) == 0);
	e = mkev(EV_SYN, SYN_REPORT, 0);
	n = FEED(&t, e);
	CHECK(n == 1);
	CHECK(out[0].action == MT_DOWN && out[0].slot == 0 && out[0].id == 0);
	CHECK(out[0].x == 500 && out[0].y == 1200);

	/* move: only x changes */
	e = mkev(EV_ABS, ABS_MT_POSITION_X, 510);  FEED(&t, e);
	e = mkev(EV_SYN, SYN_REPORT, 0);
	n = FEED(&t, e);
	CHECK(n == 1);
	CHECK(out[0].action == MT_MOVE && out[0].x == 510 && out[0].y == 1200);

	/* a SYN_REPORT with nothing changed emits nothing */
	e = mkev(EV_SYN, SYN_REPORT, 0);
	CHECK(FEED(&t, e) == 0);

	/* lift */
	e = mkev(EV_ABS, ABS_MT_TRACKING_ID, -1);  FEED(&t, e);
	e = mkev(EV_KEY, BTN_TOUCH, 0);            FEED(&t, e);
	e = mkev(EV_SYN, SYN_REPORT, 0);
	n = FEED(&t, e);
	CHECK(n == 1);
	CHECK(out[0].action == MT_UP && out[0].slot == 0);

	/* a second contact in the same slot is a fresh DOWN */
	e = mkev(EV_ABS, ABS_MT_TRACKING_ID, 3);   FEED(&t, e);
	e = mkev(EV_ABS, ABS_MT_POSITION_X, 100);  FEED(&t, e);
	e = mkev(EV_SYN, SYN_REPORT, 0);
	n = FEED(&t, e);
	CHECK(n == 1 && out[0].action == MT_DOWN && out[0].id == 3 && out[0].x == 100);
}

static void test_mt_b_two_fingers(void)
{
	struct mt_tracker t;
	struct mt_event out[MT_MAX_SLOTS];
	struct input_event e;
	int n;

	mt_tracker_init(&t, true);
	e = mkev(EV_ABS, ABS_MT_SLOT, 0);          FEED(&t, e);
	e = mkev(EV_ABS, ABS_MT_TRACKING_ID, 0);   FEED(&t, e);
	e = mkev(EV_ABS, ABS_MT_POSITION_X, 10);   FEED(&t, e);
	e = mkev(EV_ABS, ABS_MT_POSITION_Y, 20);   FEED(&t, e);
	e = mkev(EV_ABS, ABS_MT_SLOT, 1);          FEED(&t, e);
	e = mkev(EV_ABS, ABS_MT_TRACKING_ID, 1);   FEED(&t, e);
	e = mkev(EV_ABS, ABS_MT_POSITION_X, 900);  FEED(&t, e);
	e = mkev(EV_ABS, ABS_MT_POSITION_Y, 2000); FEED(&t, e);
	e = mkev(EV_SYN, SYN_REPORT, 0);
	n = FEED(&t, e);
	CHECK(n == 2);
	CHECK(out[0].slot == 0 && out[0].action == MT_DOWN && out[0].x == 10 && out[0].y == 20);
	CHECK(out[1].slot == 1 && out[1].action == MT_DOWN && out[1].x == 900 && out[1].y == 2000);

	/* lift only slot 0; slot 1 unchanged and silent */
	e = mkev(EV_ABS, ABS_MT_SLOT, 0);          FEED(&t, e);
	e = mkev(EV_ABS, ABS_MT_TRACKING_ID, -1);  FEED(&t, e);
	e = mkev(EV_SYN, SYN_REPORT, 0);
	n = FEED(&t, e);
	CHECK(n == 1 && out[0].slot == 0 && out[0].action == MT_UP);

	/* slot 1 moves; the current slot is still 0 until ABS_MT_SLOT says otherwise */
	e = mkev(EV_ABS, ABS_MT_SLOT, 1);          FEED(&t, e);
	e = mkev(EV_ABS, ABS_MT_POSITION_Y, 1990); FEED(&t, e);
	e = mkev(EV_SYN, SYN_REPORT, 0);
	n = FEED(&t, e);
	CHECK(n == 1 && out[0].slot == 1 && out[0].action == MT_MOVE && out[0].y == 1990);
}

static void test_mt_b_ignores_noise_and_bad_slots(void)
{
	struct mt_tracker t;
	struct mt_event out[MT_MAX_SLOTS];
	struct input_event e;

	mt_tracker_init(&t, true);
	/* position updates on an empty slot are not contacts */
	e = mkev(EV_ABS, ABS_MT_POSITION_X, 5);    FEED(&t, e);
	e = mkev(EV_SYN, SYN_REPORT, 0);
	CHECK(FEED(&t, e) == 0);
	/* an out-of-range slot index is ignored, current slot stays 0 */
	e = mkev(EV_ABS, ABS_MT_SLOT, MT_MAX_SLOTS + 5); FEED(&t, e);
	CHECK(t.st.cur == 0);
	e = mkev(EV_ABS, ABS_MT_SLOT, -1);         FEED(&t, e);
	CHECK(t.st.cur == 0);
	/* on an MT device the single-touch ABS_X/BTN_TOUCH mirror is ignored */
	e = mkev(EV_ABS, ABS_X, 77);               FEED(&t, e);
	e = mkev(EV_KEY, BTN_TOUCH, 1);            FEED(&t, e);
	e = mkev(EV_SYN, SYN_REPORT, 0);
	CHECK(FEED(&t, e) == 0);
	/* other event types are ignored */
	e = mkev(EV_MSC, MSC_SCAN, 1);             CHECK(FEED(&t, e) == 0);
	e = mkev(EV_SYN, SYN_MT_REPORT, 0);        CHECK(FEED(&t, e) == 0);
}

static void test_single_touch_fallback(void)
{
	struct mt_tracker t;
	struct mt_event out[MT_MAX_SLOTS];
	struct input_event e;
	int n;

	mt_tracker_init(&t, false);
	e = mkev(EV_ABS, ABS_X, 300);              FEED(&t, e);
	e = mkev(EV_ABS, ABS_Y, 400);              FEED(&t, e);
	e = mkev(EV_KEY, BTN_TOUCH, 1);            FEED(&t, e);
	e = mkev(EV_SYN, SYN_REPORT, 0);
	n = FEED(&t, e);
	CHECK(n == 1 && out[0].action == MT_DOWN && out[0].slot == 0);
	CHECK(out[0].x == 300 && out[0].y == 400);
	e = mkev(EV_ABS, ABS_Y, 410);              FEED(&t, e);
	e = mkev(EV_SYN, SYN_REPORT, 0);
	n = FEED(&t, e);
	CHECK(n == 1 && out[0].action == MT_MOVE && out[0].y == 410);
	e = mkev(EV_KEY, BTN_TOUCH, 0);            FEED(&t, e);
	e = mkev(EV_SYN, SYN_REPORT, 0);
	n = FEED(&t, e);
	CHECK(n == 1 && out[0].action == MT_UP);
}

/* ---- coordinate mapping ---- */

static void test_abs_to_screen(void)
{
	CHECK(abs_to_screen(0, 0, 1080, 1080) == 0);
	CHECK(abs_to_screen(1080, 0, 1080, 1080) == 1079);
	CHECK(abs_to_screen(540, 0, 1080, 1080) == 539);
	/* a different raw range is rescaled, not passed through */
	CHECK(abs_to_screen(2160, 0, 2160, 1080) == 1079);
	CHECK(abs_to_screen(1080, 0, 2160, 1080) == 539);
	/* clamped at both ends */
	CHECK(abs_to_screen(-5, 0, 1080, 1080) == 0);
	CHECK(abs_to_screen(5000, 0, 1080, 1080) == 1079);
	/* degenerate range: value returned as-is */
	CHECK(abs_to_screen(42, 0, 0, 1080) == 42);
}

int main(void)
{
	test_pack_rgba8888();
	test_pack_rgb565();
	test_put_pixel_bounds_and_stride();
	test_pattern_colours();
	test_draw_pattern_full_surface();
	test_fill_circle_clipped();
	test_mt_b_single_contact();
	test_mt_b_two_fingers();
	test_mt_b_ignores_noise_and_bad_slots();
	test_single_touch_fallback();
	test_abs_to_screen();
	printf("test-fbtouch: %d/%d checks passed\n", g_tests - g_failures, g_tests);
	return g_failures ? 1 : 0;
}
