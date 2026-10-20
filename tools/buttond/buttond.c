/*
 * buttond - side-button gesture daemon for chef-cyclo.
 *
 * The phone has three physical buttons and two evdev devices for them:
 * `qpnp_pon` (PMIC, KEY_POWER 116 and the RESIN line as KEY_VOLUMEDOWN 114)
 * and `gpio-keys` (KEY_VOLUMEUP 115). Nothing in Linux turns raw
 * press/release pairs into the gestures a bike computer wants (short,
 * double, long, two-button chords) with more than one consumer, and the
 * evdev layer hands every event to every reader, so this daemon is the
 * single owner of those devices (EVIOCGRAB) and publishes gestures on a
 * Unix stream socket, /run/chef/buttond.sock, in the same lease shape the GPS
 * manager uses. Requests are words, one line each; every line buttond
 * sends is one JSON object with a "type" member (the shared envelope of
 * docs/features/local-sockets.md, served by tools/common/linesrv.c), proto
 * 1, written down in docs/features/buttons-and-power-off.md:
 *
 *   (connect)          {"type":"hello","daemon":"buttond","proto":1}
 *   claim G            {"type":"ok","cmd":"claim","gesture":"G"}
 *   release G          {"type":"ok","cmd":"release","gesture":"G"}
 *                      (also when G was not claimed)
 *   watch              {"type":"ok","cmd":"watch"}  (every gesture from now
 *                      on; suppresses no default)
 *   status             {"type":"status","buttons":{"power":"up",...},
 *                      "long_ms":1500,"double_ms":300,"poweroff_hold_ms":1500,
 *                      "poweroff":"idle|armed|ready","claims":{"G":N,...},
 *                      "watchers":N}
 *   event              {"type":"gesture","gesture":"power.long"} to every
 *                      claimant of G and every watcher
 *   errors             {"type":"error","cmd":"claim","reason":"unknown
 *                      gesture","gesture":"x"}, {...,"reason":"bad
 *                      arguments"}, {"type":"error","cmd":"x","reason":
 *                      "unknown command"} (an echoed word is its first 32
 *                      bytes, JSON-escaped), and from the library "line too
 *                      long" (over 127 bytes) and "too many clients" (the
 *                      17th connection, then closed)
 *
 * Empty lines are ignored and a CR before the newline is stripped. A
 * client that lets its 4096-byte output queue overflow is dropped. A
 * claim lives on the connection: when the client dies its claims vanish
 * and the built-in default for that gesture resumes. The ride recorder,
 * not the UI, is meant to claim "power.long -> finish ride" for the same
 * reason it holds the durable `ride` GPS lease.
 *
 * Gestures: <button>.short, <button>.double, <button>.long for power,
 * volup, voldown; chords power+volup and power+voldown (fire on the second
 * press; both releases are then swallowed). `long` fires at the hold
 * threshold while the key is still down (so a consumer can buzz before the
 * release), and that release emits nothing. Double detection costs a
 * `double_ms` wait after the first release before a lone tap can be called
 * `short`, so it is armed for a button only while some client claims that
 * button's `.double`; otherwise `short` fires on release immediately and
 * the screen toggle stays snappy.
 *
 * Built-in defaults (used when nobody claims the gesture):
 *   power.short  toggle the system-wide screen-off flag
 *                /run/chef/screen-off: off creates it, on removes it,
 *                nothing else. Unclaimed means no chefui application runs,
 *                so nobody holds fb0 and the panel is already off; the next
 *                screen owner honours the flag (a hand-run fblog watches
 *                it). chefui applications claim power.short and use the
 *                same flag.
 *   power.long   clean power-off, in three steps so a 1.5 s hold on its own
 *                does nothing irreversible: at the long threshold a short
 *                buzz ("keep holding, release to cancel"); if the key is
 *                still down `poweroff_hold_ms` later (1.5 s more, 3 s in
 *                all) a long buzz ("release to power off"); the release
 *                then does sync() and runs `poweroff`, i.e. busybox init's
 *                orderly shutdown (::shutdown entries, SIGTERM to every
 *                process -- gps-up's cleanup included -- then RB_POWER_OFF).
 *                Releasing before the long buzz cancels; a second press
 *                in between cancels too. -p 0 disables this default.
 *   everything else is only logged for now (power menu = UI work).
 *
 * Why release, why 3 s: the PM660's own PON block was read on 2026-09-19
 * (registers 0x840.., see the 2026-09-19 (power-off, live) entry in
 * docs/build-log.md): the power key alone hard-
 * resets the PMIC after S1 6720 ms + S2 2000 ms = ~8.7 s held, with no
 * bark interrupt wired to the kernel, so software gets no warning; the
 * RESIN (voldown) and KPDPWR+RESIN combo resets are disabled there. KPDPWR
 * and USB are enabled PON triggers, so a key still held while the PMIC
 * powers down would turn the phone straight back on -- hence the action is
 * on release. 3 s leaves >5 s before the hardware reset, and holding past
 * it just means the PMIC reboots the phone (into the flashed slot). The
 * "Power+VolDown" escape to Android is that same 8.7 s reset with VolDown
 * held into the bootloader; the software power-menu chord is power+volup.
 *
 * Usage: buttond [-i DEV]... [-l LONG_MS] [-d DOUBLE_MS] [-p HOLD_MS]
 *                [-S SOCK] [-o OFF_FILE] [-V VIB] [-G] [-v]
 *   -i  evdev device to use (repeatable; default: every /dev/input/event*,
 *       in numeric order, that reports KEY_POWER/KEY_VOLUMEUP/KEY_VOLUMEDOWN
 *       and no ABS axes: tools/evdev.h's evdev_scan + evdev_key_mask)
 *   -l  long-hold threshold in ms (default 1500)
 *   -d  double-tap window in ms (default 300)
 *   -p  extra hold after power.long before a release powers off
 *       (default 1500; 0 = never power off, only log)
 *   -S  socket path (default /run/chef/buttond.sock)
 *   -o  screen-off flag (default /run/chef/screen-off)
 *   -V  vibrator (default /sys/class/timed_output/vibrator/enable, ms)
 *   -G  do not EVIOCGRAB the devices (shared, for experiments)
 *   -v  echo the log lines to stderr as well
 * Client mode, for the shell (the hello is never printed):
 *   buttond status            the status line; exit 0, or 1 on an error
 *                             reply or no connection
 *   buttond watch             then every later line raw, one JSON object
 *   buttond claim GESTURE...  per line, until EOF or ^C
 *
 * The pure parts (gesture recognizer, gesture names, claim registry,
 * protocol line handling, dispatch, the screen toggle and the power-off
 * state machine with injected paths/hooks) are separated from the
 * syscalls and covered by tools/buttond/tests/test_buttond.c, which
 * includes this file with BUTTOND_NO_MAIN and never powers anything off.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>

#include "evdev.h"
#include "common/chef_log.h"
#include "common/chef_paths.h"
#include "common/json.h"
#include "common/linesrv.h"

#define BUTTOND_VIB_PATH   "/sys/class/timed_output/vibrator/enable"
#define BUTTOND_LONG_MS    1500
#define BUTTOND_DOUBLE_MS  300
#define BUTTOND_POWEROFF_HOLD_MS 1500	/* after power.long: 3 s held in all */
#define BUTTOND_BUZZ_ARM_MS   100
#define BUTTOND_BUZZ_READY_MS 300
#define BUTTOND_MAX_DEVS   4
#define BUTTOND_MAX_CLIENTS 16
#define BUTTOND_LINE_MAX   128	/* a request line, its newline included */
#define BUTTOND_OUTQ       4096	/* per-client output queue */
#define BUTTOND_PROTO      1
#define BUTTOND_ECHO_MAX   32	/* bytes of a bad word echoed in an error */

