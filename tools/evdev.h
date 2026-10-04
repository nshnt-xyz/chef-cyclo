/*
 * evdev.h - the chef-cyclo evdev plumbing, shared by every program that
 * reads input devices directly (the chefui UI platform's touch.c, fbtouch,
 * buttond). Header-only, static functions, libc + <linux/input.h> only, so
 * single-file builds (mkinitramfs.sh's `$MUSLCC ... tools/fbtouch.c`) need
 * nothing extra. There is no libinput/libevdev/mtdev on the phone.
 *
 *   evdev_bit()            test a bit in an EVIOCGBIT/EVIOCGPROP bitmap
 *                          declared with EVDEV_BITMAP(name, max)
 *   evdev_is_touchscreen() INPUT_PROP_DIRECT plus ABS_MT_POSITION_X/Y
 *   evdev_key_mask()       which of some key codes a device reports; 0 for
 *                          a device without EV_KEY or with EV_ABS (the touch
 *                          panel also reports BTN_TOUCH and must never be
 *                          taken for a button device)
 *   evdev_scan()           visit dir's event* nodes in ascending numeric
 *                          order (event2 before event10), whatever order
 *                          readdir returns; never grabs
 *   evdev_mt_*()           multitouch protocol B slot decoder
 *
 * The classifiers only issue read-only EVIOCG* ioctls on an fd the caller
 * opened; nothing here grabs, writes or reads events except
 * evdev_mt_feed()'s resync.
 *
 * MT-B decoding (the NT36525 reports ABS_MT_SLOT, ABS_MT_TRACKING_ID and
 * ABS_MT_POSITION_X/Y, 10 slots): events update per-slot state (tracking
 * id, raw x, raw y) and every SYN_REPORT completes a frame, which the
 * caller reads from mt->slots. A slot's coordinates are never cleared on
 * release: the input core drops a per-slot value equal to the one last
 * sent, so a new contact at the same X (or Y) as the previous one in that
 * slot arrives without that axis. SYN_DROPPED means the kernel buffer
 * overflowed: events up to the next SYN_REPORT are discarded, then the
 * slots are re-read with EVIOCGMTSLOTS (and the current slot with
 * EVIOCGABS(ABS_MT_SLOT)) and that SYN_REPORT completes a frame with the
 * resynced state; if the re-read fails every contact is released.
 */
#ifndef CHEF_CYCLO_EVDEV_H
#define CHEF_CYCLO_EVDEV_H

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/input.h>

/* ---------------------------------------------------------------- bits */

#define EVDEV_LONG_BITS     (8 * sizeof(unsigned long))
#define EVDEV_NLONGS(nbits) (((nbits) + EVDEV_LONG_BITS - 1) / EVDEV_LONG_BITS)
/* A bitmap for codes 0..max (EV_MAX, KEY_MAX, ABS_MAX, INPUT_PROP_MAX):
 * pass sizeof(name) as the ioctl length. */
#define EVDEV_BITMAP(name, max) unsigned long name[EVDEV_NLONGS((max) + 1)]

static inline bool evdev_bit(const unsigned long *bits, unsigned int bit)
{
	return (bits[bit / EVDEV_LONG_BITS] >> (bit % EVDEV_LONG_BITS)) & 1;
}

/* ------------------------------------------------------- classification */

/* A direct-touch multitouch device: INPUT_PROP_DIRECT (a touchscreen, not
 * a touchpad) and ABS_MT_POSITION_X/Y. False when either query fails. */
static inline bool evdev_is_touchscreen(int fd)
{
	EVDEV_BITMAP(props, INPUT_PROP_MAX);
	EVDEV_BITMAP(abs, ABS_MAX);

	memset(props, 0, sizeof(props));
	memset(abs, 0, sizeof(abs));
	if (ioctl(fd, EVIOCGPROP(sizeof(props)), props) < 0 ||
	    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) < 0)
		return false;
	return evdev_bit(props, INPUT_PROP_DIRECT) && evdev_bit(abs, ABS_MT_POSITION_X) &&
	       evdev_bit(abs, ABS_MT_POSITION_Y);
}

/* Bit i set when the device reports key codes[i] (n <= 32). 0 when it has
 * no EV_KEY, has EV_ABS, or a query fails. */
