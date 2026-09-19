/* Host tests for tools/buttond.c: the gesture recognizer (short on release
 * when doubles are not armed, double/single with the tap window when they
 * are, long at the hold threshold from the tick and from a late release,
 * chords in both press orders, ignored releases/repeats, deadlines), the
 * gesture name table, the claim registry and its double-arming, the line
 * protocol over real socketpairs (partial lines, bad input, dead peers),
 * dispatch to claimants/watchers vs. the built-in default, the screen
 * toggle against a temp flag file with an injected kill, and the power-off
 * state machine (arm at long, cancel on early release or re-press, READY
 * buzz, act on release, claimed/-p 0 variants) with an injected poweroff
 * and a temp vibrator file. buttond.c is included with BUTTOND_NO_MAIN; no
 * evdev device, socket path or process is touched and nothing powers off.
 * Build/run: see tools/Makefile ("make test").
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>

#define BUTTOND_NO_MAIN
#include "../buttond.c"

static int g_failures;
static int g_tests;

#define CHECK(cond) do { \
	g_tests++; \
	if (!(cond)) { \
		g_failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

/* ---- helpers ---- */

static char g_log[8][256];
static int g_nlog;

static void test_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(g_log[g_nlog % 8], sizeof(g_log[0]), fmt, ap);
	va_end(ap);
	g_nlog++;
}

static const char *last_log(void)
{
	return g_nlog ? g_log[(g_nlog - 1) % 8] : "";
}

static char g_killed[32];
static int g_kill_rc = 1;

static int test_kill(const char *comm, int sig)
{
	snprintf(g_killed, sizeof(g_killed), "%s/%d", comm, sig);
	return g_kill_rc;
}

static int g_poweroffs;

static void test_poweroff(struct server *s)
{
	(void)s;
	g_poweroffs++;
}

/* Last value written to the fake vibrator (then emptied), -1 if none. */
static int vib_read(const char *path)
{
	FILE *f = fopen(path, "r");
	int v = -1;

	if (f) {
		if (fscanf(f, "%d", &v) != 1)
			v = -1;
		fclose(f);
	}
	f = fopen(path, "w");		/* the sysfs node always exists: empty, not unlink */
	if (f)
		fclose(f);
	return v;
}

/* Drain the recognizer into an array; returns the count. */
static int fired(struct recog *r, int *out, int max)
{
	int n = 0, g;

	while ((g = recog_pop(r)) >= 0 && n < max)
		out[n++] = g;
	return n;
}

static int recv_line(int fd, char *buf, size_t len)
{
	ssize_t n = recv(fd, buf, len - 1, MSG_DONTWAIT);

	if (n <= 0) {
		buf[0] = '\0';
		return (int)n;
	}
	buf[n] = '\0';
	return (int)n;
}

/* ---- recognizer ---- */

static void test_recog_short_unarmed(void)
{
	struct recog r;
	int g[8];

	recog_init(&r, 1500, 300);
	CHECK(recog_deadline_ms(&r, 0) == -1);
	recog_key(&r, BTN_POWER, 1, 1000);
	CHECK(recog_deadline_ms(&r, 1000) == 1500);
	CHECK(recog_deadline_ms(&r, 1400) == 1100);
	CHECK(fired(&r, g, 8) == 0);
	recog_key(&r, BTN_POWER, 0, 1100);
	CHECK(fired(&r, g, 8) == 1 && g[0] == GES_POWER_SHORT);
	CHECK(recog_deadline_ms(&r, 1100) == -1);
	/* a second tap right after is another short, not a double */
	recog_key(&r, BTN_POWER, 1, 1150);
	recog_key(&r, BTN_POWER, 0, 1200);
	CHECK(fired(&r, g, 8) == 1 && g[0] == GES_POWER_SHORT);
}

