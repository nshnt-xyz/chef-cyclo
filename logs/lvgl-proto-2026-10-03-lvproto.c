/*
 * lvproto - research prototype: LVGL 9.6 on the chef MDSS fbdev, following
 * tools/fbdev.h's contract. Not production code.
 *
 * Questions it answers live:
 *  Q1 DIRECT-mode double buffering into the two fb pages + FBIOPAN_DISPLAY
 *     yoffset page flips (timing, tearing)
 *  Q2 in-place R/B swizzle of LVGL's XRGB8888 output into MDSS RGBA8888
 *     (cost per dirty pixel)
 *  Q3 lv_evdev touch (discovered by caps, scaled from ABS ranges)
 *  Q4 screen off/on via FBIOBLANK with touch suspend/resume, backlight commit
 *  Q5 fblog handoff via /run/fb0.lock
 *  Q6 idle wakeups / CPU, full-screen redraw fps
 *
 * usage: lvproto [-t secs] [-b bright] [-a] [-F] [-n]
 *   -a  animation (spinner + moving bar)   -F  full-screen invalidate per frame
 *   -n  don't claim power.short from buttond
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <linux/input.h>

#include "fbdev.h"
#include "lvgl.h"

#define BL_PATH "/sys/class/leds/lcd-backlight/brightness"

static struct fbdev fb;
static int lockfd = -1;
static lv_display_t *disp;
static lv_indev_t *touch;
static int touch_fd = -1;
static int btn_fd = -1;
static volatile sig_atomic_t quit, toggle_req;
static double cpu_last;
static int screen_on = 1;
static int brightness = 96;
static int full_inval;
static lv_obj_t *lbl_clock, *lbl_stats, *lbl_touch, *lbl_btn, *bar_obj, *lbl_gest;

/* stats, reset every second */
static unsigned st_frames, st_flushes;
static double st_swz_ms, st_pan_ms, st_handler_ms, st_max_pan_ms;
static unsigned long st_swz_px;
static unsigned st_touch_reads, st_wakeups;

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static uint32_t tick_cb(void)
{
	return (uint32_t)now_ms();
}

static void on_sig(int s)
{
	if (s == SIGUSR1)
		toggle_req = 1;
	else
		quit = 1;
}

static double cpu_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int write_brightness(int v)
{
	char b[16];
	int fd = open(BL_PATH, O_WRONLY | O_CLOEXEC), n, r;
	if (fd < 0)
		return -errno;
	n = snprintf(b, sizeof b, "%d\n", v);
	r = write(fd, b, n) == n ? 0 : -errno;
	close(fd);
	return r;
}

/* ------------------------------------------------------------- display */

static uint8_t *page_base(int page)
{
	return fb.map + (size_t)page * fb.fix.line_length * fb.var.yres;
}

static int pan_to(int page)
{
	fb.var.xoffset = 0;
	fb.var.yoffset = page * fb.var.yres;
	fb.var.activate = FB_ACTIVATE_VBL;
	return ioctl(fb.fd, FBIOPAN_DISPLAY, &fb.var) < 0 ? -errno : 0;
}

/*
 * LVGL renders into a cached RAM shadow (XRGB8888 = B,G,R,X in memory). The
 * MDSS mapping is write-combined: reads are ~100 ns/px, so never read it.
 * Each frame: copy+swizzle (RGBA for MDSS) the dirty areas of this frame and
 * of the previous one into the back page, then pan to it.
 */
#define MAX_AREAS 32
static uint8_t *shadow;
static uint32_t shadow_stride;
static lv_area_t cur[MAX_AREAS], prev[MAX_AREAS];
static int ncur, nprev, cur_full, prev_full;
static int back_page = 1;

static void copy_swizzle(uint8_t *dst_base, const lv_area_t *a)
{
	int32_t y, x, w = lv_area_get_width(a);

	for (y = a->y1; y <= a->y2; y++) {
		const uint32_t *s = (const uint32_t *)(shadow + (size_t)y * shadow_stride) + a->x1;
		uint32_t *d = (uint32_t *)(dst_base + (size_t)y * fb.fix.line_length) + a->x1;
		for (x = 0; x < w; x++) {
			uint32_t v = s[x];
			d[x] = 0xff000000u | (v & 0x0000ff00u) |
			       ((v >> 16) & 0xffu) | ((v & 0xffu) << 16);
		}
	}
	st_swz_px += (unsigned long)w * lv_area_get_height(a);
}

