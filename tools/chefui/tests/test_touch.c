/* Host tests for touch.c from injected input_event streams: scaling and
 * rotation, rapid taps reusing slot 0 with one unchanged axis (the
 * prototype's 0,0 bug), press + release in one batch giving two states,
 * multi-slot primary tracking, SYN_DROPPED resync through a wrapped
 * EVIOCGMTSLOTS, release-all on screen off, wake, queue overflow, fd
 * reads and touchscreen discovery. Built with -Wl,--wrap=ioctl. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "../touch.h"

/* ---- the fake device behind the wrapped ioctl ---- */

static struct {
	int fail;			/* EVIOCGMTSLOTS/EVIOCGABS fail with EIO */
	int32_t id[CU_TOUCH_SLOTS], x[CU_TOUCH_SLOTS], y[CU_TOUCH_SLOTS];
	int32_t cur_slot;
	int mtslots_calls;
} dev;

int __real_ioctl(int fd, unsigned long req, ...);

static bool fd_is(int fd, const char *suffix)
{
	char link[64], path[256];
	ssize_t n;

	snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
	n = readlink(link, path, sizeof(path) - 1);
	if (n <= 0)
		return false;
	path[n] = '\0';
	return strlen(path) >= strlen(suffix) && strcmp(path + strlen(path) - strlen(suffix), suffix) == 0;
}

int __wrap_ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	void *arg;

	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (_IOC_TYPE(req) != 'E')
		return __real_ioctl(fd, req, arg);
	if (_IOC_NR(req) == _IOC_NR(EVIOCGMTSLOTS(0))) {
		struct { __u32 code; __s32 v[64]; } *m = arg;
		int n = (int)((_IOC_SIZE(req) - sizeof(__u32)) / sizeof(__s32)), i;

		dev.mtslots_calls++;
		if (dev.fail) {
			errno = EIO;
			return -1;
		}
		for (i = 0; i < n && i < CU_TOUCH_SLOTS; i++)
			m->v[i] = m->code == ABS_MT_TRACKING_ID ? dev.id[i] :
				  m->code == ABS_MT_POSITION_X ? dev.x[i] : dev.y[i];
		return 0;
	}
	if (_IOC_NR(req) >= 0x40 && _IOC_NR(req) < 0x40 + ABS_CNT) {	/* EVIOCGABS */
		struct input_absinfo *a = arg;
		unsigned code = _IOC_NR(req) - 0x40;

		if (dev.fail) {
			errno = EIO;
			return -1;
		}
		memset(a, 0, sizeof(*a));
		if (code == ABS_MT_POSITION_X)
			a->maximum = 720;
		else if (code == ABS_MT_POSITION_Y)
			a->maximum = 1600;
		else if (code == ABS_MT_SLOT) {
			a->maximum = 9;
			a->value = dev.cur_slot;
		}
		return 0;
	}
	if (_IOC_NR(req) == _IOC_NR(EVIOCGPROP(0)) || _IOC_NR(req) == _IOC_NR(EVIOCGBIT(EV_ABS, 0))) {
		uint8_t *bits = arg;
		bool touch = fd_is(fd, "/event3"), keys = fd_is(fd, "/event1");

		memset(bits, 0, _IOC_SIZE(req));
		if (_IOC_NR(req) == _IOC_NR(EVIOCGPROP(0))) {
			if (touch)
				bits[INPUT_PROP_DIRECT / 8] |= 1u << (INPUT_PROP_DIRECT % 8);
		} else if (touch || keys) {
			/* event1 has MT axes but no DIRECT property (a touchpad) */
			bits[ABS_MT_POSITION_X / 8] |= 1u << (ABS_MT_POSITION_X % 8);
			bits[ABS_MT_POSITION_Y / 8] |= 1u << (ABS_MT_POSITION_Y % 8);
		}
		return 0;
	}
	return __real_ioctl(fd, req, arg);
}

/* ---- event helpers ---- */

static struct input_event evs[256];
static size_t nev;

static void ev(uint16_t type, uint16_t code, int32_t value)
{
	evs[nev].type = type;
	evs[nev].code = code;
	evs[nev].value = value;
	nev++;
}

static void syn(void) { ev(EV_SYN, SYN_REPORT, 0); }
static void slot(int s) { ev(EV_ABS, ABS_MT_SLOT, s); }
static void tid(int id) { ev(EV_ABS, ABS_MT_TRACKING_ID, id); }
static void px(int x) { ev(EV_ABS, ABS_MT_POSITION_X, x); }
static void py(int y) { ev(EV_ABS, ABS_MT_POSITION_Y, y); }

