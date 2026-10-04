/* Host tests for tools/evdev.h: bitmap helper, node-name parsing, the
 * numeric scan order and callback ownership against a temporary directory,
 * the touchscreen and key classifiers against fake devices, and the MT-B
 * decoder (multi-slot frames, coordinates kept on release, out-of-range
 * slots, setup, SYN_DROPPED discard + EVIOCGMTSLOTS resync, resync failure
 * releasing every contact). Built with -Wl,--wrap=open,--wrap=close,
 * --wrap=ioctl: open/close are recorded and passed through, ioctl answers
 * for the fake device registered on each fd. No real input device. */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../evdev.h"

static int g_failures;
static int g_tests;

#define CHECK(cond) do { \
	g_tests++; \
	if (!(cond)) { \
		g_failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

/* ---- fake devices behind the wrapped syscalls ---- */

enum kind { NONE, KEYS_POWER_VOLDOWN, KEYS_VOLUP, TOUCHSCREEN, TOUCHPAD, KEYS_AND_ABS, BROKEN, MT };

#define MAXFD 1024
static enum kind fd_kind[MAXFD];

static char opened[16][64];		/* basenames, in open order */
static int nopened, open_flags_bad, nclosed;
static const char *fail_open_base;	/* this node's open fails with EACCES */

static struct {
	int fail;			/* EVIOCGMTSLOTS/EVIOCGABS fail with EIO */
	int slot_max;			/* ABS_MT_SLOT maximum */
	int32_t id[EVDEV_MT_SLOTS], x[EVDEV_MT_SLOTS], y[EVDEV_MT_SLOTS];
	int32_t cur;
	int mtslots_calls;
} dev;

int __real_open(const char *path, int flags, ...);
int __real_close(int fd);
int __real_ioctl(int fd, unsigned long req, ...);

static enum kind kind_of(const char *base)
{
	if (!strcmp(base, "event0"))
		return KEYS_POWER_VOLDOWN;
	if (!strcmp(base, "event1"))
		return TOUCHSCREEN;
	if (!strcmp(base, "event2"))
		return TOUCHPAD;
	if (!strcmp(base, "event3"))
		return KEYS_VOLUP;
	if (!strcmp(base, "event10"))
		return KEYS_AND_ABS;
	if (!strcmp(base, "event11"))
		return BROKEN;
	return NONE;
}

int __wrap_open(const char *path, int flags, ...)
{
	const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	int fd;

	if (flags != (O_RDONLY | O_NONBLOCK | O_CLOEXEC))
		open_flags_bad++;
	if (nopened < 16)
		snprintf(opened[nopened++], sizeof(opened[0]), "%s", base);
	if (fail_open_base && !strcmp(base, fail_open_base)) {
		errno = EACCES;
		return -1;
	}
	fd = __real_open(path, flags);
	if (fd >= 0 && fd < MAXFD)
		fd_kind[fd] = kind_of(base);
	return fd;
}

int __wrap_close(int fd)
{
	nclosed++;
	if (fd >= 0 && fd < MAXFD)
		fd_kind[fd] = NONE;
	return __real_close(fd);
}

static void set_bit(void *buf, unsigned bit)
{
	((unsigned char *)buf)[bit / 8] |= 1u << (bit % 8);	/* the kernel's byte layout */
}

int __wrap_ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	void *arg;
	enum kind k = fd >= 0 && fd < MAXFD ? fd_kind[fd] : NONE;
	unsigned nr = _IOC_NR(req);

	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (_IOC_TYPE(req) != 'E')
		return __real_ioctl(fd, req, arg);
	if (k == BROKEN || k == NONE) {
		errno = ENOTTY;
		return -1;
	}
	if (nr == _IOC_NR(EVIOCGMTSLOTS(0))) {
		struct { __u32 code; __s32 v[64]; } *m = arg;
		int n = (int)((_IOC_SIZE(req) - sizeof(__u32)) / sizeof(__s32)), i;

		dev.mtslots_calls++;
		if (dev.fail) {
			errno = EIO;
			return -1;
		}
		for (i = 0; i < n && i < EVDEV_MT_SLOTS; i++)
			m->v[i] = m->code == ABS_MT_TRACKING_ID ? dev.id[i] :
				  m->code == ABS_MT_POSITION_X ? dev.x[i] : dev.y[i];
		return 0;
	}
	if (nr >= 0x40 && nr < 0x40 + ABS_CNT) {	/* EVIOCGABS */
		struct input_absinfo *a = arg;

		if (dev.fail) {
			errno = EIO;
			return -1;
		}
		memset(a, 0, sizeof(*a));
		if (nr - 0x40 == ABS_MT_SLOT) {
			a->maximum = dev.slot_max;
			a->value = dev.cur;
		}
		return 0;
	}
	if (nr == _IOC_NR(EVIOCGPROP(0)) || (nr >= 0x20 && nr <= 0x20 + EV_MAX)) {
		memset(arg, 0, _IOC_SIZE(req));
		if (nr == _IOC_NR(EVIOCGPROP(0))) {
			if (k == TOUCHSCREEN)
				set_bit(arg, INPUT_PROP_DIRECT);
		} else if (nr == 0x20) {			/* EVIOCGBIT(0): event types */
			if (k != TOUCHPAD)
				set_bit(arg, EV_KEY);
			if (k == TOUCHSCREEN || k == TOUCHPAD || k == KEYS_AND_ABS)
				set_bit(arg, EV_ABS);
		} else if (nr == 0x20 + EV_KEY) {
			if (k == KEYS_POWER_VOLDOWN) {
				set_bit(arg, KEY_POWER);
				set_bit(arg, KEY_VOLUMEDOWN);
			} else if (k == KEYS_VOLUP || k == KEYS_AND_ABS) {
				set_bit(arg, KEY_VOLUMEUP);
			} else if (k == TOUCHSCREEN) {
				set_bit(arg, BTN_TOUCH);
			}
		} else if (nr == 0x20 + EV_ABS) {
			if (k == TOUCHSCREEN || k == TOUCHPAD) {
				set_bit(arg, ABS_MT_SLOT);
				set_bit(arg, ABS_MT_POSITION_X);
				set_bit(arg, ABS_MT_POSITION_Y);
				set_bit(arg, ABS_MT_TRACKING_ID);
			} else if (k == KEYS_AND_ABS) {
				set_bit(arg, ABS_X);
			}
		}
		return 0;
	}
	errno = ENOTTY;
	return -1;
}

