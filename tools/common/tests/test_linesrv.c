/* Host tests for tools/common/linesrv.c over a real AF_UNIX listener in a
 * temp dir and over socketpairs: on_connect's hello, framing across
 * partial writes and several lines in one read, CR stripping, empty
 * lines, the overlong discard, the tokenizer and its argc 9 overflow, the
 * client limit reply, must-deliver overflow dropping the client with one
 * on_close, the lossy "dropped" line, a drop during a broadcast loop, a
 * drop from inside on_line with more lines buffered (its own and another
 * client's), POLLOUT flushing a queued tail, stale poll entries, EOF and
 * close_all. No daemon is involved.
 * Build/run: see tools/common/Makefile ("make test").
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "linesrv.h"

static int g_failures;
static int g_tests;

#define CHECK(cond) do { \
	g_tests++; \
	if (!(cond)) { \
		g_failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

#define MAXC 8
#define HELLO "{\"type\":\"hello\",\"daemon\":\"test\",\"proto\":1}\n"
#define ERR_LONG "{\"type\":\"error\",\"reason\":\"line too long\"}\n"
#define ERR_FULL "{\"type\":\"error\",\"reason\":\"too many clients\"}\n"

/* ---- callbacks that record what happened ---- */

static char g_log[8][256];
static int g_nlog;
static int g_connects;
static int g_closes[MAXC];
static int g_nclose;
static char g_lines[64][300];	/* "slot argc: a|b|c" */
static int g_nlines;
static bool g_hello = true;
static bool g_drop_on_connect;
static int g_ctx_seen;

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

static void on_connect(struct linesrv *srv, struct linesrv_client *c)
{
	if (linesrv_ctx(srv) == &g_ctx_seen)
		g_ctx_seen++;
	g_connects++;
	if (g_hello)
		linesrv_send(c, HELLO, sizeof(HELLO) - 1);
	if (g_drop_on_connect)
		linesrv_drop(c);
}

static void broadcast(struct linesrv *srv, const char *line)
{
	int i;

	for (i = 0; i < linesrv_max_clients(srv); i++) {
		struct linesrv_client *c = linesrv_client_at(srv, i);

		if (c)
			linesrv_send(c, line, strlen(line));
	}
}

static void on_line(struct linesrv *srv, struct linesrv_client *c, int argc, char **argv)
{
	char *p = g_lines[g_nlines % 64];
	size_t len = sizeof(g_lines[0]), off;
	int i;

	off = (size_t)snprintf(p, len, "%d %d:", linesrv_slot(c), argc);
	for (i = 0; i < argc && off < len; i++)
		off += (size_t)snprintf(p + off, len - off, "%s%s", i ? "|" : " ", argv[i]);
	g_nlines++;
	if (!strcmp(argv[0], "drop")) {
		linesrv_drop(c);
	} else if (!strcmp(argv[0], "dropslot") && argc == 2) {
		struct linesrv_client *o = linesrv_client_at(srv, atoi(argv[1]));

		if (o)
			linesrv_drop(o);
	} else if (!strcmp(argv[0], "bcast")) {
		broadcast(srv, "{\"type\":\"gesture\"}\n");
	} else if (!strcmp(argv[0], "echo")) {
		linesrv_sendf(c, "{\"type\":\"ok\",\"cmd\":\"echo\",\"argc\":%d}\n", argc);
	}
}

/* on_close may itself send to the others: they must survive it */
static bool g_close_broadcast;

static void on_close(struct linesrv *srv, struct linesrv_client *c)
{
	g_closes[linesrv_slot(c)]++;
	g_nclose++;
	CHECK(linesrv_fd(c) < 0);
	CHECK(linesrv_send(c, "x\n", 2) == -1);
	if (g_close_broadcast)
		broadcast(srv, "{\"type\":\"left\"}\n");
}

static void reset(void)
{
	g_nlog = g_connects = g_nclose = g_nlines = 0;
	memset(g_closes, 0, sizeof(g_closes));
	g_hello = true;
	g_drop_on_connect = false;
	g_close_broadcast = false;
}

