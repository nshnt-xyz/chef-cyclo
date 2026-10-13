/*
 * blesensd - the one owner of the phone's BLE sensors (heart rate,
 * cycling power, speed and cadence): it pairs them, keeps them connected
 * while Bluetooth is on, holds their notifications for its whole life and
 * serves the values to any number of clients (the settings app now, the
 * ride recorder later). BlueZ ends a client's notification sessions when
 * that client leaves the bus, so one long-lived owner is needed. See
 * docs/features/bluetooth.md ("BLE sensors").
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
 *
 * A device may carry several sensor kinds (a power meter with cadence, or
 * the PC emulator with all three): one record, one connection, one value
 * stream per kind. Records (address, kinds, paired date, a pending-forget
 * flag, name) are saved through `chef-state blesens-save` and read with
 * `chef-state blesens-load` at start, both as child processes in their own
 * process group (storage never blocks this loop), bounded, the whole group
 * killed at the bound, when the leader exits and at exit.
 *
 * Connections: BlueZ reconnects LE devices on its own only for profiles
 * that ask for it (HID, audio), not for these services, so while the
 * adapter is powered a saved sensor that is not connected gets
 * Device1.Connect again (an LE create-connection that waits for the
 * sensor's advertisement), with backoff 5 s doubling to 30 s. Sensors are
 * off most of the time, and every attempt keeps the radio scanning, so
 * this runs only while a client holds the sensors (`hold on`: the settings
 * pages while seen, a ride) or for a while (-r, 5 min) after this daemon
 * starts, the adapter comes on, bluetoothd appears, a sensor is paired or
 * a link drops; otherwise no attempt is made ("waiting") until the next
 * hold. A connection a sensor makes is kept either way. Once its
 * services are resolved, StartNotify on its measurement characteristics,
 * once per connection (BlueZ answers success for a session it already
 * has); the battery level comes from BlueZ's Battery1 where it exists,
 * else one read per connection and an owned battery notification session
 * where offered. Only notifications on a session this
 * connection started count as values; cached Value properties do not.
 * Pairing uses this daemon's NoInputNoOutput agent, default only during
 * the attempt (requests are answered only for the device being paired) and bonds (Device1.Pair) only when
 * Connect or StartNotify is refused for authentication.
 *
 * Every asynchronous call carries its own identity (the pairing attempt,
 * or the sensor and its connection epoch); calls that no longer apply are
 * cancelled, and a late reply that slips through is checked against the
 * identity and dropped.
 *
 * Everything runs on one poll loop: the bus (sd-bus, static basu on the
 * phone), the socket and its clients, the chef-state children and a 1 s
 * housekeeping tick. Nothing here logs a value; kmsg gets transitions.
 *
 * Usage: blesensd [-S SOCK] [-s STATE_CMD] [-w WHEEL_MM] [-r RETRY_S] [-T] [-v]
 *        blesensd [-S SOCK] status | list | watch | scan | pair ADDR |
 *                 cancel ADDR | forget ADDR                     (client)
 * -T divides every timer by 10 (host tests only).
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(CHEF_BASU)
#include <basu/sd-bus.h>
#else
#include <systemd/sd-bus.h>
#endif

#include "blesens.h"

#define SOCK_PATH "/run/blesensd.sock"
#define BLUEZ "org.bluez"
#define ADAPTER_IF BLUEZ ".Adapter1"
#define DEVICE_IF BLUEZ ".Device1"
#define CHR_IF BLUEZ ".GattCharacteristic1"
#define BATTERY_IF BLUEZ ".Battery1"
#define AGENT_PATH "/chef/blesensd/agent"
#define PROPS "org.freedesktop.DBus.Properties"
#define OBJMGR "org.freedesktop.DBus.ObjectManager"

#define MAX_CLIENTS 16
#define MAX_SENSORS 8
#define MAX_DEVS 128
#define MAX_CHRS 128
#define MAX_PENDS 64
#define LINE_MAX_IN 256
#define OUT_MAX 512

/* timers in ms, divided by g_scale (10 under -T) */
#define TICK_MS (1000 / g_scale)
#define STALE_MS (5000 / g_scale)
#define BACKOFF_MIN_MS (5000 / g_scale)
#define BACKOFF_MAX_MS (30000 / g_scale)
#define CONNECT_TIMEOUT_US (25000000ull / g_scale)
#define PAIR_TIMEOUT_MS (20000 / g_scale)
#define CHILD_LOAD_MS (5000 / g_scale)
#define CHILD_SAVE_MS (10000 / g_scale)
#define EXIT_SAVE_MS (3000 / g_scale)
#define RETRY_MS (5000 / g_scale)
#define BUS_BACKOFF_MAX_MS (10000 / g_scale)

static bool g_verbose;
static int g_scale = 1;
static int g_retry_s = 300;	/* -r: reconnect this long without a hold */
static volatile sig_atomic_t g_stop;
static int g_sigpipe[2] = { -1, -1 };
static pid_t g_self;

/* ------------------------------------------------------------ helpers */

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void notef(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void notef(const char *fmt, ...)
{
	char buf[300];
	va_list ap;
	int fd;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fprintf(stderr, "blesensd: %s\n", buf);
	fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd >= 0) {
		dprintf(fd, "blesensd: %s\n", buf);
		close(fd);
	}
}

#define vlog(...) do { if (g_verbose) notef(__VA_ARGS__); } while (0)

/* "AA:BB:CC:DD:EE:FF" (upper case hex) */
static bool addr_ok(const char *a)
{
	if (!a || strlen(a) != 17)
		return false;
	for (int i = 0; i < 17; i++)
		if (i % 3 == 2 ? a[i] != ':' : !isxdigit((unsigned char)a[i]) || islower((unsigned char)a[i]))
			return false;
	return true;
}

/* JSON string body: quotes, backslashes and controls escaped. */
static void json_str(char *out, size_t n, const char *s)
{
	size_t o = 0;

	for (; s && *s && o + 7 < n; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '"' || c == '\\') {
			out[o++] = '\\';
			out[o++] = (char)c;
		} else if (c < 0x20 || c == 0x7f) {
			o += (size_t)snprintf(out + o, n - o, "\\u%04x", c);
		} else {
			out[o++] = (char)c;
		}
	}
	out[o] = 0;
}

static void kinds_str(unsigned kinds, char *out, size_t n)
{
	size_t o = 0;

	out[0] = 0;
	for (int k = 0; k < BS_KINDS; k++)
		if (kinds & (1u << k))
			o += (size_t)snprintf(out + o, n - o, "%s%s", o ? "," : "", bs_kind_name(k));
}

static unsigned kinds_parse(const char *s)
{
	char buf[64], *save = NULL, *t;
	unsigned k = 0;

	snprintf(buf, sizeof(buf), "%s", s ? s : "");
	for (t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
		int i = bs_kind_from_name(t);

		if (i < 0)
			return 0;
		k |= 1u << i;
	}
	return k;
}

/* Printable ASCII only, at most n-1 bytes, no leading or trailing spaces:
 * names go into records and JSON. */
static void name_clean(char *out, size_t n, const char *s)
{
	size_t o = 0;

	while (s && *s == ' ')
		s++;
	for (; s && *s && o + 1 < n; s++)
		out[o++] = (*s >= 0x20 && *s < 0x7f) ? *s : '?';
	while (o && out[o - 1] == ' ')
		o--;
	out[o] = 0;
}

/* ------------------------------------------------------------ state */

struct dev {
	bool used;
	char path[128], addr[18], name[64];
	bool connected, resolved, paired, trusted;
	bool has_rssi;
	int rssi;
	int battery;		/* Battery1 Percentage, -1 none */
	unsigned kinds;		/* sensor services among its UUIDs */
	bool found_sent;	/* announced as found to watchers */
};

struct chr {
	bool used;
	char path[192], dev[128];
	int uuid16;
	bool can_notify, can_read;
};

enum nstate { NS_NONE, NS_PENDING, NS_OURS };

struct sensor {
	bool used;
	uint32_t sid;		/* identity: never reused */
	char addr[18], name[64], date[16];
	unsigned kinds;
	bool forgetting;	/* removal asked, not yet durably finished */
	bool removal_confirmed;
	uint64_t forget_revision, removal_revision;
	int64_t forget_next_ms;
	bool forget_inflight;
	/* the last advertisement RSSI seen (kept across connections) */
	bool has_rssi;
	int rssi;
	/* per connection; epoch bumps when the connection is lost */
	uint32_t epoch;
	bool was_connected, was_resolved;
	bool link_up;		/* for the log: the device's own connection */
	bool connect_inflight;
	int backoff_ms;
	int64_t next_try_ms;
	enum nstate notify[BS_KINDS];
	bool battery_read;
	enum nstate battery_notify;
	int gatt_battery;	/* from a 0x2A19 read, -1 none */
	int64_t last_ms[BS_KINDS];
	bool has_power;
	int hr, power;
	double cadence[BS_KINDS], speed;
	struct bs_revs crank[BS_KINDS], wheel;
	char shown[OUT_MAX];	/* the last sensor event sent */
};

enum pair_step { PS_IDLE, PS_CONNECT, PS_RESOLVE, PS_NOTIFY, PS_BOND, PS_TRUST };

struct client {
	int fd;
	bool watch, scan, hold;
	char in[LINE_MAX_IN];
	size_t len;
	bool discarding;
};

/* What an asynchronous call was for, and for whom. */
enum pk {
	PK_OBJECTS, PK_AGENT, PK_AGENT_RELEASE, PK_FILTER, PK_SCAN, PK_STOPSCAN, PK_CONNECT, PK_NOTIFY, PK_BATTERY, PK_BATTERY_NOTIFY,
	PK_FORGET, PK_PAIR_AGENT, PK_PAIR_CONNECT, PK_PAIR_NOTIFY, PK_PAIR_BOND, PK_PAIR_TRUST, PK_PLAIN,
};

struct pend {
	bool used;
	sd_bus_slot *slot;
	enum pk kind;
	uint32_t sid, epoch, pair;
	int k;
};

struct child {
	pid_t pid;		/* the leader, 0 once reaped */
	pid_t pgid;		/* its group, kept until the child is finished */
	int64_t until;
};

static struct {
	const char *sock_path, *state_cmd;
	int wheel_mm;
	sd_bus *bus;
	int bus_fd;
	int bus_backoff_ms;
	int64_t bus_retry_ms;
	bool bluez;		/* GetManagedObjects answered */
	char bluez_owner[64];	/* its unique name: agent calls must come from it */
	sd_bus_slot *slots[8];
	int n_slots;
	sd_bus_slot *agent_slot;
	bool agent_registered, agent_inflight;
	bool agent_releasing, agent_release_inflight;
	int64_t agent_retry_ms;
	char adapter[64];
	bool powered, discovering;
	bool scan_on, scan_inflight;	/* our discovery session */
	int64_t scan_retry_ms;
	struct dev devs[MAX_DEVS];
	bool devs_full_logged;
	struct chr chrs[MAX_CHRS];
	struct sensor sensors[MAX_SENSORS];
	uint32_t next_sid;
	struct client clients[MAX_CLIENTS];
	int64_t retry_until;	/* reconnect without a hold until then */
	bool held;		/* a client holds (as last seen) */
	struct pend pends[MAX_PENDS];
	/* records */
	bool loaded;
	struct child load, save;
	int load_fd;
	char load_buf[2048];
	size_t load_len;
	bool save_pending, save_write_ok;
	uint64_t record_revision, save_revision, saved_revision;
	int64_t save_retry_ms;
	/* the one pairing */
	struct {
		enum pair_step step;
		uint32_t id;	/* identity of this attempt */
		char addr[18], path[128], name[64];
		int64_t deadline;
		bool bonded;	/* Pair() succeeded: undone on failure */
		bool connected_by_us, agent_requested;
		unsigned kinds;
	} pr;
} D = { .bus_fd = -1, .load_fd = -1 };