static void flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px)
{
	(void)px;
	if (ncur < MAX_AREAS)
		cur[ncur++] = *area;
	else
		cur_full = 1;
	st_flushes++;
	if (lv_display_flush_is_last(d)) {
		lv_area_t full = { 0, 0, fb.var.xres - 1, fb.var.yres - 1 };
		uint8_t *dst = page_base(back_page);
		double t0 = now_ms(), t1;
		int i, r;

		for (i = 0; i < ncur; i++)
			if (lv_area_get_size(&cur[i]) == lv_area_get_size(&full))
				cur_full = 1;
		if (cur_full || prev_full) {
			copy_swizzle(dst, &full);
		} else {
			for (i = 0; i < nprev; i++)
				copy_swizzle(dst, &prev[i]);
			for (i = 0; i < ncur; i++)
				copy_swizzle(dst, &cur[i]);
		}
		t1 = now_ms();
		st_swz_ms += t1 - t0;
		r = pan_to(back_page);
		if (r)
			fprintf(stderr, "pan page %d: %s\n", back_page, strerror(-r));
		t0 = now_ms() - t1;
		st_pan_ms += t0;
		if (t0 > st_max_pan_ms)
			st_max_pan_ms = t0;
		st_frames++;
		back_page ^= 1;
		memcpy(prev, cur, sizeof(lv_area_t) * ncur);
		nprev = ncur;
		prev_full = cur_full;
		ncur = 0;
		cur_full = 0;
	}
	lv_display_flush_ready(d);
}

static int display_init(void)
{
	int r;

	lockfd = fb_lock_open(NULL);
	if (lockfd < 0)
		return lockfd;
	r = fb_lock_exclusive(lockfd, 10000);
	if (r) {
		fprintf(stderr, "lock: %s\n", strerror(-r));
		return r;
	}
	r = fb_open_probe(&fb, "/dev/fb0");
	if (r)
		return r;
	fprintf(stderr, "fb: %ux%u virt %ux%u bpp %u stride %u smem %u R%u G%u B%u A%u/%u\n",
		fb.var.xres, fb.var.yres, fb.var.xres_virtual, fb.var.yres_virtual,
		fb.var.bits_per_pixel, fb.fix.line_length, fb.fix.smem_len,
		fb.var.red.offset, fb.var.green.offset, fb.var.blue.offset,
		fb.var.transp.offset, fb.var.transp.length);
	if (fb.var.bits_per_pixel != 32 || fb.var.red.offset != 0 ||
	    fb.var.blue.offset != 16 || fb.var.yres_virtual < 2 * fb.var.yres) {
		fprintf(stderr, "unexpected fb layout\n");
		return -EINVAL;
	}
	r = fb_unblank(&fb);
	if (r)
		fprintf(stderr, "unblank: %s\n", strerror(-r));
	r = fb_map(&fb);
	if (r)
		return r;

	disp = lv_display_create(fb.var.xres, fb.var.yres);
	lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
	shadow_stride = fb.var.xres * 4;
	shadow = aligned_alloc(64, (size_t)shadow_stride * fb.var.yres);
	if (!shadow)
		return -ENOMEM;
	lv_display_set_buffers_with_stride(disp, shadow, NULL,
		shadow_stride * fb.var.yres, shadow_stride,
		LV_DISPLAY_RENDER_MODE_DIRECT);
	lv_display_set_flush_cb(disp, flush_cb);
	write_brightness(brightness);
	return 0;
}

static void screen_set(int on)
{
	double t0 = now_ms();

	if (on == screen_on)
		return;
	if (!on) {
		int r = fb_powerdown(&fb);
		fprintf(stderr, "screen off: %s (%.1f ms)\n", r ? strerror(-r) : "ok", now_ms() - t0);
	} else {
		int r = fb_unblank(&fb);
		write_brightness(brightness);
		lv_obj_invalidate(lv_screen_active());
		prev_full = 1;	/* the pages may hold another client's frame */
		fprintf(stderr, "screen on: %s (%.1f ms)\n", r ? strerror(-r) : "ok", now_ms() - t0);
	}
	screen_on = on;
}

static void display_close(void)
{
	if (fb.fd >= 0) {
		pan_to(0);
		fb_powerdown_close(&fb);
	}
	if (lockfd >= 0)
		close(lockfd);
}

/* --------------------------------------------------------------- input */

#define BIT(arr, b) ((arr)[(b) / 8] & (1u << ((b) % 8)))

