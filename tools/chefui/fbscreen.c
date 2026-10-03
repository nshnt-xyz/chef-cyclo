/* fbscreen.c - see fbscreen.h. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fbscreen.h"

static void nolog(const char *fmt, ...)
{
	(void)fmt;
}

void cu_fb_init(struct cu_fbscreen *s, cu_fb_log_fn log)
{
	memset(s, 0, sizeof(*s));
	s->p.fb = CU_FB_PATH;
	s->p.lock = FB_LOCK_PATH;
	s->p.flag = CU_FB_FLAG_PATH;
	s->p.backlight = CU_FB_BL_PATH;
	s->p.modes = CU_FB_MODES_PATH;
	s->log = log ? log : nolog;
	s->lockfd = -1;
	s->fb.fd = -1;
	s->brightness = 96;
}

int cu_fb_parse_mode(const char *text, uint32_t *xres, uint32_t *yres)
{
	const char *p = strchr(text, ':');
	char *end;
	unsigned long x, y;

	p = p ? p + 1 : text;
	x = strtoul(p, &end, 10);
	if (end == p || *end != 'x')
		return -EINVAL;
	p = end + 1;
	y = strtoul(p, &end, 10);
	if (end == p || x == 0 || y == 0 || x > 16384 || y > 16384)
		return -EINVAL;
	*xres = (uint32_t)x;
	*yres = (uint32_t)y;
	return 0;
}

static int read_mode(struct cu_fbscreen *s)
{
	char buf[128];
	ssize_t n;
	int fd = open(s->p.modes, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return -errno;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -EINVAL;
	buf[n] = '\0';
	return cu_fb_parse_mode(buf, &s->xres, &s->yres);
}

static bool flag_present(const struct cu_fbscreen *s)
{
	return access(s->p.flag, F_OK) == 0;
}

static int flag_create(struct cu_fbscreen *s)
{
	int fd = open(s->p.flag, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);

	if (fd < 0) {
		int e = errno;

		s->log("create %s: %s", s->p.flag, strerror(e));
		return -e;
	}
	close(fd);
	return 0;
}

static int flag_remove(struct cu_fbscreen *s)
{
	if (unlink(s->p.flag) == 0 || errno == ENOENT)
		return 0;
	s->log("remove %s: %s", s->p.flag, strerror(errno));
	return -errno;
}

int cu_fb_write_backlight(struct cu_fbscreen *s)
{
	char v[16];
	int n = snprintf(v, sizeof(v), "%d\n", s->brightness), r = 0;
	int fd = open(s->p.backlight, O_WRONLY | O_CLOEXEC);

	if (fd < 0)
		return -errno;
	if (write(fd, v, (size_t)n) != n)
		r = errno ? -errno : -EIO;
	close(fd);
	return r;
}

/* CHEFUI_LOCK_FD from a handoff: must be an open fd on the lock file that
 * already holds (so can re-take without waiting) the exclusive lock. */
static int adopt_lock(struct cu_fbscreen *s)
{
	const char *v = getenv(CU_FB_LOCK_ENV);
	struct stat a, b;
	char *end;
	long fd;

	if (!v)
		return -ENOENT;
	fd = strtol(v, &end, 10);
	unsetenv(CU_FB_LOCK_ENV);
	if (end == v || *end || fd < 0 || fd > 65535 ||
	    fstat((int)fd, &a) < 0 || stat(s->p.lock, &b) < 0 ||
	    a.st_dev != b.st_dev || a.st_ino != b.st_ino) {
		s->log("%s=%s is not the screen lock; taking it normally", CU_FB_LOCK_ENV, v);
		return -EBADF;
	}
	if (flock((int)fd, LOCK_EX | LOCK_NB) < 0) {
		s->log("%s=%s does not hold the lock (%s); taking it normally",
		       CU_FB_LOCK_ENV, v, strerror(errno));
		close((int)fd);
		return -EBADF;
	}
	(void)fcntl((int)fd, F_SETFD, FD_CLOEXEC);
	s->lockfd = (int)fd;
	s->lock_inherited = true;
	return 0;
}

static int take_lock(struct cu_fbscreen *s)
{
	int fd, r;

	if (adopt_lock(s) == 0) {
		s->log("lock inherited (fd %d)", s->lockfd);
		return 0;
	}
	fd = fb_lock_open(s->p.lock);
	if (fd < 0) {
		s->log("lock %s: %s", s->p.lock, strerror(-fd));
		return fd;
	}
	r = fb_lock_exclusive(fd, CU_FB_LOCK_WAIT_MS);
	if (r) {
		s->log("lock %s: %s (held by another screen client)", s->p.lock, strerror(-r));
		close(fd);
		return r;
	}
	s->lockfd = fd;
	s->lock_inherited = false;
	s->log("lock acquired");
	return 0;
}

