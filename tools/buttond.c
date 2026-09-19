/*
 * buttond - side-button gesture daemon for chef-cyclo (README item 14).
 *
 * The phone has three physical buttons and two evdev devices for them:
 * `qpnp_pon` (PMIC, KEY_POWER 116 and the RESIN line as KEY_VOLUMEDOWN 114)
 * and `gpio-keys` (KEY_VOLUMEUP 115). Nothing in Linux turns raw
 * press/release pairs into the gestures a bike computer wants (short,
 * double, long, two-button chords) with more than one consumer, and the
 * evdev layer hands every event to every reader, so this daemon is the
 * single owner of those devices (EVIOCGRAB) and publishes gestures on a
 * Unix stream socket, /run/buttond.sock, in the same lease shape the GPS
 * manager (README 7.5) uses:
 *
 *   client -> buttond    claim power.long        (repeatable)
 *                        release power.long
 *                        watch                   (see every gesture; does
 *                                                 not suppress defaults)
 *                        status
 *   buttond -> client    ok claim power.long | err unknown gesture x | ...
 *                        event power.long        (one line per gesture)
 *
 * A claim lives on the connection: when the client dies its claims vanish
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
 *   power.short  toggle the panel through fblog's documented idle protocol:
 *                create/remove /run/fblog.off and SIGTERM the running fblog,
 *                which init respawns (idling, or back on the panel).
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
 * (registers 0x840.., see the README entry): the power key alone hard-
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
 *   -i  evdev device to use (repeatable; default: every /dev/input/event*
 *       that reports KEY_POWER/KEY_VOLUMEUP/KEY_VOLUMEDOWN and no ABS axes)
 *   -l  long-hold threshold in ms (default 1500)
 *   -d  double-tap window in ms (default 300)
 *   -p  extra hold after power.long before a release powers off
 *       (default 1500; 0 = never power off, only log)
 *   -S  socket path (default /run/buttond.sock)
 *   -o  fblog idle flag (default /run/fblog.off)
 *   -V  vibrator (default /sys/class/timed_output/vibrator/enable, ms)
 *   -G  do not EVIOCGRAB the devices (shared, for experiments)
 *   -v  echo the kmsg notes to stderr as well
 * Client mode, for the telnet shell:
 *   buttond status | watch | claim GESTURE...   (streams events until ^C)
 *
 * The pure parts (gesture recognizer, gesture names, claim registry,
 * protocol line handling, dispatch, the screen toggle and the power-off
 * state machine with injected paths/hooks) are separated from the
 * syscalls and covered by tools/tests/test_buttond.c, which includes this
 * file with BUTTOND_NO_MAIN and never powers anything off.
 */
#define _GNU_SOURCE
#include <dirent.h>
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
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>

#define BUTTOND_SOCK_PATH  "/run/buttond.sock"
#define BUTTOND_OFF_PATH   "/run/fblog.off"
#define BUTTOND_VIB_PATH   "/sys/class/timed_output/vibrator/enable"
#define BUTTOND_LONG_MS    1500
#define BUTTOND_DOUBLE_MS  300
#define BUTTOND_POWEROFF_HOLD_MS 1500	/* after power.long: 3 s held in all */
#define BUTTOND_BUZZ_ARM_MS   100
#define BUTTOND_BUZZ_READY_MS 300
#define BUTTOND_MAX_DEVS   4
#define BUTTOND_MAX_CLIENTS 16
#define BUTTOND_LINE_MAX   128

static bool g_verbose;
static volatile sig_atomic_t g_stop;

static void kmsg_note(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int fd, n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (g_verbose)
		fprintf(stderr, "buttond: %s\n", buf);
	fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	dprintf(fd, "buttond: %s\n", buf);
	close(fd);
}

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

struct client {
	int fd;			/* -1 = free slot */
	bool watch;
	bool discarding;	/* overlong line: drop bytes up to the next newline */
	uint32_t claims;	/* bit per enum gesture */
	char in[BUTTOND_LINE_MAX];
	size_t inlen;
};

enum poweroff_state { PO_IDLE, PO_ARMED, PO_READY };

struct server {
	struct recog recog;
	struct client clients[BUTTOND_MAX_CLIENTS];
	const char *off_path;
	const char *vib_path;
	unsigned poweroff_hold_ms;	/* 0 = the power.long default only logs */
	enum poweroff_state po_state;
	int64_t po_armed_at;		/* when power.long fired */
	unsigned po_presses;		/* recog presses at arm time: re-press cancels */
	/* injected for the tests; the defaults are the real actions */
	void (*log)(const char *fmt, ...);
	int (*kill_comm)(const char *comm, int sig);
	void (*poweroff)(struct server *s);
};

