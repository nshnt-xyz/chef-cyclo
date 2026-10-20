/*
 * blesensd's clients and requests, and the event lines it sends them.
 * The daemon as a whole is described in main.c.
 *
 * Socket protocol on /run/blesensd.sock: the shared local socket protocol
 * (docs/features/local-sockets.md, tools/common/linesrv.c), proto 1,
 * written down in docs/features/bluetooth.md ("Socket contract"). One
 * request per line, plain words; every line from the daemon is one JSON
 * object with a "type" member:
 *
 *   (connect)         {"type":"hello","daemon":"blesensd","proto":1}
 *   status            {"type":"status","bluez":true,"adapter":true,"powered":true,...}
 *   list              {"type":"sensor",...} per sensor, then {"type":"ok","cmd":"list"}
 *   watch             {"type":"ok","cmd":"watch"}, the adapter, every sensor, its
 *                     fresh values and every found device, then events
 *   scan on|off       LE discovery for sensor services while any client
 *                     wants it (per connection; it ends with the connection)
 *   hold on|off       keep the sensors connected: reconnect for as long as
 *                     any client holds (per connection, like scan)
 *   pair ADDR         a found device: connect, resolve, bond if it needs it,
 *                     trust, save; "pair" events follow
 *   cancel ADDR       stop that pairing
 *   forget ADDR       remove the bond and the record: "ok" once accepted,
 *                     "forgotten" once BlueZ confirmed the removal
 *
 *   events  {"type":"adapter","present":true,"powered":true,"scanning":false}
 *           {"type":"sensor","addr":"..","kinds":"hr,power","name":"..",
 *            "state":"off|waiting|connecting|connected|forgetting","battery":80,
 *            "rssi":-58,"rssi_live":false,"paired":"2026-10-13"}
 *                     (battery/rssi null when unknown; rssi is the last
 *                     advertisement seen, live only while scanning)
 *           {"type":"value","addr":"..","kind":"hr","value":142}
 *           {"type":"value","addr":"..","kind":"power","value":245,"cadence":88}
 *           {"type":"value","addr":"..","kind":"csc","speed":25.3,"cadence":90}
 *                     (null: no value, e.g. no notification for 5 s)
 *           {"type":"found","addr":"..","kinds":"hr","name":"..","rssi":-60}
 *           {"type":"lost","addr":".."}
 *           {"type":"pair","addr":"..","state":"connecting|resolving|bonding|
 *            saving|done|failed","reason":".."}
 *           {"type":"forgotten","addr":".."}
 *   replies {"type":"ok","cmd":"..."} or {"type":"error","cmd":"...","reason":"..."}
 *   errors  "unknown command" (cmd: the word as sent, its first 32 bytes,
 *           escaped), "bad arguments" (a missing, extra or invalid one),
 *           the refusals of pair, cancel and forget; the library's
 *           "line too long" and "too many clients" carry no cmd
 *
 * Limits: 16 clients, request lines of 255 bytes before the newline, a
 * 16384-byte output queue per client. Every line must be delivered (a lost
 * null value or sensor state would leave a client showing a stale one): a
 * client whose queue overflows is not reading and is dropped, its scan and
 * hold with it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "blesensd.h"
#include "common/json.h"

/* ------------------------------------------------------------ clients */

static struct client *client_of(struct linesrv_client *lc)
{
	return &D.clients[linesrv_slot(lc)];
}

static void on_connect(struct linesrv *srv, struct linesrv_client *lc)
{
	struct client *c = client_of(lc);

	(void)srv;
	memset(c, 0, sizeof(*c));
	c->lc = lc;
	linesrv_sendf(lc, "{\"type\":\"hello\",\"daemon\":\"blesensd\",\"proto\":%d}\n", PROTO_VERSION);
}

/* Exactly once per connection, whatever ended it: its scan and hold go.
 * Inside a broadcast they are synced once the loop is over, so no watcher
 * gets a newer line ahead of the one being sent; at exit not at all. */
static void on_close(struct linesrv *srv, struct linesrv_client *lc)
{
	struct client *c = client_of(lc);
	bool scanned = c->scan, holding = c->hold;

	(void)srv;
	memset(c, 0, sizeof(*c));
	if (D.closing)
		return;
	if (D.broadcasting) {
		D.sync_scan |= scanned;
		D.sync_links |= holding;
		return;
	}
	if (scanned)
		bd_scan_sync();
	if (holding)
		bd_links_sync();
}

static void on_line(struct linesrv *srv, struct linesrv_client *lc, int argc, char **argv);

