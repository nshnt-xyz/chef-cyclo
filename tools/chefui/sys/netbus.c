/*
 * netbus.c - Wi-Fi (NetworkManager) and Bluetooth (BlueZ) state over the
 * system bus with sd-bus: static basu on the phone (<basu/sd-bus.h>),
 * libsystemd on the PC (<systemd/sd-bus.h>), the same API.
 *
 * Nothing here blocks the chefui loop:
 *  - the connection is sd-bus's own nonblocking connect; authentication
 *    and Hello complete inside sd_bus_process() on later wakeups;
 *  - every read is an asynchronous method call whose reply lands in a
 *    callback; matches are added asynchronously too;
 *  - the bus fd joins the loop with sd_bus_get_events() and an LVGL
 *    timer follows sd_bus_get_timeout(); each wakeup processes at most
 *    DISPATCH_MAX messages and continues on the next loop pass;
 *  - calls carry NO_AUTO_START, so a query never starts NetworkManager or
 *    bluetoothd through D-Bus activation.
 *
 * State is re-read (Wi-Fi: WirelessEnabled, wlan0 State/ActiveConnection,
 * the connection Id; Bluetooth: GetManagedObjects for Adapter1 Powered
 * and Device1 Connected) at start, when the service's name gets an owner,
 * and on PropertiesChanged/InterfacesAdded/Removed signals that touch
 * those properties, coalesced over REFRESH_COALESCE_MS. No polling. A
 * lost bus is closed (never flushed) and reopened with backoff. The bus
 * is only closed outside sd_bus_process(), never from a callback.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "chefui.h"
#include "netbus_int.h"
#include "sys.h"
#include "sysstate.h"

#define NM "org.freedesktop.NetworkManager"
#define NM_PATH "/org/freedesktop/NetworkManager"
#define NM_DEVICE NM ".Device"
#define NM_ACTIVE NM ".Connection.Active"
#define BLUEZ "org.bluez"
#define PROPS "org.freedesktop.DBus.Properties"
#define OBJMGR "org.freedesktop.DBus.ObjectManager"
#define WIFI_IFACE "wlan0"

#define DISPATCH_MAX 32
#define REFRESH_COALESCE_MS 100
#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 10000

struct refresh {
	bool inflight, dirty;
	unsigned id;		/* replies for an older id are ignored */
	int pending;
	bool failed;
	lv_timer_t *soon;
	void (*run)(void);
};

static struct {
	bool running;
	sd_bus *bus;
	int fd;
	bool broken;		/* set in callbacks, acted on after processing */
	bool in_dispatch;	/* inside sd_bus_process(): never close the bus */
	lv_timer_t *timeout, *more, *retry;
	int backoff_ms;
	bool down_logged;
	unsigned seq;

	struct refresh nm, bz;
	struct {
		bool known, enabled, device;
		uint32_t dev_state;
		char ssid[64];
	} wifi;			/* assembled by the NM refresh */
	struct {
		bool known, adapter, powered;
		int connected;
	} bt;			/* assembled by the BlueZ refresh */
	netbus_bus_cb clients[NETBUS_CLIENTS_MAX];
	int n_clients;
} N = { .fd = -1 };

static void dispatch(void);
static void bus_open(void);

/* ------------------------------------------------------------- loop glue */

static uint64_t now_usec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void bus_io(int fd, short revents, void *user)
{
	(void)fd;
	(void)revents;
	(void)user;
	dispatch();
}

static void timeout_fired(lv_timer_t *t)
{
	(void)t;
	dispatch();
}

static void more_fired(lv_timer_t *t)
{
	(void)t;
	N.more = NULL;
	dispatch();
}

/* Follow what sd-bus wants: fd events and its next timeout. */
static void rearm(void)
{
	uint64_t at = UINT64_MAX;
	int ev;

	if (!N.bus)
		return;
	ev = sd_bus_get_events(N.bus);
	if (ev < 0)
		ev = POLLIN;
	chefui_watch_fd(N.fd, (short)ev, bus_io, NULL);
	if (sd_bus_get_timeout(N.bus, &at) < 0)
		at = UINT64_MAX;
	if (at == UINT64_MAX) {
		lv_timer_pause(N.timeout);
	} else {
		uint64_t now = now_usec();
		uint32_t ms = at <= now ? 0 : (uint32_t)((at - now + 999) / 1000);

		lv_timer_set_period(N.timeout, ms);
		lv_timer_reset(N.timeout);
		lv_timer_resume(N.timeout);
	}
}

