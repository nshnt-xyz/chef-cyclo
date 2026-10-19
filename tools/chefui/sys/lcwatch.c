/*
 * lcwatch.c - a lineclient on the chefui loop (lcwatch.h).
 */
#include <time.h>

#include "chefui.h"
#include "lcwatch.h"

int64_t lcwatch_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void after(struct lcwatch *w)
{
	if (w->after)
		w->after(w->user);
	else
		lcwatch_sync(w);
}

static void io(int fd, short revents, void *user)
{
	struct lcwatch *w = user;

	(void)fd;
	lineclient_handle(w->lc, revents, w->now());
	after(w);
}

static void fired(lv_timer_t *t)
{
	struct lcwatch *w = lv_timer_get_user_data(t);

	w->timer = NULL;	/* one-shot: LVGL deletes it after this call */
	w->timer_at = -1;
	lineclient_tick(w->lc, w->now());
	after(w);
}

void lcwatch_init(struct lcwatch *w, struct lineclient *lc, int64_t (*now)(void),
		  void (*after_cb)(void *user), void *user)
{
	w->lc = lc;
	w->now = now ? now : lcwatch_now;
	w->after = after_cb;
	w->user = user;
	w->fd = -1;
	w->events = 0;
	w->timer = NULL;
	w->timer_at = -1;
}

static void unregister_stale(struct lcwatch *w)
{
	struct pollfd p = lineclient_pollfd(w->lc);

	if (w->fd >= 0 && w->fd != p.fd) {
		chefui_unwatch_fd(w->fd);
		w->fd = -1;
	}
}

static void register_current(struct lcwatch *w)
{
	struct pollfd p = lineclient_pollfd(w->lc);
	int64_t d = lineclient_deadline(w->lc);

	if (p.fd >= 0 && (p.fd != w->fd || p.events != w->events)) {
		chefui_watch_fd(p.fd, p.events, io, w);
		w->fd = p.fd;
		w->events = p.events;
	}
	if (d == w->timer_at)
		return;
	if (w->timer) {
		lv_timer_delete(w->timer);
		w->timer = NULL;
	}
	w->timer_at = d;
	if (d >= 0) {
		int64_t ms = d - w->now();

		w->timer = lv_timer_create(fired, ms > 0 ? (uint32_t)ms : 0, w);
		lv_timer_set_repeat_count(w->timer, 1);
	}
}

void lcwatch_sync_all(struct lcwatch *ws, int n)
{
	for (int i = 0; i < n; i++)
		unregister_stale(&ws[i]);
	for (int i = 0; i < n; i++)
		register_current(&ws[i]);
}

void lcwatch_sync(struct lcwatch *w)
{
	lcwatch_sync_all(w, 1);
}

void lcwatch_fini(struct lcwatch *w)
{
	if (w->fd >= 0)
		chefui_unwatch_fd(w->fd);
	w->fd = -1;
	if (w->timer)
		lv_timer_delete(w->timer);
	w->timer = NULL;
	w->timer_at = -1;
}