/* ---- bits and names ---- */

static void test_bits(void)
{
	EVDEV_BITMAP(bits, KEY_MAX);

	CHECK(sizeof(bits) * 8 >= KEY_MAX + 1);
	memset(bits, 0, sizeof(bits));
	set_bit(bits, 0);
	set_bit(bits, 63);
	set_bit(bits, 64);
	set_bit(bits, KEY_MAX);
	CHECK(evdev_bit(bits, 0) && evdev_bit(bits, 63) && evdev_bit(bits, 64));
	CHECK(evdev_bit(bits, KEY_MAX));
	CHECK(!evdev_bit(bits, 1) && !evdev_bit(bits, 62) && !evdev_bit(bits, 65));
}

static void test_node_number(void)
{
	CHECK(evdev_node_number("event0") == 0);
	CHECK(evdev_node_number("event7") == 7);
	CHECK(evdev_node_number("event10") == 10);
	CHECK(evdev_node_number("event01") == -1);	/* not a kernel name */
	CHECK(evdev_node_number("event") == -1);
	CHECK(evdev_node_number("eventx") == -1);
	CHECK(evdev_node_number("event-1") == -1);
	CHECK(evdev_node_number("event1x") == -1);
	CHECK(evdev_node_number("event99999999999") == -1);
	CHECK(evdev_node_number("mice") == -1);
	CHECK(evdev_node_number("mouse0") == -1);
}

/* ---- scan and classification ---- */

static char dir[] = "/tmp/chef-evdev-XXXXXX";
/* created in this order so readdir's order is unlikely to be numeric */
static const char *const nodes[] = { "event10", "mice", "event2", "event01", "event0",
				     "event11", "eventx", "event3", "event1" };