/* Listen on PATH, owner only. 0 or -errno. */
int bd_server_start(const char *path)
{
	struct linesrv_config cfg = {
		.max_clients = MAX_CLIENTS,
		.line_max = LINE_MAX_IN,
		.outq_size = OUTQ_MAX,
		.on_connect = on_connect,
		.on_line = on_line,
		.on_close = on_close,
	};
	int rc;

	D.srv = linesrv_new(&cfg);
	if (!D.srv)
		return -ENOMEM;
	rc = linesrv_listen(D.srv, path);
	if (!rc && chmod(path, 0600) < 0)
		rc = -errno;
	if (rc) {
		bd_server_fini();
		return rc;
	}
	return 0;
}

/* Every client closed (nothing synced), the listener closed and its path
 * unlinked. */
void bd_server_fini(void)
{
	if (!D.srv)
		return;
	D.closing = true;
	linesrv_close_all(D.srv);
	D.srv = NULL;
	D.closing = false;
}

static int client_send(struct linesrv_client *lc, const char *line)
{
	return linesrv_send(lc, line, strlen(line));
}

/* LINE to every watcher. One that cannot take it is dropped; the scan and
 * hold it changes are synced after the loop. */
void bd_broadcast(const char *line)
{
	size_t len = strlen(line);

	if (!D.srv)
		return;	/* exiting: nobody is listening any more */
	D.broadcasting++;
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (D.clients[i].lc && D.clients[i].watch)
			linesrv_send(D.clients[i].lc, line, len);
	if (--D.broadcasting)
		return;
	if (D.sync_scan) {
		D.sync_scan = false;
		bd_scan_sync();
	}
	if (D.sync_links) {
		D.sync_links = false;
		bd_links_sync();
	}
}

static void adapter_line(char *b, size_t n)
{
	snprintf(b, n, "{\"type\":\"adapter\",\"present\":%s,\"powered\":%s,\"scanning\":%s}\n",
		 D.adapter[0] ? "true" : "false", D.powered ? "true" : "false", D.scan_on ? "true" : "false");
}

void bd_adapter_event(void)
{
	char b[OUT_MAX];
	static char shown[OUT_MAX];

	adapter_line(b, sizeof(b));
	if (strcmp(b, shown)) {
		snprintf(shown, sizeof(shown), "%s", b);
		bd_broadcast(b);
	}
}

static void sensor_line(const struct sensor *s, char *b, size_t n)
{
	const struct dev *d = bd_dev_by_addr(s->addr);
	char name[160], kinds[32], bat[16], rssi[16];
	int battery = bd_sensor_battery(s);

	json_escape(name, sizeof(name), s->name);
	bd_kinds_str(s->kinds, kinds, sizeof(kinds));
	if (battery >= 0)
		snprintf(bat, sizeof(bat), "%d", battery);
	else
		snprintf(bat, sizeof(bat), "null");
	if (s->has_rssi)
		snprintf(rssi, sizeof(rssi), "%d", s->rssi);
	else
		snprintf(rssi, sizeof(rssi), "null");
	snprintf(b, n, "{\"type\":\"sensor\",\"addr\":\"%s\",\"kinds\":\"%s\",\"name\":\"%s\","
		 "\"state\":\"%s\",\"battery\":%s,\"rssi\":%s,\"rssi_live\":%s,\"paired\":\"%s\"}\n",
		 s->addr, kinds, name, bd_sensor_state(s), bat, rssi,
		 d && d->has_rssi && D.scan_on ? "true" : "false", s->date);
}

void bd_sensor_event(struct sensor *s, bool force)
{
	char b[OUT_MAX];

	sensor_line(s, b, sizeof(b));
	if (!force && !strcmp(b, s->shown))
		return;
	snprintf(s->shown, sizeof(s->shown), "%s", b);
	bd_broadcast(b);
}

static void found_line(const struct dev *d, char *b, size_t n)
{
	char name[160], kinds[32], rssi[16];

	json_escape(name, sizeof(name), d->name);
	bd_kinds_str(d->kinds, kinds, sizeof(kinds));
	if (d->has_rssi)
		snprintf(rssi, sizeof(rssi), "%d", d->rssi);
	else
		snprintf(rssi, sizeof(rssi), "null");
	snprintf(b, n, "{\"type\":\"found\",\"addr\":\"%s\",\"kinds\":\"%s\",\"name\":\"%s\",\"rssi\":%s}\n",
		 d->addr, kinds, name, rssi);
}