static volatile sig_atomic_t g_stop;

/* ------------------------------------------------------------ buttons */

enum button { BTN_POWER, BTN_VOLUP, BTN_VOLDOWN, BTN_COUNT };

static const char *const button_names[BTN_COUNT] = { "power", "volup", "voldown" };

static int button_from_code(unsigned code)
{
	switch (code) {
	case KEY_POWER: return BTN_POWER;
	case KEY_VOLUMEUP: return BTN_VOLUP;
	case KEY_VOLUMEDOWN: return BTN_VOLDOWN;
	default: return -1;
	}
}

/* ----------------------------------------------------------- gestures */

enum gesture {
	GES_POWER_SHORT, GES_POWER_DOUBLE, GES_POWER_LONG,
	GES_VOLUP_SHORT, GES_VOLUP_DOUBLE, GES_VOLUP_LONG,
	GES_VOLDOWN_SHORT, GES_VOLDOWN_DOUBLE, GES_VOLDOWN_LONG,
	GES_POWER_VOLUP, GES_POWER_VOLDOWN,
	GES_COUNT
};

static const char *const gesture_names[GES_COUNT] = {
	"power.short", "power.double", "power.long",
	"volup.short", "volup.double", "volup.long",
	"voldown.short", "voldown.double", "voldown.long",
	"power+volup", "power+voldown",
};

enum kind { KIND_SHORT, KIND_DOUBLE, KIND_LONG };

static enum gesture gesture_of(enum button b, enum kind k)
{
	return (enum gesture)(b * 3 + k);
}

static int gesture_from_name(const char *name)
{
	int i;

	for (i = 0; i < GES_COUNT; i++)
		if (strcmp(name, gesture_names[i]) == 0)
			return i;
	return -1;
}

/* --------------------------------------------------------- recognizer */

struct btn_state {
	bool down;
	bool consumed;		/* long or chord fired: the release is swallowed */
	bool pending;		/* one short tap released, waiting for a second */
	int taps;		/* presses in the current tap sequence */
	unsigned presses;	/* every press ever, so a release+press is visible */
	int64_t down_at;
	int64_t pending_until;
};

struct recog {
	unsigned long_ms;
	unsigned double_ms;
	bool double_armed[BTN_COUNT];
	struct btn_state b[BTN_COUNT];
	int out[8];		/* gestures fired since the last drain, in order */
	int nout;
	int dropped;		/* fired with a full queue (never happens in practice) */
};