#define NNODES (sizeof(nodes) / sizeof(nodes[0]))

static void make_nodes(void)
{
	char path[128];
	unsigned i;

	CHECK(mkdtemp(dir) != NULL);
	for (i = 0; i < NNODES; i++) {
		int fd;

		snprintf(path, sizeof(path), "%s/%s", dir, nodes[i]);
		fd = __real_open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
		CHECK(fd >= 0);
		__real_close(fd);
	}
}

static void remove_nodes(void)
{
	char path[128];
	unsigned i;

	for (i = 0; i < NNODES; i++) {
		snprintf(path, sizeof(path), "%s/%s", dir, nodes[i]);
		unlink(path);
	}
	rmdir(dir);
}

struct scan_log {
	char seen[16][64];
	int n;
	int keep_from;		/* keep nodes whose index >= keep_from */
	int stop_at;		/* KEEP_STOP at this index, -1 = never */
	int kept_fds[16], nkept;
};

static int log_cb(int fd, const char *path, void *ctx)
{
	struct scan_log *l = ctx;
	int i = l->n;

	snprintf(l->seen[l->n++], sizeof(l->seen[0]), "%s", strrchr(path, '/') + 1);
	CHECK(strncmp(path, dir, strlen(dir)) == 0);
	CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
	CHECK(fcntl(fd, F_GETFL) & O_NONBLOCK);
	if (i < l->keep_from)
		return EVDEV_SCAN_REJECT;
	l->kept_fds[l->nkept++] = fd;
	return i == l->stop_at ? EVDEV_SCAN_KEEP_STOP : EVDEV_SCAN_KEEP;
}

static void test_scan_order_and_ownership(void)
{
	static const char *const order[] = { "event0", "event1", "event2", "event3", "event10", "event11" };
	struct scan_log l;
	int i, rc;

	/* everything rejected: numeric order, every fd closed by the scan */
	memset(&l, 0, sizeof(l));
	l.keep_from = 100;
	l.stop_at = -1;
	nopened = nclosed = open_flags_bad = 0;
	rc = evdev_scan(dir, log_cb, &l);
	CHECK(rc == 0 && l.n == 6);
	for (i = 0; i < 6 && i < l.n; i++)
		CHECK(strcmp(l.seen[i], order[i]) == 0);
	CHECK(nopened == 6 && nclosed == 6 && open_flags_bad == 0);

	/* keep from the third on, stop at the fifth: the scan closes only
	 * the rejected ones and opens nothing after the stop */
	memset(&l, 0, sizeof(l));
	l.keep_from = 2;
	l.stop_at = 4;
	nopened = nclosed = 0;
	rc = evdev_scan(dir, log_cb, &l);
	CHECK(rc == 3 && l.n == 5 && l.nkept == 3);
	CHECK(nopened == 5 && nclosed == 2);
	for (i = 0; i < l.nkept; i++) {
		CHECK(fcntl(l.kept_fds[i], F_GETFD) >= 0);	/* still open: the caller's */
		close(l.kept_fds[i]);
	}

	/* an unreadable directory is an error, not "nothing found" */
	CHECK(evdev_scan("/nonexistent/chef-evdev", log_cb, &l) == -ENOENT);
}

struct fail_log {
	char seen[16][64];
	int fds[16], n;
	int ret_on_fail;	/* what the callback answers for fd < 0 */
};

static int fail_cb(int fd, const char *path, void *ctx)
{
	struct fail_log *l = ctx;

	l->fds[l->n] = fd;
	snprintf(l->seen[l->n++], sizeof(l->seen[0]), "%s", strrchr(path, '/') + 1);
	return fd < 0 ? l->ret_on_fail : EVDEV_SCAN_REJECT;
}