static void publish_wifi(bool known, bool enabled, bool device, uint32_t st, const char *ssid)
{
	struct sys_state *s = &sys_state;

	if (s->wifi.known == known && s->wifi.enabled == enabled && s->wifi.device == device &&
	    s->wifi.dev_state == st && !strcmp(s->wifi.ssid, ssid))
		return;
	s->wifi.known = known;
	s->wifi.enabled = enabled;
	s->wifi.device = device;
	s->wifi.dev_state = st;
	snprintf(s->wifi.ssid, sizeof(s->wifi.ssid), "%s", ssid);
	sys_changed(SYS_WIFI);
}

static void publish_bt(bool known, bool adapter, bool powered, int connected)
{
	struct sys_state *s = &sys_state;

	if (s->bt.known == known && s->bt.adapter == adapter && s->bt.powered == powered &&
	    s->bt.connected == connected)
		return;
	s->bt.known = known;
	s->bt.adapter = adapter;
	s->bt.powered = powered;
	s->bt.connected = connected;
	sys_changed(SYS_BT);
}

static void retry_fired(lv_timer_t *t)
{
	(void)t;
	N.retry = NULL;
	if (N.running && !N.bus)
		bus_open();
}

static void refresh_reset(struct refresh *r)
{
	r->inflight = r->dirty = false;
	r->pending = 0;
	r->id = ++N.seq;	/* drop any reply still in flight */
}

/* Close (never flush: that could block) and reopen later. */
static void bus_lost(const char *why, int err)
{
	if (!N.down_logged)
		chefui_log("netbus: system bus %s: %s; retrying", why, strerror(err > 0 ? err : -err));
	N.down_logged = true;
	if (N.bus) {
		sd_bus *b = N.bus;

		chefui_unwatch_fd(N.fd);
		sd_bus_close(b);
		N.bus = NULL;
		N.fd = -1;
		/* clients drop their slots (which hold references) first */
		for (int i = 0; i < N.n_clients; i++)
			N.clients[i](NULL);
		sd_bus_unref(b);
	}
	N.broken = false;
	lv_timer_pause(N.timeout);
	refresh_reset(&N.nm);
	refresh_reset(&N.bz);
	publish_wifi(false, false, false, 0, "");
	publish_bt(false, false, false, 0);
	if (N.running && !N.retry) {
		N.retry = lv_timer_create(retry_fired, N.backoff_ms, NULL);
		lv_timer_set_repeat_count(N.retry, 1);
		N.backoff_ms = N.backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : N.backoff_ms * 2;
	}
}

static void dispatch(void)
{
	int i, r = 0;

	if (!N.bus)
		return;
	N.in_dispatch = true;
	for (i = 0; i < DISPATCH_MAX; i++) {
		r = sd_bus_process(N.bus, NULL);
		if (r <= 0 || N.broken)
			break;
	}
	N.in_dispatch = false;
	if (r < 0 || N.broken || !sd_bus_is_open(N.bus)) {
		bus_lost("lost", r < 0 ? r : -ECONNRESET);
		return;
	}
	if (r > 0 && !N.more) {
		/* more queued: continue on the next loop pass */
		N.more = lv_timer_create(more_fired, 0, NULL);
		lv_timer_set_repeat_count(N.more, 1);
	}
	rearm();
}

/* An asynchronous call without D-Bus activation. arg: NULL or one string;
 * arg2 a second string (Properties.Get). 0 or -errno. */
static int call(const char *dest, const char *path, const char *iface, const char *member,
		const char *arg, const char *arg2, sd_bus_message_handler_t cb, unsigned id)
{
	sd_bus_message *m = NULL;
	int r;

	r = sd_bus_message_new_method_call(N.bus, &m, dest, path, iface, member);
	if (r >= 0)
		r = sd_bus_message_set_auto_start(m, 0);
	if (r >= 0 && arg)
		r = sd_bus_message_append_basic(m, 's', arg);
	if (r >= 0 && arg2)
		r = sd_bus_message_append_basic(m, 's', arg2);
	if (r >= 0)
		r = sd_bus_call_async(N.bus, NULL, m, cb, (void *)(uintptr_t)id, 0);
	sd_bus_message_unref(m);
	if (r < 0)
		N.broken = true;
	return r < 0 ? r : 0;
}