static void recog_init(struct recog *r, unsigned long_ms, unsigned double_ms)
{
	memset(r, 0, sizeof(*r));
	r->long_ms = long_ms;
	r->double_ms = double_ms;
}

static void recog_emit(struct recog *r, enum gesture g)
{
	if (r->nout < (int)(sizeof(r->out) / sizeof(r->out[0])))
		r->out[r->nout++] = g;
	else
		r->dropped++;
}

/* Returns the next fired gesture, or -1 when the queue is empty. */
static int recog_pop(struct recog *r)
{
	int g;

	if (!r->nout)
		return -1;
	g = r->out[0];
	memmove(r->out, r->out + 1, (size_t)(r->nout - 1) * sizeof(r->out[0]));
	r->nout--;
	return g;
}

static void btn_reset(struct btn_state *s)
{
	s->consumed = false;
	s->pending = false;
	s->taps = 0;
}

/* A chord partner is a key currently held that has fired nothing yet. */
static bool btn_chordable(const struct btn_state *s)
{
	return s->down && !s->consumed;
}

static void recog_press(struct recog *r, enum button b, int64_t now)
{
	struct btn_state *s = &r->b[b];
	struct btn_state *p = &r->b[BTN_POWER];

	if (s->down)
		return;		/* duplicate press (or repeat reported as 1) */
	s->down = true;
	s->down_at = now;
	s->consumed = false;
	s->presses++;
	if (b != BTN_POWER && btn_chordable(p)) {
		recog_emit(r, b == BTN_VOLUP ? GES_POWER_VOLUP : GES_POWER_VOLDOWN);
		p->consumed = true;
		p->pending = false;
		p->taps = 0;
		s->consumed = true;
		s->pending = false;
		s->taps = 0;
		return;
	}
	if (b == BTN_POWER) {
		enum button other = btn_chordable(&r->b[BTN_VOLUP]) ? BTN_VOLUP :
				    btn_chordable(&r->b[BTN_VOLDOWN]) ? BTN_VOLDOWN : BTN_COUNT;
		if (other != BTN_COUNT) {
			recog_emit(r, other == BTN_VOLUP ? GES_POWER_VOLUP : GES_POWER_VOLDOWN);
			r->b[other].consumed = true;
			r->b[other].pending = false;
			r->b[other].taps = 0;
			s->consumed = true;
			s->pending = false;
			s->taps = 0;
			return;
		}
	}
	if (s->pending) {
		s->pending = false;
		s->taps++;
	} else {
		s->taps = 1;
	}
}

static void recog_release(struct recog *r, enum button b, int64_t now)
{
	struct btn_state *s = &r->b[b];

	if (!s->down)
		return;		/* held at startup, or a duplicate release */
	s->down = false;
	if (s->consumed) {
		btn_reset(s);
		return;
	}
	if (now - s->down_at >= (int64_t)r->long_ms) {
		/* the tick that should have fired this was late */
		recog_emit(r, gesture_of(b, KIND_LONG));
		btn_reset(s);
		return;
	}
	if (s->taps >= 2) {
		recog_emit(r, gesture_of(b, KIND_DOUBLE));
		btn_reset(s);
		return;
	}
	if (r->double_armed[b]) {
		s->pending = true;
		s->pending_until = now + (int64_t)r->double_ms;
		return;
	}
	recog_emit(r, gesture_of(b, KIND_SHORT));
	btn_reset(s);
}

/* Feed one evdev key event. value: 1 press, 0 release, 2 autorepeat. */
static void recog_key(struct recog *r, enum button b, int value, int64_t now)
{
	if (value == 1)
		recog_press(r, b, now);
	else if (value == 0)
		recog_release(r, b, now);
	/* autorepeat carries nothing the hold timer does not */
}

/* Fire whatever deadline has passed: long holds and expired tap windows. */
static void recog_tick(struct recog *r, int64_t now)
{
	int b;

	for (b = 0; b < BTN_COUNT; b++) {
		struct btn_state *s = &r->b[b];

		if (s->down && !s->consumed && now - s->down_at >= (int64_t)r->long_ms) {
			recog_emit(r, gesture_of((enum button)b, KIND_LONG));
			s->consumed = true;
			s->pending = false;
			s->taps = 0;
		}
		if (s->pending && now >= s->pending_until) {
			recog_emit(r, gesture_of((enum button)b, KIND_SHORT));
			btn_reset(s);
		}
	}
}

/* Milliseconds until recog_tick() has something to do, or -1 for never. */
static int recog_deadline_ms(const struct recog *r, int64_t now)
{
	int64_t best = -1;
	int b;

	for (b = 0; b < BTN_COUNT; b++) {
		const struct btn_state *s = &r->b[b];
		int64_t t;

		if (s->down && !s->consumed) {
			t = s->down_at + (int64_t)r->long_ms;
			if (best < 0 || t < best)
				best = t;
		}
		if (s->pending) {
			t = s->pending_until;
			if (best < 0 || t < best)
				best = t;
		}
	}
	if (best < 0)
		return -1;
	if (best <= now)
		return 0;
	return best - now > INT32_MAX ? INT32_MAX : (int)(best - now);
}