static void test_scan_open_failure(void)
{
	struct fail_log l;

	/* a node that does not open reaches the callback as -errno, so it can
	 * be logged; nothing is closed for it and the scan goes on */
	memset(&l, 0, sizeof(l));
	fail_open_base = "event2";
	nopened = nclosed = 0;
	CHECK(evdev_scan(dir, fail_cb, &l) == 0);
	CHECK(l.n == 6 && nopened == 6 && nclosed == 5);
	CHECK(strcmp(l.seen[2], "event2") == 0 && l.fds[2] == -EACCES);
	CHECK(l.fds[1] >= 0 && l.fds[3] >= 0 && strcmp(l.seen[3], "event3") == 0);

	/* even a callback that answers KEEP for it does not get it counted */
	memset(&l, 0, sizeof(l));
	l.ret_on_fail = EVDEV_SCAN_KEEP;
	nopened = nclosed = 0;
	CHECK(evdev_scan(dir, fail_cb, &l) == 0 && l.n == 6 && nclosed == 5);

	/* KEEP_STOP still stops the scan */
	memset(&l, 0, sizeof(l));
	l.ret_on_fail = EVDEV_SCAN_KEEP_STOP;
	nopened = nclosed = 0;
	CHECK(evdev_scan(dir, fail_cb, &l) == 0 && l.n == 3 && nopened == 3 && nclosed == 2);
	fail_open_base = NULL;
}

static int classify_cb(int fd, const char *path, void *ctx)
{
	static const unsigned short codes[3] = { KEY_POWER, KEY_VOLUMEUP, KEY_VOLUMEDOWN };
	uint32_t *res = ctx;
	long n = evdev_node_number(strrchr(path, '/') + 1);

	CHECK(n >= 0 && n < 16);
	if (n >= 0 && n < 16)
		res[n] = (evdev_is_touchscreen(fd) ? 0x100u : 0) | evdev_key_mask(fd, codes, 3);
	return EVDEV_SCAN_REJECT;
}

static void test_classifiers(void)
{
	uint32_t res[16];

	memset(res, 0xff, sizeof(res));
	CHECK(evdev_scan(dir, classify_cb, res) == 0);
	CHECK(res[0] == 0x5);		/* power + voldown, a button device */
	CHECK(res[1] == 0x100);		/* touchscreen: its BTN_TOUCH is no button */
	CHECK(res[2] == 0);		/* MT axes but no INPUT_PROP_DIRECT: a touchpad */
	CHECK(res[3] == 0x2);		/* volup */
	CHECK(res[10] == 0);		/* volup but also EV_ABS: never a button device */
	CHECK(res[11] == 0);		/* every query fails */
	CHECK(evdev_is_touchscreen(-1) == false);
	CHECK(evdev_key_mask(-1, (const unsigned short[]){ KEY_POWER }, 1) == 0);
}

/* ---- MT-B decoder ---- */

#define MTFD 900			/* fake fd answered by the wrapped ioctl */

static struct input_event mkev(uint16_t type, uint16_t code, int32_t value)
{
	struct input_event e;

	memset(&e, 0, sizeof(e));
	e.type = type;
	e.code = code;
	e.value = value;
	return e;
}

/* Feed one event; returns what evdev_mt_feed said. */
static bool feed(struct evdev_mt *mt, uint16_t type, uint16_t code, int32_t value)
{
	struct input_event e = mkev(type, code, value);

	return evdev_mt_feed(mt, MTFD, &e);
}

static bool syn(struct evdev_mt *mt) { return feed(mt, EV_SYN, SYN_REPORT, 0); }

static void reset_dev(void)
{
	int i;

	memset(&dev, 0, sizeof(dev));
	dev.slot_max = 9;
	for (i = 0; i < EVDEV_MT_SLOTS; i++)
		dev.id[i] = -1;
	fd_kind[MTFD] = MT;
}