/* A device worth offering: sensor services, not ours, seen in our scan. */
static bool dev_offerable(const struct dev *d)
{
	return D.scan_on && d->addr[0] && d->kinds && !bd_sensor_by_addr(d->addr) && d->has_rssi;
}

/* Announce found/lost (and RSSI or name updates) to watchers. */
void bd_found_update(struct dev *d, bool changed)
{
	char b[OUT_MAX];

	if (dev_offerable(d)) {
		if (d->found_sent && !changed)
			return;
		found_line(d, b, sizeof(b));
		bd_broadcast(b);
		d->found_sent = true;
	} else if (d->found_sent) {
		snprintf(b, sizeof(b), "{\"type\":\"lost\",\"addr\":\"%s\"}\n", d->addr);
		bd_broadcast(b);
		d->found_sent = false;
	}
}

static bool value_fresh(const struct sensor *s, int k)
{
	return s->last_ms[k] && bd_now_ms() - s->last_ms[k] < STALE_MS;
}

static void value_line(const struct sensor *s, int k, char *b, size_t n)
{
	char v1[16], v2[16];
	bool fresh = value_fresh(s, k);

	snprintf(v2, sizeof(v2), "null");
	if (fresh && k != BS_HR && s->cadence[k] >= 0)
		snprintf(v2, sizeof(v2), "%.0f", s->cadence[k]);
	if (k == BS_HR) {
		if (fresh && s->hr >= 0)
			snprintf(v1, sizeof(v1), "%d", s->hr);
		else
			snprintf(v1, sizeof(v1), "null");
		snprintf(b, n, "{\"type\":\"value\",\"addr\":\"%s\",\"kind\":\"hr\",\"value\":%s}\n", s->addr, v1);
	} else if (k == BS_POWER) {
		if (fresh && s->has_power)
			snprintf(v1, sizeof(v1), "%d", s->power);
		else
			snprintf(v1, sizeof(v1), "null");
		snprintf(b, n, "{\"type\":\"value\",\"addr\":\"%s\",\"kind\":\"power\",\"value\":%s,"
			 "\"cadence\":%s}\n", s->addr, v1, v2);
	} else {
		if (fresh && s->speed >= 0)
			snprintf(v1, sizeof(v1), "%.1f", s->speed);
		else
			snprintf(v1, sizeof(v1), "null");
		snprintf(b, n, "{\"type\":\"value\",\"addr\":\"%s\",\"kind\":\"csc\",\"speed\":%s,"
			 "\"cadence\":%s}\n", s->addr, v1, v2);
	}
}

void bd_value_event(struct sensor *s, int k)
{
	char b[OUT_MAX];

	value_line(s, k, b, sizeof(b));
	bd_broadcast(b);
}

void bd_pair_event(const char *state, const char *reason)
{
	char b[OUT_MAX], r[100];

	json_escape(r, sizeof(r), reason ? reason : "");
	snprintf(b, sizeof(b), "{\"type\":\"pair\",\"addr\":\"%s\",\"state\":\"%s\",\"reason\":\"%s\"}\n",
		 D.pr.addr, state, r);
	bd_broadcast(b);
}


/* ------------------------------------------------------------ requests */

static void reply_ok(struct linesrv_client *lc, const char *cmd)
{
	linesrv_sendf(lc, "{\"type\":\"ok\",\"cmd\":\"%s\"}\n", cmd);
}

static void reply_err(struct linesrv_client *lc, const char *cmd, const char *reason)
{
	char r[100];

	json_escape(r, sizeof(r), reason);
	linesrv_sendf(lc, "{\"type\":\"error\",\"cmd\":\"%s\",\"reason\":\"%s\"}\n", cmd, r);
}

static bool on_off(const char *arg)
{
	return !strcmp(arg, "on") || !strcmp(arg, "off");
}

/* A reply that drops the client clears its slot (on_close): a send to a
 * dropped client does nothing, and nothing is written to the slot after a
 * reply unless c->lc says the client is still there. */
