/* Host tests for tools/buttond/buttond.c: the gesture recognizer (short on
 * release when doubles are not armed, double/single with the tap window
 * when they are, long at the hold threshold from the tick and from a late
 * release, chords in both press orders, ignored releases/repeats,
 * deadlines), the gesture name table, the claim registry and its
 * double-arming, the line protocol over real socketpairs (partial lines,
 * bad input, dead peers), dispatch to claimants/watchers vs. the built-in
 * default, the screen toggle against a temp flag file (it only creates
 * and removes the flag), and the power-off state machine (arm at long, cancel on early
 * release or re-press, READY buzz, act on release, claimed/-p 0 variants)
 * with an injected poweroff and a temp vibrator file. buttond.c is
 * included with BUTTOND_NO_MAIN; no evdev device, socket path or process
 * is touched and nothing powers off. Build/run: see tools/buttond/Makefile
 * ("make test").
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

#define HELLO "{\"type\":\"hello\",\"daemon\":\"buttond\",\"proto\":1}\n"

struct peer {
	struct linesrv_client *c;
	int fd;		/* our end */
};

/* One poll round of the server's sockets. */
static void pump(struct server *s)
{
	struct pollfd pfd[1 + BUTTOND_MAX_CLIENTS];
	int n = linesrv_pollfds(s->srv, pfd, 1 + BUTTOND_MAX_CLIENTS);

	if (poll(pfd, (nfds_t)n, 0) > 0)
		linesrv_handle(s->srv, pfd, n);
}

/* A connected client; the hello is the first thing it reads. */
static struct peer connect_peer(struct server *s)
{
	struct peer p;
	char buf[128];
	int sv[2];

	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	p.fd = sv[0];
	p.c = linesrv_adopt(s->srv, sv[1]);
	CHECK(p.c != NULL);
	recv_line(p.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, HELLO) == 0);
	return p;
}

static void say(struct server *s, struct peer *p, const char *text)
{
	CHECK(send(p->fd, text, strlen(text), 0) == (ssize_t)strlen(text));
	pump(s);
}

/* Send TEXT and expect exactly REPLY back. */
static void ask(struct server *s, struct peer *p, const char *text, const char *reply)
{
	char buf[1024];

	say(s, p, text);
	recv_line(p->fd, buf, sizeof(buf));
	CHECK(strcmp(buf, reply) == 0);
	if (strcmp(buf, reply))
		fprintf(stderr, "  %s-> %s", text, buf);
}