static void broadcast(const char *line);
static void sensor_event(struct sensor *s, bool force);
static void save_records(void);
static void save_start(void);
static void agent_register(int64_t now);
static void agent_release_drive(int64_t now);
static void pair_fail(const char *reason);
static void bus_lost(const char *why);
static void scan_sync(void);

static struct dev *dev_by_path(const char *p)
{
	for (int i = 0; i < MAX_DEVS; i++)
		if (D.devs[i].used && !strcmp(D.devs[i].path, p))
			return &D.devs[i];
	return NULL;
}

static struct dev *dev_by_addr(const char *a)
{
	for (int i = 0; i < MAX_DEVS; i++)
		if (D.devs[i].used && !strcmp(D.devs[i].addr, a))
			return &D.devs[i];
	return NULL;
}

static struct sensor *sensor_by_addr(const char *a)
{
	for (int i = 0; i < MAX_SENSORS; i++)
		if (D.sensors[i].used && !strcmp(D.sensors[i].addr, a))
			return &D.sensors[i];
	return NULL;
}

static struct sensor *sensor_by_sid(uint32_t sid)
{
	for (int i = 0; i < MAX_SENSORS; i++)
		if (D.sensors[i].used && D.sensors[i].sid == sid)
			return &D.sensors[i];
	return NULL;
}

static struct chr *chr_by_path(const char *p)
{
	for (int i = 0; i < MAX_CHRS; i++)
		if (D.chrs[i].used && !strcmp(D.chrs[i].path, p))
			return &D.chrs[i];
	return NULL;
}

/* The characteristic uuid16 of the device at dev path. */
static struct chr *chr_of(const char *dev, int uuid16)
{
	for (int i = 0; i < MAX_CHRS; i++)
		if (D.chrs[i].used && D.chrs[i].uuid16 == uuid16 && !strcmp(D.chrs[i].dev, dev))
			return &D.chrs[i];
	return NULL;
}

/* "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF/service0010/char0011" -> device */
static void dev_of_path(const char *p, char *out, size_t n)
{
	const char *d = strstr(p, "/dev_");
	const char *e = d ? strchr(d + 1, '/') : NULL;
	size_t l = d ? (e ? (size_t)(e - p) : strlen(p)) : 0;

	if (l >= n)
		l = 0;
	memcpy(out, p, l);
	out[l] = 0;
}

static bool is_sensor_chr(int u)
{
	return u == BS_CHR_HR || u == BS_CHR_POWER || u == BS_CHR_CSC || u == BS_CHR_BATTERY;
}

static int sensor_battery(const struct sensor *s)
{
	const struct dev *d = dev_by_addr(s->addr);

	if (d && d->battery >= 0 && d->connected)
		return d->battery;
	return s->gatt_battery;
}

static bool sensor_connected(const struct sensor *s)
{
	const struct dev *d = dev_by_addr(s->addr);

	return D.powered && d && d->connected && d->resolved;
}

static bool held(void)
{
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (D.clients[i].fd >= 0 && D.clients[i].hold)
			return true;
	return false;
}

/* Reconnect now? While held, or within the window after a trigger. */
static bool links_wanted(void)
{
	return held() || now_ms() < D.retry_until;
}

/* A trigger (start, adapter on, bluetoothd, pairing, a link lost): try
 * again now and for -r seconds. */
static void retry_window(void);

static const char *sensor_state(const struct sensor *s)
{
	const struct dev *d = dev_by_addr(s->addr);

	if (s->forgetting)
		return "forgetting";
	if (!D.powered)
		return "off";
	if (sensor_connected(s))
		return "connected";
	return links_wanted() || s->connect_inflight || (d && d->connected) ? "connecting" : "waiting";
}

/* ------------------------------------------------------------ clients */

static void links_sync(void);

static void client_drop(struct client *c)
{
	bool scanned = c->scan, holding = c->hold;

	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	c->watch = c->scan = c->hold = false;
	c->len = 0;
	if (scanned)
		scan_sync();
	if (holding)
		links_sync();
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

static void broadcast(const char *line)
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

static void adapter_event(void)
{
	char b[OUT_MAX];
	static char shown[OUT_MAX];

	adapter_line(b, sizeof(b));
	if (strcmp(b, shown)) {
		snprintf(shown, sizeof(shown), "%s", b);
		broadcast(b);
	}
}

static void sensor_line(const struct sensor *s, char *b, size_t n)
{
	const struct dev *d = dev_by_addr(s->addr);
	char name[160], kinds[32], bat[16], rssi[16];
	int battery = sensor_battery(s);

	json_str(name, sizeof(name), s->name);
	kinds_str(s->kinds, kinds, sizeof(kinds));
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
		 s->addr, kinds, name, sensor_state(s), bat, rssi,
		 d && d->has_rssi && D.scan_on ? "true" : "false", s->date);
}

static void sensor_event(struct sensor *s, bool force)
{
	char b[OUT_MAX];

	sensor_line(s, b, sizeof(b));
	if (!force && !strcmp(b, s->shown))
		return;
	snprintf(s->shown, sizeof(s->shown), "%s", b);
	broadcast(b);
}

static void found_line(const struct dev *d, char *b, size_t n)
{
	char name[160], kinds[32], rssi[16];

	json_str(name, sizeof(name), d->name);
	kinds_str(d->kinds, kinds, sizeof(kinds));
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
	return D.scan_on && d->addr[0] && d->kinds && !sensor_by_addr(d->addr) && d->has_rssi;
}

/* Announce found/lost (and RSSI or name updates) to watchers. */
static void found_update(struct dev *d, bool changed)
{
	char b[OUT_MAX];

	if (dev_offerable(d)) {
		if (d->found_sent && !changed)
			return;
		found_line(d, b, sizeof(b));
		broadcast(b);
		d->found_sent = true;
	} else if (d->found_sent) {
		snprintf(b, sizeof(b), "{\"type\":\"lost\",\"addr\":\"%s\"}\n", d->addr);
		broadcast(b);
		d->found_sent = false;
	}
}

