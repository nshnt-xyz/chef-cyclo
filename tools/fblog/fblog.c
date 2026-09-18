/*
 * fblog - read-only boot/probe output screen for chef-cyclo.
 *
 * Tails /dev/kmsg and paints it on the panel so a fastboot-booted image
 * shows what is running and what it printed, without a serial console and
 * without touch: this process never opens an input device, accepts no
 * input, and nothing it draws is interactive. Everything the image already
 * logs to the kernel ring shows up: /init ("cyclo-init:"), bt-up, gps-up
 * ("gps-up:"), fbtouch ("fbtouch:") and any probe that does
 * `echo "text" > /dev/kmsg` (or `some-cmd 2>&1 | tee /dev/kmsg`).
 *
 * Policy: every userspace line (kmsg facility != 0, i.e. written through
 * /dev/kmsg) is shown; kernel-originated lines only at KERN_ERR (3) or
 * more severe by default (`-k LEVEL` raises/lowers that, `-a` shows all),
 * so the screen is the boot/probe narrative plus real kernel faults
 * (modem ERR_FATAL, I2C failures, oopses), not the whole dmesg. Newest
 * line at the bottom, long lines wrap with a two-space indent, a header
 * shows the kernel release, uptime, battery level and how many records
 * the ring overran while we were behind.
 *
 * Framebuffer contract (fbdev.h, live-verified quirks in the README's
 * gotchas): open, FBIOBLANK UNBLANK, mmap, draw, FBIOPAN_DISPLAY, stay
 * resident; write lcd-backlight then commit; heartbeat-commit once a
 * second while idle so an external `fbtouch bl N` lands (in practice the
 * `fbtouch: backlight N` kmsg note itself triggers the repaint commit, so
 * it lands in ~30 ms; the heartbeat is the fallback for a writer that
 * logs nothing); FBIOBLANK POWERDOWN before the last close
 * (SIGTERM/SIGINT).
 *
 * Coexistence with `fbtouch show` (which draws its own screen): fbdev.h's
 * advisory screen lock. fblog takes the shared lock only while it draws
 * and commits; when the non-blocking try fails a foreground client owns
 * the screen, so fblog drops its mapping and stops drawing (the fd stays
 * open, which is harmless: mdss_fb refcounts opens and fbtouch blanks
 * through the fb core anyway). Once the try succeeds again fblog resumes
 * with UNBLANK + remap + redraw + backlight commit, which is exactly the
 * panel-on/touch-resume path fbtouch's own -c cycle exercises. Killing
 * fbtouch mid-run releases the lock the same way.
 *
 * Opting out without rebuilding: create /run/fblog.off and `kill` the
 * running fblog; the respawned one idles without touching fb0 (panel and
 * touch IC stay powered down). Remove the file and kill it again to
 * bring the screen back.
 *
 * Usage: fblog [-f /dev/fb0] [-b LEVEL] [-s SCALE] [-k LEVEL] [-a]
 *   -b  backlight 0-255 (default 96)
 *   -s  glyph scale (default 2: 9x15 font -> 18x32 cells incl. leading,
 *       59 cols x 68 rows under the header on 1080x2246)
 *   -k  highest kernel log level shown (default 3 = KERN_ERR)
 *   -a  show every kmsg record
 *
 * The pure parts (record parsing, policy, ring, wrapping layout, glyph
 * rendering, header) are separated from the syscalls and covered by
 * tools/tests/test_fblog.c, which includes this file with FBLOG_NO_MAIN.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "../fbdev.h"
#include "font9x15.h"

#define FBLOG_MAX_LINE   240	/* chars kept per line, incl. the timestamp */
#define FBLOG_RING_LINES 512
#define FBLOG_OFF_PATH   "/run/fblog.off"
#define FBLOG_BL_PATH    "/sys/class/leds/lcd-backlight/brightness"
#define FBLOG_BATT_PATH  "/sys/class/power_supply/battery/capacity"
#define FBLOG_KMSG_MAX   8192	/* >= the kernel's LOG_LINE_MAX + prefix */

/* ----------------------------------------------------------- kmsg records */

