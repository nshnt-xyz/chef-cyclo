/*
 * fakeloop.c - see fakeloop.h.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "chefui.h"
#include "fakeloop.h"

#define MAXW 16

static struct {
	int fd;
	short events;
	chefui_fd_cb cb;
	void *user;
} W[MAXW];
static int nw;
static bool fake;
static uint32_t fake_now;
static char logs[64][320];
static int nlogs;

static uint32_t real_tick(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static uint32_t fake_tick(void)
{
	return fake_now;
}

void fakeloop_init(bool fake_time)
{
	fake = fake_time;
	fake_now = 1000;
	lv_init();
	lv_tick_set_cb(fake ? fake_tick : real_tick);
}

static void once(int max_wait);

void fakeloop_advance(uint32_t ms)
{
	/* in steps, so periodic timers fire as often as they would; ready
	 * fds are dispatched too (no waiting) */
	while (ms) {
		uint32_t step = ms > 10 ? 10 : ms;

		fake_now += step;
		ms -= step;
		once(0);
	}
}

int chefui_watch_fd(int fd, short events, chefui_fd_cb cb, void *user)
{
	for (int i = 0; i < nw; i++)
		if (W[i].fd == fd) {
			W[i].events = events;
			W[i].cb = cb;
			W[i].user = user;
			return 0;
		}
	if (nw == MAXW)
		return -ENOSPC;
	W[nw].fd = fd;
	W[nw].events = events;
	W[nw].cb = cb;
	W[nw].user = user;
	nw++;
	return 0;
}

int chefui_unwatch_fd(int fd)
{
	for (int i = 0; i < nw; i++)
		if (W[i].fd == fd) {
			W[i] = W[--nw];
			return 0;
		}
	return -ENOENT;
}

int fakeloop_watched(void)
{
	return nw;
}

void chefui_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(logs[nlogs % 64], sizeof(logs[0]), fmt, ap);
	va_end(ap);
	if (getenv("FAKELOOP_VERBOSE"))
		fprintf(stderr, "log: %s\n", logs[nlogs % 64]);
	nlogs++;
}

int fakeloop_log_count(const char *s)
{
	int n = 0;

	for (int i = 0; i < (nlogs < 64 ? nlogs : 64); i++)
		n += strstr(logs[i], s) != NULL;
	return n;
}

void fakeloop_log_clear(void)
{
	nlogs = 0;
}

static void once(int max_wait)
{
	struct pollfd p[MAXW];
	struct { int fd; chefui_fd_cb cb; void *user; } snap[MAXW];
	uint32_t next = lv_timer_handler();
	int n = nw, t = next == LV_NO_TIMER_READY ? max_wait : (int)next;

	if (t > max_wait || fake)
		t = max_wait;	/* fake time: LVGL's deadlines are not wall time */
	for (int i = 0; i < n; i++) {
		p[i] = (struct pollfd){ W[i].fd, W[i].events, 0 };
		snap[i].fd = W[i].fd;
		snap[i].cb = W[i].cb;
		snap[i].user = W[i].user;
	}
	if (poll(p, (nfds_t)n, t) <= 0)
		return;
	for (int i = 0; i < n; i++) {
		if (!p[i].revents)
			continue;
		for (int j = 0; j < nw; j++)
			if (W[j].fd == snap[i].fd && W[j].cb == snap[i].cb) {
				W[j].cb(snap[i].fd, p[i].revents, W[j].user);
				break;
			}
	}
}

bool fakeloop_run_until(bool (*cond)(void *), void *arg, int timeout_ms)
{
	uint32_t end = real_tick() + (uint32_t)timeout_ms;

	for (;;) {
		if (cond && cond(arg))
			return true;
		if ((int32_t)(real_tick() - end) >= 0)
			return cond ? cond(arg) : false;
		once(20);
	}
}

void fakeloop_run_for(int ms)
{
	fakeloop_run_until(NULL, NULL, ms);
}

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
	(void)a;
	(void)px;
	lv_display_flush_ready(d);
}

lv_display_t *fakeloop_display(int w, int h)
{
	lv_display_t *d = lv_display_create(w, h);
	size_t stride = (size_t)w * 4;
	void *buf = aligned_alloc(64, (stride * (size_t)h + 63) & ~(size_t)63);

	lv_display_set_color_format(d, LV_COLOR_FORMAT_XRGB8888);
	lv_display_set_buffers_with_stride(d, buf, NULL, (uint32_t)(stride * (size_t)h),
					   (uint32_t)stride, LV_DISPLAY_RENDER_MODE_DIRECT);
	lv_display_set_flush_cb(d, flush_cb);
	return d;
}
