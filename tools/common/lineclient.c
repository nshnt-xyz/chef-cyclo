/*
 * lineclient.c - the client connection core (lineclient.h).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "lineclient.h"

void lineclient_init(struct lineclient *lc, const struct lineclient_config *cfg)
{
	memset(lc, 0, sizeof(*lc));
	lc->cfg = *cfg;
	if (cfg->path) {
		snprintf(lc->path, sizeof(lc->path), "%s", cfg->path);
		lc->cfg.path = lc->path;
	}
	lc->fd = -1;
	lc->retry_at = -1;
	lc->backoff_ms = cfg->backoff_min_ms;
}

void *lineclient_ctx(const struct lineclient *lc)
{
	return lc->cfg.ctx;
}

bool lineclient_up(const struct lineclient *lc)
{
	return lc->up;
}

/* Close the fd; on_lost if the connection had been up. */
static void lc_close(struct lineclient *lc, int64_t now)
{
	bool was_up = lc->up;

	if (lc->fd >= 0)
		close(lc->fd);
	lc->fd = -1;
	lc->connecting = lc->up = false;
	lc->len = 0;
	lc->skipping = false;
	lc->gen++;
	if (was_up && lc->cfg.on_lost)
		lc->cfg.on_lost(lc, now);
}

/* The next attempt after the current backoff, which then doubles. */
static void lc_schedule(struct lineclient *lc, int64_t now)
{
	if (!lc->wanted || lc->fd >= 0)
		return;
	lc->retry_at = now + lc->backoff_ms;
	lc->backoff_ms = lc->backoff_ms * 2 > lc->cfg.backoff_max_ms ? lc->cfg.backoff_max_ms
								    : lc->backoff_ms * 2;
}

static void lc_connect(struct lineclient *lc, int64_t now)
{
	struct sockaddr_un su = { .sun_family = AF_UNIX };
	struct sockaddr_in si = { .sin_family = AF_INET };
	struct sockaddr *sa;
	socklen_t salen;
	int fd;

	lc->retry_at = -1;
	if (lc->cfg.path) {
		if (strlen(lc->path) >= sizeof(su.sun_path)) {
			lc_schedule(lc, now);
			return;
		}
		strcpy(su.sun_path, lc->path);
		sa = (struct sockaddr *)&su;
		salen = sizeof(su);
	} else {
		si.sin_port = htons((uint16_t)lc->cfg.port);
		si.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		sa = (struct sockaddr *)&si;
		salen = sizeof(si);
	}
	fd = socket(sa->sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		lc_schedule(lc, now);
		return;
	}
	if (connect(fd, sa, salen) && errno != EINPROGRESS) {
		close(fd);	/* ENOENT, ECONNREFUSED, EAGAIN: not there yet */
		lc_schedule(lc, now);
		return;
	}
	/* even a connect that completed at once is finished on POLLOUT */
	lc->fd = fd;
	lc->connecting = true;
}

void lineclient_start(struct lineclient *lc, int64_t now)
{
	bool was = lc->wanted;

	lc->wanted = true;
	if (lc->fd >= 0 || (was && lc->retry_at >= 0))
		return;
	lc_connect(lc, now);
}

void lineclient_stop(struct lineclient *lc, int64_t now)
{
	lc->wanted = false;
	lc->retry_at = -1;
	lc->backoff_ms = lc->cfg.backoff_min_ms;
	if (lc->fd >= 0)
		lc_close(lc, now);
}

void lineclient_close(struct lineclient *lc, int64_t now)
{
	if (lc->fd < 0)
		return;
	lc_close(lc, now);
	lc_schedule(lc, now);
}

void lineclient_healthy(struct lineclient *lc)
{
	lc->backoff_ms = lc->cfg.backoff_min_ms;
}

int lineclient_send(struct lineclient *lc, const char *line, int64_t now)
{
	size_t n = strlen(line);
	ssize_t w;

	if (!lc->up)
		return -1;
	do
		w = send(lc->fd, line, n, MSG_NOSIGNAL | MSG_DONTWAIT);
	while (w < 0 && errno == EINTR);
	if (w == (ssize_t)n)
		return 0;
	lineclient_close(lc, now);	/* short requests: partial = broken */
	return -1;
}

struct pollfd lineclient_pollfd(const struct lineclient *lc)
{
	struct pollfd p = { .fd = lc->fd, .events = 0 };

	if (lc->fd >= 0)
		p.events = lc->connecting ? POLLOUT : POLLIN;
	return p;
}

/* Bytes into lines; false when a callback closed the connection. */
static bool lc_feed(struct lineclient *lc, const char *p, size_t n, int64_t now)
{
	unsigned long gen = lc->gen;

	while (n) {
		const char *nl = memchr(p, '\n', n);
		size_t k = nl ? (size_t)(nl - p) : n;

		if (lc->skipping) {
			/* nothing kept until the newline */
		} else if (lc->len + k >= sizeof(lc->buf)) {
			lc->skipping = true;
			lc->dropped++;
			lc->len = 0;
		} else {
			memcpy(lc->buf + lc->len, p, k);
			lc->len += k;
		}
		if (!nl)
			return true;
		if (!lc->skipping) {
			size_t len = lc->len;

			if (len && lc->buf[len - 1] == '\r')
				len--;
			lc->buf[len] = '\0';
			lc->len = 0;
			if (lc->cfg.on_line)
				lc->cfg.on_line(lc, lc->buf, len, now);
			if (lc->gen != gen)
				return false;
		}
		lc->len = 0;
		lc->skipping = false;
		p = nl + 1;
		n -= k + 1;
	}
	return true;
}

void lineclient_handle(struct lineclient *lc, short revents, int64_t now)
{
	char buf[LINECLIENT_READ_SIZE];
	unsigned long gen = lc->gen;
	int i;

	if (lc->fd < 0 || !revents)
		return;
	if (lc->connecting) {
		int err = 0;
		socklen_t l = sizeof(err);

		if (getsockopt(lc->fd, SOL_SOCKET, SO_ERROR, &err, &l) || err ||
		    (revents & (POLLERR | POLLHUP | POLLNVAL))) {
			lc_close(lc, now);	/* never up: no on_lost */
			lc_schedule(lc, now);
			return;
		}
		if (!(revents & POLLOUT))
			return;
		lc->connecting = false;
		lc->up = true;
		if (lc->cfg.on_connected)
			lc->cfg.on_connected(lc, now);
		return;
	}
	for (i = 0; i < LINECLIENT_READS && lc->gen == gen; i++) {
		ssize_t n = recv(lc->fd, buf, sizeof(buf), MSG_DONTWAIT);

		if (n < 0 && errno == EINTR)
			continue;	/* counts as one of the reads */
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		if (n <= 0) {
			lineclient_close(lc, now);	/* EOF or an error */
			return;
		}
		if (!lc_feed(lc, buf, (size_t)n, now))
			return;
	}
}

int64_t lineclient_deadline(const struct lineclient *lc)
{
	return lc->wanted && lc->fd < 0 ? lc->retry_at : -1;
}

void lineclient_tick(struct lineclient *lc, int64_t now)
{
	if (lc->wanted && lc->fd < 0 && lc->retry_at >= 0 && now >= lc->retry_at)
		lc_connect(lc, now);
}
