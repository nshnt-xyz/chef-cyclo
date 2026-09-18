/*
 * fbtouch - display and touch-panel probe for chef-cyclo.
 *
 * Exercises the legacy fbdev contract of the 4.4 MDSS driver
 * (drivers/video/fbdev/msm/mdss_fb.c) and the evdev stream of the Novatek
 * NT36xxx touch driver, without Android:
 *
 *   open /dev/fb0             -> mdss_fb_open() unblanks the panel
 *   FBIOBLANK UNBLANK         -> fb core fires FB_EVENT_BLANK so the touch
 *                                driver's fb notifier resumes the controller
 *   mmap                      -> mdss_fb_fbmem_ion_mmap() allocates the fb
 *   draw + FBIOPAN_DISPLAY    -> mdss_fb_pan_display() commits the frame
 *   lcd-backlight brightness  -> applied by mdss at the *next* frame commit
 *   FBIOBLANK POWERDOWN       -> panel off through the fb core, touch driver
 *                                suspended by the same notifier
 *   close (last fd)           -> mdss_fb_release_all() powers the panel down
 *
 * so the fb stays open for the whole run; the display only lives as long
 * as this process (or, later, the UI) holds it.
 *
 * Two mdss_fb quirks, both live-verified on chef (2026-09-18) and both
 * handled here, that any later display client must respect too:
 *
 * 1. A backlight write only reaches the WLED on the next frame commit.
 *    mdss_fb_release_all() sets the backlight to 0 *before* powering the
 *    panel down, so the power-down saves 0 as the level to restore. The
 *    next open's unblank restores that 0, clears the "unset" level and
 *    blocks backlight updates until the first kickoff -- but
 *    mdss_fb_update_backlight() returns early when there is no unset
 *    level, so the block is never lifted by the first frame. Every later
 *    sysfs write is parked until a further commit. Android's HWC commits
 *    continuously and never notices; a tool that pans once and waits gets
 *    a black screen (the DSI link is up and the panel answers 0x9C, only
 *    the WLED is off). So: commit a frame after every backlight write,
 *    and commit a heartbeat frame once a second while idle so an external
 *    `fbtouch bl N` takes effect too.
 *
 * 2. The last-close power-down bypasses the fb notifier chain (it calls
 *    mdss_fb_blank_sub() directly, not fb_blank()), so the NT36xxx driver
 *    never suspends although the IC -- a TDDI, on the panel's own rails --
 *    loses power. On the next open the driver says "Touch is already
 *    resume", skips its bootloader reset, and no touch events arrive. So:
 *    blank through FBIOBLANK POWERDOWN before closing, which suspends the
 *    driver, and unblank through FBIOBLANK UNBLANK after opening, which
 *    resumes it with the reset.
 *
 * Commands:
 *   fbtouch info                 panel/fb sysfs and touch device caps (does
 *                                not open fb0, so no blank side effect)
 *   fbtouch show [options]       test pattern, backlight, then touch events
 *   fbtouch bl <0-255>           set the backlight (takes effect at the next
 *                                frame commit -- see quirk 1 below)
 * Options for show:
 *   -t SECS      run for SECS seconds after the frame is up (0 = until
 *                SIGTERM/SIGINT; default 30)
 *   -b LEVEL     backlight 0-255 written after the first commit (default 128)
 *   -f DEV       framebuffer (default /dev/fb0)
 *   -i DEV       input device (default: scan /dev/input/event* for the
 *                first one with ABS_MT_POSITION_X)
 *   -c SECS      at +SECS into the run, blank (FB_BLANK_POWERDOWN) the panel
 *                for 3 s and unblank it again through the fb core, then
 *                redraw: exercises mdss_dsi_panel_off/on and the touch
 *                driver's suspend/resume notifier without closing the fd
 *   -r           exit 2 if no touch contact was seen ("require touch")
 *   -q           don't paint touch contacts, only log them
 *
 * Output is one line per event on stdout (line-buffered) plus "fbtouch:"
 * markers on /dev/kmsg so the run can be correlated with dmesg. The pure
 * parts (pixel packing, the multitouch slot decoder, the test pattern) are
 * separated from the syscalls and covered by tools/tests/test_fbtouch.c,
 * which includes this file with FBTOUCH_NO_MAIN defined.
 */