static int find_touch(char *path, size_t n)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	int found = 0;

	if (!d)
		return 0;
	while (!found && (e = readdir(d))) {
		uint8_t props[INPUT_PROP_CNT / 8 + 1] = {0}, abs[ABS_CNT / 8 + 1] = {0};
		char p[64];
		int fd;
		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
		fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGPROP(sizeof props), props) >= 0 &&
		    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs) >= 0 &&
		    BIT(props, INPUT_PROP_DIRECT) && BIT(abs, ABS_MT_POSITION_X)) {
			snprintf(path, n, "%s", p);
			found = 1;
		}
		close(fd);
	}
	closedir(d);
	return found;
}

static void touch_log_cb(lv_event_t *e)
{
	lv_point_t p;
	lv_indev_get_point(touch, &p);
	fprintf(stderr, "[%.3f] touch %s %d,%d\n", now_ms() / 1e3,
		lv_event_get_code(e) == LV_EVENT_PRESSED ? "down" : "up", (int)p.x, (int)p.y);
}

static void input_init(void)
{
	char path[64];

	if (!find_touch(path, sizeof path)) {
		fprintf(stderr, "no touchscreen found\n");
		return;
	}
	touch_fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (touch_fd < 0) {
		perror(path);
		return;
	}
	touch = lv_evdev_create_fd(LV_INDEV_TYPE_POINTER, touch_fd);
	lv_indev_set_mode(touch, LV_INDEV_MODE_EVENT);
	{
		/* lv_evdev only reads ABS_X/ABS_Y; NT36525 advertises MT axes only */
		struct input_absinfo ax = {0}, ay = {0};
		ioctl(touch_fd, EVIOCGABS(ABS_MT_POSITION_X), &ax);
		ioctl(touch_fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay);
		lv_evdev_set_calibration(touch, ax.minimum, ay.minimum, ax.maximum, ay.maximum);
		fprintf(stderr, "touch calib x %d..%d y %d..%d\n", ax.minimum, ax.maximum, ay.minimum, ay.maximum);
	}
	lv_indev_add_event_cb(touch, touch_log_cb, LV_EVENT_PRESSED, NULL);
	lv_indev_add_event_cb(touch, touch_log_cb, LV_EVENT_RELEASED, NULL);
	fprintf(stderr, "touch: %s fd %d\n", path, touch_fd);
}

static void buttond_init(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	const char *m = "claim power.short\n";

	strcpy(sa.sun_path, "/run/buttond.sock");
	btn_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (btn_fd < 0 || connect(btn_fd, (void *)&sa, sizeof sa) < 0 ||
	    write(btn_fd, m, strlen(m)) != (ssize_t)strlen(m)) {
		perror("buttond");
		if (btn_fd >= 0)
			close(btn_fd);
		btn_fd = -1;
	}
}

static void buttond_read(void)
{
	char buf[256];
	ssize_t n = read(btn_fd, buf, sizeof buf - 1);

	if (n <= 0) {
		fprintf(stderr, "buttond closed\n");
		close(btn_fd);
		btn_fd = -1;
		return;
	}
	buf[n] = 0;
	fprintf(stderr, "buttond: %s", buf);
	if (strstr(buf, "event power.short")) {
		screen_set(!screen_on);
		if (lbl_btn)
			lv_label_set_text_fmt(lbl_btn, "power.short -> screen %s", screen_on ? "on" : "off");
	}
}

/* ------------------------------------------------------------------ UI */

static void bright_cb(lv_event_t *e)
{
	lv_obj_t *s = lv_event_get_target(e);
	brightness = lv_slider_get_value(s);
	fprintf(stderr, "[%.3f] brightness %d\n", now_ms() / 1e3, brightness);
	write_brightness(brightness);	/* lands at the commit this redraw causes */
}

static void btn_cb(lv_event_t *e)
{
	static int n;
	lv_obj_t *l = lv_event_get_user_data(e);
	lv_label_set_text_fmt(l, "tapped %d", ++n);
	fprintf(stderr, "[%.3f] button tapped %d\n", now_ms() / 1e3, n);
}

static void anim_x(void *o, int32_t v)
{
	lv_obj_set_x(o, v);
}

