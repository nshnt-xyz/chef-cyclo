/* Host unit tests for tools/fblog/fblog.c: /dev/kmsg record parsing, the
 * show/hide policy, the line ring, the wrapping layout, glyph rendering
 * into a fake RGBA8888 surface, the font table and the header; and the
 * daemon's borrow/resume path with the screen-off flag (tick(),
 * idle_until_flag_gone()) against a temporary "fb" file, lock and flag,
 * with ioctl() wrapped (-Wl,--wrap=ioctl) to play the MDSS fb ioctls and
 * record their order. No real framebuffer or kmsg -- fblog.c is included
 * with FBLOG_NO_MAIN so only main() is compiled out.
 * Build/run: see tools/Makefile ("make test").
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define FBLOG_NO_MAIN
#include "../fblog/fblog.c"

/* Integrated flag protocol: real buttond defaults drive the fblog idle loop. */
#define BUTTOND_NO_MAIN
#define kmsg_note buttond_kmsg_note
#define g_stop buttond_g_stop
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "../buttond.c"
#pragma GCC diagnostic pop
#undef kmsg_note
#undef g_stop


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
static struct fb_var_screeninfo rgba8888_var(uint32_t xres, uint32_t yres)
{
	struct fb_var_screeninfo v;

	memset(&v, 0, sizeof(v));
	v.xres = xres;
	v.yres = yres;
	v.bits_per_pixel = 32;
	v.red.offset = 0;    v.red.length = 8;
	v.green.offset = 8;  v.green.length = 8;
	v.blue.offset = 16;  v.blue.length = 8;
	v.transp.offset = 24; v.transp.length = 8;
	return v;
}

struct fake {
	struct fb_var_screeninfo var;
	uint8_t *mem;
	struct surface s;
};

/* A surface with a stride wider than the visible width, like fb0's 4352
 * bytes for 1080 px, so stride and width are not confused. */
static void fake_init(struct fake *f, uint32_t xres, uint32_t yres)
{
	f->var = rgba8888_var(xres, yres);
	f->s.line_length = xres * 4 + 64;
	f->mem = calloc((size_t)f->s.line_length * yres, 1);
	f->s.base = f->mem;
	f->s.xres = xres;
	f->s.yres = yres;
	f->s.bpp = 4;
	f->s.var = &f->var;
}

static uint32_t fake_px(const struct fake *f, int x, int y)
{
	uint32_t px;

	memcpy(&px, f->mem + (size_t)y * f->s.line_length + (size_t)x * 4, 4);
	return px;
}

/* ---- kmsg parsing ---- */

static void test_kmsg_parse(void)
{
	struct kmsg_rec r;
	const char *rec = "12,345,6789012,-;gps-up: modem up, qmux bridge on /run/qmux_socket\n";
	const char *kern = "3,12,44113000,c;pil-q6v5-mss 4080000.qcom,mss: modem: ERR_FATAL\n SUBSYSTEM=pil\n DEVICE=+platform:4080000.qcom,mss\n";
	const char *esc = "12,1,2000,-;tab\\x09here \\xc3\\xa9 end\n";
	const char *trunc = "12,1,2000,-;no newline at all";

	memset(&r, 0xaa, sizeof(r));
	CHECK(kmsg_parse(rec, strlen(rec), &r) == 0);
	CHECK(r.facility == 1 && r.level == 4);	/* 12 = LOG_USER<<3 | KERN_WARNING */
	CHECK(r.seq == 345);
	CHECK(r.ts_us == 6789012);
	CHECK(strcmp(r.msg, "gps-up: modem up, qmux bridge on /run/qmux_socket") == 0);

	CHECK(kmsg_parse(kern, strlen(kern), &r) == 0);
	CHECK(r.facility == 0 && r.level == 3);
	CHECK(r.ts_us == 44113000);
	CHECK(strcmp(r.msg, "pil-q6v5-mss 4080000.qcom,mss: modem: ERR_FATAL") == 0); /* continuation dropped */

	CHECK(kmsg_parse(esc, strlen(esc), &r) == 0);
	CHECK(strcmp(r.msg, "tab here \xc3\xa9 end") == 0);	/* \x09 -> tab -> space, \xc3\xa9 restored */

	CHECK(kmsg_parse(trunc, strlen(trunc), &r) == 0);
	CHECK(strcmp(r.msg, "no newline at all") == 0);

	CHECK(kmsg_parse("garbage without semicolon\n", 26, &r) == -1);
	CHECK(kmsg_parse("x,1,2,-;m\n", 10, &r) == -1);	/* prio not a number */
	CHECK(kmsg_parse("12;m\n", 5, &r) == -1);		/* missing seq/ts */
	CHECK(kmsg_parse("12,1;m\n", 7, &r) == -1);
	CHECK(kmsg_parse("12,1,2;m\n", 9, &r) == 0);		/* no flags field: tolerated */

	/* a message longer than the kept width is truncated, not overrun */
	{
		char big[2048];
		size_t n = (size_t)snprintf(big, sizeof(big), "12,1,2,-;");
		memset(big + n, 'x', sizeof(big) - n - 2);
		big[sizeof(big) - 2] = '\n';
		big[sizeof(big) - 1] = '\0';
		CHECK(kmsg_parse(big, strlen(big), &r) == 0);
		CHECK(strlen(r.msg) == FBLOG_MAX_LINE - 1);
	}
}

