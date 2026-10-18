/*
 * linesrv.c - see linesrv.h. The listener, client table, line framing
 * and unbuffered send were copied in buttond, sensord, blesensd and
 * gps-manager before this library; the bounded queue with the lossy
 * "dropped" count is sensord's.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "chef_log.h"
#include "linesrv.h"

#define LINESRV_ERR_LONG "{\"type\":\"error\",\"reason\":\"line too long\"}\n"
#define LINESRV_ERR_FULL "{\"type\":\"error\",\"reason\":\"too many clients\"}\n"

struct linesrv_client {
	struct linesrv *srv;
	int fd;			/* -1 = free slot */
	int slot;
	unsigned gen;		/* bumped on every drop: stale poll entries */
	bool discarding;	/* overlong line: drop bytes up to the next newline */
	char *in;		/* line_max bytes */
	size_t inlen;
	char *out;		/* outq_size bytes + 1 for vsnprintf's NUL */
	size_t outlen;
	unsigned long dropped;	/* lossy lines not yet reported */
};

struct linesrv {
	struct linesrv_config cfg;
	int lfd;
	char *path;		/* listening path, unlinked at close_all */
	struct linesrv_client *clients;
	/* what linesrv_pollfds() put in each entry: -1 = the listener */
	int *poll_slot;
	unsigned *poll_gen;
	int poll_n;
};

struct linesrv *linesrv_new(const struct linesrv_config *cfg)
{
	struct linesrv *srv;
	int i;

	if (cfg->max_clients < 1 || cfg->line_max < 2 || cfg->outq_size < 1 || !cfg->on_line)
		return NULL;
	srv = calloc(1, sizeof(*srv));
	if (!srv)
		return NULL;
	srv->cfg = *cfg;
	if (!srv->cfg.log)
		srv->cfg.log = chef_log;
	srv->lfd = -1;
	srv->clients = calloc((size_t)cfg->max_clients, sizeof(*srv->clients));
	srv->poll_slot = calloc((size_t)cfg->max_clients + 1, sizeof(*srv->poll_slot));
	srv->poll_gen = calloc((size_t)cfg->max_clients + 1, sizeof(*srv->poll_gen));
	if (!srv->clients || !srv->poll_slot || !srv->poll_gen)
		goto fail;
	for (i = 0; i < cfg->max_clients; i++) {
		struct linesrv_client *c = &srv->clients[i];

		c->srv = srv;
		c->fd = -1;
		c->slot = i;
		c->in = malloc(cfg->line_max);
		c->out = malloc(cfg->outq_size + 1);
		if (!c->in || !c->out)
			goto fail;
	}
	return srv;
fail:
	linesrv_close_all(srv);
	return NULL;
}

int linesrv_listen(struct linesrv *srv, const char *path)
{
	struct sockaddr_un sa;
	char *copy;
	int fd;

	if (srv->lfd >= 0)
		return -EBUSY;
	if (strlen(path) >= sizeof(sa.sun_path))
		return -ENAMETOOLONG;
	copy = strdup(path);
	if (!copy)
		return -ENOMEM;
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		int err = errno;

		free(copy);
		return -err;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	strcpy(sa.sun_path, path);
	unlink(path);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, 8) < 0) {
		int err = errno;

		close(fd);
		free(copy);
		return -err;
	}
	srv->lfd = fd;
	srv->path = copy;
	return 0;
}

int linesrv_connect(const char *path)
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

void linesrv_drop(struct linesrv_client *c)
{
	struct linesrv *srv = c->srv;

	if (c->fd < 0)
		return;
	close(c->fd);
	c->fd = -1;
	c->gen++;
	c->inlen = 0;
	c->outlen = 0;
	c->discarding = false;
	c->dropped = 0;
	if (srv->cfg.on_close)
		srv->cfg.on_close(srv, c);
}

struct linesrv_client *linesrv_adopt(struct linesrv *srv, int fd)
{
	struct linesrv_client *c = NULL;
	int i, fl;