/* ------------------------------------------------- clients and claims */

/* buttond's own state per connection, indexed by the linesrv slot. */
struct client {
	bool watch;
	uint32_t claims;	/* bit per enum gesture */
};

enum poweroff_state { PO_IDLE, PO_ARMED, PO_READY };

struct server {
	struct recog recog;
	struct linesrv *srv;
	struct client clients[BUTTOND_MAX_CLIENTS];
	const char *off_path;
	const char *vib_path;
	unsigned poweroff_hold_ms;	/* 0 = the power.long default only logs */
	enum poweroff_state po_state;
	int64_t po_armed_at;		/* when power.long fired */
	unsigned po_presses;		/* recog presses at arm time: re-press cancels */
	/* injected for the tests; the defaults are the real actions */
	void (*log)(const char *fmt, ...);
	void (*poweroff)(struct server *s);
};

static void system_poweroff(struct server *s);
static void on_connect(struct linesrv *srv, struct linesrv_client *c);
static void on_line(struct linesrv *srv, struct linesrv_client *c, int argc, char **argv);
static void on_close(struct linesrv *srv, struct linesrv_client *c);

/* LOG NULL = chef_log; the socket library logs through it too. 0, or
 * -ENOMEM. */
static int server_init(struct server *s, unsigned long_ms, unsigned double_ms,
		       void (*log)(const char *fmt, ...))
{
	struct linesrv_config cfg = {
		.max_clients = BUTTOND_MAX_CLIENTS,
		.line_max = BUTTOND_LINE_MAX,
		.outq_size = BUTTOND_OUTQ,
		.on_connect = on_connect,
		.on_line = on_line,
		.on_close = on_close,
		.ctx = s,
		.log = log ? log : chef_log,
	};

	memset(s, 0, sizeof(*s));
	recog_init(&s->recog, long_ms, double_ms);
	s->off_path = CHEF_SCREEN_OFF;
	s->vib_path = BUTTOND_VIB_PATH;
	s->poweroff_hold_ms = BUTTOND_POWEROFF_HOLD_MS;
	s->log = cfg.log;
	s->poweroff = system_poweroff;
	s->srv = linesrv_new(&cfg);
	return s->srv ? 0 : -ENOMEM;
}

/* Every client closed (claims released), the socket path unlinked. */
static void server_fini(struct server *s)
{
	linesrv_close_all(s->srv);
	s->srv = NULL;
}

static int server_claim_count(const struct server *s, enum gesture g)
{
	int i, n = 0;

	for (i = 0; i < BUTTOND_MAX_CLIENTS; i++)
		if (linesrv_client_at(s->srv, i) && (s->clients[i].claims & (1u << g)))
			n++;
	return n;
}

/* Double detection is only worth its delay while someone wants doubles. */
static void server_rearm(struct server *s)
{
	int b;

	for (b = 0; b < BTN_COUNT; b++)
		s->recog.double_armed[b] =
			server_claim_count(s, gesture_of((enum button)b, KIND_DOUBLE)) > 0;
}

static struct client *client_of(struct server *s, struct linesrv_client *c)
{
	return &s->clients[linesrv_slot(c)];
}

static void on_connect(struct linesrv *srv, struct linesrv_client *c)
{
	struct server *s = linesrv_ctx(srv);

	memset(client_of(s, c), 0, sizeof(struct client));
	linesrv_sendf(c, "{\"type\":\"hello\",\"daemon\":\"buttond\",\"proto\":%d}\n", BUTTOND_PROTO);
}

/* Exactly once per connection, whatever ended it: its claims go. */
static void on_close(struct linesrv *srv, struct linesrv_client *c)
{
	struct server *s = linesrv_ctx(srv);

	memset(client_of(s, c), 0, sizeof(struct client));
	server_rearm(s);
}

static void server_status_line(const struct server *s, char *buf, size_t len)
{
	size_t off = 0;
	int i, n = 0;

	off += (size_t)snprintf(buf + off, len - off, "{\"type\":\"status\",\"buttons\":{");
	for (i = 0; i < BTN_COUNT; i++)
		off += (size_t)snprintf(buf + off, len - off, "%s\"%s\":\"%s\"", i ? "," : "",
					button_names[i], s->recog.b[i].down ? "down" : "up");
	off += (size_t)snprintf(buf + off, len - off,
				"},\"long_ms\":%u,\"double_ms\":%u,\"poweroff_hold_ms\":%u,\"poweroff\":\"%s\",\"claims\":{",
				s->recog.long_ms, s->recog.double_ms, s->poweroff_hold_ms,
				s->po_state == PO_IDLE ? "idle" : s->po_state == PO_ARMED ? "armed" : "ready");
	for (i = 0; i < GES_COUNT; i++) {
		int k = server_claim_count(s, (enum gesture)i);

		if (!k)
			continue;
		off += (size_t)snprintf(buf + off, len - off, "%s\"%s\":%d", n++ ? "," : "",
					gesture_names[i], k);
	}
	for (i = 0, n = 0; i < BUTTOND_MAX_CLIENTS; i++)
		if (linesrv_client_at(s->srv, i) && s->clients[i].watch)
			n++;
	snprintf(buf + off, len - off, "},\"watchers\":%d}\n", n);
}