/* ---- policy ---- */

static void test_policy(void)
{
	struct kmsg_rec u = { .facility = 1, .level = 6 };
	struct kmsg_rec k6 = { .facility = 0, .level = 6 };
	struct kmsg_rec k3 = { .facility = 0, .level = 3 };
	struct kmsg_rec k0 = { .facility = 0, .level = 0 };

	CHECK(fblog_wants(&u, 3, false));		/* userspace always */
	CHECK(!fblog_wants(&k6, 3, false));	/* KERN_INFO hidden by default */
	CHECK(fblog_wants(&k3, 3, false));		/* KERN_ERR shown */
	CHECK(fblog_wants(&k0, 3, false));
	CHECK(fblog_wants(&k6, 7, false));		/* -k 7 */
	CHECK(!fblog_wants(&k3, 2, false));	/* -k 2 hides plain errors */
	CHECK(fblog_wants(&k6, 3, true));		/* -a */

	CHECK(fblog_kind(&u) == LINE_USER);
	CHECK(fblog_kind(&k6) == LINE_KERN);
	CHECK(fblog_kind(&k3) == LINE_KERN_ERR);
	CHECK(fblog_kind(&k0) == LINE_KERN_ERR);
}

/* ---- ring ---- */

static void test_ring(void)
{
	static struct ring r;
	char text[300];
	unsigned i;
	struct kmsg_rec rec = { .facility = 1, .level = 5, .ts_us = 46712345, .msg = "gps-up: modem up" };

	ring_init(&r);
	CHECK(r.count == 0);
	ring_push(&r, LINE_USER, "first");
	ring_push(&r, LINE_KERN, "second");
	CHECK(r.count == 2);
	CHECK(strcmp(ring_get(&r, 0)->text, "first") == 0);
	CHECK(strcmp(ring_get(&r, 1)->text, "second") == 0);
	CHECK(ring_get(&r, 0)->kind == LINE_USER && ring_get(&r, 1)->kind == LINE_KERN);
	CHECK(ring_get(&r, 1)->len == 6);

	/* overflow: the oldest lines fall off, order is preserved */
	for (i = 0; i < FBLOG_RING_LINES + 5; i++) {
		snprintf(text, sizeof(text), "line %u", i);
		ring_push(&r, LINE_USER, text);
	}
	CHECK(r.count == FBLOG_RING_LINES);
	CHECK(strcmp(ring_get(&r, 0)->text, "line 5") == 0);	/* 519 pushed, 512 kept: "first","second","line 0".."line 4" fell off */
	snprintf(text, sizeof(text), "line %u", FBLOG_RING_LINES + 4);
	CHECK(strcmp(ring_get(&r, FBLOG_RING_LINES - 1)->text, text) == 0);

	/* an over-long line is truncated to the kept width */
	memset(text, 'y', sizeof(text) - 1);
	text[sizeof(text) - 1] = '\0';
	ring_push(&r, LINE_USER, text);
	CHECK(ring_get(&r, FBLOG_RING_LINES - 1)->len == FBLOG_MAX_LINE - 1);

	format_line(text, sizeof(text), &rec);
	CHECK(strcmp(text, "   46.712 gps-up: modem up") == 0);
	rec.ts_us = 5;
	format_line(text, sizeof(text), &rec);
	CHECK(strcmp(text, "    0.000 gps-up: modem up") == 0);
}

/* ---- layout ---- */