	fl = fcntl(fd, F_GETFL);
	if (fl >= 0)
		fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	for (i = 0; i < srv->cfg.max_clients; i++) {
		if (srv->clients[i].fd < 0) {
			c = &srv->clients[i];
			break;
		}
	}
	if (!c) {
		srv->cfg.log("client limit (%d) reached, refusing", srv->cfg.max_clients);
		/* best effort: a peer that cannot take it learns from the close */
		send(fd, LINESRV_ERR_FULL, sizeof(LINESRV_ERR_FULL) - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
		close(fd);
		return NULL;
	}
	c->fd = fd;
	c->inlen = 0;
	c->outlen = 0;
	c->discarding = false;
	c->dropped = 0;
	if (srv->cfg.on_connect)
		srv->cfg.on_connect(srv, c);
	return c->fd >= 0 ? c : NULL;	/* on_connect may have dropped it */
}

void linesrv_close_all(struct linesrv *srv)
{
	int i;

	if (!srv)
		return;
	if (srv->clients) {
		for (i = 0; i < srv->cfg.max_clients; i++)
			if (srv->clients[i].in)	/* fully set up */
				linesrv_drop(&srv->clients[i]);
		for (i = 0; i < srv->cfg.max_clients; i++) {
			free(srv->clients[i].in);
			free(srv->clients[i].out);
		}
	}
	if (srv->lfd >= 0) {
		close(srv->lfd);
		unlink(srv->path);
	}
	free(srv->path);
	free(srv->clients);
	free(srv->poll_slot);
	free(srv->poll_gen);
	free(srv);
}

/* Push queued output; the socket is non-blocking. 0, or -1 (dropped). */
static int client_flush(struct linesrv_client *c)
{
	size_t off = 0;

	while (off < c->outlen) {
		ssize_t n = send(c->fd, c->out + off, c->outlen - off, MSG_NOSIGNAL | MSG_DONTWAIT);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			linesrv_drop(c);
			return -1;
		}
		off += (size_t)n;
	}
	memmove(c->out, c->out + off, c->outlen - off);
	c->outlen -= off;
	return 0;
}

static int client_overflow(struct linesrv_client *c)
{
	c->srv->cfg.log("client output full, disconnecting it");
	linesrv_drop(c);
	return -1;
}

int linesrv_send(struct linesrv_client *c, const char *buf, size_t len)
{
	if (c->fd < 0)
		return -1;
	if (len > c->srv->cfg.outq_size - c->outlen)
		return client_overflow(c);
	memcpy(c->out + c->outlen, buf, len);
	c->outlen += len;
	return client_flush(c);
}

int linesrv_sendf(struct linesrv_client *c, const char *fmt, ...)
{
	size_t room;
	va_list ap;
	int n;

	if (c->fd < 0)
		return -1;
	/* formatted straight into the queue: out has one spare byte for the NUL */
	room = c->srv->cfg.outq_size - c->outlen;
	va_start(ap, fmt);
	n = vsnprintf(c->out + c->outlen, room + 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return 0;
	if ((size_t)n > room)
		return client_overflow(c);
	c->outlen += (size_t)n;
	return client_flush(c);
}

int linesrv_send_lossy(struct linesrv_client *c, const char *buf, size_t len)
{
	char note[64];
	size_t k = 0;

	if (c->fd < 0)
		return -1;
	if (c->dropped)
		k = (size_t)snprintf(note, sizeof(note), "{\"type\":\"dropped\",\"count\":%lu}\n", c->dropped);
	if (k + len > c->srv->cfg.outq_size - c->outlen) {
		c->dropped++;
		return 1;
	}
	if (k) {
		memcpy(c->out + c->outlen, note, k);
		c->outlen += k;
		c->dropped = 0;
	}
	memcpy(c->out + c->outlen, buf, len);
	c->outlen += len;
	return client_flush(c);
}

/* Split LINE in place: see linesrv.h for the argc 9 overflow. */
static int tokenize(char *line, char **argv)
{
	int argc = 0;
	char *p = line, *end;

	end = line + strlen(line);
	while (end > line && (end[-1] == ' ' || end[-1] == '\t'))
		*--end = '\0';
	for (;;) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		if (argc == LINESRV_MAX_WORDS) {
			argv[argc++] = p;	/* the unsplit rest */
			break;
		}
		argv[argc++] = p;
		while (*p && *p != ' ' && *p != '\t')
			p++;
		if (!*p)
			break;
		*p++ = '\0';
	}
	return argc;
}