static struct linesrv *mk(int max_clients, size_t line_max, size_t outq)
{
	struct linesrv_config cfg = {
		.max_clients = max_clients,
		.line_max = line_max,
		.outq_size = outq,
		.on_connect = on_connect,
		.on_line = on_line,
		.on_close = on_close,
		.ctx = &g_ctx_seen,
		.log = test_log,
	};

	reset();
	return linesrv_new(&cfg);
}

/* ---- helpers ---- */

static void pump(struct linesrv *srv, int timeout_ms)
{
	struct pollfd pfd[1 + MAXC];
	int n = linesrv_pollfds(srv, pfd, 1 + MAXC);

	if (poll(pfd, (nfds_t)n, timeout_ms) > 0)
		linesrv_handle(srv, pfd, n);
}

static void pump_n(struct linesrv *srv, int times)
{
	while (times-- > 0)
		pump(srv, 0);
}

/* The server end adopted, the peer end returned (blocking). */
static int pair(struct linesrv *srv, struct linesrv_client **cp)
{
	int sv[2];
	struct linesrv_client *c;

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0)
		return -1;
	c = linesrv_adopt(srv, sv[0]);
	if (cp)
		*cp = c;
	return sv[1];
}

/* Everything readable on FD within TIMEOUT_MS, NUL-terminated. */
static size_t peer_read(int fd, char *buf, size_t len, int timeout_ms)
{
	size_t off = 0;

	for (;;) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		ssize_t n;

		if (off + 1 >= len || poll(&p, 1, off ? 0 : timeout_ms) <= 0)
			break;
		n = read(fd, buf + off, len - 1 - off);
		if (n <= 0)
			break;
		off += (size_t)n;
	}
	buf[off] = '\0';
	return off;
}

/* Unread data first, then EOF. */
static bool peer_eof(int fd)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	char buf[256];
	ssize_t n;

	do {
		if (poll(&p, 1, 200) != 1)
			return false;
		n = read(fd, buf, sizeof(buf));
	} while (n > 0);
	return n == 0;
}

static void wr(int fd, const char *s)
{
	if (write(fd, s, strlen(s)) != (ssize_t)strlen(s))
		CHECK(!"short write");
}

