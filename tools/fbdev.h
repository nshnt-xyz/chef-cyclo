/*
 * fbdev.h - the chef-cyclo framebuffer contract, shared by every client of
 * /dev/fb0 (fbtouch, fblog, later the UI). Header-only, static functions,
 * no libc beyond ioctl/mmap/flock so it links freestanding-ish under musl.
 *
 * The 4.4 MDSS driver (mdss_fb.c) has two live-verified quirks (README
 * gotchas, fbtouch.c header) that make the order of operations binding:
 *
 *   open           -> first open unblanks the panel, last close powers it
 *                     down and zeroes the backlight (mdss_fb_release_all)
 *   FBIOBLANK UNBLANK after open, through the fb core, so the NT36xxx
 *                     touch driver's fb notifier resumes the IC
 *   mmap, draw, FBIOPAN_DISPLAY (commit); stay resident
 *   backlight      -> a lcd-backlight write reaches the WLED only at the
 *                     next commit: write, then commit; heartbeat-commit
 *                     while idle so an external `fbtouch bl N` lands
 *                     (with fblog resident it lands in ~30 ms, not at the
 *                     heartbeat: fbtouch bl's own kmsg note is a new log
 *                     line, and fblog's repaint of it is the commit --
 *                     live-verified 2026-09-18)
 *   FBIOBLANK POWERDOWN before the last close, so the touch driver
 *                     suspends with the panel (the last-close power-down
 *                     bypasses the notifier and leaves it un-suspended)
 *
 * Screen lock: several processes may hold fb0 open (the driver refcounts
 * opens), so two clients drawing into the same buffer is the real hazard.
 * The convention here is an advisory flock(2) on FB_LOCK_PATH:
 *   - a resident background client (fblog) takes LOCK_SH|LOCK_NB only for
 *     the milliseconds it draws and commits, never while idle, and treats
 *     a failed try as "someone borrowed the screen": it stops drawing,
 *     drops its mapping and retries every FB_LOCK_RETRY_MS;
 *   - a foreground client (fbtouch show) takes LOCK_EX with
 *     fb_lock_exclusive() before opening fb0 and releases it after its
 *     POWERDOWN + close; the background client then resumes with an
 *     UNBLANK, a redraw and a backlight commit.
 * Locks die with their process, so a killed client never wedges the
 * screen. Nothing here touches any input device.
 */
#ifndef CHEF_CYCLO_FBDEV_H
#define CHEF_CYCLO_FBDEV_H

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/fb.h>

#define FB_LOCK_PATH     "/run/fb0.lock"
#define FB_LOCK_RETRY_MS 250

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

static inline uint32_t pack_pixel(const struct fb_var_screeninfo *var,
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

static inline void put_pixel(struct surface *s, int x, int y, uint32_t px)
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

static inline void fill_rect(struct surface *s, int x0, int y0, int w, int h, uint32_t px)
{
	int x, y;

	for (y = y0; y < y0 + h; y++)
		for (x = x0; x < x0 + w; x++)
			put_pixel(s, x, y, px);
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

/* open + read the screeninfo; -errno on failure (fd is closed again). */
static inline int fb_open_probe(struct fbdev *fb, const char *path)
{
	memset(fb, 0, sizeof(*fb));
	fb->fd = open(path, O_RDWR | O_CLOEXEC);
	if (fb->fd < 0)
		return -errno;
	if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->var) < 0 ||
	    ioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->fix) < 0) {
		int e = errno;
		close(fb->fd);
		fb->fd = -1;
		return -e;
	}
	return 0;
}

/* Through the fb core, so FB_EVENT_BLANK reaches the touch driver. */
static inline int fb_unblank(struct fbdev *fb)
{
	return ioctl(fb->fd, FBIOBLANK, FB_BLANK_UNBLANK) < 0 ? -errno : 0;
}

static inline int fb_powerdown(struct fbdev *fb)
{
	return ioctl(fb->fd, FBIOBLANK, FB_BLANK_POWERDOWN) < 0 ? -errno : 0;
}

/* mmap one page set and fill in surf; -EINVAL for an unsupported depth. */
static inline int fb_map(struct fbdev *fb)
{
	uint32_t bpp = fb->var.bits_per_pixel / 8;

	if (bpp != 2 && bpp != 4)
		return -EINVAL;
	/* one visible page is enough; the driver allocates the whole
	 * virtual size on the first mmap anyway */
	fb->map_len = (size_t)fb->fix.line_length * fb->var.yres_virtual;
	if (fb->map_len == 0)
		fb->map_len = (size_t)fb->fix.line_length * fb->var.yres;
	fb->map = mmap(NULL, fb->map_len, PROT_READ | PROT_WRITE, MAP_SHARED,
		       fb->fd, 0);
	if (fb->map == MAP_FAILED) {
		fb->map = NULL;
		return -errno;
	}
	fb->surf.base = fb->map;
	fb->surf.xres = fb->var.xres;
	fb->surf.yres = fb->var.yres;
	fb->surf.line_length = fb->fix.line_length;
	fb->surf.bpp = bpp;
	fb->surf.var = &fb->var;
	return 0;
}

static inline void fb_unmap(struct fbdev *fb)
{
	if (fb->map)
		munmap(fb->map, fb->map_len);
	fb->map = NULL;
	fb->surf.base = NULL;
}

/* Commit the visible page: mdss_fb_pan_display() blocks until the frame
 * is out, and applies any pending backlight level (quirk 1). */
static inline int fb_commit(struct fbdev *fb)
{
	fb->var.xoffset = 0;
	fb->var.yoffset = 0;
	fb->var.activate = FB_ACTIVATE_VBL;
	return ioctl(fb->fd, FBIOPAN_DISPLAY, &fb->var) < 0 ? -errno : 0;
}

/* quirk 2: POWERDOWN through the fb core, then close. Safe to call with a
 * fd that another client also holds: the panel goes off through the
 * notifier chain either way and the last closer's release finds it off. */
static inline void fb_powerdown_close(struct fbdev *fb)
{
	if (fb->fd < 0)
		return;
	fb_unmap(fb);
	(void)fb_powerdown(fb);
	close(fb->fd);
	fb->fd = -1;
}

/* ---------------------------------------------------------------- lock */

/* Open (creating) the lock file; -errno on failure. The fd is the lock. */
static inline int fb_lock_open(const char *path)
{
	int fd = open(path ? path : FB_LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	return fd < 0 ? -errno : fd;
}

/* Non-blocking shared try: 0 = held (call fb_lock_release when done),
 * -EWOULDBLOCK = a foreground client owns the screen, other -errno. */
static inline int fb_lock_try_shared(int lockfd)
{
	return flock(lockfd, LOCK_SH | LOCK_NB) < 0 ? -errno : 0;
}

/* Exclusive ownership for a foreground client, polling every 20 ms for up
 * to timeout_ms (the background client only holds its share for
 * milliseconds at a time, so this is normally immediate). */
static inline int fb_lock_exclusive(int lockfd, int timeout_ms)
{
	int waited = 0;

	for (;;) {
		if (flock(lockfd, LOCK_EX | LOCK_NB) == 0)
			return 0;
		if (errno != EWOULDBLOCK)
			return -errno;
		if (waited >= timeout_ms)
			return -ETIMEDOUT;
		{
			struct timespec ts = { 0, 20 * 1000 * 1000 };
			nanosleep(&ts, NULL);
		}
		waited += 20;
	}
}

static inline void fb_lock_release(int lockfd)
{
	(void)flock(lockfd, LOCK_UN);
}

#endif /* CHEF_CYCLO_FBDEV_H */
