/*
 * wifi.c - the Wi-Fi pages' NetworkManager client (see wifi.h), on the
 * netbus connection: asynchronous calls and matches only, every call with
 * NO_AUTO_START, nothing blocking the chefui loop.
 *
 * While watched, one refresh reads wlan0 (GetDeviceByIpIface), its access
 * points (GetAllAccessPoints, then each AP's properties), the device, its
 * active connection, IPv4 config and active AP, NetworkManager's
 * connectivity, and the saved Wi-Fi profiles (ListConnections and each
 * profile's GetSettings, which carry no secrets). It reruns on NM's
 * PropertiesChanged and Settings signals, coalesced (REFRESH_DELAY_MS
 * after the first signal, at most every REFRESH_MIN_MS). Not watched: no
 * matches, no calls, the view is left as it was.
 *
 * A scan (Device.Wireless.RequestScan) runs at watch start and every
 * WIFI_SCAN_PERIOD_MS while the list asks for it; "scanning" lasts until
 * LastScan changes or SCAN_SHOW_MS.
 *
 * A join is followed by the device's StateChanged signal (matched before
 * the call is sent): ACTIVATED ends it, FAILED ends it with the reason, a
 * profile this join created is deleted on failure, and JOIN_TIMEOUT_MS
 * bounds it. A join lives on when the pages are left.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include "chefui.h"
#include "netbus_int.h"
#include "sysstate.h"
#include "wifi.h"

#define NM "org.freedesktop.NetworkManager"
#define NM_PATH "/org/freedesktop/NetworkManager"
#define NM_SETTINGS_PATH NM_PATH "/Settings"
#define NM_SETTINGS NM ".Settings"
#define NM_CONN NM ".Settings.Connection"
#define NM_DEVICE NM ".Device"
#define NM_WIRELESS NM ".Device.Wireless"
#define NM_AP NM ".AccessPoint"
#define NM_ACTIVE NM ".Connection.Active"
#define NM_IP4 NM ".IP4Config"
#define PROPS "org.freedesktop.DBus.Properties"
#define WIFI_IFACE "wlan0"

#define REFRESH_DELAY_MS 300
#define REFRESH_MIN_MS 1500
#define SCAN_SHOW_MS 15000
#define JOIN_TIMEOUT_MS 90000
#define APS_MAX 64
#define KNOWN_MAX 32
#define SLOTS_MAX 6

/* NMDeviceState */
#define DEV_ACTIVATED 100
#define DEV_FAILED 120

struct known {
	struct wifi_ssid ssid;
	char path[WIFI_PATH_MAX];
};

/* One refresh being assembled; published when its last reply is in. */
struct asm_ {
	char dev[WIFI_PATH_MAX];
	struct wifi_ap aps[APS_MAX];
	bool ap_ok[APS_MAX];		/* its properties arrived */
	int n_aps;
	struct known known[KNOWN_MAX];
	bool known_ok[KNOWN_MAX];	/* a Wi-Fi profile with an SSID */
	int n_known;
	struct wifi_active cur;
	struct wifi_ap cur_ap;
	bool cur_ap_ok;
	bool check_enabled, check_available;
	uint32_t connectivity;
};

static struct {
	sd_bus *bus;
	bool watch, scan;
	sd_bus_slot *slots[SLOTS_MAX];
	int n_slots;

	/* refresh */
	unsigned gen;			/* replies for an older gen are dropped */
	bool inflight, dirty;
	int pending;
	struct asm_ *a;
	lv_timer_t *soon;
	int64_t last_start_ms;

	/* the result of the last refresh, for joins and Forget */
	char dev[WIFI_PATH_MAX];
	struct known known[KNOWN_MAX];
	int n_known;

	/* scanning */
	lv_timer_t *scan_timer, *scan_end;
	int64_t last_scan;		/* LastScan seen (-1 unknown) */
	bool scan_inflight;
	bool scan_wanted;		/* asked before wlan0 was known */

	/* join */
	sd_bus_slot *owner_slot;	/* NetworkManager's name owner, always */

	/* join */
	struct {
		unsigned id;		/* replies and signals of older joins dropped */
		bool fresh;		/* a new profile: deleted (by uuid) on failure */
		char uuid[WIFI_UUID_LEN + 1];
		enum wifi_sec sec;
		char active[WIFI_PATH_MAX];	/* the call's ActiveConnection */
		bool saw_fail;		/* the device went FAILED, with this reason */
		uint32_t fail_reason;
		sd_bus_slot *dev_slot, *ac_slot, *call_slot, *get_slot;
		sd_bus_message *msg;	/* holds the passphrase until the reply */
		char *psk;
		size_t psk_len;
		lv_timer_t *timeout;
	} j;
} W = { .last_scan = -1 };

static void refresh_now(void);
static void refresh_soon(void);
static void join_finish(enum wifi_join st);
static void scan_request(void);

static int64_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void timer_del(lv_timer_t **t)
{
	if (*t) {
		lv_timer_delete(*t);
		*t = NULL;
	}
}

static void publish(void)
{
	sys_changed(SYS_WIFI_VIEW);
}

/* ---------------------------------------------------------- message reading */

/* Read a variant of basic type t into p, or skip a variant of another type.
 * 1 read, 0 skipped, <0 error. */
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

static int vread_str(sd_bus_message *m, char t, char *out, size_t n)
{
	const char *s = NULL;
	int r = vread(m, t, &s);

	if (r > 0)
		snprintf(out, n, "%s", s ? s : "");
	return r;
}

/* A variant "ay" holding an SSID. */
static int vread_ssid(sd_bus_message *m, struct wifi_ssid *ssid)
{
	const char *contents = NULL;
	const void *p = NULL;
	size_t len = 0;
	char type;
	int r = sd_bus_message_peek_type(m, &type, &contents);

	if (r <= 0)
		return r < 0 ? r : -ENXIO;
	if (type != 'v' || !contents || strcmp(contents, "ay"))
		return sd_bus_message_skip(m, "v") < 0 ? -EBADMSG : 0;
	if ((r = sd_bus_message_enter_container(m, 'v', "ay")) <= 0 ||
	    (r = sd_bus_message_read_array(m, 'y', &p, &len)) < 0)
		return r < 0 ? r : -ENXIO;
	ssid->len = len > WIFI_SSID_MAX ? WIFI_SSID_MAX : (int)len;
	if (ssid->len)
		memcpy(ssid->b, p, (size_t)ssid->len);
	return sd_bus_message_exit_container(m) < 0 ? -EBADMSG : 1;
}

/* Walk an a{sv}; cb reads or skips each value (it must consume it). */
typedef int (*dict_cb)(const char *key, sd_bus_message *m, void *u);

static int read_dict(sd_bus_message *m, dict_cb cb, void *u)
{
	int r = sd_bus_message_enter_container(m, 'a', "{sv}");

	if (r <= 0)
		return r < 0 ? r : -ENXIO;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *key = NULL;

		if ((r = sd_bus_message_read_basic(m, 's', &key)) <= 0)
			return r < 0 ? r : -ENXIO;
		if ((r = cb(key ? key : "", m, u)) < 0)
			return r;
		if ((r = sd_bus_message_exit_container(m)) < 0)
			return r;
	}
	if (r < 0)
		return r;
	return sd_bus_message_exit_container(m);
}