static int kill_by_comm(const char *comm, int sig);
static void system_poweroff(struct server *s);

static void server_init(struct server *s, unsigned long_ms, unsigned double_ms)
{
	int i;

	memset(s, 0, sizeof(*s));
	recog_init(&s->recog, long_ms, double_ms);
	for (i = 0; i < BUTTOND_MAX_CLIENTS; i++)
		s->clients[i].fd = -1;
	s->off_path = BUTTOND_OFF_PATH;
	s->vib_path = BUTTOND_VIB_PATH;
	s->poweroff_hold_ms = BUTTOND_POWEROFF_HOLD_MS;
	s->log = kmsg_note;
	s->kill_comm = kill_by_comm;
	s->poweroff = system_poweroff;
}

static int server_claim_count(const struct server *s, enum gesture g)
{
	int i, n = 0;

	for (i = 0; i < BUTTOND_MAX_CLIENTS; i++)
		if (s->clients[i].fd >= 0 && (s->clients[i].claims & (1u << g)))
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

static struct client *server_add_client(struct server *s, int fd)
{
	int i;

	for (i = 0; i < BUTTOND_MAX_CLIENTS; i++) {
		struct client *c = &s->clients[i];

		if (c->fd >= 0)
			continue;
		memset(c, 0, sizeof(*c));
		c->fd = fd;
		return c;
	}
	return NULL;
}

static void server_drop_client(struct server *s, struct client *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	c->claims = 0;
	c->watch = false;
	server_rearm(s);
}

static int client_send(struct client *c, const char *line)
{
	size_t len = strlen(line), off = 0;

	while (off < len) {
		ssize_t n = send(c->fd, line + off, len - off, MSG_NOSIGNAL | MSG_DONTWAIT);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;	/* EAGAIN too: a client that does not read is dropped */
		}
		off += (size_t)n;
	}
	return 0;
}

static void client_reply(struct server *s, struct client *c, const char *fmt, ...)
{
	char buf[BUTTOND_LINE_MAX * 2];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (client_send(c, buf))
		server_drop_client(s, c);
}

static void server_status_line(const struct server *s, char *buf, size_t len)
{
	size_t off = 0;
	int i, n = 0;

	off += (size_t)snprintf(buf + off, len - off, "ok status");
	for (i = 0; i < BTN_COUNT; i++)
		off += (size_t)snprintf(buf + off, len - off, " %s=%s", button_names[i],
					s->recog.b[i].down ? "down" : "up");
	off += (size_t)snprintf(buf + off, len - off, " long_ms=%u double_ms=%u poweroff_hold_ms=%u poweroff=%s claims=",
				s->recog.long_ms, s->recog.double_ms, s->poweroff_hold_ms,
				s->po_state == PO_IDLE ? "idle" : s->po_state == PO_ARMED ? "armed" : "ready");
	for (i = 0; i < GES_COUNT; i++) {
		int k = server_claim_count(s, (enum gesture)i);

		if (!k)
			continue;
		off += (size_t)snprintf(buf + off, len - off, "%s%s:%d", n++ ? "," : "",
					gesture_names[i], k);
	}
	if (!n)
		off += (size_t)snprintf(buf + off, len - off, "-");
	for (i = 0, n = 0; i < BUTTOND_MAX_CLIENTS; i++)
		if (s->clients[i].fd >= 0 && s->clients[i].watch)
			n++;
	snprintf(buf + off, len - off, " watchers=%d\n", n);
}

/* One complete request line from a client (no trailing newline). */
static void server_handle_line(struct server *s, struct client *c, char *line)
{
	char *cmd, *arg, *save = NULL;

	cmd = strtok_r(line, " \t\r", &save);
	if (!cmd)
		return;
	arg = strtok_r(NULL, " \t\r", &save);
	if (strcmp(cmd, "claim") == 0 || strcmp(cmd, "release") == 0) {
		int g = arg ? gesture_from_name(arg) : -1;

		if (g < 0) {
			client_reply(s, c, "err unknown gesture %s\n", arg ? arg : "");
			return;
		}
		if (cmd[0] == 'c')
			c->claims |= 1u << g;
		else
			c->claims &= ~(1u << g);
		server_rearm(s);
		client_reply(s, c, "ok %s %s\n", cmd, gesture_names[g]);
	} else if (strcmp(cmd, "watch") == 0) {
		c->watch = true;
		client_reply(s, c, "ok watch\n");
	} else if (strcmp(cmd, "status") == 0) {
		char buf[512];

		server_status_line(s, buf, sizeof(buf));
		client_reply(s, c, "%s", buf);
	} else {
		client_reply(s, c, "err unknown command %s\n", cmd);
	}
}