static bool value_fresh(const struct sensor *s, int k)
{
	return s->last_ms[k] && now_ms() - s->last_ms[k] < STALE_MS;
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

static void value_event(struct sensor *s, int k)
{
	char b[OUT_MAX];

	value_line(s, k, b, sizeof(b));
	broadcast(b);
}

static void pair_event(const char *state, const char *reason)
{
	char b[OUT_MAX], r[100];

	json_str(r, sizeof(r), reason ? reason : "");
	snprintf(b, sizeof(b), "{\"type\":\"pair\",\"addr\":\"%s\",\"state\":\"%s\",\"reason\":\"%s\"}\n",
		 D.pr.addr, state, r);
	broadcast(b);
}

/* ------------------------------------------------------------ bus calls */

static int on_reply(sd_bus_message *m, void *u, sd_bus_error *e);

static struct pend *pend_new(enum pk kind)
{
	for (int i = 0; i < MAX_PENDS; i++)
		if (!D.pends[i].used) {
			memset(&D.pends[i], 0, sizeof(D.pends[i]));
			D.pends[i].used = true;
			D.pends[i].kind = kind;
			return &D.pends[i];
		}
	return NULL;
}

static void pend_free(struct pend *p)
{
	p->slot = sd_bus_slot_unref(p->slot);	/* cancels the call if still pending */
	p->used = false;
}

/* Cancel the calls of a sensor (sid; its removal only with forget too),
 * of the pairing (pair) or all. */
static void pend_cancel(bool all, uint32_t sid, uint32_t pair, bool forget)
{
	for (int i = 0; i < MAX_PENDS; i++) {
		struct pend *p = &D.pends[i];

		if (p->used && (all || (sid && p->sid == sid && (forget || p->kind != PK_FORGET)) ||
				(pair && p->pair == pair)))
			pend_free(p);
	}
}

/*
 * An asynchronous call to BlueZ; the reply goes to on_reply with a copy of
 * *tmpl. types and the arguments follow sd_bus_message_append. 0 or -errno.
 */
static int callv(const struct pend *tmpl, const char *path, const char *iface, const char *member,
		 uint64_t usec, const char *types, va_list ap)
{
	sd_bus_message *m = NULL;
	struct pend *p;
	int r;

	if (!D.bus)
		return -ENOTCONN;
	if (!(p = pend_new(tmpl->kind))) {
		notef("too many calls in flight: %s not sent", member);
		return -EBUSY;
	}
	p->sid = tmpl->sid;
	p->epoch = tmpl->epoch;
	p->pair = tmpl->pair;
	p->k = tmpl->k;
	r = sd_bus_message_new_method_call(D.bus, &m, BLUEZ, path, iface, member);
	if (r >= 0)
		r = sd_bus_message_set_auto_start(m, 0);
	if (r >= 0 && types)
		r = sd_bus_message_appendv(m, types, ap);
	if (r >= 0)
		r = sd_bus_call_async(D.bus, &p->slot, m, on_reply, p, usec);
	sd_bus_message_unref(m);
	if (r < 0)
		pend_free(p);
	return r < 0 ? r : 0;
}

static int call(const struct pend *tmpl, const char *path, const char *iface, const char *member,
		uint64_t usec, const char *types, ...)
{
	va_list ap;
	int r;

	va_start(ap, types);
	r = callv(tmpl, path, iface, member, usec, types, ap);
	va_end(ap);
	return r;
}

static struct pend plain(enum pk kind)
{
	return (struct pend){ .kind = kind };
}

static struct pend for_sensor(enum pk kind, const struct sensor *s, int k)
{
	return (struct pend){ .kind = kind, .sid = s->sid, .epoch = s->epoch, .k = k };
}

static struct pend for_pair(enum pk kind)
{
	return (struct pend){ .kind = kind, .pair = D.pr.id };
}

static const char *err_name(sd_bus_message *m)
{
	const sd_bus_error *e = sd_bus_message_get_error(m);

	return e && e->name ? e->name : "error";
}

static bool err_is(sd_bus_message *m, const char *name)
{
	return sd_bus_message_is_method_error(m, name) > 0;
}

/* ------------------------------------------------------------ object cache */

static struct dev *dev_get(const char *path)
{
	struct dev *d = dev_by_path(path), *free_d = NULL;

	if (d)
		return d;
	for (int i = 0; i < MAX_DEVS && !free_d; i++)
		if (!D.devs[i].used)
			free_d = &D.devs[i];
	if (!free_d) {
		if (!D.devs_full_logged)
			notef("more than %d Bluetooth devices: the rest are ignored", MAX_DEVS);
		D.devs_full_logged = true;
		return NULL;
	}
	memset(free_d, 0, sizeof(*free_d));
	free_d->used = true;
	free_d->battery = -1;
	snprintf(free_d->path, sizeof(free_d->path), "%s", path);
	return free_d;
}

/* Read a variant of basic type t; skip it if it is another type.
 * 1 read, 0 skipped, < 0 malformed. */
static int vread(sd_bus_message *m, char t, void *p)
{
	const char *contents = NULL;
	char type, sig[2] = { t, 0 };
	int r = sd_bus_message_peek_type(m, &type, &contents);

	if (r <= 0)
		return r < 0 ? r : -ENXIO;
	if (type != 'v' || !contents || strcmp(contents, sig))
		return sd_bus_message_skip(m, "v") < 0 ? -EBADMSG : 0;
	if ((r = sd_bus_message_enter_container(m, 'v', sig)) <= 0 ||
	    (r = sd_bus_message_read_basic(m, t, p)) <= 0)
		return r < 0 ? r : -ENXIO;
	return sd_bus_message_exit_container(m) < 0 ? -EBADMSG : 1;
}

static int vread_bool(sd_bus_message *m, bool *out)
{
	int v = 0, r = vread(m, 'b', &v);

	if (r > 0)
		*out = v;
	return r;
}

static int vread_str(sd_bus_message *m, char *out, size_t n)
{
	const char *s = NULL;
	int r = vread(m, 's', &s);

	if (r > 0)
		snprintf(out, n, "%s", s ? s : "");
	return r;
}

/* A variant "as": UUIDs (the sensor kinds among them, into *kinds) or
 * characteristic flags (into c). */
static int vread_strs(sd_bus_message *m, unsigned *kinds, struct chr *c)
{
	const char *contents = NULL, *s;
	char type;
	int r = sd_bus_message_peek_type(m, &type, &contents);

	if (r <= 0)
		return r < 0 ? r : -ENXIO;
	if (type != 'v' || !contents || strcmp(contents, "as"))
		return sd_bus_message_skip(m, "v") < 0 ? -EBADMSG : 0;
	if (sd_bus_message_enter_container(m, 'v', "as") <= 0 ||
	    sd_bus_message_enter_container(m, 'a', "s") < 0)
		return -EBADMSG;
	if (kinds)
		*kinds = 0;
	if (c)
		c->can_notify = c->can_read = false;
	while ((r = sd_bus_message_read_basic(m, 's', &s)) > 0) {
		if (kinds) {
			int k = bs_kind_of_service(bs_uuid16(s));

			if (k >= 0)
				*kinds |= 1u << k;
		} else if (c && (!strcmp(s, "notify") || !strcmp(s, "indicate"))) {
			c->can_notify = true;
		} else if (c && !strcmp(s, "read")) {
			c->can_read = true;
		}
	}
	if (r < 0 || sd_bus_message_exit_container(m) < 0 || sd_bus_message_exit_container(m) < 0)
		return -EBADMSG;
	return 1;
}

static void value_bytes(struct chr *c, const uint8_t *b, size_t n);

/* A variant "ay": a characteristic Value. Only a live notification
 * (PropertiesChanged) is handed on; a snapshot's cached value is not. */
static int vread_value(sd_bus_message *m, struct chr *c, bool live)
{
	const char *contents = NULL;
	const void *p = NULL;
	size_t n = 0;
	char type;
	int r = sd_bus_message_peek_type(m, &type, &contents);

	if (r <= 0)
		return r < 0 ? r : -ENXIO;
	if (type != 'v' || !contents || strcmp(contents, "ay"))
		return sd_bus_message_skip(m, "v") < 0 ? -EBADMSG : 0;
	if (sd_bus_message_enter_container(m, 'v', "ay") <= 0 ||
	    sd_bus_message_read_array(m, 'y', &p, &n) < 0 || sd_bus_message_exit_container(m) < 0)
		return -EBADMSG;
	if (c && live)
		value_bytes(c, p, n);
	return 1;
}

/*
 * One interface's a{sv} for the object at path. live: from a
 * PropertiesChanged signal (else a snapshot: GetManagedObjects or
 * InterfacesAdded). Characteristics are kept only when they are one of
 * ours, which a snapshot tells by its UUID; a signal only updates a kept one.
 */
static int read_iface(sd_bus_message *m, const char *path, const char *iface, bool live)
{
	struct dev *d = NULL;
	struct chr *c = NULL, tmp;
	bool is_adapter = !strcmp(iface, ADAPTER_IF), is_chr = !strcmp(iface, CHR_IF);
	int r = sd_bus_message_enter_container(m, 'a', "{sv}");

	if (r <= 0)
		return r < 0 ? r : -ENXIO;
	if (!strcmp(iface, DEVICE_IF) || !strcmp(iface, BATTERY_IF))
		d = live ? dev_by_path(path) : dev_get(path);
	else if (is_chr) {
		c = chr_by_path(path);
		if (!c && !live) {
			memset(&tmp, 0, sizeof(tmp));
			snprintf(tmp.path, sizeof(tmp.path), "%s", path);
			dev_of_path(path, tmp.dev, sizeof(tmp.dev));
			c = &tmp;
		}
	} else if (is_adapter && !D.adapter[0] && !live)
		snprintf(D.adapter, sizeof(D.adapter), "%s", path);
	if (!strcmp(iface, BATTERY_IF) && d && !live)
		d->battery = -1;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *k = NULL;

		if (sd_bus_message_read_basic(m, 's', &k) <= 0)
			return -EBADMSG;
		if (d && !strcmp(iface, BATTERY_IF) && !strcmp(k, "Percentage")) {
			uint8_t v = 0;

			r = vread(m, 'y', &v);
			if (r > 0)
				d->battery = v <= 100 ? v : -1;
		} else if (d && !strcmp(iface, BATTERY_IF)) {
			r = sd_bus_message_skip(m, "v");
		} else if (d && !strcmp(k, "Address"))
			r = vread_str(m, d->addr, sizeof(d->addr));
		else if (d && !strcmp(k, "Alias")) {
			char raw[64];

			r = vread_str(m, raw, sizeof(raw));
			if (r > 0)
				name_clean(d->name, sizeof(d->name), raw);
		} else if (d && !strcmp(k, "Connected"))
			r = vread_bool(m, &d->connected);
		else if (d && !strcmp(k, "ServicesResolved"))
			r = vread_bool(m, &d->resolved);
		else if (d && !strcmp(k, "Paired"))
			r = vread_bool(m, &d->paired);
		else if (d && !strcmp(k, "Trusted"))
			r = vread_bool(m, &d->trusted);
		else if (d && !strcmp(k, "UUIDs"))
			r = vread_strs(m, &d->kinds, NULL);
		else if (d && !strcmp(k, "RSSI")) {
			int16_t v = 0;

			r = vread(m, 'n', &v);
			if (r > 0) {
				struct sensor *s = sensor_by_addr(d->addr);

				d->rssi = v;
				d->has_rssi = true;
				if (s) {
					s->rssi = v;
					s->has_rssi = true;
				}
			}
		} else if (c && !strcmp(k, "UUID")) {
			char u[40];

			r = vread_str(m, u, sizeof(u));
			if (r > 0 && c == &tmp)
				c->uuid16 = bs_uuid16(u);
		} else if (c && !strcmp(k, "Flags"))
			r = vread_strs(m, NULL, c);
		else if (c && !strcmp(k, "Value"))
			r = vread_value(m, c, live);
		else if (is_adapter && !strcmp(path, D.adapter) && !strcmp(k, "Powered"))
			r = vread_bool(m, &D.powered);
		else if (is_adapter && !strcmp(path, D.adapter) && !strcmp(k, "Discovering"))
			r = vread_bool(m, &D.discovering);
		else
			r = sd_bus_message_skip(m, "v");
		if (r < 0 || sd_bus_message_exit_container(m) < 0)
			return -EBADMSG;
	}
	if (r < 0)
		return r;
	if (sd_bus_message_exit_container(m) < 0)
		return -EBADMSG;
	if (c == &tmp && is_sensor_chr(tmp.uuid16)) {
		for (int i = 0; i < MAX_CHRS; i++)
			if (!D.chrs[i].used) {
				D.chrs[i] = tmp;
				D.chrs[i].used = true;
				break;
			}
	}
	/* the address is the same on every name ours could match: keep the
	 * sensor's last RSSI in step with a device a snapshot brought */
	if (d && d->addr[0] && d->has_rssi) {
		struct sensor *s = sensor_by_addr(d->addr);

		if (s) {
			s->rssi = d->rssi;
			s->has_rssi = true;
		}
	}
	return 1;
}

/* PropertiesChanged's invalidated list. */
static void read_invalidated(sd_bus_message *m, const char *iface, struct dev *d)
{
	const char *k;

	if (sd_bus_message_enter_container(m, 'a', "s") <= 0)
		return;
	while (sd_bus_message_read_basic(m, 's', &k) > 0) {
		if (d && !strcmp(iface, DEVICE_IF) && !strcmp(k, "RSSI"))
			d->has_rssi = false;
		else if (d && !strcmp(iface, BATTERY_IF) && !strcmp(k, "Percentage"))
			d->battery = -1;
	}
	sd_bus_message_exit_container(m);
}

/* ------------------------------------------------------------ values */

static void value_bytes(struct chr *c, const uint8_t *b, size_t n)
{
	struct dev *d = dev_by_path(c->dev);
	struct sensor *s = d ? sensor_by_addr(d->addr) : NULL;
	struct bs_sample smp;
	int64_t now = now_ms();
	double rate;
	int k = c->uuid16 == BS_CHR_HR ? BS_HR : c->uuid16 == BS_CHR_POWER ? BS_POWER
		: c->uuid16 == BS_CHR_CSC ? BS_CSC : -1;

	/* ours, connected, and on a session this connection started */
	if (!s || s->forgetting || !d->connected)
		return;
	if (c->uuid16 == BS_CHR_BATTERY) {
		if (s->battery_notify != NS_NONE && n == 1 && b[0] <= 100) {
			s->gatt_battery = b[0];
			sensor_event(s, false);
		}
		return;
	}
	if (k < 0 || !(s->kinds & (1u << k)) || s->notify[k] == NS_NONE)
		return;
	/* A rate needs two samples within the freshness interval. */
	if (!s->last_ms[k] || now - s->last_ms[k] >= STALE_MS) {
		bs_revs_reset(&s->crank[k]);
		if (k == BS_CSC)
			bs_revs_reset(&s->wheel);
	}
	if (k == BS_HR) {
		int v = bs_parse_hr(b, n);

		if (v < 0 || v > BS_HR_MAX)
			return;
		s->hr = v;
	} else if (k == BS_POWER) {
		if (bs_parse_power(b, n, &smp))
			return;
		s->power = smp.power;
		s->has_power = true;
		if (smp.has_crank) {
			rate = bs_revs_feed(&s->crank[k], smp.crank_revs, smp.crank_time, 16, 1024, now);
			s->cadence[k] = rate > BS_CADENCE_MAX ? -1 : rate;
		} else {
			s->cadence[k] = -1;
			bs_revs_reset(&s->crank[k]);
		}
	} else {
		if (bs_parse_csc(b, n, &smp))
			return;
		if (smp.has_wheel) {
			rate = bs_revs_feed(&s->wheel, smp.wheel_revs, smp.wheel_time, 32, 1024, now);
			s->speed = rate < 0 ? -1 : bs_speed_kmh(rate, D.wheel_mm);
			if (s->speed > BS_SPEED_MAX)
				s->speed = -1;
		} else {
			s->speed = -1;
			bs_revs_reset(&s->wheel);
		}
		if (smp.has_crank) {
			rate = bs_revs_feed(&s->crank[k], smp.crank_revs, smp.crank_time, 16, 1024, now);
			s->cadence[k] = rate > BS_CADENCE_MAX ? -1 : rate;
		} else {
			s->cadence[k] = -1;
			bs_revs_reset(&s->crank[k]);
		}
	}
	s->last_ms[k] = now;
	value_event(s, k);
}

/* ------------------------------------------------------------ connections */

static void retry_window(void)
{
	int64_t now = now_ms();

	D.retry_until = now + (int64_t)g_retry_s * 1000 / g_scale;
	for (int i = 0; i < MAX_SENSORS; i++)
		if (D.sensors[i].used) {
			D.sensors[i].next_try_ms = 0;
			D.sensors[i].backoff_ms = BACKOFF_MIN_MS;
		}
}

/* A hold starts or ends; the window lapses: sensor states follow. */
static void sensor_conn_reset(struct sensor *s);

static void links_sync(void)
{
	bool h = held();

	if (h && !D.held) {
		for (int i = 0; i < MAX_SENSORS; i++)
			if (D.sensors[i].used) {
				D.sensors[i].next_try_ms = 0;
				D.sensors[i].backoff_ms = BACKOFF_MIN_MS;
			}
		vlog("held: reconnecting");
	}
	D.held = h;
	for (int i = 0; i < MAX_SENSORS; i++)
		if (D.sensors[i].used) {
			struct sensor *s = &D.sensors[i];
			struct dev *d = dev_by_addr(s->addr);

			/* Cancelling our bus callback alone does not stop BlueZ's
			 * outstanding LE scan/connection procedure. */
			if (!links_wanted() && s->connect_inflight && d && !d->connected) {
				struct pend t;

				sensor_conn_reset(s);
				t = for_sensor(PK_PLAIN, s, 0);
				call(&t, d->path, DEVICE_IF, "Disconnect", 0, NULL);
			}
			sensor_event(s, false);
		}
}