static void test_recog_long(void)
{
	struct recog r;
	int g[8];

	recog_init(&r, 1500, 300);
	recog_key(&r, BTN_POWER, 1, 0);
	recog_tick(&r, 1499);
	CHECK(fired(&r, g, 8) == 0);
	CHECK(recog_deadline_ms(&r, 1499) == 1);
	recog_tick(&r, 1500);
	CHECK(fired(&r, g, 8) == 1 && g[0] == GES_POWER_LONG);
	CHECK(recog_deadline_ms(&r, 1500) == -1);	/* consumed: no more deadlines */
	recog_tick(&r, 3000);
	CHECK(fired(&r, g, 8) == 0);			/* long fires once */
	recog_key(&r, BTN_POWER, 0, 4000);
	CHECK(fired(&r, g, 8) == 0);			/* release after long is silent */
	/* late release with no tick in between still counts as long */
	recog_key(&r, BTN_POWER, 1, 5000);
	recog_key(&r, BTN_POWER, 0, 7000);
	CHECK(fired(&r, g, 8) == 1 && g[0] == GES_POWER_LONG);
	/* autorepeat does not change anything */
	recog_key(&r, BTN_POWER, 1, 8000);
	recog_key(&r, BTN_POWER, 2, 8100);
	recog_key(&r, BTN_POWER, 2, 8200);
	recog_key(&r, BTN_POWER, 0, 8300);
	CHECK(fired(&r, g, 8) == 1 && g[0] == GES_POWER_SHORT);
}

static void test_recog_double_armed(void)
{
	struct recog r;
	int g[8];

	recog_init(&r, 1500, 300);
	r.double_armed[BTN_POWER] = true;
	/* tap, tap -> double */
	recog_key(&r, BTN_POWER, 1, 0);
	recog_key(&r, BTN_POWER, 0, 100);
	CHECK(fired(&r, g, 8) == 0);			/* held back for the window */
	CHECK(recog_deadline_ms(&r, 100) == 300);
	recog_key(&r, BTN_POWER, 1, 250);
	CHECK(recog_deadline_ms(&r, 250) == 1500);	/* now the long timer */
	recog_key(&r, BTN_POWER, 0, 350);
	CHECK(fired(&r, g, 8) == 1 && g[0] == GES_POWER_DOUBLE);
	CHECK(recog_deadline_ms(&r, 350) == -1);
	/* lone tap -> short once the window expires */
	recog_key(&r, BTN_POWER, 1, 1000);
	recog_key(&r, BTN_POWER, 0, 1100);
	recog_tick(&r, 1399);
	CHECK(fired(&r, g, 8) == 0);
	recog_tick(&r, 1400);
	CHECK(fired(&r, g, 8) == 1 && g[0] == GES_POWER_SHORT);
	/* tap then a long hold -> long, not double */
	recog_key(&r, BTN_POWER, 1, 2000);
	recog_key(&r, BTN_POWER, 0, 2100);
	recog_key(&r, BTN_POWER, 1, 2200);
	recog_tick(&r, 3700);
	CHECK(fired(&r, g, 8) == 1 && g[0] == GES_POWER_LONG);
	recog_key(&r, BTN_POWER, 0, 3800);
	CHECK(fired(&r, g, 8) == 0);
	/* three quick taps: double, then the third starts a new sequence */
	recog_key(&r, BTN_POWER, 1, 5000);
	recog_key(&r, BTN_POWER, 0, 5050);
	recog_key(&r, BTN_POWER, 1, 5100);
	recog_key(&r, BTN_POWER, 0, 5150);
	recog_key(&r, BTN_POWER, 1, 5200);
	recog_key(&r, BTN_POWER, 0, 5250);
	recog_tick(&r, 5600);
	CHECK(fired(&r, g, 8) == 2 && g[0] == GES_POWER_DOUBLE && g[1] == GES_POWER_SHORT);
	/* volume buttons are unaffected by power's arming */
	recog_key(&r, BTN_VOLUP, 1, 6000);
	recog_key(&r, BTN_VOLUP, 0, 6050);
	CHECK(fired(&r, g, 8) == 1 && g[0] == GES_VOLUP_SHORT);
}

