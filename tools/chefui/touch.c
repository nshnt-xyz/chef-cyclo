/* touch.c - see touch.h. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "touch.h"

static void slots_clear(struct cu_slot *s)
{
	int i;

	for (i = 0; i < CU_TOUCH_SLOTS; i++) {
		s[i].id = -1;
		s[i].x = 0;
		s[i].y = 0;
	}
}

void cu_touch_init(struct cu_touch *t, const struct cu_geom *g)
{
	memset(t, 0, sizeof(*t));
	t->fd = -1;
	t->geom = *g;
	t->nslots = CU_TOUCH_SLOTS;
	/* until a device says otherwise: raw = physical pixels */
	t->ax.maximum = g->pw - 1;
	t->ay.maximum = g->ph - 1;
	slots_clear(t->work);
	slots_clear(t->last.s);
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
	memcpy(t->q[idx].s, t->work, sizeof(t->work));
}

void cu_touch_feed(struct cu_touch *t, const struct input_event *ev, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		const struct input_event *e = &ev[i];

		if (t->dropping) {
			if (e->type == EV_SYN && e->code == SYN_REPORT) {
				t->dropping = false;
				if (cu_touch_resync(t) == 0) {
					t->resyncs++;
				} else {
					int s;

					for (s = 0; s < CU_TOUCH_SLOTS; s++)
						t->work[s].id = -1;
				}
				push_frame(t);
			}
			continue;
		}
		if (e->type == EV_SYN) {
			if (e->code == SYN_REPORT) {
				push_frame(t);
			} else if (e->code == SYN_DROPPED) {
				t->dropping = true;
				t->drops++;
			}
			continue;
		}
		if (e->type != EV_ABS)
			continue;
		if (e->code == ABS_MT_SLOT) {
			t->cur_slot = e->value >= 0 && e->value < CU_TOUCH_SLOTS ? e->value : -1;
			continue;
		}
		if (t->cur_slot < 0)
			continue;
		switch (e->code) {
		case ABS_MT_TRACKING_ID:
			t->work[t->cur_slot].id = e->value < 0 ? -1 : e->value;
			break;
		case ABS_MT_POSITION_X:
			t->work[t->cur_slot].x = e->value;
			break;
		case ABS_MT_POSITION_Y:
			t->work[t->cur_slot].y = e->value;
			break;
		default:
			break;
		}
	}
}

static void state_from(struct cu_touch *t, const struct cu_touch_frame *f,
		       const struct cu_touch_frame *before, struct cu_touch_state *st)
{
	int i;

	st->nc = 0;
	st->ndown = 0;
	for (i = 0; i < CU_TOUCH_SLOTS; i++) {
		const struct cu_slot *s = &f->s[i];
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
		const struct cu_slot *s = &f.s[t->primary];

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
	int i;

	for (i = 0; i < CU_TOUCH_SLOTS; i++)
		t->work[i].id = -1;
	t->qcount = 0;
	t->dropping = false;
	push_frame(t);
}

int cu_touch_resync(struct cu_touch *t)
{
	struct {
		__u32 code;
		__s32 v[CU_TOUCH_SLOTS];
	} id, x, y;
	struct input_absinfo slot;
	int i, n = t->nslots < CU_TOUCH_SLOTS ? t->nslots : CU_TOUCH_SLOTS;

	if (t->fd < 0)
		return -EBADF;
	memset(&id, 0, sizeof(id));
	memset(&x, 0, sizeof(x));
	memset(&y, 0, sizeof(y));
	id.code = ABS_MT_TRACKING_ID;
	x.code = ABS_MT_POSITION_X;
	y.code = ABS_MT_POSITION_Y;
	if (ioctl(t->fd, EVIOCGMTSLOTS(sizeof(id)), &id) < 0 ||
	    ioctl(t->fd, EVIOCGMTSLOTS(sizeof(x)), &x) < 0 ||
	    ioctl(t->fd, EVIOCGMTSLOTS(sizeof(y)), &y) < 0 ||
	    ioctl(t->fd, EVIOCGABS(ABS_MT_SLOT), &slot) < 0)
		return -errno;
	for (i = 0; i < CU_TOUCH_SLOTS; i++) {
		if (i < n) {
			t->work[i].id = id.v[i] < 0 ? -1 : id.v[i];
			t->work[i].x = x.v[i];
			t->work[i].y = y.v[i];
		} else {
			t->work[i].id = -1;
		}
	}
	t->cur_slot = slot.value >= 0 && slot.value < CU_TOUCH_SLOTS ? slot.value : -1;
	return 0;
}

/* ------------------------------------------------------------ device I/O */

#define TEST_BIT(arr, b) ((arr)[(b) / 8] & (1u << ((b) % 8)))

int cu_touch_is_touchscreen(int fd)
{
	uint8_t props[INPUT_PROP_CNT / 8 + 1], abs[ABS_CNT / 8 + 1];

	memset(props, 0, sizeof(props));
	memset(abs, 0, sizeof(abs));
	if (ioctl(fd, EVIOCGPROP(sizeof(props)), props) < 0 ||
	    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) < 0)
		return 0;
	return TEST_BIT(props, INPUT_PROP_DIRECT) && TEST_BIT(abs, ABS_MT_POSITION_X) &&
	       TEST_BIT(abs, ABS_MT_POSITION_Y) ? 1 : 0;
}

int cu_touch_open_scan(struct cu_touch *t, const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	char best[300] = "";
	int fd = -1;

	if (!d)
		return -ENODEV;
	/* lowest-numbered matching node, independent of readdir order */
	while ((e = readdir(d))) {
		char p[300];
		int f;

		if (strncmp(e->d_name, "event", 5))
			continue;
		if (best[0] && strtol(e->d_name + 5, NULL, 10) >= strtol(strrchr(best, '/') + 6, NULL, 10))
			continue;
		snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
		f = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (f < 0)
			continue;
		if (cu_touch_is_touchscreen(f))
			snprintf(best, sizeof(best), "%s", p);
		close(f);
	}
	closedir(d);
	if (!best[0])
		return -ENODEV;
	fd = open(best, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	{
		struct input_absinfo ax, ay, slot;

		if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) < 0 ||
		    ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay) < 0) {
			int err = errno;

			close(fd);
			return -err;
		}
		t->ax = ax;
		t->ay = ay;
		if (ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &slot) == 0 && slot.maximum >= 0)
			t->nslots = slot.maximum + 1 < CU_TOUCH_SLOTS ? slot.maximum + 1 : CU_TOUCH_SLOTS;
		else
			t->nslots = 1;
	}
	t->fd = fd;
	snprintf(t->path, sizeof(t->path), "%s", best);
	slots_clear(t->work);
	t->cur_slot = 0;
	t->dropping = false;
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
	int i;

	cu_touch_drain(t);
	t->qcount = 0;
	t->dropping = false;
	if (cu_touch_resync(t) != 0)
		for (i = 0; i < CU_TOUCH_SLOTS; i++)
			t->work[i].id = -1;
	push_frame(t);
}

void cu_touch_close(struct cu_touch *t)
{
	if (t->fd >= 0)
		close(t->fd);
	t->fd = -1;
	t->path[0] = '\0';
}