/* The connection is gone (or never was): everything per connection goes,
 * values go stale now, and late replies for it no longer match. */
static void sensor_conn_reset(struct sensor *s)
{
	bool had[BS_KINDS];

	pend_cancel(false, s->sid, 0, false);
	s->epoch++;
	s->was_connected = s->was_resolved = false;
	s->connect_inflight = false;
	s->battery_read = false;
	s->battery_notify = NS_NONE;
	s->gatt_battery = -1;
	s->hr = -1;
	s->power = 0;
	s->has_power = false;
	s->speed = -1;
	for (int k = 0; k < BS_KINDS; k++) {
		had[k] = s->last_ms[k] != 0;
		s->notify[k] = NS_NONE;
		s->last_ms[k] = 0;
		s->cadence[k] = -1;
		bs_revs_reset(&s->crank[k]);
	}
	bs_revs_reset(&s->wheel);
	for (int k = 0; k < BS_KINDS; k++)
		if (had[k])
			value_event(s, k);
}

static void sensor_init(struct sensor *s, const char *addr, unsigned kinds, const char *date, const char *name)
{
	memset(s, 0, sizeof(*s));
	s->used = true;
	s->sid = ++D.next_sid;
	snprintf(s->addr, sizeof(s->addr), "%s", addr);
	s->kinds = kinds;
	snprintf(s->date, sizeof(s->date), "%s", date);
	snprintf(s->name, sizeof(s->name), "%s", name);
	s->backoff_ms = BACKOFF_MIN_MS;
	s->gatt_battery = -1;
	s->hr = -1;
	s->speed = -1;
	for (int k = 0; k < BS_KINDS; k++) {
		s->cadence[k] = -1;
		bs_revs_reset(&s->crank[k]);
	}
	bs_revs_reset(&s->wheel);
}

/* One sensor's step towards connected with notifications on. */
static void sensor_keep(struct sensor *s, int64_t now)
{
	struct dev *d = dev_by_addr(s->addr);
	struct pend t;

	if (s->forgetting || !D.powered || !d || (D.pr.step != PS_IDLE && !strcmp(D.pr.addr, s->addr)))
		return;
	if (!d->connected) {
		if (!s->connect_inflight && now >= s->next_try_ms && links_wanted()) {
			t = for_sensor(PK_CONNECT, s, 0);
			if (call(&t, d->path, DEVICE_IF, "Connect", CONNECT_TIMEOUT_US, NULL) == 0)
				s->connect_inflight = true;
		}
		return;
	}
	if (!s->was_connected) {
		s->was_connected = true;
		s->backoff_ms = BACKOFF_MIN_MS;
	}
	if (!d->resolved)
		return;
	s->was_resolved = true;
	for (int k = 0; k < BS_KINDS; k++) {
		struct chr *c;

		if (!(s->kinds & (1u << k)) || s->notify[k] != NS_NONE)
			continue;
		c = chr_of(d->path, bs_chr_of_kind(k));
		t = for_sensor(PK_NOTIFY, s, k);
		if (c && c->can_notify && call(&t, c->path, CHR_IF, "StartNotify", 0, NULL) == 0)
			s->notify[k] = NS_PENDING;
	}
	if (d->battery < 0) {
		struct chr *c = chr_of(d->path, BS_CHR_BATTERY);

		t = for_sensor(PK_BATTERY, s, 0);
		if (!s->battery_read && c && c->can_read && call(&t, c->path, CHR_IF, "ReadValue", 0, "a{sv}", 0) == 0)
			s->battery_read = true;
		t = for_sensor(PK_BATTERY_NOTIFY, s, 0);
		if (s->battery_notify == NS_NONE && c && c->can_notify &&
		    call(&t, c->path, CHR_IF, "StartNotify", 0, NULL) == 0)
			s->battery_notify = NS_PENDING;
	}
}

static void sensor_reply(const struct pend *p, sd_bus_message *m)
{
	struct sensor *s = sensor_by_sid(p->sid);

	if (!s || s->epoch != p->epoch)
		return;		/* forgotten, replaced or another connection */
	if (p->kind == PK_CONNECT) {
		s->connect_inflight = false;
		if (sd_bus_message_is_method_error(m, NULL)) {
			vlog("connect %s: %s; next in %d ms", s->addr, err_name(m), s->backoff_ms);
			s->next_try_ms = now_ms() + s->backoff_ms;
			s->backoff_ms = s->backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s->backoff_ms * 2;
		}
	} else if (p->kind == PK_NOTIFY) {
		/* InProgress: BlueZ already has our session (registering) */
		if (!sd_bus_message_is_method_error(m, NULL) || err_is(m, BLUEZ ".Error.InProgress")) {
			s->notify[p->k] = NS_OURS;
		} else {
			notef("%s: %s notifications refused: %s", s->addr, bs_kind_name(p->k), err_name(m));
			s->notify[p->k] = NS_NONE;	/* tried again on the next tick */
		}
	} else if (p->kind == PK_BATTERY_NOTIFY) {
		s->battery_notify = !sd_bus_message_is_method_error(m, NULL) ||
			err_is(m, BLUEZ ".Error.InProgress") ? NS_OURS : NS_NONE;
	} else if (p->kind == PK_BATTERY) {
		const void *v = NULL;
		size_t n = 0;

		if (!sd_bus_message_is_method_error(m, NULL) && sd_bus_message_read_array(m, 'y', &v, &n) >= 0 &&
		    n == 1 && ((const uint8_t *)v)[0] <= 100)
			s->gatt_battery = ((const uint8_t *)v)[0];
	}
	sensor_event(s, false);
}

/* ------------------------------------------------------------ scanning */

static bool scan_wanted(void)
{
	if (!D.bluez || !D.powered || !D.adapter[0])
		return false;
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (D.clients[i].fd >= 0 && D.clients[i].scan)
			return true;
	return false;
}

/* Our discovery session follows scan_wanted. BlueZ ends it on its own when
 * the adapter powers off; that is seen through Powered. */
static void scan_sync(void)
{
	bool want = scan_wanted();
	struct pend t;

	if (D.scan_inflight || want == D.scan_on || now_ms() < D.scan_retry_ms)
		return;
	if (want) {
		/* LE only, our services: a dual-mode peer known over BR/EDR
		 * would otherwise be connected over BR/EDR */
		t = plain(PK_FILTER);
		if (call(&t, D.adapter, ADAPTER_IF, "SetDiscoveryFilter", 0, "a{sv}", 2, "Transport", "s", "le",
			 "UUIDs", "as", 3, "0000180d-0000-1000-8000-00805f9b34fb",
			 "00001818-0000-1000-8000-00805f9b34fb", "00001816-0000-1000-8000-00805f9b34fb") == 0)
			D.scan_inflight = true;
	} else {
		t = plain(PK_STOPSCAN);
		if (D.powered && call(&t, D.adapter, ADAPTER_IF, "StopDiscovery", 0, NULL) == 0) {
			D.scan_inflight = true;
		} else {
			D.scan_on = false;
			for (int i = 0; i < MAX_DEVS; i++)
				if (D.devs[i].used)
					found_update(&D.devs[i], false);
			adapter_event();
		}
	}
}

static void scan_reply(const struct pend *p, sd_bus_message *m)
{
	bool err = sd_bus_message_is_method_error(m, NULL);
	struct pend t;

	D.scan_inflight = false;
	if (p->kind == PK_FILTER) {
		t = plain(PK_SCAN);
		if (err) {
			notef("discovery filter: %s", err_name(m));
			D.scan_retry_ms = now_ms() + RETRY_MS;
		} else if (call(&t, D.adapter, ADAPTER_IF, "StartDiscovery", 0, NULL) == 0) {
			D.scan_inflight = true;
		}
		return;
	}
	if (p->kind == PK_SCAN) {
		if (err) {
			notef("discovery: %s", err_name(m));
			D.scan_retry_ms = now_ms() + RETRY_MS;
		} else {
			D.scan_on = true;
			notef("scanning for sensors");
		}
	} else {
		if (err)
			vlog("StopDiscovery: %s", err_name(m));
		D.scan_on = false;	/* an error means it is not ours to stop any more */
		notef("scan stopped");
	}
	for (int i = 0; i < MAX_DEVS; i++)
		if (D.devs[i].used)
			found_update(&D.devs[i], false);
	adapter_event();
	scan_sync();	/* the want may have changed meanwhile */
}

/* ------------------------------------------------------------ pairing */

/* BlueZ may initiate security during GATT resolution, before our own
 * StartNotify. Be its default agent only for the active pairing attempt.
 * Unregister removes us from BlueZ's default-agent queue, restoring the
 * previous agent; re-registration is ordinary and does not make us default. */
static void pair_agent_release(void)
{
	if (!D.pr.agent_requested)
		return;
	D.pr.agent_requested = false;
	D.agent_registered = false;
	D.agent_releasing = true;
	D.agent_retry_ms = 0;
	agent_release_drive(now_ms());
}


static void pair_done(void)
{
	struct sensor *s = NULL;
	struct dev *d = dev_by_addr(D.pr.addr);
	time_t t = time(NULL);
	struct tm tm;
	char date[16] = "";

	for (int i = 0; i < MAX_SENSORS && !s; i++)
		if (!D.sensors[i].used)
			s = &D.sensors[i];
	if (!s || !d) {
		pair_fail(!s ? "too many sensors" : "device gone");
		return;
	}
	/* a date only from a set clock (the boot clock starts at 1970) */
	if (gmtime_r(&t, &tm) && tm.tm_year + 1900 >= 2026)
		strftime(date, sizeof(date), "%Y-%m-%d", &tm);
	sensor_init(s, D.pr.addr, D.pr.kinds, date, D.pr.name);
	if (d->has_rssi) {
		s->rssi = d->rssi;
		s->has_rssi = true;
	}
	pend_cancel(false, 0, D.pr.id, false);
	D.pr.step = PS_IDLE;
	pair_agent_release();
	retry_window();
	pair_event("done", "");
	notef("paired %s", s->addr);
	save_records();
	found_update(d, false);
	sensor_event(s, true);
	sensor_keep(s, now_ms());
}

static void pair_fail(const char *reason)
{
	struct dev *d = dev_by_addr(D.pr.addr);
	struct pend t = plain(PK_PLAIN);
	enum pair_step step = D.pr.step;

	if (step == PS_IDLE)
		return;
	notef("pairing %s failed: %s", D.pr.addr, reason);
	pend_cancel(false, 0, D.pr.id, false);
	D.pr.step = PS_IDLE;
	pair_agent_release();
	if (D.bus && D.bluez && d && !strcmp(d->path, D.pr.path)) {
		if (step == PS_BOND)
			call(&t, d->path, DEVICE_IF, "CancelPairing", 0, NULL);
		if (D.pr.bonded || step == PS_BOND)
			call(&t, D.adapter, ADAPTER_IF, "RemoveDevice", 0, "o", d->path);
		else if (D.pr.connected_by_us)
			call(&t, d->path, DEVICE_IF, "Disconnect", 0, NULL);
	}
	pair_event("failed", reason);
}

static void pair_trust(void)
{
	struct pend t = for_pair(PK_PAIR_TRUST);

	D.pr.step = PS_TRUST;
	pair_event("saving", "");
	if (call(&t, D.pr.path, PROPS, "Set", 0, "ssv", DEVICE_IF, "Trusted", "b", 1))
		pair_fail("trust");
}

static bool auth_error(sd_bus_message *m)
{
	const sd_bus_error *e = sd_bus_message_get_error(m);
	const char *msg = e && e->message ? e->message : "";

	return err_is(m, BLUEZ ".Error.NotPermitted") || err_is(m, BLUEZ ".Error.NotAuthorized") ||
	       err_is(m, BLUEZ ".Error.AuthenticationFailed") || strstr(msg, "uthenticat") ||
	       strstr(msg, "ncrypt");
}