static int skip_value(sd_bus_message *m)
{
	return sd_bus_message_skip(m, "v") < 0 ? -EBADMSG : 0;
}

/* ------------------------------------------------------------ refresh */

/* Reply user data: the refresh generation and an index (an AP or a
 * profile of this refresh). */
#define UD(i) ((void *)(((uintptr_t)W.gen << 8) | (uintptr_t)(i)))
#define UD_IDX(ud) ((int)((uintptr_t)(ud) & 0xff))

static bool current(void *ud)
{
	return W.inflight && W.a && ((uintptr_t)ud >> 8) == (uintptr_t)W.gen;
}

static void finish_refresh(void)
{
	struct asm_ *a = W.a;
	struct wifi_ssid known[KNOWN_MAX], active = { .len = 0 };
	struct wifi_ap aps[APS_MAX];
	struct wifi_view *v = &wifi_view;
	int i, n_aps = 0;

	W.inflight = false;
	W.a = NULL;
	if (!a)
		return;
	snprintf(W.dev, sizeof(W.dev), "%s", a->dev);
	W.n_known = 0;
	for (i = 0; i < a->n_known; i++)
		if (a->known_ok[i]) {
			W.known[W.n_known] = a->known[i];
			known[W.n_known++] = a->known[i].ssid;
		}
	for (i = 0; i < a->n_aps; i++)
		if (a->ap_ok[i])
			aps[n_aps++] = a->aps[i];

	if (a->cur.valid && a->cur_ap_ok) {
		active = a->cur_ap.ssid;
		a->cur.ap_known = true;
		a->cur.freq = a->cur_ap.freq;
		a->cur.strength = a->cur_ap.strength;
		a->cur.sec = wifi_sec_from_flags(a->cur_ap.flags, a->cur_ap.wpa, a->cur_ap.rsn);
		wifi_ssid_text(&a->cur_ap.ssid, a->cur.text, sizeof(a->cur.text));
	}
	if (a->cur.valid && !a->cur.text[0])
		snprintf(a->cur.text, sizeof(a->cur.text), "%s", a->cur.id);
	/* only a running check is a claim about the internet */
	a->cur.connectivity_known = a->check_enabled && a->check_available && a->connectivity != 0;
	a->cur.connectivity = a->connectivity;
	v->cur = a->cur;
	v->n = wifi_group(aps, n_aps, a->cur.valid && active.len ? &active : NULL, known, W.n_known,
			  v->nets, WIFI_NETS_MAX);
	free(a);
	publish();
	if (W.scan && W.scan_wanted && W.dev[0])
		scan_request();
	if (W.dirty && W.watch)
		refresh_now();
}

static void step_done(void)
{
	if (--W.pending <= 0)
		finish_refresh();
}

/* Issue one call of this refresh; its reply handler calls step_done(). */
static bool step_call(sd_bus_message *m, sd_bus_message_handler_t cb, int idx)
{
	bool ok = m && !netbus_send(m, NULL, cb, UD(idx), 0);

	if (ok)
		W.pending++;
	sd_bus_message_unref(m);
	return ok;
}

static sd_bus_message *get_all(const char *path, const char *iface)
{
	sd_bus_message *m = NULL;

	if (netbus_new_call(&m, NM, path, PROPS, "GetAll") < 0)
		return NULL;
	if (sd_bus_message_append(m, "s", iface) < 0) {
		sd_bus_message_unref(m);
		return NULL;
	}
	return m;
}

static bool real_path(const char *p)
{
	return p && *p && strcmp(p, "/");
}

/* --- access points */

static int ap_prop(const char *key, sd_bus_message *m, void *u)
{
	struct wifi_ap *ap = u;
	uint8_t y = 0;
	int r;

	if (!strcmp(key, "Ssid"))
		return vread_ssid(m, &ap->ssid) < 0 ? -EBADMSG : 0;
	if (!strcmp(key, "Flags"))
		return vread(m, 'u', &ap->flags) < 0 ? -EBADMSG : 0;
	if (!strcmp(key, "WpaFlags"))
		return vread(m, 'u', &ap->wpa) < 0 ? -EBADMSG : 0;
	if (!strcmp(key, "RsnFlags"))
		return vread(m, 'u', &ap->rsn) < 0 ? -EBADMSG : 0;
	if (!strcmp(key, "Frequency"))
		return vread(m, 'u', &ap->freq) < 0 ? -EBADMSG : 0;
	if (!strcmp(key, "Strength")) {
		r = vread(m, 'y', &y);
		ap->strength = y;
		return r < 0 ? -EBADMSG : 0;
	}
	return skip_value(m);
}

static int ap_reply(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	int i = UD_IDX(ud);

	(void)e;
	if (!current(ud))
		return 0;
	if (i < W.a->n_aps && !sd_bus_message_is_method_error(m, NULL) &&
	    read_dict(m, ap_prop, &W.a->aps[i]) >= 0)
		W.a->ap_ok[i] = true;
	step_done();
	return 0;
}

static int aps_reply(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	const char *path;

	(void)e;
	if (!current(ud))
		return 0;
	if (!sd_bus_message_is_method_error(m, NULL) &&
	    sd_bus_message_enter_container(m, 'a', "o") > 0) {
		while (W.a->n_aps < APS_MAX && sd_bus_message_read_basic(m, 'o', &path) > 0) {
			int i = W.a->n_aps;

			memset(&W.a->aps[i], 0, sizeof(W.a->aps[i]));
			snprintf(W.a->aps[i].path, sizeof(W.a->aps[i].path), "%s", path);
			if (step_call(get_all(path, NM_AP), ap_reply, i))
				W.a->n_aps++;
		}
	}
	step_done();
	return 0;
}

/* --- the active AP and the last scan */

static int cur_ap_reply(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	(void)e;
	if (!current(ud))
		return 0;
	if (!sd_bus_message_is_method_error(m, NULL) && read_dict(m, ap_prop, &W.a->cur_ap) >= 0)
		W.a->cur_ap_ok = true;
	step_done();
	return 0;
}

static int wireless_prop(const char *key, sd_bus_message *m, void *u)
{
	char *active_ap = u;

	if (!strcmp(key, "ActiveAccessPoint"))
		return vread_str(m, 'o', active_ap, WIFI_PATH_MAX) < 0 ? -EBADMSG : 0;
	if (!strcmp(key, "LastScan")) {
		int64_t t = -1;
		int r = vread(m, 'x', &t);

		if (r > 0 && t != W.last_scan) {
			if (W.last_scan != -1 && wifi_view.scanning) {
				wifi_view.scanning = false;	/* that scan finished */
				timer_del(&W.scan_end);
			}
			W.last_scan = t;
		}
		return r < 0 ? -EBADMSG : 0;
	}
	return skip_value(m);
}