static void test_recog_chords(void)
{
	struct recog r;
	int g[8];

	recog_init(&r, 1500, 300);
	/* power first */
	recog_key(&r, BTN_POWER, 1, 0);
	recog_key(&r, BTN_VOLUP, 1, 200);
	CHECK(fired(&r, g, 8) == 1 && g[0] == GES_POWER_VOLUP);
	CHECK(recog_deadline_ms(&r, 200) == -1);	/* both consumed, no long timers */
	recog_key(&r, BTN_VOLUP, 0, 300);
	recog_key(&r, BTN_POWER, 0, 400);
	CHECK(fired(&r, g, 8) == 0);
	/* volume first */
	recog_key(&r, BTN_VOLDOWN, 1, 1000);
	recog_key(&r, BTN_POWER, 1, 1100);
	CHECK(fired(&r, g, 8) == 1 && g[0] == GES_POWER_VOLDOWN);
	recog_key(&r, BTN_POWER, 0, 1200);
	recog_key(&r, BTN_VOLDOWN, 0, 1300);
	CHECK(fired(&r, g, 8) == 0);
	/* chord even when held past the long threshold: nothing else fires */
	recog_key(&r, BTN_POWER, 1, 2000);
	recog_key(&r, BTN_VOLUP, 1, 2100);
	recog_tick(&r, 5000);
	recog_key(&r, BTN_POWER, 0, 5100);
	recog_key(&r, BTN_VOLUP, 0, 5200);
	CHECK(fired(&r, g, 8) == 1 && g[0] == GES_POWER_VOLUP);
	/* after power.long fired, a volume press is plain (power no longer chordable) */
	recog_key(&r, BTN_POWER, 1, 6000);
	recog_tick(&r, 7500);
	recog_key(&r, BTN_VOLUP, 1, 7600);
	recog_key(&r, BTN_VOLUP, 0, 7700);
	recog_key(&r, BTN_POWER, 0, 7800);
	CHECK(fired(&r, g, 8) == 2 && g[0] == GES_POWER_LONG && g[1] == GES_VOLUP_SHORT);
	/* volup + voldown together is not a chord: two independent buttons */
	recog_key(&r, BTN_VOLUP, 1, 8000);
	recog_key(&r, BTN_VOLDOWN, 1, 8050);
	recog_key(&r, BTN_VOLUP, 0, 8100);
	recog_key(&r, BTN_VOLDOWN, 0, 8150);
	CHECK(fired(&r, g, 8) == 2 && g[0] == GES_VOLUP_SHORT && g[1] == GES_VOLDOWN_SHORT);
}

static void test_recog_edges(void)
{
	struct recog r;
	int g[8];
	int i;

	recog_init(&r, 1500, 300);
	/* release of a key that was down before we started: ignored */
	recog_key(&r, BTN_POWER, 0, 0);
	CHECK(fired(&r, g, 8) == 0);
	CHECK(recog_deadline_ms(&r, 0) == -1);
	/* duplicate press keeps the original timestamp */
	recog_key(&r, BTN_POWER, 1, 100);
	recog_key(&r, BTN_POWER, 1, 1000);
	recog_tick(&r, 1600);
	CHECK(fired(&r, g, 8) == 1 && g[0] == GES_POWER_LONG);
	recog_key(&r, BTN_POWER, 0, 1700);
	/* deadline is the earliest of several */
	recog_key(&r, BTN_VOLUP, 1, 2000);
	recog_key(&r, BTN_VOLDOWN, 1, 2100);
	CHECK(recog_deadline_ms(&r, 2100) == 1400);
	recog_tick(&r, 3600);
	CHECK(fired(&r, g, 8) == 2 && g[0] == GES_VOLUP_LONG && g[1] == GES_VOLDOWN_LONG);
	recog_key(&r, BTN_VOLUP, 0, 3700);
	recog_key(&r, BTN_VOLDOWN, 0, 3700);
	CHECK(fired(&r, g, 8) == 0);
	/* queue overflow is counted, not overrun */
	for (i = 0; i < 10; i++) {
		recog_key(&r, BTN_VOLUP, 1, 5000 + i * 100);
		recog_key(&r, BTN_VOLUP, 0, 5050 + i * 100);
	}
	CHECK(r.nout == 8 && r.dropped == 2);
	CHECK(fired(&r, g, 8) == 8);
	CHECK(recog_pop(&r) == -1);
}