#define _GNU_SOURCE
#include <dirent.h>
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
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <linux/fb.h>
#include <linux/input.h>

#define BL_PATH "/sys/class/leds/lcd-backlight/brightness"
#define BL_MAX_PATH "/sys/class/leds/lcd-backlight/max_brightness"
#define FB_SYSFS "/sys/class/graphics/fb0"

/* ---------------------------------------------------------------- pixels */

/*
 * Pack an 8-bit-per-channel colour according to the framebuffer's bitfield
 * layout. MDSS registers fb0 as MDP_RGBA_8888 (red at bit 0, green 8, blue
 * 16, alpha 24 - i.e. bytes R,G,B,A in memory), but reading the offsets
 * from the var keeps this correct for RGB565 or ARGB layouts too.
 */
static inline uint32_t pack_channel(uint32_t v8, const struct fb_bitfield *bf)
{
	if (bf->length == 0)
		return 0;
	if (bf->length >= 8)
		return (v8 << (bf->length - 8)) << bf->offset;
	return (v8 >> (8 - bf->length)) << bf->offset;
}

static uint32_t pack_pixel(const struct fb_var_screeninfo *var,
			   uint8_t r, uint8_t g, uint8_t b)
{
	uint32_t px = pack_channel(r, &var->red) | pack_channel(g, &var->green)
		    | pack_channel(b, &var->blue);
	if (var->transp.length)
		px |= pack_channel(0xff, &var->transp);
	return px;
}

struct surface {
	uint8_t *base;			/* start of the visible page */
	uint32_t xres, yres;
	uint32_t line_length;		/* bytes per line */
	uint32_t bpp;			/* bytes per pixel: 2 or 4 */
	const struct fb_var_screeninfo *var;
};

static void put_pixel(struct surface *s, int x, int y, uint32_t px)
{
	uint8_t *p;

	if (x < 0 || y < 0 || (uint32_t)x >= s->xres || (uint32_t)y >= s->yres)
		return;
	p = s->base + (size_t)y * s->line_length + (size_t)x * s->bpp;
	if (s->bpp == 4)
		memcpy(p, &px, 4);
	else {
		uint16_t v = (uint16_t)px;
		memcpy(p, &v, 2);
	}
}

static void fill_rect(struct surface *s, int x0, int y0, int w, int h, uint32_t px)
{
	int x, y;

	for (y = y0; y < y0 + h; y++)
		for (x = x0; x < x0 + w; x++)
			put_pixel(s, x, y, px);
}

static void fill_circle(struct surface *s, int cx, int cy, int r, uint32_t px)
{
	int x, y;

	for (y = -r; y <= r; y++)
		for (x = -r; x <= r; x++)
			if (x * x + y * y <= r * r)
				put_pixel(s, cx + x, cy + y, px);
}

/*
 * Test pattern: eight colour bars over the top 55 %, a horizontal grey ramp
 * under them, a 6-pixel white border, and a 4x8 grid of thin lines over
 * the ramp area so a touch can be placed against a known position. Every
 * primary and secondary colour appears, so a channel-order mistake (R/B
 * swap) or a missing lane shows up at a glance.
 */
static const uint8_t bar_rgb[8][3] = {
	{ 255, 255, 255 }, { 255, 255, 0 }, { 0, 255, 255 }, { 0, 255, 0 },
	{ 255, 0, 255 },   { 255, 0, 0 },   { 0, 0, 255 },   { 0, 0, 0 },
};

/* Colour of the test pattern at (x, y), before the border/grid overlay. */
static void pattern_rgb(uint32_t xres, uint32_t yres, int x, int y,
			uint8_t *r, uint8_t *g, uint8_t *b)
{
	uint32_t bars_h = yres * 55 / 100;

	if ((uint32_t)y < bars_h) {
		int i = x * 8 / (int)xres;
		if (i > 7)
			i = 7;
		*r = bar_rgb[i][0];
		*g = bar_rgb[i][1];
		*b = bar_rgb[i][2];
	} else {
		uint8_t v = (uint8_t)(x * 255 / (int)(xres - 1));
		*r = *g = *b = v;
	}
}