static void test_server_claims(void)
{
	struct server s;
	struct peer a, b;
	char buf[512];

	CHECK(server_init(&s, 1500, 300, test_log) == 0);
	CHECK(s.off_path && strcmp(s.off_path, "/run/chef/screen/off") == 0);

	a = connect_peer(&s);
	ask(&s, &a, "claim power.long\n", "{\"type\":\"ok\",\"cmd\":\"claim\",\"gesture\":\"power.long\"}\n");
	CHECK(server_claim_count(&s, GES_POWER_LONG) == 1);
	CHECK(!s.recog.double_armed[BTN_POWER]);

	ask(&s, &a, "claim power.double\n", "{\"type\":\"ok\",\"cmd\":\"claim\",\"gesture\":\"power.double\"}\n");
	CHECK(s.recog.double_armed[BTN_POWER]);
	CHECK(!s.recog.double_armed[BTN_VOLUP]);

	ask(&s, &a, "claim nope\n",
	    "{\"type\":\"error\",\"cmd\":\"claim\",\"reason\":\"unknown gesture\",\"gesture\":\"nope\"}\n");
	ask(&s, &a, "release nope\n",
	    "{\"type\":\"error\",\"cmd\":\"release\",\"reason\":\"unknown gesture\",\"gesture\":\"nope\"}\n");
	ask(&s, &a, "claim\n", "{\"type\":\"error\",\"cmd\":\"claim\",\"reason\":\"bad arguments\"}\n");
	ask(&s, &a, "claim power.short volup.short\n",
	    "{\"type\":\"error\",\"cmd\":\"claim\",\"reason\":\"bad arguments\"}\n");
	CHECK(server_claim_count(&s, GES_POWER_SHORT) == 0);	/* extra words: nothing claimed */
	ask(&s, &a, "release\n", "{\"type\":\"error\",\"cmd\":\"release\",\"reason\":\"bad arguments\"}\n");
	ask(&s, &a, "watch now\n", "{\"type\":\"error\",\"cmd\":\"watch\",\"reason\":\"bad arguments\"}\n");
	ask(&s, &a, "status all\n", "{\"type\":\"error\",\"cmd\":\"status\",\"reason\":\"bad arguments\"}\n");
	ask(&s, &a, "a b c d e f g h i j\n", "{\"type\":\"error\",\"cmd\":\"a\",\"reason\":\"unknown command\"}\n");
	ask(&s, &a, "claim 1 2 3 4 5 6 7 8 9\n", "{\"type\":\"error\",\"cmd\":\"claim\",\"reason\":\"bad arguments\"}\n");
	ask(&s, &a, "frobnicate x\n", "{\"type\":\"error\",\"cmd\":\"frobnicate\",\"reason\":\"unknown command\"}\n");
	/* echoed words are escaped and cut at 32 bytes */
	ask(&s, &a, "fr\"ob\\x\x01\n",
	    "{\"type\":\"error\",\"cmd\":\"fr\\\"ob\\\\x\\u0001\",\"reason\":\"unknown command\"}\n");
	ask(&s, &a, "claim 0123456789abcdef0123456789abcdefXYZ\n",
	    "{\"type\":\"error\",\"cmd\":\"claim\",\"reason\":\"unknown gesture\","
	    "\"gesture\":\"0123456789abcdef0123456789abcdef\"}\n");
	ask(&s, &a, "\xc3\xa9t\x7f\n", "{\"type\":\"error\",\"cmd\":\"??t?\",\"reason\":\"unknown command\"}\n");
	say(&s, &a, "\n");			/* empty line: ignored */
	CHECK(recv_line(a.fd, buf, sizeof(buf)) < 0);
	say(&s, &a, " \t \r\n");		/* so is a blank one */
	CHECK(recv_line(a.fd, buf, sizeof(buf)) < 0);

	/* status */
	ask(&s, &a, "status\n",
	    "{\"type\":\"status\",\"buttons\":{\"power\":\"up\",\"volup\":\"up\",\"voldown\":\"up\"},"
	    "\"long_ms\":1500,\"double_ms\":300,\"poweroff_hold_ms\":1500,\"poweroff\":\"idle\","
	    "\"claims\":{\"power.double\":1,\"power.long\":1},\"watchers\":0}\n");

	/* a second claimant, then release/disconnect bookkeeping */
	b = connect_peer(&s);
	ask(&s, &b, "claim power.double\nwatch\n",	/* two lines in one recv */
	    "{\"type\":\"ok\",\"cmd\":\"claim\",\"gesture\":\"power.double\"}\n{\"type\":\"ok\",\"cmd\":\"watch\"}\n");
	CHECK(server_claim_count(&s, GES_POWER_DOUBLE) == 2);
	ask(&s, &a, "release power.double\n", "{\"type\":\"ok\",\"cmd\":\"release\",\"gesture\":\"power.double\"}\n");
	CHECK(server_claim_count(&s, GES_POWER_DOUBLE) == 1);
	CHECK(s.recog.double_armed[BTN_POWER]);	/* b still holds it */
	/* releasing twice is fine */
	ask(&s, &a, "release power.double\n", "{\"type\":\"ok\",\"cmd\":\"release\",\"gesture\":\"power.double\"}\n");

	s.recog.b[BTN_POWER].down = true;
	ask(&s, &b, "status\n",
	    "{\"type\":\"status\",\"buttons\":{\"power\":\"down\",\"volup\":\"up\",\"voldown\":\"up\"},"
	    "\"long_ms\":1500,\"double_ms\":300,\"poweroff_hold_ms\":1500,\"poweroff\":\"idle\","
	    "\"claims\":{\"power.double\":1,\"power.long\":1},\"watchers\":1}\n");
	s.recog.b[BTN_POWER].down = false;

	/* peer b goes away: its claims vanish and doubles disarm */
	close(b.fd);
	pump(&s);
	CHECK(linesrv_fd(b.c) == -1);
	CHECK(server_claim_count(&s, GES_POWER_DOUBLE) == 0);
	CHECK(!s.recog.double_armed[BTN_POWER]);
	CHECK(server_claim_count(&s, GES_POWER_LONG) == 1);

	/* partial lines are assembled across reads */
	say(&s, &a, "cla");
	CHECK(recv_line(a.fd, buf, sizeof(buf)) < 0);
	say(&s, &a, "im volup.sho");
	CHECK(recv_line(a.fd, buf, sizeof(buf)) < 0);
	ask(&s, &a, "rt\n", "{\"type\":\"ok\",\"cmd\":\"claim\",\"gesture\":\"volup.short\"}\n");
	/* CRLF and extra whitespace tolerated */
	ask(&s, &a, "claim  voldown.long \r\n", "{\"type\":\"ok\",\"cmd\":\"claim\",\"gesture\":\"voldown.long\"}\n");

	/* an overlong line is rejected once and its tail discarded up to the
	 * newline, so the next command is not corrupted by the leftovers */
	memset(buf, 'x', 300);
	buf[300] = '\0';
	ask(&s, &a, buf, "{\"type\":\"error\",\"reason\":\"line too long\"}\n");
	pump(&s);				/* the rest of the x's */
	pump(&s);
	CHECK(recv_line(a.fd, buf, sizeof(buf)) < 0);
	say(&s, &a, "yyy\nstatus\n");		/* end of the bad line, then a good one */
	recv_line(a.fd, buf, sizeof(buf));
	CHECK(strncmp(buf, "{\"type\":\"status\",", 17) == 0);
	CHECK(strchr(buf, '\n') == buf + strlen(buf) - 1);	/* exactly one reply */
	/* 127 bytes before the newline still fit */
	memset(buf, ' ', 127);
	memcpy(buf + 127 - 6, "status", 6);
	strcpy(buf + 127, "\n");
	say(&s, &a, buf);
	recv_line(a.fd, buf, sizeof(buf));
	CHECK(strncmp(buf, "{\"type\":\"status\",", 17) == 0);

	close(a.fd);
	linesrv_drop(a.c);
	CHECK(server_claim_count(&s, GES_POWER_LONG) == 0);

	/* the client table is bounded: the 17th gets an error and is closed */
	{
		struct peer ps[BUTTOND_MAX_CLIENTS];
		int sv[2], i;

		for (i = 0; i < BUTTOND_MAX_CLIENTS; i++)
			ps[i] = connect_peer(&s);
		CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
		CHECK(linesrv_adopt(s.srv, sv[1]) == NULL);
		recv_line(sv[0], buf, sizeof(buf));
		CHECK(strcmp(buf, "{\"type\":\"error\",\"reason\":\"too many clients\"}\n") == 0);
		CHECK(recv(sv[0], buf, 1, 0) == 0);	/* then closed */
		CHECK(strcmp(last_log(), "client limit (16) reached, refusing") == 0);
		close(sv[0]);
		for (i = 0; i < BUTTOND_MAX_CLIENTS; i++) {
			close(ps[i].fd);
			linesrv_drop(ps[i].c);
		}
	}
	server_fini(&s);
}