/* ---- names ---- */

static void test_names(void)
{
	int i;

	for (i = 0; i < GES_COUNT; i++)
		CHECK(gesture_from_name(gesture_names[i]) == i);
	CHECK(gesture_from_name("power") == -1);
	CHECK(gesture_from_name("") == -1);
	CHECK(gesture_from_name("power.short ") == -1);
	CHECK(gesture_of(BTN_POWER, KIND_SHORT) == GES_POWER_SHORT);
	CHECK(gesture_of(BTN_VOLDOWN, KIND_LONG) == GES_VOLDOWN_LONG);
	CHECK(strcmp(gesture_names[gesture_of(BTN_VOLUP, KIND_DOUBLE)], "volup.double") == 0);
	CHECK(button_from_code(KEY_POWER) == BTN_POWER);
	CHECK(button_from_code(KEY_VOLUMEUP) == BTN_VOLUP);
	CHECK(button_from_code(KEY_VOLUMEDOWN) == BTN_VOLDOWN);
	CHECK(button_from_code(KEY_A) == -1);
	CHECK(button_from_code(BTN_TOUCH) == -1);
}

/* ---- server: claims and protocol ---- */

struct peer {
	struct client *c;
	int fd;		/* our end */
};

static struct peer connect_peer(struct server *s)
{
	struct peer p;
	int sv[2];

	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	p.fd = sv[0];
	p.c = server_add_client(s, sv[1]);
	CHECK(p.c != NULL);
	return p;
}

static void say(struct server *s, struct peer *p, const char *text)
{
	CHECK(send(p->fd, text, strlen(text), 0) == (ssize_t)strlen(text));
	server_client_input(s, p->c);
}