struct kmsg_rec {
	unsigned facility;		/* 0 = kernel, 1 = user (/dev/kmsg writers) */
	unsigned level;			/* 0..7 */
	unsigned long long seq;
	unsigned long long ts_us;
	char msg[FBLOG_MAX_LINE];	/* first line only, \xNN unescaped */
};

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/*
 * Parse one /dev/kmsg record: "prio,seq,ts_us,flags[,...];message\n" with
 * optional " KEY=VALUE\n" continuation lines (dropped). The kernel escapes
 * bytes it considers unprintable as \xNN; they are restored here and the
 * renderer draws anything outside 0x20..0x7e as the box glyph. Returns 0,
 * or -1 for anything that is not a record.
 */
static int kmsg_parse(const char *buf, size_t len, struct kmsg_rec *out)
{
	const char *semi = memchr(buf, ';', len);
	const char *p = buf, *end;
	char *ep;
	unsigned long long prio;
	size_t n = 0;

	if (!semi)
		return -1;
	prio = strtoull(p, &ep, 10);
	if (ep == p || *ep != ',')
		return -1;
	p = ep + 1;
	out->seq = strtoull(p, &ep, 10);
	if (ep == p || *ep != ',')
		return -1;
	p = ep + 1;
	out->ts_us = strtoull(p, &ep, 10);
	if (ep == p || (*ep != ',' && *ep != ';'))
		return -1;
	out->facility = (unsigned)(prio >> 3);
	out->level = (unsigned)(prio & 7);

	p = semi + 1;
	end = memchr(p, '\n', len - (size_t)(p - buf));
	if (!end)
		end = buf + len;
	while (p < end && n < sizeof(out->msg) - 1) {
		char c;

		if (*p == '\\' && end - p >= 4 && p[1] == 'x' &&
		    hexval(p[2]) >= 0 && hexval(p[3]) >= 0) {
			c = (char)(hexval(p[2]) * 16 + hexval(p[3]));
			p += 4;
		} else {
			c = *p++;
		}
		out->msg[n++] = c == '\t' ? ' ' : c;
	}
	out->msg[n] = '\0';
	return 0;
}

/* ------------------------------------------------------------------ policy */

enum line_kind { LINE_USER = 0, LINE_KERN, LINE_KERN_ERR, LINE_NOTE };

static bool fblog_wants(const struct kmsg_rec *r, unsigned max_kern_level, bool all)
{
	if (all || r->facility != 0)
		return true;
	return r->level <= max_kern_level;
}

static enum line_kind fblog_kind(const struct kmsg_rec *r)
{
	if (r->facility != 0)
		return LINE_USER;
	return r->level <= 3 ? LINE_KERN_ERR : LINE_KERN;
}

/* -------------------------------------------------------------------- ring */

struct line {
	char text[FBLOG_MAX_LINE];
	uint8_t kind;
	uint16_t len;
};

struct ring {
	struct line l[FBLOG_RING_LINES];
	unsigned head;		/* next slot to write */
	unsigned count;		/* <= FBLOG_RING_LINES */
};

static void ring_init(struct ring *r)
{
	r->head = 0;
	r->count = 0;
}

static void ring_push(struct ring *r, enum line_kind kind, const char *text)
{
	struct line *l = &r->l[r->head];
	size_t n = strlen(text);

	if (n > sizeof(l->text) - 1)
		n = sizeof(l->text) - 1;
	memcpy(l->text, text, n);
	l->text[n] = '\0';
	l->len = (uint16_t)n;
	l->kind = (uint8_t)kind;
	r->head = (r->head + 1) % FBLOG_RING_LINES;
	if (r->count < FBLOG_RING_LINES)
		r->count++;
}

/* i = 0 is the oldest kept line, count-1 the newest. */
static const struct line *ring_get(const struct ring *r, unsigned i)
{
	unsigned first = (r->head + FBLOG_RING_LINES - r->count) % FBLOG_RING_LINES;

	return &r->l[(first + i) % FBLOG_RING_LINES];
}