static void test_layout(void)
{
	static struct ring r;
	struct row_ref refs[64];
	struct line l;
	char seg[80];
	unsigned n;

	CHECK(line_segments(0, 10) == 1);
	CHECK(line_segments(10, 10) == 1);
	CHECK(line_segments(11, 10) == 2);	/* 10 + 1 continuation of up to 8 */
	CHECK(line_segments(18, 10) == 2);
	CHECK(line_segments(19, 10) == 3);
	CHECK(line_segments(50, 2) == 1);	/* degenerate width: never divide by zero */

	memset(&l, 0, sizeof(l));
	strcpy(l.text, "0123456789abcdefghijk");	/* 21 chars */
	l.len = 21;
	CHECK(segment_text(&l, 0, 10, seg, sizeof(seg)) == 10 && strcmp(seg, "0123456789") == 0);
	CHECK(segment_text(&l, 1, 10, seg, sizeof(seg)) == 10 && strcmp(seg, "  abcdefgh") == 0);
	CHECK(segment_text(&l, 2, 10, seg, sizeof(seg)) == 5 && strcmp(seg, "  ijk") == 0);
	CHECK(segment_text(&l, 3, 10, seg, sizeof(seg)) == 2 && strcmp(seg, "  ") == 0); /* past the end: indent only */
	CHECK(segment_text(&l, 0, 10, seg, 4) == 3 && strcmp(seg, "012") == 0);	/* cap honoured */

	ring_init(&r);
	CHECK(layout_tail(&r, 10, 5, refs) == 0);	/* empty ring */
	ring_push(&r, LINE_USER, "a");			/* 1 seg */
	ring_push(&r, LINE_USER, "0123456789abcdefghijk");	/* 3 segs */
	ring_push(&r, LINE_USER, "z");			/* 1 seg */
	CHECK(layout_tail(&r, 10, 0, refs) == 0);

	/* everything fits */
	n = layout_tail(&r, 10, 8, refs);
	CHECK(n == 5);
	CHECK(refs[0].line == 0 && refs[0].seg == 0);
	CHECK(refs[1].line == 1 && refs[1].seg == 0);
	CHECK(refs[2].line == 1 && refs[2].seg == 1);
	CHECK(refs[3].line == 1 && refs[3].seg == 2);
	CHECK(refs[4].line == 2 && refs[4].seg == 0);

	/* exactly fits */
	CHECK(layout_tail(&r, 10, 5, refs) == 5 && refs[0].line == 0);

	/* too small: the newest rows win, the oldest line loses its head */
	n = layout_tail(&r, 10, 3, refs);
	CHECK(n == 3);
	CHECK(refs[0].line == 1 && refs[0].seg == 1);
	CHECK(refs[1].line == 1 && refs[1].seg == 2);
	CHECK(refs[2].line == 2 && refs[2].seg == 0);

	n = layout_tail(&r, 10, 1, refs);
	CHECK(n == 1 && refs[0].line == 2 && refs[0].seg == 0);

	/* wide enough that nothing wraps */
	n = layout_tail(&r, 80, 8, refs);
	CHECK(n == 3 && refs[1].line == 1 && refs[1].seg == 0 && refs[2].line == 2);
}

/* ---- geometry ---- */

static void test_geometry(void)
{
	struct geom g;

	geom_compute(&g, 1080, 2246, 2);	/* chef's fb0 at the default scale */
	CHECK(g.cell_w == 18 && g.cell_h == 32);
	CHECK(g.margin == 4);
	CHECK(g.header_h == 40);
	CHECK(g.cols == (1080 - 8) / 18);	/* 59 */
	CHECK(g.text_y0 == 44);
	CHECK(g.rows == (2246 - 44 - 4) / 32);	/* 68 */
	CHECK(g.text_y0 + g.rows * g.cell_h <= 2246);

	geom_compute(&g, 1080, 2246, 3);
	CHECK(g.cell_w == 27 && g.cols == (1080 - 12) / 27);

	geom_compute(&g, 1080, 2246, 0);	/* clamps to 1 */
	CHECK(g.scale == 1 && g.cell_w == 9);

	geom_compute(&g, 20, 20, 2);		/* tiny: no rows, no crash */
	CHECK(g.cols == 0 && g.rows == 0);
}

/* ---- font ---- */

static void test_font(void)
{
	unsigned i, r, nonblank = 0;

	CHECK(FBLOG_FONT_W == 9 && FBLOG_FONT_H == 15 && FBLOG_FONT_GLYPHS == 96);
	for (i = 0; i < FBLOG_FONT_GLYPHS; i++) {
		unsigned set = 0;
		for (r = 0; r < FBLOG_FONT_H; r++) {
			CHECK((fblog_font[i][r] & ~((1u << FBLOG_FONT_W) - 1)) == 0);	/* fits 9 bits */
			if (fblog_font[i][r])
				set++;
		}
		if (set)
			nonblank++;
	}
	CHECK(nonblank == FBLOG_FONT_GLYPHS - 1);	/* only the space is blank */
	for (r = 0; r < FBLOG_FONT_H; r++)
		CHECK(fblog_font[0][r] == 0);
	/* 'A' has its crossbar row fully set between the stems */
	CHECK(fblog_font['A' - 0x20][8] == 0x00fe);
	/* the box glyph stands in for anything outside printable ASCII */
	CHECK(glyph_for('A') == fblog_font['A' - 0x20]);
	CHECK(glyph_for(' ') == fblog_font[0]);
	CHECK(glyph_for('~') == fblog_font['~' - 0x20]);
	CHECK(glyph_for(0x7f) == fblog_font[95]);
	CHECK(glyph_for(0x01) == fblog_font[95]);
	CHECK(glyph_for(0xc3) == fblog_font[95]);
	CHECK(fblog_font[95][1] != 0 && fblog_font[95][7] != 0);	/* the box has edges */
}

/* ---- rendering ---- */

