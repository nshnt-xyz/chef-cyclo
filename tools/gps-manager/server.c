/*
 * gps-manager's lease server on /run/gps-manager.sock. Requests are
 * words, one line each; every line gps-manager sends is one JSON object
 * with a "type" member (the shared envelope of
 * docs/features/local-sockets.md, served by tools/common/linesrv.c),
 * proto 1, written down in docs/features/gps.md "Socket contract":
 *
 *   (connect)        {"type":"hello","daemon":"gps-manager","proto":1}
 *   lease K          {"type":"ok","cmd":"lease","kind":"map","held":1}
 *                    (K: map or ride; held: this connection's count of
 *                    that kind after the request)
 *   release K        {"type":"ok","cmd":"release","kind":"map","held":0}
 *   watch            {"type":"ok","cmd":"watch"}, then at once a switch
 *                    and a state line, then every change of either
 *   enable, disable  {"type":"ok","cmd":"enable","enabled":true}
 *   retry            {"type":"ok","cmd":"retry"} (only in FAILED)
 *   status           {"type":"status","state":"RUNNING","enabled":true,
 *                    "leases":{"map":1,"ride":0},"clients":2,"watchers":1,
 *                    "cid":1,"phase":"up","nmea":1234,"bringups":1,
 *                    "failures":0,"streak":0,"leaked":0,"grace":false,
 *                    "up_ms":84000,"fix_ms":80000,"detail":"CID 1, fix"}
 *                    (cid, up_ms, fix_ms null when there is none)
 *   events           {"type":"switch","enabled":false} before the state
 *                    change it causes, and {"type":"state","state":
 *                    "RUNNING","detail":"CID 1, fix","up_ms":84000,
 *                    "fix_ms":80000}: up_ms the age of the current
 *                    bring-up, fix_ms the time from its start to its
 *                    first fix, each null when there is none; watchers
 *                    only
 *   errors           {"type":"error","cmd":"lease","reason":"unknown lease
 *                    kind","kind":"x"}, {...,"reason":"disabled"} (a lease
 *                    while the switch is off), {"type":"error","cmd":
 *                    "release","reason":"no lease","kind":"map"},
 *                    {"type":"error","cmd":"retry","reason":"not failed"},
 *                    {...,"reason":"bad arguments"}, {"type":"error","cmd":
 *                    "x","reason":"unknown command"} (an echoed word is its
 *                    first 32 bytes, JSON-escaped), and from the library
 *                    "line too long" (over 127 bytes) and "too many
 *                    clients" (the 17th connection, then closed)
 *
 * Empty lines are ignored and a CR before the newline is stripped. Every
 * line is must-deliver: a client that lets its 4096-byte output queue
 * overflow is dropped, its leases with it.
 *
 * The switch: disabled, a new lease gets the "disabled" error; leases
 * already held stay on their connections but no longer count, GNSS is torn
 * down at once (no grace) and the idle state reads DISABLED; enabling
 * counts them again (main.c saves it and reads it back at start). Until
 * the saved value is read, leases are accepted and held, not acted on.
 *
 * A lease lives on its connection, so a client that dies releases it;
 * leases are counted per connection and kind and summed for the core.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "common/chef_log.h"
#include "common/json.h"
#include "common/linesrv.h"
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

/* kind < 0: all kinds. 0 after gm_server_fini(): no connection holds any. */
static int gm_server_count(const struct gm_server *s, int kind)
{
	int i, k, n = 0;

	for (i = 0; s->srv && i < GM_MAX_CLIENTS; i++) {
		if (!linesrv_client_at(s->srv, i))
			continue;
		for (k = 0; k < LK_COUNT; k++)
			if (kind < 0 || kind == k)
				n += s->clients[i].leases[k];
	}
	return n;
}

/* Held leases count only while the switch is on (and has been read). */
static void gm_server_sync(struct gm_server *s)
{
	int n = s->m->enabled && s->m->loaded ? gm_server_count(s, -1) : 0;

	if (n != s->m->leases)
		gm_set_leases(s->m, s->now(), n);
}

static struct gm_client *gm_client_of(struct gm_server *s, struct linesrv_client *c)
{
	return &s->clients[linesrv_slot(c)];
}

static void on_connect(struct linesrv *srv, struct linesrv_client *c)
{
	struct gm_server *s = linesrv_ctx(srv);

	memset(gm_client_of(s, c), 0, sizeof(struct gm_client));
	linesrv_sendf(c, "{\"type\":\"hello\",\"daemon\":\"gps-manager\",\"proto\":%d}\n", GM_PROTO);
}

/* Exactly once per connection, whatever ended it: its leases go. Inside a
 * broadcast the count is synced once the loop is over, so no watcher gets
 * a newer line ahead of the one being sent. */
