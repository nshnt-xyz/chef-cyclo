/*
 * touch.h - multitouch protocol-B evdev reader for the fbdev backend.
 * No LVGL: chefui.c turns its frames into LVGL pointer reads and gesture
 * recognizer input. Covered by tests/test_touch.c with injected
 * input_event streams and a wrapped ioctl().
 *
 * Why not lv_evdev (prototype findings): it scales from ABS_X/ABS_Y,
 * which the NT36525 does not have (only ABS_MT_POSITION_X/Y, 0..720 x
 * 0..1600); it zeroes a slot's coordinates on release although the input
 * core never resends an unchanged per-slot value (rapid taps then read
 * 0 on one axis); it ignores SYN_DROPPED; and it drains every queued
 * event per read, so a press and release in one batch become one state.
 *
 * Here: events are applied to per-slot state (tracking id, raw x, raw y)
 * and every SYN_REPORT snapshots the slots as one frame into a queue.
 * A pointer read pops exactly one frame and says whether more are queued
 * (LVGL's continue_reading). Coordinates are never cleared on release.
 * SYN_DROPPED discards events up to the next SYN_REPORT, then resyncs the
 * slots with EVIOCGMTSLOTS; if that fails every contact is released.
 *
 * Primary pointer: the first contact down drives the pointer until it
 * lifts, which reads as a release even if other contacts stay down; the
 * next primary starts only once every contact is up.
 */
#ifndef CHEFUI_TOUCH_H
#define CHEFUI_TOUCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <linux/input.h>

#include "copy.h"

#define CU_TOUCH_SLOTS  10
#define CU_TOUCH_QUEUE  64

struct cu_slot {
	int32_t id;		/* tracking id, -1 = no contact */
	int32_t x, y;		/* raw, kept across release */
};

struct cu_touch_frame {
	struct cu_slot s[CU_TOUCH_SLOTS];
};

/* One contact as the application sees it, logical coordinates. */
struct cu_contact {
	int slot;
	int32_t id;
	int32_t x, y;
	bool down;		/* false: lifted in this frame */
};

/* What one read delivers. */
struct cu_touch_state {
	bool pressed;			/* primary pointer */
	int32_t x, y;			/* primary, logical, last known when released */
	struct cu_contact c[CU_TOUCH_SLOTS];	/* down now, or lifted this frame */
	int nc;
	int ndown;
	bool more;			/* further complete frames are queued */
};

struct cu_touch {
	int fd;				/* -1 when no device is open */
	char path[300];
	struct cu_geom geom;
	struct input_absinfo ax, ay;	/* ABS_MT_POSITION_X/Y ranges */
	int nslots;			/* device slots, <= CU_TOUCH_SLOTS */

	/* working state, updated event by event */
	struct cu_slot work[CU_TOUCH_SLOTS];
	int cur_slot;
	bool dropping;			/* after SYN_DROPPED, until SYN_REPORT */
	bool overflow;			/* queue overflowed since the last pop */

	/* complete frames */
	struct cu_touch_frame q[CU_TOUCH_QUEUE];
	unsigned qhead, qcount;

	/* delivered state */
	struct cu_touch_frame last;	/* the frame the last pop delivered */
	int primary;			/* slot, -1 = none */
	int32_t primary_id;
	bool wait_all_up;
	int32_t px, py;			/* primary, logical */

	unsigned long resyncs, drops;
};

/* Zero state for geometry g, no device. */
void cu_touch_init(struct cu_touch *t, const struct cu_geom *g);

/* Raw device coordinates -> logical screen point (scaled, rotated, clamped). */
void cu_touch_map(const struct cu_touch *t, int32_t rx, int32_t ry, int32_t *lx, int32_t *ly);

/* Apply events. Resyncs through t->fd on the SYN_REPORT after a
 * SYN_DROPPED. */
void cu_touch_feed(struct cu_touch *t, const struct input_event *ev, size_t n);

/* Pop one frame into *st (st->more = more are queued). With nothing
 * queued it reports the current delivered state unchanged and returns
 * false; true when a frame was consumed. */
bool cu_touch_pop(struct cu_touch *t, struct cu_touch_state *st);

/* True while any delivered contact is down. */
bool cu_touch_any_down(const struct cu_touch *t);

/* Release every contact: working slots and the queue are cleared and one
 * all-up frame is queued, so the next pop reports the release. */
void cu_touch_release_all(struct cu_touch *t);

/* EVIOCGMTSLOTS for tracking id, X, Y and EVIOCGABS(ABS_MT_SLOT) for the
 * current slot; 0, or -errno (then nothing was changed). */
int cu_touch_resync(struct cu_touch *t);

/* --- device I/O --- */

/* Probe one evdev node: 1 if it is a direct-touch MT device, else 0. */
int cu_touch_is_touchscreen(int fd);

/* Scan dir (normally "/dev/input") for event* nodes and open the first
 * touchscreen nonblocking, reading its axis ranges and slot count.
 * 0, or -ENODEV when none was found. Never grabs. */
int cu_touch_open_scan(struct cu_touch *t, const char *dir);

/* Read everything queued on the fd and feed it. 0, -EAGAIN-free; a
 * negative errno other than EAGAIN/EINTR means the device is gone:
 * the caller closes it (cu_touch_close) and rescans later. */
int cu_touch_read_fd(struct cu_touch *t);

/* Read and discard everything queued on the fd (stale events after the
 * screen was off). */
void cu_touch_drain(struct cu_touch *t);

/* Screen on: drain stale events, drop queued frames, resync the slots
 * (all released if that fails) and queue the result as one frame. */
void cu_touch_wake(struct cu_touch *t);

void cu_touch_close(struct cu_touch *t);

#endif /* CHEFUI_TOUCH_H */