/* One request, already split into words by the library. */
static void on_line(struct linesrv *srv, struct linesrv_client *c, int argc, char **argv)
{
	struct server *s = linesrv_ctx(srv);
	struct client *cl = client_of(s, c);
	const char *cmd = argv[0];
	char esc[BUTTOND_ECHO_MAX * 6 + 1];

	if (strcmp(cmd, "claim") == 0 || strcmp(cmd, "release") == 0) {
		int g;

		if (argc != 2) {
			linesrv_sendf(c, "{\"type\":\"error\",\"cmd\":\"%s\",\"reason\":\"bad arguments\"}\n", cmd);
			return;
		}
		g = gesture_from_name(argv[1]);
		if (g < 0) {
			json_escape_n(esc, sizeof(esc), argv[1], BUTTOND_ECHO_MAX);
			linesrv_sendf(c, "{\"type\":\"error\",\"cmd\":\"%s\",\"reason\":\"unknown gesture\",\"gesture\":\"%s\"}\n",
				      cmd, esc);
			return;
		}
		if (cmd[0] == 'c')
			cl->claims |= 1u << g;
		else
			cl->claims &= ~(1u << g);
		server_rearm(s);
		linesrv_sendf(c, "{\"type\":\"ok\",\"cmd\":\"%s\",\"gesture\":\"%s\"}\n", cmd, gesture_names[g]);
	} else if (strcmp(cmd, "watch") == 0 || strcmp(cmd, "status") == 0) {
		char buf[768];

		if (argc != 1) {
			linesrv_sendf(c, "{\"type\":\"error\",\"cmd\":\"%s\",\"reason\":\"bad arguments\"}\n", cmd);
			return;
		}
		if (cmd[0] == 'w') {
			cl->watch = true;
			linesrv_sendf(c, "{\"type\":\"ok\",\"cmd\":\"watch\"}\n");
			return;
		}
		server_status_line(s, buf, sizeof(buf));
		linesrv_send(c, buf, strlen(buf));
	} else {
		json_escape_n(esc, sizeof(esc), cmd, BUTTOND_ECHO_MAX);
		linesrv_sendf(c, "{\"type\":\"error\",\"cmd\":\"%s\",\"reason\":\"unknown command\"}\n", esc);
	}
}

/* -------------------------------------------------- default actions */

/*
 * Screen-off creates the flag, screen-on removes it; nothing else. The
 * next screen owner honours it (a hand-run fblog watches it while
 * running). Returns 1 for off, 0 for on, or negative errno on flag failure.
 */
