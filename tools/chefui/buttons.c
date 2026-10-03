/* buttons.c - see buttons.h. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "buttons.h"

static void nolog(const char *fmt, ...)
{
	(void)fmt;
}

static bool has_claim(const struct cu_buttons *b, const char *g)
{
	int i;

	for (i = 0; i < b->nclaims; i++)
		if (strcmp(b->claims[i], g) == 0)
			return true;
	return false;
}

static void add_claim(struct cu_buttons *b, const char *g)
{
	if (!g[0] || has_claim(b, g))
		return;
	if (strcmp(g, "power+voldown") == 0) {
		b->log("not claiming %s: reserved", g);
		return;
	}
	if (b->nclaims == CU_BTN_MAX_CLAIMS || strlen(g) >= CU_BTN_NAME_MAX) {
		b->log("not claiming %s: too many or too long", g);
		return;
	}
	snprintf(b->claims[b->nclaims++], CU_BTN_NAME_MAX, "%s", g);
}

void cu_buttons_init(struct cu_buttons *b, const char *sock_path, const char *const *extra,
		     cu_btn_log_fn log)
{
	memset(b, 0, sizeof(*b));
	b->fd = -1;
	b->log = log ? log : nolog;
	snprintf(b->sock_path, sizeof(b->sock_path), "%s", sock_path ? sock_path : CU_BTN_SOCK_PATH);
	add_claim(b, "power.short");
	for (; extra && *extra; extra++)
		add_claim(b, *extra);
}

int cu_buttons_connect(struct cu_buttons *b, int64_t now_ms)
{
	struct sockaddr_un sa;
	char line[CU_BTN_NAME_MAX + 8];
	int fd, i;

	if (b->fd >= 0)
		return 0;
	b->next_try_ms = now_ms + CU_BTN_RETRY_MS;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", b->sock_path);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return -errno;
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		int e = errno;

		close(fd);
		return -e;
	}
	for (i = 0; i < b->nclaims; i++) {
		int n = snprintf(line, sizeof(line), "claim %s\n", b->claims[i]);

		if (send(fd, line, (size_t)n, MSG_NOSIGNAL) != n) {
			int e = errno ? errno : EIO;

			close(fd);
			return -e;
		}
	}
	b->fd = fd;
	b->inlen = 0;
	b->discarding = false;
	b->connects++;
	b->log("buttond connected (%s), %d claim%s", b->sock_path, b->nclaims,
	       b->nclaims == 1 ? "" : "s");
	return 0;
}

bool cu_buttons_dispatch(struct cu_buttons *b, const char *gesture)
{
	if (b->app_cb && b->app_cb(gesture, b->app_user))
		return true;
	if (strcmp(gesture, "power.short") == 0 && b->default_cb)
		b->default_cb(gesture, b->default_user);
	return false;
}

static void handle_line(struct cu_buttons *b, char *line)
{
	size_t n = strlen(line);

	while (n && (line[n - 1] == '\r' || line[n - 1] == ' '))
		line[--n] = '\0';
	if (strncmp(line, "event ", 6) == 0 && line[6])
		cu_buttons_dispatch(b, line + 6);
	else if (strncmp(line, "err", 3) == 0)
		b->log("buttond: %s", line);
	/* "ok ..." acknowledgements need nothing */
}

void cu_buttons_feed(struct cu_buttons *b, const char *data, size_t n)
{
	while (n) {
		size_t room = sizeof(b->in) - 1 - b->inlen, take = n < room ? n : room;
		char *nl;

		memcpy(b->in + b->inlen, data, take);
		b->inlen += take;
		b->in[b->inlen] = '\0';
		data += take;
		n -= take;
		while ((nl = memchr(b->in, '\n', b->inlen))) {
			size_t used = (size_t)(nl - b->in) + 1;

			*nl = '\0';
			if (b->discarding) {
				b->discarding = false;
			} else {
				unsigned long gen = b->closes;

				handle_line(b, b->in);
				if (b->closes != gen)
					return;	/* a callback closed the link (failed handoff) */
			}
			memmove(b->in, b->in + used, b->inlen - used);
			b->inlen -= used;
			b->in[b->inlen] = '\0';
		}
		if (b->inlen == sizeof(b->in) - 1) {
			/* no newline in a full buffer: drop up to the next one */
			b->inlen = 0;
			b->discarding = true;
		}
	}
}

void cu_buttons_close(struct cu_buttons *b)
{
	b->closes++;
	if (b->fd >= 0)
		close(b->fd);
	b->fd = -1;
	b->inlen = 0;
	b->discarding = false;
}

void cu_buttons_input(struct cu_buttons *b, int64_t now_ms)
{
	char buf[256];

	while (b->fd >= 0) {
		ssize_t n = recv(b->fd, buf, sizeof(buf), MSG_DONTWAIT);

		if (n > 0) {
			cu_buttons_feed(b, buf, (size_t)n);
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
			return;
		b->log("buttond connection lost (%s); reconnecting every %d ms",
		       n == 0 ? "closed" : strerror(errno), CU_BTN_RETRY_MS);
		cu_buttons_close(b);
		b->next_try_ms = now_ms + CU_BTN_RETRY_MS;
	}
}

int cu_buttons_timeout_ms(const struct cu_buttons *b, int64_t now_ms)
{
	int64_t t;

	if (b->fd >= 0)
		return -1;
	t = b->next_try_ms - now_ms;
	return t < 0 ? 0 : (int)t;
}

void cu_buttons_tick(struct cu_buttons *b, int64_t now_ms)
{
	if (b->fd < 0 && now_ms >= b->next_try_ms)
		(void)cu_buttons_connect(b, now_ms);
}
