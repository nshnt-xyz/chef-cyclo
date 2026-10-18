/*
 * gps-manager's lease server on /run/gps-manager.sock: a line protocol in
 * the buttond style.
 *
 *   client -> gps-manager   lease map | lease ride     (counted per connection)
 *                           release map | release ride
 *                           status
 *                           watch                      (stream state changes)
 *                           retry                      (only in FAILED)
 *                           enable | disable           (the GPS switch)
 *   gps-manager -> client   ok lease map 1 | err ...
 *                           ok status state=RUNNING enabled=1 leases=map:1,ride:0 ...
 *                           ok enabled 0
 *                           state RUNNING <detail>     (watchers)
 *                           acq UP_MS FIX_MS           (watchers, after every state line and in
 *                                                       the watch reply: the age of the current
 *                                                       bring-up and the time from its start to
 *                                                       its first fix, -1 when none)
 *                           enabled 0                  (watchers)
 *
 * The switch: disabled, a new lease gets "err disabled"; leases already
 * held stay on their connections but no longer count, GNSS is torn down at
 * once (no grace) and the idle state reads DISABLED; enabling counts them
 * again (main.c saves it and reads it back at start). Until the saved
 * value is read, leases are accepted and held, not acted on.
 *
 * A lease lives on its connection, so a client that dies releases it;
 * leases are counted per connection and kind and summed for the core.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "gps-manager.h"

const char *const gm_kind_names[] = { "map", "ride" };

static int gm_kind_from_name(const char *s)
{
	int i;

	for (i = 0; s && i < LK_COUNT; i++)
		if (strcmp(s, gm_kind_names[i]) == 0)
			return i;
	return -1;
}

/* kind < 0: all kinds */
static int gm_server_count(const struct gm_server *s, int kind)
{
	int i, k, n = 0;

	for (i = 0; i < GM_MAX_CLIENTS; i++) {
		if (s->clients[i].fd < 0)
			continue;
		for (k = 0; k < LK_COUNT; k++)
			if (kind < 0 || kind == k)
				n += s->clients[i].leases[k];
	}
	return n;
}

void gm_server_init(struct gm_server *s, struct gm *m, int64_t (*now)(void))
{
	int i;

	memset(s, 0, sizeof(*s));
	s->m = m;
	s->now = now;
	for (i = 0; i < GM_MAX_CLIENTS; i++)
		s->clients[i].fd = -1;
}

/* Held leases count only while the switch is on (and has been read). */
static void gm_server_sync(struct gm_server *s)
{
	int n = s->m->enabled && s->m->loaded ? gm_server_count(s, -1) : 0;

	if (n != s->m->leases)
		gm_set_leases(s->m, s->now(), n);
}

struct gm_client *gm_server_add(struct gm_server *s, int fd)
{
	int i;

	for (i = 0; i < GM_MAX_CLIENTS; i++) {
		struct gm_client *c = &s->clients[i];

		if (c->fd >= 0)
			continue;
		memset(c, 0, sizeof(*c));
		c->fd = fd;
		return c;
	}
	return NULL;
}

static void gm_server_drop(struct gm_server *s, struct gm_client *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	memset(c->leases, 0, sizeof(c->leases));
	c->watch = false;
	gm_server_sync(s);
}

/* Non-blocking; a client whose socket buffer is full is dropped. */
static int gm_send(int fd, const char *line)
{
	size_t len = strlen(line), off = 0;

	while (off < len) {
		ssize_t n = send(fd, line + off, len - off, MSG_NOSIGNAL | MSG_DONTWAIT);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		off += (size_t)n;
	}
	return 0;
}

static void gm_reply(struct gm_server *s, struct gm_client *c, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (gm_send(c->fd, buf))
		gm_server_drop(s, c);
}

static void gm_status_line(const struct gm_server *s, char *buf, size_t len)
{
	const struct gm *m = s->m;
	int i, watchers = 0, clients = 0;

	for (i = 0; i < GM_MAX_CLIENTS; i++) {
		if (s->clients[i].fd < 0)
			continue;
		clients++;
		watchers += s->clients[i].watch;
	}
	snprintf(buf, len,
		 "ok status state=%s enabled=%d leases=map:%d,ride:%d clients=%d watchers=%d cid=%d phase=%s nmea=%lu bringups=%lu failures=%lu streak=%d leaked=%lu grace=%s detail=%s\n",
		 gm_state_names[m->state], m->enabled, gm_server_count(s, LK_MAP), gm_server_count(s, LK_RIDE),
		 clients, watchers, m->cid, gm_phase_names[m->phase], m->nmea_lines, m->bringups,
		 m->failures, m->streak, m->leaked, m->grace_at ? "on" : "off", m->detail);
}

/* The "acq UP_MS FIX_MS" line: the age of the current bring-up and its first fix, -1 none. */
static void gm_acq_line(const struct gm_server *s, char *buf, size_t len)
{
	const struct gm *m = s->m;
	int64_t now = s->now();

	if (m->start_at)
		snprintf(buf, len, "acq %lld %lld\n", (long long)(now - m->start_at),
			 m->first_fix_at ? (long long)(m->first_fix_at - m->start_at) : -1LL);
	else
		snprintf(buf, len, "acq -1 -1\n");
}