static int small_sndbuf(struct linesrv_client *c)
{
	int sz = 1;	/* the kernel rounds it up to its minimum */
	socklen_t l = sizeof(sz);

	setsockopt(linesrv_fd(c), SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
	getsockopt(linesrv_fd(c), SOL_SOCKET, SO_SNDBUF, &sz, &l);
	return sz;
}

/* ---- tests ---- */

static void test_listener(void)
{
	char dir[] = "/tmp/linesrv-test-XXXXXX", path[128], buf[512];
	struct linesrv *srv;
	int fd, fd2, i;

	if (!mkdtemp(dir)) {
		CHECK(!"mkdtemp");
		return;
	}
	snprintf(path, sizeof(path), "%s/test.sock", dir);
	srv = mk(2, 128, 4096);
	CHECK(srv != NULL);
	CHECK(linesrv_connect(path) == -ENOENT);

	/* a stale socket file is replaced */
	fd = open(path, O_WRONLY | O_CREAT, 0600);
	close(fd);
	CHECK(linesrv_listen(srv, path) == 0);
	CHECK(linesrv_listen(srv, path) == -EBUSY);
	CHECK(linesrv_max_pollfds(srv) == 3);

	fd = linesrv_connect(path);
	CHECK(fd >= 0);
	pump(srv, 200);
	CHECK(g_connects == 1 && g_ctx_seen >= 1);
	CHECK(peer_read(fd, buf, sizeof(buf), 200) && !strcmp(buf, HELLO));
	wr(fd, "echo a b\n");
	pump(srv, 200);
	CHECK(g_nlines == 1 && !strcmp(g_lines[0], "0 3: echo|a|b"));
	CHECK(peer_read(fd, buf, sizeof(buf), 200) && !strcmp(buf, "{\"type\":\"ok\",\"cmd\":\"echo\",\"argc\":3}\n"));

	/* the client limit over the listener: reply, then closed */
	fd2 = linesrv_connect(path);
	i = linesrv_connect(path);
	CHECK(fd2 >= 0 && i >= 0);
	pump(srv, 200);
	CHECK(g_connects == 2);
	CHECK(peer_read(fd2, buf, sizeof(buf), 200) && !strcmp(buf, HELLO));
	CHECK(peer_read(i, buf, sizeof(buf), 200) && !strcmp(buf, ERR_FULL));
	CHECK(peer_eof(i));
	CHECK(strstr(last_log(), "client limit (2) reached") != NULL);
	close(i);

	/* EOF: on_close once, and the slot takes the next connection */
	close(fd2);
	pump(srv, 200);
	CHECK(g_nclose == 1 && g_closes[1] == 1);
	fd2 = linesrv_connect(path);
	pump(srv, 200);
	CHECK(g_connects == 3 && linesrv_client_at(srv, 1) != NULL);

	/* close_all: on_close for each remaining client, the path unlinked */
	linesrv_close_all(srv);
	CHECK(g_nclose == 3 && g_closes[0] == 1 && g_closes[1] == 2);
	CHECK(access(path, F_OK) != 0);
	CHECK(peer_eof(fd) && peer_eof(fd2));
	close(fd);
	close(fd2);

	/* a path that does not fit sun_path */
	srv = mk(1, 128, 4096);
	memset(buf, 'a', 200);
	buf[0] = '/';
	buf[200] = '\0';
	CHECK(linesrv_listen(srv, buf) == -ENAMETOOLONG);
	CHECK(linesrv_connect(buf) == -ENAMETOOLONG);
	linesrv_close_all(srv);
	rmdir(dir);

	/* bad configs */
	{
		struct linesrv_config cfg = { .max_clients = 0, .line_max = 128, .outq_size = 64, .on_line = on_line };

		CHECK(linesrv_new(&cfg) == NULL);
		cfg.max_clients = 1;
		cfg.on_line = NULL;
		CHECK(linesrv_new(&cfg) == NULL);
	}
}

static void test_framing(void)
{
	struct linesrv *srv = mk(4, 128, 4096);
	char buf[512];
	int p;

	p = pair(srv, NULL);
	CHECK(peer_read(p, buf, sizeof(buf), 200) && !strcmp(buf, HELLO));

	/* one line across partial writes */
	wr(p, "cla");
	pump_n(srv, 2);
	CHECK(g_nlines == 0);
	wr(p, "im power.");
	pump_n(srv, 2);
	CHECK(g_nlines == 0);
	wr(p, "short\n");
	pump_n(srv, 2);
	CHECK(g_nlines == 1 && !strcmp(g_lines[0], "0 2: claim|power.short"));

	/* several lines in one read, empty and blank ones ignored, CR stripped */
	wr(p, "a\nb c\n\n \t \r\nd\r\ne f\r\n");
	pump_n(srv, 2);
	CHECK(g_nlines == 5);
	CHECK(!strcmp(g_lines[1], "0 1: a"));
	CHECK(!strcmp(g_lines[2], "0 2: b|c"));
	CHECK(!strcmp(g_lines[3], "0 1: d"));
	CHECK(!strcmp(g_lines[4], "0 2: e|f"));

	/* only the CR right before the newline goes */
	wr(p, "x\ry\r\r\n");
	pump_n(srv, 2);
	CHECK(g_nlines == 6 && !strcmp(g_lines[5], "0 1: x\ry\r"));

	/* a line that ends in the middle of a read keeps its tail */
	wr(p, "one\ntw");
	pump_n(srv, 2);
	wr(p, "o\n");
	pump_n(srv, 2);
	CHECK(g_nlines == 8 && !strcmp(g_lines[6], "0 1: one") && !strcmp(g_lines[7], "0 1: two"));
	CHECK(peer_read(p, buf, sizeof(buf), 0) == 0);	/* nothing was sent back */

	close(p);
	pump_n(srv, 2);
	CHECK(g_nclose == 1);
	linesrv_close_all(srv);
	CHECK(g_nclose == 1);	/* not twice */
}

static void test_overlong(void)
{
	struct linesrv *srv = mk(2, 128, 4096);
	char line[400], buf[512];
	int p;

	p = pair(srv, NULL);
	peer_read(p, buf, sizeof(buf), 200);

	/* 127 bytes before the newline fit (the newline is the 128th) */
	memset(line, 'w', 127);
	strcpy(line + 127, "\n");
	wr(p, line);
	pump_n(srv, 2);
	CHECK(g_nlines == 1 && strlen(g_lines[0]) == strlen("0 1: ") + 127);
	CHECK(peer_read(p, buf, sizeof(buf), 0) == 0);

	/* 128: error line once, the rest discarded up to the newline */
	memset(line, 'v', 128);
	strcpy(line + 128, "\nnext\n");
	wr(p, line);
	pump_n(srv, 4);
	CHECK(peer_read(p, buf, sizeof(buf), 200) && !strcmp(buf, ERR_LONG));
	CHECK(g_nlines == 2 && !strcmp(g_lines[1], "0 1: next"));

	/* far over, across several writes and reads: one error */
	memset(line, 'u', 300);
	line[300] = '\0';
	wr(p, line);
	pump_n(srv, 4);
	wr(p, line);
	pump_n(srv, 4);
	wr(p, "uuu\nafter\n");
	pump_n(srv, 4);
	CHECK(peer_read(p, buf, sizeof(buf), 200) && !strcmp(buf, ERR_LONG));
	CHECK(g_nlines == 3 && !strcmp(g_lines[2], "0 1: after"));

	/* a 128-byte line that ends exactly at the buffer's end, newline next */
	memset(line, 't', 128);
	line[128] = '\0';
	wr(p, line);
	pump_n(srv, 2);
	wr(p, "\nok\n");
	pump_n(srv, 2);
	CHECK(peer_read(p, buf, sizeof(buf), 200) && !strcmp(buf, ERR_LONG));
	CHECK(g_nlines == 4 && !strcmp(g_lines[3], "0 1: ok"));
	close(p);
	linesrv_close_all(srv);
}

static void test_tokenizer(void)
{
	struct linesrv *srv = mk(2, 128, 4096);
	char buf[512];
	int p;

	p = pair(srv, NULL);
	peer_read(p, buf, sizeof(buf), 200);
	wr(p, "  \tclaim \t  power.short\t\n");
	wr(p, "a b c d e f g h\n");
	wr(p, "a b c d e f g h i\n");
	wr(p, "a b c d e f g h i  j\tk  \n");
	wr(p, "a b c d e f g h \n");
	pump_n(srv, 4);
	CHECK(g_nlines == 5);
	CHECK(!strcmp(g_lines[0], "0 2: claim|power.short"));
	CHECK(!strcmp(g_lines[1], "0 8: a|b|c|d|e|f|g|h"));
	CHECK(!strcmp(g_lines[2], "0 9: a|b|c|d|e|f|g|h|i"));
	CHECK(!strcmp(g_lines[3], "0 9: a|b|c|d|e|f|g|h|i  j\tk"));
	CHECK(!strcmp(g_lines[4], "0 8: a|b|c|d|e|f|g|h"));
	close(p);
	linesrv_close_all(srv);
}

static void test_client_limit_adopt(void)
{
	struct linesrv *srv = mk(2, 128, 4096);
	struct linesrv_client *c = NULL;
	char buf[512];
	int p[3], i;

	for (i = 0; i < 3; i++)
		p[i] = pair(srv, &c);
	CHECK(c == NULL);
	CHECK(g_connects == 2);
	CHECK(peer_read(p[2], buf, sizeof(buf), 200) && !strcmp(buf, ERR_FULL));
	CHECK(peer_eof(p[2]));
	CHECK(strstr(last_log(), "client limit (2) reached, refusing") != NULL);
	CHECK(g_nclose == 0);	/* never connected, never closed */
	for (i = 0; i < 3; i++)
		close(p[i]);
	linesrv_close_all(srv);
	CHECK(g_nclose == 2);

	/* dropped by its own on_connect: NULL, closed once, slot free */
	srv = mk(1, 128, 4096);
	g_drop_on_connect = true;
	p[0] = pair(srv, &c);
	CHECK(c == NULL);
	CHECK(g_connects == 1 && g_nclose == 1);
	CHECK(peer_read(p[0], buf, sizeof(buf), 200) && !strcmp(buf, HELLO));
	CHECK(peer_eof(p[0]));
	CHECK(linesrv_client_at(srv, 0) == NULL);
	g_drop_on_connect = false;
	p[1] = pair(srv, &c);
	CHECK(c != NULL && linesrv_slot(c) == 0);
	close(p[0]);
	close(p[1]);
	linesrv_close_all(srv);
	CHECK(g_nclose == 2);
}

static void test_must_deliver_overflow(void)
{
	struct linesrv *srv = mk(2, 128, 256);
	struct linesrv_client *c, *other;
	char line[64], buf[512];
	int p, q, i, rc = 0;

	p = pair(srv, &c);
	q = pair(srv, &other);
	peer_read(q, buf, sizeof(buf), 200);
	small_sndbuf(c);
	/* the peer never reads: the kernel fills, then the 256-byte queue */
	memset(line, 'r', 63);
	line[63] = '\n';
	for (i = 0; i < 100000 && rc == 0; i++)
		rc = linesrv_send(c, line, sizeof(line));
	CHECK(rc == -1);
	CHECK(i < 100000);
	CHECK(g_nclose == 1 && g_closes[0] == 1);
	CHECK(!strcmp(last_log(), "client output full, disconnecting it"));
	CHECK(linesrv_fd(c) == -1 && linesrv_client_at(srv, 0) == NULL);
	CHECK(linesrv_send(c, line, sizeof(line)) == -1);
	CHECK(linesrv_sendf(c, "%s", "x\n") == -1);
	CHECK(linesrv_send_lossy(c, line, sizeof(line)) == -1);
	linesrv_drop(c);
	CHECK(g_nclose == 1);
	/* the other client is untouched */
	CHECK(linesrv_send(other, "{}\n", 3) == 0);
	CHECK(peer_read(q, buf, sizeof(buf), 200) && !strcmp(buf, "{}\n"));

	/* sendf of a line bigger than the whole queue drops too */
	CHECK(linesrv_sendf(other, "%0300d\n", 1) == -1);
	CHECK(g_nclose == 2 && g_closes[1] == 1);
	close(p);
	close(q);
	linesrv_close_all(srv);
	CHECK(g_nclose == 2);
}

static void test_lossy(void)
{
	struct linesrv *srv = mk(2, 128, 256);
	struct linesrv_client *c;
	char line[64], buf[1 << 16], *tail;
	int p, i, rc = 0, dropped;
	size_t got = 0;

	g_hello = false;
	p = pair(srv, &c);
	small_sndbuf(c);
	memset(line, 's', 63);
	line[63] = '\n';
	for (i = 0; i < 100000 && rc == 0; i++)
		rc = linesrv_send_lossy(c, line, sizeof(line));
	CHECK(rc == 1);
	for (dropped = 1; dropped < 5; dropped++)
		CHECK(linesrv_send_lossy(c, line, sizeof(line)) == 1);
	CHECK(g_nclose == 0 && linesrv_fd(c) >= 0);

	/* the peer catches up; POLLOUT flushes the queue */
	for (i = 0; i < 50; i++) {
		size_t n = peer_read(p, buf + got, sizeof(buf) - got, 20);

		got += n;
		pump(srv, 0);
	}
	CHECK(got > 0 && !strstr(buf, "dropped"));
	/* a must-deliver reply does not carry the note; the next lossy line does */
	CHECK(linesrv_send(c, "{\"r\":1}\n", 8) == 0);
	CHECK(linesrv_send_lossy(c, "{\"next\":1}\n", 11) == 0);
	got += peer_read(p, buf + got, sizeof(buf) - got, 200);
	CHECK(got > 0 && buf[got - 1] == '\n');
	tail = buf + got - 11;
	CHECK(!strcmp(tail, "{\"next\":1}\n"));
	tail -= strlen("{\"type\":\"dropped\",\"count\":5}\n");
	CHECK(!strncmp(tail, "{\"type\":\"dropped\",\"count\":5}\n", 29));
	tail -= 8;
	CHECK(!strncmp(tail, "{\"r\":1}\n", 8));
	CHECK(strstr(buf, "dropped") == tail + 8 + 9);	/* reported once */
	CHECK(strstr(tail + 8 + 9 + 7, "dropped") == NULL);

	/* the count was reset */
	CHECK(linesrv_send_lossy(c, "{\"n\":2}\n", 8) == 0);
	CHECK(peer_read(p, buf, sizeof(buf), 200) && !strcmp(buf, "{\"n\":2}\n"));
	close(p);
	linesrv_close_all(srv);
}

static void test_pollout_tail(void)
{
	struct linesrv *srv = mk(2, 128, 1 << 16);
	struct linesrv_client *c;
	struct pollfd pfd[1 + MAXC];
	static char big[40000], buf[1 << 16];
	size_t got = 0;
	int p, n, sz, i;

	g_hello = false;
	p = pair(srv, &c);
	sz = small_sndbuf(c);
	CHECK((size_t)sz < sizeof(big));
	for (i = 0; i < (int)sizeof(big) - 1; i++)
		big[i] = (char)('a' + i % 26);
	big[sizeof(big) - 1] = '\n';
	CHECK(linesrv_send(c, big, sizeof(big)) == 0);
	n = linesrv_pollfds(srv, pfd, 1 + MAXC);
	CHECK(n == 1 && pfd[0].events == (POLLIN | POLLOUT));
	for (i = 0; i < 2000 && got < sizeof(big); i++) {
		got += peer_read(p, buf + got, sizeof(buf) - got, 10);
		pump(srv, 10);
	}
	CHECK(got == sizeof(big) && !memcmp(buf, big, sizeof(big)));
	n = linesrv_pollfds(srv, pfd, 1 + MAXC);
	CHECK(n == 1 && pfd[0].events == POLLIN);
	CHECK(g_nclose == 0);
	close(p);
	linesrv_close_all(srv);
}

static void test_drop_in_broadcast(void)
{
	struct linesrv *srv = mk(4, 128, 4096);
	char buf[512];
	int p[3], i;

	g_hello = false;
	for (i = 0; i < 3; i++)
		p[i] = pair(srv, NULL);
	/* the middle peer is gone: its send fails while we loop */
	close(p[1]);
	g_close_broadcast = true;
	broadcast(srv, "{\"type\":\"gesture\",\"gesture\":\"power.short\"}\n");
	CHECK(g_nclose == 1 && g_closes[1] == 1);
	CHECK(peer_read(p[0], buf, sizeof(buf), 200));
	CHECK(!strcmp(buf, "{\"type\":\"gesture\",\"gesture\":\"power.short\"}\n{\"type\":\"left\"}\n"));
	CHECK(peer_read(p[2], buf, sizeof(buf), 200));
	CHECK(!strcmp(buf, "{\"type\":\"left\"}\n{\"type\":\"gesture\",\"gesture\":\"power.short\"}\n"));

	/* a broadcast from inside on_line, sender included */
	g_close_broadcast = false;
	wr(p[2], "bcast\n");
	pump_n(srv, 2);
	CHECK(peer_read(p[0], buf, sizeof(buf), 200) && !strcmp(buf, "{\"type\":\"gesture\"}\n"));
	CHECK(peer_read(p[2], buf, sizeof(buf), 200) && !strcmp(buf, "{\"type\":\"gesture\"}\n"));
	close(p[0]);
	close(p[2]);
	linesrv_close_all(srv);
	CHECK(g_nclose == 3);
}

static void test_drop_in_on_line(void)
{
	struct linesrv *srv = mk(4, 128, 4096);
	char buf[512];
	int a, b;

	g_hello = false;
	/* its own drop: the lines after it are not handled */
	a = pair(srv, NULL);
	wr(a, "one\ndrop\nafter\nmore\n");
	pump_n(srv, 3);
	CHECK(g_nlines == 2 && !strcmp(g_lines[1], "0 1: drop"));
	CHECK(g_nclose == 1 && g_closes[0] == 1);
	CHECK(peer_eof(a));
	close(a);

	/* another client dropped while both have input pending in one poll */
	a = pair(srv, NULL);	/* slot 0 again */
	b = pair(srv, NULL);	/* slot 1 */
	wr(b, "never\n");
	wr(a, "dropslot 1\n");
	pump(srv, 200);
	CHECK(g_nlines == 3 && !strcmp(g_lines[2], "0 2: dropslot|1"));
	CHECK(g_nclose == 2 && g_closes[1] == 1);
	pump_n(srv, 2);
	CHECK(g_nlines == 3);
	CHECK(peer_read(b, buf, sizeof(buf), 0) == 0 && peer_eof(b));
	close(b);
	close(a);
	pump_n(srv, 2);
	CHECK(g_nclose == 3);
	linesrv_close_all(srv);
	CHECK(g_nclose == 3);
}

static void test_stale_entries(void)
{
	struct linesrv *srv = mk(2, 128, 4096);
	struct linesrv_client *c;
	struct pollfd pfd[1 + MAXC];
	int p, q, n;

	g_hello = false;
	p = pair(srv, &c);
	n = linesrv_pollfds(srv, pfd, 1 + MAXC);
	CHECK(n == 1);
	/* the slot goes and comes back with another fd before handle() */
	linesrv_drop(c);
	q = pair(srv, &c);
	CHECK(c && linesrv_slot(c) == 0);
	wr(q, "fresh\n");
	pfd[0].revents = POLLIN;
	linesrv_handle(srv, pfd, n);
	CHECK(g_nlines == 0);
	pump(srv, 200);
	CHECK(g_nlines == 1 && !strcmp(g_lines[0], "0 1: fresh"));
	/* more entries than were filled are ignored */
	linesrv_pollfds(srv, pfd, 1 + MAXC);
	pfd[1] = (struct pollfd){ .fd = 0, .events = POLLIN, .revents = POLLIN };
	linesrv_handle(srv, pfd, 2);
	close(p);
	close(q);
	linesrv_close_all(srv);
	CHECK(g_nclose == 2);
}

static void test_eof_and_hup(void)
{
	struct linesrv *srv = mk(2, 128, 4096);
	char buf[64];
	int p;

	g_hello = false;
	/* a request and EOF in one go: the request is still handled */
	p = pair(srv, NULL);
	wr(p, "status\n");
	shutdown(p, SHUT_WR);
	pump_n(srv, 3);
	CHECK(g_nlines == 1 && !strcmp(g_lines[0], "0 1: status"));
	CHECK(g_nclose == 1);
	CHECK(peer_read(p, buf, sizeof(buf), 0) == 0);
	close(p);
	/* a partial line at EOF is dropped with the client */
	p = pair(srv, NULL);
	wr(p, "partial");
	close(p);
	pump_n(srv, 3);
	CHECK(g_nlines == 1 && g_nclose == 2);
	linesrv_close_all(srv);
	CHECK(g_nclose == 2);
}

int main(void)
{
	test_listener();
	test_framing();
	test_overlong();
	test_tokenizer();
	test_client_limit_adopt();
	test_must_deliver_overflow();
	test_lossy();
	test_pollout_tail();
	test_drop_in_broadcast();
	test_drop_in_on_line();
	test_stale_entries();
	test_eof_and_hup();
	printf("test-linesrv: %d/%d checks passed\n", g_tests - g_failures, g_tests);
	return g_failures ? 1 : 0;
}
