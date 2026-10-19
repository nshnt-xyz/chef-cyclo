/* Host tests for tools/common/lineclient.c against real AF_UNIX and TCP
 * loopback listeners, with an injected clock: a refused connect retried
 * with the doubling backoff and its deadlines, start while waiting, a
 * connect completing later on POLLOUT, a TCP port that refuses, framing
 * across reads and several lines in one read, CR stripping, the overlong
 * discard and the longest line that fits, the read bound per wakeup, EOF
 * and a reset each calling on_lost once, a callback closing or stopping
 * its connection with more lines buffered, sends, healthy and stop
 * resetting the backoff. No daemon is involved.
 * Build/run: see tools/common/Makefile ("make test").
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "lineclient.h"

static int g_failures;
static int g_tests;

#define CHECK(cond) do { \
	g_tests++; \
	if (!(cond)) { \
		g_failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

/* ---- callbacks that record what happened ---- */

static int g_connected, g_lost;
static int64_t g_lost_at;
static char g_lines[16][128];
static size_t g_lens[16];
static int g_nlines;
static size_t g_bytes;		/* line bytes delivered */
static enum { ACT_NONE, ACT_CLOSE, ACT_STOP, ACT_SEND } g_act;

static void on_connected(struct lineclient *lc, int64_t now)
{
	(void)lc;
	(void)now;
	g_connected++;
}

static void on_line(struct lineclient *lc, char *line, size_t len, int64_t now)
{
	CHECK(line[len] == '\0' && strlen(line) == len);
	if (g_nlines < 16) {
		snprintf(g_lines[g_nlines], sizeof(g_lines[0]), "%s", line);
		g_lens[g_nlines] = len;
	}
	g_nlines++;
	g_bytes += len;
	if (strcmp(line, "close") == 0 && g_act == ACT_CLOSE)
		lineclient_close(lc, now);
	if (strcmp(line, "close") == 0 && g_act == ACT_STOP)
		lineclient_stop(lc, now);
	if (strcmp(line, "close") == 0 && g_act == ACT_SEND)
		CHECK(lineclient_send(lc, "pong\n", now) == 0);
}

static void on_lost(struct lineclient *lc, int64_t now)
{
	CHECK(!lineclient_up(lc));
	g_lost++;
	g_lost_at = now;
}

static void reset(void)
{
	g_connected = g_lost = g_nlines = 0;
	g_bytes = 0;
	g_lost_at = 0;
	g_act = ACT_NONE;
}

/* ---- helpers ---- */

static char g_dir[] = "/tmp/lineclient-test.XXXXXX";

static int listen_unix(const char *path)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
	unlink(path);
	CHECK(bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
	CHECK(listen(fd, 4) == 0);
	return fd;
}