static void draw_pattern(struct surface *s)
{
	const int border = 6;
	uint32_t white = pack_pixel(s->var, 255, 255, 255);
	uint32_t grey = pack_pixel(s->var, 96, 96, 96);
	uint32_t bars_h = s->yres * 55 / 100;
	int x, y, i;

	for (y = 0; (uint32_t)y < s->yres; y++) {
		for (x = 0; (uint32_t)x < s->xres; x++) {
			uint8_t r, g, b;
			pattern_rgb(s->xres, s->yres, x, y, &r, &g, &b);
			put_pixel(s, x, y, pack_pixel(s->var, r, g, b));
		}
	}
	/* grid over the ramp area: 4 columns x 8 rows */
	for (i = 1; i < 4; i++)
		fill_rect(s, (int)(s->xres * i / 4), (int)bars_h, 2,
			  (int)(s->yres - bars_h), grey);
	for (i = 1; i < 8; i++)
		fill_rect(s, 0, (int)(bars_h + (s->yres - bars_h) * i / 8),
			  (int)s->xres, 2, grey);
	/* border */
	fill_rect(s, 0, 0, (int)s->xres, border, white);
	fill_rect(s, 0, (int)s->yres - border, (int)s->xres, border, white);
	fill_rect(s, 0, 0, border, (int)s->yres, white);
	fill_rect(s, (int)s->xres - border, 0, border, (int)s->yres, white);
}

/* ----------------------------------------------------- multitouch decode */

#define MT_MAX_SLOTS 16

enum mt_action { MT_NONE = 0, MT_DOWN, MT_MOVE, MT_UP };

struct mt_slot {
	int id;			/* tracking id, -1 = no contact */
	int x, y;
	bool dirty;		/* something changed since the last SYN_REPORT */
};

struct mt_state {
	struct mt_slot slot[MT_MAX_SLOTS];
	int cur;		/* current slot (ABS_MT_SLOT) */
	bool mt;		/* device speaks MT protocol B */
	bool btn_touch;		/* single-touch fallback: BTN_TOUCH state */
};

struct mt_event {
	int slot;
	enum mt_action action;
	int id, x, y;
};

static void mt_init(struct mt_state *st, bool mt)
{
	int i;

	memset(st, 0, sizeof(*st));
	st->mt = mt;
	for (i = 0; i < MT_MAX_SLOTS; i++)
		st->slot[i].id = -1;
}

/*
 * Feed one evdev event. Returns the number of contact events written to
 * out (at most MT_MAX_SLOTS, only ever on SYN_REPORT). For MT-B devices
 * (the NT36xxx driver) a contact begins when a slot's tracking id goes
 * from -1 to >= 0 and ends when it returns to -1. For legacy single-touch
 * devices, BTN_TOUCH + ABS_X/ABS_Y drive slot 0 instead.
 */
static int mt_feed(struct mt_state *st, const struct input_event *ev,
		   struct mt_event *out)
{
	struct mt_slot *s;
	int n = 0, i;

	if (ev->type == EV_ABS) {
		switch (ev->code) {
		case ABS_MT_SLOT:
			if (ev->value >= 0 && ev->value < MT_MAX_SLOTS)
				st->cur = ev->value;
			return 0;
		case ABS_MT_TRACKING_ID:
			s = &st->slot[st->cur];
			if (s->id != ev->value) {
				s->dirty = true;
				/* new contact: remember the id, positions follow */
				if (ev->value < 0)
					s->id = -2; /* pending UP, see SYN */
				else
					s->id = ev->value;
			}
			return 0;
		case ABS_MT_POSITION_X:
			s = &st->slot[st->cur];
			if (s->x != ev->value) {
				s->x = ev->value;
				s->dirty = true;
			}
			return 0;
		case ABS_MT_POSITION_Y:
			s = &st->slot[st->cur];
			if (s->y != ev->value) {
				s->y = ev->value;
				s->dirty = true;
			}
			return 0;
		case ABS_X:
			if (st->mt)
				return 0;
			if (st->slot[0].x != ev->value) {
				st->slot[0].x = ev->value;
				st->slot[0].dirty = true;
			}
			return 0;
		case ABS_Y:
			if (st->mt)
				return 0;
			if (st->slot[0].y != ev->value) {
				st->slot[0].y = ev->value;
				st->slot[0].dirty = true;
			}
			return 0;
		default:
			return 0;
		}
	}
	if (ev->type == EV_KEY && ev->code == BTN_TOUCH && !st->mt) {
		bool down = ev->value != 0;
		if (down != st->btn_touch) {
			st->btn_touch = down;
			st->slot[0].dirty = true;
			st->slot[0].id = down ? 0 : -2;
		}
		return 0;
	}
	if (ev->type != EV_SYN || ev->code != SYN_REPORT)
		return 0;