static inline uint32_t evdev_key_mask(int fd, const unsigned short *codes, unsigned int n)
{
	EVDEV_BITMAP(evbits, EV_MAX);
	EVDEV_BITMAP(keybits, KEY_MAX);
	uint32_t mask = 0;
	unsigned int i;

	memset(evbits, 0, sizeof(evbits));
	memset(keybits, 0, sizeof(keybits));
	if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0 ||
	    !evdev_bit(evbits, EV_KEY) || evdev_bit(evbits, EV_ABS) ||
	    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0)
		return 0;
	for (i = 0; i < n && i < 32; i++)
		if (codes[i] <= KEY_MAX && evdev_bit(keybits, codes[i]))
			mask |= UINT32_C(1) << i;
	return mask;
}

/* ----------------------------------------------------------------- scan */

#define EVDEV_PATH_MAX 300

/* evdev_scan() callback results. The callback owns a kept fd. */
enum {
	EVDEV_SCAN_REJECT = 0,		/* the scan closes the fd and goes on */
	EVDEV_SCAN_KEEP = 1,		/* keep it, go on */
	EVDEV_SCAN_KEEP_STOP = 2,	/* keep it, stop scanning */
};

typedef int (*evdev_scan_fn)(int fd, const char *path, void *ctx);

/* N for a node named "eventN" (decimal, no sign or leading zero), else -1. */
static inline long evdev_node_number(const char *name)
{
	const char *p = name + 5;
	long v = 0;

	if (strncmp(name, "event", 5) || !*p || (*p == '0' && p[1]))
		return -1;
	for (; *p; p++) {
		if (*p < '0' || *p > '9' || v > 99999)
			return -1;
		v = v * 10 + (*p - '0');
	}
	return v;
}

/*
 * Open each event* node of dir (normally "/dev/input") in ascending
 * numeric order, O_RDONLY|O_NONBLOCK|O_CLOEXEC, and pass it to cb, whose
 * result (EVDEV_SCAN_*) says whether the fd is kept and whether to go on;
 * a rejected fd is closed here. A node that does not open is passed as
 * fd = -errno, so the callback can log it: the callback must not keep a
 * negative fd (whatever it returns, it is not counted or closed, and only
 * EVDEV_SCAN_KEEP_STOP stops the scan). Returns the number of fds kept,
 * or -errno when dir cannot be read. Never grabs: that is the caller's
 * decision.
 */
static inline int evdev_scan(const char *dir, evdev_scan_fn cb, void *ctx)
{
	DIR *d = opendir(dir);
	long last = -1;
	int kept = 0;

	if (!d)
		return -errno;
	/* one pass per node, each picking the next number up: no allocation,
	 * and the handful of input nodes makes the quadratic cost moot */
	for (;;) {
		char path[EVDEV_PATH_MAX];
		struct dirent *de;
		long next = -1;
		int fd, rc;

		rewinddir(d);
		while ((de = readdir(d))) {
			long n = evdev_node_number(de->d_name);

			if (n > last && (next < 0 || n < next))
				next = n;
		}
		if (next < 0)
			break;
		last = next;
		if (snprintf(path, sizeof(path), "%s/event%ld", dir, next) >= (int)sizeof(path))
			continue;
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) {
			if (cb(-errno, path, ctx) == EVDEV_SCAN_KEEP_STOP)
				break;
			continue;
		}
		rc = cb(fd, path, ctx);
		if (rc != EVDEV_SCAN_KEEP && rc != EVDEV_SCAN_KEEP_STOP) {
			close(fd);
			continue;
		}
		kept++;
		if (rc == EVDEV_SCAN_KEEP_STOP)
			break;
	}
	closedir(d);
	return kept;
}

/* --------------------------------------------------------- MT-B decoder */

#define EVDEV_MT_SLOTS 10

struct evdev_slot {
	int32_t id;		/* tracking id, -1 = no contact */
	int32_t x, y;		/* raw, kept across release */
};

struct evdev_mt {
	struct evdev_slot slots[EVDEV_MT_SLOTS];
	int cur;		/* current slot, -1 = out of range (its events are ignored) */
	int nslots;		/* device slots, <= EVDEV_MT_SLOTS */
	bool dropping;		/* after SYN_DROPPED, until SYN_REPORT */
	unsigned long drops, resyncs;
};

/* Lift every contact; coordinates are kept. */
static inline void evdev_mt_release_all(struct evdev_mt *mt)
{
	int i;

	for (i = 0; i < EVDEV_MT_SLOTS; i++)
		mt->slots[i].id = -1;
}

/* No contacts, slot 0 current, EVDEV_MT_SLOTS device slots, counters 0. */
static inline void evdev_mt_init(struct evdev_mt *mt)
{
	memset(mt, 0, sizeof(*mt));
	evdev_mt_release_all(mt);
	mt->nslots = EVDEV_MT_SLOTS;
}