/* "  123.456 message" -- seconds.milliseconds of the kernel timestamp. */
static void format_line(char *out, size_t cap, const struct kmsg_rec *r)
{
	snprintf(out, cap, "%5llu.%03llu %s", r->ts_us / 1000000ULL,
		 (r->ts_us / 1000ULL) % 1000ULL, r->msg);
}

/* ------------------------------------------------------------------ layout */

#define FBLOG_INDENT 2		/* continuation rows start with two spaces */

/* Number of screen rows a line takes at `cols` columns. */
static unsigned line_segments(unsigned len, unsigned cols)
{
	unsigned rest, per;

	if (cols <= FBLOG_INDENT)
		return 1;
	if (len <= cols)
		return 1;
	rest = len - cols;
	per = cols - FBLOG_INDENT;
	return 1 + (rest + per - 1) / per;
}

/* Text of segment `seg` of `l` into out (cap >= cols + 1); returns its length. */
static unsigned segment_text(const struct line *l, unsigned seg, unsigned cols,
			     char *out, size_t cap)
{
	unsigned n = 0, off, take;

	if (cap == 0)
		return 0;
	if (seg == 0) {
		take = l->len < cols ? l->len : cols;
		off = 0;
	} else {
		unsigned per = cols > FBLOG_INDENT ? cols - FBLOG_INDENT : 0;
		off = cols + (seg - 1) * per;
		take = off < l->len ? l->len - off : 0;
		if (take > per)
			take = per;
		while (n < FBLOG_INDENT && n < cap - 1)
			out[n++] = ' ';
	}
	if (take > cap - 1 - n)
		take = (unsigned)(cap - 1 - n);
	memcpy(out + n, l->text + off, take);
	n += take;
	out[n] = '\0';
	return n;
}

struct row_ref {
	unsigned line;		/* ring index (oldest = 0) */
	unsigned seg;
};

/*
 * Fill up to `rows` screen rows with the tail of the ring, newest at the
 * bottom, wrapping each line at `cols`. Returns the number of rows used;
 * a partially visible oldest line shows its trailing segments only.
 */
static unsigned layout_tail(const struct ring *r, unsigned cols, unsigned rows,
			    struct row_ref *out)
{
	unsigned total = 0, i = r->count, first, skip, n = 0;

	if (rows == 0 || r->count == 0)
		return 0;
	while (i > 0) {
		i--;
		total += line_segments(ring_get(r, i)->len, cols);
		if (total >= rows)
			break;
	}
	first = i;
	skip = total > rows ? total - rows : 0;
	for (i = first; i < r->count && n < rows; i++) {
		unsigned segs = line_segments(ring_get(r, i)->len, cols), s;

		for (s = i == first ? skip : 0; s < segs && n < rows; s++) {
			out[n].line = i;
			out[n].seg = s;
			n++;
		}
	}
	return n;
}

/* ---------------------------------------------------------------- geometry */

struct geom {
	unsigned scale;
	unsigned cell_w, cell_h;	/* px per glyph cell */
	unsigned margin;		/* px, left/right/bottom */
	unsigned header_h;		/* px, header band incl. its padding */
	unsigned cols, rows;		/* text grid under the header */
	unsigned text_y0;		/* first text row's y */
};

static void geom_compute(struct geom *g, uint32_t xres, uint32_t yres, unsigned scale)
{
	if (scale < 1)
		scale = 1;
	g->scale = scale;
	g->cell_w = FBLOG_FONT_W * scale;
	g->cell_h = (FBLOG_FONT_H + 1) * scale;	/* one scaled px of leading */
	g->margin = 2 * scale;
	g->header_h = g->cell_h + 2 * g->margin;
	g->cols = xres > 2 * g->margin ? (xres - 2 * g->margin) / g->cell_w : 0;
	g->text_y0 = g->header_h + g->margin;
	g->rows = yres > g->text_y0 + g->margin
		? (yres - g->text_y0 - g->margin) / g->cell_h : 0;
}

/* --------------------------------------------------------------- rendering */