	for (i = 0; i < MT_MAX_SLOTS; i++) {
		s = &st->slot[i];
		if (!s->dirty)
			continue;
		s->dirty = false;
		if (s->id == -2) {
			s->id = -1;
			out[n].action = MT_UP;
		} else if (s->id < 0) {
			continue; /* position noise on an empty slot */
		} else {
			/* the tracker turns a slot's first live report into DOWN */
			out[n].action = MT_MOVE;
		}
		out[n].slot = i;
		out[n].id = s->id;
		out[n].x = s->x;
		out[n].y = s->y;
		n++;
	}
	return n;
}

/*
 * mt_feed() reports live slots as MOVE and ended ones as UP; the tracker
 * layers "has this slot reported since it became live" on top so a slot's
 * first live report becomes DOWN.
 */
struct mt_tracker {
	struct mt_state st;
	bool seen[MT_MAX_SLOTS];
};

static void mt_tracker_init(struct mt_tracker *t, bool mt)
{
	mt_init(&t->st, mt);
	memset(t->seen, 0, sizeof(t->seen));
}

static int mt_tracker_feed(struct mt_tracker *t, const struct input_event *ev,
			   struct mt_event *out)
{
	int n = mt_feed(&t->st, ev, out), i;

	for (i = 0; i < n; i++) {
		struct mt_event *e = &out[i];
		if (e->action == MT_UP) {
			t->seen[e->slot] = false;
		} else if (!t->seen[e->slot]) {
			t->seen[e->slot] = true;
			e->action = MT_DOWN;
		}
	}
	return n;
}

/* Map a raw ABS value in [min,max] to a screen coordinate in [0,res-1]. */
static int abs_to_screen(int v, int min, int max, uint32_t res)
{
	long range = (long)max - min;

	if (range <= 0)
		return v;
	if (v < min)
		v = min;
	if (v > max)
		v = max;
	return (int)(((long)(v - min) * (long)(res - 1)) / range);
}

#ifndef FBTOUCH_NO_MAIN

/* --------------------------------------------------------------- helpers */

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void kmsg(const char *fmt, ...)
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
	dprintf(fd, "fbtouch: %s\n", buf);
	close(fd);
}

static int read_sysfs(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -errno;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		return -errno;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		n--;
	buf[n] = '\0';
	return 0;
}

static int write_sysfs(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -errno;
	n = write(fd, val, strlen(val));
	close(fd);
	return n < 0 ? -errno : 0;
}

/* Writes the mdss LED node. Note quirk 1 in the header: the level reaches
 * the WLED at the next frame commit, i.e. within a second while `show`
 * runs (heartbeat), never on its own. */
static int set_backlight(int level)
{
	char v[16];
	int rc;

	snprintf(v, sizeof(v), "%d", level);
	rc = write_sysfs(BL_PATH, v);
	if (rc)
		printf("backlight: write %s failed: %s\n", BL_PATH, strerror(-rc));
	else {
		char back[16] = "?";
		read_sysfs(BL_PATH, back, sizeof(back));
		printf("backlight: wrote %d, readback %s\n", level, back);
		kmsg("backlight %d (readback %s)", level, back);
	}
	return rc;
}

/* --------------------------------------------------------------- touch */

struct touch_dev {
	int fd;
	char path[300];
	char name[80];
	bool mt;
	struct input_absinfo ax, ay;	/* ABS_MT_POSITION_X/Y or ABS_X/Y */
	int slots;
};