static void on_line(struct linesrv *srv, struct linesrv_client *lc, int argc, char **argv)
{
	struct client *c = client_of(lc);
	const char *cmd = argv[0], *arg = argc > 1 ? argv[1] : NULL;
	char b[OUT_MAX];
	const char *err;
	bool args_ok;

	(void)srv;
	if (!strcmp(cmd, "status") || !strcmp(cmd, "list") || !strcmp(cmd, "watch")) {
		args_ok = argc == 1;
	} else if (!strcmp(cmd, "scan") || !strcmp(cmd, "hold")) {
		args_ok = argc == 2 && on_off(arg);
	} else if (!strcmp(cmd, "pair") || !strcmp(cmd, "cancel") || !strcmp(cmd, "forget")) {
		args_ok = argc == 2 && bd_addr_ok(arg);
	} else {
		char esc[ECHO_MAX * 6 + 1];

		json_escape_n(esc, sizeof(esc), cmd, ECHO_MAX);
		reply_err(lc, esc, "unknown command");
		return;
	}
	if (!args_ok) {
		reply_err(lc, cmd, "bad arguments");
		return;
	}
	if (!strcmp(cmd, "status")) {
		int n = 0, conn = 0;

		for (int i = 0; i < MAX_SENSORS; i++)
			if (D.sensors[i].used && !D.sensors[i].forgetting) {
				n++;
				conn += bd_sensor_connected(&D.sensors[i]);
			}
		snprintf(b, sizeof(b), "{\"type\":\"status\",\"bluez\":%s,\"adapter\":%s,\"powered\":%s,"
			 "\"scanning\":%s,\"loaded\":%s,\"sensors\":%d,\"connected\":%d,\"pairing\":%s,"
			 "\"agent\":%s,\"wheel_mm\":%d,\"held\":%s,\"reconnecting\":%s}\n",
			 D.bluez ? "true" : "false", D.adapter[0] ? "true" : "false", D.powered ? "true" : "false",
			 D.scan_on ? "true" : "false", D.loaded ? "true" : "false", n, conn,
			 D.pr.step != PS_IDLE ? "true" : "false", D.agent_registered ? "true" : "false", D.wheel_mm,
			 bd_held() ? "true" : "false", bd_links_wanted() ? "true" : "false");
		client_send(lc, b);
	} else if (!strcmp(cmd, "list") || !strcmp(cmd, "watch")) {
		bool watch = cmd[0] == 'w';

		if (watch) {
			reply_ok(lc, cmd);
			adapter_line(b, sizeof(b));
			client_send(lc, b);
		}
		for (int i = 0; i < MAX_SENSORS && c->lc; i++) {
			struct sensor *s = &D.sensors[i];

			if (!s->used)
				continue;
			sensor_line(s, b, sizeof(b));
			client_send(lc, b);
			for (int k = 0; watch && k < BS_KINDS && c->lc; k++)
				if ((s->kinds & (1u << k)) && value_fresh(s, k)) {
					value_line(s, k, b, sizeof(b));
					client_send(lc, b);
				}
		}
		for (int i = 0; watch && i < MAX_DEVS && c->lc; i++)
			if (D.devs[i].used && D.devs[i].found_sent) {
				found_line(&D.devs[i], b, sizeof(b));
				client_send(lc, b);
			}
		if (watch && c->lc && D.pr.step != PS_IDLE) {
			static const char *const names[] = { "", "connecting", "resolving", "resolving",
							     "bonding", "saving" };
			char r[1] = "";

			snprintf(b, sizeof(b), "{\"type\":\"pair\",\"addr\":\"%s\",\"state\":\"%s\",\"reason\":\"%s\"}\n",
				 D.pr.addr, names[D.pr.step], r);
			client_send(lc, b);
		}
		if (c->lc && !watch)
			reply_ok(lc, cmd);
		if (c->lc && watch)
			c->watch = true;
	} else if (!strcmp(cmd, "scan")) {
		c->scan = !strcmp(arg, "on");
		reply_ok(lc, "scan");
		bd_scan_sync();
	} else if (!strcmp(cmd, "hold")) {
		c->hold = !strcmp(arg, "on");
		reply_ok(lc, "hold");
		bd_links_sync();
	} else if (!strcmp(cmd, "pair")) {
		/* the reply before the first event, so a client can tell */
		err = bd_pair_refusal(arg);
		if (err) {
			reply_err(lc, "pair", err);
		} else {
			reply_ok(lc, "pair");
			bd_pair_start(arg);
		}
	} else if (!strcmp(cmd, "cancel")) {
		if (D.pr.step != PS_IDLE && !strcmp(D.pr.addr, arg)) {
			reply_ok(lc, "cancel");
			bd_pair_fail("cancelled");
		} else {
			reply_err(lc, "cancel", "not pairing");
		}
	} else {
		if (!D.loaded) {
			reply_err(lc, "forget", "loading");
		} else if (!bd_sensor_by_addr(arg)) {
			reply_err(lc, "forget", "not paired");
		} else {
			reply_ok(lc, "forget");
			bd_forget(arg);
		}
	}
}