/* Authentication can be required by a GATT profile during Connect,
 * before ServicesResolved, as well as by our measurement subscription. */
static void pair_bond(void)
{
	struct pend t;

	if (!D.agent_registered) {
		pair_fail("no pairing agent");
		return;
	}
	D.pr.step = PS_BOND;
	pair_event("bonding", "");
	t = for_pair(PK_PAIR_BOND);
	if (call(&t, D.pr.path, DEVICE_IF, "Pair", (uint64_t)PAIR_TIMEOUT_MS * 1000, NULL))
		pair_fail("pair");
}

/* A new attempt can arrive while the previous one re-registers. */
static void pair_agent_request(void)
{
	struct pend t;

	if (!D.agent_registered || D.pr.agent_requested)
		return;
	t = for_pair(PK_PAIR_AGENT);
	if (call(&t, "/org/bluez", BLUEZ ".AgentManager1", "RequestDefaultAgent", 0, "o", AGENT_PATH))
		pair_fail("pairing agent");
	else
		D.pr.agent_requested = true;
}

/* Prove the measurement works: StartNotify on the first kind's value. */
static void pair_notify(void)
{
	struct dev *d = dev_by_addr(D.pr.addr);
	struct chr *c = NULL;
	struct pend t = for_pair(PK_PAIR_NOTIFY);

	for (int k = 0; k < BS_KINDS && !c && d; k++)
		if (D.pr.kinds & (1u << k))
			c = chr_of(d->path, bs_chr_of_kind(k));
	if (!c || !c->can_notify) {
		pair_fail("no sensor measurement found");
		return;
	}
	D.pr.step = PS_NOTIFY;
	if (call(&t, c->path, CHR_IF, "StartNotify", 0, NULL))
		pair_fail("notify");
}

/* The pairing goes on when the device's services are resolved. */
static void pair_check(void)
{
	struct dev *d = dev_by_addr(D.pr.addr);

	if (D.pr.step == PS_IDLE)
		return;
	if (!d || strcmp(d->path, D.pr.path)) {
		pair_fail("device gone");
		return;
	}
	if (now_ms() >= D.pr.deadline) {
		pair_fail("timed out");
		return;
	}
	if (D.pr.step == PS_CONNECT && !D.pr.agent_requested && !D.pr.connected_by_us)
		pair_agent_request();
	if (D.pr.step == PS_RESOLVE && d->connected && d->resolved) {
		unsigned k = d->kinds;

		if (k)
			D.pr.kinds = k;	/* the full list once resolved */
		pair_notify();
		return;
	}
	if (now_ms() >= D.pr.deadline)
		pair_fail("timed out");
}

static void pair_reply(const struct pend *p, sd_bus_message *m)
{
	bool err = sd_bus_message_is_method_error(m, NULL);
	struct pend t;

	if (D.pr.step == PS_IDLE || p->pair != D.pr.id)
		return;		/* a cancelled or earlier attempt */
	switch (p->kind) {
	case PK_PAIR_AGENT: {
		struct dev *d = dev_by_addr(D.pr.addr);

		if (D.pr.step != PS_CONNECT)
			return;
		if (err || !d) {
			pair_fail(err ? err_name(m) : "device gone");
			return;
		}
		if (d->connected) {
			D.pr.step = PS_RESOLVE;
			pair_event("resolving", "");
			pair_check();
		} else {
			t = for_pair(PK_PAIR_CONNECT);
			D.pr.connected_by_us = true;
			if (call(&t, d->path, DEVICE_IF, "Connect", CONNECT_TIMEOUT_US, NULL))
				pair_fail("connect");
		}
		break;
	}
	case PK_PAIR_CONNECT:
		if (D.pr.step != PS_CONNECT)
			return;
		if (err) {
			if (auth_error(m) && !D.pr.bonded)
				pair_bond();
			else
				pair_fail(err_name(m));
			return;
		}
		D.pr.step = PS_RESOLVE;
		pair_event("resolving", "");
		pair_check();
		break;
	case PK_PAIR_NOTIFY:
		if (D.pr.step != PS_NOTIFY)
			return;
		if (!err) {
			pair_trust();
		} else if (auth_error(m) && !D.pr.bonded) {
			pair_bond();
		} else {
			pair_fail(err_name(m));
		}
		break;
	case PK_PAIR_BOND:
		if (D.pr.step != PS_BOND)
			return;
		if (err && !err_is(m, BLUEZ ".Error.AlreadyExists")) {
			pair_fail(err_name(m));
			return;
		}
		D.pr.bonded = true;
		D.pr.step = PS_RESOLVE;
		pair_check();
		break;
	case PK_PAIR_TRUST:
		if (D.pr.step != PS_TRUST)
			return;
		if (err)
			pair_fail(err_name(m));
		else
			pair_done();
		break;
	default:
		break;
	}
}

/* Why ADDR cannot be paired now, or NULL. */
static const char *pair_refusal(const char *addr)
{
	struct dev *d = dev_by_addr(addr);
	int n = 0;

	if (!D.loaded)
		return "loading";
	if (!D.bluez || !D.powered)
		return "bluetooth off";
	if (D.pr.step != PS_IDLE)
		return "busy";
	if (sensor_by_addr(addr))
		return "already paired";

	if (!d || !d->kinds)
		return "not a sensor in range";
	for (int i = 0; i < MAX_SENSORS; i++)
		n += D.sensors[i].used;
	return n >= MAX_SENSORS ? "too many sensors" : NULL;
}

static void pair_start(const char *addr)
{
	struct dev *d = dev_by_addr(addr);

	memset(&D.pr, 0, sizeof(D.pr));
	{
		static uint32_t ids;

		D.pr.id = ++ids;
	}
	snprintf(D.pr.addr, sizeof(D.pr.addr), "%s", addr);
	snprintf(D.pr.path, sizeof(D.pr.path), "%s", d->path);
	/* the advertised name the user picked; a connection can replace the
	 * alias with the device's GAP name */
	snprintf(D.pr.name, sizeof(D.pr.name), "%s", d->name[0] ? d->name : addr);
	D.pr.kinds = d->kinds;
	D.pr.deadline = now_ms() + PAIR_TIMEOUT_MS;
	D.pr.step = PS_CONNECT;
	notef("pairing %s", addr);
	pair_event("connecting", "");
	pair_agent_request();
}

/* ------------------------------------------------------------ forget */

static void forget_finish(struct sensor *s)
{
	if (!s->removal_confirmed) {
		s->removal_confirmed = true;
		save_records();
		s->removal_revision = D.record_revision;
	}
}

/* Report completion only once the record deletion is saved. */
static void forget_durable(struct sensor *s)
{
	char b[OUT_MAX], addr[18];

	snprintf(addr, sizeof(addr), "%s", s->addr);
	pend_cancel(false, s->sid, 0, true);
	memset(s, 0, sizeof(*s));
	notef("forgot %s", addr);
	snprintf(b, sizeof(b), "{\"type\":\"forgotten\",\"addr\":\"%s\"}\n", addr);
	broadcast(b);
	/* back to an ordinary found device if it is still advertising */
	if (dev_by_addr(addr))
		found_update(dev_by_addr(addr), false);
}

/* Drive a pending removal: RemoveDevice by the device's current path. */
static void forget_step(struct sensor *s, int64_t now)
{
	struct dev *d;
	struct pend t;

	if (s->removal_confirmed) {
		if (D.saved_revision >= s->removal_revision)
			forget_durable(s);
		return;
	}
	if (!s->forgetting || D.saved_revision < s->forget_revision || s->forget_inflight ||
	    now < s->forget_next_ms || !D.bluez || !D.adapter[0])
		return;
	d = dev_by_addr(s->addr);
	if (!d) {
		forget_finish(s);	/* BlueZ does not know it (any more) */
		return;
	}
	t = for_sensor(PK_FORGET, s, 0);
	if (call(&t, D.adapter, ADAPTER_IF, "RemoveDevice", 0, "o", d->path) == 0)
		s->forget_inflight = true;
	else
		s->forget_next_ms = now + RETRY_MS;
}

static void forget_reply(const struct pend *p, sd_bus_message *m)
{
	struct sensor *s = sensor_by_sid(p->sid);

	if (!s || !s->forgetting)
		return;
	s->forget_inflight = false;
	if (!sd_bus_message_is_method_error(m, NULL) || err_is(m, BLUEZ ".Error.DoesNotExist") ||
	    err_is(m, "org.freedesktop.DBus.Error.UnknownObject")) {
		forget_finish(s);
	} else {
		notef("%s: removal failed (%s), retrying", s->addr, err_name(m));
		s->forget_next_ms = now_ms() + RETRY_MS;
	}
}

static void forget(const char *addr)
{
	struct sensor *s = sensor_by_addr(addr);

	if (!s || s->forgetting)
		return;
	sensor_conn_reset(s);
	s->forgetting = true;
	s->forget_next_ms = 0;
	notef("forgetting %s", addr);
	save_records();		/* save the flag before removing from BlueZ */
	s->forget_revision = D.record_revision;
	sensor_event(s, false);
	forget_step(s, now_ms());
}

/* ------------------------------------------------------------ records */