static void test_server_claims(void)
{
	struct server s;
	struct peer a, b;
	char buf[512];

	server_init(&s, 1500, 300);
	s.log = test_log;
	s.kill_comm = test_kill;
	CHECK(s.off_path && strcmp(s.off_path, "/run/fblog.off") == 0);

	a = connect_peer(&s);
	say(&s, &a, "claim power.long\n");
	recv_line(a.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, "ok claim power.long\n") == 0);
	CHECK(server_claim_count(&s, GES_POWER_LONG) == 1);
	CHECK(!s.recog.double_armed[BTN_POWER]);

	say(&s, &a, "claim power.double\n");
	recv_line(a.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, "ok claim power.double\n") == 0);
	CHECK(s.recog.double_armed[BTN_POWER]);
	CHECK(!s.recog.double_armed[BTN_VOLUP]);

	say(&s, &a, "claim nope\n");
	recv_line(a.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, "err unknown gesture nope\n") == 0);
	say(&s, &a, "claim\n");
	recv_line(a.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, "err unknown gesture \n") == 0);
	say(&s, &a, "frobnicate x\n");
	recv_line(a.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, "err unknown command frobnicate\n") == 0);
	say(&s, &a, "\n");			/* empty line: ignored */
	CHECK(recv_line(a.fd, buf, sizeof(buf)) < 0);

	/* status */
	say(&s, &a, "status\n");
	recv_line(a.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, "ok status power=up volup=up voldown=up long_ms=1500 double_ms=300 "
			  "poweroff_hold_ms=1500 poweroff=idle claims=power.double:1,power.long:1 watchers=0\n") == 0);

	/* a second claimant, then release/disconnect bookkeeping */
	b = connect_peer(&s);
	say(&s, &b, "claim power.double\nwatch\n");	/* two lines in one recv */
	recv_line(b.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, "ok claim power.double\nok watch\n") == 0);
	CHECK(server_claim_count(&s, GES_POWER_DOUBLE) == 2);
	say(&s, &a, "release power.double\n");
	recv_line(a.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, "ok release power.double\n") == 0);
	CHECK(server_claim_count(&s, GES_POWER_DOUBLE) == 1);
	CHECK(s.recog.double_armed[BTN_POWER]);	/* b still holds it */
	say(&s, &a, "release power.double\n");	/* releasing twice is fine */
	recv_line(a.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, "ok release power.double\n") == 0);

	s.recog.b[BTN_POWER].down = true;
	say(&s, &b, "status\n");
	recv_line(b.fd, buf, sizeof(buf));
	CHECK(strstr(buf, "power=down volup=up") != NULL);
	CHECK(strstr(buf, "claims=power.double:1,power.long:1 watchers=1") != NULL);
	s.recog.b[BTN_POWER].down = false;

	/* peer b goes away: its claims vanish and doubles disarm */
	close(b.fd);
	server_client_input(&s, b.c);
	CHECK(b.c->fd == -1);
	CHECK(server_claim_count(&s, GES_POWER_DOUBLE) == 0);
	CHECK(!s.recog.double_armed[BTN_POWER]);
	CHECK(server_claim_count(&s, GES_POWER_LONG) == 1);

	/* partial lines are assembled across reads */
	say(&s, &a, "cla");
	CHECK(recv_line(a.fd, buf, sizeof(buf)) < 0);
	say(&s, &a, "im volup.sho");
	CHECK(recv_line(a.fd, buf, sizeof(buf)) < 0);
	say(&s, &a, "rt\n");
	recv_line(a.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, "ok claim volup.short\n") == 0);
	/* CRLF and extra whitespace tolerated */
	say(&s, &a, "claim  voldown.long \r\n");
	recv_line(a.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, "ok claim voldown.long\n") == 0);

	/* an overlong line is rejected once and its tail discarded up to the
	 * newline, so the next command is not corrupted by the leftovers */
	memset(buf, 'x', 300);
	buf[300] = '\0';
	say(&s, &a, buf);
	recv_line(a.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, "err line too long\n") == 0);
	server_client_input(&s, a.c);		/* the rest of the x's */
	CHECK(recv_line(a.fd, buf, sizeof(buf)) < 0);
	say(&s, &a, "yyy\nstatus\n");		/* end of the bad line, then a good one */
	recv_line(a.fd, buf, sizeof(buf));
	CHECK(strncmp(buf, "ok status", 9) == 0);
	CHECK(strchr(buf, '\n') == buf + strlen(buf) - 1);	/* exactly one reply */

	close(a.fd);
	server_drop_client(&s, a.c);
	CHECK(server_claim_count(&s, GES_POWER_LONG) == 0);

	/* the client table is bounded */
	{
		struct peer ps[BUTTOND_MAX_CLIENTS];
		int sv[2], i;

		for (i = 0; i < BUTTOND_MAX_CLIENTS; i++)
			ps[i] = connect_peer(&s);
		CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
		CHECK(server_add_client(&s, sv[1]) == NULL);
		close(sv[0]);
		close(sv[1]);
		for (i = 0; i < BUTTOND_MAX_CLIENTS; i++) {
			close(ps[i].fd);
			server_drop_client(&s, ps[i].c);
		}
	}
}

/* ---- server: dispatch and defaults ---- */