static void ui_build(int anim)
{
	lv_obj_t *scr = lv_screen_active(), *o, *l;

	lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
	lv_obj_set_style_text_color(scr, lv_color_white(), 0);

	lbl_clock = lv_label_create(scr);
	lv_obj_set_style_text_font(lbl_clock, &lv_font_montserrat_48, 0);
	lv_obj_align(lbl_clock, LV_ALIGN_TOP_MID, 0, 60);

	/* colour check: R, G, B swatches must show red, green, blue */
	for (int i = 0; i < 3; i++) {
		static const uint32_t c[] = {0xff0000, 0x00ff00, 0x0000ff};
		o = lv_obj_create(scr);
		lv_obj_set_size(o, 300, 120);
		lv_obj_set_style_bg_color(o, lv_color_hex(c[i]), 0);
		lv_obj_set_style_border_width(o, 0, 0);
		lv_obj_align(o, LV_ALIGN_TOP_LEFT, 30 + i * 345, 180);
	}

	o = lv_button_create(scr);
	lv_obj_set_size(o, 600, 200);
	lv_obj_align(o, LV_ALIGN_TOP_MID, 0, 380);
	l = lv_label_create(o);
	lv_obj_set_style_text_font(l, &lv_font_montserrat_48, 0);
	lv_label_set_text(l, "TAP ME");
	lv_obj_center(l);
	lv_obj_add_event_cb(o, btn_cb, LV_EVENT_CLICKED, l);

	o = lv_slider_create(scr);
	lv_obj_set_size(o, 900, 60);
	lv_slider_set_range(o, 5, 255);
	lv_slider_set_value(o, brightness, LV_ANIM_OFF);
	lv_obj_align(o, LV_ALIGN_TOP_MID, 0, 680);
	lv_obj_add_event_cb(o, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);

	lbl_touch = lv_label_create(scr);
	lv_obj_set_style_text_font(lbl_touch, &lv_font_montserrat_28, 0);
	lv_obj_align(lbl_touch, LV_ALIGN_TOP_LEFT, 30, 800);
	lv_label_set_text(lbl_touch, "touch: -");

	lbl_gest = lv_label_create(scr);
	lv_obj_set_style_text_font(lbl_gest, &lv_font_montserrat_28, 0);
	lv_obj_align(lbl_gest, LV_ALIGN_TOP_LEFT, 30, 860);
	lv_label_set_text(lbl_gest, "gesture: -");
	/* gesture recognition off: lv_evdev zeroes MT slots */

	lbl_btn = lv_label_create(scr);
	lv_obj_set_style_text_font(lbl_btn, &lv_font_montserrat_28, 0);
	lv_obj_align(lbl_btn, LV_ALIGN_TOP_LEFT, 30, 920);
	lv_label_set_text(lbl_btn, "power.short: -");

	lbl_stats = lv_label_create(scr);
	lv_obj_set_style_text_font(lbl_stats, &lv_font_montserrat_28, 0);
	lv_obj_align(lbl_stats, LV_ALIGN_BOTTOM_LEFT, 30, -40);
	lv_label_set_text(lbl_stats, "stats: -");

	/* corner targets for the touch mapping check */
	for (int i = 0; i < 4; i++) {
		o = lv_obj_create(scr);
		lv_obj_set_size(o, 90, 90);
		lv_obj_set_style_bg_color(o, lv_color_hex(0xffff00), 0);
		lv_obj_set_style_radius(o, 45, 0);
		lv_obj_align(o, (lv_align_t[]){LV_ALIGN_TOP_LEFT, LV_ALIGN_TOP_RIGHT,
				 LV_ALIGN_BOTTOM_LEFT, LV_ALIGN_BOTTOM_RIGHT}[i], 0, 0);
		lv_obj_set_flag(o, LV_OBJ_FLAG_CLICKABLE, false);
	}

	if (anim) {
		o = lv_spinner_create(scr);
		lv_obj_set_size(o, 300, 300);
		lv_obj_align(o, LV_ALIGN_CENTER, 0, 350);
		bar_obj = lv_obj_create(scr);
		lv_obj_set_size(bar_obj, 200, 120);
		lv_obj_set_style_bg_color(bar_obj, lv_color_hex(0x00c0ff), 0);
		lv_obj_align(bar_obj, LV_ALIGN_BOTTOM_LEFT, 0, -200);
		lv_anim_t a;
		lv_anim_init(&a);
		lv_anim_set_var(&a, bar_obj);
		lv_anim_set_exec_cb(&a, anim_x);
		lv_anim_set_values(&a, 0, 880);
		lv_anim_set_duration(&a, 1500);
		lv_anim_set_playback_duration(&a, 1500);
		lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
		lv_anim_start(&a);
	}
}