/*
 * A newly opened device: slot count from ABS_MT_SLOT's maximum (capped at
 * EVDEV_MT_SLOTS; 1 when the query fails or the device has no slots),
 * contacts cleared, slot 0 current, no pending SYN_DROPPED. The counters
 * are kept. Follow with evdev_mt_resync() for contacts already down.
 * 0, or -errno from the query.
 */
static inline int evdev_mt_setup(struct evdev_mt *mt, int fd)
{
	struct input_absinfo slot;
	int rc = 0, i;

	if (ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &slot) < 0) {
		rc = -errno;
		mt->nslots = 1;
	} else if (slot.maximum < 0) {
		mt->nslots = 1;
	} else {
		mt->nslots = slot.maximum + 1 < EVDEV_MT_SLOTS ? slot.maximum + 1 : EVDEV_MT_SLOTS;
	}
	for (i = 0; i < EVDEV_MT_SLOTS; i++) {
		mt->slots[i].id = -1;
		mt->slots[i].x = 0;
		mt->slots[i].y = 0;
	}
	mt->cur = 0;
	mt->dropping = false;
	return rc;
}

/*
 * Re-read every slot's tracking id and position (EVIOCGMTSLOTS) and the
 * current slot (EVIOCGABS(ABS_MT_SLOT)) from fd; slots beyond the
 * device's are released. 0, or -errno (then nothing was changed).
 */
static inline int evdev_mt_resync(struct evdev_mt *mt, int fd)
{
	struct {
		__u32 code;
		__s32 v[EVDEV_MT_SLOTS];
	} id, x, y;
	struct input_absinfo slot;
	int i, n = mt->nslots < EVDEV_MT_SLOTS ? mt->nslots : EVDEV_MT_SLOTS;

	if (fd < 0)
		return -EBADF;
	memset(&id, 0, sizeof(id));
	memset(&x, 0, sizeof(x));
	memset(&y, 0, sizeof(y));
	id.code = ABS_MT_TRACKING_ID;
	x.code = ABS_MT_POSITION_X;
	y.code = ABS_MT_POSITION_Y;
	if (ioctl(fd, EVIOCGMTSLOTS(sizeof(id)), &id) < 0 ||
	    ioctl(fd, EVIOCGMTSLOTS(sizeof(x)), &x) < 0 ||
	    ioctl(fd, EVIOCGMTSLOTS(sizeof(y)), &y) < 0 ||
	    ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &slot) < 0)
		return -errno;
	for (i = 0; i < EVDEV_MT_SLOTS; i++) {
		if (i < n) {
			mt->slots[i].id = id.v[i] < 0 ? -1 : id.v[i];
			mt->slots[i].x = x.v[i];
			mt->slots[i].y = y.v[i];
		} else {
			mt->slots[i].id = -1;
		}
	}
	mt->cur = slot.value >= 0 && slot.value < EVDEV_MT_SLOTS ? slot.value : -1;
	return 0;
}

/*
 * Apply one event. True when it completed a frame, now in mt->slots: a
 * SYN_REPORT, or the SYN_REPORT ending a SYN_DROPPED discard, after the
 * resync through fd (all contacts released if that fails).
 */
static inline bool evdev_mt_feed(struct evdev_mt *mt, int fd, const struct input_event *e)
{
	if (mt->dropping) {
		if (e->type != EV_SYN || e->code != SYN_REPORT)
			return false;
		mt->dropping = false;
		if (evdev_mt_resync(mt, fd) == 0)
			mt->resyncs++;
		else
			evdev_mt_release_all(mt);
		return true;
	}
	if (e->type == EV_SYN) {
		if (e->code == SYN_REPORT)
			return true;
		if (e->code == SYN_DROPPED) {
			mt->dropping = true;
			mt->drops++;
		}
		return false;
	}
	if (e->type != EV_ABS)
		return false;
	if (e->code == ABS_MT_SLOT) {
		mt->cur = e->value >= 0 && e->value < EVDEV_MT_SLOTS ? e->value : -1;
		return false;
	}
	if (mt->cur < 0)
		return false;
	switch (e->code) {
	case ABS_MT_TRACKING_ID:
		mt->slots[mt->cur].id = e->value < 0 ? -1 : e->value;
		break;
	case ABS_MT_POSITION_X:
		mt->slots[mt->cur].x = e->value;
		break;
	case ABS_MT_POSITION_Y:
		mt->slots[mt->cur].y = e->value;
		break;
	default:
		break;
	}
	return false;
}

#endif /* CHEF_CYCLO_EVDEV_H */