static int wireless_reply(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	char ap[WIFI_PATH_MAX] = "";

	(void)e;
	if (!current(ud))
		return 0;
	if (!sd_bus_message_is_method_error(m, NULL) && read_dict(m, wireless_prop, ap) >= 0 &&
	    real_path(ap))
		step_call(get_all(ap, NM_AP), cur_ap_reply, 0);
	step_done();
	return 0;
}

/* --- device, IPv4, active connection */

/* AddressData aa{sv}: the first address and its prefix. */
static int read_addresses(sd_bus_message *m, struct wifi_active *c)
{
	const char *contents = NULL;
	char type;
	bool first = true;
	int r;

	if (sd_bus_message_peek_type(m, &type, &contents) <= 0 || type != 'v' || !contents ||
	    strcmp(contents, "aa{sv}"))
		return skip_value(m);
	if (sd_bus_message_enter_container(m, 'v', "aa{sv}") <= 0 ||
	    sd_bus_message_enter_container(m, 'a', "a{sv}") < 0)
		return -EBADMSG;
	while ((r = sd_bus_message_enter_container(m, 'a', "{sv}")) > 0) {
		while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
			const char *k = NULL;
			uint32_t prefix = 0;

			if (sd_bus_message_read_basic(m, 's', &k) <= 0)
				return -EBADMSG;
			if (first && k && !strcmp(k, "address")) {
				r = vread_str(m, 's', c->ip4, sizeof(c->ip4));
			} else if (first && k && !strcmp(k, "prefix")) {
				r = vread(m, 'u', &prefix);
				if (r > 0)
					c->prefix = (int)prefix;
			} else {
				r = skip_value(m);
			}
			if (r < 0 || sd_bus_message_exit_container(m) < 0)
				return -EBADMSG;
		}
		if (r < 0 || sd_bus_message_exit_container(m) < 0)
			return -EBADMSG;
		first = false;
	}
	if (r < 0 || sd_bus_message_exit_container(m) < 0 || sd_bus_message_exit_container(m) < 0)
		return -EBADMSG;
	return 0;
}

static int ip4_prop(const char *key, sd_bus_message *m, void *u)
{
	struct wifi_active *c = u;

	if (!strcmp(key, "Gateway"))
		return vread_str(m, 's', c->gw, sizeof(c->gw)) < 0 ? -EBADMSG : 0;
	if (!strcmp(key, "AddressData"))
		return read_addresses(m, c);
	return skip_value(m);
}

static int ip4_reply(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	(void)e;
	if (!current(ud))
		return 0;
	if (!sd_bus_message_is_method_error(m, NULL))
		(void)read_dict(m, ip4_prop, &W.a->cur);
	step_done();
	return 0;
}

static int active_prop(const char *key, sd_bus_message *m, void *u)
{
	struct wifi_active *c = u;

	if (!strcmp(key, "Id"))
		return vread_str(m, 's', c->id, sizeof(c->id)) < 0 ? -EBADMSG : 0;
	if (!strcmp(key, "Connection"))
		return vread_str(m, 'o', c->conn, sizeof(c->conn)) < 0 ? -EBADMSG : 0;
	if (!strcmp(key, "Uuid"))
		return vread_str(m, 's', c->uuid, sizeof(c->uuid)) < 0 ? -EBADMSG : 0;
	return skip_value(m);
}

static int active_reply(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	(void)e;
	if (!current(ud))
		return 0;
	if (!sd_bus_message_is_method_error(m, NULL))
		(void)read_dict(m, active_prop, &W.a->cur);
	step_done();
	return 0;
}

struct dev_props {
	uint32_t state;
	char active[WIFI_PATH_MAX], ip4[WIFI_PATH_MAX];
};

static int dev_prop(const char *key, sd_bus_message *m, void *u)
{
	struct dev_props *d = u;

	if (!strcmp(key, "State"))
		return vread(m, 'u', &d->state) < 0 ? -EBADMSG : 0;
	if (!strcmp(key, "ActiveConnection"))
		return vread_str(m, 'o', d->active, sizeof(d->active)) < 0 ? -EBADMSG : 0;
	if (!strcmp(key, "Ip4Config"))
		return vread_str(m, 'o', d->ip4, sizeof(d->ip4)) < 0 ? -EBADMSG : 0;
	return skip_value(m);
}

static int dev_props_reply(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	struct dev_props d = { 0 };

	(void)e;
	if (!current(ud))
		return 0;
	if (!sd_bus_message_is_method_error(m, NULL) && read_dict(m, dev_prop, &d) >= 0) {
		W.a->cur.dev_state = d.state;
		if (real_path(d.active)) {
			W.a->cur.valid = true;
			step_call(get_all(d.active, NM_ACTIVE), active_reply, 0);
			if (real_path(d.ip4))
				step_call(get_all(d.ip4, NM_IP4), ip4_reply, 0);
		}
	}
	step_done();
	return 0;
}

static int device_reply(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	const char *path = NULL;
	sd_bus_message *q = NULL;

	(void)e;
	if (!current(ud))
		return 0;
	/* an error is "no such device": wlan0 not up (yet) */
	if (!sd_bus_message_is_method_error(m, NULL) && sd_bus_message_read_basic(m, 'o', &path) > 0 &&
	    real_path(path)) {
		snprintf(W.a->dev, sizeof(W.a->dev), "%s", path);
		if (netbus_new_call(&q, NM, path, NM_WIRELESS, "GetAllAccessPoints") == 0)
			step_call(q, aps_reply, 0);
		step_call(get_all(path, NM_DEVICE), dev_props_reply, 0);
		step_call(get_all(path, NM_WIRELESS), wireless_reply, 0);
	}
	step_done();
	return 0;
}

/* --- connectivity */

static int nm_prop(const char *key, sd_bus_message *m, void *u)
{
	struct asm_ *a = u;
	int b = 0, r;

	if (!strcmp(key, "Connectivity"))
		return vread(m, 'u', &a->connectivity) < 0 ? -EBADMSG : 0;
	if (!strcmp(key, "ConnectivityCheckEnabled") || !strcmp(key, "ConnectivityCheckAvailable")) {
		r = vread(m, 'b', &b);
		if (!strcmp(key, "ConnectivityCheckEnabled"))
			a->check_enabled = r > 0 && b;
		else
			a->check_available = r > 0 && b;
		return r < 0 ? -EBADMSG : 0;
	}
	return skip_value(m);
}

static int nm_reply(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	(void)e;
	if (!current(ud))
		return 0;
	if (!sd_bus_message_is_method_error(m, NULL))
		(void)read_dict(m, nm_prop, W.a);
	step_done();
	return 0;
}

/* --- saved profiles: GetSettings a{sa{sv}}, which carries no secrets */

struct conn_info {
	bool wifi;
	struct wifi_ssid ssid;
};

static int conn_section(const char *key, sd_bus_message *m, void *u)
{
	struct conn_info *ci = u;
	char t[32] = "";
	int r;

	if (strcmp(key, "type"))
		return skip_value(m);
	r = vread_str(m, 's', t, sizeof(t));
	ci->wifi = r > 0 && !strcmp(t, "802-11-wireless");
	return r < 0 ? -EBADMSG : 0;
}