void gm_server_broadcast(struct gm_server *s)
{
	char buf[GM_DETAIL_MAX + 64];
	size_t n;
	int i;

	snprintf(buf, sizeof(buf), "state %s %s\n", gm_state_names[s->m->state], s->m->detail);
	n = strlen(buf);
	gm_acq_line(s, buf + n, sizeof(buf) - n);
	for (i = 0; i < GM_MAX_CLIENTS; i++) {
		struct gm_client *c = &s->clients[i];

		if (c->fd >= 0 && c->watch && gm_send(c->fd, buf))
			gm_server_drop(s, c);
	}
}

/* "enabled N" to every watcher. */
static void gm_server_broadcast_enabled(struct gm_server *s)
{
	char buf[16];
	int i;

	snprintf(buf, sizeof(buf), "enabled %d\n", s->m->enabled);
	for (i = 0; i < GM_MAX_CLIENTS; i++) {
		struct gm_client *c = &s->clients[i];

		if (c->fd >= 0 && c->watch && gm_send(c->fd, buf))
			gm_server_drop(s, c);
	}
}

/*
 * The switch, from a client (persist: saved) or the saved value read at
 * start. Off: held leases stop counting and GNSS goes down at once, the
 * grace skipped; idle then reads DISABLED. On: they count again.
 */
void gm_server_set_enabled(struct gm_server *s, bool on, bool persist)
{
	struct gm *m = s->m;
	int64_t now = s->now();
	bool was = m->enabled;

	m->loaded = true;
	m->enabled = on;
	if (was != on) {
		gm_log(m, "GPS %s", on ? "enabled" : "disabled");
		gm_server_broadcast_enabled(s);	/* before the state it causes */
	}
	if (!on) {
		gm_server_sync(s);	/* held leases stop counting: down at once */
		if (m->grace_at)
			gm_teardown(m, now);	/* already in its grace: no waiting either */
		if (m->phase == PH_IDLE && m->state == ST_OFF)
			gm_set(m, ST_OFF, "idle");	/* reads DISABLED */
	} else {
		if (m->state == ST_DISABLED)
			gm_set(m, ST_OFF, "idle");
		else if (m->phase == PH_IDLE && m->state == ST_OFF && strcmp(m->detail, "idle"))
			gm_set(m, ST_OFF, "idle");	/* was waiting for the saved switch */
		gm_server_sync(s);
	}
	if (persist && m->ops.persist)
		m->ops.persist(m->ops.ctx, on);
}

static void gm_handle_line(struct gm_server *s, struct gm_client *c, char *line)
{
	char *cmd, *arg, *save = NULL;

	cmd = strtok_r(line, " \t\r", &save);
	if (!cmd)
		return;
	arg = strtok_r(NULL, " \t\r", &save);
	if (strcmp(cmd, "lease") == 0 || strcmp(cmd, "release") == 0) {
		int k = gm_kind_from_name(arg);

		if (k < 0) {
			gm_reply(s, c, "err unknown lease kind %s\n", arg ? arg : "");
			return;
		}
		if (cmd[0] == 'l' && s->m->loaded && !s->m->enabled) {
			gm_reply(s, c, "err disabled\n");
			return;
		}
		if (cmd[0] == 'l') {
			c->leases[k]++;
		} else if (c->leases[k] > 0) {
			c->leases[k]--;
		} else {
			gm_reply(s, c, "err no %s lease on this connection\n", gm_kind_names[k]);
			return;
		}
		gm_reply(s, c, "ok %s %s %d\n", cmd, gm_kind_names[k], c->leases[k]);
		if (c->fd >= 0)
			gm_server_sync(s);
	} else if (strcmp(cmd, "watch") == 0) {
		c->watch = true;
		char acq[40];

		gm_acq_line(s, acq, sizeof(acq));
		gm_reply(s, c, "ok watch\nenabled %d\nstate %s %s\n%s", s->m->enabled,
			 gm_state_names[s->m->state], s->m->detail, acq);
	} else if (strcmp(cmd, "enable") == 0 || strcmp(cmd, "disable") == 0) {
		bool on = cmd[0] == 'e';

		gm_reply(s, c, "ok enabled %d\n", on);
		if (c->fd >= 0)
			gm_server_set_enabled(s, on, true);
	} else if (strcmp(cmd, "status") == 0) {
		char buf[512];

		gm_status_line(s, buf, sizeof(buf));
		gm_reply(s, c, "%s", buf);
	} else if (strcmp(cmd, "retry") == 0) {
		if (gm_retry(s->m, s->now()) == 0)
			gm_reply(s, c, "ok retry\n");
		else
			gm_reply(s, c, "err retry only applies in FAILED\n");
	} else {
		gm_reply(s, c, "err unknown command %s\n", cmd);
	}
}

void gm_client_input(struct gm_server *s, struct gm_client *c)
{
	ssize_t n;
	char *nl;

	n = recv(c->fd, c->in + c->inlen, sizeof(c->in) - 1 - c->inlen, MSG_DONTWAIT);
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
		gm_server_drop(s, c);
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
			c->discarding = false;
		else
			gm_handle_line(s, c, c->in);
		if (c->fd < 0)
			return;
		memmove(c->in, c->in + used, c->inlen - used);
		c->inlen -= used;
		c->in[c->inlen] = '\0';
	}
	if (c->discarding) {
		c->inlen = 0;
	} else if (c->inlen == sizeof(c->in) - 1) {
		gm_reply(s, c, "err line too long\n");
		c->inlen = 0;
		c->discarding = true;
	}
}