/* A claimant that stops reading is dropped once its queue is full, and
 * the default for its gesture comes back. */
static void test_server_slow_claimant(void)
{
	struct server s;
	struct peer a;
	char dir[] = "/tmp/buttond-test-XXXXXX";
	char off[128], buf[128];
	int i, early, sz = 1;

	CHECK(mkdtemp(dir) != NULL);
	snprintf(off, sizeof(off), "%s/screen-off", dir);
	CHECK(server_init(&s, 1500, 300, test_log) == 0);
	s.off_path = off;

	a = connect_peer(&s);
	ask(&s, &a, "claim power.short\n", "{\"type\":\"ok\",\"cmd\":\"claim\",\"gesture\":\"power.short\"}\n");
	setsockopt(linesrv_fd(a.c), SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
	for (i = 0, early = 0; i < 100000 && linesrv_fd(a.c) >= 0; i++) {
		if (access(off, F_OK) == 0)
			early++;		/* claimed: no toggle yet */
		server_dispatch(&s, GES_POWER_SHORT, 0);
	}
	CHECK(early == 0);
	CHECK(linesrv_fd(a.c) == -1);
	CHECK(i > BUTTOND_OUTQ / 45);		/* the queue took its share first */
	CHECK(server_claim_count(&s, GES_POWER_SHORT) == 0);
	/* the dropping dispatch itself already fell back to the default */
	CHECK(access(off, F_OK) == 0);
	CHECK(strstr(last_log(), "screen off") != NULL);
	server_dispatch(&s, GES_POWER_SHORT, 0);
	CHECK(access(off, F_OK) != 0 && strstr(last_log(), "screen on") != NULL);
	/* what it did get was whole lines */
	{
		const char *ev = "{\"type\":\"gesture\",\"gesture\":\"power.short\"}\n";

		recv_line(a.fd, buf, strlen(ev) + 1);
		CHECK(strcmp(buf, ev) == 0);
	}
	close(a.fd);
	server_fini(&s);
	rmdir(dir);
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
	snprintf(off, sizeof(off), "%s/screen-off", dir);

	CHECK(server_init(&s, 1500, 300, test_log) == 0);
	s.off_path = off;

	/* nobody claims power.short: default toggles the flag, nothing else */
	server_dispatch(&s, GES_POWER_SHORT, 0);
	CHECK(access(off, F_OK) == 0);
	CHECK(strncmp(last_log(), "screen off: created ", 20) == 0 && strstr(last_log(), off) != NULL);
	server_dispatch(&s, GES_POWER_SHORT, 0);
	CHECK(access(off, F_OK) != 0);
	CHECK(strstr(last_log(), "screen on: removed") != NULL);
	rc = screen_toggle(&s);
	CHECK(rc == 1 && access(off, F_OK) == 0);
	rc = screen_toggle(&s);
	CHECK(rc == 0 && access(off, F_OK) != 0);
	/* flag directory missing: negative errno, logged */
	s.off_path = "/nonexistent-dir/screen-off";
	rc = screen_toggle(&s);
	CHECK(rc == -ENOENT);
	CHECK(strstr(last_log(), "screen off: create") != NULL);
	s.off_path = off;
	/* never through a symlink: the flag's directory is group-writable */
	{
		char target[160];

		snprintf(target, sizeof(target), "%s/target", dir);
		CHECK(symlink(target, off) == 0);
		CHECK(screen_toggle(&s) == -ELOOP);
		CHECK(access(target, F_OK) != 0);
		CHECK(unlink(off) == 0);
	}

	/* other unclaimed gestures only log (power.long is test_server_poweroff) */
	server_dispatch(&s, GES_POWER_VOLDOWN, 0);
	CHECK(strstr(last_log(), "power+voldown unclaimed: no default yet") != NULL);
	server_dispatch(&s, GES_VOLUP_SHORT, 0);
	CHECK(strcmp(last_log(), "volup.short unclaimed: no default") == 0);
	CHECK(access(off, F_OK) != 0);

	/* a claimant gets the event and the default is suppressed */
	a = connect_peer(&s);
	say(&s, &a, "claim power.short\n");
	recv_line(a.fd, buf, sizeof(buf));
	server_dispatch(&s, GES_POWER_SHORT, 0);
	recv_line(a.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, "{\"type\":\"gesture\",\"gesture\":\"power.short\"}\n") == 0);
	CHECK(access(off, F_OK) != 0);
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
	CHECK(strcmp(buf, "{\"type\":\"gesture\",\"gesture\":\"power.short\"}\n") == 0);
	recv_line(a.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, "{\"type\":\"gesture\",\"gesture\":\"power.short\"}\n") == 0);
	CHECK(access(off, F_OK) != 0);		/* still claimed by a */
	close(a.fd);
	pump(&s);				/* a is gone */
	server_dispatch(&s, GES_POWER_SHORT, 0);
	recv_line(w.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, "{\"type\":\"gesture\",\"gesture\":\"power.short\"}\n") == 0);
	CHECK(access(off, F_OK) == 0);	/* default is back */
	unlink(off);

	/* a peer that vanished without us noticing is dropped at dispatch */
	a = connect_peer(&s);
	say(&s, &a, "claim volup.long\n");
	recv_line(a.fd, buf, sizeof(buf));
	close(a.fd);
	CHECK(linesrv_fd(a.c) >= 0);
	server_dispatch(&s, GES_VOLUP_LONG, 0);
	CHECK(linesrv_fd(a.c) == -1);
	CHECK(strcmp(last_log(), "volup.long unclaimed: no default") == 0);
	recv_line(w.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, "{\"type\":\"gesture\",\"gesture\":\"volup.long\"}\n") == 0);	/* the watcher saw it */

	/* end to end through the recognizer: two events in one drain */
	recog_key(&s.recog, BTN_POWER, 1, 0);
	recog_key(&s.recog, BTN_POWER, 0, 50);
	recog_key(&s.recog, BTN_VOLUP, 1, 100);
	recog_key(&s.recog, BTN_VOLUP, 0, 150);
	server_drain(&s, 0);
	recv_line(w.fd, buf, sizeof(buf));
	CHECK(strcmp(buf, "{\"type\":\"gesture\",\"gesture\":\"power.short\"}\n"
			  "{\"type\":\"gesture\",\"gesture\":\"volup.short\"}\n") == 0);
	CHECK(access(off, F_OK) == 0);
	unlink(off);

	close(w.fd);
	linesrv_drop(w.c);
	server_fini(&s);
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

	CHECK(server_init(&s, 1500, 300, test_log) == 0);
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
	CHECK(strstr(buf, "\"power\":\"down\"") != NULL &&
	      strstr(buf, "\"poweroff_hold_ms\":1500,\"poweroff\":\"armed\"") != NULL);
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
	CHECK(strcmp(buf, "{\"type\":\"gesture\",\"gesture\":\"power.long\"}\n") == 0);
	CHECK(vib_read(vib) == -1);
	recog_key(&s.recog, BTN_POWER, 0, 55000);
	server_drain(&s, 55000);
	server_tick(&s, 55000);
	CHECK(g_poweroffs == 1);
	close(a.fd);
	linesrv_drop(a.c);

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
	server_fini(&s);
	rmdir(dir);
}

int main(void)
{
	chef_log_set_socket("/nonexistent/chef-log-socket");	/* never the host's /dev/log */
	test_recog_short_unarmed();
	test_recog_long();
	test_recog_double_armed();
	test_recog_chords();
	test_recog_edges();
	test_names();
	test_server_claims();
	test_server_slow_claimant();
	test_server_dispatch();
	test_server_poweroff();
	printf("test-buttond: %d/%d checks passed\n", g_tests - g_failures, g_tests);
	return g_failures ? 1 : 0;
}