/* Buffer bytes from a client fd and hand complete lines over. */
static void server_client_input(struct server *s, struct client *c)
{
	ssize_t n;
	char *nl;

	n = recv(c->fd, c->in + c->inlen, sizeof(c->in) - 1 - c->inlen, MSG_DONTWAIT);
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
		server_drop_client(s, c);
		return;
	}
	if (n < 0)
		return;
	c->inlen += (size_t)n;
	c->in[c->inlen] = '\0';
	while ((nl = memchr(c->in, '\n', c->inlen))) {
		size_t used = (size_t)(nl - c->in) + 1;

		*nl = '\0';
		if (c->discarding)
			c->discarding = false;	/* the tail of a rejected line */
		else
			server_handle_line(s, c, c->in);
		if (c->fd < 0)
			return;
		memmove(c->in, c->in + used, c->inlen - used);
		c->inlen -= used;
		c->in[c->inlen] = '\0';
	}
	if (c->discarding) {
		c->inlen = 0;
	} else if (c->inlen == sizeof(c->in) - 1) {
		client_reply(s, c, "err line too long\n");
		c->inlen = 0;
		c->discarding = true;
	}
}

/* -------------------------------------------------- default actions */

/*
 * fblog's documented idle protocol: the flag file decides what the
 * respawned fblog does, the signal makes init respawn it now. Toggle =
 * flip the flag, then signal. Returns 1 when the screen was asked to go
 * off, 0 when asked to come back, negative errno if the flag could not be
 * changed (then nothing is signalled).
 */