static bool has_bit(const unsigned long *bits, unsigned int bit)
{
	return (bits[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1;
}

static int touch_open_one(struct touch_dev *td, const char *path)
{
	unsigned long absbits[(ABS_MAX + 8 * sizeof(long)) / (8 * sizeof(long))];
	int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);

	if (fd < 0)
		return -errno;
	memset(absbits, 0, sizeof(absbits));
	if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) < 0) {
		close(fd);
		return -ENOTTY;
	}
	memset(td, 0, sizeof(*td));
	td->fd = fd;
	snprintf(td->path, sizeof(td->path), "%s", path);
	if (ioctl(fd, EVIOCGNAME(sizeof(td->name)), td->name) < 0)
		snprintf(td->name, sizeof(td->name), "?");
	if (has_bit(absbits, ABS_MT_POSITION_X) && has_bit(absbits, ABS_MT_POSITION_Y)) {
		struct input_absinfo slot;
		td->mt = true;
		ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &td->ax);
		ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &td->ay);
		if (has_bit(absbits, ABS_MT_SLOT) && ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &slot) == 0)
			td->slots = slot.maximum + 1;
		return 0;
	}
	if (has_bit(absbits, ABS_X) && has_bit(absbits, ABS_Y)) {
		ioctl(fd, EVIOCGABS(ABS_X), &td->ax);
		ioctl(fd, EVIOCGABS(ABS_Y), &td->ay);
		return 0;
	}
	close(fd);
	return -ENODEV;
}