/* Read what the client sent and hand complete lines over. */
static void client_input(struct linesrv_client *c)
{
	struct linesrv *srv = c->srv;
	size_t line_max = srv->cfg.line_max, start = 0;
	unsigned gen = c->gen;
	ssize_t n;
	char *nl;

	n = recv(c->fd, c->in + c->inlen, line_max - c->inlen, MSG_DONTWAIT);
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
		linesrv_drop(c);
		return;
	}
	if (n < 0)
		return;
	c->inlen += (size_t)n;
	while ((nl = memchr(c->in + start, '\n', c->inlen - start))) {
		char *line = c->in + start, *argv[LINESRV_MAX_WORDS + 1];
		int argc;

		start = (size_t)(nl - c->in) + 1;
		if (c->discarding) {
			c->discarding = false;	/* the tail of a rejected line */
			continue;
		}
		*nl = '\0';
		if (nl > line && nl[-1] == '\r')
			nl[-1] = '\0';
		argc = tokenize(line, argv);
		if (argc)
			srv->cfg.on_line(srv, c, argc, argv);
		if (c->fd < 0 || c->gen != gen)
			return;
	}
	if (c->discarding) {
		c->inlen = 0;
	} else if (start == 0 && c->inlen == line_max) {
		c->inlen = 0;
		c->discarding = true;
		linesrv_send(c, LINESRV_ERR_LONG, sizeof(LINESRV_ERR_LONG) - 1);
	} else {
		memmove(c->in, c->in + start, c->inlen - start);
		c->inlen -= start;
	}
}

int linesrv_max_pollfds(const struct linesrv *srv)
{
	return 1 + srv->cfg.max_clients;
}

int linesrv_pollfds(struct linesrv *srv, struct pollfd *pfd, int max)
{
	int i, n = 0;

	if (srv->lfd >= 0 && n < max) {
		pfd[n] = (struct pollfd){ .fd = srv->lfd, .events = POLLIN };
		srv->poll_slot[n++] = -1;
	}
	for (i = 0; i < srv->cfg.max_clients && n < max; i++) {
		struct linesrv_client *c = &srv->clients[i];

		if (c->fd < 0)
			continue;
		pfd[n] = (struct pollfd){ .fd = c->fd, .events = (short)(POLLIN | (c->outlen ? POLLOUT : 0)) };
		srv->poll_slot[n] = i;
		srv->poll_gen[n++] = c->gen;
	}
	srv->poll_n = n;
	return n;
}

void linesrv_handle(struct linesrv *srv, const struct pollfd *pfd, int n)
{
	bool accept_ready = false;
	int i;

	if (n > srv->poll_n)
		n = srv->poll_n;
	for (i = 0; i < n; i++) {
		struct linesrv_client *c;
		int slot = srv->poll_slot[i];

		if (slot < 0) {
			accept_ready = (pfd[i].revents & POLLIN) != 0;
			continue;
		}
		c = &srv->clients[slot];
		if (c->fd < 0 || c->gen != srv->poll_gen[i] || c->fd != pfd[i].fd)
			continue;
		if ((pfd[i].revents & POLLOUT) && client_flush(c))
			continue;
		if (pfd[i].revents & (POLLIN | POLLHUP | POLLERR))
			client_input(c);
	}
	/* after the clients, so a slot freed above can take a new one */
	while (accept_ready && srv->lfd >= 0) {
		int cfd = accept4(srv->lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

		if (cfd < 0)
			break;
		linesrv_adopt(srv, cfd);
	}
}

void *linesrv_ctx(const struct linesrv *srv)
{
	return srv->cfg.ctx;
}

int linesrv_max_clients(const struct linesrv *srv)
{
	return srv->cfg.max_clients;
}

struct linesrv_client *linesrv_client_at(struct linesrv *srv, int slot)
{
	if (slot < 0 || slot >= srv->cfg.max_clients || srv->clients[slot].fd < 0)
		return NULL;
	return &srv->clients[slot];
}

int linesrv_slot(const struct linesrv_client *c)
{
	return c->slot;
}

int linesrv_fd(const struct linesrv_client *c)
{
	return c->fd;
}

struct linesrv *linesrv_of(const struct linesrv_client *c)
{
	return c->srv;
}