static int wireless_section(const char *key, sd_bus_message *m, void *u)
{
	struct conn_info *ci = u;

	if (!strcmp(key, "ssid"))
		return vread_ssid(m, &ci->ssid) < 0 ? -EBADMSG : 0;
	return skip_value(m);
}

static int read_settings(sd_bus_message *m, struct conn_info *ci)
{
	int r = sd_bus_message_enter_container(m, 'a', "{sa{sv}}");

	if (r <= 0)
		return r < 0 ? r : -ENXIO;
	while ((r = sd_bus_message_enter_container(m, 'e', "sa{sv}")) > 0) {
		const char *section = NULL;

		if (sd_bus_message_read_basic(m, 's', &section) <= 0)
			return -EBADMSG;
		if (section && !strcmp(section, "connection"))
			r = read_dict(m, conn_section, ci);
		else if (section && !strcmp(section, "802-11-wireless"))
			r = read_dict(m, wireless_section, ci);
		else
			r = sd_bus_message_skip(m, "a{sv}");
		if (r < 0 || sd_bus_message_exit_container(m) < 0)
			return -EBADMSG;
	}
	return r < 0 ? r : 0;
}

static int settings_reply(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	struct conn_info ci = { .wifi = false };
	int i = UD_IDX(ud);

	(void)e;
	if (!current(ud))
		return 0;
	if (i < W.a->n_known && !sd_bus_message_is_method_error(m, NULL) &&
	    read_settings(m, &ci) >= 0 && ci.wifi && ci.ssid.len > 0) {
		W.a->known[i].ssid = ci.ssid;
		W.a->known_ok[i] = true;
	}
	step_done();
	return 0;
}

static int list_reply(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	const char *path;

	(void)e;
	if (!current(ud))
		return 0;
	if (!sd_bus_message_is_method_error(m, NULL) &&
	    sd_bus_message_enter_container(m, 'a', "o") > 0) {
		while (W.a->n_known < KNOWN_MAX && sd_bus_message_read_basic(m, 'o', &path) > 0) {
			int i = W.a->n_known;
			sd_bus_message *q = NULL;

			memset(&W.a->known[i], 0, sizeof(W.a->known[i]));
			snprintf(W.a->known[i].path, sizeof(W.a->known[i].path), "%s", path);
			if (netbus_new_call(&q, NM, path, NM_CONN, "GetSettings") == 0 &&
			    step_call(q, settings_reply, i))
				W.a->n_known++;
		}
	}
	step_done();
	return 0;
}

static void refresh_now(void)
{
	sd_bus_message *m = NULL;

	if (!W.bus)
		return;
	if (W.inflight) {
		W.dirty = true;
		return;
	}
	W.a = calloc(1, sizeof(*W.a));
	if (!W.a)
		return;
	W.inflight = true;
	W.dirty = false;
	W.pending = 1;			/* this function's own step */
	W.gen++;
	W.last_start_ms = mono_ms();
	if (netbus_new_call(&m, NM, NM_PATH, NM, "GetDeviceByIpIface") == 0 &&
	    sd_bus_message_append(m, "s", WIFI_IFACE) >= 0)
		step_call(m, device_reply, 0);
	else
		sd_bus_message_unref(m);
	step_call(get_all(NM_PATH, NM), nm_reply, 0);
	m = NULL;
	if (netbus_new_call(&m, NM, NM_SETTINGS_PATH, NM_SETTINGS, "ListConnections") == 0)
		step_call(m, list_reply, 0);
	step_done();
	netbus_kick();
}

static void soon_fired(lv_timer_t *t)
{
	(void)t;
	W.soon = NULL;
	if (W.watch)
		refresh_now();
}

/* Coalesce change signals into one refresh. */
static void refresh_soon(void)
{
	int64_t wait;

	if (!W.watch || W.soon)
		return;
	wait = W.last_start_ms + REFRESH_MIN_MS - mono_ms();
	if (wait < REFRESH_DELAY_MS)
		wait = REFRESH_DELAY_MS;
	W.soon = lv_timer_create(soon_fired, (uint32_t)wait, NULL);
	lv_timer_set_repeat_count(W.soon, 1);
}

static int any_signal(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	(void)m;
	(void)ud;
	(void)e;
	refresh_soon();
	return 0;
}

/* ------------------------------------------------------------ scanning */

static void scan_end_fired(lv_timer_t *t)
{
	(void)t;
	W.scan_end = NULL;
	if (wifi_view.scanning) {
		wifi_view.scanning = false;
		publish();
	}
}

static int scan_reply(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	(void)ud;
	(void)e;
	W.scan_inflight = false;
	/* refused (NM scans already, or activates): no indicator */
	if (sd_bus_message_is_method_error(m, NULL) || !W.scan)
		return 0;
	wifi_view.scanning = true;
	timer_del(&W.scan_end);
	W.scan_end = lv_timer_create(scan_end_fired, SCAN_SHOW_MS, NULL);
	lv_timer_set_repeat_count(W.scan_end, 1);
	publish();
	return 0;
}

static void scan_request(void)
{
	sd_bus_message *m = NULL;

	if (!W.bus || W.scan_inflight)
		return;
	W.scan_wanted = !W.dev[0];
	if (W.scan_wanted)
		return;		/* the refresh finding wlan0 asks again */
	if (netbus_new_call(&m, NM, W.dev, NM_WIRELESS, "RequestScan") == 0 &&
	    sd_bus_message_append(m, "a{sv}", 0) >= 0 && netbus_send(m, NULL, scan_reply, NULL, 0) == 0)
		W.scan_inflight = true;
	sd_bus_message_unref(m);
	netbus_kick();
}

static void scan_fired(lv_timer_t *t)
{
	(void)t;
	scan_request();
}

/* ------------------------------------------------------------ matches */

static void drop_matches(void)
{
	for (int i = 0; i < W.n_slots; i++)
		sd_bus_slot_unref(W.slots[i]);
	W.n_slots = 0;
}

static void add_matches(void)
{
	static const char *const rules[] = {
		"type='signal',sender='" NM "',interface='" PROPS "',member='PropertiesChanged'",
		"type='signal',sender='" NM "',interface='" NM_SETTINGS "'",
		"type='signal',sender='" NM "',interface='" NM_CONN "'",
		"type='signal',sender='" NM "',interface='" NM_WIRELESS "'",
	};

	drop_matches();
	for (unsigned i = 0; i < sizeof(rules) / sizeof(rules[0]) && W.n_slots < SLOTS_MAX; i++)
		if (netbus_match(&W.slots[W.n_slots], rules[i], any_signal, NULL) == 0)
			W.n_slots++;
}

/* ------------------------------------------------- deleting profiles */

/*
 * Profiles are deleted by UUID, never by a remembered object path (NM
 * numbers paths again after a restart): Settings.GetConnectionByUuid on
 * the well-known name, then Delete addressed to the unique name that
 * answered the lookup, so a NetworkManager that restarted in between never
 * receives it (its paths may name other profiles). Only a successful
 * Delete, or the current owner saying no profile has the UUID, confirms;
 * a Delete error looks the UUID up again. Errors retry every
 * CLEANUP_RETRY_MS up to CLEANUP_TRIES times, and once more whenever the
 * bus or NetworkManager comes back; an owner change also drops every
 * attempt in flight (their replies are ignored).
 *
 * The queue has CLEANUP_MAX entries and never evicts one: a new profile's
 * join reserves its entry before the profile is created (armed only if
 * the join fails), and Forget or a join is refused with -ENOSPC when the
 * queue is full.
 */