static void test_server_dispatch(void)
{
	struct server s;
	struct peer a, w;
	char buf[512];
	char dir[] = "/tmp/buttond-test-XXXXXX";
	char off[128];
	int rc;

	CHECK(mkdtemp(dir) != NULL);
	snprintf(off, sizeof(off), "%s/fblog.off", dir);

	server_init(&s, 1500, 300);
	s.log = test_log;
	s.kill_comm = test_kill;
	s.off_path = off;

	/* nobody claims power.short: default toggles the screen off, then on */
	g_killed[0] = '\0';
	server_dispatch(&s, GES_POWER_SHORT, 0);
	CHECK(access(off, F_OK) == 0);
	CHECK(strcmp(g_killed, "fblog/15") == 0);
	CHECK(strstr(last_log(), "screen off: created") != NULL);
	CHECK(strstr(last_log(), "signalled fblog (1 process)") != NULL);
	g_killed[0] = '\0';
	server_dispatch(&s, GES_POWER_SHORT, 0);
	CHECK(access(off, F_OK) != 0);
	CHECK(strcmp(g_killed, "fblog/15") == 0);
	CHECK(strstr(last_log(), "screen on: removed") != NULL);
	/* fblog not running: the flag is still set, the log says so */
	g_kill_rc = -ESRCH;
	rc = screen_toggle(&s);
	CHECK(rc == 1 && access(off, F_OK) == 0);
	CHECK(strstr(last_log(), "fblog not running") != NULL);
	rc = screen_toggle(&s);
	CHECK(rc == 0 && access(off, F_OK) != 0);
	g_kill_rc = 1;
	/* flag directory missing: negative errno, nothing signalled */
	s.off_path = "/nonexistent-dir/fblog.off";
	g_killed[0] = '\0';
	rc = screen_toggle(&s);
	CHECK(rc == -ENOENT && g_killed[0] == '\0');
	CHECK(strstr(last_log(), "screen off: create") != NULL);
	s.off_path = off;

	/* other unclaimed gestures only log (power.long is test_server_poweroff) */
	g_killed[0] = '\0';
	server_dispatch(&s, GES_POWER_VOLDOWN, 0);
	CHECK(strstr(last_log(), "power+voldown unclaimed: no default yet") != NULL);
	server_dispatch(&s, GES_VOLUP_SHORT, 0);
	CHECK(strcmp(last_log(), "volup.short unclaimed: no default") == 0);
	CHECK(g_killed[0] == '\0' && access(off, F_OK) != 0);

	/* a claimant gets the event and the default is suppressed */
	a = connect_peer(&s);
	say(&s, &a, "claim power.short\n");
	recv_line(a.fd, buf, sizeof(buf));
	g_killed[0] = '\0';
	server_dispatch(&s, GES_POWER_SHORT, 0);
	recv_line(a.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, "event power.short\n") == 0);
	CHECK(g_killed[0] == '\0' && access(off, F_OK) != 0);
	CHECK(strcmp(last_log(), "power.short -> 1 claimant") == 0);
	/* unclaimed gestures do not reach a plain claimant */
	server_dispatch(&s, GES_VOLUP_SHORT, 0);
	CHECK(recv_line(a.fd, buf, sizeof(buf)) < 0);

	/* a watcher sees everything but suppresses nothing */
	w = connect_peer(&s);
	say(&s, &w, "watch\n");
	recv_line(w.fd, buf, sizeof(buf));
	server_dispatch(&s, GES_POWER_SHORT, 0);
	recv_line(w.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, "event power.short\n") == 0);
	recv_line(a.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, "event power.short\n") == 0);
	CHECK(g_killed[0] == '\0');		/* still claimed by a */
	close(a.fd);
	server_client_input(&s, a.c);		/* a is gone */
	server_dispatch(&s, GES_POWER_SHORT, 0);
	recv_line(w.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, "event power.short\n") == 0);
	CHECK(strcmp(g_killed, "fblog/15") == 0 && access(off, F_OK) == 0);	/* default is back */
	unlink(off);

	/* a peer that vanished without us noticing is dropped at dispatch */
	a = connect_peer(&s);
	say(&s, &a, "claim volup.long\n");
	recv_line(a.fd, buf, sizeof(buf));
	close(a.fd);
	CHECK(a.c->fd >= 0);
	server_dispatch(&s, GES_VOLUP_LONG, 0);
	CHECK(a.c->fd == -1);
	CHECK(strcmp(last_log(), "volup.long unclaimed: no default") == 0);
	recv_line(w.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, "event volup.long\n") == 0);	/* the watcher saw it */

	/* end to end through the recognizer: two events in one drain */
	recog_key(&s.recog, BTN_POWER, 1, 0);
	recog_key(&s.recog, BTN_POWER, 0, 50);
	recog_key(&s.recog, BTN_VOLUP, 1, 100);
	recog_key(&s.recog, BTN_VOLUP, 0, 150);
	server_drain(&s, 0);
	recv_line(w.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, "event power.short\nevent volup.short\n") == 0);
	CHECK(access(off, F_OK) == 0);
	unlink(off);

	close(w.fd);
	server_drop_client(&s, w.c);
	rmdir(dir);
}