static void test_draw_text(void)
{
	struct fake f;
	uint32_t white, black;
	unsigned r, c;

	fake_init(&f, 64, 40);
	white = pack_pixel(&f.var, 255, 255, 255);
	black = 0;

	/* scale 1: 'A' at (3,2) reproduces the glyph bitmap exactly */
	draw_text(&f.s, 3, 2, 1, white, "A", 10);
	for (r = 0; r < FBLOG_FONT_H; r++)
		for (c = 0; c < FBLOG_FONT_W; c++) {
			int on = (fblog_font['A' - 0x20][r] >> (FBLOG_FONT_W - 1 - c)) & 1;
			CHECK(fake_px(&f, 3 + (int)c, 2 + (int)r) == (on ? white : black));
		}
	CHECK(fake_px(&f, 2, 10) == black);	/* nothing left of x */
	CHECK(fake_px(&f, 3 + 9, 10) == black);	/* nothing in the next cell */

	/* scale 2: each font pixel is a 2x2 block; second glyph is 18 px over */
	memset(f.mem, 0, (size_t)f.s.line_length * f.s.yres);
	draw_text(&f.s, 0, 0, 2, white, "AA", 10);
	CHECK(fake_px(&f, 2 * 1, 2 * 8) == white && fake_px(&f, 2 * 1 + 1, 2 * 8 + 1) == white); /* crossbar */
	CHECK(fake_px(&f, 18 + 2 * 1, 2 * 8) == white);
	CHECK(fake_px(&f, 2 * 0, 2 * 8) == black);	/* column 0 of 'A' is empty */

	/* maxchars limits, and the terminator stops earlier */
	memset(f.mem, 0, (size_t)f.s.line_length * f.s.yres);
	draw_text(&f.s, 0, 0, 1, white, "AAAA", 2);
	CHECK(fake_px(&f, 9 + 1, 8) == white);
	CHECK(fake_px(&f, 18 + 1, 8) == black);

	/* off the right/bottom edge is clipped, not written past the buffer */
	{
		struct fake g;
		fake_init(&g, 10, 10);
		draw_text(&g.s, 5, 5, 3, white, "AAAAAAAAAAAAAAAAAAAA", 100);
		CHECK(fake_px(&g, 9, 9) == black || fake_px(&g, 9, 9) == white); /* in bounds either way */
		free(g.mem);
	}
	free(f.mem);
}

static void test_surface_clear_and_render(void)
{
	struct fake f;
	struct geom g;
	struct palette pal;
	static struct ring r;
	uint32_t bg, hbg;
	unsigned i, lit = 0, x, y;

	fake_init(&f, 200, 120);
	geom_compute(&g, 200, 120, 1);
	palette_init(&pal, &f.var);
	bg = pack_pixel(&f.var, 0, 0, 0);
	hbg = pack_pixel(&f.var, 0, 56, 112);
	CHECK(bg == 0xff000000u);	/* RGBA8888: opaque black, not memset(0) */

	surface_clear(&f.s, 0xdeadbeefu);
	CHECK(fake_px(&f, 0, 0) == 0xdeadbeefu && fake_px(&f, 199, 119) == 0xdeadbeefu);
	/* the stride padding is untouched (still calloc'd 0) */
	CHECK(f.mem[(size_t)3 * f.s.line_length + 200 * 4] == 0);

	ring_init(&r);
	for (i = 0; i < 5; i++)
		ring_push(&r, i == 2 ? LINE_KERN_ERR : LINE_USER, "hello");
	render(&f.s, &g, &pal, &r, "hdr");
	CHECK(fake_px(&f, 0, 0) == hbg);				/* header band */
	CHECK(fake_px(&f, 199, (int)g.header_h - 1) == hbg);
	CHECK(fake_px(&f, 199, (int)g.header_h) == bg);		/* text area background */
	/* 'h' of "hdr" drawn at the margin in header colour: some pixel lit */
	for (y = g.margin; y < g.margin + FBLOG_FONT_H; y++)
		for (x = g.margin; x < g.margin + FBLOG_FONT_W; x++)
			if (fake_px(&f, (int)x, (int)y) == pal.header_fg)
				lit++;
	CHECK(lit > 10);
	/* row 2 (the KERN_ERR line) uses its own colour, row 0 the user colour */
	lit = 0;
	for (y = 0; y < FBLOG_FONT_H; y++)
		for (x = 0; x < FBLOG_FONT_W; x++) {
			uint32_t p0 = fake_px(&f, (int)(g.margin + x), (int)(g.text_y0 + y));
			uint32_t p2 = fake_px(&f, (int)(g.margin + x), (int)(g.text_y0 + 2 * g.cell_h + y));
			CHECK(p0 == bg || p0 == pal.kind[LINE_USER]);
			CHECK(p2 == bg || p2 == pal.kind[LINE_KERN_ERR]);
			if (p2 == pal.kind[LINE_KERN_ERR])
				lit++;
		}
	CHECK(lit > 10);
	free(f.mem);
}

/* ---- header ---- */

static void test_header(void)
{
	char buf[128];

	format_header(buf, sizeof(buf), "4.4.192", 123, 94, 0);
	CHECK(strcmp(buf, "chef-cyclo 4.4.192 up 123s bat 94%") == 0);
	format_header(buf, sizeof(buf), "4.4.192", 5, -1, 0);	/* no battery node */
	CHECK(strcmp(buf, "chef-cyclo 4.4.192 up 5s") == 0);
	format_header(buf, sizeof(buf), "4.4.192", 5, 50, 7);
	CHECK(strcmp(buf, "chef-cyclo 4.4.192 up 5s bat 50% lost 7") == 0);
	CHECK(format_header(buf, 12, "4.4.192", 5, 50, 7) == 11);	/* truncated, terminated */
}