static int get_prop(const char *dest, const char *path, const char *iface, const char *prop,
		    sd_bus_message_handler_t cb, unsigned id)
{
	return call(dest, path, PROPS, "Get", iface, prop, cb, id);
}

/* Read a Properties.Get reply's variant of basic type t into p. */
static int read_variant(sd_bus_message *m, char t, void *p)
{
	char sig[2] = { t, 0 };
	int r = sd_bus_message_enter_container(m, 'v', sig);

	if (r <= 0)
		return r < 0 ? r : -ENXIO;
	r = sd_bus_message_read_basic(m, t, p);
	if (r <= 0)
		return r < 0 ? r : -ENXIO;
	return sd_bus_message_exit_container(m);
}

/* Refresh bookkeeping: a refresh issues calls (pending), each reply calls
 * step_done(); the last one publishes. A change during a refresh reruns it. */
static bool current(struct refresh *r, void *ud)
{
	return r->inflight && (unsigned)(uintptr_t)ud == r->id;
}

static void refresh_soon_fired(lv_timer_t *t)
{
	struct refresh *r = lv_timer_get_user_data(t);

	r->soon = NULL;
	if (!N.bus)
		return;
	r->run();
	if (N.broken)
		bus_lost("lost", -EIO);
	else
		rearm();
}

/* Coalesce change signals into one refresh. */
static void refresh_soon(struct refresh *r)
{
	if (r->soon)
		return;
	r->soon = lv_timer_create(refresh_soon_fired, REFRESH_COALESCE_MS, r);
	lv_timer_set_repeat_count(r->soon, 1);
}

static bool begin(struct refresh *r)
{
	if (r->inflight) {
		r->dirty = true;
		return false;
	}
	r->inflight = true;
	r->dirty = false;
	r->failed = false;
	r->pending = 0;
	r->id = ++N.seq;
	return true;
}

/* ------------------------------------------------------- NetworkManager */

static void nm_publish(void)
{
	bool ok = N.wifi.known;

	publish_wifi(ok, ok && N.wifi.enabled, ok && N.wifi.device, ok ? N.wifi.dev_state : 0,
		     ok && N.wifi.dev_state == SYS_NM_DEVICE_ACTIVATED ? N.wifi.ssid : "");
}

static void nm_step_done(void)
{
	if (--N.nm.pending > 0)
		return;
	N.nm.inflight = false;
	nm_publish();
	if (N.nm.dirty)
		refresh_soon(&N.nm);
}

static int nm_id_reply(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	const char *id = NULL;

	(void)e;
	if (!current(&N.nm, ud))
		return 0;
	if (!sd_bus_message_is_method_error(m, NULL) && read_variant(m, 's', &id) >= 0 && id)
		snprintf(N.wifi.ssid, sizeof(N.wifi.ssid), "%s", id);
	nm_step_done();
	return 0;
}

static int nm_active_reply(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	const char *path = NULL;

	(void)e;
	if (!current(&N.nm, ud))
		return 0;
	if (!sd_bus_message_is_method_error(m, NULL) && read_variant(m, 'o', &path) >= 0 && path &&
	    strcmp(path, "/")) {
		if (!get_prop(NM, path, NM_ACTIVE, "Id", nm_id_reply, N.nm.id))
			N.nm.pending++;
	}
	nm_step_done();
	return 0;
}

static int nm_state_reply(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	uint32_t st = 0;

	(void)e;
	if (!current(&N.nm, ud))
		return 0;
	if (!sd_bus_message_is_method_error(m, NULL) && read_variant(m, 'u', &st) >= 0)
		N.wifi.dev_state = st;
	nm_step_done();
	return 0;
}

static int nm_device_reply(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	const char *path = NULL;

	(void)e;
	if (!current(&N.nm, ud))
		return 0;
	/* an error here is "no such device": wlan0 not up (yet) */
	if (!sd_bus_message_is_method_error(m, NULL) && sd_bus_message_read_basic(m, 'o', &path) > 0 &&
	    path) {
		N.wifi.device = true;
		if (!get_prop(NM, path, NM_DEVICE, "State", nm_state_reply, N.nm.id))
			N.nm.pending++;
		if (!get_prop(NM, path, NM_DEVICE, "ActiveConnection", nm_active_reply, N.nm.id))
			N.nm.pending++;
	}
	nm_step_done();
	return 0;
}