static void test_mt_init_and_setup(void)
{
	struct evdev_mt mt;
	int i, ok = 1;

	reset_dev();
	evdev_mt_init(&mt);
	for (i = 0; i < EVDEV_MT_SLOTS; i++)
		ok &= mt.slots[i].id == -1 && mt.slots[i].x == 0 && mt.slots[i].y == 0;
	CHECK(ok && mt.cur == 0 && mt.nslots == EVDEV_MT_SLOTS && !mt.dropping);
	CHECK(mt.drops == 0 && mt.resyncs == 0);

	mt.slots[3].id = 7;
	mt.slots[3].x = 5;
	mt.cur = 3;
	mt.dropping = true;
	mt.drops = 2;
	dev.slot_max = 4;
	CHECK(evdev_mt_setup(&mt, MTFD) == 0 && mt.nslots == 5);
	CHECK(mt.slots[3].id == -1 && mt.slots[3].x == 0 && mt.cur == 0 && !mt.dropping);
	CHECK(mt.drops == 2);			/* counters kept */
	dev.slot_max = 31;
	CHECK(evdev_mt_setup(&mt, MTFD) == 0 && mt.nslots == EVDEV_MT_SLOTS);
	dev.slot_max = -1;
	CHECK(evdev_mt_setup(&mt, MTFD) == 0 && mt.nslots == 1);
	dev.slot_max = 9;
	dev.fail = 1;
	CHECK(evdev_mt_setup(&mt, MTFD) == -EIO && mt.nslots == 1);
}

static void test_mt_frames(void)
{
	struct evdev_mt mt;

	reset_dev();
	evdev_mt_init(&mt);
	/* two contacts in one frame: complete only at SYN_REPORT */
	CHECK(!feed(&mt, EV_ABS, ABS_MT_SLOT, 0));
	CHECK(!feed(&mt, EV_ABS, ABS_MT_TRACKING_ID, 1));
	CHECK(!feed(&mt, EV_ABS, ABS_MT_POSITION_X, 100));
	CHECK(!feed(&mt, EV_ABS, ABS_MT_POSITION_Y, 200));
	CHECK(!feed(&mt, EV_ABS, ABS_MT_SLOT, 1));
	CHECK(!feed(&mt, EV_ABS, ABS_MT_TRACKING_ID, 2));
	CHECK(!feed(&mt, EV_ABS, ABS_MT_POSITION_X, 300));
	CHECK(!feed(&mt, EV_ABS, ABS_MT_POSITION_Y, 400));
	CHECK(!feed(&mt, EV_ABS, ABS_MT_TOUCH_MAJOR, 9));	/* other axes ignored */
	CHECK(!feed(&mt, EV_KEY, BTN_TOUCH, 1));
	CHECK(!feed(&mt, EV_SYN, SYN_MT_REPORT, 0));
	CHECK(syn(&mt));
	CHECK(mt.slots[0].id == 1 && mt.slots[0].x == 100 && mt.slots[0].y == 200);
	CHECK(mt.slots[1].id == 2 && mt.slots[1].x == 300 && mt.slots[1].y == 400);
	CHECK(mt.slots[2].id == -1 && mt.cur == 1);

	/* release keeps the coordinates; a new contact in that slot that
	 * repeats the old Y arrives without it and still has it */
	CHECK(!feed(&mt, EV_ABS, ABS_MT_TRACKING_ID, -1));
	CHECK(syn(&mt));
	CHECK(mt.slots[1].id == -1 && mt.slots[1].x == 300 && mt.slots[1].y == 400);
	CHECK(!feed(&mt, EV_ABS, ABS_MT_TRACKING_ID, 3));
	CHECK(!feed(&mt, EV_ABS, ABS_MT_POSITION_X, 310));
	CHECK(syn(&mt));
	CHECK(mt.slots[1].id == 3 && mt.slots[1].x == 310 && mt.slots[1].y == 400);
	CHECK(mt.slots[0].id == 1);		/* untouched slot unchanged */

	/* an out-of-range slot swallows its events until a valid one */
	CHECK(!feed(&mt, EV_ABS, ABS_MT_SLOT, EVDEV_MT_SLOTS));
	CHECK(mt.cur == -1);
	CHECK(!feed(&mt, EV_ABS, ABS_MT_TRACKING_ID, -1));
	CHECK(!feed(&mt, EV_ABS, ABS_MT_POSITION_X, 1));
	CHECK(syn(&mt));
	CHECK(mt.slots[0].id == 1 && mt.slots[1].id == 3 && mt.slots[1].x == 310);
	CHECK(!feed(&mt, EV_ABS, ABS_MT_SLOT, 0));
	CHECK(!feed(&mt, EV_ABS, ABS_MT_TRACKING_ID, -1));
	CHECK(syn(&mt));
	CHECK(mt.slots[0].id == -1 && mt.slots[0].x == 100);
}