/* ---- screen lock handshake (fbdev.h) ----
 * The real thing, on a temp file: the background client's shared try
 * succeeds when nobody owns the screen, fails with EWOULDBLOCK while a
 * foreground client (a forked child) holds the exclusive lock, and
 * succeeds again once that child exits -- which is how a killed fbtouch
 * hands the screen back too. Two pipes: ready (child -> parent) and
 * release (parent -> child), so neither side can consume its own byte. */
static void test_lock_handshake(void)
{
	char path[] = "/tmp/fblog-lock-test-XXXXXX";
	int tmp = mkstemp(path), bg, fg, status, ready[2], release[2];
	pid_t child;
	char c = 0;

	CHECK(tmp >= 0);
	close(tmp);
	bg = fb_lock_open(path);
	CHECK(bg >= 0);
	CHECK(fb_lock_try_shared(bg) == 0);
	fb_lock_release(bg);

	/* a foreground client cannot take exclusive while we hold shared */
	CHECK(fb_lock_try_shared(bg) == 0);
	fg = fb_lock_open(path);
	CHECK(fg >= 0);
	CHECK(fb_lock_exclusive(fg, 60) == -ETIMEDOUT);
	fb_lock_release(bg);
	CHECK(fb_lock_exclusive(fg, 60) == 0);	/* immediate once released */
	CHECK(fb_lock_try_shared(bg) == -EWOULDBLOCK);
	fb_lock_release(fg);
	close(fg);
	CHECK(fb_lock_try_shared(bg) == 0);
	fb_lock_release(bg);

	/* a separate process (fbtouch) owning it, then dying without unlock */
	CHECK(pipe(ready) == 0 && pipe(release) == 0);
	child = fork();
	CHECK(child >= 0);
	if (child == 0) {
		int f = fb_lock_open(path);
		int rc = f >= 0 ? fb_lock_exclusive(f, 1000) : -1;

		close(ready[0]);
		close(release[1]);
		(void)!write(ready[1], rc == 0 ? "y" : "n", 1);
		close(ready[1]);
		(void)!read(release[0], &c, 1);	/* wait for the parent's go */
		_exit(0);			/* no unlock: the kernel drops it */
	}
	close(ready[1]);
	close(release[0]);
	CHECK(read(ready[0], &c, 1) == 1 && c == 'y');
	CHECK(fb_lock_try_shared(bg) == -EWOULDBLOCK);
	CHECK(fb_lock_exclusive(bg, 40) == -ETIMEDOUT);
	(void)!write(release[1], "g", 1);
	close(release[1]);
	CHECK(waitpid(child, &status, 0) == child);
	close(ready[0]);
	CHECK(fb_lock_try_shared(bg) == 0);
	fb_lock_release(bg);
	close(bg);
	unlink(path);
}

/* ---- retry/backoff timing ---- */

static void test_timing(void)
{
	CHECK(retry_delay_ms(0) == 0);
	CHECK(retry_delay_ms(1) == 1000);
	CHECK(retry_delay_ms(2) == 2000);
	CHECK(retry_delay_ms(3) == 4000);
	CHECK(retry_delay_ms(4) == 8000);
	CHECK(retry_delay_ms(5) == 10000);
	CHECK(retry_delay_ms(40) == 10000);	/* no shift overflow, capped */

	/* idle: heartbeat cadence */
	CHECK(next_timeout_ms(false, 0, false, 0) == 1000);
	CHECK(next_timeout_ms(false, 400, false, 0) == 600);
	CHECK(next_timeout_ms(false, 1500, false, 0) == 0);	/* overdue: now */
	/* lines pending: repaint cadence */
	CHECK(next_timeout_ms(true, 0, false, 0) == 100);
	CHECK(next_timeout_ms(true, 30, false, 0) == 70);
	CHECK(next_timeout_ms(true, 100, false, 0) == 0);
	/* paused: never sleep past the lock retry interval */
	CHECK(next_timeout_ms(false, 0, true, 0) == FB_LOCK_RETRY_MS);
	CHECK(next_timeout_ms(true, 0, true, 0) == 100);
	/* backing off: the retry deadline wins, even with lines pending */
	CHECK(next_timeout_ms(true, 5000, false, 4000) == 4000);
	CHECK(next_timeout_ms(false, 0, true, 7000) == 7000);
	CHECK(next_timeout_ms(false, 0, false, 1) == 1);	/* never 0 while a retry is pending */
}

/* ---- daemon: borrow, resume, screen-off flag (wrapped ioctl) ---- */

#define FAKE_W 64
#define FAKE_H 40
#define FAKE_STRIDE (FAKE_W * 4 + 64)

struct ioctl_rec {
	unsigned long req;
	long arg;		/* FBIOBLANK level, FBIOPAN yoffset */
	bool flag;		/* the screen-off flag existed at the call */
};

static struct ioctl_rec g_ioc[64];
static int g_nioc;
static const char *g_flag_path;	/* for the flag column of the log */

int __real_ioctl(int fd, unsigned long req, ...);