static int screen_toggle(struct server *s)
{
	bool off = access(s->off_path, F_OK) != 0;	/* absent -> go off */
	int fd, e;

	if (off) {
		fd = open(s->off_path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
		if (fd < 0) {
			e = errno;
			s->log("screen off: create %s: %s", s->off_path, strerror(e));
			return -e;
		}
		close(fd);
		s->log("screen off: created %s", s->off_path);
		return 1;
	}
	if (unlink(s->off_path) && errno != ENOENT) {
		e = errno;
		s->log("screen on: remove %s: %s", s->off_path, strerror(e));
		return -e;
	}
	s->log("screen on: removed %s", s->off_path);
	return 0;
}

/* timed_output: a duration in ms, the driver stops by itself. */
static void buzz(struct server *s, unsigned ms)
{
	int fd = open(s->vib_path, O_WRONLY | O_CLOEXEC);

	if (fd < 0)
		return;
	dprintf(fd, "%u\n", ms);
	close(fd);
}

/*
 * power.long default, step 1: arm. The release before the READY buzz
 * cancels (server_tick), so a plain 1.5 s hold is harmless.
 */
static void poweroff_arm(struct server *s, int64_t now)
{
	if (!s->poweroff_hold_ms) {
		s->log("power.long unclaimed: power-off disabled (-p 0)");
		return;
	}
	s->po_state = PO_ARMED;
	s->po_armed_at = now;
	s->po_presses = s->recog.b[BTN_POWER].presses;
	buzz(s, BUTTOND_BUZZ_ARM_MS);
	s->log("power.long: keep holding %u ms more to power off, release now to cancel",
	       s->poweroff_hold_ms);
}

/* Steps 2 and 3: READY once held long enough, act on the release. */
static void server_tick(struct server *s, int64_t now)
{
	const struct btn_state *p = &s->recog.b[BTN_POWER];
	bool released = !p->down || p->presses != s->po_presses;

	if (s->po_state == PO_IDLE)
		return;
	if (released) {
		enum poweroff_state was = s->po_state;

		s->po_state = PO_IDLE;
		if (was == PO_READY) {
			s->log("power off: released after %lld ms, shutting down now",
			       (long long)(now - s->po_armed_at) + s->recog.long_ms);
			buzz(s, BUTTOND_BUZZ_READY_MS);
			s->poweroff(s);
		} else {
			s->log("power off cancelled: released after %lld ms",
			       (long long)(now - s->po_armed_at) + s->recog.long_ms);
		}
		return;
	}
	if (s->po_state == PO_ARMED && now - s->po_armed_at >= (int64_t)s->poweroff_hold_ms) {
		s->po_state = PO_READY;
		buzz(s, BUTTOND_BUZZ_READY_MS);
		s->log("power off ready: release the button to power off");
	}
}

/* Poll timeout: the recognizer's deadline or the READY transition. */
static int server_deadline_ms(const struct server *s, int64_t now)
{
	int d = recog_deadline_ms(&s->recog, now);

	if (s->po_state == PO_ARMED) {
		int64_t t = s->po_armed_at + (int64_t)s->poweroff_hold_ms - now;
		int p = t <= 0 ? 0 : t > INT32_MAX ? INT32_MAX : (int)t;

		if (d < 0 || p < d)
			d = p;
	}
	return d;
}

static void server_default_action(struct server *s, enum gesture g, int64_t now)
{
	switch (g) {
	case GES_POWER_SHORT:
		screen_toggle(s);
		break;
	case GES_POWER_LONG:
		poweroff_arm(s, now);
		break;
	case GES_POWER_VOLUP:
	case GES_POWER_VOLDOWN:
		s->log("%s unclaimed: no default yet (power menu is UI work)",
		       gesture_names[g]);
		break;
	default:
		s->log("%s unclaimed: no default", gesture_names[g]);
		break;
	}
}

/* Deliver one fired gesture: claimants and watchers, else the default. */
static void server_dispatch(struct server *s, enum gesture g, int64_t now)
{
	char line[64];
	int i, n, claimed = 0;

	n = snprintf(line, sizeof(line), "{\"type\":\"gesture\",\"gesture\":\"%s\"}\n", gesture_names[g]);
	for (i = 0; i < BUTTOND_MAX_CLIENTS; i++) {
		struct linesrv_client *c = linesrv_client_at(s->srv, i);
		bool claims = (s->clients[i].claims & (1u << g)) != 0;

		if (!c || !(claims || s->clients[i].watch))
			continue;
		/* a client that does not take it is dropped, claims and all */
		if (linesrv_send(c, line, (size_t)n))
			continue;
		if (claims)
			claimed++;
	}
	if (claimed)
		s->log("%s -> %d claimant%s", gesture_names[g], claimed, claimed == 1 ? "" : "s");
	else
		server_default_action(s, g, now);
}

static void server_drain(struct server *s, int64_t now)
{
	int g;

	while ((g = recog_pop(&s->recog)) >= 0)
		server_dispatch(s, (enum gesture)g, now);
}

/* --------------------------------------------------------- syscalls */

/*
 * The orderly way down: sync, then `poweroff` (busybox: SIGUSR2 to init,
 * which runs the ::shutdown entries, SIGTERMs everything, SIGKILLs a
 * second later and calls reboot(RB_POWER_OFF)). If the applet is not there
 * the same signal is sent directly; if even init will not take it, the
 * last resort is the syscall after a sync -- the user is holding the
 * power button asking for exactly this.
 */
static void system_poweroff(struct server *s)
{
	pid_t pid;
	int st = -1;

	sync();
	pid = fork();
	if (pid == 0) {
		execlp("poweroff", "poweroff", (char *)NULL);
		_exit(127);
	}
	if (pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0)
		return;
	s->log("poweroff command failed (status %d); signalling init directly", st);
	if (kill(1, SIGUSR2) == 0)
		return;
	s->log("init would not take SIGUSR2 (%s); reboot(RB_POWER_OFF)", strerror(errno));
	sync();
	reboot(RB_POWER_OFF);
}

#ifndef BUTTOND_NO_MAIN

static bool g_verbose;

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

struct keydev {
	int fd;
	char path[64];
	char name[64];
	unsigned buttons;	/* bit per enum button */
};

/* Key code per enum button, for evdev_key_mask(). */
static const unsigned short button_codes[BTN_COUNT] = { KEY_POWER, KEY_VOLUMEUP, KEY_VOLUMEDOWN };

/*
 * Take over an open evdev fd only if it has at least one of our keys and
 * no absolute axes (the touch panel also reports EV_KEY for BTN_TOUCH and
 * must stay available to fbtouch/the UI). Returns 0, -ENODEV for a device
 * we do not want, or -errno; on failure the caller closes fd.
 */
static int keydev_setup(struct keydev *kd, int fd, const char *path, bool grab)
{
	unsigned buttons = evdev_key_mask(fd, button_codes, BTN_COUNT);

	if (!buttons)
		return -ENODEV;
	if (grab && ioctl(fd, EVIOCGRAB, (void *)1) < 0)
		return -errno;
	memset(kd, 0, sizeof(*kd));
	kd->fd = fd;
	kd->buttons = buttons;
	snprintf(kd->path, sizeof(kd->path), "%s", path);
	if (ioctl(fd, EVIOCGNAME(sizeof(kd->name)), kd->name) < 0)
		snprintf(kd->name, sizeof(kd->name), "?");
	return 0;
}

/* Open one evdev node and keep it as keydev_setup() decides. */
static int keydev_open(struct keydev *kd, const char *path, bool grab)
{
	int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC), rc;

	if (fd < 0)
		return -errno;
	rc = keydev_setup(kd, fd, path, grab);
	if (rc)
		close(fd);
	return rc;
}