static int screen_toggle(struct server *s)
{
	bool off = access(s->off_path, F_OK) != 0;	/* absent -> go off */
	int rc, killed;

	if (off) {
		int fd = open(s->off_path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);

		if (fd < 0) {
			s->log("screen off: create %s: %s", s->off_path, strerror(errno));
			return -errno;
		}
		close(fd);
	} else if (unlink(s->off_path) && errno != ENOENT) {
		s->log("screen on: remove %s: %s", s->off_path, strerror(errno));
		return -errno;
	}
	killed = s->kill_comm("fblog", SIGTERM);
	rc = off ? 1 : 0;
	if (killed < 0)
		s->log("screen %s: %s %s, but fblog not running (%s); it will honour the flag when started",
		       off ? "off" : "on", off ? "created" : "removed", s->off_path,
		       strerror(-killed));
	else
		s->log("screen %s: %s %s, signalled fblog (%d process%s); init respawns it",
		       off ? "off" : "on", off ? "created" : "removed", s->off_path,
		       killed, killed == 1 ? "" : "es");
	return rc;
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
	int i, claimed = 0;

	snprintf(line, sizeof(line), "event %s\n", gesture_names[g]);
	for (i = 0; i < BUTTOND_MAX_CLIENTS; i++) {
		struct client *c = &s->clients[i];
		bool claims = (c->claims & (1u << g)) != 0;

		if (c->fd < 0 || !(claims || c->watch))
			continue;
		if (client_send(c, line)) {
			server_drop_client(s, c);
			continue;
		}
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

/* SIGTERM every process whose /proc/PID/comm is `comm` (busybox pidof). */
static int kill_by_comm(const char *comm, int sig)
{
	DIR *d = opendir("/proc");
	struct dirent *de;
	int n = 0;

	if (!d)
		return -errno;
	while ((de = readdir(d))) {
		char path[64], buf[32];
		int fd;
		ssize_t len;
		pid_t pid;

		if (de->d_name[0] < '1' || de->d_name[0] > '9')
			continue;
		pid = (pid_t)atoi(de->d_name);
		if (pid <= 0 || pid == getpid())
			continue;
		snprintf(path, sizeof(path), "/proc/%ld/comm", (long)pid);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		len = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (len <= 0)
			continue;
		buf[len] = '\0';
		if (len && buf[len - 1] == '\n')
			buf[len - 1] = '\0';
		if (strcmp(buf, comm) == 0 && kill(pid, sig) == 0)
			n++;
	}
	closedir(d);
	return n ? n : -ESRCH;
}

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

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool has_bit(const unsigned long *bits, unsigned int bit)
{
	return (bits[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1;
}

struct keydev {
	int fd;
	char path[64];
	char name[64];
	unsigned buttons;	/* bit per enum button */
};

/*
 * Open one evdev node and keep it only if it has at least one of our keys
 * and no absolute axes (the touch panel also reports EV_KEY for BTN_TOUCH
 * and must stay available to fbtouch/the UI). Returns 0, -ENODEV for a
 * device we do not want, or -errno.
 */
static int keydev_open(struct keydev *kd, const char *path, bool grab)
{
	unsigned long evbits[(EV_MAX + 8 * sizeof(long)) / (8 * sizeof(long))];
	unsigned long keybits[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))];
	int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	int b;

	if (fd < 0)
		return -errno;
	memset(evbits, 0, sizeof(evbits));
	memset(keybits, 0, sizeof(keybits));
	if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0 ||
	    !has_bit(evbits, EV_KEY) || has_bit(evbits, EV_ABS) ||
	    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0) {
		close(fd);
		return -ENODEV;
	}
	memset(kd, 0, sizeof(*kd));
	for (b = 0; b < BTN_COUNT; b++) {
		unsigned code = b == BTN_POWER ? KEY_POWER : b == BTN_VOLUP ? KEY_VOLUMEUP : KEY_VOLUMEDOWN;

		if (has_bit(keybits, code))
			kd->buttons |= 1u << b;
	}
	if (!kd->buttons) {
		close(fd);
		return -ENODEV;
	}
	if (grab && ioctl(fd, EVIOCGRAB, (void *)1) < 0) {
		int err = errno;

		close(fd);
		return -err;
	}
	kd->fd = fd;
	snprintf(kd->path, sizeof(kd->path), "%s", path);
	if (ioctl(fd, EVIOCGNAME(sizeof(kd->name)), kd->name) < 0)
		snprintf(kd->name, sizeof(kd->name), "?");
	return 0;
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

static int keydev_scan(struct keydev *kds, int max, bool grab)
{
	DIR *d = opendir("/dev/input");
	struct dirent *de;
	int n = 0;

	if (!d)
		return -errno;
	while ((de = readdir(d)) && n < max) {
		char path[80];
		int rc;

		if (strncmp(de->d_name, "event", 5) || strlen(de->d_name) > 16)
			continue;
		snprintf(path, sizeof(path), "/dev/input/%.16s", de->d_name);
		rc = keydev_open(&kds[n], path, grab);
		if (rc == 0)
			n++;
		else if (rc != -ENODEV)
			kmsg_note("%s: %s (skipped)", path, strerror(-rc));
	}
	closedir(d);
	return n;
}

static int sock_listen(const char *path)
{
	struct sockaddr_un sa;
	int fd;

	if (strlen(path) >= sizeof(sa.sun_path))
		return -ENAMETOOLONG;
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	strcpy(sa.sun_path, path);
	unlink(path);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, 8) < 0) {
		int err = errno;

		close(fd);
		return -err;
	}
	return fd;
}

static int sock_connect(const char *path)
{
	struct sockaddr_un sa;
	int fd;

	if (strlen(path) >= sizeof(sa.sun_path))
		return -ENAMETOOLONG;
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	strcpy(sa.sun_path, path);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		int err = errno;

		close(fd);
		return -err;
	}
	return fd;
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