/* Find the first MT (or, failing that, single-touch ABS) input device. */
static int touch_open(struct touch_dev *td, const char *explicit)
{
	struct dirent *de;
	DIR *d;
	int best = -ENODEV;
	struct touch_dev cand, st_fallback;
	bool have_st = false;

	if (explicit)
		return touch_open_one(td, explicit);
	d = opendir("/dev/input");
	if (!d)
		return -errno;
	while ((de = readdir(d))) {
		char path[sizeof(cand.path)];
		if (strncmp(de->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		if (touch_open_one(&cand, path))
			continue;
		if (cand.mt) {
			*td = cand;
			best = 0;
			break;
		}
		if (!have_st) {
			st_fallback = cand;
			have_st = true;
		} else {
			close(cand.fd);
		}
	}
	closedir(d);
	if (best == 0) {
		if (have_st)
			close(st_fallback.fd);
		return 0;
	}
	if (have_st) {
		*td = st_fallback;
		return 0;
	}
	return best;
}

static void touch_print(const struct touch_dev *td)
{
	printf("touch: %s \"%s\" %s x=[%d..%d] y=[%d..%d] slots=%d\n",
	       td->path, td->name, td->mt ? "MT-B" : "single-touch",
	       td->ax.minimum, td->ax.maximum, td->ay.minimum, td->ay.maximum,
	       td->slots);
}

/* ------------------------------------------------------------------ fb */

struct fbdev {
	int fd;
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	uint8_t *map;
	size_t map_len;
	struct surface surf;
};

static int fb_open_probe(struct fbdev *fb, const char *path)
{
	fb->fd = open(path, O_RDWR | O_CLOEXEC);
	if (fb->fd < 0) {
		printf("fb: open %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->var) < 0 ||
	    ioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->fix) < 0) {
		printf("fb: FBIOGET_*SCREENINFO: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

static void fb_print(const struct fbdev *fb)
{
	printf("fb: %s %ux%u (virtual %ux%u) %u bpp line_length=%u smem_len=%u\n",
	       fb->fix.id, fb->var.xres, fb->var.yres, fb->var.xres_virtual,
	       fb->var.yres_virtual, fb->var.bits_per_pixel, fb->fix.line_length,
	       fb->fix.smem_len);
	printf("fb: red@%u/%u green@%u/%u blue@%u/%u transp@%u/%u\n",
	       fb->var.red.offset, fb->var.red.length, fb->var.green.offset,
	       fb->var.green.length, fb->var.blue.offset, fb->var.blue.length,
	       fb->var.transp.offset, fb->var.transp.length);
}

static int fb_map(struct fbdev *fb)
{
	uint32_t bpp = fb->var.bits_per_pixel / 8;

	if (bpp != 2 && bpp != 4) {
		printf("fb: unsupported %u bpp\n", fb->var.bits_per_pixel);
		return -1;
	}
	/* one visible page is enough; the driver allocates the whole
	 * virtual size on the first mmap anyway */
	fb->map_len = (size_t)fb->fix.line_length * fb->var.yres_virtual;
	if (fb->map_len == 0)
		fb->map_len = (size_t)fb->fix.line_length * fb->var.yres;
	fb->map = mmap(NULL, fb->map_len, PROT_READ | PROT_WRITE, MAP_SHARED,
		       fb->fd, 0);
	if (fb->map == MAP_FAILED) {
		printf("fb: mmap %zu bytes: %s\n", fb->map_len, strerror(errno));
		fb->map = NULL;
		return -1;
	}
	fb->surf.base = fb->map;
	fb->surf.xres = fb->var.xres;
	fb->surf.yres = fb->var.yres;
	fb->surf.line_length = fb->fix.line_length;
	fb->surf.bpp = bpp;
	fb->surf.var = &fb->var;
	return 0;
}

static int fb_commit(struct fbdev *fb)
{
	fb->var.xoffset = 0;
	fb->var.yoffset = 0;
	fb->var.activate = FB_ACTIVATE_VBL;
	if (ioctl(fb->fd, FBIOPAN_DISPLAY, &fb->var) < 0) {
		printf("fb: FBIOPAN_DISPLAY: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

static void print_panel_sysfs(void)
{
	static const char *const keys[] = {
		"panel_name", "panel_supplier", "panel_ver", "msm_fb_panel_status",
		"msm_fb_type", "modes", "bits_per_pixel", "virtual_size", "stride",
		NULL
	};
	char path[128], buf[128];
	int i;

	for (i = 0; keys[i]; i++) {
		snprintf(path, sizeof(path), FB_SYSFS "/%s", keys[i]);
		if (read_sysfs(path, buf, sizeof(buf)) == 0)
			printf("panel: %s=%s\n", keys[i], buf);
	}
	if (read_sysfs(BL_PATH, buf, sizeof(buf)) == 0) {
		char max[32] = "?";
		read_sysfs(BL_MAX_PATH, max, sizeof(max));
		printf("backlight: %s/%s\n", buf, max);
	}
}

/* ------------------------------------------------------------- commands */

static int cmd_info(const char *fbpath, const char *inpath)
{
	struct touch_dev td;
	int rc;

	(void)fbpath;
	/* Deliberately never opens /dev/fb0: the first open unblanks and the
	 * last close powers the panel down (see the header), so an "info"
	 * command would kill the bootloader splash. Geometry comes from
	 * sysfs instead. */
	print_panel_sysfs();
	rc = touch_open(&td, inpath);
	if (rc) {
		printf("touch: no touch input device found (%s)\n", strerror(-rc));
		return 1;
	}
	touch_print(&td);
	close(td.fd);
	return 0;
}

/*
 * Panel off and on again through the fb core (mdss_fb_blank + the fb
 * notifier chain), the way Android's screen-off/on does it: the fd stays
 * open, so this is the FB_BLANK_POWERDOWN -> UNBLANK path, not the
 * last-close power-down. The touch driver suspends and resumes on the
 * same events.
 */
static int blank_cycle(struct fbdev *fb, double t0)
{
	double t;

	printf("cycle: FBIOBLANK POWERDOWN at +%.3f s\n", now_s() - t0);
	kmsg("blank cycle: powerdown");
	if (ioctl(fb->fd, FBIOBLANK, FB_BLANK_POWERDOWN) < 0) {
		printf("cycle: FBIOBLANK POWERDOWN: %s\n", strerror(errno));
		return -1;
	}
	t = now_s();
	printf("cycle: panel off (%.3f s), holding 3 s\n", t - t0);
	sleep(3);
	kmsg("blank cycle: unblank");
	if (ioctl(fb->fd, FBIOBLANK, FB_BLANK_UNBLANK) < 0) {
		printf("cycle: FBIOBLANK UNBLANK: %s\n", strerror(errno));
		return -1;
	}
	printf("cycle: unblanked in %.3f s\n", now_s() - t);
	draw_pattern(&fb->surf);
	if (fb_commit(fb))
		return -1;
	printf("cycle: frame recommitted at +%.3f s\n", now_s() - t0);
	kmsg("blank cycle: frame recommitted");
	return 0;
}

static const uint8_t slot_rgb[8][3] = {
	{ 255, 0, 0 }, { 0, 160, 255 }, { 0, 200, 0 }, { 255, 160, 0 },
	{ 200, 0, 255 }, { 0, 220, 220 }, { 255, 90, 140 }, { 255, 255, 255 },
};

static int cmd_show(const char *fbpath, const char *inpath, int secs,
		    int bl, int cycle_at, bool require_touch, bool paint)
{
	struct fbdev fb;
	struct touch_dev td;
	struct mt_tracker trk;
	struct mt_event evs[MT_MAX_SLOTS];
	bool have_touch;
	unsigned long contacts = 0, reports = 0, frames = 0;
	int xmin = INT32_MAX, xmax = INT32_MIN, ymin = INT32_MAX, ymax = INT32_MIN;
	double t0, t_end, last_commit = 0, t_touch_first = -1, t_cycle = 0;
	int rc;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	print_panel_sysfs();

	t0 = now_s();
	kmsg("show: opening %s", fbpath);
	if (fb_open_probe(&fb, fbpath))
		return 1;
	fb_print(&fb);
	printf("fb: opened in %.3f s\n", now_s() - t0);

	/* Through the fb core so FB_EVENT_BLANK reaches the touch driver's
	 * notifier (mdss_fb_open's own unblank bypasses the notifier chain). */
	if (ioctl(fb.fd, FBIOBLANK, FB_BLANK_UNBLANK) < 0)
		printf("fb: FBIOBLANK UNBLANK: %s (continuing)\n", strerror(errno));
	else
		printf("fb: unblanked in %.3f s\n", now_s() - t0);

	if (fb_map(&fb))
		return 1;
	printf("fb: mapped %zu bytes\n", fb.map_len);
	draw_pattern(&fb.surf);
	if (fb_commit(&fb))
		return 1;
	frames++;
	last_commit = now_s();
	printf("fb: first frame committed at +%.3f s\n", last_commit - t0);
	kmsg("first frame committed (%ux%u %u bpp)", fb.var.xres, fb.var.yres,
	     fb.var.bits_per_pixel);
	set_backlight(bl);
	/* quirk 1: the level is applied by the next commit */
	if (fb_commit(&fb) == 0)
		frames++;
	last_commit = now_s();

	rc = touch_open(&td, inpath);
	have_touch = rc == 0;
	if (!have_touch)
		printf("touch: no touch input device found (%s)\n", strerror(-rc));
	else {
		touch_print(&td);
		mt_tracker_init(&trk, td.mt);
	}

	t_end = secs > 0 ? now_s() + secs : 0;
	if (cycle_at > 0)
		t_cycle = t0 + cycle_at;
	printf("running for %s; touch the screen now\n",
	       secs > 0 ? "the requested window" : "ever (SIGTERM to stop)");
	while (!stop_requested) {
		struct pollfd pfd = { .fd = have_touch ? td.fd : -1, .events = POLLIN };
		struct input_event ev[32];
		ssize_t n;
		int timeout_ms = 250, i, k;
		bool dirty = false;

		if (t_cycle && now_s() >= t_cycle) {
			t_cycle = 0;
			if (blank_cycle(&fb, t0) == 0) {
				frames++;
				set_backlight(bl);
				if (fb_commit(&fb) == 0) /* quirk 1 */
					frames++;
				last_commit = now_s();
			}
		}
		if (t_end) {
			double left = t_end - now_s();
			if (left <= 0)
				break;
			if (left * 1000 < timeout_ms)
				timeout_ms = (int)(left * 1000) + 1;
		}
		if (poll(&pfd, 1, timeout_ms) <= 0) {
			/* idle heartbeat (quirk 1): lets a backlight write made
			 * from another shell take effect within a second */
			if (now_s() - last_commit >= 1.0) {
				if (fb_commit(&fb) == 0)
					frames++;
				last_commit = now_s();
			}
			continue;
		}
		if (!(pfd.revents & POLLIN))
			continue;
		n = read(td.fd, ev, sizeof(ev));
		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			printf("touch: read: %s\n", strerror(errno));
			break;
		}
		for (i = 0; i < (int)(n / sizeof(ev[0])); i++) {
			int cnt = mt_tracker_feed(&trk, &ev[i], evs);
			if (cnt)
				reports++;
			for (k = 0; k < cnt; k++) {
				struct mt_event *e = &evs[k];
				int sx = abs_to_screen(e->x, td.ax.minimum, td.ax.maximum, fb.var.xres);
				int sy = abs_to_screen(e->y, td.ay.minimum, td.ay.maximum, fb.var.yres);
				const char *what = e->action == MT_DOWN ? "down"
						 : e->action == MT_UP ? "up" : "move";

				printf("touch: +%.3f slot=%d id=%d %s raw=(%d,%d) screen=(%d,%d)\n",
				       now_s() - t0, e->slot, e->id, what, e->x, e->y, sx, sy);
				if (e->action == MT_DOWN) {
					contacts++;
					if (t_touch_first < 0) {
						t_touch_first = now_s() - t0;
						kmsg("first touch contact at +%.3f s raw=(%d,%d)",
						     t_touch_first, e->x, e->y);
					}
				}
				if (e->action != MT_UP) {
					if (sx < xmin) xmin = sx;
					if (sx > xmax) xmax = sx;
					if (sy < ymin) ymin = sy;
					if (sy > ymax) ymax = sy;
					if (paint) {
						const uint8_t *c = slot_rgb[e->slot & 7];
						fill_circle(&fb.surf, sx, sy, 22,
							    pack_pixel(&fb.var, c[0], c[1], c[2]));
						dirty = true;
					}
				}
			}
		}
		/* repaint at most ~60 Hz; the pan blocks until the frame is done */
		if (dirty && now_s() - last_commit > 0.016) {
			if (fb_commit(&fb) == 0)
				frames++;
			last_commit = now_s();
		}
	}

	printf("summary: frames=%lu touch_reports=%lu contacts=%lu%s\n",
	       frames, reports, contacts, stop_requested ? " (stopped by signal)" : "");
	if (contacts)
		printf("summary: touched screen area x=[%d..%d] y=[%d..%d], first contact +%.3f s\n",
		       xmin, xmax, ymin, ymax, t_touch_first);
	kmsg("done: frames=%lu contacts=%lu", frames, contacts);
	if (have_touch)
		close(td.fd);
	if (fb.map)
		munmap(fb.map, fb.map_len);
	/* quirk 2: panel off through the fb core first, so the touch driver
	 * suspends along with the IC; the close below then finds the panel
	 * already off. */
	kmsg("blanking before close");
	if (ioctl(fb.fd, FBIOBLANK, FB_BLANK_POWERDOWN) < 0)
		printf("fb: FBIOBLANK POWERDOWN before close: %s\n", strerror(errno));
	else
		printf("fb: blanked before close\n");
	close(fb.fd); /* last close: mdss_fb_release_all() */
	if (require_touch && contacts == 0)
		return 2;
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: fbtouch info [-f fbdev] [-i eventdev]\n"
		"       fbtouch show [-t secs] [-b level] [-c secs] [-f fbdev] [-i eventdev] [-r] [-q]\n"
		"       fbtouch bl <0-255>\n");
}

int main(int argc, char **argv)
{
	const char *fbpath = "/dev/fb0", *inpath = NULL, *cmd;
	int secs = 30, bl = 128, cycle_at = 0, opt;
	bool require_touch = false, paint = true;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 2) {
		usage();
		return 64;
	}
	cmd = argv[1];
	optind = 2;
	while ((opt = getopt(argc, argv, "t:b:c:f:i:rq")) != -1) {
		switch (opt) {
		case 't': secs = atoi(optarg); break;
		case 'b': bl = atoi(optarg); break;
		case 'c': cycle_at = atoi(optarg); break;
		case 'f': fbpath = optarg; break;
		case 'i': inpath = optarg; break;
		case 'r': require_touch = true; break;
		case 'q': paint = false; break;
		default: usage(); return 64;
		}
	}
	if (!strcmp(cmd, "info"))
		return cmd_info(fbpath, inpath);
	if (!strcmp(cmd, "show"))
		return cmd_show(fbpath, inpath, secs, bl, cycle_at, require_touch, paint);
	if (!strcmp(cmd, "bl")) {
		if (optind >= argc) {
			usage();
			return 64;
		}
		return set_backlight(atoi(argv[optind])) ? 1 : 0;
	}
	usage();
	return 64;
}

#endif /* FBTOUCH_NO_MAIN */