static void on_close(struct linesrv *srv, struct linesrv_client *c)
{
	struct gm_server *s = linesrv_ctx(srv);

	memset(gm_client_of(s, c), 0, sizeof(struct gm_client));
	if (s->closing)
		return;
	if (s->broadcasting)
		s->sync_pending = true;
	else
		gm_server_sync(s);
}

static void on_line(struct linesrv *srv, struct linesrv_client *c, int argc, char **argv);

int gm_server_init(struct gm_server *s, struct gm *m, int64_t (*now)(void),
		   void (*log)(const char *fmt, ...))
{
	struct linesrv_config cfg = {
		.max_clients = GM_MAX_CLIENTS,
		.line_max = GM_LINE_MAX,
		.outq_size = GM_OUTQ,
		.on_connect = on_connect,
		.on_line = on_line,
		.on_close = on_close,
		.ctx = s,
		.log = log ? log : chef_log,
	};

	memset(s, 0, sizeof(*s));
	s->m = m;
	s->now = now;
	s->srv = linesrv_new(&cfg);
	return s->srv ? 0 : -ENOMEM;
}

void gm_server_fini(struct gm_server *s)
{
	if (!s->srv)
		return;
	s->closing = true;
	linesrv_close_all(s->srv);
	s->srv = NULL;
	s->closing = false;
}

/* `"up_ms":N,"fix_ms":N` of the current bring-up, null when none. */
static void gm_acq_members(const struct gm_server *s, char *buf, size_t len)
{
	const struct gm *m = s->m;
	int64_t now = s->now();

	if (m->start_at && m->first_fix_at)
		snprintf(buf, len, "\"up_ms\":%lld,\"fix_ms\":%lld", (long long)(now - m->start_at),
			 (long long)(m->first_fix_at - m->start_at));
	else if (m->start_at)
		snprintf(buf, len, "\"up_ms\":%lld,\"fix_ms\":null", (long long)(now - m->start_at));
	else
		snprintf(buf, len, "\"up_ms\":null,\"fix_ms\":null");
}

static void gm_status_line(const struct gm_server *s, char *buf, size_t len)
{
	const struct gm *m = s->m;
	char detail[GM_DETAIL_MAX * 6], acq[64], cid[16];
	int i, watchers = 0, clients = 0;

	for (i = 0; i < GM_MAX_CLIENTS; i++) {
		if (!linesrv_client_at(s->srv, i))
			continue;
		clients++;
		watchers += s->clients[i].watch;
	}
	json_escape(detail, sizeof(detail), m->detail);
	gm_acq_members(s, acq, sizeof(acq));
	if (m->cid > 0)
		snprintf(cid, sizeof(cid), "%d", m->cid);
	else
		snprintf(cid, sizeof(cid), "null");
	snprintf(buf, len,
		 "{\"type\":\"status\",\"state\":\"%s\",\"enabled\":%s,\"leases\":{\"map\":%d,\"ride\":%d},"
		 "\"clients\":%d,\"watchers\":%d,\"cid\":%s,\"phase\":\"%s\",\"nmea\":%lu,\"bringups\":%lu,"
		 "\"failures\":%lu,\"streak\":%d,\"leaked\":%lu,\"grace\":%s,%s,\"detail\":\"%s\"}\n",
		 gm_state_names[m->state], m->enabled ? "true" : "false", gm_server_count(s, LK_MAP),
		 gm_server_count(s, LK_RIDE), clients, watchers, cid, gm_phase_names[m->phase], m->nmea_lines,
		 m->bringups, m->failures, m->streak, m->leaked, m->grace_at ? "true" : "false", acq, detail);
}

static void gm_state_line(const struct gm_server *s, char *buf, size_t len)
{
	char detail[GM_DETAIL_MAX * 6], acq[64];

	json_escape(detail, sizeof(detail), s->m->detail);
	gm_acq_members(s, acq, sizeof(acq));
	snprintf(buf, len, "{\"type\":\"state\",\"state\":\"%s\",\"detail\":\"%s\",%s}\n",
		 gm_state_names[s->m->state], detail, acq);
}

static void gm_switch_line(const struct gm_server *s, char *buf, size_t len)
{
	snprintf(buf, len, "{\"type\":\"switch\",\"enabled\":%s}\n", s->m->enabled ? "true" : "false");
}

/* LINE to every watcher. A watcher that cannot take it is dropped; the
 * lease count it changes is synced after the loop. */
static void gm_server_to_watchers(struct gm_server *s, const char *line)
{
	size_t len = strlen(line);
	int i;

	if (!s->srv)
		return;	/* shutting down: nobody is listening any more */
	s->broadcasting++;
	for (i = 0; i < GM_MAX_CLIENTS; i++) {
		struct linesrv_client *c = linesrv_client_at(s->srv, i);

		if (c && s->clients[i].watch)
			linesrv_send(c, line, len);
	}
	if (--s->broadcasting == 0 && s->sync_pending) {
		s->sync_pending = false;
		gm_server_sync(s);
	}
}