#define CLEANUP_MAX 4
#define CLEANUP_TRIES 6
#define CLEANUP_RETRY_MS 5000
#define CLEANUP_RELOOKUPS 3
#define NM_ERR_INVALID_CONNECTION NM_SETTINGS ".InvalidConnection"

struct cleanup {
	char uuid[WIFI_UUID_LEN + 1];
	bool armed;			/* false: reserved by a running join */
	int tries, relookups;
	bool inflight;
	unsigned id;			/* replies for an older attempt are dropped */
};

static struct cleanup cleanups[CLEANUP_MAX];
static lv_timer_t *cleanup_timer;
static unsigned cleanup_seq;

static void cleanup_send(struct cleanup *c);
static void cleanup_timer_fired(lv_timer_t *t);

static struct cleanup *cleanup_find(unsigned id)
{
	for (int i = 0; i < CLEANUP_MAX; i++)
		if (cleanups[i].uuid[0] && cleanups[i].inflight && cleanups[i].id == id)
			return &cleanups[i];
	return NULL;
}

static void cleanup_done(struct cleanup *c, const char *how)
{
	chefui_log("wifi: profile %s", how);
	memset(c, 0, sizeof(*c));
	publish();
}

static const char *err_name(sd_bus_message *m)
{
	const sd_bus_error *e = m ? sd_bus_message_get_error(m) : NULL;

	return e && e->name ? e->name : "no bus";
}

static void cleanup_failed(struct cleanup *c, sd_bus_message *m)
{
	c->inflight = false;
	if (++c->tries == CLEANUP_TRIES)
		chefui_log("wifi: deleting a profile failed %d times (%s); retried when "
			   "NetworkManager restarts", c->tries, err_name(m));
	if (c->tries < CLEANUP_TRIES && !cleanup_timer) {
		cleanup_timer = lv_timer_create(cleanup_timer_fired, CLEANUP_RETRY_MS, NULL);
		lv_timer_set_repeat_count(cleanup_timer, 1);
	}
}

static int cleanup_delete_reply(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	struct cleanup *c = cleanup_find((unsigned)(uintptr_t)ud);

	(void)e;
	if (!c)
		return 0;
	if (!sd_bus_message_is_method_error(m, NULL)) {
		cleanup_done(c, "deleted");
		return 0;
	}
	/* the path went away (or never was this profile's): look again */
	c->inflight = false;
	if (++c->relookups <= CLEANUP_RELOOKUPS) {
		cleanup_send(c);
		netbus_kick();
	} else {
		c->relookups = 0;
		cleanup_failed(c, m);
	}
	return 0;
}

static int cleanup_lookup_reply(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	struct cleanup *c = cleanup_find((unsigned)(uintptr_t)ud);
	const sd_bus_error *err = sd_bus_message_get_error(m);
	const char *path = NULL, *owner = sd_bus_message_get_sender(m);
	sd_bus_message *q = NULL;

	(void)e;
	if (!c)
		return 0;
	if (sd_bus_message_is_method_error(m, NULL)) {
		/* NetworkManager itself says no profile has this UUID */
		if (err && err->name && !strcmp(err->name, NM_ERR_INVALID_CONNECTION))
			cleanup_done(c, "already gone");
		else
			cleanup_failed(c, m);
		return 0;
	}
	if (sd_bus_message_read(m, "o", &path) <= 0 || !path || !owner || owner[0] != ':' ||
	    netbus_new_call(&q, owner, path, NM_CONN, "Delete") < 0 ||
	    netbus_send(q, NULL, cleanup_delete_reply, ud, 0) < 0)
		cleanup_failed(c, NULL);
	sd_bus_message_unref(q);
	return 0;
}

static void cleanup_send(struct cleanup *c)
{
	sd_bus_message *m = NULL;

	if (!W.bus || c->inflight || !c->armed)
		return;
	c->id = ++cleanup_seq;
	if (netbus_new_call(&m, NM, NM_SETTINGS_PATH, NM_SETTINGS, "GetConnectionByUuid") >= 0 &&
	    sd_bus_message_append(m, "s", c->uuid) >= 0 &&
	    netbus_send(m, NULL, cleanup_lookup_reply, (void *)(uintptr_t)c->id, 0) >= 0)
		c->inflight = true;
	else
		cleanup_failed(c, NULL);
	sd_bus_message_unref(m);
}

/* fresh_round: the bus or NetworkManager came back, entries that used up
 * their tries get one more */
static void cleanup_run(bool fresh_round)
{
	for (int i = 0; i < CLEANUP_MAX; i++) {
		struct cleanup *c = &cleanups[i];

		if (!c->uuid[0] || !c->armed || c->inflight)
			continue;
		if (fresh_round && c->tries >= CLEANUP_TRIES)
			c->tries = CLEANUP_TRIES - 1;
		if (c->tries < CLEANUP_TRIES)
			cleanup_send(c);
	}
	netbus_kick();
}

/* The owner changed or the bus went: replies to attempts in flight are
 * from (or for) another NetworkManager; drop them. */
static void cleanup_invalidate(void)
{
	for (int i = 0; i < CLEANUP_MAX; i++)
		if (cleanups[i].inflight) {
			cleanups[i].inflight = false;
			cleanups[i].id = ++cleanup_seq;
		}
}

static void cleanup_timer_fired(lv_timer_t *t)
{
	(void)t;
	cleanup_timer = NULL;
	cleanup_run(false);
}

/* A free entry for uuid (armed now, or reserved for a join), or NULL. */
static struct cleanup *cleanup_reserve(const char *uuid, bool armed)
{
	struct cleanup *c = NULL;

	for (int i = 0; i < CLEANUP_MAX; i++)
		if (!strcmp(cleanups[i].uuid, uuid)) {
			c = &cleanups[i];	/* already queued: (re)arm it */
			c->armed = c->armed || armed;
			return c;
		}
	for (int i = 0; i < CLEANUP_MAX && !c; i++)
		if (!cleanups[i].uuid[0])
			c = &cleanups[i];
	if (!c)
		return NULL;
	memset(c, 0, sizeof(*c));
	snprintf(c->uuid, sizeof(c->uuid), "%s", uuid);
	c->armed = armed;
	return c;
}

/* Is there room for one more entry? */
static bool cleanup_reserve_check(void)
{
	for (int i = 0; i < CLEANUP_MAX; i++)
		if (!cleanups[i].uuid[0])
			return true;
	return false;
}

static struct cleanup *cleanup_entry(const char *uuid)
{
	for (int i = 0; i < CLEANUP_MAX; i++)
		if (cleanups[i].uuid[0] && !strcmp(cleanups[i].uuid, uuid))
			return &cleanups[i];
	return NULL;
}

bool sys_wifi_cleanup_pending(void)
{
	for (int i = 0; i < CLEANUP_MAX; i++)
		if (cleanups[i].uuid[0] && cleanups[i].armed)
			return true;
	return false;
}