/* chef-state children: their own process group, killed as a group. */
static pid_t spawn_state(const char *verb, int in, int out)
{
	pid_t pid = fork();

	if (pid < 0)
		return -errno;
	if (pid == 0) {
		int nul = open("/dev/null", O_RDWR | O_CLOEXEC);
		sigset_t none;

		prctl(PR_SET_PDEATHSIG, SIGTERM);
		if (getppid() != g_self)
			_exit(127);
		setpgid(0, 0);
		dup2(in >= 0 ? in : nul, 0);
		dup2(out >= 0 ? out : nul, 1);
		dup2(nul, 2);
		signal(SIGPIPE, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		signal(SIGCHLD, SIG_DFL);
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		execlp(D.state_cmd, D.state_cmd, verb, (char *)NULL);
		_exit(127);
	}
	setpgid(pid, pid);
	return pid;
}

static void child_kill(struct child *c)
{
	if (c->pgid > 0)
		kill(-c->pgid, SIGKILL);
}

/* The record lines: "sensor ADDR KINDS DATE|- FLAG NAME" */
static size_t records_text(char *out, size_t n)
{
	size_t o = 0;

	out[0] = 0;
	for (int i = 0; i < MAX_SENSORS; i++) {
		const struct sensor *s = &D.sensors[i];
		char kinds[32];
		int w;

		if (!s->used || s->removal_confirmed)
			continue;
		kinds_str(s->kinds, kinds, sizeof(kinds));
		w = snprintf(out + o, n - o, "sensor %s %s %s %s %s\n", s->addr, kinds,
			     s->date[0] ? s->date : "-", s->forgetting ? "forget" : "-",
			     s->name[0] ? s->name : s->addr);
		if (w < 0 || (size_t)w >= n - o)
			break;
		o += (size_t)w;
	}
	return o;
}

static void save_records(void)
{
	D.record_revision++;
	D.save_pending = true;
	save_start();
}

static void save_start(void)
{
	char text[2048];
	int p[2];
	size_t len;
	pid_t pid;

	if (!D.save_pending || D.save.pgid > 0 || now_ms() < D.save_retry_ms)
		return;
	D.save_pending = false;
	len = records_text(text, sizeof(text));
	if (pipe2(p, O_CLOEXEC) < 0) {
		notef("pipe: %s; sensors not saved", strerror(errno));
		D.save_pending = true;
		D.save_retry_ms = now_ms() + RETRY_MS;
		return;
	}
	pid = spawn_state("blesens-save", p[0], -1);
	close(p[0]);
	if (pid < 0) {
		close(p[1]);
		notef("cannot run %s blesens-save: %s; sensors not saved", D.state_cmd, strerror(-pid));
		D.save_pending = true;
		D.save_retry_ms = now_ms() + RETRY_MS;
		return;
	}
	/* at most 2 KiB into an empty pipe: never blocks */
	D.save_write_ok = write(p[1], text, len) == (ssize_t)len;
	if (!D.save_write_ok)
		notef("blesens-save: short write");
	close(p[1]);
	D.save_revision = D.record_revision;
	D.save = (struct child){ .pid = pid, .pgid = pid, .until = now_ms() + CHILD_SAVE_MS };
}

static void save_finished(bool success)
{
	if (success && D.save_write_ok) {
		D.saved_revision = D.save_revision;
		D.save_retry_ms = 0;
	} else {
		D.save_pending = true;
		D.save_retry_ms = now_ms() + RETRY_MS;
	}
	child_kill(&D.save);	/* nothing of it outlives the leader */
	D.save = (struct child){ 0 };
	if (D.save_pending)
		save_start();
}

static void load_parse(void)
{
	char *save = NULL, *line;
	int n = 0;

	D.load_buf[D.load_len] = 0;
	for (line = strtok_r(D.load_buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char addr[18], kinds[32], date[16], flag[8], name[64];
		int off = 0;
		struct sensor *s;

		if (sscanf(line, "sensor %17s %31s %15s %7s %n", addr, kinds, date, flag, &off) != 4 || !off ||
		    !addr_ok(addr) || !kinds_parse(kinds) || (strcmp(flag, "-") && strcmp(flag, "forget")) ||
		    n >= MAX_SENSORS || sensor_by_addr(addr))
			continue;
		name_clean(name, sizeof(name), line + off);
		s = &D.sensors[n++];
		sensor_init(s, addr, kinds_parse(kinds), strcmp(date, "-") ? date : "", name[0] ? name : addr);
		s->forgetting = !strcmp(flag, "forget");
	}
	notef("%d sensor%s", n, n == 1 ? "" : "s");
}

static void load_finish(bool timed_out)
{
	if (D.load_fd >= 0) {
		close(D.load_fd);
		D.load_fd = -1;
	}
	child_kill(&D.load);
	D.load = (struct child){ 0 };
	if (timed_out)
		notef("%s blesens-load gave no answer: no sensors this time", D.state_cmd);
	else
		load_parse();
	D.loaded = true;
	for (int i = 0; i < MAX_SENSORS; i++)
		if (D.sensors[i].used)
			sensor_event(&D.sensors[i], true);
}

static void load_start(void)
{
	int p[2];
	pid_t pid;

	if (pipe2(p, O_CLOEXEC) < 0) {
		load_finish(true);
		return;
	}
	pid = spawn_state("blesens-load", -1, p[1]);
	close(p[1]);
	if (pid < 0) {
		close(p[0]);
		notef("cannot run %s blesens-load: %s", D.state_cmd, strerror(-pid));
		load_finish(true);
		return;
	}
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	D.load = (struct child){ .pid = pid, .pgid = pid, .until = now_ms() + CHILD_LOAD_MS };
	D.load_fd = p[0];
}

/* Read what is in the pipe now; EOF closes it. */
static void load_read(void)
{
	size_t room = D.load_len < sizeof(D.load_buf) - 1 ? sizeof(D.load_buf) - 1 - D.load_len : 0;
	ssize_t n = room ? read(D.load_fd, D.load_buf + D.load_len, room) : 0;

	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (n > 0)
		D.load_len += (size_t)n;
	if (n <= 0 || D.load_len >= sizeof(D.load_buf) - 1) {
		close(D.load_fd);
		D.load_fd = -1;
	}
}

static void reap(void)
{
	int status;
	pid_t pid;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);

		if (pid == D.save.pid) {
			if (rc)
				notef("%s blesens-save exited %d: sensors may not be saved", D.state_cmd, rc);
			save_finished(rc == 0);
		} else if (pid == D.load.pid) {
			D.load.pid = 0;
			/* its output is complete: what a descendant still holds
			 * open is not waited for */
			child_kill(&D.load);
		}
	}
	if (!D.loaded && D.load.pgid > 0 && !D.load.pid) {
		/* the leader is done and its writes are in the pipe: take
		 * them, never wait for a killed descendant's end */
		size_t before;

		do {
			before = D.load_len;
			if (D.load_fd >= 0)
				load_read();
		} while (D.load_fd >= 0 && D.load_len > before);
		load_finish(false);
	}
}

static void children_tick(int64_t now)
{
	if (D.load.pgid > 0 && now >= D.load.until)
		load_finish(true);	/* kills the group */
	if (D.save.pgid > 0 && now >= D.save.until) {
		notef("%s blesens-save still running after %d s: killed", D.state_cmd, CHILD_SAVE_MS / 1000);
		child_kill(&D.save);	/* the leader's exit (reaped next) finishes it */
		D.save.until = INT64_MAX;
	}
}

/* ------------------------------------------------------------ signals */

/* After any change in BlueZ's objects: connections lost, found devices,
 * the pairing, the scan, every sensor event. */
static void devices_changed(void)
{
	for (int i = 0; i < MAX_SENSORS; i++) {
		struct sensor *s = &D.sensors[i];
		struct dev *d = s->used ? dev_by_addr(s->addr) : NULL;

		if (!s->used)
			continue;
		/* a connection that went (or lost its services) ends ours */
		if ((s->was_connected && (!d || !d->connected || !D.powered)) ||
		    (s->was_resolved && (!d || !d->resolved)))
			sensor_conn_reset(s);
		if (s->link_up != (d && d->connected && D.powered)) {
			s->link_up = !s->link_up;
			notef("%s %s", s->addr, s->link_up ? "connected" : "disconnected");
			if (!s->link_up && D.powered && !s->forgetting)
				retry_window();	/* it may be back in a moment */
		}
		sensor_event(s, false);
	}
	for (int i = 0; i < MAX_DEVS; i++)
		if (D.devs[i].used)
			found_update(&D.devs[i], false);
	adapter_event();
	pair_check();
	scan_sync();
}

static int props_signal(sd_bus_message *m, void *u, sd_bus_error *e)
{
	const char *iface = NULL, *path = sd_bus_message_get_path(m);
	bool was_powered = D.powered;
	int old_rssi = 0;
	bool old_has = false;
	struct dev *d;

	(void)u;
	(void)e;
	if (!path || sd_bus_message_read_basic(m, 's', &iface) <= 0 || !iface)
		return 0;
	if (strcmp(iface, DEVICE_IF) && strcmp(iface, CHR_IF) && strcmp(iface, ADAPTER_IF) &&
	    strcmp(iface, BATTERY_IF))
		return 0;
	d = dev_by_path(path);
	if (d) {
		old_rssi = d->rssi;
		old_has = d->has_rssi;
	}
	if (read_iface(m, path, iface, true) < 0)
		return 0;
	read_invalidated(m, iface, d);
	if (was_powered != D.powered) {
		notef("adapter %s", D.powered ? "on" : "off");
		if (D.powered)
			retry_window();
		if (!D.powered) {
			pair_fail("bluetooth off");
			D.scan_on = false;	/* BlueZ ended every discovery */
		}
	}
	if (d && (d->rssi != old_rssi || d->has_rssi != old_has))
		found_update(d, true);
	devices_changed();
	return 0;
}

static int added_signal(sd_bus_message *m, void *u, sd_bus_error *e)
{
	const char *path = NULL;
	int r;

	(void)u;
	(void)e;
	if (sd_bus_message_read_basic(m, 'o', &path) <= 0 || !path ||
	    sd_bus_message_enter_container(m, 'a', "{sa{sv}}") <= 0)
		return 0;
	while ((r = sd_bus_message_enter_container(m, 'e', "sa{sv}")) > 0) {
		const char *iface = NULL;

		if (sd_bus_message_read_basic(m, 's', &iface) <= 0)
			return 0;
		if (iface && (!strcmp(iface, DEVICE_IF) || !strcmp(iface, CHR_IF) || !strcmp(iface, ADAPTER_IF) ||
			      !strcmp(iface, BATTERY_IF)))
			r = read_iface(m, path, iface, false);
		else
			r = sd_bus_message_skip(m, "a{sv}");
		if (r < 0 || sd_bus_message_exit_container(m) < 0)
			return 0;
	}
	devices_changed();
	return 0;
}

static int removed_signal(sd_bus_message *m, void *u, sd_bus_error *e)
{
	const char *path = NULL, *iface;
	struct dev *d;

	(void)u;
	(void)e;
	if (sd_bus_message_read_basic(m, 'o', &path) <= 0 || !path ||
	    sd_bus_message_enter_container(m, 'a', "s") <= 0)
		return 0;
	while (sd_bus_message_read_basic(m, 's', &iface) > 0) {
		if (!strcmp(iface, DEVICE_IF) && (d = dev_by_path(path))) {
			d->connected = d->resolved = false;
			d->has_rssi = false;
			found_update(d, false);
			for (int i = 0; i < MAX_CHRS; i++)
				if (D.chrs[i].used && !strcmp(D.chrs[i].dev, path))
					D.chrs[i].used = false;
			d->used = false;
		} else if (!strcmp(iface, BATTERY_IF) && (d = dev_by_path(path))) {
			d->battery = -1;
		} else if (!strcmp(iface, CHR_IF)) {
			struct chr *c = chr_by_path(path);
			char dev[128];

			if (c) {
				/* our session on it is gone with it */
				snprintf(dev, sizeof(dev), "%s", c->dev);
				c->used = false;
				d = dev_by_path(dev);
				for (int i = 0; d && i < MAX_SENSORS; i++)
					if (D.sensors[i].used && !D.sensors[i].forgetting &&
					    !strcmp(D.sensors[i].addr, d->addr) &&
					    (D.sensors[i].was_connected || D.sensors[i].was_resolved))
						sensor_conn_reset(&D.sensors[i]);
			}
		} else if (!strcmp(iface, ADAPTER_IF) && !strcmp(path, D.adapter)) {
			D.adapter[0] = 0;
			D.powered = false;
			D.scan_on = false;
			pair_fail("adapter gone");
		}
	}
	devices_changed();
	return 0;
}

/* ------------------------------------------------------------ the agent */

/* Only BlueZ, and only about the device being paired (or, for services,
 * a saved sensor); anything else is rejected. */
static int agent_call(sd_bus_message *m, void *u, sd_bus_error *e)
{
	const char *member = sd_bus_message_get_member(m), *sender = sd_bus_message_get_sender(m);
	const char *dev = NULL;
	bool ok = false;

	(void)u;
	if (!sender || !D.bluez_owner[0] || strcmp(sender, D.bluez_owner))
		return sd_bus_error_set(e, BLUEZ ".Error.Rejected", "not from bluetoothd");
	if (!strcmp(member, "Release") || !strcmp(member, "Cancel"))
		return sd_bus_reply_method_return(m, NULL);
	if (sd_bus_message_read_basic(m, 'o', &dev) <= 0 || !dev)
		return sd_bus_error_set(e, BLUEZ ".Error.Rejected", "no device");
	if (!strcmp(member, "RequestConfirmation") || !strcmp(member, "RequestAuthorization") ||
	    !strcmp(member, "DisplayPasskey") || !strcmp(member, "DisplayPinCode"))
		ok = D.pr.step != PS_IDLE && !strcmp(dev, D.pr.path);
	else if (!strcmp(member, "AuthorizeService")) {
		struct dev *d = dev_by_path(dev);
		struct sensor *s = d ? sensor_by_addr(d->addr) : NULL;

		ok = (D.pr.step != PS_IDLE && !strcmp(dev, D.pr.path)) || (s && !s->forgetting);
	}
	notef("agent %s for %s: %s", member, dev, ok ? "accepted" : "rejected");
	if (!ok)
		return sd_bus_error_set(e, BLUEZ ".Error.Rejected", "not a sensor being paired");
	return sd_bus_reply_method_return(m, NULL);
}

static int agent_reject(sd_bus_message *m, void *u, sd_bus_error *e)
{
	(void)m;
	(void)u;
	return sd_bus_error_set(e, BLUEZ ".Error.Rejected", "no input or output");
}