void gm_server_broadcast(struct gm_server *s)
{
	char buf[GM_DETAIL_MAX * 6 + 160];

	gm_state_line(s, buf, sizeof(buf));
	gm_server_to_watchers(s, buf);
}

/* The switch line to every watcher. */
static void gm_server_broadcast_enabled(struct gm_server *s)
{
	char buf[48];

	gm_switch_line(s, buf, sizeof(buf));
	gm_server_to_watchers(s, buf);
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

/* One request, already split into words by the library. */
static void on_line(struct linesrv *srv, struct linesrv_client *c, int argc, char **argv)
{
	struct gm_server *s = linesrv_ctx(srv);
	struct gm_client *cl = gm_client_of(s, c);
	const char *cmd = argv[0];
	char esc[GM_ECHO_MAX * 6 + 1];

	if (strcmp(cmd, "lease") == 0 || strcmp(cmd, "release") == 0) {
		int k;

		if (argc != 2) {
			linesrv_sendf(c, "{\"type\":\"error\",\"cmd\":\"%s\",\"reason\":\"bad arguments\"}\n", cmd);
			return;
		}
		k = gm_kind_from_name(argv[1]);
		if (k < 0) {
			json_escape_n(esc, sizeof(esc), argv[1], GM_ECHO_MAX);
			linesrv_sendf(c, "{\"type\":\"error\",\"cmd\":\"%s\",\"reason\":\"unknown lease kind\",\"kind\":\"%s\"}\n",
				      cmd, esc);
			return;
		}
		if (cmd[0] == 'l' && s->m->loaded && !s->m->enabled) {
			linesrv_sendf(c, "{\"type\":\"error\",\"cmd\":\"lease\",\"reason\":\"disabled\"}\n");
			return;
		}
		if (cmd[0] == 'l') {
			cl->leases[k]++;
		} else if (cl->leases[k] > 0) {
			cl->leases[k]--;
		} else {
			linesrv_sendf(c, "{\"type\":\"error\",\"cmd\":\"release\",\"reason\":\"no lease\",\"kind\":\"%s\"}\n",
				      gm_kind_names[k]);
			return;
		}
		/* a client dropped by its own reply has its leases synced by on_close */
		if (linesrv_sendf(c, "{\"type\":\"ok\",\"cmd\":\"%s\",\"kind\":\"%s\",\"held\":%d}\n", cmd,
				  gm_kind_names[k], cl->leases[k]) == 0)
			gm_server_sync(s);
		return;
	}
	if (strcmp(cmd, "watch") && strcmp(cmd, "enable") && strcmp(cmd, "disable") &&
	    strcmp(cmd, "status") && strcmp(cmd, "retry")) {
		json_escape_n(esc, sizeof(esc), cmd, GM_ECHO_MAX);
		linesrv_sendf(c, "{\"type\":\"error\",\"cmd\":\"%s\",\"reason\":\"unknown command\"}\n", esc);
		return;
	}
	if (argc != 1) {
		linesrv_sendf(c, "{\"type\":\"error\",\"cmd\":\"%s\",\"reason\":\"bad arguments\"}\n", cmd);
		return;
	}
	if (strcmp(cmd, "watch") == 0) {
		char buf[GM_DETAIL_MAX * 6 + 160];

		cl->watch = true;
		if (linesrv_sendf(c, "{\"type\":\"ok\",\"cmd\":\"watch\"}\n"))
			return;
		gm_switch_line(s, buf, sizeof(buf));
		if (linesrv_send(c, buf, strlen(buf)))
			return;
		gm_state_line(s, buf, sizeof(buf));
		linesrv_send(c, buf, strlen(buf));
	} else if (strcmp(cmd, "enable") == 0 || strcmp(cmd, "disable") == 0) {
		bool on = cmd[0] == 'e';

		if (linesrv_sendf(c, "{\"type\":\"ok\",\"cmd\":\"%s\",\"enabled\":%s}\n", cmd,
				  on ? "true" : "false") == 0)
			gm_server_set_enabled(s, on, true);
	} else if (strcmp(cmd, "status") == 0) {
		char buf[GM_DETAIL_MAX * 6 + 512];

		gm_status_line(s, buf, sizeof(buf));
		linesrv_send(c, buf, strlen(buf));
	} else if (gm_retry(s->m, s->now()) == 0) {
		linesrv_sendf(c, "{\"type\":\"ok\",\"cmd\":\"retry\"}\n");
	} else {
		linesrv_sendf(c, "{\"type\":\"error\",\"cmd\":\"retry\",\"reason\":\"not failed\"}\n");
	}
}
