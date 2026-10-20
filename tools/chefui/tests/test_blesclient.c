/*
 * test_blesclient.c - sys/blesclient.c (the blesensd client without LVGL)
 * with an injected clock and its own poll() loop, against a fake blesensd
 * on a real AF_UNIX socket: wants sent only after `ok watch` and again
 * after a reconnect, the reconnect deadlines, the view from blesensd's
 * lines, pair/cancel/forget requests, the view emptied when blesensd goes
 * (a running pairing failed), a hello with another proto logged once.
 * Built without LVGL's include path (Makefile: test-blesclient).
 * ble_apply_line's own rules are in test-sys.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "blesclient.h"
#include "check.h"

#define BA "AA:BB:CC:DD:EE:01"

static int g_changed;
static int g_logged;
static char g_log[512];

static void on_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(g_log, sizeof(g_log), fmt, ap);
	va_end(ap);
	g_logged++;
}

static void on_changed(struct blesclient *bc, void *ctx)
{
	(void)bc;
	(void)ctx;
	g_changed++;
}

static int listen_unix(const char *path)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	snprintf(sa.sun_path, sizeof(sa.sun_path), "%.100s", path);
	unlink(path);
	CHECK(bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
	CHECK(listen(fd, 4) == 0);
	return fd;
}

static void pump(struct blesclient *bc, int64_t now)
{
	for (int i = 0; i < 3; i++) {
		struct pollfd p = lineclient_pollfd(&bc->lc);

		if (p.fd < 0 || poll(&p, 1, 50) <= 0)
			return;
		lineclient_handle(&bc->lc, p.revents, now);
	}
}

static int accept_one(int lfd)
{
	struct pollfd p = { lfd, POLLIN, 0 };

	return poll(&p, 1, 1000) > 0 ? accept4(lfd, NULL, NULL, SOCK_CLOEXEC) : -1;
}

static void say(int fd, const char *s)
{
	CHECK(send(fd, s, strlen(s), MSG_NOSIGNAL) == (ssize_t)strlen(s));
}

static void heard(int fd, char *buf, size_t len)
{
	struct pollfd p = { fd, POLLIN, 0 };
	size_t n = 0;

	while (n < len - 1 && poll(&p, 1, 20) > 0) {
		ssize_t r = recv(fd, buf + n, len - 1 - n, MSG_DONTWAIT);

		if (r <= 0)
			break;
		n += (size_t)r;
	}
	buf[n] = '\0';
}

int main(void)
{
	char dir[] = "/tmp/blesclient-test.XXXXXX", sock[256], buf[512];
	struct blesclient bc;
	struct blesclient_config cfg = { .changed = on_changed, .log = on_log };
	struct ble_found f = { .addr = BA, .name = "Combo", .kinds = 7 };
	int lfd, c;
	int64_t t = 5000;

	CHECK(mkdtemp(dir) != NULL);
	snprintf(sock, sizeof(sock), "%s/ble.sock", dir);
	cfg.path = sock;
	blesclient_init(&bc, &cfg);

	/* wanted before anything is there: retries at 1, 2 s, nothing sent */
	blesclient_want(&bc, true, true, t);
	blesclient_start(&bc, t);
	CHECK(lineclient_deadline(&bc.lc) == t + 1000 && !bc.view.known);
	CHECK(blesclient_pair(&bc, &f, t) == -ENOTCONN && blesclient_forget(&bc, BA, t) == -ENOTCONN);
	t += 1000;
	lineclient_tick(&bc.lc, t);
	CHECK(lineclient_deadline(&bc.lc) == t + 2000);

	lfd = listen_unix(sock);
	t += 2000;
	lineclient_tick(&bc.lc, t);
	pump(&bc, t);
	c = accept_one(lfd);
	CHECK(c >= 0);
	heard(c, buf, sizeof(buf));
	CHECK(strcmp(buf, "watch\n") == 0);	/* the wants wait for ok watch */
	say(c, "{\"type\":\"hello\",\"daemon\":\"blesensd\",\"proto\":1}\n{\"type\":\"ok\",\"cmd\":\"watch\"}\n");
	pump(&bc, t);
	heard(c, buf, sizeof(buf));
	CHECK(strcmp(buf, "hold on\nscan on\n") == 0 && bc.view.known && g_changed > 0);
	CHECK(g_logged == 0);	/* the proto it knows */
	say(c, "{\"type\":\"adapter\",\"present\":true,\"powered\":true,\"scanning\":true}\n"
	       "{\"type\":\"sensor\",\"addr\":\"" BA "\",\"kinds\":\"hr\",\"name\":\"Combo\",\"state\":\"connected\"}\n"
	       "{\"type\":\"value\",\"addr\":\"" BA "\",\"kind\":\"hr\",\"value\":141}\n");
	pump(&bc, t);
	CHECK(bc.view.powered && bc.view.n == 1 && bc.view.s[0].val[BLE_HR].has && bc.view.s[0].val[BLE_HR].v == 141);

	/* requests */
	t += 10;
	CHECK(blesclient_pair(&bc, &f, t) == 0 && bc.view.pair.st == BLE_PAIR_RUNNING && bc.view.pair.started_ms == t);
	CHECK(blesclient_pair(&bc, &f, t) == -ENOTCONN);	/* one at a time */
	heard(c, buf, sizeof(buf));
	CHECK(strcmp(buf, "pair " BA "\n") == 0);
	CHECK(blesclient_forget(&bc, BA, t) == 0);
	heard(c, buf, sizeof(buf));
	CHECK(strcmp(buf, "forget " BA "\n") == 0);

	/* blesensd goes: nothing of it is current, the pairing failed; the
	 * watch reply made it healthy: back in 1 s */
	close(c);
	pump(&bc, t);
	CHECK(!bc.view.known && bc.view.n == 0 && !bc.view.powered && bc.view.pair.st == BLE_PAIR_FAILED);
	CHECK(strcmp(bc.view.pair.reason, "blesensd stopped") == 0);
	CHECK(lineclient_deadline(&bc.lc) == t + 1000);
	blesclient_pair_clear(&bc);
	CHECK(bc.view.pair.st == BLE_PAIR_NONE);
	t += 1000;
	lineclient_tick(&bc.lc, t);
	pump(&bc, t);
	c = accept_one(lfd);
	/* a hello with another proto: logged once, the client carries on */
	say(c, "{\"type\":\"hello\",\"daemon\":\"blesensd\",\"proto\":2}\n{\"type\":\"ok\",\"cmd\":\"watch\"}\n");
	pump(&bc, t);
	heard(c, buf, sizeof(buf));
	CHECK(strcmp(buf, "watch\nhold on\nscan on\n") == 0);	/* the wants again */
	CHECK(g_logged == 1 && strstr(g_log, "\"proto\":2") && strstr(g_log, "speaks proto 1"));
	say(c, "{\"type\":\"hello\",\"daemon\":\"blesensd\",\"proto\":2}\n");
	pump(&bc, t);
	CHECK(g_logged == 1 && bc.watching);
	blesclient_want(&bc, false, true, t);	/* scan implies hold */
	heard(c, buf, sizeof(buf));
	CHECK(strcmp(buf, "hold off\nscan off\n") == 0);
	CHECK(blesclient_pair(&bc, &f, t) == 0 && blesclient_cancel(&bc, t) == 0);
	heard(c, buf, sizeof(buf));
	CHECK(strcmp(buf, "pair " BA "\ncancel " BA "\n") == 0 && bc.view.pair.st == BLE_PAIR_NONE);

	blesclient_stop(&bc, t);
	CHECK(bc.lc.fd < 0 && lineclient_deadline(&bc.lc) == -1 && !bc.view.known);
	close(c);
	close(lfd);
	unlink(sock);
	rmdir(dir);
	return check_report("test-blesclient");
}