static const uint16_t *glyph_for(unsigned char c)
{
	if (c >= 0x20 && c < 0x7f)
		return fblog_font[c - 0x20];
	return fblog_font[FBLOG_FONT_GLYPHS - 1];
}

/* Draw up to maxchars of s at (x, y); each font pixel becomes a scale x
 * scale block. put_pixel clips, so partial glyphs at the edge are safe. */
static void draw_text(struct surface *s, int x, int y, unsigned scale,
		      uint32_t fg, const char *text, unsigned maxchars)
{
	unsigned ci;

	for (ci = 0; ci < maxchars && text[ci]; ci++) {
		const uint16_t *glyph = glyph_for((unsigned char)text[ci]);
		int gx = x + (int)(ci * FBLOG_FONT_W * scale);
		unsigned row, col;

		for (row = 0; row < FBLOG_FONT_H; row++) {
			uint16_t bits = glyph[row];
			if (!bits)
				continue;
			for (col = 0; col < FBLOG_FONT_W; col++)
				if (bits & (1u << (FBLOG_FONT_W - 1 - col)))
					fill_rect(s, gx + (int)(col * scale),
						  y + (int)(row * scale),
						  (int)scale, (int)scale, fg);
		}
	}
}

/* Fill the whole visible page with one packed pixel value. */
static void surface_clear(struct surface *s, uint32_t px)
{
	uint32_t x, y;
	size_t row_bytes = (size_t)s->xres * s->bpp;

	for (x = 0; x < s->xres; x++)
		put_pixel(s, (int)x, 0, px);
	for (y = 1; y < s->yres; y++)
		memcpy(s->base + (size_t)y * s->line_length, s->base, row_bytes);
}

struct palette {
	uint32_t bg, header_bg, header_fg;
	uint32_t kind[4];	/* indexed by enum line_kind */
};

static void palette_init(struct palette *p, const struct fb_var_screeninfo *var)
{
	p->bg = pack_pixel(var, 0, 0, 0);
	p->header_bg = pack_pixel(var, 0, 56, 112);
	p->header_fg = pack_pixel(var, 255, 255, 255);
	p->kind[LINE_USER] = pack_pixel(var, 235, 235, 235);
	p->kind[LINE_KERN] = pack_pixel(var, 150, 150, 150);
	p->kind[LINE_KERN_ERR] = pack_pixel(var, 255, 120, 70);
	p->kind[LINE_NOTE] = pack_pixel(var, 120, 220, 255);
}

/*
 * "chef-cyclo 4.4.192 up 123s bat 94%" plus the lost-record count when
 * the kmsg ring overran us. Returns the length written.
 */
static int format_header(char *buf, size_t cap, const char *release,
			 unsigned long uptime_s, int batt_pct, unsigned long lost)
{
	int n = snprintf(buf, cap, "chef-cyclo %s up %lus", release, uptime_s);

	if (n < 0)
		return 0;
	if (batt_pct >= 0 && (size_t)n < cap)
		n += snprintf(buf + n, cap - (size_t)n, " bat %d%%", batt_pct);
	if (lost && (size_t)n < cap)
		n += snprintf(buf + n, cap - (size_t)n, " lost %lu", lost);
	return (int)strlen(buf);
}

/* Paint header + the ring's tail. Pure given the surface: used by the tests
 * on a malloc'd surface and by the daemon on the mapped fb. */
static void render(struct surface *s, const struct geom *g, const struct palette *pal,
		   const struct ring *r, const char *header)
{
	struct row_ref refs[256];
	unsigned rows = g->rows < 256 ? g->rows : 256, n, i;
	char buf[FBLOG_MAX_LINE + FBLOG_INDENT + 1];

	surface_clear(s, pal->bg);
	fill_rect(s, 0, 0, (int)s->xres, (int)g->header_h, pal->header_bg);
	draw_text(s, (int)g->margin, (int)g->margin, g->scale, pal->header_fg, header, g->cols);

	n = layout_tail(r, g->cols, rows, refs);
	for (i = 0; i < n; i++) {
		const struct line *l = ring_get(r, refs[i].line);
		unsigned len = segment_text(l, refs[i].seg, g->cols, buf, sizeof(buf));
		int y = (int)(g->text_y0 + i * g->cell_h);

		draw_text(s, (int)g->margin, y, g->scale, pal->kind[l->kind & 3], buf, len);
	}
}