static int nm_enabled_reply(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	int on = 0;

	(void)e;
	if (!current(&N.nm, ud))
		return 0;
	if (sd_bus_message_is_method_error(m, NULL)) {
		N.wifi.known = false;	/* NetworkManager not on the bus */
	} else if (read_variant(m, 'b', &on) >= 0) {
		N.wifi.known = true;
		N.wifi.enabled = on;
	}
	nm_step_done();
	return 0;
}

static void nm_refresh(void)
{
	if (!N.bus || !begin(&N.nm))
		return;
	memset(&N.wifi, 0, sizeof(N.wifi));
	if (!get_prop(NM, NM_PATH, NM, "WirelessEnabled", nm_enabled_reply, N.nm.id))
		N.nm.pending++;
	if (!call(NM, NM_PATH, NM, "GetDeviceByIpIface", WIFI_IFACE, NULL, nm_device_reply, N.nm.id))
		N.nm.pending++;
	if (!N.nm.pending)
		N.nm.inflight = false;
}

/* ------------------------------------------------------------------ BlueZ */

static int skip_or_bool(sd_bus_message *m, bool want, int *out)
{
	const char *contents = NULL;
	char type;
	int r;

	if (!want)
		return sd_bus_message_skip(m, "v");
	r = sd_bus_message_peek_type(m, &type, &contents);
	if (r <= 0)
		return r < 0 ? r : -ENXIO;
	if (type != 'v' || !contents || strcmp(contents, "b"))
		return sd_bus_message_skip(m, "v");
	return read_variant(m, 'b', out);
}

/* a{sv} of one interface: pick out want_key as a bool into *out. */
static int read_props(sd_bus_message *m, const char *want_key, int *out)
{
	int r = sd_bus_message_enter_container(m, 'a', "{sv}");

	if (r <= 0)
		return r < 0 ? r : -ENXIO;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *key = NULL;

		r = sd_bus_message_read_basic(m, 's', &key);
		if (r <= 0)
			return r < 0 ? r : -ENXIO;
		r = skip_or_bool(m, key && !strcmp(key, want_key), out);
		if (r < 0)
			return r;
		r = sd_bus_message_exit_container(m);
		if (r < 0)
			return r;
	}
	if (r < 0)
		return r;
	return sd_bus_message_exit_container(m);
}

/* GetManagedObjects: a{oa{sa{sv}}} */
static int parse_objects(sd_bus_message *m, bool *adapter, bool *powered, int *connected)
{
	int r = sd_bus_message_enter_container(m, 'a', "{oa{sa{sv}}}");

	if (r <= 0)
		return r < 0 ? r : -ENXIO;
	while ((r = sd_bus_message_enter_container(m, 'e', "oa{sa{sv}}")) > 0) {
		const char *path;

		if ((r = sd_bus_message_read_basic(m, 'o', &path)) <= 0)
			return r < 0 ? r : -ENXIO;
		if ((r = sd_bus_message_enter_container(m, 'a', "{sa{sv}}")) <= 0)
			return r < 0 ? r : -ENXIO;
		while ((r = sd_bus_message_enter_container(m, 'e', "sa{sv}")) > 0) {
			const char *iface = NULL;
			int v = 0;

			if ((r = sd_bus_message_read_basic(m, 's', &iface)) <= 0)
				return r < 0 ? r : -ENXIO;
			if (iface && !strcmp(iface, BLUEZ ".Adapter1")) {
				if ((r = read_props(m, "Powered", &v)) < 0)
					return r;
				*adapter = true;
				*powered = *powered || v;
			} else if (iface && !strcmp(iface, BLUEZ ".Device1")) {
				if ((r = read_props(m, "Connected", &v)) < 0)
					return r;
				*connected += !!v;
			} else if ((r = sd_bus_message_skip(m, "a{sv}")) < 0) {
				return r;
			}
			if ((r = sd_bus_message_exit_container(m)) < 0)
				return r;
		}
		if (r < 0 || (r = sd_bus_message_exit_container(m)) < 0 ||
		    (r = sd_bus_message_exit_container(m)) < 0)
			return r;
	}
	if (r < 0)
		return r;
	return sd_bus_message_exit_container(m);
}

static int bz_objects_reply(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	bool adapter = false, powered = false;
	int connected = 0;

	(void)e;
	if (!current(&N.bz, ud))
		return 0;
	N.bz.inflight = false;
	if (sd_bus_message_is_method_error(m, NULL)) {
		publish_bt(false, false, false, 0);	/* bluetoothd not on the bus */
	} else if (parse_objects(m, &adapter, &powered, &connected) < 0) {
		chefui_log("netbus: unexpected GetManagedObjects reply from BlueZ");
		publish_bt(false, false, false, 0);
	} else {
		publish_bt(true, adapter, adapter && powered, adapter && powered ? connected : 0);
	}
	if (N.bz.dirty)
		refresh_soon(&N.bz);
	return 0;
}