/* Client mode: send the request lines, then print everything that comes back. */
static int client_main(const char *sock_path, int argc, char **argv)
{
	int fd = sock_connect(sock_path);
	char buf[512];
	int i;

	if (fd < 0) {
		fprintf(stderr, "buttond: connect %s: %s\n", sock_path, strerror(-fd));
		return 1;
	}
	if (strcmp(argv[0], "claim") == 0) {
		if (argc < 2) {
			usage();
			return 64;
		}
		for (i = 1; i < argc; i++) {
			snprintf(buf, sizeof(buf), "claim %s\n", argv[i]);
			if (write(fd, buf, strlen(buf)) < 0)
				return 1;
		}
	} else if (strcmp(argv[0], "status") == 0 || strcmp(argv[0], "watch") == 0) {
		snprintf(buf, sizeof(buf), "%s\n", argv[0]);
		if (write(fd, buf, strlen(buf)) < 0)
			return 1;
	} else {
		usage();
		return 64;
	}
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));

		if (n <= 0)
			break;
		if (fwrite(buf, 1, (size_t)n, stdout) != (size_t)n)
			break;
		fflush(stdout);
		if (strcmp(argv[0], "status") == 0 && memchr(buf, '\n', (size_t)n))
			break;
	}
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	struct server s;
	struct keydev kds[BUTTOND_MAX_DEVS];
	const char *explicit[BUTTOND_MAX_DEVS];
	const char *sock_path = BUTTOND_SOCK_PATH, *off_path = BUTTOND_OFF_PATH;
	const char *vib_path = BUTTOND_VIB_PATH;
	unsigned long_ms = BUTTOND_LONG_MS, double_ms = BUTTOND_DOUBLE_MS;
	unsigned poweroff_hold_ms = BUTTOND_POWEROFF_HOLD_MS;
	bool grab = true;
	int nexplicit = 0, ndev = 0, lfd, opt, i;

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

	server_init(&s, long_ms, double_ms);
	s.off_path = off_path;
	s.vib_path = vib_path;
	s.poweroff_hold_ms = poweroff_hold_ms;

	if (nexplicit) {
		for (i = 0; i < nexplicit; i++) {
			int rc = keydev_open(&kds[ndev], explicit[i], grab);

			if (rc)
				kmsg_note("%s: %s (skipped)", explicit[i], strerror(-rc));
			else
				ndev++;
		}
	} else {
		ndev = keydev_scan(kds, BUTTOND_MAX_DEVS, grab);
		if (ndev < 0) {
			kmsg_note("scan /dev/input: %s", strerror(-ndev));
			ndev = 0;
		}
	}
	if (!ndev) {
		kmsg_note("no button device found; exiting in 5 s");
		sleep(5);	/* keeps a respawning init from spinning */
		return 1;
	}
	for (i = 0; i < ndev; i++) {
		char desc[160];

		keydev_describe(&kds[i], desc, sizeof(desc));
		kmsg_note("%s %s", grab ? "grabbed" : "reading", desc);
	}

	lfd = sock_listen(sock_path);
	if (lfd < 0) {
		kmsg_note("listen %s: %s; exiting in 5 s", sock_path, strerror(-lfd));
		sleep(5);
		return 1;
	}
	if (poweroff_hold_ms)
		kmsg_note("ready: %s, long %u ms, double %u ms, power.short toggles the screen via %s, power held %u ms then released powers off",
			  sock_path, long_ms, double_ms, off_path, long_ms + poweroff_hold_ms);
	else
		kmsg_note("ready: %s, long %u ms, double %u ms, power.short toggles the screen via %s, power-off default disabled",
			  sock_path, long_ms, double_ms, off_path);

	while (!g_stop) {
		struct pollfd pfd[BUTTOND_MAX_DEVS + 1 + BUTTOND_MAX_CLIENTS];
		struct client *cmap[BUTTOND_MAX_CLIENTS];
		int n = 0, nclients = 0, rc;

		for (i = 0; i < ndev; i++)
			pfd[n++] = (struct pollfd){ .fd = kds[i].fd, .events = POLLIN };
		pfd[n++] = (struct pollfd){ .fd = lfd, .events = POLLIN };
		for (i = 0; i < BUTTOND_MAX_CLIENTS; i++) {
			if (s.clients[i].fd < 0)
				continue;
			cmap[nclients++] = &s.clients[i];
			pfd[n++] = (struct pollfd){ .fd = s.clients[i].fd, .events = POLLIN };
		}

		rc = poll(pfd, (nfds_t)n, server_deadline_ms(&s, now_ms()));
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
				kmsg_note("%s: read: %s; exiting for a respawn", kds[i].path,
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
		if (pfd[ndev].revents & POLLIN) {
			int cfd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

			if (cfd >= 0 && !server_add_client(&s, cfd)) {
				kmsg_note("client limit (%d) reached, refusing", BUTTOND_MAX_CLIENTS);
				close(cfd);
			}
		}
		for (i = 0; i < nclients; i++)
			if (pfd[ndev + 1 + i].revents & (POLLIN | POLLERR | POLLHUP))
				server_client_input(&s, cmap[i]);

		{
			int64_t now = now_ms();

			recog_tick(&s.recog, now);
			server_drain(&s, now);
			server_tick(&s, now);
		}
	}

	kmsg_note("exiting");
	for (i = 0; i < BUTTOND_MAX_CLIENTS; i++)
		if (s.clients[i].fd >= 0)
			close(s.clients[i].fd);
	close(lfd);
	unlink(sock_path);
	for (i = 0; i < ndev; i++)
		close(kds[i].fd);	/* releases the grab */
	return 0;
}

#endif /* BUTTOND_NO_MAIN */
