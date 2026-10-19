/*
 * blesensd's clients and requests, and the event lines it sends them.
 * The daemon as a whole is described in main.c.
 *
 * Line protocol on /run/blesensd.sock (one request per line; every line
 * from the daemon is one JSON object):
 *
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
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "blesensd.h"

/* ------------------------------------------------------------ clients */



static void client_drop(struct client *c)
{
	bool scanned = c->scan, holding = c->hold;

	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	c->watch = c->scan = c->hold = false;
	c->len = 0;
	if (scanned)
		bd_scan_sync();
	if (holding)
		bd_links_sync();
}

static void client_send(struct client *c, const char *line)
{
	size_t len = strlen(line), off = 0;

	while (c->fd >= 0 && off < len) {
		ssize_t n = send(c->fd, line + off, len - off, MSG_NOSIGNAL | MSG_DONTWAIT);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			client_drop(c);	/* a client that does not read is dropped */
			return;
		}
		off += (size_t)n;
	}
}

void bd_broadcast(const char *line)
{
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (D.clients[i].fd >= 0 && D.clients[i].watch)
			client_send(&D.clients[i], line);
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

	bd_json_str(name, sizeof(name), s->name);
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

	bd_json_str(name, sizeof(name), d->name);
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

	bd_json_str(r, sizeof(r), reason ? reason : "");
	snprintf(b, sizeof(b), "{\"type\":\"pair\",\"addr\":\"%s\",\"state\":\"%s\",\"reason\":\"%s\"}\n",
		 D.pr.addr, state, r);
	bd_broadcast(b);
}


/* ------------------------------------------------------------ requests */

static void reply_ok(struct client *c, const char *cmd)
{
	char b[80];

	snprintf(b, sizeof(b), "{\"type\":\"ok\",\"cmd\":\"%s\"}\n", cmd);
	client_send(c, b);
}

static void reply_err(struct client *c, const char *cmd, const char *reason)
{
	char b[200], cm[40], r[100];

	bd_json_str(cm, sizeof(cm), cmd);
	bd_json_str(r, sizeof(r), reason);
	snprintf(b, sizeof(b), "{\"type\":\"error\",\"cmd\":\"%s\",\"reason\":\"%s\"}\n", cm, r);
	client_send(c, b);
}

static void handle_line(struct client *c, char *line)
{
	char *save = NULL, *cmd = strtok_r(line, " \t\r", &save), *arg = strtok_r(NULL, " \t\r", &save);
	char b[OUT_MAX];
	const char *err;

	if (!cmd)
		return;
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
		client_send(c, b);
	} else if (!strcmp(cmd, "list") || !strcmp(cmd, "watch")) {
		bool watch = cmd[0] == 'w';

		if (watch) {
			reply_ok(c, cmd);
			adapter_line(b, sizeof(b));
			client_send(c, b);
		}
		for (int i = 0; i < MAX_SENSORS && c->fd >= 0; i++) {
			struct sensor *s = &D.sensors[i];

			if (!s->used)
				continue;
			sensor_line(s, b, sizeof(b));
			client_send(c, b);
			for (int k = 0; watch && k < BS_KINDS && c->fd >= 0; k++)
				if ((s->kinds & (1u << k)) && value_fresh(s, k)) {
					value_line(s, k, b, sizeof(b));
					client_send(c, b);
				}
		}
		for (int i = 0; watch && i < MAX_DEVS && c->fd >= 0; i++)
			if (D.devs[i].used && D.devs[i].found_sent) {
				found_line(&D.devs[i], b, sizeof(b));
				client_send(c, b);
			}
		if (watch && c->fd >= 0 && D.pr.step != PS_IDLE) {
			static const char *const names[] = { "", "connecting", "resolving", "resolving",
							     "bonding", "saving" };
			char r[1] = "";

			snprintf(b, sizeof(b), "{\"type\":\"pair\",\"addr\":\"%s\",\"state\":\"%s\",\"reason\":\"%s\"}\n",
				 D.pr.addr, names[D.pr.step], r);
			client_send(c, b);
		}
		if (c->fd >= 0 && !watch)
			reply_ok(c, cmd);
		if (c->fd >= 0 && watch)
			c->watch = true;
	} else if (!strcmp(cmd, "scan") && arg && (!strcmp(arg, "on") || !strcmp(arg, "off"))) {
		c->scan = !strcmp(arg, "on");
		reply_ok(c, "scan");
		bd_scan_sync();
	} else if (!strcmp(cmd, "hold") && arg && (!strcmp(arg, "on") || !strcmp(arg, "off"))) {
		c->hold = !strcmp(arg, "on");
		reply_ok(c, "hold");
		bd_links_sync();
	} else if (!strcmp(cmd, "pair") && bd_addr_ok(arg)) {
		/* the reply before the first event, so a client can tell */
		err = bd_pair_refusal(arg);
		if (err) {
			reply_err(c, "pair", err);
		} else {
			reply_ok(c, "pair");
			bd_pair_start(arg);
		}
	} else if (!strcmp(cmd, "cancel") && bd_addr_ok(arg)) {
		if (D.pr.step != PS_IDLE && !strcmp(D.pr.addr, arg)) {
			reply_ok(c, "cancel");
			bd_pair_fail("cancelled");
		} else {
			reply_err(c, "cancel", "not pairing");
		}
	} else if (!strcmp(cmd, "forget") && bd_addr_ok(arg)) {
		if (!D.loaded) {
			reply_err(c, "forget", "loading");
		} else if (!bd_sensor_by_addr(arg)) {
			reply_err(c, "forget", "not paired");
		} else {
			reply_ok(c, "forget");
			bd_forget(arg);
		}
	} else {
		reply_err(c, cmd, "unknown request");
	}
}

void bd_client_input(struct client *c)
{
	ssize_t n = recv(c->fd, c->in + c->len, sizeof(c->in) - 1 - c->len, MSG_DONTWAIT);
	char *nl;

	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
		client_drop(c);
		return;
	}
	if (n < 0)
		return;
	c->len += (size_t)n;
	c->in[c->len] = 0;
	while (c->fd >= 0 && (nl = memchr(c->in, '\n', c->len))) {
		size_t used = (size_t)(nl - c->in) + 1;

		*nl = 0;
		if (c->discarding)
			c->discarding = false;
		else
			handle_line(c, c->in);
		if (c->fd < 0)
			return;
		memmove(c->in, c->in + used, c->len - used);
		c->len -= used;
		c->in[c->len] = 0;
	}
	if (c->len == sizeof(c->in) - 1) {
		c->len = 0;	/* an overlong line: dropped up to its newline */
		c->discarding = true;
	}
}