static void stats_tick(lv_timer_t *t)
{
	(void)t;
	time_t now = time(NULL);
	struct tm tm;
	char line[256];
	lv_point_t p = {0, 0};

	localtime_r(&now, &tm);
	lv_label_set_text_fmt(lbl_clock, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
	snprintf(line, sizeof line,
		 "cpu %.1f%% | fps %u flush %u | swz %.2f ms %.1f Mpx | pan avg %.1f max %.1f ms | handler %.1f ms | wake %u touch %u",
		 (cpu_ms() - cpu_last) / 10.0, st_frames, st_flushes, st_swz_ms, st_swz_px / 1e6,
		 st_frames ? st_pan_ms / st_frames : 0.0, st_max_pan_ms, st_handler_ms,
		 st_wakeups, st_touch_reads);
	fprintf(stderr, "%s\n", line);
	lv_label_set_text(lbl_stats, line);
	if (touch) {
		lv_indev_get_point(touch, &p);
		lv_label_set_text_fmt(lbl_touch, "touch: %d,%d %s", (int)p.x, (int)p.y,
			lv_indev_get_state(touch) == LV_INDEV_STATE_PRESSED ? "DOWN" : "up");
	}
	cpu_last = cpu_ms();
	st_frames = st_flushes = 0;
	st_swz_ms = st_pan_ms = st_handler_ms = st_max_pan_ms = 0;
	st_swz_px = 0;
	st_touch_reads = st_wakeups = 0;
}

int main(int argc, char **argv)
{
	int opt, secs = 0, anim = 0, claim = 1, r;
	double t_end;

	while ((opt = getopt(argc, argv, "t:b:aFn")) != -1) {
		switch (opt) {
		case 't': secs = atoi(optarg); break;
		case 'b': brightness = atoi(optarg); break;
		case 'a': anim = 1; break;
		case 'F': full_inval = 1; break;
		case 'n': claim = 0; break;
		default: return 64;
		}
	}
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGHUP, on_sig);
	signal(SIGUSR1, on_sig);
	signal(SIGPIPE, SIG_IGN);

	lv_init();
	lv_tick_set_cb(tick_cb);
	r = display_init();
	if (r) {
		fprintf(stderr, "display init: %s\n", strerror(-r));
		display_close();
		return 1;
	}
	input_init();
	if (claim)
		buttond_init();
	ui_build(anim);
	lv_timer_create(stats_tick, 1000, NULL);
	fprintf(stderr, "first frame...\n");
	lv_refr_now(disp);
	fprintf(stderr, "first frame committed\n");
	t_end = secs ? now_ms() + secs * 1000.0 : 0;

	while (!quit && (!t_end || now_ms() < t_end)) {
		struct pollfd pf[2];
		int n = 0, ti = -1, tb = -1, timeout;
		uint32_t next;
		double t0;

		if (full_inval && screen_on)
			lv_obj_invalidate(lv_screen_active());
		t0 = now_ms();
		next = screen_on ? lv_timer_handler() : LV_NO_TIMER_READY;
		st_handler_ms += now_ms() - t0;

		if (touch_fd >= 0 && screen_on) {
			pf[n] = (struct pollfd){ touch_fd, POLLIN, 0 };
			ti = n++;
		}
		if (btn_fd >= 0) {
			pf[n] = (struct pollfd){ btn_fd, POLLIN, 0 };
			tb = n++;
		}
		timeout = next == LV_NO_TIMER_READY ? -1 : (int)next;
		/* keep reading while pressed: long-press, scroll and release need it */
		if (touch && screen_on && lv_indev_get_state(touch) == LV_INDEV_STATE_PRESSED &&
		    (timeout < 0 || timeout > 16))
			timeout = 16;
		if (full_inval && screen_on)
			timeout = 0;
		if (!screen_on)
			timeout = 1000;	/* only to notice -t expiry */
		if (poll(pf, n, timeout) < 0 && errno != EINTR)
			break;
		st_wakeups++;
		if (ti >= 0 && (pf[ti].revents & POLLIN)) {
			lv_indev_read(touch);
			st_touch_reads++;
		} else if (touch && screen_on && lv_indev_get_state(touch) == LV_INDEV_STATE_PRESSED) {
			lv_indev_read(touch);
		}
		if (toggle_req) {
			toggle_req = 0;
			screen_set(!screen_on);
		}
		if (tb >= 0 && (pf[tb].revents & (POLLIN | POLLHUP)))
			buttond_read();
	}
	fprintf(stderr, "exit: restoring\n");
	display_close();
	return 0;
}
