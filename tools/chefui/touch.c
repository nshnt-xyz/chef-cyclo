/* touch.c - see touch.h. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "touch.h"

void cu_touch_init(struct cu_touch *t, const struct cu_geom *g)
{
	int i;

	memset(t, 0, sizeof(*t));
	t->fd = -1;
	t->geom = *g;
	/* until a device says otherwise: raw = physical pixels */
	t->ax.maximum = g->pw - 1;
	t->ay.maximum = g->ph - 1;
	evdev_mt_init(&t->mt);
	for (i = 0; i < CU_TOUCH_SLOTS; i++)
		t->last.s[i].id = -1;
	t->primary = -1;
}

static int32_t scale(int32_t v, const struct input_absinfo *a, int32_t size)
{
	int64_t r;

	if (a->maximum <= a->minimum)
		return v;
	r = (int64_t)(v - a->minimum) * (size - 1) / (a->maximum - a->minimum);
	if (r < 0)
		r = 0;
	if (r > size - 1)
		r = size - 1;
	return (int32_t)r;
}

static int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

void cu_touch_map(const struct cu_touch *t, int32_t rx, int32_t ry, int32_t *lx, int32_t *ly)
{
	int32_t px = scale(rx, &t->ax, t->geom.pw), py = scale(ry, &t->ay, t->geom.ph);

	cu_phys_to_log(&t->geom, px, py, lx, ly);
	*lx = clamp(*lx, 0, t->geom.lw - 1);
	*ly = clamp(*ly, 0, t->geom.lh - 1);
}

static void push_frame(struct cu_touch *t)
{
	unsigned idx;

	if (t->qcount == CU_TOUCH_QUEUE) {
		/* full: the newest frame is replaced, so the latest state is
		 * never lost (an intermediate one is) */
		idx = (t->qhead + t->qcount - 1) % CU_TOUCH_QUEUE;
		t->overflow = true;
	} else {
		idx = (t->qhead + t->qcount) % CU_TOUCH_QUEUE;
		t->qcount++;
	}
	memcpy(t->q[idx].s, t->mt.slots, sizeof(t->mt.slots));
}

void cu_touch_feed(struct cu_touch *t, const struct input_event *ev, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (evdev_mt_feed(&t->mt, t->fd, &ev[i]))
			push_frame(t);
}

static void state_from(struct cu_touch *t, const struct cu_touch_frame *f,
		       const struct cu_touch_frame *before, struct cu_touch_state *st)
{
	int i;

	st->nc = 0;
	st->ndown = 0;
	for (i = 0; i < CU_TOUCH_SLOTS; i++) {
		const struct evdev_slot *s = &f->s[i];
		struct cu_contact *c;

		if (s->id < 0 && (!before || before->s[i].id < 0))
			continue;
		c = &st->c[st->nc++];
		c->slot = i;
		c->down = s->id >= 0;
		c->id = c->down ? s->id : before->s[i].id;
		cu_touch_map(t, s->x, s->y, &c->x, &c->y);
		if (c->down)
			st->ndown++;
	}
	st->pressed = t->primary >= 0;
	st->x = t->px;
	st->y = t->py;
}

bool cu_touch_pop(struct cu_touch *t, struct cu_touch_state *st)
{
	struct cu_touch_frame f;
	int i, ndown = 0;

	if (t->qcount == 0) {
		state_from(t, &t->last, NULL, st);
		st->more = false;
		return false;
	}
	f = t->q[t->qhead];
	t->qhead = (t->qhead + 1) % CU_TOUCH_QUEUE;
	t->qcount--;
	t->overflow = false;

	for (i = 0; i < CU_TOUCH_SLOTS; i++)
		if (f.s[i].id >= 0)
			ndown++;
	if (t->primary >= 0) {
		const struct evdev_slot *s = &f.s[t->primary];

		if (s->id == t->primary_id) {
			cu_touch_map(t, s->x, s->y, &t->px, &t->py);
		} else {
			/* lifted (or the slot was reused by a new contact
			 * within one frame): the pointer releases where it
			 * was last seen; the next primary waits for all-up */
			t->primary = -1;
			t->wait_all_up = ndown > 0;
		}
	} else if (t->wait_all_up) {
		if (ndown == 0)
			t->wait_all_up = false;
	}
	if (t->primary < 0 && !t->wait_all_up && ndown > 0) {
		for (i = 0; i < CU_TOUCH_SLOTS; i++)
			if (f.s[i].id >= 0)
				break;
		t->primary = i;
		t->primary_id = f.s[i].id;
		cu_touch_map(t, f.s[i].x, f.s[i].y, &t->px, &t->py);
	}
	state_from(t, &f, &t->last, st);
	t->last = f;
	st->more = t->qcount > 0;
	return true;
}

bool cu_touch_any_down(const struct cu_touch *t)
{
	int i;

	if (t->qcount)
		return true;
	for (i = 0; i < CU_TOUCH_SLOTS; i++)
		if (t->last.s[i].id >= 0)
			return true;
	return false;
}

void cu_touch_release_all(struct cu_touch *t)
{
	evdev_mt_release_all(&t->mt);
	t->qcount = 0;
	t->mt.dropping = false;
	push_frame(t);
}

int cu_touch_resync(struct cu_touch *t)
{
	return evdev_mt_resync(&t->mt, t->fd);
}

/* ------------------------------------------------------------ device I/O */

int cu_touch_is_touchscreen(int fd)
{
	return evdev_is_touchscreen(fd) ? 1 : 0;
}

/* evdev_scan() callback: keep the first touchscreen, stop there. */
static int open_first_touchscreen(int fd, const char *path, void *ctx)
{
	struct cu_touch *t = ctx;
	struct input_absinfo ax, ay;

	if (fd < 0 || !evdev_is_touchscreen(fd) ||
	    ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) < 0 ||
	    ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay) < 0)
		return EVDEV_SCAN_REJECT;
	t->fd = fd;
	t->ax = ax;
	t->ay = ay;
	snprintf(t->path, sizeof(t->path), "%s", path);
	return EVDEV_SCAN_KEEP_STOP;
}

int cu_touch_open_scan(struct cu_touch *t, const char *dir)
{
	int rc = evdev_scan(dir, open_first_touchscreen, t);

	if (rc <= 0)
		return -ENODEV;
	(void)evdev_mt_setup(&t->mt, t->fd);
	t->qcount = 0;
	(void)cu_touch_resync(t);
	return 0;
}

int cu_touch_read_fd(struct cu_touch *t)
{
	struct input_event ev[64];

	if (t->fd < 0)
		return -EBADF;
	for (;;) {
		ssize_t n = read(t->fd, ev, sizeof(ev));

		if (n < 0) {
			if (errno == EAGAIN)
				return 0;
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -ENODEV;
		cu_touch_feed(t, ev, (size_t)n / sizeof(ev[0]));
		if ((size_t)n < sizeof(ev))
			return 0;
	}
}

void cu_touch_drain(struct cu_touch *t)
{
	struct input_event ev[64];

	if (t->fd < 0)
		return;
	while (read(t->fd, ev, sizeof(ev)) > 0)
		;
}

void cu_touch_wake(struct cu_touch *t)
{
	cu_touch_drain(t);
	t->qcount = 0;
	t->mt.dropping = false;
	if (cu_touch_resync(t) != 0)
		evdev_mt_release_all(&t->mt);
	push_frame(t);
}

void cu_touch_close(struct cu_touch *t)
{
	if (t->fd >= 0)
		close(t->fd);
	t->fd = -1;
	t->path[0] = '\0';
}