static void flush(struct cu_touch *t)
{
	cu_touch_feed(t, evs, nev);
	nev = 0;
}

static void setup(struct cu_touch *t, int rot)
{
	struct cu_geom g;

	cu_geom_init(&g, rot, 1080, 2246);
	cu_touch_init(t, &g);
	t->ax.minimum = 0;
	t->ax.maximum = 720;
	t->ay.minimum = 0;
	t->ay.maximum = 1600;
	memset(&dev, 0, sizeof(dev));
	for (int i = 0; i < CU_TOUCH_SLOTS; i++)
		dev.id[i] = -1;
	nev = 0;
}

static void map(struct cu_touch *t, int rx, int ry, int32_t *x, int32_t *y)
{
	cu_touch_map(t, rx, ry, x, y);
}

/* ---- tests ---- */

static void test_scaling_rotation(void)
{
	struct cu_touch t;
	int32_t x, y;

	setup(&t, 0);
	map(&t, 0, 0, &x, &y);
	CHECK(x == 0 && y == 0);
	map(&t, 720, 1600, &x, &y);
	CHECK(x == 1079 && y == 2245);
	map(&t, 360, 800, &x, &y);
	CHECK(x == 539 && y == 1122);
	map(&t, 900, -20, &x, &y);		/* out of range: clamped */
	CHECK(x == 1079 && y == 0);

	setup(&t, 90);				/* logical 2246 x 1080 */
	map(&t, 720, 0, &x, &y);		/* physical top-right = logical top-left */
	CHECK(x == 0 && y == 0);
	map(&t, 0, 1600, &x, &y);		/* physical bottom-left = logical bottom-right */
	CHECK(x == 2245 && y == 1079);
	map(&t, 0, 0, &x, &y);			/* physical top-left = logical bottom-left */
	CHECK(x == 0 && y == 1079);

	setup(&t, 180);
	map(&t, 0, 0, &x, &y);
	CHECK(x == 1079 && y == 2245);
	setup(&t, 270);
	map(&t, 0, 0, &x, &y);			/* physical top-left = logical top-right */
	CHECK(x == 2245 && y == 0);
}

static void test_rapid_taps_unchanged_axis(void)
{
	struct cu_touch t;
	struct cu_touch_state st;
	int32_t x1, y1, x2, y2;

	setup(&t, 0);
	map(&t, 100, 200, &x1, &y1);
	map(&t, 105, 200, &x2, &y2);
	tid(5); px(100); py(200); syn();	/* tap 1 down */
	tid(-1); syn();				/* up */
	tid(6); px(105); syn();			/* tap 2: Y unchanged, so not resent */
	tid(-1); syn();
	tid(7); syn();				/* tap 3: nothing changed at all */
	flush(&t);

	CHECK(cu_touch_pop(&t, &st) && st.pressed && st.x == x1 && st.y == y1 && st.more);
	CHECK(cu_touch_pop(&t, &st) && !st.pressed && st.x == x1 && st.y == y1);
	CHECK(cu_touch_pop(&t, &st) && st.pressed && st.x == x2 && st.y == y2);	/* not 0 */
	CHECK(st.y != 0);
	CHECK(cu_touch_pop(&t, &st) && !st.pressed);
	CHECK(cu_touch_pop(&t, &st) && st.pressed && st.x == x2 && st.y == y2 && !st.more);
	CHECK(!cu_touch_pop(&t, &st) && st.pressed);	/* nothing queued: unchanged */
}

static void test_press_release_one_batch(void)
{
	struct cu_touch t;
	struct cu_touch_state st;

	setup(&t, 0);
	tid(1); px(10); py(10); syn();
	tid(-1); syn();
	flush(&t);				/* both arrive in one read */
	CHECK(cu_touch_any_down(&t));		/* frames queued: keep reading */
	CHECK(cu_touch_pop(&t, &st) && st.pressed && st.more);
	CHECK(st.nc == 1 && st.c[0].down && st.ndown == 1);
	CHECK(cu_touch_pop(&t, &st) && !st.pressed && !st.more);
	CHECK(st.nc == 1 && !st.c[0].down && st.c[0].id == 1 && st.ndown == 0);	/* the lift is reported */
	CHECK(!cu_touch_any_down(&t));
	/* a partial frame (no SYN_REPORT yet) is not delivered */
	tid(2); px(30);
	flush(&t);
	CHECK(!cu_touch_pop(&t, &st) && !st.pressed);
	syn();
	flush(&t);
	CHECK(cu_touch_pop(&t, &st) && st.pressed);
}