/* ------------------------------------------------------------------ timing */

#define FBLOG_REPAINT_MS   100	/* coalesce bursts of lines */
#define FBLOG_HEARTBEAT_MS 1000	/* idle commit: header uptime + backlight pickup */
#define FBLOG_RETRY_MAX_MS 10000

/* Backoff after `fails` consecutive open/map/commit failures: 1, 2, 4, 8 s,
 * then 10 s, so a damaged display (or a respawn loop) cannot spin. */
static unsigned retry_delay_ms(unsigned fails)
{
	unsigned ms;

	if (fails == 0)
		return 0;
	if (fails > 4)
		return FBLOG_RETRY_MAX_MS;
	ms = 1000u << (fails - 1);
	return ms > FBLOG_RETRY_MAX_MS ? FBLOG_RETRY_MAX_MS : ms;
}

/*
 * poll() timeout for the main loop, in ms. since_commit_ms = time since
 * the last successful commit; until_retry_ms > 0 while backing off after
 * a failure (drawing is not attempted before then, kmsg is still drained
 * on POLLIN). Never negative, never 0 unless something is due now.
 */
static int next_timeout_ms(bool dirty, long since_commit_ms, bool paused,
			   long until_retry_ms)
{
	long t;

	if (until_retry_ms > 0)
		return until_retry_ms > INT32_MAX ? INT32_MAX : (int)until_retry_ms;
	t = (dirty ? FBLOG_REPAINT_MS : FBLOG_HEARTBEAT_MS) - since_commit_ms;
	if (t < 0)
		t = 0;
	if (paused && t > FB_LOCK_RETRY_MS)
		t = FB_LOCK_RETRY_MS;
	return (int)t;
}

#ifndef FBLOG_NO_MAIN

/* ------------------------------------------------------------------ daemon */

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void kmsg_note(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int fd, n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	dprintf(fd, "fblog: %s\n", buf);
	close(fd);
}

static int read_int_file(const char *path, int dflt)
{
	char buf[32];
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return dflt;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return dflt;
	buf[n] = '\0';
	return atoi(buf);
}

static void write_backlight(int level)
{
	char v[32];
	int fd = open(FBLOG_BL_PATH, O_WRONLY | O_CLOEXEC);

	if (fd < 0)
		return;
	snprintf(v, sizeof(v), "%d", level);
	(void)!write(fd, v, strlen(v));
	close(fd);
}

static unsigned long uptime_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_BOOTTIME, &ts);
	return (unsigned long)ts.tv_sec;
}

struct daemon {
	const char *fbpath;
	int bl, scale;
	unsigned max_kern_level;
	bool all;

	int kfd, lockfd;
	struct fbdev fb;
	bool fb_open, mapped, paused;
	struct geom geom;
	struct palette pal;
	struct ring ring;
	unsigned long lost;
	char release[65];	/* sizeof(struct utsname.release) */
	double last_commit;
	bool dirty;
	unsigned fails;		/* consecutive show_frame failures */
	double next_try;	/* CLOCK_MONOTONIC s; draw attempts wait for it */
};

/* Drain every pending kmsg record; returns true if a line was added. */
static bool drain_kmsg(struct daemon *d)
{
	char buf[FBLOG_KMSG_MAX];
	bool added = false;

	if (d->kfd < 0)
		return false;
	for (;;) {
		ssize_t n = read(d->kfd, buf, sizeof(buf) - 1);
		struct kmsg_rec rec;
		char text[FBLOG_MAX_LINE + 32];

		if (n < 0) {
			if (errno == EPIPE) {	/* ring overran us; next read resumes */
				d->lost++;
				continue;
			}
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN)
				break;
			kmsg_note("kmsg read: %s", strerror(errno));
			close(d->kfd);
			d->kfd = -1;
			break;
		}
		if (n == 0)
			break;
		if (kmsg_parse(buf, (size_t)n, &rec) < 0)
			continue;
		if (!fblog_wants(&rec, d->max_kern_level, d->all))
			continue;
		format_line(text, sizeof(text), &rec);
		ring_push(&d->ring, fblog_kind(&rec), text);
		added = true;
	}
	return added;
}