int __wrap_ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	void *arg;

	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);
	switch (req) {
	case FBIOGET_VSCREENINFO: case FBIOGET_FSCREENINFO: case FBIOBLANK:
	case FBIOPAN_DISPLAY: case FBIOPUT_VSCREENINFO:
		break;
	default:
		return __real_ioctl(fd, req, arg);
	}
	if (g_nioc < (int)(sizeof(g_ioc) / sizeof(g_ioc[0]))) {
		g_ioc[g_nioc].req = req;
		g_ioc[g_nioc].arg = req == FBIOBLANK ? (long)arg :
				    req == FBIOPAN_DISPLAY ? (long)((struct fb_var_screeninfo *)arg)->yoffset : 0;
		g_ioc[g_nioc].flag = g_flag_path && access(g_flag_path, F_OK) == 0;
		g_nioc++;
	}
	if (req == FBIOGET_VSCREENINFO) {
		struct fb_var_screeninfo v = rgba8888_var(FAKE_W, FAKE_H);

		v.xres_virtual = FAKE_W;
		v.yres_virtual = 2 * FAKE_H;
		memcpy(arg, &v, sizeof(v));
	} else if (req == FBIOGET_FSCREENINFO) {
		struct fb_fix_screeninfo f;

		memset(&f, 0, sizeof(f));
		f.line_length = FAKE_STRIDE;
		f.smem_len = FAKE_STRIDE * 2 * FAKE_H;
		memcpy(arg, &f, sizeof(f));
	} else if (req == FBIOPUT_VSCREENINFO) {
		errno = EPERM;	/* fblog must never change the format */
		return -1;
	}
	return 0;
}

static int ioc_count(unsigned long req, long arg)
{
	int i, n = 0;

	for (i = 0; i < g_nioc; i++)
		if (g_ioc[i].req == req && (arg < 0 || g_ioc[i].arg == arg))
			n++;
	return n;
}

struct dfix {
	char dir[64];
	char fb[96], lock[96], flag[96];
	struct daemon d;
};

static void dfix_init(struct dfix *f)
{
	int fd;

	snprintf(f->dir, sizeof(f->dir), "/tmp/fblog-daemon-test-XXXXXX");
	CHECK(mkdtemp(f->dir) != NULL);
	snprintf(f->fb, sizeof(f->fb), "%s/fb0", f->dir);
	snprintf(f->lock, sizeof(f->lock), "%s/fb0.lock", f->dir);
	snprintf(f->flag, sizeof(f->flag), "%s/fblog.off", f->dir);
	fd = open(f->fb, O_RDWR | O_CREAT, 0600);
	CHECK(fd >= 0 && ftruncate(fd, (off_t)FAKE_STRIDE * 2 * FAKE_H) == 0);
	close(fd);
	g_kmsg_path = "/dev/null";
	g_bl_path = "/dev/null";
	g_flag_path = f->flag;
	g_nioc = 0;
	memset(&f->d, 0, sizeof(f->d));
	f->d.fbpath = f->fb;
	f->d.off_path = f->flag;
	f->d.bl = 96;
	f->d.scale = 1;
	f->d.max_kern_level = 3;
	f->d.kfd = -1;
	f->d.fb.fd = -1;
	snprintf(f->d.release, sizeof(f->d.release), "test");
	ring_init(&f->d.ring);
	f->d.lockfd = fb_lock_open(f->lock);
	CHECK(f->d.lockfd >= 0);
}

static void dfix_done(struct dfix *f)
{
	screen_off(&f->d);
	if (f->d.lockfd >= 0)
		close(f->d.lockfd);
	unlink(f->fb);
	unlink(f->lock);
	unlink(f->flag);
	rmdir(f->dir);
	g_flag_path = NULL;
}

static void touch_file(const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT, 0644);

	CHECK(fd >= 0);
	close(fd);
}

/* A foreground client (chefui) takes the screen exclusively. */
static int borrow(struct dfix *f)
{
	int fg = fb_lock_open(f->lock);

	CHECK(fg >= 0);
	CHECK(fb_lock_exclusive(fg, 100) == 0);
	return fg;
}

static void test_daemon_resume_without_flag(void)
{
	struct dfix f;
	int fg;

	dfix_init(&f);
	CHECK(tick(&f.d) == 0);
	CHECK(f.d.fb_open && f.d.mapped && !f.d.paused && !f.d.idle);
	CHECK(ioc_count(FBIOBLANK, FB_BLANK_UNBLANK) == 1);
	CHECK(ioc_count(FBIOPAN_DISPLAY, -1) >= 1);

	fg = borrow(&f);
	CHECK(tick(&f.d) == 0);
	CHECK(f.d.paused && !f.d.mapped && f.d.fb_open);	/* fd kept, mapping dropped */
	close(fg);	/* chefui exits (or dies) with the screen on: no flag */

	g_nioc = 0;
	CHECK(tick(&f.d) == 0);
	CHECK(!f.d.paused && !f.d.idle && f.d.mapped);
	CHECK(ioc_count(FBIOBLANK, FB_BLANK_UNBLANK) == 1);	/* resumed */
	CHECK(ioc_count(FBIOBLANK, FB_BLANK_POWERDOWN) == 0);
	CHECK(ioc_count(FBIOPUT_VSCREENINFO, -1) == 0);
	dfix_done(&f);
}