static void test_multi_slot_primary(void)
{
	struct cu_touch t;
	struct cu_touch_state st;
	int32_t ax, ay, bx, by, cx, cy;

	setup(&t, 0);
	map(&t, 10, 10, &ax, &ay);
	map(&t, 20, 20, &bx, &by);
	map(&t, 500, 500, &cx, &cy);

	tid(1); px(10); py(10); syn();				/* A down in slot 0 */
	slot(1); tid(2); px(500); py(500); syn();		/* B down in slot 1 */
	slot(0); px(20); py(20); syn();				/* A moves */
	tid(-1); syn();						/* A lifts, B stays */
	slot(1); px(510); syn();				/* B moves */
	slot(0); tid(3); px(40); py(40); syn();			/* C down while B is down */
	tid(-1); slot(1); tid(-1); syn();			/* all up */
	tid(4); px(500); py(500); syn();			/* D down in slot 1 */
	flush(&t);

	CHECK(cu_touch_pop(&t, &st) && st.pressed && st.x == ax && st.y == ay && st.ndown == 1);
	CHECK(cu_touch_pop(&t, &st) && st.pressed && st.x == ax && st.ndown == 2 && st.nc == 2);
	CHECK(cu_touch_pop(&t, &st) && st.pressed && st.x == bx && st.y == by);
	CHECK(cu_touch_pop(&t, &st) && !st.pressed && st.x == bx && st.ndown == 1);	/* primary lifted */
	CHECK(st.nc == 2 && !st.c[0].down && st.c[0].id == 1 && st.c[1].down && st.c[1].id == 2);
	CHECK(cu_touch_pop(&t, &st) && !st.pressed);			/* B is not promoted */
	CHECK(cu_touch_pop(&t, &st) && !st.pressed && st.ndown == 2);	/* nor C */
	CHECK(cu_touch_pop(&t, &st) && !st.pressed && st.ndown == 0);
	CHECK(cu_touch_pop(&t, &st) && st.pressed && st.x == cx && st.y == cy);	/* D: new primary */
	CHECK(st.c[0].slot == 1 && st.c[0].id == 4);
}

static void test_syn_dropped_resync(void)
{
	struct cu_touch t;
	struct cu_touch_state st;
	int32_t x, y;

	setup(&t, 0);
	t.fd = 99;			/* any value: the wrapped ioctl answers */
	tid(1); px(10); py(10); syn();
	flush(&t);
	CHECK(cu_touch_pop(&t, &st) && st.pressed);

	/* the kernel buffer overflowed: slot 0 lifted and slot 1 came down
	 * while we were not reading */
	dev.x[0] = 10;			/* the kernel keeps a lifted slot's values */
	dev.y[0] = 10;
	dev.id[1] = 7;
	dev.x[1] = 300;
	dev.y[1] = 400;
	dev.cur_slot = 1;
	ev(EV_SYN, SYN_DROPPED, 0);
	px(999); py(999); tid(42);	/* partial garbage: must be ignored */
	syn();
	px(310);			/* applies to the kernel's current slot (1) */
	syn();
	flush(&t);
	CHECK(t.mt.drops == 1 && t.mt.resyncs == 1 && dev.mtslots_calls == 3);
	CHECK(cu_touch_pop(&t, &st) && !st.pressed && st.ndown == 1);	/* slot 0 lift seen */
	CHECK(st.nc == 2 && !st.c[0].down && st.c[1].down && st.c[1].id == 7);
	map(&t, 300, 400, &x, &y);
	CHECK(st.c[1].x == x && st.c[1].y == y);
	CHECK(cu_touch_pop(&t, &st) && st.c[0].slot == 1);
	map(&t, 310, 400, &x, &y);
	CHECK(st.c[0].x == x && st.c[0].y == y);
	CHECK(t.mt.slots[0].id == -1 && t.mt.slots[0].x == 10);	/* coordinates kept */

	/* resync failure: every contact released */
	dev.fail = 1;
	ev(EV_SYN, SYN_DROPPED, 0);
	syn();
	flush(&t);
	CHECK(cu_touch_pop(&t, &st) && st.ndown == 0 && !st.pressed);
	CHECK(!cu_touch_any_down(&t));
}