int cu_fb_open(struct cu_fbscreen *s)
{
	const struct fb_var_screeninfo *v;
	int r = fb_open_probe(&s->fb, s->p.fb);

	if (r) {
		s->log("open %s: %s", s->p.fb, strerror(-r));
		s->fb.fd = -1;
		return r;
	}
	v = &s->fb.var;
	if (v->bits_per_pixel != 32 || v->red.offset != 0 || v->green.offset != 8 ||
	    v->blue.offset != 16 || v->yres_virtual < 2 * v->yres ||
	    s->fb.fix.line_length < v->xres * 4) {
		s->log("unsupported fb layout: %u bpp R%u G%u B%u, %ux%u virtual %ux%u stride %u "
		       "(need 32 bpp RGBA 0/8/16 and two pages; fb0's format is never changed)",
		       v->bits_per_pixel, v->red.offset, v->green.offset, v->blue.offset,
		       v->xres, v->yres, v->xres_virtual, v->yres_virtual, s->fb.fix.line_length);
		r = -EINVAL;
		goto fail;
	}
	if (s->xres && (s->xres != v->xres || s->yres != v->yres)) {
		s->log("fb0 is %ux%u but %s said %ux%u", v->xres, v->yres, s->p.modes,
		       s->xres, s->yres);
		r = -EINVAL;
		goto fail;
	}
	s->xres = v->xres;
	s->yres = v->yres;
	r = fb_unblank(&s->fb);
	if (r)
		s->log("FBIOBLANK UNBLANK: %s", strerror(-r));
	r = fb_map(&s->fb);
	if (r) {
		s->log("mmap %s: %s", s->p.fb, strerror(-r));
		goto fail;
	}
	s->front = 0;	/* unknown; fblog commits page 0 */
	return 0;
fail:
	fb_powerdown_close(&s->fb);	/* the open may have lit the panel */
	return r;
}

int cu_fb_start(struct cu_fbscreen *s, int brightness, bool *dark)
{
	int r;

	s->brightness = brightness < 1 ? 1 : brightness > 255 ? 255 : brightness;
	r = take_lock(s);
	if (r)
		return r;
	*dark = flag_present(s);
	if (*dark) {
		r = read_mode(s);
		if (r) {
			s->log("%s present but %s unreadable (%s): cannot size the screen without opening fb0",
			       s->p.flag, s->p.modes, strerror(-r));
			goto fail;
		}
		s->on = false;
		s->log("%s present: starting dark (%ux%u from %s)", s->p.flag, s->xres, s->yres,
		       s->p.modes);
		return 0;
	}
	r = cu_fb_open(s);
	if (r)
		goto fail;
	(void)cu_fb_write_backlight(s);	/* lands at the first frame's pan */
	s->on = true;
	return 0;
fail:
	close(s->lockfd);
	s->lockfd = -1;
	return r;
}

uint8_t *cu_fb_page(struct cu_fbscreen *s, int page)
{
	return s->fb.map + (size_t)page * s->fb.fix.line_length * s->fb.var.yres;
}

uint32_t cu_fb_stride(const struct cu_fbscreen *s)
{
	return s->fb.fix.line_length;
}

int cu_fb_pan(struct cu_fbscreen *s, uint32_t yoffset, int page)
{
	int r;

	if (s->fb.fd < 0)
		return -EBADF;
	r = fb_pan(&s->fb, yoffset);
	if (r == 0)
		s->front = page;
	return r;
}

int cu_fb_screen_off(struct cu_fbscreen *s)
{
	int r = 0;

	if (!s->on)
		return 0;
	if (s->fb.fd >= 0) {
		r = fb_powerdown(&s->fb);
		if (r)
			s->log("FBIOBLANK POWERDOWN: %s", strerror(-r));
	}
	s->on = false;
	(void)flag_create(s);
	return r;
}

int cu_fb_screen_on(struct cu_fbscreen *s)
{
	int r;

	if (s->on)
		return 0;
	(void)flag_remove(s);
	if (s->fb.fd < 0) {
		r = cu_fb_open(s);	/* first open: UNBLANK inside */
		if (r) {
			(void)flag_create(s);
			return r;
		}
	} else {
		r = fb_unblank(&s->fb);
		if (r)
			s->log("FBIOBLANK UNBLANK: %s", strerror(-r));
	}
	(void)cu_fb_write_backlight(s);
	s->on = true;
	return 0;
}

int cu_fb_brightness_set(struct cu_fbscreen *s, int level)
{
	int r;

	s->brightness = level < 1 ? 1 : level > 255 ? 255 : level;
	r = cu_fb_write_backlight(s);
	if (r)
		return r;
	if (s->on && s->fb.fd >= 0)
		return cu_fb_pan(s, (uint32_t)s->front * s->fb.var.yres, s->front);
	return 0;
}

void cu_fb_shutdown(struct cu_fbscreen *s)
{
	fb_powerdown_close(&s->fb);
	s->on = false;
	if (s->lockfd >= 0)
		close(s->lockfd);
	s->lockfd = -1;
}

int cu_fb_handoff_exec(struct cu_fbscreen *s, const char *path, char *const argv[], bool dark)
{
	bool was_on = s->on;
	char v[16];
	int e;

	if (s->lockfd < 0)
		return -EBADF;
	if (dark)
		(void)flag_create(s);
	else
		(void)flag_remove(s);
	fb_powerdown_close(&s->fb);
	s->on = false;
	snprintf(v, sizeof(v), "%d", s->lockfd);
	if (fcntl(s->lockfd, F_SETFD, 0) < 0 || setenv(CU_FB_LOCK_ENV, v, 1) < 0) {
		e = errno;
	} else {
		s->log("handoff to %s%s", path, dark ? " (dark)" : "");
		execv(path, argv);
		e = errno;
	}
	(void)fcntl(s->lockfd, F_SETFD, FD_CLOEXEC);
	unsetenv(CU_FB_LOCK_ENV);
	s->log("handoff to %s failed: %s", path, strerror(e));
	if (was_on) {
		if (cu_fb_screen_on(s) != 0)
			(void)flag_create(s);
	} else {
		(void)flag_create(s);
	}
	return -e;
}