static int listen_tcp(int *port)
{
	struct sockaddr_in sa = { .sin_family = AF_INET };
	socklen_t l = sizeof(sa);
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	CHECK(bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
	CHECK(listen(fd, 4) == 0);
	getsockname(fd, (struct sockaddr *)&sa, &l);
	*port = ntohs(sa.sin_port);
	return fd;
}

/* One poll round of the client (up to MS of real time), at clock NOW. */
static void pump(struct lineclient *lc, int64_t now, int ms)
{
	struct pollfd p = lineclient_pollfd(lc);

	if (p.fd < 0)
		return;
	if (poll(&p, 1, ms) > 0)
		lineclient_handle(lc, p.revents, now);
}

/* Connect to the listener LFD: the accepted peer fd. */
static int connect_up(struct lineclient *lc, int lfd, int64_t now)
{
	int cfd;

	lineclient_start(lc, now);
	CHECK(lineclient_pollfd(lc).fd >= 0 && lineclient_pollfd(lc).events == POLLOUT);
	pump(lc, now, 1000);
	CHECK(lineclient_up(lc) && lineclient_pollfd(lc).events == POLLIN);
	cfd = accept(lfd, NULL, NULL);
	CHECK(cfd >= 0);
	return cfd;
}

static void say(int fd, const char *s)
{
	CHECK(send(fd, s, strlen(s), MSG_NOSIGNAL) == (ssize_t)strlen(s));
}

static struct lineclient_config unix_cfg(const char *path)
{
	return (struct lineclient_config){
		.path = path, .backoff_min_ms = 100, .backoff_max_ms = 400,
		.on_connected = on_connected, .on_line = on_line, .on_lost = on_lost,
		.ctx = g_dir,
	};
}

/* ---- tests ---- */

static void test_refused_and_backoff(void)
{
	struct lineclient lc;
	struct lineclient_config cfg;
	char path[256];
	int lfd, cfd;

	reset();
	snprintf(path, sizeof(path), "%s/a.sock", g_dir);
	cfg = unix_cfg(path);
	lineclient_init(&lc, &cfg);
	CHECK(lineclient_ctx(&lc) == g_dir);
	CHECK(lineclient_deadline(&lc) == -1 && lineclient_pollfd(&lc).fd == -1);
	lineclient_tick(&lc, 5000);		/* idle: nothing */
	CHECK(lineclient_deadline(&lc) == -1);

	/* nothing listens: no callbacks, retries after 100, 200, 400, 400 */
	lineclient_start(&lc, 1000);
	CHECK(lineclient_pollfd(&lc).fd == -1 && lineclient_deadline(&lc) == 1100);
	lineclient_tick(&lc, 1099);
	CHECK(lineclient_deadline(&lc) == 1100);
	lineclient_start(&lc, 1050);		/* already waiting: the retry time stands */
	CHECK(lineclient_deadline(&lc) == 1100);
	lineclient_tick(&lc, 1100);
	CHECK(lineclient_deadline(&lc) == 1300);
	lineclient_tick(&lc, 1300);
	CHECK(lineclient_deadline(&lc) == 1700);
	lineclient_tick(&lc, 1700);
	CHECK(lineclient_deadline(&lc) == 2100);
	CHECK(g_connected == 0 && g_lost == 0);

	/* it appears: the due retry connects, finished on POLLOUT */
	lfd = listen_unix(path);
	lineclient_tick(&lc, 2100);
	CHECK(lineclient_pollfd(&lc).fd >= 0 && lineclient_pollfd(&lc).events == POLLOUT);
	CHECK(lineclient_deadline(&lc) == -1 && !lineclient_up(&lc));
	pump(&lc, 2101, 1000);
	CHECK(g_connected == 1 && lineclient_up(&lc) && lineclient_pollfd(&lc).events == POLLIN);
	CHECK(lineclient_send(&lc, "watch\n", 2101) == 0);
	cfd = accept(lfd, NULL, NULL);
	CHECK(cfd >= 0);
	{
		char b[16] = "";

		CHECK(recv(cfd, b, sizeof(b) - 1, 0) == 6 && strcmp(b, "watch\n") == 0);
	}

	/* lost before the client called it healthy: the backoff kept growing */
	close(cfd);
	pump(&lc, 3000, 1000);
	CHECK(g_lost == 1 && g_lost_at == 3000 && lineclient_pollfd(&lc).fd == -1);
	CHECK(lineclient_deadline(&lc) == 3400);
	CHECK(lineclient_send(&lc, "x\n", 3000) == -1);	/* not up */

	/* healthy: back to 100 */
	lineclient_tick(&lc, 3400);
	pump(&lc, 3400, 1000);
	cfd = accept(lfd, NULL, NULL);
	CHECK(g_connected == 2);
	lineclient_healthy(&lc);
	close(cfd);
	pump(&lc, 4000, 1000);
	CHECK(g_lost == 2 && lineclient_deadline(&lc) == 4100);

	/* stop: no retry, backoff reset; start connects at once */
	lineclient_stop(&lc, 4050);
	CHECK(lineclient_deadline(&lc) == -1 && g_lost == 2);	/* it was not up */
	lineclient_tick(&lc, 9000);
	CHECK(lineclient_pollfd(&lc).fd == -1);
	cfd = connect_up(&lc, lfd, 9000);
	CHECK(g_connected == 3);
	lineclient_stop(&lc, 9001);
	CHECK(g_lost == 3 && g_lost_at == 9001 && lineclient_deadline(&lc) == -1);
	close(cfd);
	close(lfd);
	unlink(path);
	/* after a stop the backoff starts at 100 again */
	lineclient_start(&lc, 10000);
	CHECK(lineclient_deadline(&lc) == 10100);
	lineclient_stop(&lc, 10000);
}

static void test_tcp(void)
{
	struct lineclient lc;
	struct lineclient_config cfg = unix_cfg(NULL);
	int lfd, cfd, port;

	reset();
	lfd = listen_tcp(&port);
	cfg.port = port;
	lineclient_init(&lc, &cfg);
	cfd = connect_up(&lc, lfd, 100);
	say(cfd, "{\"class\":\"VERSION\"}\n");
	pump(&lc, 100, 1000);
	CHECK(g_nlines == 1 && strcmp(g_lines[0], "{\"class\":\"VERSION\"}") == 0);
	/* a reset (RST) is an error: on_lost once */
	{
		struct linger lg = { 1, 0 };

		setsockopt(cfd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
	}
	close(cfd);
	pump(&lc, 200, 1000);
	CHECK(g_lost == 1 && lineclient_deadline(&lc) == 300);
	lineclient_stop(&lc, 200);
	CHECK(g_lost == 1);
	/* a port that refuses: never up, a retry scheduled */
	close(lfd);
	reset();
	lineclient_start(&lc, 1000);
	pump(&lc, 1001, 1000);		/* SO_ERROR, if the refusal was not immediate */
	CHECK(!lineclient_up(&lc) && lineclient_pollfd(&lc).fd == -1);
	CHECK(lineclient_deadline(&lc) == 1100 || lineclient_deadline(&lc) == 1101);
	CHECK(g_connected == 0 && g_lost == 0);
	lineclient_stop(&lc, 1001);
}

static void test_framing(void)
{
	static char big[LINECLIENT_LINE_MAX + 100];
	struct lineclient lc;
	struct lineclient_config cfg;
	char path[256];
	int lfd, cfd;

	reset();
	snprintf(path, sizeof(path), "%s/f.sock", g_dir);
	cfg = unix_cfg(path);
	lfd = listen_unix(path);
	lineclient_init(&lc, &cfg);
	cfd = connect_up(&lc, lfd, 0);

	say(cfd, "ab");
	pump(&lc, 1, 1000);
	CHECK(g_nlines == 0);
	say(cfd, "c\nde\r\nf\r\r\n\n");	/* several lines in one read; one CR stripped */
	pump(&lc, 2, 1000);
	CHECK(g_nlines == 4 && strcmp(g_lines[0], "abc") == 0 && strcmp(g_lines[1], "de") == 0);
	CHECK(strcmp(g_lines[2], "f\r") == 0 && g_lens[2] == 2 && g_lens[3] == 0);

	/* an over-long line is dropped whole, the next one survives */
	memset(big, 'x', sizeof(big));
	CHECK(send(cfd, big, sizeof(big), 0) == (ssize_t)sizeof(big));
	pump(&lc, 3, 1000);
	pump(&lc, 3, 100);
	say(cfd, "yyy\nnext\n");
	pump(&lc, 4, 1000);
	CHECK(g_nlines == 5 && strcmp(g_lines[4], "next") == 0 && lc.dropped == 1);
	/* the longest line that fits: LINECLIENT_LINE_MAX - 1 bytes */
	memset(big, 'z', LINECLIENT_LINE_MAX - 1);
	big[LINECLIENT_LINE_MAX - 1] = '\n';
	g_bytes = 0;
	CHECK(send(cfd, big, LINECLIENT_LINE_MAX, 0) == LINECLIENT_LINE_MAX);
	pump(&lc, 5, 1000);
	pump(&lc, 5, 100);
	pump(&lc, 5, 100);
	CHECK(g_nlines == 6 && g_bytes == LINECLIENT_LINE_MAX - 1 && lc.dropped == 1);

	/* at most LINECLIENT_READS reads per wakeup: 20000 bytes of 99-byte
	 * lines take two handles */
	{
		char line[101];
		int i;

		memset(line, 'l', 99);
		line[99] = '\n';
		line[100] = '\0';
		g_nlines = 0;
		g_bytes = 0;
		for (i = 0; i < 200; i++)
			say(cfd, line);
		usleep(50000);
		pump(&lc, 6, 1000);
		CHECK(g_bytes > 0 && g_bytes <= (size_t)LINECLIENT_READS * LINECLIENT_READ_SIZE);
		pump(&lc, 6, 1000);
		CHECK(g_nlines == 200 && g_bytes == 200 * 99);
	}

	/* EOF: on_lost once */
	close(cfd);
	pump(&lc, 10, 1000);
	CHECK(g_lost == 1 && g_lost_at == 10);
	pump(&lc, 11, 0);
	CHECK(g_lost == 1);
	lineclient_stop(&lc, 11);
	CHECK(g_lost == 1);
	close(lfd);
	unlink(path);
}

static void test_callback_closes(void)
{
	struct lineclient lc;
	struct lineclient_config cfg;
	char path[256], b[16] = "";
	int lfd, cfd;

	snprintf(path, sizeof(path), "%s/c.sock", g_dir);
	cfg = unix_cfg(path);
	lfd = listen_unix(path);
	lineclient_init(&lc, &cfg);

	/* close from on_line: the rest of that read is not delivered */
	reset();
	g_act = ACT_CLOSE;
	cfd = connect_up(&lc, lfd, 0);
	say(cfd, "one\nclose\nthree\n");
	pump(&lc, 50, 1000);
	CHECK(g_nlines == 2 && g_lost == 1 && g_lost_at == 50);
	CHECK(lineclient_pollfd(&lc).fd == -1 && lineclient_deadline(&lc) == 150);
	close(cfd);
	lineclient_tick(&lc, 150);
	pump(&lc, 150, 1000);
	cfd = accept(lfd, NULL, NULL);
	CHECK(g_connected == 2 && lineclient_up(&lc));	/* reconnected, the stale line gone */
	CHECK(g_nlines == 2);

	/* stop from on_line: on_lost once, no retry */
	reset();
	g_act = ACT_STOP;
	say(cfd, "close\nmore\n");
	pump(&lc, 200, 1000);
	CHECK(g_nlines == 1 && g_lost == 1 && lineclient_deadline(&lc) == -1);
	close(cfd);

	/* a send from on_line */
	reset();
	g_act = ACT_SEND;
	cfd = connect_up(&lc, lfd, 300);
	say(cfd, "close\n");
	pump(&lc, 300, 1000);
	CHECK(recv(cfd, b, sizeof(b) - 1, 0) == 5 && strcmp(b, "pong\n") == 0 && g_lost == 0);

	/* a failed send counts as a broken connection */
	{
		struct linger lg = { 1, 0 };

		setsockopt(cfd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
	}
	close(cfd);
	usleep(10000);
	{
		int i, rc = 0;

		for (i = 0; i < 10 && rc == 0; i++)
			rc = lineclient_send(&lc, "ping\n", 400);
		CHECK(rc == -1 && g_lost == 1 && g_lost_at == 400 && lineclient_deadline(&lc) == 500);
	}
	lineclient_stop(&lc, 400);
	close(lfd);
	unlink(path);
}

int main(void)
{
	CHECK(mkdtemp(g_dir) != NULL);
	test_refused_and_backoff();
	test_tcp();
	test_framing();
	test_callback_closes();
	rmdir(g_dir);
	printf("test-lineclient: %d/%d checks passed\n", g_tests - g_failures, g_tests);
	return g_failures ? 1 : 0;
}