/* ------------------------------------------------------------ joins */

/*
 * A join is identified by the ActiveConnection its call returns. Before
 * that reply nothing ends it: the device's StateChanged only records a
 * FAILED reason. After it, the active connection's own StateChanged
 * (matched first) and one Get of its State (sent after the match, so
 * nothing falls between them) decide: ACTIVATED is done, DEACTIVATED or
 * an active connection that no longer exists is a failure, classified by
 * the recorded device reason or the active connection's reason. A FAILED
 * device after the reply ends it too.
 */
#define AC_ACTIVATED 2
#define AC_DEACTIVATING 3
#define AC_DEACTIVATED 4
#define AC_REASON_NO_SECRETS 9

/* The passphrase's copy in the call: wiped once it is no longer needed. */
static void join_drop_msg(void)
{
	if (W.j.psk)
		wifi_wipe(W.j.psk, W.j.psk_len);
	W.j.psk = NULL;
	W.j.psk_len = 0;
	W.j.msg = sd_bus_message_unref(W.j.msg);
}

static void join_finish(enum wifi_join st)
{
	if (wifi_view.join.st != WIFI_JOIN_BUSY)
		return;
	join_drop_msg();
	W.j.call_slot = sd_bus_slot_unref(W.j.call_slot);
	W.j.dev_slot = sd_bus_slot_unref(W.j.dev_slot);
	W.j.ac_slot = sd_bus_slot_unref(W.j.ac_slot);
	W.j.get_slot = sd_bus_slot_unref(W.j.get_slot);
	timer_del(&W.j.timeout);
	/* a profile that never worked is not kept: its reserved entry arms;
	 * a working one releases the entry */
	if (W.j.fresh) {
		struct cleanup *c = cleanup_entry(W.j.uuid);

		if (c && st == WIFI_JOIN_DONE && !c->armed)
			memset(c, 0, sizeof(*c));
		else if (c && st != WIFI_JOIN_DONE) {
			c->armed = true;
			cleanup_send(c);
			netbus_kick();
		}
	}
	chefui_log("wifi: join %s", st == WIFI_JOIN_DONE ? "connected" : st == WIFI_JOIN_BAD_PASSWORD
		   ? "refused the passphrase" : "failed");
	wifi_view.join.st = st;
	publish();
	if (W.watch)
		refresh_soon();
}

static enum wifi_join join_failure(uint32_t ac_reason)
{
	bool psk = wifi_sec_needs_psk(W.j.sec);

	if (psk && ((W.j.saw_fail && wifi_reason_bad_password(W.j.fail_reason)) ||
		    ac_reason == AC_REASON_NO_SECRETS))
		return WIFI_JOIN_BAD_PASSWORD;
	return WIFI_JOIN_FAILED;
}

static bool join_current(void *ud)
{
	return wifi_view.join.st == WIFI_JOIN_BUSY && (unsigned)(uintptr_t)ud == W.j.id;
}

static void join_ac_state(uint32_t st, uint32_t reason)
{
	if (st == AC_ACTIVATED)
		join_finish(WIFI_JOIN_DONE);
	else if (st == AC_DEACTIVATING || st == AC_DEACTIVATED)
		join_finish(join_failure(reason));
}

static int join_dev_signal(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	uint32_t st = 0, old = 0, reason = 0;

	(void)e;
	if (!join_current(ud) || sd_bus_message_read(m, "uuu", &st, &old, &reason) < 0)
		return 0;
	if (st == DEV_FAILED) {
		W.j.saw_fail = true;
		W.j.fail_reason = reason;
		if (W.j.active[0])
			join_finish(join_failure(0));	/* after the reply: ours */
	}
	return 0;
}

static int join_ac_signal(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	uint32_t st = 0, reason = 0;

	(void)e;
	if (join_current(ud) && sd_bus_message_read(m, "uu", &st, &reason) > 0)
		join_ac_state(st, reason);
	return 0;
}

static int join_get_reply(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	uint32_t st = 0;

	(void)e;
	if (!join_current(ud))
		return 0;
	W.j.get_slot = sd_bus_slot_unref(W.j.get_slot);
	if (sd_bus_message_is_method_error(m, NULL))
		join_finish(join_failure(0));	/* it ended before we could look */
	else if (vread(m, 'u', &st) > 0)
		join_ac_state(st, 0);
	return 0;
}

/* The call's reply named the ActiveConnection: follow that object. */
static void join_follow(const char *active)
{
	char rule[300];
	sd_bus_message *m = NULL;

	snprintf(W.j.active, sizeof(W.j.active), "%s", active);
	snprintf(rule, sizeof(rule), "type='signal',sender='" NM "',path='%s',interface='" NM_ACTIVE
		 "',member='StateChanged'", active);
	if (netbus_match(&W.j.ac_slot, rule, join_ac_signal, (void *)(uintptr_t)W.j.id) < 0 ||
	    netbus_new_call(&m, NM, active, PROPS, "Get") < 0 ||
	    sd_bus_message_append(m, "ss", NM_ACTIVE, "State") < 0 ||
	    netbus_send(m, &W.j.get_slot, join_get_reply, (void *)(uintptr_t)W.j.id, 0) < 0)
		join_finish(WIFI_JOIN_FAILED);
	sd_bus_message_unref(m);
}

static int join_reply(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	const char *conn = NULL, *active = NULL;
	int r;

	(void)e;
	if (!join_current(ud))
		return 0;
	W.j.call_slot = sd_bus_slot_unref(W.j.call_slot);
	join_drop_msg();		/* NetworkManager has the call: wipe our copy */
	if (sd_bus_message_is_method_error(m, NULL)) {
		chefui_log("wifi: join refused: %s", sd_bus_message_get_error(m)->name);
		join_finish(WIFI_JOIN_FAILED);
		return 0;
	}
	/* AddAndActivateConnection (oo) or ActivateConnection (o) */
	r = W.j.fresh ? sd_bus_message_read(m, "oo", &conn, &active) : sd_bus_message_read(m, "o", &active);
	if (r <= 0 || !real_path(active)) {
		join_finish(WIFI_JOIN_FAILED);
		return 0;
	}
	join_follow(active);
	return 0;
}

static void join_timeout(lv_timer_t *t)
{
	(void)t;
	W.j.timeout = NULL;
	join_finish(WIFI_JOIN_FAILED);
	netbus_kick();
}

/* Start a join: refused (nothing touched) while another one runs. The
 * device match goes out before the call. */
static int join_begin(const char *text, enum wifi_sec sec, bool fresh)
{
	char rule[300];
	unsigned id;

	if (!W.bus)
		return -ENOTCONN;
	if (!W.dev[0])
		return -ENODEV;
	if (wifi_view.join.st == WIFI_JOIN_BUSY)
		return -EBUSY;
	id = W.j.id + 1;
	memset(&W.j, 0, sizeof(W.j));
	W.j.id = id;
	W.j.sec = sec;
	W.j.fresh = fresh;
	snprintf(rule, sizeof(rule), "type='signal',sender='" NM "',path='%s',interface='" NM_DEVICE
		 "',member='StateChanged'", W.dev);
	if (netbus_match(&W.j.dev_slot, rule, join_dev_signal, (void *)(uintptr_t)W.j.id) < 0)
		return -EIO;
	wifi_view.join.st = WIFI_JOIN_BUSY;
	snprintf(wifi_view.join.text, sizeof(wifi_view.join.text), "%s", text);
	W.j.timeout = lv_timer_create(join_timeout, JOIN_TIMEOUT_MS, NULL);
	lv_timer_set_repeat_count(W.j.timeout, 1);
	return 0;
}