/* ---- server: the power-off state machine ---- */

static void test_server_poweroff(void)
{
	struct server s;
	struct peer a;
	char buf[512];
	char dir[] = "/tmp/buttond-test-XXXXXX";
	char vib[128];

	CHECK(mkdtemp(dir) != NULL);
	snprintf(vib, sizeof(vib), "%s/vibrator", dir);
	CHECK(vib_read(vib) == -1);		/* creates the empty node */

	server_init(&s, 1500, 300);
	s.log = test_log;
	s.kill_comm = test_kill;
	s.poweroff = test_poweroff;
	s.vib_path = vib;
	CHECK(s.poweroff_hold_ms == 1500);
	CHECK(s.po_state == PO_IDLE);
	CHECK(server_deadline_ms(&s, 0) == -1);

	/* hold: long at 1500 arms it (short buzz), the deadline is now the READY point */
	recog_key(&s.recog, BTN_POWER, 1, 0);
	CHECK(server_deadline_ms(&s, 0) == 1500);
	recog_tick(&s.recog, 1500);
	server_drain(&s, 1500);
	CHECK(s.po_state == PO_ARMED);
	CHECK(vib_read(vib) == 100);
	CHECK(strstr(last_log(), "keep holding 1500 ms more") != NULL);
	CHECK(server_deadline_ms(&s, 1500) == 1500);
	CHECK(server_deadline_ms(&s, 2900) == 100);
	server_tick(&s, 2900);
	CHECK(s.po_state == PO_ARMED && g_poweroffs == 0 && vib_read(vib) == -1);
	/* release before READY: cancelled, nothing powered off */
	recog_key(&s.recog, BTN_POWER, 0, 2950);
	server_drain(&s, 2950);		/* the release after a long emits nothing */
	server_tick(&s, 2950);
	CHECK(s.po_state == PO_IDLE);
	CHECK(g_poweroffs == 0);
	CHECK(strcmp(last_log(), "power off cancelled: released after 2950 ms") == 0);
	CHECK(vib_read(vib) == -1);
	CHECK(server_deadline_ms(&s, 2950) == -1);

	/* hold through READY (long buzz), then release: power off */
	recog_key(&s.recog, BTN_POWER, 1, 10000);
	recog_tick(&s.recog, 11500);
	server_drain(&s, 11500);
	CHECK(s.po_state == PO_ARMED && vib_read(vib) == 100);
	server_tick(&s, 12999);
	CHECK(s.po_state == PO_ARMED);
	server_tick(&s, 13000);
	CHECK(s.po_state == PO_READY);
	CHECK(vib_read(vib) == 300);
	CHECK(strcmp(last_log(), "power off ready: release the button to power off") == 0);
	CHECK(g_poweroffs == 0);
	CHECK(server_deadline_ms(&s, 13000) == -1);	/* nothing timed any more */
	server_tick(&s, 20000);				/* held for ages: still waiting for the release */
	CHECK(s.po_state == PO_READY && g_poweroffs == 0);
	recog_key(&s.recog, BTN_POWER, 0, 20500);
	server_drain(&s, 20500);
	server_tick(&s, 20500);
	CHECK(g_poweroffs == 1);
	CHECK(s.po_state == PO_IDLE);
	CHECK(vib_read(vib) == 300);
	CHECK(strcmp(last_log(), "power off: released after 10500 ms, shutting down now") == 0);

	/* a release+press between two ticks is a release: cancel, and the new
	 * press is an ordinary one */
	recog_key(&s.recog, BTN_POWER, 1, 30000);
	recog_tick(&s.recog, 31500);
	server_drain(&s, 31500);
	server_tick(&s, 31500);
	CHECK(s.po_state == PO_ARMED);
	recog_key(&s.recog, BTN_POWER, 0, 31600);
	recog_key(&s.recog, BTN_POWER, 1, 31650);
	server_drain(&s, 31650);
	server_tick(&s, 31650);
	CHECK(s.po_state == PO_IDLE && g_poweroffs == 1);
	CHECK(strstr(last_log(), "power off cancelled") != NULL);
	recog_key(&s.recog, BTN_POWER, 0, 31700);
	server_drain(&s, 31700);
	server_tick(&s, 31700);
	CHECK(s.po_state == PO_IDLE && g_poweroffs == 1);

	/* status shows the machine */
	a = connect_peer(&s);
	recog_key(&s.recog, BTN_POWER, 1, 40000);
	recog_tick(&s.recog, 41500);
	server_drain(&s, 41500);
	say(&s, &a, "status\n");
	recv_line(a.fd, buf, sizeof(buf));
	CHECK(strstr(buf, "power=down") != NULL && strstr(buf, "poweroff_hold_ms=1500 poweroff=armed") != NULL);
	recog_key(&s.recog, BTN_POWER, 0, 41600);
	server_drain(&s, 41600);
	server_tick(&s, 41600);
	vib_read(vib);

	/* a claimant takes power.long: no arming at all */
	say(&s, &a, "claim power.long\n");
	recv_line(a.fd, buf, sizeof(buf));
	recog_key(&s.recog, BTN_POWER, 1, 50000);
	recog_tick(&s.recog, 51500);
	server_drain(&s, 51500);
	server_tick(&s, 51500);
	CHECK(s.po_state == PO_IDLE);
	recv_line(a.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, "event power.long\n") == 0);
	CHECK(vib_read(vib) == -1);
	recog_key(&s.recog, BTN_POWER, 0, 55000);
	server_drain(&s, 55000);
	server_tick(&s, 55000);
	CHECK(g_poweroffs == 1);
	close(a.fd);
	server_drop_client(&s, a.c);

	/* -p 0: the default only logs */
	s.poweroff_hold_ms = 0;
	recog_key(&s.recog, BTN_POWER, 1, 60000);
	recog_tick(&s.recog, 61500);
	server_drain(&s, 61500);
	server_tick(&s, 61500);
	CHECK(s.po_state == PO_IDLE);
	CHECK(strcmp(last_log(), "power.long unclaimed: power-off disabled (-p 0)") == 0);
	CHECK(vib_read(vib) == -1);
	recog_key(&s.recog, BTN_POWER, 0, 65000);
	server_drain(&s, 65000);
	server_tick(&s, 65000);
	CHECK(g_poweroffs == 1);

	/* a missing vibrator is not an error */
	s.poweroff_hold_ms = 1500;
	s.vib_path = "/nonexistent-dir/vibrator";
	recog_key(&s.recog, BTN_POWER, 1, 70000);
	recog_tick(&s.recog, 71500);
	server_drain(&s, 71500);
	CHECK(s.po_state == PO_ARMED);
	recog_key(&s.recog, BTN_POWER, 0, 71600);
	server_drain(&s, 71600);
	server_tick(&s, 71600);
	CHECK(s.po_state == PO_IDLE && g_poweroffs == 1);

	unlink(vib);
	rmdir(dir);
}

int main(void)
{
	test_recog_short_unarmed();
	test_recog_long();
	test_recog_double_armed();
	test_recog_chords();
	test_recog_edges();
	test_names();
	test_server_claims();
	test_server_dispatch();
	test_server_poweroff();
	printf("test-buttond: %d/%d checks passed\n", g_tests - g_failures, g_tests);
	return g_failures ? 1 : 0;
}