static void keydev_describe(const struct keydev *kd, char *buf, size_t len)
{
	size_t off = 0;
	int b;

	off += (size_t)snprintf(buf + off, len - off, "%s (%s):", kd->path, kd->name);
	for (b = 0; b < BTN_COUNT; b++)
		if (kd->buttons & (1u << b))
			off += (size_t)snprintf(buf + off, len - off, " %s", button_names[b]);
}

struct keydev_scan_ctx {
	struct keydev *kds;
	int max, n;
	bool grab;
};

/* evdev_scan() callback: keep every button device, up to max. fd < 0 is
 * a node that did not open (-errno). */
static int keydev_scan_one(int fd, const char *path, void *arg)
{
	struct keydev_scan_ctx *c = arg;
	int rc = fd < 0 ? fd : keydev_setup(&c->kds[c->n], fd, path, c->grab);

	if (rc) {
		if (rc != -ENODEV)
			chef_log("%s: %s (skipped)", path, strerror(-rc));
		return EVDEV_SCAN_REJECT;
	}
	return ++c->n < c->max ? EVDEV_SCAN_KEEP : EVDEV_SCAN_KEEP_STOP;
}

static int keydev_scan(struct keydev *kds, int max, bool grab)
{
	struct keydev_scan_ctx c = { kds, max, 0, grab };

	if (max <= 0)
		return 0;
	return evdev_scan("/dev/input", keydev_scan_one, &c);
}

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: buttond [-i DEV]... [-l LONG_MS] [-d DOUBLE_MS] [-p HOLD_MS] [-S SOCK] [-o OFF_FILE] [-V VIB] [-G] [-v]\n"
		"       buttond [-S SOCK] status | watch | claim GESTURE...\n");
}

/* The "type" of one daemon line, or "" when it has none. */
static const char *line_type(const char *line, size_t len, char *buf, size_t buflen)
{
	struct json_obj o;

	if (json_parse(&o, line, len) || !json_get_string(&o, "type", buf, buflen))
		buf[0] = '\0';
	return buf;
}

/* Client mode: send the request lines, then print what comes back, one
 * JSON object per line, without the hello. */