/* The spec case: resuming after a borrow with the flag present stays idle
 * and closes fb0. */
static void test_daemon_resume_with_flag(void)
{
	struct dfix f;
	int fg, oldfd, again;

	dfix_init(&f);
	CHECK(tick(&f.d) == 0 && f.d.fb_open);
	fg = borrow(&f);
	CHECK(tick(&f.d) == 0 && f.d.paused);
	oldfd = f.d.fb.fd;
	touch_file(f.flag);	/* chefui turned the screen off ... */
	close(fg);		/* ... and exited or died */

	g_nioc = 0;
	CHECK(tick(&f.d) == 0);
	CHECK(f.d.idle && !f.d.paused && !f.d.fb_open && !f.d.mapped);
	CHECK(f.d.fb.fd == -1);
	CHECK(fcntl(oldfd, F_GETFD) == -1 && errno == EBADF);	/* fb0 really closed */
	CHECK(g_nioc == 1);
	CHECK(g_ioc[0].req == FBIOBLANK && g_ioc[0].arg == FB_BLANK_POWERDOWN);
	CHECK(ioc_count(FBIOBLANK, FB_BLANK_UNBLANK) == 0);
	CHECK(ioc_count(FBIOPAN_DISPLAY, -1) == 0);
	/* the shared lock is not left held: a new foreground client gets it at once */
	again = fb_lock_open(f.lock);
	CHECK(fb_lock_exclusive(again, 0) == 0);
	close(again);
	dfix_done(&f);
}

/* Child: remove the flag after delay_ms, then exit. */
static pid_t remove_later(const char *path, int delay_ms)
{
	pid_t p = fork();

	if (p == 0) {
		struct timespec ts = { delay_ms / 1000, (long)(delay_ms % 1000) * 1000000L };

		nanosleep(&ts, NULL);
		unlink(path);
		_exit(0);
	}
	CHECK(p > 0);
	return p;
}

static double mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void idle_then_start(bool poll_only)
{
	struct dfix f;
	pid_t child;
	int status;
	double t0;

	dfix_init(&f);
	f.d.idle_poll_only = poll_only;
	touch_file(f.flag);
	f.d.idle = true;
	child = remove_later(f.flag, 600);
	t0 = mono_ms();
	CHECK(idle_until_flag_gone(&f.d) == 0);
	CHECK(mono_ms() - t0 >= 500);		/* it really waited */
	CHECK(!f.d.idle);
	if (poll_only)
		CHECK(f.d.idle_wakeups >= 2);	/* 250 ms rechecks */
	else
		CHECK(f.d.idle_wakeups == 1);	/* woken once, by the delete */
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(g_nioc == 0);			/* fb0 untouched while idle */

	CHECK(tick(&f.d) == 0);
	CHECK(f.d.fb_open && f.d.mapped && !f.d.paused);
	CHECK(ioc_count(FBIOBLANK, FB_BLANK_UNBLANK) == 1);
	CHECK(ioc_count(FBIOPAN_DISPLAY, -1) >= 1);
	dfix_done(&f);
}

static void test_daemon_idle_flag_removed(void)
{
	idle_then_start(false);
	idle_then_start(true);
}

static void idle_stop_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void test_daemon_idle_signal(void)
{
	for (int fallback = 0; fallback < 2; fallback++) {
		struct dfix f;
		int ready[2], status = 0;
		char byte;
		pid_t child, done;
		double start;

		dfix_init(&f);
		touch_file(f.flag);
		f.d.idle = true;
		f.d.idle_poll_only = fallback;
		CHECK(pipe(ready) == 0);
		child = fork();
		CHECK(child >= 0);
		if (child == 0) {
			close(ready[0]);
			signal(SIGTERM, idle_stop_signal);
			g_stop = 0;
			alarm(3); /* Bound a broken indefinite inotify wait. */
			if (write(ready[1], "r", 1) != 1)
				_exit(2);
			close(ready[1]);
			_exit(idle_until_flag_gone(&f.d) == 1 && f.d.idle ? 0 : 1);
		}
		close(ready[1]);
		CHECK(read(ready[0], &byte, 1) == 1);
		close(ready[0]);
		/* Let the child enter the idle wait before delivering SIGTERM. */
		struct timespec settle = { 0, 100000000L };
		nanosleep(&settle, NULL);
		start = mono_ms();
		CHECK(kill(child, SIGTERM) == 0);
		do {
			struct timespec retry = { 0, 10000000L };
			done = waitpid(child, &status, WNOHANG);
			if (done == 0)
				nanosleep(&retry, NULL);
		} while (done == 0 && mono_ms() - start < 1000);
		CHECK(done == child);
		if (done == 0) {
			kill(child, SIGKILL);
			waitpid(child, &status, 0);
		}
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
		CHECK(access(f.flag, F_OK) == 0);
		CHECK(g_nioc == 0);
		dfix_done(&f);
	}
}

/* Flag removed while chefui still holds the lock (it turned the screen
 * back on): fblog leaves idle into the borrowed state, then resumes once
 * chefui exits. */