static const sd_bus_vtable agent_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Release", "", "", agent_call, 0),
	SD_BUS_METHOD("RequestPinCode", "o", "s", agent_reject, 0),
	SD_BUS_METHOD("DisplayPinCode", "os", "", agent_call, 0),
	SD_BUS_METHOD("RequestPasskey", "o", "u", agent_reject, 0),
	SD_BUS_METHOD("DisplayPasskey", "ouq", "", agent_call, 0),
	SD_BUS_METHOD("RequestConfirmation", "ou", "", agent_call, 0),
	SD_BUS_METHOD("RequestAuthorization", "o", "", agent_call, 0),
	SD_BUS_METHOD("AuthorizeService", "os", "", agent_call, 0),
	SD_BUS_METHOD("Cancel", "", "", agent_call, 0),
	SD_BUS_VTABLE_END
};

static void agent_release_drive(int64_t now)
{
	struct pend t = plain(PK_AGENT_RELEASE);

	if (!D.bluez || !D.agent_releasing || D.agent_release_inflight || now < D.agent_retry_ms)
		return;
	if (call(&t, "/org/bluez", BLUEZ ".AgentManager1", "UnregisterAgent", 0, "o", AGENT_PATH) == 0)
		D.agent_release_inflight = true;
	else
		D.agent_retry_ms = now + RETRY_MS;
}

static void agent_register(int64_t now)
{
	struct pend t = plain(PK_AGENT);

	if (D.agent_releasing) {
		agent_release_drive(now);
		return;
	}
	if (!D.bluez || D.agent_registered || D.agent_inflight || now < D.agent_retry_ms)
		return;
	if (call(&t, "/org/bluez", BLUEZ ".AgentManager1", "RegisterAgent", 0, "os", AGENT_PATH,
		 "NoInputNoOutput") == 0)
		D.agent_inflight = true;
}

static void agent_reply(sd_bus_message *m)
{
	D.agent_inflight = false;
	if (!sd_bus_message_is_method_error(m, NULL) || err_is(m, BLUEZ ".Error.AlreadyExists")) {
		D.agent_registered = true;
		vlog("agent registered");
		pair_check();
	} else {
		notef("RegisterAgent: %s; retrying", err_name(m));
		D.agent_retry_ms = now_ms() + RETRY_MS;
	}
}

/* ------------------------------------------------------------ bus life */

static void objects_reply(sd_bus_message *m)
{
	const char *sender = sd_bus_message_get_sender(m);
	int r;

	if (sd_bus_message_is_method_error(m, NULL)) {
		vlog("GetManagedObjects: %s", err_name(m));
		return;	/* bluetoothd not there yet: NameOwnerChanged brings it */
	}
	memset(D.devs, 0, sizeof(D.devs));
	memset(D.chrs, 0, sizeof(D.chrs));
	D.adapter[0] = 0;
	D.powered = false;
	snprintf(D.bluez_owner, sizeof(D.bluez_owner), "%s", sender ? sender : "");
	if (sd_bus_message_enter_container(m, 'a', "{oa{sa{sv}}}") <= 0)
		return;
	while ((r = sd_bus_message_enter_container(m, 'e', "oa{sa{sv}}")) > 0) {
		const char *path = NULL;

		if (sd_bus_message_read_basic(m, 'o', &path) <= 0 ||
		    sd_bus_message_enter_container(m, 'a', "{sa{sv}}") <= 0)
			return;
		while ((r = sd_bus_message_enter_container(m, 'e', "sa{sv}")) > 0) {
			const char *iface = NULL;

			if (sd_bus_message_read_basic(m, 's', &iface) <= 0)
				return;
			if (!strcmp(iface, DEVICE_IF) || !strcmp(iface, CHR_IF) || !strcmp(iface, ADAPTER_IF) ||
			    !strcmp(iface, BATTERY_IF))
				r = read_iface(m, path, iface, false);
			else
				r = sd_bus_message_skip(m, "a{sv}");
			if (r < 0 || sd_bus_message_exit_container(m) < 0)
				return;
		}
		if (sd_bus_message_exit_container(m) < 0 || sd_bus_message_exit_container(m) < 0)
			return;
	}
	D.bluez = true;
	notef("bluetoothd: adapter %s, %s", D.adapter[0] ? D.adapter : "none", D.powered ? "on" : "off");
	retry_window();		/* also this daemon's start */
	agent_register(now_ms());
	devices_changed();
}

/* bluetoothd went or came back: nothing of the old one is ours any more. */
static void bluez_reset(void)
{
	pair_fail("bluetoothd gone");
	pend_cancel(true, 0, 0, true);
	D.bluez = false;
	D.bluez_owner[0] = 0;
	D.agent_registered = D.agent_inflight = false;
	D.agent_releasing = D.agent_release_inflight = false;
	D.agent_retry_ms = 0;
	D.adapter[0] = 0;
	D.powered = D.discovering = D.scan_on = D.scan_inflight = false;
	D.scan_retry_ms = 0;
	for (int i = 0; i < MAX_DEVS; i++)
		if (D.devs[i].used && D.devs[i].found_sent) {
			D.devs[i].has_rssi = false;
			found_update(&D.devs[i], false);
		}
	memset(D.devs, 0, sizeof(D.devs));
	memset(D.chrs, 0, sizeof(D.chrs));
	for (int i = 0; i < MAX_SENSORS; i++)
		if (D.sensors[i].used) {
			sensor_conn_reset(&D.sensors[i]);
			D.sensors[i].next_try_ms = 0;
			D.sensors[i].backoff_ms = BACKOFF_MIN_MS;
		}
}

static void bluez_refresh(void)
{
	struct pend t = plain(PK_OBJECTS);

	call(&t, "/", OBJMGR, "GetManagedObjects", 0, NULL);
}

static int owner_signal(sd_bus_message *m, void *u, sd_bus_error *e)
{
	const char *name, *old, *nw;

	(void)u;
	(void)e;
	if (sd_bus_message_read(m, "sss", &name, &old, &nw) < 0 || strcmp(name, BLUEZ))
		return 0;
	bluez_reset();
	if (nw && *nw) {
		notef("bluetoothd (re)started");
		bluez_refresh();
	} else {
		notef("bluetoothd gone");
	}
	adapter_event();
	for (int i = 0; i < MAX_SENSORS; i++)
		if (D.sensors[i].used)
			sensor_event(&D.sensors[i], false);
	return 0;
}

static int on_reply(sd_bus_message *m, void *u, sd_bus_error *e)
{
	struct pend *pp = u, p = *pp;

	(void)e;
	pend_free(pp);
	switch (p.kind) {
	case PK_OBJECTS:
		objects_reply(m);
		break;
	case PK_AGENT:
		agent_reply(m);
		break;
	case PK_AGENT_RELEASE:
		D.agent_release_inflight = false;
		if (!sd_bus_message_is_method_error(m, NULL) || err_is(m, BLUEZ ".Error.DoesNotExist")) {
			D.agent_releasing = false;
			D.agent_retry_ms = 0;
			agent_register(now_ms());
		} else {
			notef("pairing agent release: %s; retrying", err_name(m));
			D.agent_retry_ms = now_ms() + RETRY_MS;
		}
		break;
	case PK_FILTER:
	case PK_SCAN:
	case PK_STOPSCAN:
		scan_reply(&p, m);
		break;
	case PK_CONNECT:
	case PK_NOTIFY:
	case PK_BATTERY:
	case PK_BATTERY_NOTIFY:
		sensor_reply(&p, m);
		break;
	case PK_FORGET:
		forget_reply(&p, m);
		break;
	case PK_PAIR_AGENT:
	case PK_PAIR_CONNECT:
	case PK_PAIR_NOTIFY:
	case PK_PAIR_BOND:
	case PK_PAIR_TRUST:
		pair_reply(&p, m);
		break;
	case PK_PLAIN:
		if (sd_bus_message_is_method_error(m, NULL))
			vlog("%s", err_name(m));
		break;
	}
	return 0;
}

static void bus_open(void)
{
	static const char *const rules[] = {
		"type='signal',sender='" BLUEZ "',interface='" PROPS "',member='PropertiesChanged'",
		"type='signal',sender='" BLUEZ "',interface='" OBJMGR "',member='InterfacesAdded'",
		"type='signal',sender='" BLUEZ "',interface='" OBJMGR "',member='InterfacesRemoved'",
		"type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
		"interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='" BLUEZ "'",
	};
	static sd_bus_message_handler_t cbs[] = { props_signal, added_signal, removed_signal, owner_signal };
	int r;

	r = sd_bus_open_system(&D.bus);
	if (r < 0) {
		D.bus = NULL;
		bus_lost("unavailable");
		return;
	}
	D.bus_fd = sd_bus_get_fd(D.bus);
	for (unsigned i = 0; i < 4; i++) {
		r = sd_bus_add_match(D.bus, &D.slots[D.n_slots], rules[i], cbs[i], NULL);
		if (r < 0) {
			notef("AddMatch: %s", strerror(-r));
			bus_lost("refused a match");
			return;
		}
		D.n_slots++;
	}
	r = sd_bus_add_object_vtable(D.bus, &D.agent_slot, AGENT_PATH, BLUEZ ".Agent1", agent_vtable, NULL);
	if (r < 0)
		notef("agent object: %s", strerror(-r));
	D.bus_backoff_ms = 1000 / g_scale;
	bluez_refresh();
}

static void bus_lost(const char *why)
{
	notef("system bus %s: retrying", why);
	bluez_reset();
	for (int i = 0; i < D.n_slots; i++)
		sd_bus_slot_unref(D.slots[i]);
	D.n_slots = 0;
	D.agent_slot = sd_bus_slot_unref(D.agent_slot);
	if (D.bus) {
		sd_bus_close(D.bus);
		sd_bus_unref(D.bus);
	}
	D.bus = NULL;
	D.bus_fd = -1;
	if (D.bus_backoff_ms <= 0)
		D.bus_backoff_ms = 1000 / g_scale;
	D.bus_retry_ms = now_ms() + D.bus_backoff_ms;
	D.bus_backoff_ms = D.bus_backoff_ms * 2 > BUS_BACKOFF_MAX_MS ? BUS_BACKOFF_MAX_MS : D.bus_backoff_ms * 2;
	adapter_event();
	for (int i = 0; i < MAX_SENSORS; i++)
		if (D.sensors[i].used)
			sensor_event(&D.sensors[i], false);
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

	json_str(cm, sizeof(cm), cmd);
	json_str(r, sizeof(r), reason);
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
				conn += sensor_connected(&D.sensors[i]);
			}
		snprintf(b, sizeof(b), "{\"type\":\"status\",\"bluez\":%s,\"adapter\":%s,\"powered\":%s,"
			 "\"scanning\":%s,\"loaded\":%s,\"sensors\":%d,\"connected\":%d,\"pairing\":%s,"
			 "\"agent\":%s,\"wheel_mm\":%d,\"held\":%s,\"reconnecting\":%s}\n",
			 D.bluez ? "true" : "false", D.adapter[0] ? "true" : "false", D.powered ? "true" : "false",
			 D.scan_on ? "true" : "false", D.loaded ? "true" : "false", n, conn,
			 D.pr.step != PS_IDLE ? "true" : "false", D.agent_registered ? "true" : "false", D.wheel_mm,
			 held() ? "true" : "false", links_wanted() ? "true" : "false");
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
		scan_sync();
	} else if (!strcmp(cmd, "hold") && arg && (!strcmp(arg, "on") || !strcmp(arg, "off"))) {
		c->hold = !strcmp(arg, "on");
		reply_ok(c, "hold");
		links_sync();
	} else if (!strcmp(cmd, "pair") && addr_ok(arg)) {
		/* the reply before the first event, so a client can tell */
		err = pair_refusal(arg);
		if (err) {
			reply_err(c, "pair", err);
		} else {
			reply_ok(c, "pair");
			pair_start(arg);
		}
	} else if (!strcmp(cmd, "cancel") && addr_ok(arg)) {
		if (D.pr.step != PS_IDLE && !strcmp(D.pr.addr, arg)) {
			reply_ok(c, "cancel");
			pair_fail("cancelled");
		} else {
			reply_err(c, "cancel", "not pairing");
		}
	} else if (!strcmp(cmd, "forget") && addr_ok(arg)) {
		if (!D.loaded) {
			reply_err(c, "forget", "loading");
		} else if (!sensor_by_addr(arg)) {
			reply_err(c, "forget", "not paired");
		} else {
			reply_ok(c, "forget");
			forget(arg);
		}
	} else {
		reply_err(c, cmd, "unknown request");
	}
}

