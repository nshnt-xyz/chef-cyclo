/* Host tests for buttons.c over a real AF_UNIX listener standing in for
 * buttond: claims sent on connect, partial-line parsing, reconnect and
 * re-claim after the server closes, the default power.short toggle versus
 * an application override. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "check.h"
#include "../buttons.h"

static char g_dir[64], g_sock[128];

static int listen_at(const char *path)
{
	struct sockaddr_un sa;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
	unlink(path);
	CHECK(fd >= 0 && bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(fd, 4) == 0);
	return fd;
}

/* Everything the client sent, read until it has been quiet for 50 ms. */
static void read_all(int fd, char *buf, size_t cap)
{
	size_t n = 0;
	struct pollfd p = { .fd = fd, .events = POLLIN };

	while (n < cap - 1 && poll(&p, 1, 50) > 0) {
		ssize_t r = read(fd, buf + n, cap - 1 - n);

		if (r <= 0)
			break;
		n += (size_t)r;
	}
	buf[n] = '\0';
}

static char g_events[512];
static int g_defaults;
static bool g_handle;

static bool app_cb(const char *g, void *user)
{
	(void)user;
	strncat(g_events, g, sizeof(g_events) - strlen(g_events) - 2);
	strcat(g_events, ";");
	return g_handle;
}

static void default_cb(const char *g, void *user)
{
	(void)user;
	CHECK(strcmp(g, "power.short") == 0);
	g_defaults++;
}

static char g_log[1024];

static void logf_(const char *fmt, ...)
{
	va_list ap;
	size_t n = strlen(g_log);

	va_start(ap, fmt);
	vsnprintf(g_log + n, sizeof(g_log) - n, fmt, ap);
	va_end(ap);
	strncat(g_log, "\n", sizeof(g_log) - strlen(g_log) - 1);
}

static void reset(void)
{
	g_events[0] = '\0';
	g_defaults = 0;
	g_handle = false;
	g_log[0] = '\0';
}

static void test_claims_on_connect(void)
{
	struct cu_buttons b;
	const char *extra[] = { "volup.short", "power.short", "power+voldown", "voldown.long", NULL };
	char buf[512];
	int l = listen_at(g_sock), s;

	reset();
	cu_buttons_init(&b, g_sock, extra, logf_);
	CHECK(b.nclaims == 3);		/* power.short once, the chord refused */
	CHECK(strstr(g_log, "power+voldown") != NULL);
	CHECK(cu_buttons_connect(&b, 0) == 0 && b.fd >= 0);
	s = accept(l, NULL, NULL);
	read_all(s, buf, sizeof(buf));
	CHECK(strcmp(buf, "claim power.short\nclaim volup.short\nclaim voldown.long\n") == 0);
	CHECK(strstr(buf, "power.long") == NULL);
	CHECK(cu_buttons_timeout_ms(&b, 0) == -1);
	cu_buttons_close(&b);
	close(s);
	close(l);

	/* defaults: only power.short */
	cu_buttons_init(&b, NULL, NULL, NULL);
	CHECK(b.nclaims == 1 && strcmp(b.claims[0], "power.short") == 0);
	CHECK(strcmp(b.sock_path, "/run/buttond.sock") == 0);
}

static void test_partial_lines(void)
{
	struct cu_buttons b;
	char big[600];

	reset();
	cu_buttons_init(&b, g_sock, NULL, logf_);
	b.app_cb = app_cb;
	cu_buttons_feed(&b, "ok claim power.short\nev", 23);
	CHECK(g_events[0] == '\0');
	cu_buttons_feed(&b, "ent power.sh", 12);
	CHECK(g_events[0] == '\0');
	cu_buttons_feed(&b, "ort\nevent volup.short\r\nerr unknown gesture x\n", 45);
	CHECK(strcmp(g_events, "power.short;volup.short;") == 0);
	CHECK(strstr(g_log, "err unknown gesture x") != NULL);
	/* byte by byte */
	{
		const char *m = "event voldown.long\n";
		size_t i;

		for (i = 0; m[i]; i++)
			cu_buttons_feed(&b, m + i, 1);
	}
	CHECK(strcmp(g_events, "power.short;volup.short;voldown.long;") == 0);
	/* an over-long line is dropped up to its newline, the next one works */
	memset(big, 'x', sizeof(big));
	cu_buttons_feed(&b, "event ", 6);
	cu_buttons_feed(&b, big, sizeof(big));
	cu_buttons_feed(&b, "\nevent power.double\n", 20);
	CHECK(strcmp(g_events, "power.short;volup.short;voldown.long;power.double;") == 0);
	CHECK(b.inlen == 0);
}