static int client_main(const char *sock_path, int argc, char **argv)
{
	bool claim = strcmp(argv[0], "claim") == 0, status = strcmp(argv[0], "status") == 0;
	char buf[4096], type[16];
	size_t len = 0;
	int fd, i;

	if (claim ? argc < 2 : !status && strcmp(argv[0], "watch") != 0) {
		usage();
		return 64;
	}
	fd = linesrv_connect(sock_path);
	if (fd < 0) {
		fprintf(stderr, "buttond: connect %s: %s\n", sock_path, strerror(-fd));
		return 1;
	}
	/* MSG_NOSIGNAL: a connection buttond refused (too many clients) is an
	 * error message and exit 1, not death by SIGPIPE */
	for (i = claim ? 1 : 0; i < (claim ? argc : 1); i++) {
		snprintf(buf, sizeof(buf), claim ? "claim %s\n" : "%s\n", argv[i]);
		if (send(fd, buf, strlen(buf), MSG_NOSIGNAL) < 0) {
			fprintf(stderr, "buttond: send to %s: %s\n", sock_path, strerror(errno));
			close(fd);
			return 1;
		}
	}
	for (;;) {
		ssize_t n = read(fd, buf + len, sizeof(buf) - len);
		char *nl;

		if (n <= 0)
			break;
		len += (size_t)n;
		while ((nl = memchr(buf, '\n', len))) {
			size_t used = (size_t)(nl - buf) + 1;

			line_type(buf, used - 1, type, sizeof(type));
			if (status && strcmp(type, "status") == 0) {
				fwrite(buf, 1, used, stdout);
				close(fd);
				return 0;
			}
			if (status && strcmp(type, "error") == 0) {
				fwrite(buf, 1, used, stderr);
				close(fd);
				return 1;
			}
			if (!status && strcmp(type, "hello") != 0) {
				if (fwrite(buf, 1, used, stdout) != used)
					goto out;
				fflush(stdout);
			}
			memmove(buf, buf + used, len - used);
			len -= used;
		}
		if (len == sizeof(buf)) {	/* no line is this long: pass it on */
			if (!status)
				fwrite(buf, 1, len, stdout);
			len = 0;
		}
	}
out:
	close(fd);
	if (status) {
		fprintf(stderr, "buttond: no status reply\n");
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct server s;
	struct keydev kds[BUTTOND_MAX_DEVS];
	const char *explicit[BUTTOND_MAX_DEVS];
	const char *sock_path = CHEF_BUTTOND_SOCK, *off_path = CHEF_SCREEN_OFF;
	const char *vib_path = BUTTOND_VIB_PATH;
	unsigned long_ms = BUTTOND_LONG_MS, double_ms = BUTTOND_DOUBLE_MS;
	unsigned poweroff_hold_ms = BUTTOND_POWEROFF_HOLD_MS;
	bool grab = true;
	int nexplicit = 0, ndev = 0, nsrv, rc, opt, i;

	while ((opt = getopt(argc, argv, "i:l:d:p:S:o:V:Gv")) != -1) {
		switch (opt) {
		case 'i':
			if (nexplicit == BUTTOND_MAX_DEVS) {
				usage();
				return 64;
			}
			explicit[nexplicit++] = optarg;
			break;
		case 'l': long_ms = (unsigned)atoi(optarg); break;
		case 'd': double_ms = (unsigned)atoi(optarg); break;
		case 'p': poweroff_hold_ms = (unsigned)atoi(optarg); break;
		case 'S': sock_path = optarg; break;
		case 'o': off_path = optarg; break;
		case 'V': vib_path = optarg; break;
		case 'G': grab = false; break;
		case 'v': g_verbose = true; break;
		default: usage(); return 64;
		}
	}
	if (optind < argc)
		return client_main(sock_path, argc - optind, argv + optind);
	if (long_ms < 200 || double_ms < 50 || double_ms >= long_ms) {
		usage();
		return 64;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	chef_log_init("buttond", g_verbose);
	if (server_init(&s, long_ms, double_ms, NULL)) {
		chef_log("out of memory; exiting in 5 s");
		sleep(5);
		return 1;
	}
	s.off_path = off_path;
	s.vib_path = vib_path;
	s.poweroff_hold_ms = poweroff_hold_ms;

	if (nexplicit) {
		for (i = 0; i < nexplicit; i++) {
			int rc = keydev_open(&kds[ndev], explicit[i], grab);

			if (rc)
				chef_log("%s: %s (skipped)", explicit[i], strerror(-rc));
			else
				ndev++;
		}
	} else {
		ndev = keydev_scan(kds, BUTTOND_MAX_DEVS, grab);
		if (ndev < 0) {
			chef_log("scan /dev/input: %s", strerror(-ndev));
			ndev = 0;
		}
	}
	if (!ndev) {
		chef_log("no button device found; exiting in 5 s");
		sleep(5);	/* keeps a respawning init from spinning */
		return 1;
	}
	for (i = 0; i < ndev; i++) {
		char desc[160];

		keydev_describe(&kds[i], desc, sizeof(desc));
		chef_log("%s %s", grab ? "grabbed" : "reading", desc);
	}

	rc = linesrv_listen(s.srv, sock_path);
	if (rc < 0) {
		chef_log("listen %s: %s; exiting in 5 s", sock_path, strerror(-rc));
		sleep(5);
		return 1;
	}
	if (poweroff_hold_ms)
		chef_log("ready: %s, long %u ms, double %u ms, power.short toggles the screen via %s, power held %u ms then released powers off",
			  sock_path, long_ms, double_ms, off_path, long_ms + poweroff_hold_ms);
	else
		chef_log("ready: %s, long %u ms, double %u ms, power.short toggles the screen via %s, power-off default disabled",
			  sock_path, long_ms, double_ms, off_path);

	while (!g_stop) {
		struct pollfd pfd[BUTTOND_MAX_DEVS + 1 + BUTTOND_MAX_CLIENTS];

		for (i = 0; i < ndev; i++)
			pfd[i] = (struct pollfd){ .fd = kds[i].fd, .events = POLLIN };
		nsrv = linesrv_pollfds(s.srv, pfd + ndev, 1 + BUTTOND_MAX_CLIENTS);

		rc = poll(pfd, (nfds_t)(ndev + nsrv), server_deadline_ms(&s, now_ms()));
		if (rc < 0 && errno != EINTR)
			break;
		if (g_stop)
			break;

		for (i = 0; i < ndev; i++) {
			struct input_event ev[16];
			ssize_t len;

			if (!(pfd[i].revents & (POLLIN | POLLERR | POLLHUP)))
				continue;
			len = read(kds[i].fd, ev, sizeof(ev));
			if (len < 0 && (errno == EAGAIN || errno == EINTR))
				continue;
			if (len <= 0) {
				chef_log("%s: read: %s; exiting for a respawn", kds[i].path,
					  len ? strerror(errno) : "EOF");
				g_stop = 1;
				break;
			}
			for (size_t k = 0; k < (size_t)len / sizeof(ev[0]); k++) {
				int b;

				if (ev[k].type != EV_KEY)
					continue;
				b = button_from_code(ev[k].code);
				if (b < 0)
					continue;
				if (g_verbose)
					fprintf(stderr, "buttond: %s %s\n", button_names[b],
						ev[k].value == 1 ? "press" : ev[k].value == 0 ? "release" : "repeat");
				recog_key(&s.recog, (enum button)b, ev[k].value, now_ms());
			}
		}
		if (g_stop)
			break;
		if (rc > 0)
			linesrv_handle(s.srv, pfd + ndev, nsrv);

		{
			int64_t now = now_ms();

			recog_tick(&s.recog, now);
			server_drain(&s, now);
			server_tick(&s, now);
		}
	}

	chef_log("exiting");
	server_fini(&s);
	for (i = 0; i < ndev; i++)
		close(kds[i].fd);	/* releases the grab */
	return 0;
}

#endif /* BUTTOND_NO_MAIN */