static void client_input(struct client *c)
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

/* ------------------------------------------------------------ main loop */

static void tick(void)
{
	int64_t now = now_ms();
	static bool wanted;

	if (wanted != links_wanted()) {
		wanted = !wanted;
		if (!wanted)
			notef("no sensor holder: reconnection paused");
		links_sync();
	}
	children_tick(now);
	save_start();
	if (!D.bus && now >= D.bus_retry_ms)
		bus_open();
	agent_register(now);
	for (int i = 0; i < MAX_SENSORS; i++) {
		struct sensor *s = &D.sensors[i];

		if (!s->used)
			continue;
		if (D.loaded) {
			forget_step(s, now);
			if (s->used && D.bluez)
				sensor_keep(s, now);
		}
		/* values with no notification for STALE_MS go to null once */
		for (int k = 0; s->used && k < BS_KINDS; k++)
			if (s->last_ms[k] && now - s->last_ms[k] >= STALE_MS) {
				s->last_ms[k] = 0;
				value_event(s, k);
			}
	}
	pair_check();
	scan_sync();
}

static int sock_listen(const char *path)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);

	if (fd < 0)
		return -errno;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
	unlink(path);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || chmod(path, 0600) < 0 || listen(fd, 8) < 0) {
		int e = errno;

		close(fd);
		return -e;
	}
	return fd;
}

static void on_stop(int sig)
{
	(void)sig;
	g_stop = 1;
	if (write(g_sigpipe[1], "x", 1) < 0) {
		/* a wakeup is already pending */
	}
}

static void on_chld(int sig)
{
	(void)sig;
	if (write(g_sigpipe[1], "c", 1) < 0) {
		/* as above */
	}
}

static void loop_round(int lfd)
{
	struct pollfd pfd[4 + MAX_CLIENTS];
	struct client *cmap[MAX_CLIENTS];
	int n = 0, nc = 0, iload = -1, ilisten = -1, iclients, timeout = TICK_MS;
	static int64_t next_tick;
	uint64_t usec = UINT64_MAX;
	int64_t now = now_ms();

	if (next_tick - now < timeout)
		timeout = next_tick > now ? (int)(next_tick - now) : 0;
	pfd[n++] = (struct pollfd){ .fd = g_sigpipe[0], .events = POLLIN };
	if (lfd >= 0) {
		ilisten = n;
		pfd[n++] = (struct pollfd){ .fd = lfd, .events = POLLIN };
	}
	if (D.bus) {
		int ev = sd_bus_get_events(D.bus);

		pfd[n++] = (struct pollfd){ .fd = D.bus_fd, .events = (short)(ev < 0 ? POLLIN : ev) };
		if (sd_bus_get_timeout(D.bus, &usec) >= 0 && usec != UINT64_MAX) {
			struct timespec ts;
			uint64_t now_us;
			int t;

			clock_gettime(CLOCK_MONOTONIC, &ts);
			now_us = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
			t = usec <= now_us ? 0 : (int)((usec - now_us + 999) / 1000);
			if (t < timeout)
				timeout = t;
		}
	}
	if (D.load_fd >= 0) {
		iload = n;
		pfd[n++] = (struct pollfd){ .fd = D.load_fd, .events = POLLIN };
	}
	iclients = n;
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (D.clients[i].fd >= 0) {
			cmap[nc++] = &D.clients[i];
			pfd[n++] = (struct pollfd){ .fd = D.clients[i].fd, .events = POLLIN };
		}
	if (poll(pfd, (nfds_t)n, timeout) < 0 && errno != EINTR)
		return;
	if (pfd[0].revents & POLLIN) {
		char junk[32];

		while (read(g_sigpipe[0], junk, sizeof(junk)) > 0)
			;
	}
	if (iload >= 0 && D.load_fd >= 0 && (pfd[iload].revents & (POLLIN | POLLHUP | POLLERR)))
		load_read();
	reap();
	if (D.bus) {
		for (int i = 0; i < 64; i++) {
			int r = sd_bus_process(D.bus, NULL);

			if (r < 0) {
				bus_lost("lost");
				break;
			}
			if (r == 0)
				break;
		}
	}
	if (ilisten >= 0 && (pfd[ilisten].revents & POLLIN)) {
		int cfd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
		struct client *c = NULL;

		for (int i = 0; i < MAX_CLIENTS && !c && cfd >= 0; i++)
			if (D.clients[i].fd < 0)
				c = &D.clients[i];
		if (c) {
			memset(c, 0, sizeof(*c));
			c->fd = cfd;
		} else if (cfd >= 0) {
			close(cfd);
		}
	}
	for (int i = 0; i < nc; i++)
		if (cmap[i]->fd >= 0 && (pfd[iclients + i].revents & (POLLIN | POLLHUP | POLLERR)))
			client_input(cmap[i]);
	if (now_ms() >= next_tick) {
		next_tick = now_ms() + TICK_MS;
		tick();
	}
}

/* ------------------------------------------------------------ client CLI */

/* Does this JSON line have "key":"value"? */
static bool has_kv(const char *line, const char *key, const char *value)
{
	char pat[96];

	snprintf(pat, sizeof(pat), "\"%s\":\"%s\"", key, value);
	return strstr(line, pat) != NULL;
}

/* Is this line the end of the request? */
static bool client_done(const char *cmd, const char *arg, const char *line)
{
	if (!strcmp(cmd, "status"))
		return has_kv(line, "type", "status");
	if (has_kv(line, "type", "error") && has_kv(line, "cmd", cmd))
		return true;
	if (!strcmp(cmd, "pair"))
		return has_kv(line, "type", "pair") && has_kv(line, "addr", arg) &&
		       (has_kv(line, "state", "done") || has_kv(line, "state", "failed"));
	if (!strcmp(cmd, "forget"))
		return has_kv(line, "type", "forgotten") && has_kv(line, "addr", arg);
	if (!strcmp(cmd, "list") || !strcmp(cmd, "cancel"))
		return has_kv(line, "type", "ok") && has_kv(line, "cmd", cmd);
	return false;	/* watch, scan: until interrupted */
}

static int client_main(const char *sock, int argc, char **argv)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	const char *cmd = argv[0], *arg = argc > 1 ? argv[1] : "";
	char req[128], buf[4096];
	size_t len = 0;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	bool known = !strcmp(cmd, "status") || !strcmp(cmd, "list") || !strcmp(cmd, "watch") ||
		     !strcmp(cmd, "scan") || ((!strcmp(cmd, "pair") || !strcmp(cmd, "cancel") ||
					       !strcmp(cmd, "forget")) && addr_ok(arg));

	if (!known) {
		fprintf(stderr, "blesensd: unknown request (status|list|watch|scan|pair ADDR|cancel ADDR|forget ADDR)\n");
		return 64;
	}
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sock);
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		fprintf(stderr, "blesensd: connect %s: %s\n", sock, strerror(errno));
		return 1;
	}
	if (!strcmp(cmd, "scan"))
		snprintf(req, sizeof(req), "watch\nscan on\n");
	else if (!strcmp(cmd, "pair") || !strcmp(cmd, "forget"))
		snprintf(req, sizeof(req), "watch\n%s %s\n", cmd, arg);	/* the outcome is an event */
	else
		snprintf(req, sizeof(req), "%s%s%s\n", cmd, *arg ? " " : "", arg);
	if (write(fd, req, strlen(req)) != (ssize_t)strlen(req))
		return 1;
	for (;;) {
		ssize_t n = read(fd, buf + len, sizeof(buf) - 1 - len);
		char *nl;

		if (n <= 0)
			break;
		len += (size_t)n;
		buf[len] = 0;
		while ((nl = strchr(buf, '\n'))) {
			bool done, failed;

			*nl = 0;
			printf("%s\n", buf);
			done = client_done(cmd, arg, buf);
			failed = has_kv(buf, "type", "error") || has_kv(buf, "state", "failed");
			len -= (size_t)(nl + 1 - buf);
			memmove(buf, nl + 1, len + 1);
			if (done) {
				fflush(stdout);
				close(fd);
				return failed ? 1 : 0;
			}
		}
		fflush(stdout);
		if (len == sizeof(buf) - 1)
			len = 0;	/* no line is this long */
	}
	close(fd);
	return 1;
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	int opt, lfd;

	D.sock_path = SOCK_PATH;
	D.state_cmd = "chef-state";
	D.wheel_mm = 2105;
	while ((opt = getopt(argc, argv, "+S:s:w:r:Tv")) != -1) {
		switch (opt) {
		case 'S': D.sock_path = optarg; break;
		case 's': D.state_cmd = optarg; break;
		case 'w': D.wheel_mm = atoi(optarg); break;
		case 'r': g_retry_s = atoi(optarg); break;
		case 'T': g_scale = 10; break;
		case 'v': g_verbose = true; break;
		default:
			fprintf(stderr, "usage: blesensd [-S SOCK] [-s STATE_CMD] [-w WHEEL_MM] [-r RETRY_S] [-T] [-v]\n"
				"       blesensd [-S SOCK] status|list|watch|scan|pair ADDR|cancel ADDR|forget ADDR\n");
			return 64;
		}
	}
	if (optind < argc)
		return client_main(D.sock_path, argc - optind, argv + optind);
	if (D.wheel_mm < 500 || D.wheel_mm > 4000) {
		fprintf(stderr, "blesensd: wheel circumference %d mm out of range (500 to 4000)\n", D.wheel_mm);
		return 64;
	}
	g_self = getpid();
	for (int i = 0; i < MAX_CLIENTS; i++)
		D.clients[i].fd = -1;
	if (pipe2(g_sigpipe, O_CLOEXEC | O_NONBLOCK) < 0)
		return 1;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_stop;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sa.sa_handler = on_chld;
	sa.sa_flags = SA_NOCLDSTOP | SA_RESTART;
	sigaction(SIGCHLD, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	lfd = sock_listen(D.sock_path);
	if (lfd < 0) {
		notef("listen %s: %s; exiting in 5 s", D.sock_path, strerror(-lfd));
		sleep(5);
		return 1;
	}
	load_start();
	bus_open();
	notef("ready on %s (wheel %d mm)", D.sock_path, D.wheel_mm);
	while (!g_stop)
		loop_round(lfd);
	notef("exiting");
	close(lfd);
	unlink(D.sock_path);
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (D.clients[i].fd >= 0) {
			close(D.clients[i].fd);
			D.clients[i].fd = -1;
		}
	/* the last save, bounded; then nothing of ours outlives us */
	{
		int64_t until = now_ms() + EXIT_SAVE_MS;

		while ((D.save.pgid > 0 || D.save_pending) && now_ms() < until) {
			if (D.save.pgid <= 0)
				save_start();
			loop_round(-1);
		}
		if (D.save.pgid > 0 || D.save_pending)
			notef("exiting with the sensor save unfinished");
		child_kill(&D.save);
		child_kill(&D.load);
	}
	if (D.scan_on && D.bus && D.adapter[0]) {
		sd_bus_message *m = NULL;

		if (sd_bus_message_new_method_call(D.bus, &m, BLUEZ, D.adapter, ADAPTER_IF, "StopDiscovery") >= 0)
			sd_bus_call(D.bus, m, 500000, NULL, NULL);	/* bounded */
		sd_bus_message_unref(m);
	}
	if (D.bus) {
		pend_cancel(true, 0, 0, true);
		sd_bus_flush(D.bus);
		sd_bus_close(D.bus);
		sd_bus_unref(D.bus);
	}
	return 0;
}
