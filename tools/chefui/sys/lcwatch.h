/*
 * lcwatch.h - a ../common/lineclient on the chefui loop: the client's fd
 * registered with chefui_watch_fd() (registered again whenever the fd or
 * its events change) and one one-shot lv_timer at lineclient_deadline().
 * The fd callback runs lineclient_handle(), the timer lineclient_tick(),
 * both with the monotonic clock, then the `after` hook, which must sync
 * every lcwatch the call may have changed (a client's callbacks can start
 * or stop another client: gps-manager's state starts gpsd).
 *
 * The owner syncs after every call of its own into a lineclient too, so
 * a closed fd is never left registered: unregister before register
 * (lcwatch_sync_all() does that over a group), or a new socket that got
 * the closed one's number would lose its registration.
 */
#ifndef CHEF_LCWATCH_H
#define CHEF_LCWATCH_H

#include <stdint.h>

#include "common/lineclient.h"
#include "lvgl.h"

struct lcwatch {
	struct lineclient *lc;
	void (*after)(void *user);	/* NULL: lcwatch_sync() of this one */
	void *user;
	int fd;				/* registered, -1 none */
	short events;
	lv_timer_t *timer;
	int64_t timer_at;		/* the deadline the timer is set for */
};

void lcwatch_init(struct lcwatch *w, struct lineclient *lc, void (*after)(void *user), void *user);
/* The registration and the timer brought in line with the client. */
void lcwatch_sync(struct lcwatch *w);
/* The same for N watches: every stale fd unregistered first. */
void lcwatch_sync_all(struct lcwatch *ws, int n);
/* Unregistered and the timer deleted (the client itself is not touched). */
void lcwatch_fini(struct lcwatch *w);

/* The monotonic clock in ms, as lineclient takes it. */
int64_t lcwatch_now(void);

#endif /* CHEF_LCWATCH_H */