/* Undo a join_begin() of this very call (its message never went out). */
static void join_abort(void)
{
	join_finish(WIFI_JOIN_FAILED);
	wifi_view.join.st = WIFI_JOIN_IDLE;
	publish();
}

static int join_send(sd_bus_message *m)
{
	int r = netbus_send(m, &W.j.call_slot, join_reply, (void *)(uintptr_t)W.j.id, 0);

	if (r < 0) {
		join_drop_msg();
		join_finish(WIFI_JOIN_FAILED);
	}
	publish();
	netbus_kick();
	return r;
}

static const struct known *known_for(const struct wifi_ssid *ssid)
{
	for (int i = 0; i < W.n_known; i++)
		if (wifi_ssid_eq(&W.known[i].ssid, ssid))
			return &W.known[i];
	return NULL;
}

/* A random (version 4) UUID for a new profile: its identity for cleanup.
 * Never blocks: getrandom without waiting for the pool, else
 * /dev/urandom (uniqueness is what matters here, not secrecy). */
static int new_uuid(char out[WIFI_UUID_LEN + 1])
{
	uint8_t b[16];
	bool ok = getrandom(b, sizeof(b), GRND_NONBLOCK) == (ssize_t)sizeof(b);

	if (!ok) {
		int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC | O_NONBLOCK);

		ok = fd >= 0 && read(fd, b, sizeof(b)) == (ssize_t)sizeof(b);
		if (fd >= 0)
			close(fd);
	}
	if (!ok)
		return -EAGAIN;
	b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);
	b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
	snprintf(out, WIFI_UUID_LEN + 1,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12],
		 b[13], b[14], b[15]);
	return 0;
}

static int build_profile(sd_bus_message *m, const struct wifi_ssid *ssid, const char *id,
			 const char *uuid, enum wifi_sec sec, const char *psk, bool hidden, char **psk_at)
{
	const char *km = wifi_sec_key_mgmt(sec);
	int r;

	if ((r = sd_bus_message_open_container(m, 'a', "{sa{sv}}")) < 0)
		return r;
	if ((r = sd_bus_message_open_container(m, 'e', "sa{sv}")) < 0 ||
	    (r = sd_bus_message_append(m, "s", "connection")) < 0 ||
	    (r = sd_bus_message_open_container(m, 'a', "{sv}")) < 0 ||
	    (r = sd_bus_message_append(m, "{sv}", "id", "s", id)) < 0 ||
	    (r = sd_bus_message_append(m, "{sv}", "uuid", "s", uuid)) < 0 ||
	    (r = sd_bus_message_append(m, "{sv}", "type", "s", "802-11-wireless")) < 0 ||
	    (r = sd_bus_message_close_container(m)) < 0 || (r = sd_bus_message_close_container(m)) < 0)
		return r;
	if ((r = sd_bus_message_open_container(m, 'e', "sa{sv}")) < 0 ||
	    (r = sd_bus_message_append(m, "s", "802-11-wireless")) < 0 ||
	    (r = sd_bus_message_open_container(m, 'a', "{sv}")) < 0 ||
	    (r = sd_bus_message_open_container(m, 'e', "sv")) < 0 ||
	    (r = sd_bus_message_append(m, "s", "ssid")) < 0 ||
	    (r = sd_bus_message_open_container(m, 'v', "ay")) < 0 ||
	    (r = sd_bus_message_append_array(m, 'y', ssid->b, (size_t)ssid->len)) < 0 ||
	    (r = sd_bus_message_close_container(m)) < 0 || (r = sd_bus_message_close_container(m)) < 0 ||
	    (r = sd_bus_message_append(m, "{sv}", "mode", "s", "infrastructure")) < 0 ||
	    (hidden && (r = sd_bus_message_append(m, "{sv}", "hidden", "b", 1)) < 0) ||
	    (r = sd_bus_message_close_container(m)) < 0 || (r = sd_bus_message_close_container(m)) < 0)
		return r;
	if (km) {
		size_t len = strlen(psk);
		char *p = NULL;

		if ((r = sd_bus_message_open_container(m, 'e', "sa{sv}")) < 0 ||
		    (r = sd_bus_message_append(m, "s", "802-11-wireless-security")) < 0 ||
		    (r = sd_bus_message_open_container(m, 'a', "{sv}")) < 0 ||
		    (r = sd_bus_message_append(m, "{sv}", "key-mgmt", "s", km)) < 0 ||
		    (r = sd_bus_message_open_container(m, 'e', "sv")) < 0 ||
		    (r = sd_bus_message_append(m, "s", "psk")) < 0 ||
		    (r = sd_bus_message_open_container(m, 'v', "s")) < 0 ||
		    (r = sd_bus_message_append_string_space(m, len, &p)) < 0)
			return r;
		memcpy(p, psk, len);
		*psk_at = p;
		if ((r = sd_bus_message_close_container(m)) < 0 || (r = sd_bus_message_close_container(m)) < 0 ||
		    (r = sd_bus_message_close_container(m)) < 0 || (r = sd_bus_message_close_container(m)) < 0)
			return r;
	}
	return sd_bus_message_close_container(m);
}

static bool psk_ok(const char *psk, enum wifi_sec sec)
{
	size_t len = strlen(psk);

	if (!wifi_sec_needs_psk(sec))
		return len == 0;
	if (len < WIFI_PSK_MIN || len > WIFI_PSK_MAX)
		return false;
	for (size_t i = 0; i < len; i++)
		if (psk[i] < 0x20 || psk[i] > 0x7E)
			return false;
	return true;
}