static void bz_refresh(void)
{
	if (!N.bus || !begin(&N.bz))
		return;
	if (call(BLUEZ, "/", OBJMGR, "GetManagedObjects", NULL, NULL, bz_objects_reply, N.bz.id))
		N.bz.inflight = false;
}

/* ---------------------------------------------------------------- signals */

/* PropertiesChanged (sa{sv}as): does it touch one of keys for iface? */
static bool props_touch(sd_bus_message *m, const char *const *ifaces, const char *const *keys)
{
	const char *iface = NULL;
	bool hit = false, known = false;
	int r;

	if (sd_bus_message_read_basic(m, 's', &iface) <= 0 || !iface)
		return false;
	for (int i = 0; ifaces[i]; i++)
		known = known || !strcmp(iface, ifaces[i]);
	if (!known)
		return false;
	if (sd_bus_message_enter_container(m, 'a', "{sv}") <= 0)
		return true;	/* odd but ours: refresh */
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *key = NULL;

		if (sd_bus_message_read_basic(m, 's', &key) <= 0)
			return true;
		for (int i = 0; key && keys[i]; i++)
			hit = hit || !strcmp(key, keys[i]);
		if (sd_bus_message_skip(m, "v") < 0 || sd_bus_message_exit_container(m) < 0)
			return true;
	}
	if (r < 0 || sd_bus_message_exit_container(m) < 0)
		return true;
	if (sd_bus_message_enter_container(m, 'a', "s") > 0) {
		const char *key;

		while (sd_bus_message_read_basic(m, 's', &key) > 0)
			for (int i = 0; key && keys[i]; i++)
				hit = hit || !strcmp(key, keys[i]);
	}
	return hit;
}

static int nm_props_signal(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	static const char *const ifaces[] = { NM, NM_DEVICE, NM_ACTIVE, NULL };
	static const char *const keys[] = { "WirelessEnabled", "PrimaryConnection",
		"ActiveConnections", "Devices", "State", "ActiveConnection", "Id", NULL };

	(void)ud;
	(void)e;
	if (props_touch(m, ifaces, keys))
		refresh_soon(&N.nm);
	return 0;
}

static int nm_signal(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	const char *member = sd_bus_message_get_member(m);

	(void)ud;
	(void)e;
	if (member && (!strcmp(member, "DeviceAdded") || !strcmp(member, "DeviceRemoved")))
		refresh_soon(&N.nm);
	return 0;
}

static int bz_props_signal(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	static const char *const ifaces[] = { BLUEZ ".Adapter1", BLUEZ ".Device1", NULL };
	static const char *const keys[] = { "Powered", "Connected", NULL };

	(void)ud;
	(void)e;
	if (props_touch(m, ifaces, keys))
		refresh_soon(&N.bz);
	return 0;
}

static int bz_objects_signal(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	(void)m;
	(void)ud;
	(void)e;
	refresh_soon(&N.bz);
	return 0;
}

/* NameOwnerChanged (sss) for NM or BlueZ: a new owner is a (re)started
 * service; no owner means it went away. */
static int owner_signal(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	const char *name = NULL, *old = NULL, *new = NULL;
	struct refresh *r = ud;

	(void)e;
	if (sd_bus_message_read(m, "sss", &name, &old, &new) < 0)
		return 0;
	if (new && *new) {
		refresh_soon(r);
	} else if (r == &N.nm) {
		refresh_reset(&N.nm);
		publish_wifi(false, false, false, 0, "");
	} else {
		refresh_reset(&N.bz);
		publish_bt(false, false, false, 0);
	}
	return 0;
}

static int match_installed(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	(void)ud;
	(void)e;
	if (sd_bus_message_is_method_error(m, NULL))
		chefui_log("netbus: AddMatch failed: %s", sd_bus_message_get_error(m)->message);
	return 0;
}

static void add_match(const char *rule, sd_bus_message_handler_t cb, void *ud)
{
	if (sd_bus_add_match_async(N.bus, NULL, rule, cb, match_installed, ud) < 0)
		N.broken = true;
}

/* -------------------------------------------------------------- lifecycle */