static void test_daemon_idle_flag_removed_while_locked(void)
{
	struct dfix f;
	pid_t child;
	int status, fg;

	dfix_init(&f);
	touch_file(f.flag);
	f.d.idle = true;
	fg = borrow(&f);
	child = remove_later(f.flag, 100);
	CHECK(idle_until_flag_gone(&f.d) == 0);
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(tick(&f.d) == 0);
	CHECK(f.d.paused && !f.d.fb_open && !f.d.idle);
	CHECK(g_nioc == 0);			/* nothing opened while borrowed */
	CHECK(next_timeout_ms(f.d.dirty, 0, f.d.paused, 0) <= FB_LOCK_RETRY_MS);
	CHECK(tick(&f.d) == 0 && f.d.paused);	/* still borrowed */
	close(fg);				/* chefui exits with the screen on */
	CHECK(tick(&f.d) == 0);
	CHECK(!f.d.paused && f.d.fb_open && f.d.mapped && !f.d.idle);
	CHECK(ioc_count(FBIOBLANK, FB_BLANK_UNBLANK) == 1);
	dfix_done(&f);
}

static pid_t protocol_child;
static int protocol_signals;
static const char *protocol_flag;

static void protocol_log(const char *fmt, ...)
{
	(void)fmt;
}

static int protocol_kill(const char *comm, int signal_number)
{
	CHECK(strcmp(comm, "fblog") == 0 && signal_number == SIGTERM);
	CHECK(access(protocol_flag, F_OK) == 0); /* flag first, only off signals */
	protocol_signals++;
	return protocol_child > 0 && kill(protocol_child, signal_number) == 0 ? 1 : -ESRCH;
}

static int protocol_read(int fd, char expected)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	char value = 0;
	return poll(&pfd, 1, 700) > 0 && read(fd, &value, 1) == 1 && value == expected;
}

static void test_buttond_fblog_wake_protocol(void)
{
	for (int fallback = 0; fallback < 2; fallback++) {
		struct dfix f;
		struct server buttons;
		int ready[2], status;
		pid_t child;
		dfix_init(&f);
		touch_file(f.flag);
		f.d.idle = true;
		f.d.idle_poll_only = fallback;
		CHECK(pipe(ready) == 0);
		child = fork();
		CHECK(child >= 0);
		if (child == 0) {
			close(ready[0]);
			g_stop = 0;
			signal(SIGTERM, idle_stop_signal);
			alarm(3);
			(void)!write(ready[1], "r", 1);
			if (idle_until_flag_gone(&f.d) || tick(&f.d) || !f.d.fb_open)
				_exit(2);
			(void)!write(ready[1], "o", 1);
			/* Stay resident: a wake signal would cause premature blank. */
			while (!g_stop) poll(NULL, 0, 100);
			screen_off(&f.d);
			(void)!write(ready[1], "c", 1);
			_exit(0);
		}
		close(ready[1]);
		protocol_child = child;
		protocol_signals = 0;
		protocol_flag = f.flag;
		server_init(&buttons, 1500, 300);
		buttons.off_path = f.flag;
		buttons.log = protocol_log;
		buttons.kill_comm = protocol_kill;
		CHECK(protocol_read(ready[0], 'r'));
		CHECK(screen_toggle(&buttons) == 0);
		CHECK(protocol_signals == 0);
		CHECK(protocol_read(ready[0], 'o'));
		CHECK(waitpid(child, &status, WNOHANG) == 0); /* same daemon alive */
		CHECK(screen_toggle(&buttons) == 1);
		CHECK(protocol_signals == 1 && access(f.flag, F_OK) == 0);
		CHECK(protocol_read(ready[0], 'c'));
		CHECK(waitpid(child, &status, 0) == child);
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
		close(ready[0]);
		/* No daemon / delayed init respawn: wake is persistent state,
		 * never a launch or signal. Next fblog start sees flag absent. */
		protocol_child = -1;
		CHECK(screen_toggle(&buttons) == 0 && access(f.flag, F_OK) != 0);
		CHECK(protocol_signals == 1);
		CHECK(idle_until_flag_gone(&f.d) == 0);
		CHECK(tick(&f.d) == 0 && f.d.fb_open && !f.d.idle);
		CHECK(ioc_count(FBIOBLANK, FB_BLANK_UNBLANK) == 1);
		dfix_done(&f);
	}
}

int main(void)
{
	(void)drain_kmsg;
	test_buttond_fblog_wake_protocol();
	test_daemon_resume_without_flag();
	test_daemon_resume_with_flag();
	test_daemon_idle_flag_removed();
	test_daemon_idle_signal();
	test_daemon_idle_flag_removed_while_locked();
	test_timing();
	test_lock_handshake();
	test_kmsg_parse();
	test_policy();
	test_ring();
	test_layout();
	test_geometry();
	test_font();
	test_draw_text();
	test_surface_clear_and_render();
	test_header();
	if (g_failures) {
		fprintf(stderr, "test-fblog: %d/%d checks FAILED\n", g_failures, g_tests);
		return 1;
	}
	printf("test-fblog: %d/%d checks passed\n", g_tests, g_tests);
	return 0;
}