static void test_reconnect(void)
{
	struct cu_buttons b;
	char buf[256];
	const char *extra[] = { "volup.short", NULL };
	int l = listen_at(g_sock), s;

	reset();
	cu_buttons_init(&b, g_sock, extra, logf_);
	b.app_cb = app_cb;
	CHECK(cu_buttons_connect(&b, 1000) == 0);
	s = accept(l, NULL, NULL);
	read_all(s, buf, sizeof(buf));
	CHECK(write(s, "ok claim power.short\nevent volup.short\n", 39) == 39);
	cu_buttons_input(&b, 1000);
	CHECK(strcmp(g_events, "volup.short;") == 0);

	/* buttond restarts: the connection closes */
	close(s);
	cu_buttons_input(&b, 2000);
	CHECK(b.fd == -1);
	CHECK(cu_buttons_timeout_ms(&b, 2000) == 1000);
	CHECK(cu_buttons_timeout_ms(&b, 2600) == 400);
	cu_buttons_tick(&b, 2500);		/* backoff not over */
	CHECK(b.fd == -1);

	/* not listening yet: the try fails, the next is 1 s later */
	close(l);
	unlink(g_sock);
	cu_buttons_tick(&b, 3000);
	CHECK(b.fd == -1 && cu_buttons_timeout_ms(&b, 3000) == 1000);

	/* back: reconnect and claim again */
	l = listen_at(g_sock);
	cu_buttons_tick(&b, 3999);
	CHECK(b.fd == -1);
	cu_buttons_tick(&b, 4000);
	CHECK(b.fd >= 0 && b.connects == 2);
	s = accept(l, NULL, NULL);
	read_all(s, buf, sizeof(buf));
	CHECK(strcmp(buf, "claim power.short\nclaim volup.short\n") == 0);
	CHECK(write(s, "event power.short\n", 18) == 18);
	cu_buttons_input(&b, 4100);
	CHECK(strcmp(g_events, "volup.short;power.short;") == 0);
	cu_buttons_close(&b);
	close(s);
	close(l);
	unlink(g_sock);
}

static void test_default_toggle(void)
{
	struct cu_buttons b;

	reset();
	cu_buttons_init(&b, g_sock, NULL, NULL);
	b.default_cb = default_cb;

	/* no application callback: power.short toggles, others do nothing */
	CHECK(!cu_buttons_dispatch(&b, "power.short"));
	CHECK(g_defaults == 1);
	CHECK(!cu_buttons_dispatch(&b, "volup.short"));
	CHECK(g_defaults == 1);

	/* the application sees it first but does not handle it */
	b.app_cb = app_cb;
	g_handle = false;
	CHECK(!cu_buttons_dispatch(&b, "power.short"));
	CHECK(g_defaults == 2 && strcmp(g_events, "power.short;") == 0);

	/* the application overrides it */
	g_handle = true;
	CHECK(cu_buttons_dispatch(&b, "power.short"));
	CHECK(g_defaults == 2);
	CHECK(strcmp(g_events, "power.short;power.short;") == 0);

	/* also through the socket path */
	g_handle = false;
	cu_buttons_feed(&b, "event power.short\n", 18);
	CHECK(g_defaults == 3);
}

/* A callback that closes the connection mid-buffer (a failed handoff
 * reconnects from inside the dispatch) must not corrupt the parser. */
static struct cu_buttons *g_closing;

static bool closing_cb(const char *g, void *user)
{
	(void)user;
	strncat(g_events, g, sizeof(g_events) - strlen(g_events) - 2);
	strcat(g_events, ";");
	cu_buttons_close(g_closing);
	return true;
}

static void test_close_inside_callback(void)
{
	struct cu_buttons b;

	reset();
	cu_buttons_init(&b, g_sock, NULL, NULL);
	b.app_cb = closing_cb;
	g_closing = &b;
	cu_buttons_feed(&b, "event power.short\nevent volup.short\npartial", 44);
	CHECK(strcmp(g_events, "power.short;") == 0);	/* the rest went with the link */
	CHECK(b.inlen == 0);
	b.app_cb = app_cb;
	cu_buttons_feed(&b, "event voldown.short\n", 20);
	CHECK(strcmp(g_events, "power.short;voldown.short;") == 0);
}

int main(void)
{
	snprintf(g_dir, sizeof(g_dir), "/tmp/chefui-btn-XXXXXX");
	if (!mkdtemp(g_dir))
		return 1;
	snprintf(g_sock, sizeof(g_sock), "%s/buttond.sock", g_dir);
	test_claims_on_connect();
	test_partial_lines();
	test_reconnect();
	test_default_toggle();
	test_close_inside_callback();
	unlink(g_sock);
	rmdir(g_dir);
	return check_report("test-buttons");
}