/* Under the shared lock: (re)open/unblank/map as needed, paint, commit. */
static int show_frame(struct daemon *d, bool force_backlight)
{
	char header[128];
	int rc;

	if (!d->fb_open) {
		rc = fb_open_probe(&d->fb, d->fbpath);
		if (rc)
			return rc;
		d->fb_open = true;
		geom_compute(&d->geom, d->fb.var.xres, d->fb.var.yres, (unsigned)d->scale);
		palette_init(&d->pal, &d->fb.var);
		kmsg_note("screen %ux%u %u bpp, %ux%u cells at scale %d",
			  d->fb.var.xres, d->fb.var.yres, d->fb.var.bits_per_pixel,
			  d->geom.cols, d->geom.rows, d->scale);
		force_backlight = true;
	}
	if (!d->mapped) {
		/* first show, or resuming after a foreground client blanked
		 * the panel: through the fb core so the touch driver follows */
		rc = fb_unblank(&d->fb);
		if (rc)
			kmsg_note("FBIOBLANK UNBLANK: %s", strerror(-rc));
		rc = fb_map(&d->fb);
		if (rc)
			return rc;
		d->mapped = true;
		force_backlight = true;
	}
	format_header(header, sizeof(header), d->release, uptime_s(),
		      read_int_file(FBLOG_BATT_PATH, -1), d->lost);
	render(&d->fb.surf, &d->geom, &d->pal, &d->ring, header);
	rc = fb_commit(&d->fb);
	if (rc)
		return rc;
	if (force_backlight) {
		write_backlight(d->bl);	/* lands at the next commit (quirk 1) */
		rc = fb_commit(&d->fb);
		if (rc)
			return rc;
	}
	d->last_commit = now_s();
	d->dirty = false;
	return 0;
}

/* One tick: try to own the screen briefly; pause or resume accordingly. */
static int tick(struct daemon *d)
{
	int rc;
	bool resumed = false;

	if (d->lockfd >= 0) {
		rc = fb_lock_try_shared(d->lockfd);
		if (rc == -EWOULDBLOCK) {
			if (!d->paused) {
				d->paused = true;
				fb_unmap(&d->fb);
				d->mapped = false;
				kmsg_note("paused: screen borrowed (%s)", FB_LOCK_PATH);
			}
			return 0;
		}
		if (rc)
			kmsg_note("flock %s: %s (continuing unlocked)", FB_LOCK_PATH, strerror(-rc));
	}
	if (d->paused) {
		d->paused = false;
		resumed = true;
	}
	rc = show_frame(d, resumed);
	if (d->lockfd >= 0)
		fb_lock_release(d->lockfd);
	if (rc) {
		d->fails++;
		d->next_try = now_s() + retry_delay_ms(d->fails) / 1000.0;
		kmsg_note("%s failed: %s; retry %u in %u ms",
			  d->fb_open ? (d->mapped ? "commit" : "map") : "open",
			  strerror(-rc), d->fails, retry_delay_ms(d->fails));
		if (d->fails >= 3 && d->fb_open) {
			/* a mapped-but-uncommittable fb: drop the mapping so the
			 * next attempt goes through unblank + map again */
			fb_unmap(&d->fb);
			d->mapped = false;
		}
	} else {
		d->fails = 0;
		d->next_try = 0;
		if (resumed)
			kmsg_note("resumed");
	}
	return rc;
}

static void screen_off(struct daemon *d)
{
	if (d->fb_open)
		fb_powerdown_close(&d->fb);	/* quirk 2 */
	d->fb_open = false;
	d->mapped = false;
}

static void usage(void)
{
	fprintf(stderr, "usage: fblog [-f /dev/fb0] [-b backlight] [-s scale] [-k kern-level] [-a]\n");
}