static void bus_open(void)
{
	sd_bus *b = NULL;
	int r;

	r = sd_bus_open_system(&b);	/* nonblocking connect; Hello is async */
	if (r < 0) {
		bus_lost("unavailable", r);
		return;
	}
	N.bus = b;
	N.fd = sd_bus_get_fd(b);
	N.broken = false;
	(void)sd_bus_set_exit_on_disconnect(b, 0);
	add_match("type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
		  "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='" NM "'",
		  owner_signal, &N.nm);
	add_match("type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
		  "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='" BLUEZ "'",
		  owner_signal, &N.bz);
	add_match("type='signal',sender='" NM "',interface='" PROPS "',member='PropertiesChanged'",
		  nm_props_signal, NULL);
	add_match("type='signal',sender='" NM "',interface='" NM "'", nm_signal, NULL);
	add_match("type='signal',sender='" BLUEZ "',interface='" PROPS "',member='PropertiesChanged'",
		  bz_props_signal, NULL);
	add_match("type='signal',sender='" BLUEZ "',interface='" OBJMGR "'", bz_objects_signal, NULL);
	nm_refresh();
	bz_refresh();
	for (int i = 0; i < N.n_clients && !N.broken; i++)
		N.clients[i](b);
	if (N.broken) {
		bus_lost("setup failed", -EIO);
		return;
	}
	if (N.down_logged)
		chefui_log("netbus: system bus connected");
	N.down_logged = false;
	N.backoff_ms = BACKOFF_MIN_MS;
	rearm();
}

int sys_netbus_start(void)
{
	N.running = true;
	N.backoff_ms = BACKOFF_MIN_MS;
	N.nm.run = nm_refresh;
	N.bz.run = bz_refresh;
	N.timeout = lv_timer_create(timeout_fired, 1000, NULL);
	lv_timer_pause(N.timeout);
	bus_open();
	return 0;
}

void sys_netbus_stop(void)
{
	N.running = false;
	if (N.bus) {
		sd_bus *b = N.bus;

		chefui_unwatch_fd(N.fd);
		sd_bus_close(b);
		N.bus = NULL;
		for (int i = 0; i < N.n_clients; i++)
			N.clients[i](NULL);
		sd_bus_unref(b);
	}
	lv_timer_t **timers[] = { &N.timeout, &N.more, &N.retry, &N.nm.soon, &N.bz.soon };

	for (unsigned i = 0; i < sizeof(timers) / sizeof(timers[0]); i++)
		if (*timers[i]) {
			lv_timer_delete(*timers[i]);
			*timers[i] = NULL;
		}
}

/* ------------------------------------------------------------- clients */

void netbus_add_client(netbus_bus_cb cb)
{
	if (N.n_clients < NETBUS_CLIENTS_MAX)
		N.clients[N.n_clients++] = cb;
	if (N.bus)
		cb(N.bus);
}

sd_bus *netbus_bus(void)
{
	return N.bus;
}

int netbus_new_call(sd_bus_message **m, const char *dest, const char *path, const char *iface,
		    const char *member)
{
	int r;

	*m = NULL;
	if (!N.bus)
		return -ENOTCONN;
	r = sd_bus_message_new_method_call(N.bus, m, dest, path, iface, member);
	if (r >= 0)
		r = sd_bus_message_set_auto_start(*m, 0);
	if (r < 0) {
		sd_bus_message_unref(*m);
		*m = NULL;
	}
	return r < 0 ? r : 0;
}

int netbus_send(sd_bus_message *m, sd_bus_slot **slot, sd_bus_message_handler_t cb, void *ud,
		uint64_t usec)
{
	int r;

	if (!N.bus)
		return -ENOTCONN;
	r = sd_bus_call_async(N.bus, slot, m, cb, ud, usec);
	if (r < 0)
		N.broken = true;
	return r < 0 ? r : 0;
}

int netbus_match(sd_bus_slot **slot, const char *rule, sd_bus_message_handler_t cb, void *ud)
{
	int r;

	if (!N.bus)
		return -ENOTCONN;
	r = sd_bus_add_match_async(N.bus, slot, rule, cb, match_installed, ud);
	if (r < 0)
		N.broken = true;
	return r < 0 ? r : 0;
}

void netbus_kick(void)
{
	if (!N.bus || N.in_dispatch)
		return;		/* dispatch() acts on it when it returns */
	if (N.broken)
		bus_lost("lost", -EIO);
	else if (!N.in_dispatch)
		rearm();
}