static void test_release_all_and_wake(void)
{
	struct cu_touch t;
	struct cu_touch_state st;
	int p[2];
	struct input_event stale[3];
	char c;

	setup(&t, 0);
	tid(1); px(10); py(10); syn();
	tid(-1); syn();
	tid(2); px(20); syn();		/* down, frames still queued */
	flush(&t);
	CHECK(cu_touch_pop(&t, &st) && st.pressed);
	cu_touch_release_all(&t);	/* screen off */
	CHECK(cu_touch_pop(&t, &st) && !st.pressed && st.ndown == 0 && !st.more);
	CHECK(!cu_touch_any_down(&t));

	/* screen on: stale events are drained, slots resynced */
	CHECK(pipe2(p, O_NONBLOCK) == 0);
	memset(stale, 0, sizeof(stale));
	stale[0].type = EV_ABS; stale[0].code = ABS_MT_TRACKING_ID; stale[0].value = 9;
	stale[1].type = EV_SYN; stale[1].code = SYN_REPORT;
	stale[2] = stale[1];
	CHECK(write(p[1], stale, sizeof(stale)) == (ssize_t)sizeof(stale));
	t.fd = p[0];
	dev.id[0] = 11;			/* a finger already on the glass */
	dev.x[0] = 360;
	dev.y[0] = 800;
	cu_touch_wake(&t);
	CHECK(read(p[0], &c, 1) == -1 && errno == EAGAIN);	/* drained */
	CHECK(cu_touch_pop(&t, &st) && st.ndown == 1 && st.c[0].id == 11 && !st.more);
	close(p[0]);
	close(p[1]);
}

static void test_queue_overflow(void)
{
	struct cu_touch t;
	struct cu_touch_state st;
	int i, pops = 0;

	setup(&t, 0);
	for (i = 0; i < CU_TOUCH_QUEUE + 10; i++) {
		px(i);
		syn();
		if (nev > 200)
			flush(&t);
	}
	tid(3); px(5); py(5); syn();	/* the newest state survives */
	flush(&t);
	while (cu_touch_pop(&t, &st))
		pops++;
	CHECK(pops == CU_TOUCH_QUEUE);
	CHECK(st.pressed && st.c[0].id == 3);
}

static void test_read_fd(void)
{
	struct cu_touch t;
	struct cu_touch_state st;
	struct input_event e[4];
	int p[2];

	setup(&t, 0);
	CHECK(pipe2(p, O_NONBLOCK) == 0);
	memset(e, 0, sizeof(e));
	e[0].type = EV_ABS; e[0].code = ABS_MT_TRACKING_ID; e[0].value = 1;
	e[1].type = EV_SYN; e[1].code = SYN_REPORT;
	e[2].type = EV_ABS; e[2].code = ABS_MT_TRACKING_ID; e[2].value = -1;
	e[3].type = EV_SYN; e[3].code = SYN_REPORT;
	CHECK(write(p[1], e, sizeof(e)) == (ssize_t)sizeof(e));
	t.fd = p[0];
	CHECK(cu_touch_read_fd(&t) == 0);
	CHECK(cu_touch_pop(&t, &st) && st.pressed && st.more);
	CHECK(cu_touch_pop(&t, &st) && !st.pressed);
	CHECK(cu_touch_read_fd(&t) == 0);	/* EAGAIN is not an error */
	close(p[1]);
	CHECK(cu_touch_read_fd(&t) == -ENODEV);	/* device gone */
	cu_touch_close(&t);
	CHECK(t.fd == -1);
}

static void test_discovery(void)
{
	char dir[] = "/tmp/chefui-input-XXXXXX", path[64];
	struct cu_touch t;
	const char *names[] = { "event0", "event1", "event3", "mice" };
	unsigned i;

	CHECK(mkdtemp(dir) != NULL);
	setup(&t, 0);
	CHECK(cu_touch_open_scan(&t, dir) == -ENODEV);	/* empty */
	for (i = 0; i < 4; i++) {
		int fd;

		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		fd = open(path, O_WRONLY | O_CREAT, 0600);
		CHECK(fd >= 0);
		close(fd);
	}
	dev.cur_slot = 0;
	CHECK(cu_touch_open_scan(&t, dir) == 0);
	snprintf(path, sizeof(path), "%s/event3", dir);
	CHECK(strcmp(t.path, path) == 0);	/* not the touchpad-like event1 */
	CHECK(t.ax.maximum == 720 && t.ay.maximum == 1600 && t.mt.nslots == 10);
	CHECK(fcntl(t.fd, F_GETFL) & O_NONBLOCK);
	cu_touch_close(&t);
	for (i = 0; i < 4; i++) {
		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		unlink(path);
	}
	rmdir(dir);
}

int main(void)
{
	test_scaling_rotation();
	test_rapid_taps_unchanged_axis();
	test_press_release_one_batch();
	test_multi_slot_primary();
	test_syn_dropped_resync();
	test_release_all_and_wake();
	test_queue_overflow();
	test_read_fd();
	test_discovery();
	return check_report("test-touch");
}