int sys_wifi_join_new(const struct wifi_ssid *ssid, enum wifi_sec sec, char *psk, bool hidden,
		      const char *ap)
{
	sd_bus_message *m = NULL;
	char text[WIFI_SSID_TEXT], *at = NULL, uuid[WIFI_UUID_LEN + 1];
	size_t psk_size = strlen(psk) + 1;
	int r;

	if (ssid->len <= 0 || ssid->len > WIFI_SSID_MAX || !wifi_sec_joinable(sec) || !psk_ok(psk, sec)) {
		wifi_wipe(psk, psk_size);
		return -EINVAL;
	}
	wifi_ssid_text(ssid, text, sizeof(text));
	if (wifi_view.join.st == WIFI_JOIN_BUSY || new_uuid(uuid) < 0 || !W.bus ||
	    !cleanup_reserve_check()) {
		wifi_wipe(psk, psk_size);
		return wifi_view.join.st == WIFI_JOIN_BUSY ? -EBUSY : !W.bus ? -ENOTCONN
			: !cleanup_reserve_check() ? -ENOSPC : -EAGAIN;
	}
	r = join_begin(text, sec, true);
	if (r < 0) {
		wifi_wipe(psk, psk_size);
		return r;		/* another join runs on, untouched */
	}
	/* the delete entry exists before the profile can */
	snprintf(W.j.uuid, sizeof(W.j.uuid), "%s", uuid);
	if (!cleanup_reserve(uuid, false))
		r = -ENOSPC;
	if (r >= 0)
		r = netbus_new_call(&m, NM, NM_PATH, NM, "AddAndActivateConnection");
	if (r >= 0)
		r = build_profile(m, ssid, text, W.j.uuid, sec, psk, hidden, &at);
	/* the caller's copy goes now, whatever happens next */
	wifi_wipe(psk, psk_size);
	if (r >= 0)
		r = sd_bus_message_append(m, "oo", W.dev, !hidden && ap && real_path(ap) ? ap : "/");
#if !defined(CHEF_BASU)
	if (r >= 0)
		(void)sd_bus_message_sensitive(m);	/* libsystemd also wipes it on free */
#endif
	if (r < 0) {
		if (at)
			wifi_wipe(at, psk_size - 1);
		sd_bus_message_unref(m);
		/* nothing was sent: no profile to delete, free the entry */
		if (cleanup_entry(uuid) && !cleanup_entry(uuid)->armed)
			memset(cleanup_entry(uuid), 0, sizeof(struct cleanup));
		W.j.fresh = false;
		join_abort();
		return r;
	}
	W.j.msg = m;			/* kept (and wiped) until the reply */
	W.j.psk = at;
	W.j.psk_len = at ? psk_size - 1 : 0;
	chefui_log("wifi: joining a new %s network", wifi_sec_short(sec));
	return join_send(m);
}

int sys_wifi_join(const struct wifi_net *n)
{
	const struct known *k = known_for(&n->ssid);
	sd_bus_message *m = NULL;
	int r;

	if (!k) {
		char none[1] = "";

		if (n->sec != WIFI_SEC_OPEN)
			return -ENOKEY;		/* needs the passphrase page */
		return sys_wifi_join_new(&n->ssid, WIFI_SEC_OPEN, none, false, n->ap);
	}
	r = join_begin(n->text, n->sec, false);
	if (r < 0)
		return r;		/* another join runs on, untouched */
	r = netbus_new_call(&m, NM, NM_PATH, NM, "ActivateConnection");
	if (r >= 0)
		r = sd_bus_message_append(m, "ooo", k->path, W.dev, real_path(n->ap) ? n->ap : "/");
	if (r < 0) {
		sd_bus_message_unref(m);
		join_abort();
		return r;
	}
	chefui_log("wifi: activating a saved profile");
	r = join_send(m);
	sd_bus_message_unref(m);
	return r;
}

void sys_wifi_join_ack(void)
{
	if (wifi_view.join.st != WIFI_JOIN_BUSY && wifi_view.join.st != WIFI_JOIN_IDLE) {
		wifi_view.join.st = WIFI_JOIN_IDLE;
		publish();
	}
}

int sys_wifi_forget(const char *uuid)
{
	struct cleanup *c;

	if (!W.bus)
		return -ENOTCONN;
	if (!uuid || strlen(uuid) != WIFI_UUID_LEN)
		return -EINVAL;
	c = cleanup_reserve(uuid, true);
	if (!c)
		return -ENOSPC;		/* never evicts a pending delete */
	chefui_log("wifi: forgetting a profile");
	cleanup_send(c);
	netbus_kick();
	return 0;
}

static int set_reply(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	(void)ud;
	(void)e;
	if (sd_bus_message_is_method_error(m, NULL))
		chefui_log("wifi: WirelessEnabled refused: %s", sd_bus_message_get_error(m)->name);
	return 0;
}

int sys_wifi_set_enabled(bool on)
{
	sd_bus_message *m = NULL;
	int r = netbus_new_call(&m, NM, NM_PATH, PROPS, "Set");

	if (r >= 0)
		r = sd_bus_message_append(m, "ssv", NM, "WirelessEnabled", "b", (int)on);
	if (r >= 0)
		r = netbus_send(m, NULL, set_reply, NULL, 0);
	sd_bus_message_unref(m);
	netbus_kick();
	return r;
}

/* ------------------------------------------------------------ watch */

void sys_wifi_watch(bool watch, bool scan)
{
	scan = scan && watch;
	if (watch && !W.watch) {
		W.watch = true;
		if (W.bus) {
			add_matches();
			refresh_now();
		}
	} else if (!watch && W.watch) {
		W.watch = false;
		drop_matches();
		timer_del(&W.soon);
	}
	if (scan && !W.scan) {
		W.scan = true;
		W.scan_timer = lv_timer_create(scan_fired, WIFI_SCAN_PERIOD_MS, NULL);
		scan_request();
	} else if (!scan && W.scan) {
		W.scan = false;
		timer_del(&W.scan_timer);
		timer_del(&W.scan_end);
		if (wifi_view.scanning) {
			wifi_view.scanning = false;
			publish();
		}
	}
	netbus_kick();
}

/* NetworkManager (re)started or went away: a join cannot outlive it, a
 * pending delete gets another round, the view is read again. */
static int owner_signal(sd_bus_message *m, void *ud, sd_bus_error *e)
{
	const char *name = NULL, *old = NULL, *new = NULL;

	(void)ud;
	(void)e;
	if (sd_bus_message_read(m, "sss", &name, &old, &new) < 0)
		return 0;
	cleanup_invalidate();
	if (new && *new) {
		cleanup_run(true);
	} else if (wifi_view.join.st == WIFI_JOIN_BUSY) {
		join_finish(WIFI_JOIN_FAILED);
	}
	refresh_soon();
	return 0;
}

/* netbus client: the bus (re)opened, or NULL when it went away. */
static void bus_changed(sd_bus *bus)
{
	if (!bus) {
		W.bus = NULL;		/* nothing below may queue on it */
		drop_matches();
		W.owner_slot = sd_bus_slot_unref(W.owner_slot);
		if (wifi_view.join.st == WIFI_JOIN_BUSY)
			join_finish(WIFI_JOIN_FAILED);	/* a new profile stays queued */
		cleanup_invalidate();
		timer_del(&cleanup_timer);
		free(W.a);
		W.a = NULL;
		W.inflight = false;
		W.scan_inflight = false;
		timer_del(&W.soon);
		timer_del(&W.scan_end);
		W.dev[0] = 0;
		W.last_scan = -1;
		memset(&wifi_view.cur, 0, sizeof(wifi_view.cur));
		wifi_view.n = 0;
		wifi_view.scanning = false;
		publish();
		return;
	}
	W.bus = bus;
	(void)netbus_match(&W.owner_slot,
			   "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
			   "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='" NM "'",
			   owner_signal, NULL);
	cleanup_run(true);
	if (W.watch) {
		add_matches();
		refresh_now();
	}
}

void sys_wifi_init(void)
{
	static bool done;

	if (!done) {
		done = true;
		netbus_add_client(bus_changed);
	}
}