int main(int argc, char **argv)
{
	struct daemon d;
	struct utsname un;
	int opt, rc;

	memset(&d, 0, sizeof(d));
	d.fbpath = "/dev/fb0";
	d.bl = 96;
	d.scale = 2;
	d.max_kern_level = 3;
	d.kfd = -1;
	d.lockfd = -1;
	d.fb.fd = -1;
	ring_init(&d.ring);

	while ((opt = getopt(argc, argv, "f:b:s:k:a")) != -1) {
		switch (opt) {
		case 'f': d.fbpath = optarg; break;
		case 'b': d.bl = atoi(optarg); break;
		case 's': d.scale = atoi(optarg); break;
		case 'k': d.max_kern_level = (unsigned)atoi(optarg); break;
		case 'a': d.all = true; break;
		default: usage(); return 64;
		}
	}
	if (d.scale < 1 || d.scale > 8 || d.bl < 0 || d.bl > 255) {
		usage();
		return 64;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	if (access(FBLOG_OFF_PATH, F_OK) == 0) {
		kmsg_note("%s present: idling, not touching %s", FBLOG_OFF_PATH, d.fbpath);
		while (!g_stop)
			pause();
		return 0;
	}

	if (uname(&un) == 0)
		snprintf(d.release, sizeof(d.release), "%s", un.release);
	else
		snprintf(d.release, sizeof(d.release), "?");

	d.kfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (d.kfd < 0)
		kmsg_note("open /dev/kmsg: %s (screen will show only the header)", strerror(errno));
	d.lockfd = fb_lock_open(NULL);
	if (d.lockfd < 0) {
		kmsg_note("lock %s: %s (continuing unlocked)", FB_LOCK_PATH, strerror(-d.lockfd));
		d.lockfd = -1;
	}

	kmsg_note("starting (backlight %d, scale %d, kernel level <= %u%s)",
		  d.bl, d.scale, d.max_kern_level, d.all ? ", all" : "");
	drain_kmsg(&d);	/* the boot so far, from the start of the ring */
	rc = tick(&d);
	if (rc && !d.fb_open) {
		kmsg_note("cannot open %s: %s; exiting in 5 s", d.fbpath, strerror(-rc));
		sleep(5);	/* keeps a respawning init from spinning */
		return 1;
	}

	while (!g_stop) {
		struct pollfd pfd = { .fd = d.kfd, .events = POLLIN };
		double now = now_s();
		long since_ms = (long)((now - d.last_commit) * 1000);
		long retry_ms = d.fails ? (long)((d.next_try - now) * 1000) + 1 : 0;
		int timeout_ms;

		/* repaint promptly when lines arrived, else heartbeat at 1 Hz
		 * (header uptime + quirk-1 backlight pickup); after a failure
		 * wait out the backoff (retry_delay_ms) instead of spinning */
		timeout_ms = next_timeout_ms(d.dirty, since_ms, d.paused, retry_ms);

		rc = poll(&pfd, d.kfd >= 0 ? 1 : 0, timeout_ms);
		if (rc < 0 && errno != EINTR)
			break;
		/* devkmsg_poll() reports a ring overrun as POLLERR|POLLPRI, not
		 * POLLIN: drain on those too so the EPIPE is consumed and the
		 * reader moves on to the next record */
		if (rc > 0 && (pfd.revents & (POLLIN | POLLERR | POLLPRI)) && drain_kmsg(&d))
			d.dirty = true;
		if (g_stop)
			break;

		now = now_s();
		if (d.fails && now < d.next_try)
			continue;
		since_ms = (long)((now - d.last_commit) * 1000);
		if ((d.dirty && since_ms >= FBLOG_REPAINT_MS) || since_ms >= FBLOG_HEARTBEAT_MS ||
		    d.paused || d.fails)
			(void)tick(&d);
	}

	kmsg_note("exiting: blanking before close");
	screen_off(&d);
	if (d.kfd >= 0)
		close(d.kfd);
	if (d.lockfd >= 0)
		close(d.lockfd);
	return 0;
}

#endif /* FBLOG_NO_MAIN */