static void test_mt_syn_dropped(void)
{
	struct evdev_mt mt;
	int i, ok = 1;

	reset_dev();
	evdev_mt_init(&mt);
	feed(&mt, EV_ABS, ABS_MT_TRACKING_ID, 1);
	feed(&mt, EV_ABS, ABS_MT_POSITION_X, 10);
	feed(&mt, EV_ABS, ABS_MT_POSITION_Y, 20);
	CHECK(syn(&mt));

	/* while we were not reading: slot 0 lifted (the kernel keeps its
	 * values), slot 2 came down, and the device's current slot is 2 */
	dev.x[0] = 10;
	dev.y[0] = 20;
	dev.id[2] = 8;
	dev.x[2] = 500;
	dev.y[2] = 600;
	dev.cur = 2;
	dev.slot_max = 5;
	mt.nslots = 3;			/* only slots 0..2 are read back */
	dev.id[4] = 9;			/* beyond the device's slots: ignored */
	mt.slots[4].id = 4;		/* and released */
	CHECK(!feed(&mt, EV_SYN, SYN_DROPPED, 0));
	CHECK(mt.dropping && mt.drops == 1);
	/* partial garbage up to the SYN_REPORT is discarded */
	CHECK(!feed(&mt, EV_ABS, ABS_MT_SLOT, 0));
	CHECK(!feed(&mt, EV_ABS, ABS_MT_TRACKING_ID, 42));
	CHECK(!feed(&mt, EV_ABS, ABS_MT_POSITION_X, 999));
	CHECK(!feed(&mt, EV_SYN, SYN_MT_REPORT, 0));
	CHECK(!feed(&mt, EV_SYN, SYN_DROPPED, 0));
	CHECK(mt.slots[0].id == 1 && mt.slots[0].x == 10 && dev.mtslots_calls == 0);
	/* the SYN_REPORT resyncs and completes the frame */
	CHECK(syn(&mt));
	CHECK(!mt.dropping && mt.resyncs == 1 && dev.mtslots_calls == 3);
	CHECK(mt.slots[0].id == -1 && mt.slots[0].x == 10 && mt.slots[0].y == 20);
	CHECK(mt.slots[2].id == 8 && mt.slots[2].x == 500 && mt.slots[2].y == 600);
	CHECK(mt.slots[4].id == -1 && mt.cur == 2);
	/* the next event applies to the kernel's current slot */
	CHECK(!feed(&mt, EV_ABS, ABS_MT_POSITION_X, 510));
	CHECK(syn(&mt));
	CHECK(mt.slots[2].x == 510 && mt.slots[2].y == 600);

	/* resync failure: every contact is released, coordinates kept */
	dev.fail = 1;
	CHECK(!feed(&mt, EV_SYN, SYN_DROPPED, 0));
	CHECK(syn(&mt));
	for (i = 0; i < EVDEV_MT_SLOTS; i++)
		ok &= mt.slots[i].id == -1;
	CHECK(ok && mt.slots[2].x == 510 && mt.drops == 2 && mt.resyncs == 1 && !mt.dropping);

	/* direct resync: no fd, or a failing device, changes nothing */
	mt.slots[1].id = 5;
	CHECK(evdev_mt_resync(&mt, -1) == -EBADF && mt.slots[1].id == 5);
	CHECK(evdev_mt_resync(&mt, MTFD) == -EIO && mt.slots[1].id == 5);
	dev.fail = 0;
	dev.cur = 40;			/* out of range: no current slot */
	CHECK(evdev_mt_resync(&mt, MTFD) == 0 && mt.slots[1].id == -1 && mt.cur == -1);
}

int main(void)
{
	test_bits();
	test_node_number();
	make_nodes();
	test_scan_order_and_ownership();
	test_scan_open_failure();
	test_classifiers();
	remove_nodes();
	test_mt_init_and_setup();
	test_mt_frames();
	test_mt_syn_dropped();
	printf("test-evdev: %d/%d checks passed\n", g_tests - g_failures, g_tests);
	return g_failures ? 1 : 0;
}
